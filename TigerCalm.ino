/*
 * ================================================================
 * TigerCalm — ESP32-WROOM-DA
 * University of the Pacific Hackathon
 * ================================================================
 *
 * Sensors:
 *   INMP441 I2S microphone  → real sound level measurement
 *   DHT11                   → temperature + humidity
 *   PIR                     → activity (rolling trigger count)
 *
 * WIRING:
 * ----------------------------------------------------------------
 *   INMP441:
 *     VDD → 3.3V
 *     GND → GND
 *     SCK → GPIO 14   (I2S bit clock)
 *     WS  → GPIO 15   (I2S word select)
 *     SD  → GPIO 32   (I2S data in)
 *     L/R → GND       (left channel — REQUIRED)
 *
 *   DHT11 module (blue, 3 pins):
 *     +   → 3.3V
 *     out → GPIO 4
 *     −   → GND
 *
 *   PIR sensor:
 *     VCC → VIN (5V pin on ESP32 devboard)
 *     OUT → GPIO 5
 *     GND → GND
 * ----------------------------------------------------------------
 *
 * LIBRARIES — install via Arduino Library Manager:
 *   DHT sensor library   (by Adafruit)
 *   ArduinoJson          (by Benoit Blanchon, version 6.x)
 *   driver/i2s.h is built into ESP32 Arduino core — no install
 *
 * CALIBRATION:
 *   Open Serial Monitor at 115200 baud after uploading.
 *   Look for lines like: "Sound dBFS: -58.3  Score: 12"
 *   In a quiet room your dBFS should be around -70 to -55.
 *   In a loud room it should be around -30 to -15.
 *   If your scores seem wrong, adjust QUIET_DBFS and LOUD_DBFS below.
 * ================================================================
 */

#include <Arduino.h>
#include <driver/i2s.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ================================================================
// CONFIG — fill these in
// ================================================================

const char* WIFI_SSID      = "iPhone (301)";
const char* WIFI_PASSWORD  = "11223344";
const char* FIREBASE_URL   = "https://tigercalm-1fa50-default-rtdb.firebaseio.com";
const char* GEMINI_KEY     = "AIzaSyAv4nO-1GMPVaMNd1upY-lwn-USKynnO1Y";
const char* LOCATION       = "Dining Hall";

// Sound calibration — adjust if scores seem off
// Open Serial Monitor, note the dBFS values, set these accordingly
const float QUIET_DBFS  = -70.0f;   // dBFS reading in a very quiet room
const float LOUD_DBFS   = -15.0f;   // dBFS reading in a loud room

// ================================================================
// PINS
// ================================================================

// INMP441 I2S pins
#define I2S_BCK_PIN   4   // Bit clock
#define I2S_WS_PIN    5   // Word select (LRCLK)
#define I2S_DATA_PIN  3   // Serial data in

// DHT11
#define DHT_PIN  6
DHT dht(DHT_PIN, DHT11);

// PIR
#define PIR_PIN  7

// ================================================================
// TIMING
// ================================================================

#define SCAN_INTERVAL_MS    30000   // Full scan every 30 seconds
#define PIR_COOLDOWN_MS      3000   // Min gap between PIR recordings
#define ACTIVITY_WINDOW_MS 120000   // 2-minute rolling window

// ================================================================
// PIR ACTIVITY TRACKER
// ================================================================

#define MAX_PIR_HISTORY  60

unsigned long pirTimes[MAX_PIR_HISTORY];
int           pirCount     = 0;
unsigned long lastPirTime  = 0;
bool          pirLastState = false;

void recordPir() {
  unsigned long now = millis();
  if (now - lastPirTime < PIR_COOLDOWN_MS) return;
  lastPirTime = now;
  pirTimes[pirCount % MAX_PIR_HISTORY] = now;
  pirCount++;
  Serial.println("PIR: trigger");
}

int getActivity() {
  unsigned long cutoff = millis() - ACTIVITY_WINDOW_MS;
  int n = 0;
  int stored = min(pirCount, MAX_PIR_HISTORY);
  for (int i = 0; i < stored; i++) {
    if (pirTimes[i] > cutoff) n++;
  }
  return n;
}

// ================================================================
// I2S SETUP (INMP441)
// ================================================================

void setupI2S() {
  i2s_config_t config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = 44100,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,  // L/R=GND → left
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 4,
    .dma_buf_len          = 256,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_BCK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_DATA_PIN
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  if (err != ESP_OK) Serial.println("I2S install error: " + String(err));

  err = i2s_set_pin(I2S_NUM_0, &pins);
  if (err != ESP_OK) Serial.println("I2S pin error: " + String(err));

  Serial.println("I2S (INMP441) ready");
}

// ================================================================
// SOUND LEVEL READING
// ================================================================

int readSoundLevel() {
  /*
   * The INMP441 outputs 24-bit PCM samples in the upper 24 bits
   * of a 32-bit I2S frame. We shift right by 8 to recover them.
   *
   * Steps:
   *   1. Read 512 samples from I2S DMA buffer
   *   2. Compute RMS amplitude
   *   3. Convert to dBFS (decibels relative to full scale)
   *   4. Map dBFS to a 0–100 score using calibration constants
   */
  const int SAMPLES = 512;
  int32_t buf[SAMPLES];
  size_t bytesRead = 0;

  i2s_read(I2S_NUM_0, (void*)buf, sizeof(buf), &bytesRead,
           pdMS_TO_TICKS(200));

  int n = bytesRead / sizeof(int32_t);
  if (n == 0) return 0;

  // Calculate RMS
  int64_t sumSq = 0;
  for (int i = 0; i < n; i++) {
    int32_t sample = buf[i] >> 8;  // recover 24-bit signed value
    sumSq += (int64_t)sample * sample;
  }
  double rms = sqrt((double)sumSq / n);

  // Convert to dBFS
  // Max possible 24-bit amplitude = 8,388,607
  float dBFS = 20.0f * log10f((float)(rms / 8388607.0) + 1e-10f);

  // Map to 0-100 score using calibration constants
  float score = (dBFS - QUIET_DBFS) / (LOUD_DBFS - QUIET_DBFS) * 100.0f;
  int result  = (int)constrain(score, 0.0f, 100.0f);

  Serial.printf("Sound  RMS: %.0f  dBFS: %.1f  Score: %d\n",
                rms, dBFS, result);
  return result;
}

// ================================================================
// DHT11
// ================================================================

float readTemp() {
  float t = dht.readTemperature(true); // true = Fahrenheit
  if (isnan(t)) {
    Serial.println("DHT: temp read failed — using default");
    return 72.0f;
  }
  return t;
}

float readHumidity() {
  float h = dht.readHumidity();
  if (isnan(h)) {
    Serial.println("DHT: humidity read failed — using default");
    return 50.0f;
  }
  return h;
}

// ================================================================
// COMFORT SCORE (0–100)
// ================================================================

int calcComfortScore(float temp, float humidity, int sound, int activity) {
  float score = 100.0f;

  // Temperature penalty (ideal: 68–74°F)
  if      (temp > 82.0f) score -= (temp - 82.0f) * 4.0f;
  else if (temp > 76.0f) score -= (temp - 76.0f) * 2.5f;
  else if (temp < 64.0f) score -= (64.0f - temp) * 2.5f;

  // Humidity penalty (ideal: 40–55%)
  if      (humidity > 65.0f) score -= (humidity - 65.0f) * 1.5f;
  else if (humidity < 28.0f) score -= (28.0f - humidity) * 1.5f;

  // Sound penalty — biggest sensory trigger
  if      (sound > 75) score -= 38.0f;
  else if (sound > 55) score -= 24.0f;
  else if (sound > 35) score -= 12.0f;
  else if (sound > 20) score -= 5.0f;

  // Activity penalty (PIR triggers in last 2 min)
  if      (activity >= 12) score -= 20.0f;
  else if (activity >= 7)  score -= 12.0f;
  else if (activity >= 3)  score -= 5.0f;

  return (int)constrain(score, 0.0f, 100.0f);
}

String comfortLevel(int score) {
  if (score >= 68) return "COMFORTABLE";
  if (score >= 38) return "MODERATE";
  return "OVERWHELMING";
}

String comfortColor(int score) {
  if (score >= 68) return "green";
  if (score >= 38) return "orange";
  return "red";
}

// ================================================================
// GEMINI API
// ================================================================

String callGemini(float temp, float humidity, int sound,
                  int activity, int score, String level) {

  if (WiFi.status() != WL_CONNECTED) {
    return String(LOCATION) + " is currently " + level + ".";
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/"
               "gemini-1.5-flash:generateContent?key=";
  url += GEMINI_KEY;

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(12000);

  String soundLabel = sound < 30 ? "quiet" : sound < 60 ? "moderate" : "loud";
  String actLabel   = activity < 4 ? "low" : activity < 8 ? "moderate" : "high";

  String prompt =
    "You are a warm, friendly campus assistant at University of the Pacific. "
    "A student is checking if the " + String(LOCATION) + " is comfortable right now. "
    "Current conditions: Temperature " + String(temp, 0) + "°F, "
    "Humidity " + String(humidity, 0) + "%, "
    "Sound level " + soundLabel + " (" + String(sound) + "/100), "
    "Activity level " + actLabel + " (" + String(activity) + " movements in last 2 minutes), "
    "Overall comfort score " + String(score) + "/100, Status: " + level + ". "
    "Write 2 friendly sentences (max 35 words total). "
    "Don't mention specific numbers. "
    "If OVERWHELMING, suggest a specific better time like 'after 2pm'. "
    "Be warm, clear, and encouraging.";

  StaticJsonDocument<768> req;
  JsonArray  contents = req.createNestedArray("contents");
  JsonObject content  = contents.createNestedObject();
  JsonArray  parts    = content.createNestedArray("parts");
  parts.createNestedObject()["text"] = prompt;

  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  if (code != 200) {
    Serial.println("Gemini error: " + String(code));
    http.end();
    return "The " + String(LOCATION) + " is currently " + level + ". Check the readings below.";
  }

  String resp = http.getString();
  http.end();

  StaticJsonDocument<4096> res;
  if (deserializeJson(res, resp)) return "Check the readings below for current conditions.";

  String msg = res["candidates"][0]["content"]["parts"][0]["text"].as<String>();
  msg.trim();
  return msg;
}

// ================================================================
// FIREBASE
// ================================================================

void postToFirebase(float temp, float humidity, int sound,
                    int activity, int score, String level,
                    String color, String message) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase: skipped — no WiFi");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, String(FIREBASE_URL) + "/tigerCalm.json");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);

  // Build JSON payload
  StaticJsonDocument<1024> doc;
  doc["location"] = LOCATION;

  JsonObject sensors = doc.createNestedObject("sensors");
  sensors["temperatureF"] = round(temp * 10.0f) / 10.0f;
  sensors["humidity"]     = round(humidity * 10.0f) / 10.0f;

  JsonObject env = doc.createNestedObject("environment");
  env["soundScore"]    = sound;
  env["activityCount"] = activity;

  JsonObject status = doc.createNestedObject("status");
  status["comfortScore"] = score;
  status["level"]        = level;
  status["color"]        = color;
  status["message"]      = message;
  status["lastUpdated"]  = (long)(millis() / 1000);

  String body;
  serializeJson(doc, body);

  int code = http.PUT(body);
  Serial.println("Firebase: HTTP " + String(code));
  http.end();
}

// ================================================================
// WIFI
// ================================================================

void connectWifi() {
  Serial.print("WiFi connecting to " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected — IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed — check credentials, continuing offline");
  }
}

// ================================================================
// FULL SCAN
// ================================================================

unsigned long lastScanTime = 0;

void runScan(String trigger) {
  Serial.println("\n======= SCAN (" + trigger + ") =======");

  float  temp     = readTemp();
  float  humidity = readHumidity();
  int    sound    = readSoundLevel();
  int    activity = getActivity();

  int    score = calcComfortScore(temp, humidity, sound, activity);
  String level = comfortLevel(score);
  String color = comfortColor(score);

  Serial.printf("Temp: %.1f°F  Hum: %.1f%%  Sound: %d  Activity: %d\n",
                temp, humidity, sound, activity);
  Serial.printf("Score: %d  Level: %s\n", score, level.c_str());

  Serial.println("Calling Gemini...");
  String message = callGemini(temp, humidity, sound, activity, score, level);
  Serial.println("Gemini: " + message);

  postToFirebase(temp, humidity, sound, activity, score, level, color, message);

  lastScanTime = millis();
  Serial.println("=========================================\n");
}

// ================================================================
// SETUP & LOOP
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n🐯 TigerCalm — ESP32-WROOM-DA");
  Serial.println("================================");

  pinMode(PIR_PIN, INPUT);
  dht.begin();
  delay(2000); // DHT11 warmup

  setupI2S();
  connectWifi();

  Serial.println("Running initial scan...");
  runScan("boot");
}

void loop() {
  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped — reconnecting");
    connectWifi();
  }

  // PIR check — record when it goes HIGH (rising edge only)
  bool pirNow = (digitalRead(PIR_PIN) == HIGH);
  if (pirNow && !pirLastState) {
    recordPir();
  }
  pirLastState = pirNow;

  // Periodic scan
  if (millis() - lastScanTime >= SCAN_INTERVAL_MS) {
    runScan("timer");
  }

  delay(100);
}
