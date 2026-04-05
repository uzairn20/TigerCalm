🐯 TigerCalm
Campus Sensory Comfort Monitor — University of the Pacific Hackathon

Hack the Pacific Life — for every Tiger.


What is TigerCalm?
TigerCalm is a real-time campus comfort monitoring system designed to help students with autism, anxiety, and sensory processing disorders make informed decisions before entering any campus space.
Using a combination of physical sensors and Google AI, TigerCalm continuously monitors sound levels, temperature, humidity, and activity in a location — then delivers a live comfort score and a friendly AI-generated recommendation directly to a student-facing web dashboard accessible on any phone or laptop.

The Problem
For students with sensory sensitivities, walking into a loud crowded dining hall or a hot stuffy study room is not just uncomfortable — it can be completely overwhelming. There is currently no way for a Pacific student to know what conditions are like inside a space before they walk through the door. TigerCalm solves this.

How It Works
Physical Sensors → ESP32-C3 → Firebase → Dashboard → Gemini AI

The ESP32-C3 reads all three sensors every 10 to 30 seconds
It calculates a comfort score from 0 to 100
It calls Google Gemini to generate a friendly plain-English message
It posts everything to Firebase Realtime Database
The dashboard reads Firebase live and updates automatically
Students see live gauges and a recommendation on any device


Features

Real-time sound measurement using INMP441 I2S digital microphone
Temperature and humidity monitoring using DHT11 sensor
Activity detection using PIR motion sensor with rolling 2-minute window
Overall comfort score calculated from all four factors
Google Gemini AI generates friendly human-readable recommendations
Live dashboard with animated arc gauges — works on any phone or laptop
Instant motion alerts — dashboard updates immediately when activity is detected
No app download required — pure HTML dashboard, open in any browser


Hardware
Component — Purpose
Seeed Studio XIAO ESP32-C3 — Main microcontroller and WiFi
INMP441 I2S Microphone — Sound level measurement
DHT11 — Temperature and humidity
PIR Motion Sensor — Activity detection

Wiring
INMP441 SCK → GPIO 8 — D8
INMP441 WS → GPIO 9 — D9
INMP441 SD → GPIO 10 — D10
INMP441 L/R → GND
INMP441 VDD → 3.3V
DHT11 out → GPIO 6 — D4
PIR out → GPIO 7 — D5
PIR VCC → 5V

Technologies Used
Google Gemini API — Generates friendly plain-English comfort recommendations
Google Firebase Realtime Database — Streams live sensor data to the dashboard
ESP32-C3 Arduino Framework — Reads sensors and posts data over WiFi
INMP441 I2S — Professional-grade digital sound measurement
HTML and JavaScript — Live student-facing dashboard

Comfort Score Formula
The overall score from 0 to 100 is calculated from four factors:
Temperature — ideal range 68 to 74 degrees F
Humidity — ideal range 40 to 55 percent
Sound level — ideal below 30 out of 100
Activity — ideal under 3 triggers per 2 minutes

Project Structure
TigerCalm/
src/main.cpp — ESP32 sensor code
dashboard.html — Student-facing web dashboard
platformio.ini — PlatformIO board configuration

Team
University of the Pacific — GDSC Hackathon
Hack the Pacific Life
