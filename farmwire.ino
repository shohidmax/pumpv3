// =================================================================
// ESP32 Simple WebSocket Client - Fixed & Optimized
// =================================================================

#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <vector>

// --- Configuration ---
const char* websocket_server_host = "pumpv3-jpu6.onrender.com";
const uint16_t websocket_server_port = 443;
#define WDT_TIMEOUT 30 // 30 Seconds Watchdog

const char* firmwareUrl = "https://github.com/shohidmax/pumpv3/releases/download/shohidpump/abbu_pump_online.ino.bin";
const char* versionUrl = "https://raw.githubusercontent.com/shohidmax/pumpv3/refs/heads/main/version.txt";

// Current firmware version
const char* currentFirmwareVersion = "1.1.4"; // Incremented version for fix

// Time Configuration (UTC+6 for Bangladesh)
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 21600;
const int   daylightOffset_sec = 0;

// Timers
unsigned long lastUpdateCheck = 0;
const unsigned long updateCheckInterval = 5 * 60 * 1000; // 5 minutes

unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 10000; // Check WiFi every 10 seconds

// --- PIN DEFINITIONS ---
#define RELAY_1 12
#define RELAY_2 14
#define RELAY_3 13 // Renamed from relay_3 for consistency
#define SWITCH_1 19
#define SWITCH_2 18

// --- DATA STRUCTURES ---
struct MotorLog {
    String onTime;
    String offTime;
    String duration;
};

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

std::vector<MotorLog> motorLogs;
const int MAX_LOGS = 50; // Limit logs to save RAM

String relay1_startTimeStr = ""; // To store formatted start time

// --- FORWARD DECLARATIONS ---
void checkForFirmwareUpdate();
String fetchLatestVersion();
void downloadAndApplyFirmware();
bool startOTAUpdate(WiFiClient* client, int contentLength);
String getFormattedTime();
void addLog(String onTime, String offTime, String duration);
void sendLogPage(int page);

// --- FUNCTIONS ---

String getFormattedTime() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "N/A";
    }
    char timeStringBuff[30];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(timeStringBuff);
}

void addLog(String onTime, String offTime, String duration) {
    if (motorLogs.size() >= MAX_LOGS) {
        motorLogs.erase(motorLogs.begin()); // Remove oldest
    }
    motorLogs.push_back({onTime, offTime, duration});
}

void sendStatus() {
    String currentMotor = (digitalRead(SWITCH_1) == LOW) ? "ON" : "OFF";
    String currentMode = (digitalRead(SWITCH_2) == LOW) ? "Emergency" : "Normal";
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

void sendLogPage(int page) {
    int itemsPerPage = 10;
    int totalLogs = motorLogs.size();
    int totalPages = (totalLogs + itemsPerPage - 1) / itemsPerPage;
    if (totalPages == 0) totalPages = 1;

    if (page < 0) page = 0;
    if (page >= totalPages) page = totalPages - 1;

    JsonDocument doc;
    doc["type"] = "logPageUpdate";
    JsonObject payload = doc.createNestedObject("payload");
    
    payload["currentPage"] = page;
    payload["totalPages"] = totalPages;
    
    JsonArray logsArray = payload.createNestedArray("motorLogs");
    
    // Iterate backwards to show newest first
    int startIdx = totalLogs - 1 - (page * itemsPerPage);
    int endIdx = startIdx - itemsPerPage + 1;
    if (endIdx < 0) endIdx = 0;

    for (int i = startIdx; i >= endIdx && i >= 0; i--) {
        if (i < motorLogs.size()) { // Safety check
            JsonObject logItem = logsArray.createNestedObject();
            JsonDocument logJson; // Needed to serialize inside the array as string if client expects strings, 
                                  // OR just send objects. The JS client parses payload.motorLogs entries.
                                  // Client JS: JSON.parse(logString); inside the loop.
                                  // So we need to push a STRINGIFIED JSON object.
            
            logJson["onTime"] = motorLogs[i].onTime;
            logJson["offTime"] = motorLogs[i].offTime;
            logJson["duration"] = motorLogs[i].duration;
            
            String logString;
            serializeJson(logJson, logString);
            logsArray.add(logString);
        }
    }

    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(output);
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
                    relay1_startTimeStr = getFormattedTime(); // Capture start time
                } else if (command == "RELAY_2") {
                    digitalWrite(RELAY_2, HIGH);
                    relay2_timer = millis();
                    // Relay 2 is STOP. If Motor was ON, we need to log.
                    // But typically logs are generated when status changes.
                    // We will handle logging in the loop based on status change or explicit STOP command?
                    // Use SWITCH_1 status for robust logging.
                } else if (command == "RESET") {
                    digitalWrite(RELAY_3, HIGH);
                    relay3_timer = millis();
                } else if (command == "RESTART_ESP") {
                    webSocket.disconnect();
                    delay(500);
                    ESP.restart();
                } else if (command == "CHECK_UPDATE") {
                    checkForFirmwareUpdate();
                } else if (command == "GET_LOG_PAGE") {
                    int page = doc["value"] | 0;
                    sendLogPage(page);
                } else if (command == "CLEAR_LOGS") {
                    motorLogs.clear();
                    sendLogPage(0); // Send empty page
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
    pinMode(RELAY_3, OUTPUT); digitalWrite(RELAY_3, LOW);
    pinMode(SWITCH_1, INPUT_PULLUP);
    pinMode(SWITCH_2, INPUT_PULLUP);

    // Watchdog Setup (FIXED)
    // Deinit first to avoid "TWDT already initialized" error
    esp_task_wdt_deinit(); 
    
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000, // Corrected to ms (30000ms)
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL);

    // WiFi Setup
    WiFi.mode(WIFI_STA);
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // 3 Minutes timeout for hotspot

    if (!wm.autoConnect("ESP32-Simple")) {
        Serial.println("Failed to connect. Restarting...");
        delay(3000);
        ESP.restart();
    }

    Serial.println("WiFi Connected!");
    Serial.println("IP: " + WiFi.localIP().toString());
    Serial.println("Current Version: " + String(currentFirmwareVersion));

    // Time Setup
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Waiting for time sync...");
    // Give time to sync but don't block too long
    unsigned long startSync = millis();
    while (time(nullptr) < 100000 && millis() - startSync < 5000) {
        delay(100);
    }
    Serial.println("Time: " + getFormattedTime());

    // Check for update ONCE at startup
    checkForFirmwareUpdate();
    
    // WebSocket Setup
    webSocket.beginSSL(websocket_server_host, websocket_server_port, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}

// Global state for logging based on actual motor status (SWITCH_1)
bool lastMotorStateOn = false;
unsigned long motorStartTime = 0;
String motorStartTimeStr = "";

void loop() {
    esp_task_wdt_reset(); // Keep device alive
    webSocket.loop();

    // WiFi Check
    if (millis() - lastWifiCheck > wifiCheckInterval) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi Lost! Attempting reconnect...");
            WiFi.disconnect();
            WiFi.reconnect();
        }
    }

    // Handle Relay Timers
    unsigned long currentMillis = millis();
    if (relay1_timer > 0 && currentMillis - relay1_timer >= relay_duration) {
        digitalWrite(RELAY_1, LOW); relay1_timer = 0;
    }
    if (relay2_timer > 0 && currentMillis - relay2_timer >= relay_duration) {
        digitalWrite(RELAY_2, LOW); relay2_timer = 0;
    }
    if (relay3_timer > 0 && currentMillis - relay3_timer >= relay_duration) {
        digitalWrite(RELAY_3, LOW); relay3_timer = 0;
    }

    // Logic to Generate Logs based on SWITCH_1 (Feedback)
    // SWITCH_1 is LOW when Motor is ON (INPUT_PULLUP logic from original code seems to imply logic)
    // Line 59: String currentMotor = (digitalRead(SWITCH_1) == LOW) ? "ON" : "OFF";
    bool currentMotorStateOn = (digitalRead(SWITCH_1) == LOW);

    if (currentMotorStateOn && !lastMotorStateOn) {
        // Motor Just Started
        motorStartTime = millis();
        motorStartTimeStr = getFormattedTime();
        lastMotorStateOn = true;
    } else if (!currentMotorStateOn && lastMotorStateOn) {
        // Motor Just Stopped
        unsigned long durationMillis = millis() - motorStartTime;
        unsigned long durationSeconds = durationMillis / 1000;
        
        int hours = durationSeconds / 3600;
        int minutes = (durationSeconds % 3600) / 60;
        int seconds = durationSeconds % 60;
        char durationStr[20];
        snprintf(durationStr, sizeof(durationStr), "%02dh %02dm %02ds", hours, minutes, seconds);

        String offTimeStr = getFormattedTime();
        
        // Add to log
        addLog(motorStartTimeStr, offTimeStr, String(durationStr));
        Serial.println("Log Added: " + motorStartTimeStr + " - " + offTimeStr);
        
        // Notify clients of new logs
        sendLogPage(0); // Refresh first page
        
        lastMotorStateOn = false;
    }

    // Check Status and Send Update
    sendStatus();
}

void checkForFirmwareUpdate() {
  Serial.println("Checking for firmware update...");
  if (WiFi.status() != WL_CONNECTED) return;

  String latestVersion = fetchLatestVersion();
  if (latestVersion == "") {
    Serial.println("Failed to fetch latest version");
    return;
  }

  Serial.println("Current: " + String(currentFirmwareVersion));
  Serial.println("Latest: " + latestVersion);

  if (latestVersion != currentFirmwareVersion) {
    Serial.println("New firmware available. Starting OTA update...");
    esp_task_wdt_reset(); 
    downloadAndApplyFirmware();
  } else {
    Serial.println("Device is up to date.");
  }
}

String fetchLatestVersion() {
  HTTPClient http;
  http.setTimeout(10000); 
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

  const unsigned long timeoutDuration = 120 * 1000; 
  unsigned long lastDataTime = millis();

  while (written < contentLength) {
    esp_task_wdt_reset(); 

    if (client->available()) {
      uint8_t buffer[256]; 
      size_t len = client->read(buffer, sizeof(buffer));
      if (len > 0) {
        Update.write(buffer, len);
        written += len;
        
        lastDataTime = millis(); 

        progress = (written * 100) / contentLength;
        if (progress != lastProgress) {
          Serial.printf("Progress: %d%%\n", progress);
          lastProgress = progress;
        }
      }
    }
    
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