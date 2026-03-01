// ================================================================
//   SMART IRRIGATION SYSTEM - ADVANCED VERSION
//   ESP32 + DHT11 + Soil Sensor + OLED + ThingSpeak + Alerts
// ================================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "DHTesp.h"

// ------------------- CHANGE THESE -------------------a
const char* ssid     = "GANESH";
const char* password = "99999999";
String apiKey        = "HMTC7XHC0OLVRF8A";   // ThingSpeak Write API Key
String readApiKey    = "YOUR_READ_API_KEY";    // ThingSpeak Read API Key (optional)
long   channelID     = 0;                      // Your ThingSpeak Channel ID
// ---------------------------------------------------

// ------------------- PIN CONFIG -------------------
#define SOIL_PIN    34
#define RELAY_PIN   14
#define DHT_PIN     27
#define BTN_PIN     13   // Manual override button (optional, add a push button)
// ---------------------------------------------------

// ------------------- OLED CONFIG ------------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
// ---------------------------------------------------

// ------------------- CALIBRATION ------------------
int dryValue = 4095;
int wetValue = 1200;
// ---------------------------------------------------

// ------------------- THRESHOLDS -------------------
#define MOISTURE_LOW      40    // Turn pump ON below this
#define MOISTURE_HIGH     70    // Turn pump OFF above this (hysteresis)
#define TEMP_ALERT_HIGH   40.0  // Temp too high alert
#define TEMP_ALERT_LOW    5.0   // Temp too low alert
#define HUM_ALERT_LOW     20    // Humidity too low alert
// ---------------------------------------------------

// ------------------- TIMING -----------------------
#define UPLOAD_INTERVAL   15000   // ThingSpeak upload interval (ms)
#define SENSOR_INTERVAL   2000    // Sensor read interval (ms)
#define DISPLAY_PAGES     3       // Number of OLED screen pages
#define PAGE_DURATION     4000    // Duration each page shows (ms)
// ---------------------------------------------------

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
DHTesp dht;

// ------------------- GLOBALS ----------------------
float temperature   = 0;
float humidity      = 0;
int   moisture      = 0;
int   adcRaw        = 0;
int   pumpStatus    = 0;
bool  manualOverride = false;
bool  manualPumpState = false;

unsigned long lastUpload    = 0;
unsigned long lastSensor    = 0;
unsigned long lastPageSwitch = 0;
unsigned long pumpStartTime = 0;
unsigned long totalPumpRuntime = 0;

int   currentPage        = 0;
int   uploadCount        = 0;
int   lastHttpCode       = 0;
bool  wifiConnected      = false;

// Alert flags
bool alertTemp  = false;
bool alertHum   = false;
bool alertDry   = false;
// ---------------------------------------------------

// ================================================================
//   WIFI
// ================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("SMART IRRIGATION");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setCursor(0, 16);
  display.println("Connecting WiFi...");
  display.setCursor(0, 28);
  display.print(ssid);
  display.display();

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 30) {
    delay(500);
    Serial.print(".");
    timeout++;
    // Animate dots on OLED
    display.setCursor(0, 40);
    for (int i = 0; i < (timeout % 4); i++) display.print(".");
    display.display();
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("SMART IRRIGATION");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setCursor(0, 20);

  if (wifiConnected) {
    Serial.println("\nWiFi Connected!");
    Serial.println(WiFi.localIP());
    display.println("WiFi Connected!");
    display.setCursor(0, 34);
    display.setTextSize(1);
    display.print("IP: ");
    display.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi Failed! Running offline.");
    display.println("WiFi FAILED");
    display.setCursor(0, 34);
    display.println("Running offline...");
  }
  display.display();
  delay(2000);
}

// ================================================================
//   SENSOR READINGS
// ================================================================
void readSensors() {
  // --- Soil Moisture (average 5 reads for stability) ---
  long sum = 0;
  for (int i = 0; i < 5; i++) {
    sum += analogRead(SOIL_PIN);
    delay(10);
  }
  adcRaw = sum / 5;
  moisture = map(adcRaw, dryValue, wetValue, 0, 100);
  moisture = constrain(moisture, 0, 100);

  // --- DHT11 ---
  TempAndHumidity data = dht.getTempAndHumidity();

  // Validate DHT reading (DHT11 sometimes returns NaN)
  if (!isnan(data.temperature) && !isnan(data.humidity)) {
    temperature = data.temperature;
    humidity    = data.humidity;
  }

  // --- Alerts ---
  alertDry  = (moisture < MOISTURE_LOW);
  alertTemp = (temperature > TEMP_ALERT_HIGH || temperature < TEMP_ALERT_LOW);
  alertHum  = (humidity < HUM_ALERT_LOW);

  Serial.printf("[SENSOR] Moisture: %d%% (ADC:%d) | Temp: %.1f C | Hum: %.0f%%\n",
                moisture, adcRaw, temperature, humidity);
}

// ================================================================
//   PUMP CONTROL  (hysteresis to avoid rapid switching)
// ================================================================
void controlPump() {
  if (manualOverride) {
    // Manual mode: use whatever state was set by button
    if (manualPumpState) {
      digitalWrite(RELAY_PIN, LOW);   // ON
      pumpStatus = 1;
    } else {
      digitalWrite(RELAY_PIN, HIGH);  // OFF
      pumpStatus = 0;
    }
    return;
  }

  // Auto mode with hysteresis
  if (moisture < MOISTURE_LOW && pumpStatus == 0) {
    digitalWrite(RELAY_PIN, LOW);   // Turn ON
    pumpStatus = 1;
    pumpStartTime = millis();
    Serial.println("[PUMP] Turned ON — low moisture");
  } else if (moisture >= MOISTURE_HIGH && pumpStatus == 1) {
    digitalWrite(RELAY_PIN, HIGH);  // Turn OFF
    totalPumpRuntime += (millis() - pumpStartTime);
    pumpStatus = 0;
    Serial.println("[PUMP] Turned OFF — moisture adequate");
  }
}

// ================================================================
//   MANUAL OVERRIDE BUTTON
// ================================================================
void checkButton() {
  static bool lastState = HIGH;
  static unsigned long pressTime = 0;
  bool state = digitalRead(BTN_PIN);

  if (state == LOW && lastState == HIGH) {
    pressTime = millis();
  }

  if (state == HIGH && lastState == LOW) {
    unsigned long held = millis() - pressTime;

    if (held > 2000) {
      // Long press: toggle manual override mode
      manualOverride = !manualOverride;
      if (!manualOverride) {
        Serial.println("[BTN] Manual override DISABLED — back to AUTO");
      } else {
        manualPumpState = false;
        Serial.println("[BTN] Manual override ENABLED");
      }
    } else if (held > 50) {
      // Short press: if in override, toggle pump
      if (manualOverride) {
        manualPumpState = !manualPumpState;
        Serial.printf("[BTN] Manual pump: %s\n", manualPumpState ? "ON" : "OFF");
      }
    }
  }

  lastState = state;
}

// ================================================================
//   OLED DISPLAY - Multiple Pages
// ================================================================

// --- Page 0: Main Overview ---
void displayPage0() {
  display.clearDisplay();
  display.setTextSize(1);

  // Header
  display.fillRect(0, 0, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 1);
  display.print("SMART IRRIGATION");
  if (manualOverride) {
    display.setCursor(100, 1);
    display.print("MAN");
  }
  display.setTextColor(SSD1306_WHITE);

  // Moisture bar
  display.setCursor(0, 13);
  display.print("Soil:");
  display.print(moisture);
  display.print("%");

  int barWidth = map(moisture, 0, 100, 0, 80);
  display.drawRect(45, 13, 82, 8, SSD1306_WHITE);
  display.fillRect(45, 13, barWidth, 8, SSD1306_WHITE);

  // Temp & Hum
  display.setCursor(0, 25);
  display.print("Temp:");
  display.print(temperature, 1);
  display.print("C");

  display.setCursor(0, 35);
  display.print("Hum: ");
  display.print((int)humidity);
  display.print("%");

  // Pump status
  display.setCursor(0, 46);
  display.print("Pump:");
  if (pumpStatus) {
    display.fillRoundRect(30, 44, 30, 10, 2, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(33, 46);
    display.print(" ON");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.drawRoundRect(30, 44, 32, 10, 2, SSD1306_WHITE);
    display.setCursor(33, 46);
    display.print("OFF");
  }

  // WiFi indicator
  display.setCursor(75, 46);
  display.print("WiFi:");
  display.print(wifiConnected ? "OK" : "--");

  // Alert icons
  if (alertDry || alertTemp || alertHum) {
    display.setCursor(0, 56);
    display.print("! ALERT:");
    if (alertDry)  display.print("DRY ");
    if (alertTemp) display.print("TEMP ");
    if (alertHum)  display.print("HUM");
  } else {
    display.setCursor(0, 56);
    display.print("Status: NORMAL");
  }

  display.display();
}

// --- Page 1: Sensor Detail ---
void displayPage1() {
  display.clearDisplay();
  display.setTextSize(1);

  display.fillRect(0, 0, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(12, 1);
  display.print("-- SENSOR DATA --");
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 13);
  display.print("ADC Raw  : ");
  display.println(adcRaw);

  display.setCursor(0, 23);
  display.print("Moisture : ");
  display.print(moisture);
  display.println("%");

  display.setCursor(0, 33);
  display.print("Temp     : ");
  display.print(temperature, 2);
  display.println(" C");

  display.setCursor(0, 43);
  display.print("Humidity : ");
  display.print((int)humidity);
  display.println("%");

  display.setCursor(0, 53);
  display.print("DHT Pin:27 Soil:34 Rly:14");

  display.display();
}

// --- Page 2: System / Upload Info ---
void displayPage2() {
  display.clearDisplay();
  display.setTextSize(1);

  display.fillRect(0, 0, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(15, 1);
  display.print("-- SYSTEM INFO --");
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 13);
  display.print("Uploads : ");
  display.println(uploadCount);

  display.setCursor(0, 23);
  display.print("Last HTTP: ");
  display.println(lastHttpCode);

  display.setCursor(0, 33);
  unsigned long uptime = millis() / 1000;
  display.print("Uptime  : ");
  display.print(uptime / 3600); display.print("h ");
  display.print((uptime % 3600) / 60); display.print("m ");
  display.print(uptime % 60); display.println("s");

  display.setCursor(0, 43);
  unsigned long pumpSec = totalPumpRuntime / 1000;
  if (pumpStatus) pumpSec += (millis() - pumpStartTime) / 1000;
  display.print("Pump RT : ");
  display.print(pumpSec / 60); display.print("m ");
  display.print(pumpSec % 60); display.println("s");

  display.setCursor(0, 53);
  display.print("Mode: ");
  display.print(manualOverride ? "MANUAL" : "AUTO");
  display.print("  SSID:");
  display.print(ssid);

  display.display();
}

void updateDisplay() {
  unsigned long now = millis();
  if (now - lastPageSwitch >= PAGE_DURATION) {
    currentPage = (currentPage + 1) % DISPLAY_PAGES;
    lastPageSwitch = now;
  }

  switch (currentPage) {
    case 0: displayPage0(); break;
    case 1: displayPage1(); break;
    case 2: displayPage2(); break;
  }
}

// ================================================================
//   THINGSPEAK UPLOAD
// ================================================================
void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TS] WiFi not connected, skipping upload.");
    return;
  }

  // Reconnect if dropped
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    delay(3000);
  }

  HTTPClient http;
  String url = "http://api.thingspeak.com/update?api_key=" + apiKey
             + "&field1=" + String(moisture)
             + "&field2=" + String(temperature, 1)
             + "&field3=" + String((int)humidity)
             + "&field4=" + String(pumpStatus)
             + "&field5=" + String(adcRaw)
             + "&field6=" + String(alertDry ? 1 : 0);

  http.begin(url);
  http.setTimeout(8000);
  lastHttpCode = http.GET();
  http.end();

  uploadCount++;
  wifiConnected = (lastHttpCode > 0);

  Serial.printf("[TS] Upload #%d | HTTP: %d | M:%d%% T:%.1f H:%d P:%d\n",
                uploadCount, lastHttpCode, moisture, temperature, (int)humidity, pumpStatus);
}

// ================================================================
//   SERIAL PRINT DASHBOARD
// ================================================================
void printSerial() {
  Serial.println("========================================");
  Serial.printf("  Moisture : %d%% (ADC: %d)\n", moisture, adcRaw);
  Serial.printf("  Temp     : %.1f C\n", temperature);
  Serial.printf("  Humidity : %.0f%%\n", humidity);
  Serial.printf("  Pump     : %s\n", pumpStatus ? "ON" : "OFF");
  Serial.printf("  Mode     : %s\n", manualOverride ? "MANUAL" : "AUTO");
  Serial.printf("  WiFi     : %s\n", wifiConnected ? "Connected" : "Disconnected");
  if (alertDry || alertTemp || alertHum) {
    Serial.print("  ALERTS   : ");
    if (alertDry)  Serial.print("[DRY] ");
    if (alertTemp) Serial.print("[TEMP] ");
    if (alertHum)  Serial.print("[HUM] ");
    Serial.println();
  }
  Serial.println("========================================");
}

// ================================================================
//   SETUP
// ================================================================
void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed!");
    while (1);
  }
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 10);
  display.println("SMART");
  display.setCursor(10, 30);
  display.println("IRRIGATE");
  display.setTextSize(1);
  display.setCursor(10, 52);
  display.println("v2.0  Advanced");
  display.display();
  delay(2000);

  // Pins
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);   // Pump OFF (active LOW)
  pinMode(BTN_PIN, INPUT_PULLUP);  // Manual override button

  // DHT
  dht.setup(DHT_PIN, DHTesp::DHT11);

  // WiFi
  connectWiFi();

  Serial.println("\n[BOOT] Smart Irrigation v2.0 Ready");
  Serial.println("[BOOT] Auto threshold: ON <" + String(MOISTURE_LOW) + "% | OFF >" + String(MOISTURE_HIGH) + "%");
  Serial.println("[BOOT] Long-press GPIO" + String(BTN_PIN) + " to toggle manual override");
}

// ================================================================
//   LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  // --- Button check (always) ---
  checkButton();

  // --- Sensor Read ---
  if (now - lastSensor >= SENSOR_INTERVAL) {
    lastSensor = now;
    readSensors();
    controlPump();
    printSerial();
  }

  // --- Display Update (every loop, handles page timing) ---
  updateDisplay();

  // --- ThingSpeak Upload ---
  if (now - lastUpload >= UPLOAD_INTERVAL) {
    lastUpload = now;
    uploadToThingSpeak();
  }

  delay(50);
}
