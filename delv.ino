// =================================================================
// ESP32 WebSocket IoT Controller - FINAL VERSION with Local Admin Panel & Log Pagination
// =================================================================

// LIBRARIES
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include "time.h"
#include <WebServer.h>

// --- OBJECTS ---
WebSocketsClient webSocket;
Preferences preferences;
WebServer server(80);

// --- Configuration ---
String websocket_server_host;
const uint16_t websocket_server_port = 443;
#define WDT_TIMEOUT 20

// NTP Time Configuration
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 6 * 3600; // Bangladesh Standard Time (GMT+6)
const int   daylightOffset_sec = 0;

// --- PIN DEFINITIONS ---
#define RELAY_1 12
#define RELAY_2 14
#define relay_3 13
#define SWITCH_1 19
#define SWITCH_2 18

// --- GLOBAL VARIABLES ---
unsigned long relay1_timer = 0, relay2_timer = 0, relay3_timer = 0;
const int relay_duration = 1000;

unsigned long lastStatusUpdateTime = 0, lastKeepAliveTime = 0;
String Motor_Stat = "OFF", System_Mode = "Normal", lastAction = "System Boot";
int wifiSignal = 0;

String lastMotorStatus = "OFF";
bool timeSynced = false;
unsigned long lastWifiCheckTime = 0;

// --- FORWARD DECLARATIONS ---
void sendStatusUpdate();
void sendLogPage(int page);
void addMotorLog(time_t onTimestamp, time_t offTimestamp);
String formatDuration(unsigned long seconds);
void connectWiFi();

// --- Local Admin Panel HTML ---
const char adminPanelHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en" data-theme="light"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP32 Admin Panel</title><link href="https://cdn.jsdelivr.net/npm/daisyui@4.10.2/dist/full.min.css" rel="stylesheet" type="text/css" /><script src="https://cdn.tailwindcss.com"></script></head><body class="bg-base-200 min-h-screen p-4 sm:p-8 flex items-center justify-center"><div class="max-w-md w-full"><div class="text-center mb-6"><h1 class="text-3xl font-bold">Device Admin Panel</h1><p class="text-base-content/70">Configure your device settings below.</p></div><div class="card bg-base-100 shadow-xl mb-6"><div class="card-body"><h2 class="card-title">WebSocket Server</h2><form action="/save" method="POST"><div class="form-control"><label class="label"><span class="label-text">Server Host URL</span></label><input type="text" name="host" placeholder="e.g., pumpv3.onrender.com" class="input input-bordered w-full" value="%WEBSOCKET_HOST%"/></div><div class="form-control mt-4"><button type="submit" class="btn btn-primary">Save and Restart</button></div></form></div></div><div class="card bg-base-100 shadow-xl"><div class="card-body"><h2 class="card-title">WiFi Configuration</h2><p>Clicking this button will erase saved WiFi credentials and restart the device into AP mode.</p><form action="/resetwifi" method="POST" onsubmit="return confirm('Are you sure you want to reset WiFi settings?');"><div class="form-control mt-4"><button type="submit" class="btn btn-error">Reset WiFi Settings</button></div></form></div></div></div></body></html>
)rawliteral";

// --- Web Server Handlers ---
void handleRoot() {
  String html = adminPanelHtml;
  html.replace("%WEBSOCKET_HOST%", websocket_server_host);
  server.send(200, "text/html", html);
}

void handleSave() {
  String newHost = server.arg("host");
  if (newHost.length() > 0) {
    preferences.begin("esp-config", false);
    preferences.putString("ws_host", newHost);
    preferences.end();
    server.send(200, "text/plain", "Host saved. The device will now restart.");
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Host cannot be empty.");
  }
}

void handleResetWifi() {
    server.send(200, "text/plain", "WiFi settings reset. Restarting and opening config portal...");
    delay(1000);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
}

// --- WebSocket Event Handler ---
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    esp_task_wdt_reset();
    switch(type) {
        case WStype_DISCONNECTED: Serial.println("[WSc] Disconnected!"); break;
        case WStype_CONNECTED:
            Serial.println("[WSc] Connected to server!");
            webSocket.sendTXT("{\"type\":\"esp32-identify\"}");
            break;
        case WStype_TEXT: {
            JsonDocument doc;
            if (deserializeJson(doc, payload) == DeserializationError::Ok) {
                String command = doc["command"];
                if (command == "RELAY_1") {
                    digitalWrite(RELAY_1, HIGH); relay1_timer = millis();
                    lastAction = "Motor ON (Web)";
                } else if (command == "RELAY_2") {
                    digitalWrite(RELAY_2, HIGH); relay2_timer = millis();
                    lastAction = "Motor OFF (Web)";
                } else if (command == "RESET") {
                    digitalWrite(relay_3, HIGH); relay3_timer = millis();
                    lastAction = "System Reset (Web)";
                } else if (command == "RESTART_ESP") {
                    lastAction = "Restarting device..."; sendStatusUpdate();
                    delay(1000); ESP.restart();
                } else if (command == "CLEAR_LOGS") {
                    preferences.begin("motor_logs", false); preferences.clear(); preferences.end();
                    lastAction = "Logs Cleared"; Serial.println("All motor logs cleared.");
                } else if (command == "GET_LOG_PAGE") {
                    sendLogPage(doc["value"]);
                    return; 
                }
                sendStatusUpdate();
            }
            break;
        }
        default: break;
    }
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    Serial.println("\n\n--- Booting ESP32 Controller with Admin Panel ---");
    delay(30000);
    preferences.begin("esp-config", true); 
    websocket_server_host = preferences.getString("ws_host", "pumpv3.onrender.com");
    preferences.end();
    
    pinMode(RELAY_1, OUTPUT); pinMode(RELAY_2, OUTPUT); pinMode(relay_3, OUTPUT);
    pinMode(SWITCH_1, INPUT_PULLUP); pinMode(SWITCH_2, INPUT_PULLUP);
    digitalWrite(RELAY_1, LOW); digitalWrite(RELAY_2, LOW); digitalWrite(relay_3, LOW);
    
    connectWiFi(); 
    
    if (WiFi.status() == WL_CONNECTED) {
        lastAction = "Device Online";
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        struct tm timeinfo;
        if(getLocalTime(&timeinfo, 5000) && timeinfo.tm_year > (2023 - 1900)) {
             timeSynced = true;
             Serial.println("Time Synced!");
             preferences.begin("motor_state", true);
             String lastKnownStatus = preferences.getString("last_stat", "OFF");
             time_t savedOnTime = preferences.getULong64("on_time", 0);
             preferences.end();
             if(lastKnownStatus == "ON" && savedOnTime > 0) {
                Serial.println("Recovery log needed.");
                time_t bootTime; time(&bootTime);
                addMotorLog(savedOnTime, bootTime);
                preferences.begin("motor_state", false);
                preferences.putString("last_stat", "OFF");
                preferences.putULong64("on_time", 0);
                preferences.end();
             }
        } else { Serial.println("Time sync failed."); }
    }
    
    lastMotorStatus = (digitalRead(SWITCH_1) == LOW) ? "ON" : "OFF";
    
    esp_task_wdt_config_t wdt_config = { .timeout_ms = WDT_TIMEOUT * 1000, .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, .trigger_panic = true };
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL); 
    
    webSocket.beginSSL(websocket_server_host.c_str(), websocket_server_port, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    
    Serial.println("Setup complete. Entering main loop.");
}

// --- MAIN LOOP ---
void loop() {
    esp_task_wdt_reset(); 
    
    if (WiFi.status() == WL_CONNECTED) {
        server.handleClient();
        webSocket.loop();
    } else {
        if (millis() - lastWifiCheckTime > 30000) {
            Serial.println("WiFi disconnected. Attempting to reconnect...");
            WiFi.reconnect();
            lastWifiCheckTime = millis();
        }
        return;
    }

    if (relay1_timer > 0 && millis() - relay1_timer >= relay_duration) { digitalWrite(RELAY_1, LOW); relay1_timer = 0; }
    if (relay2_timer > 0 && millis() - relay2_timer >= relay_duration) { digitalWrite(RELAY_2, LOW); relay2_timer = 0; }
    if (relay3_timer > 0 && millis() - relay3_timer >= relay_duration) { digitalWrite(relay_3, LOW); relay3_timer = 0; }

    String currentMotorStatus = (digitalRead(SWITCH_1) == LOW) ? "ON" : "OFF";
    if (currentMotorStatus != lastMotorStatus) {
        if (timeSynced) {
            preferences.begin("motor_state", false);
            if (currentMotorStatus == "ON") {
                time_t onTimestamp; time(&onTimestamp);
                preferences.putULong64("on_time", onTimestamp);
            } else {
                time_t savedOnTime = preferences.getULong64("on_time", 0);
                time_t offTimestamp; time(&offTimestamp);
                addMotorLog(savedOnTime, offTimestamp);
                preferences.putULong64("on_time", 0);
            }
            preferences.putString("last_stat", currentMotorStatus);
            preferences.end();
        }
        lastMotorStatus = currentMotorStatus;
        sendStatusUpdate(); 
    }

    if (millis() - lastStatusUpdateTime > 5000) { 
        sendStatusUpdate();
        lastStatusUpdateTime = millis();
    }

    if (millis() - lastKeepAliveTime > 180000) {
        if (webSocket.isConnected()) { webSocket.sendTXT("{\"type\":\"ping\"}"); }
        lastKeepAliveTime = millis();
    }
}

// --- HELPER FUNCTIONS ---

void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    Serial.print("Attempting to connect to saved WiFi");
    
    int timeout = 20; // 20 seconds
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
        Serial.print(".");
        delay(1000);
        timeout--;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nFailed to connect. Starting Configuration Portal.");
        WiFiManager wm;
        wm.setConfigPortalTimeout(180); // 3 minutes
        
        if (wm.startConfigPortal("ESP32-Setup")) {
            Serial.println("WiFi configured successfully via portal!");
        } else {
            Serial.println("Config Portal timed out. Will retry connection in background.");
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected!");
        Serial.print("IP Address: "); Serial.println(WiFi.localIP());
        server.on("/", HTTP_GET, handleRoot);
        server.on("/save", HTTP_POST, handleSave);
        server.on("/resetwifi", HTTP_POST, handleResetWifi);
        server.begin();
        Serial.println("Local Admin Panel is running.");
    } else {
        Serial.println("Could not connect to WiFi. Local Admin Panel disabled.");
    }
}


void sendStatusUpdate() {
    Motor_Stat = (digitalRead(SWITCH_1) == LOW) ? "ON" : "OFF";
    System_Mode = (digitalRead(SWITCH_2) == LOW) ? "Normal" : "Emergency";
    wifiSignal = constrain(map(WiFi.RSSI(), -100, -30, 0, 100), 0, 100);

    JsonDocument doc;
    doc["type"] = "statusUpdate";
    
    JsonObject payload = doc.createNestedObject("payload");
    payload["motorStatus"] = Motor_Stat;
    payload["systemMode"] = System_Mode;
    payload["lastAction"] = lastAction;
    payload["wifiSignal"] = wifiSignal;
    payload["localIP"] = WiFi.localIP().toString();
    payload["wsHost"] = websocket_server_host;
    
    String jsonString;
    serializeJson(doc, jsonString);
    if(webSocket.isConnected()) { webSocket.sendTXT(jsonString); }
}

void sendLogPage(int page) {
    StaticJsonDocument<4096> doc;
    doc["type"] = "logPageUpdate";
    
    JsonObject payload = doc.createNestedObject("payload");
    JsonArray motorLogs = payload.createNestedArray("motorLogs");
    
    preferences.begin("motor_logs", true);
    uint8_t logCount = preferences.getUChar("log_count", 0);
    uint8_t logIndex = preferences.getUChar("log_idx", 0);
    
    int logsPerPage = 20;
    int totalPages = (logCount + logsPerPage - 1) / logsPerPage;
    if (totalPages == 0) totalPages = 1;
    
    payload["currentPage"] = page;
    payload["totalPages"] = totalPages;
    
    int startIndex = page * logsPerPage;
    int endIndex = min((startIndex + logsPerPage), (int)logCount);

    for (int i = startIndex; i < endIndex; i++) {
        uint8_t currentLogPos = (logIndex - 1 - i + 100) % 100;
        String logKey = "log_" + String(currentLogPos);
        String logEntryJson = preferences.getString(logKey.c_str(), "");
        if (logEntryJson != "") { motorLogs.add(logEntryJson); }
    }
    preferences.end();
    
    String jsonString;
    serializeJson(doc, jsonString);
    if(webSocket.isConnected()) { 
        webSocket.sendTXT(jsonString);
        Serial.printf("Sent log page %d.\n", page);
    }
}


void addMotorLog(time_t onTimestamp, time_t offTimestamp) {
    if (!timeSynced || onTimestamp == 0 || onTimestamp > offTimestamp) { return; }
    
    unsigned long duration = offTimestamp - onTimestamp;

    char onTimeStr[20], offTimeStr[20];
    strftime(onTimeStr, sizeof(onTimeStr), "%d/%m %I:%M%p", localtime(&onTimestamp));
    strftime(offTimeStr, sizeof(offTimeStr), "%d/%m %I:%M%p", localtime(&offTimestamp));

    preferences.begin("motor_logs", false);
    uint8_t logCount = preferences.getUChar("log_count", 0);
    uint8_t logIndex = preferences.getUChar("log_idx", 0);

    JsonDocument logEntry;
    logEntry["onTime"] = String(onTimeStr);
    logEntry["offTime"] = String(offTimeStr);
    logEntry["duration"] = formatDuration(duration);

    String logJsonString;
    serializeJson(logEntry, logJsonString);
    
    String logKey = "log_" + String(logIndex);
    preferences.putString(logKey.c_str(), logJsonString);

    logIndex = (logIndex + 1) % 100;
    if (logCount < 100) { logCount++; }

    preferences.putUChar("log_idx", logIndex);
    preferences.putUChar("log_count", logCount);
    preferences.end();

    Serial.println("New motor cycle log added: " + logJsonString);
}

String formatDuration(unsigned long totalSeconds) {
    if (totalSeconds < 60) return String(totalSeconds) + "s";
    
    unsigned long hours = totalSeconds / 3600;
    totalSeconds %= 3600;
    unsigned long minutes = totalSeconds / 60;
    unsigned long seconds = totalSeconds % 60;
    
    String durationString = "";
    if (hours > 0) durationString += String(hours) + "h ";
    if (minutes > 0 || hours > 0) durationString += String(minutes) + "m ";
    durationString += String(seconds) + "s";
    return durationString;
}

