// =================================================================
// ESP32 Simple WebSocket Client - Fixed & Optimized (Always Connected)
// =================================================================

#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_task_wdt.h>

// --- Configuration ---
const char* websocket_server_host = "pumpv3-jpu6.onrender.com";
const uint16_t websocket_server_port = 443;
#define WDT_TIMEOUT 30 // 30 Seconds Watchdog

// Updated URLs from your latest code
const char* firmwareUrl = "https://github.com/shohidmax/pumpv3/releases/download/shohidpump/abbu_pump_online.ino.bin";
const char* versionUrl = "https://raw.githubusercontent.com/shohidmax/pumpv3/refs/heads/main/version.txt";

// Current firmware version
const char* currentFirmwareVersion = "1.1.3";

// Timers
unsigned long lastUpdateCheck = 0;
const unsigned long updateCheckInterval = 5 * 60 * 1000; // 5 minutes

unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 10000; // Check WiFi every 10 seconds

// --- PIN DEFINITIONS ---
#define RELAY_1 12
#define RELAY_2 14
#define relay_3 13
#define SWITCH_1 23
#define SWITCH_2 22

// --- GLOBAL VARIABLES ---
WebSocketsClient webSocket;
unsigned long relay1_timer = 0;
unsigned long relay2_timer = 0;
unsigned long relay3_timer = 0;
const int relay_duration = 1000; // 1 Second Pulse

unsigned long lastStatusUpdate = 0;
String lastMotorStat = "";
String lastSysMode = "";
int lastWifiSignal = 0;

// --- FORWARD DECLARATIONS ---
void checkForFirmwareUpdate();
String fetchLatestVersion();
void downloadAndApplyFirmware();
bool startOTAUpdate(WiFiClient* client, int contentLength);

// --- FUNCTIONS ---

void sendStatus() {
    String currentMotor = (digitalRead(SWITCH_1) == LOW) ? "ON" : "OFF";
    String currentMode = (digitalRead(SWITCH_2) == LOW) ? "Normal" : "Emergency";//Emergency Normal
    int currentSignal = constrain(map(WiFi.RSSI(), -100, -30, 0, 100), 0, 100);

    // Only send data if something changed or every 5 seconds
    if (currentMotor != lastMotorStat || currentMode != lastSysMode || abs(currentSignal - lastWifiSignal) > 5 || millis() - lastStatusUpdate > 5000) {
        
        JsonDocument doc;
        doc["type"] = "statusUpdate";
        
        JsonObject payload = doc.createNestedObject("payload");
        payload["motorStatus"] = currentMotor;
        payload["systemMode"] = currentMode;
        payload["wifiSignal"] = currentSignal;
        payload["localIP"] = WiFi.localIP().toString();
        payload["version"] = currentFirmwareVersion;
        
        String jsonString;
        serializeJson(doc, jsonString);
        webSocket.sendTXT(jsonString);

        lastMotorStat = currentMotor;
        lastSysMode = currentMode;
        lastWifiSignal = currentSignal;
        lastStatusUpdate = millis();
    }
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    esp_task_wdt_reset(); // Reset Watchdog on activity
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("[WSc] Disconnected!");
            break;
        case WStype_CONNECTED:
            Serial.println("[WSc] Connected!");
            webSocket.sendTXT("{\"type\":\"esp32-identify\"}");
            break;
        case WStype_TEXT: {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);
            if (!error) {
                String command = doc["command"];
                if (command == "RELAY_1") {
                    digitalWrite(RELAY_1, HIGH);
                    relay1_timer = millis();
                } else if (command == "RELAY_2") {
                    digitalWrite(RELAY_2, HIGH);
                    relay2_timer = millis();
                } else if (command == "RESET") {
                    digitalWrite(relay_3, HIGH);
                    relay3_timer = millis();
                } else if (command == "RESTART_ESP") {
                    webSocket.disconnect();
                    delay(500);
                    ESP.restart();
                } else if (command == "CHECK_UPDATE") {
                    checkForFirmwareUpdate();
                }
                // Send immediate update after command
                lastStatusUpdate = 0; // Force update
            }
            break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    
    // Initialize Pins
    pinMode(RELAY_1, OUTPUT); digitalWrite(RELAY_1, LOW);
    pinMode(RELAY_2, OUTPUT); digitalWrite(RELAY_2, LOW);
    pinMode(relay_3, OUTPUT); digitalWrite(relay_3, LOW);
    pinMode(SWITCH_1, INPUT_PULLUP);
    pinMode(SWITCH_2, INPUT_PULLUP);

    // Watchdog Setup (Initialize early to catch boot loops)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL);

    // WiFi Setup
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true); // Enable Auto Reconnect Hardware Level
    WiFi.persistent(true);       // Save WiFi settings
    
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // 3 Minutes timeout for hotspot

    if (!wm.autoConnect("ESP32-Simple")) {
        Serial.println("Failed to connect. Restarting...");
        delay(3000);
        ESP.restart();
    }

    Serial.println("WiFi Connected!");
    Serial.println("Current Version: " + String(currentFirmwareVersion));

    // Check for update ONCE at startup
    checkForFirmwareUpdate();
    
    // WebSocket Setup
    webSocket.beginSSL(websocket_server_host, websocket_server_port, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}

void loop() {
    esp_task_wdt_reset(); // Keep device alive
    webSocket.loop();

    // FIXED: Smart Reconnect Logic
    // এটি প্রতি ১০ সেকেন্ডে চেক করবে কানেকশন আছে কিনা। না থাকলে রিকানেক্ট করবে।
    // এটি লুপ আটকে রাখবে না।
    if (millis() - lastWifiCheck > wifiCheckInterval) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi Lost! Attempting reconnect...");
            WiFi.reconnect(); // DO NOT USE disconnect() here
        }
    }

    // Handle Relay Timers (Non-blocking)
    unsigned long currentMillis = millis();
    if (relay1_timer > 0 && currentMillis - relay1_timer >= relay_duration) {
        digitalWrite(RELAY_1, LOW); relay1_timer = 0;
    }
    if (relay2_timer > 0 && currentMillis - relay2_timer >= relay_duration) {
        digitalWrite(RELAY_2, LOW); relay2_timer = 0;
    }
    if (relay3_timer > 0 && currentMillis - relay3_timer >= relay_duration) {
        digitalWrite(relay_3, LOW); relay3_timer = 0;
    }

    // Check Status and Send Update
    sendStatus();
}

void checkForFirmwareUpdate() {
  Serial.println("Checking for firmware update...");
  if (WiFi.status() != WL_CONNECTED) return;

  // Step 1: Fetch the latest version
  String latestVersion = fetchLatestVersion();
  if (latestVersion == "") {
    Serial.println("Failed to fetch latest version");
    return;
  }

  Serial.println("Current: " + String(currentFirmwareVersion));
  Serial.println("Latest: " + latestVersion);

  // Step 2: Compare versions
  if (latestVersion != currentFirmwareVersion) {
    Serial.println("New firmware available. Starting OTA update...");
    esp_task_wdt_reset(); // Reset WDT before heavy task
    downloadAndApplyFirmware();
  } else {
    Serial.println("Device is up to date.");
  }
}

String fetchLatestVersion() {
  HTTPClient http;
  http.setTimeout(10000); // 10s Timeout
  http.begin(versionUrl);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String latestVersion = http.getString();
    latestVersion.trim();
    http.end();
    return latestVersion;
  } else {
    Serial.printf("Failed to fetch version. HTTP code: %d\n", httpCode);
    http.end();
    return "";
  }
}

void downloadAndApplyFirmware() {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000); 
  http.begin(firmwareUrl);

  int httpCode = http.GET();
  Serial.printf("HTTP GET code: %d\n", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    Serial.printf("Firmware size: %d bytes\n", contentLength);

    if (contentLength > 0) {
      WiFiClient* stream = http.getStreamPtr();
      if (startOTAUpdate(stream, contentLength)) {
        Serial.println("OTA update successful, restarting...");
        delay(1000);
        ESP.restart();
      } else {
        Serial.println("OTA update failed");
      }
    } else {
      Serial.println("Invalid firmware size");
    }
  } else {
    Serial.printf("Failed to fetch firmware. HTTP code: %d\n", httpCode);
  }
  http.end();
}

bool startOTAUpdate(WiFiClient* client, int contentLength) {
  Serial.println("Initializing update...");
  if (!Update.begin(contentLength)) {
    Serial.printf("Update begin failed: %s\n", Update.errorString());
    return false;
  }

  Serial.println("Writing firmware...");
  size_t written = 0;
  int progress = 0;
  int lastProgress = 0;

  const unsigned long timeoutDuration = 120 * 1000; // 2 Minutes
  unsigned long lastDataTime = millis();

  while (written < contentLength) {
    esp_task_wdt_reset(); // CRITICAL: Prevent Watchdog Reset during OTA

    if (client->available()) {
      uint8_t buffer[256];
      size_t len = client->read(buffer, sizeof(buffer));
      if (len > 0) {
        Update.write(buffer, len);
        written += len;
        
        lastDataTime = millis(); // Reset timeout on data

        // Calculate and print progress
        progress = (written * 100) / contentLength;
        if (progress != lastProgress) {
          Serial.printf("Progress: %d%%\n", progress);
          lastProgress = progress;
        }
      }
    }
    
    // Check for timeout
    if (millis() - lastDataTime > timeoutDuration) {
      Serial.println("Error: Connection timed out during update.");
      Update.abort();
      return false;
    }

    yield();
  }

  if (written != contentLength) {
    Serial.printf("Error: Write incomplete. Exp: %d, Got: %d\n", contentLength, written);
    Update.abort();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("Error: Update end failed: %s\n", Update.errorString());
    return false;
  }

  return true;
}