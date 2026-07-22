#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Wire.h>

//---------- Wi-Fi / OTA ------------
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFi.h>

const char *ssid = "KAVI619";
const char *password = "12345678";

// ---------- REST API Settings ----------
const char *apiEndpoint =
    "http://192.168.137.1:3001/api/esp32Ingest"; // Local computer's IP address(chnaged 42 to1)
unsigned long lastPostTime = 0;
const unsigned long POST_INTERVAL = 10000UL; // Send readings every 10 seconds

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 17
#define OLED_SCL 16

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- DHT22 ----------------
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ---------------- MQ Sensors ----------------
// 0 = MQ2, 1 = MQ135
const int HEATER_PIN[2] = {21, 22};
const int ANALOG_PIN[2] = {33, 34};

const char *SENSOR_NAME[2] = {"MQ-2", "MQ-135"};

// Heater switches every 60 s
const unsigned long HEAT_DURATION = 60000UL;

int activeSensor = 0;
unsigned long stateStartTime = 0;

bool warmupComplete = false;
int cycleCount = 0;

// Latest values
float lastADC[2] = {0, 0};
float lastVoltage[2] = {0, 0};

// Max values seen this session (since power-on, only counted after warmup)
float maxADC[2] = {0, 0};

float temperature = 0;
float humidity = 0;

// ---------------- Food spoilage thresholds ----------------
const float MQ_BOTH_THRESHOLD = 400.0;     // MQ2 AND MQ135 both above this
const float MQ135_ALONE_THRESHOLD = 500.0; // MQ135 alone above this
bool foodSpoiled = false;

// ---------------- Wi-Fi power saving ----------------
// Wi-Fi/OTA stay on for this long after boot (enough time to upload new
// code), then get switched off completely to save power for the rest
// of the run. Change this value if you need a longer/shorter window.
const unsigned long WIFI_ON_DURATION = 20UL * 60UL * 1000UL; // 20 minutes
unsigned long bootTime = 0;
bool wifiIsOff = false;

//--------------------------------------------------

void setAllHeaters(int onIndex) {
  for (int i = 0; i < 2; i++) {
    digitalWrite(HEATER_PIN[i], (i == onIndex) ? HIGH : LOW);
  }
}

// Faster averaging (about 20ms per sensor)
float readSensorAverage(int analogPin) {
  long sum = 0;

  for (int i = 0; i < 10; i++) {
    sum += analogRead(analogPin);
    delay(2);
  }

  return sum / 10.0;
}

// Read ALL MQ sensors continuously
// NOTE: this is only called once warmup (first 2 heating cycles) is complete,
// so no readings are taken or stored before that point.
void readAllMQSensors() {
  for (int i = 0; i < 2; i++) {
    lastADC[i] = readSensorAverage(ANALOG_PIN[i]);
    lastVoltage[i] = lastADC[i] * 3.3 / 4095.0;

    // Track the max reading seen since power-on (post-warmup only)
    if (lastADC[i] > maxADC[i]) {
      maxADC[i] = lastADC[i];
    }
  }
}

// Check the spoilage condition using the PEAK (max) readings seen so far,
// not just the instantaneous reading. This makes the alert "latch" on:
// once either threshold is crossed, foodSpoiled stays true for the rest
// of the power cycle (maxADC never decreases until the board is reset).
//   (MQ2 AND MQ135 max both above MQ_BOTH_THRESHOLD at the same time)
//   OR (MQ135 max alone above MQ135_ALONE_THRESHOLD)
void checkFoodSpoilage() {
  bool bothHigh =
      (maxADC[0] > MQ_BOTH_THRESHOLD) && (maxADC[1] > MQ_BOTH_THRESHOLD);
  bool mq135VeryHigh = (maxADC[1] > MQ135_ALONE_THRESHOLD);

  foodSpoiled = bothHigh || mq135VeryHigh;
}

// Post sensor readings to REST API
void postSensorData() {
  if (WiFi.status() == WL_CONNECTED && !wifiIsOff) {
    HTTPClient http;
    http.begin(apiEndpoint);
    http.addHeader("Content-Type", "application/json");

    // Construct JSON payload
    String payload = "{\"device_id\":\"ESP32-WROOM\"";
    payload += ",\"mq2_value\":" + String(lastADC[0]);
    payload += ",\"mq135_value\":" + String(lastADC[1]);
    payload += ",\"temperature\":" + String(temperature, 1);
    payload += ",\"humidity\":" + String(humidity, 1);
    payload +=
        ",\"food_state\":\"" + String(foodSpoiled ? "spoiled" : "fresh") + "\"";
    payload += ",\"confidence\":0.95";
    payload += "}";

    int httpResponseCode = http.POST(payload);
    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
    } else {
      Serial.print("Error code on sending POST: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  }
}

// 128x64 OLED, text size 1 = 8px tall lines.
// Using a fixed 8px grid (0,8,16,24,32,40,48,56) so lines never overlap
// or get clipped: that's exactly 8 usable rows, top to bottom.
void updateDisplay(unsigned long remainingMs) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (!warmupComplete) {
    display.setCursor(0, 0);
    display.println("WARMUP MODE");

    display.setCursor(0, 8);
    display.print("Heating: ");
    display.println(SENSOR_NAME[activeSensor]);

    display.setCursor(0, 16);
    display.print("Cycle: ");
    display.print(cycleCount + 1);
    display.print("/2");

    display.setCursor(0, 24);
    display.print("Time left: ");
    display.print(remainingMs / 1000);
    display.println("s");

    // row at y=32 left blank as a spacer

    display.setCursor(0, 40);
    display.println("Sensor reads");

    display.setCursor(0, 48);
    display.println("disabled until");

    display.setCursor(0, 56);
    display.println("warmup completes");
  } else if (foodSpoiled) {
    // Keep the raw readings visible up top...
    display.setCursor(0, 0);
    display.print("MQ2 ");
    display.print((int)lastADC[0]);
    display.print("/");
    display.print((int)maxADC[0]);

    display.setCursor(0, 8);
    display.print("MQ135 ");
    display.print((int)lastADC[1]);
    display.print("/");
    display.print((int)maxADC[1]);

    display.setCursor(0, 16);
    display.print("T:");
    display.print(temperature, 1);
    display.print((char)247);
    display.print("C  H:");
    display.print(humidity, 0);
    display.print("%");

    // ...and give the alert its own high-contrast boxed banner
    // (white filled box, black text) across the bottom third
    // of the screen so it's impossible to miss.
    display.fillRect(0, 28, SCREEN_WIDTH, 36, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);

    display.setCursor(25, 36);
    display.println("!! WARNING !!");

    display.setCursor(16, 48);
    display.println("FOOD IS SPOILED");
  } else {
    // Row 1: MQ2 current/max
    display.setCursor(0, 0);
    display.print("MQ2 ");
    display.print((int)lastADC[0]);
    display.print("/");
    display.print((int)maxADC[0]);

    // Row 2: MQ135 current/max
    display.setCursor(0, 8);
    display.print("MQ135 ");
    display.print((int)lastADC[1]);
    display.print("/");
    display.print((int)maxADC[1]);

    // row at y=16 left blank as a spacer

    display.setCursor(0, 24);
    display.print("T:");
    display.print(temperature, 1);
    display.print((char)247);
    display.print("C");

    display.setCursor(70, 24);
    display.print("H:");
    display.print(humidity, 0);
    display.print("%");

    // row at y=32 left blank as a spacer

    display.setCursor(0, 40);
    display.print("Heating: ");
    display.println(SENSOR_NAME[activeSensor]);

    // row at y=48 left blank as a spacer

    display.setCursor(0, 56);
    display.print("Next: ");
    display.print(remainingMs / 1000);
    display.print("s");
  }

  display.display();
}

void setup() {
  Serial.begin(115200);

  // ---- Wi-Fi / OTA setup ----
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname("ESP32-WROOM");
    ArduinoOTA.setPassword("12345678");
    ArduinoOTA.begin();
  }

  // Start the 5-minute Wi-Fi/OTA window from power-on
  bootTime = millis();

  // ---- Sensor / display setup ----
  for (int i = 0; i < 2; i++) {
    pinMode(HEATER_PIN[i], OUTPUT);
    digitalWrite(HEATER_PIN[i], LOW);
  }

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (1)
      ;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  dht.begin();

  activeSensor = 0;
  stateStartTime = millis();

  // Start heating MQ-2
  setAllHeaters(activeSensor);
}

void loop() {
  unsigned long now = millis();

  if (!wifiIsOff) {
    // Still within the upload window: keep handling OTA requests
    ArduinoOTA.handle();

    // Once 5 minutes have passed since boot, switch Wi-Fi off for good
    // to save power. Re-flashing after this point needs a USB upload.
    if (now - bootTime >= WIFI_ON_DURATION) {
      ArduinoOTA.end();
      WiFi.disconnect(true); // drop the connection
      WiFi.mode(WIFI_OFF);   // power down the Wi-Fi radio
      wifiIsOff = true;
    }
  }

  unsigned long elapsed = now - stateStartTime;

  // Only read / store MQ sensor data once warmup (first 2 heating
  // cycles) has finished. During warmup no readings are taken at all,
  // so lastADC / maxADC stay untouched (0) and can't cause false triggers.
  if (warmupComplete) {
    readAllMQSensors();
    checkFoodSpoilage();

    // Periodically post sensor readings to the REST API
    if (now - lastPostTime >= POST_INTERVAL) {
      lastPostTime = now;
      postSensorData();
    }
  }

  // Read DHT22 every 15 seconds
  static unsigned long dhtTimer = 0;

  if (now - dhtTimer >= 15000) {
    dhtTimer = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t))
      temperature = t;

    if (!isnan(h))
      humidity = h;
  }

  unsigned long remaining =
      (elapsed >= HEAT_DURATION) ? 0 : (HEAT_DURATION - elapsed);

  updateDisplay(remaining);

  // Rotate heater every 60 seconds
  if (elapsed >= HEAT_DURATION) {
    activeSensor++;

    if (activeSensor >= 2) {
      activeSensor = 0;
      cycleCount++;

      // Require 2 full heating cycles before trusting readings,
      // since the first cycle can show faulty startup values
      if (cycleCount >= 2) {
        warmupComplete = true;
      }
    }

    stateStartTime = millis();
    setAllHeaters(activeSensor);
  }
}
