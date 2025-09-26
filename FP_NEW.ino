#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// --- Servo Motor Setup ---
#define SERVO_PIN 13
Servo myServo;

// --- OLED Display Setup ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- WiFi Credentials (Fallback) ---
const char* ssid = "CANDOR";
const char* password = "a1b2c3d4e5";

// --- Server and WebSocket ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// --- Fingerprint Sensor Setup ---
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);
bool sensorConnected = false;

// --- State Machine and Timers ---
enum EnrollmentState { IDLE, WAIT_FOR_FIRST_PRESS, WAIT_FOR_FIRST_LIFT, WAIT_FOR_SECOND_PRESS, ENROLL_FAILED };
EnrollmentState enrollState = IDLE;
int enrollId = 0;
String enrollName = "";
uint32_t lastClient = 0;
unsigned long unlockUntilTime = 0;
unsigned long lastScanTime = 0;
const unsigned long scanInterval = 200;

// --- NEW: WiFi Scan Globals ---
bool scanInProgress = false;
uint32_t wsClientIdForScan = 0;


// --- Function Prototypes ---
void displayMessage(const String& message);
void handleEnrollment();
void handleFingerScan();
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
bool isIdTaken(int id);
void setDefaultLed();
void loadWifiCredentials();
void saveWifiCredentials(const char* ssid, const char* password);
void checkWifiScan();

// --- Helper function to set the LED to its default "off" state
void setDefaultLed() {
  if (sensorConnected) {
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0);
  }
}

// --- Helper function to get name from ID ---
String getNameFromMap(int id) {
  if (SPIFFS.exists("/fp_map.json")) {
    File file = SPIFFS.open("/fp_map.json", "r");
    if (file) {
      JsonDocument doc;
      deserializeJson(doc, file);
      file.close();
      String idKey = String(id);
      if (!doc[idKey].isNull()) {
        return doc[idKey].as<String>();
      }
    }
  }
  return "Unknown";
}

void saveNameToMap(int id, String name) {
  JsonDocument doc;
  if (SPIFFS.exists("/fp_map.json")) {
    File readFile = SPIFFS.open("/fp_map.json", "r");
    if (readFile) {
      deserializeJson(doc, readFile);
      readFile.close();
    }
  }
  doc[String(id)] = name;
  File writeFile = SPIFFS.open("/fp_map.json", "w");
  serializeJson(doc, writeFile);
  writeFile.close();
  Serial.println("Updated fingerprint name map.");
}

// --- Helper function to remove a name from the map ---
void removeNameFromMap(int id) {
    JsonDocument doc;
    if (SPIFFS.exists("/fp_map.json")) {
        File readFile = SPIFFS.open("/fp_map.json", "r");
        if (readFile) {
            deserializeJson(doc, readFile);
            readFile.close();
            doc.remove(String(id));
            File writeFile = SPIFFS.open("/fp_map.json", "w");
            serializeJson(doc, writeFile);
            writeFile.close();
            Serial.println("Removed entry from fingerprint map.");
        }
    }
}

void notifyWebClient(const char* status, const char* message) {
    if (lastClient == 0) return;
    JsonDocument doc;
    doc["type"] = "enroll_status";
    doc["status"] = status;
    doc["message"] = message;
    String jsonString;
    serializeJson(doc, jsonString);
    ws.text(lastClient, jsonString);
}

void updateStatus(const char* status, const char* message) {
    displayMessage(message);
    notifyWebClient(status, message);
}

// --- Save WiFi credentials to SPIFFS ---
void saveWifiCredentials(const char* newSsid, const char* newPassword) {
    JsonDocument doc;
    doc["ssid"] = newSsid;
    doc["password"] = newPassword;
    
    File file = SPIFFS.open("/wifi.json", "w");
    if (!file) {
        Serial.println("Failed to open wifi.json for writing");
        return;
    }
    serializeJson(doc, file);
    file.close();
    Serial.println("WiFi credentials saved.");
}

// --- Load WiFi credentials from SPIFFS ---
void loadWifiCredentials() {
    if (SPIFFS.exists("/wifi.json")) {
        File file = SPIFFS.open("/wifi.json", "r");
        if (file) {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, file);
            if (!error) {
                ssid = strdup(doc["ssid"]);
                password = strdup(doc["password"]);
                Serial.println("Loaded WiFi credentials from SPIFFS.");
            } else {
                Serial.println("Failed to parse wifi.json.");
            }
            file.close();
        }
    } else {
        Serial.println("wifi.json not found, using default credentials.");
    }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting...");
  myServo.attach(SERVO_PIN);
  myServo.write(0);

  Wire.begin(OLED_SDA, OLED_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }

  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  Serial2.begin(57600);
  finger.begin(57600);
  if (finger.verifyPassword()) {
    Serial.println("Found fingerprint sensor!");
    sensorConnected = true;
    setDefaultLed();
  } else {
    Serial.println("Did not find fingerprint sensor :(");
    sensorConnected = false;
  }

  displayMessage("Connecting to\nWiFi...");
  loadWifiCredentials(); // Load credentials before connecting
  WiFi.mode(WIFI_STA); // Set WiFi mode to Station
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
  Serial.print("Web Server is running at: http://");
  Serial.println(WiFi.localIP());

  // Show WiFi Name on OLED
  String wifiMessage = "WiFi:\n" + WiFi.SSID();
  displayMessage(wifiMessage);
  delay(2000);

  // Show IP Address on OLED
  String ipMessage = "IP Address:\n" + WiFi.localIP().toString();
  displayMessage(ipMessage);
  delay(2000);

  displayMessage("Ready & Locked");

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/UI.html", "text/html");
  });
  server.serveStatic("/", SPIFFS, "/");

  server.begin();
}

void loop() {
  ws.cleanupClients();
  checkWifiScan(); // Check the status of an ongoing async WiFi scan
  
  if (unlockUntilTime > 0 && millis() >= unlockUntilTime) 
  {
    myServo.write(0);
    displayMessage("Ready & Locked");
    unlockUntilTime = 0;
  }
  
  if (enrollState != IDLE) {
    handleEnrollment();
  } 
  else if (millis() - lastScanTime >= scanInterval) {
    lastScanTime = millis();
    handleFingerScan();
  }
}

// --- NEW FUNCTION: Handles async WiFi scan results ---
void checkWifiScan() {
    if (scanInProgress) {
        int8_t n = WiFi.scanComplete();
        if (n >= 0) { // Scan is done (0 or more networks found)
            Serial.printf("Scan complete. Found %d networks.\n", n);
            scanInProgress = false;

            JsonDocument wifiDoc;
            wifiDoc["type"] = "wifi_list";
            JsonArray networks = wifiDoc["networks"].to<JsonArray>();

            if (n > 0) {
                // To prevent memory issues, only send the top 15 networks.
                const int MAX_NETWORKS_TO_SEND = 15;
                int networks_to_add = (n > MAX_NETWORKS_TO_SEND) ? MAX_NETWORKS_TO_SEND : n;
                Serial.printf("Processing and sending top %d networks.\n", networks_to_add);

                for (int i = 0; i < networks_to_add; ++i) {
                    JsonObject net = networks.add<JsonObject>();
                    net["ssid"] = WiFi.SSID(i);
                    net["rssi"] = WiFi.RSSI(i);
                    net["encryption"] = WiFi.encryptionType(i);
                }
            }
            
            String jsonString;
            size_t jsonSize = serializeJson(wifiDoc, jsonString);

            if (jsonSize == 0) {
                Serial.println("Error: Failed to serialize WiFi list JSON, likely due to low memory.");
                JsonDocument errorDoc;
                errorDoc["type"] = "wifi_scan_failed";
                errorDoc["message"] = "Failed to create network list (memory error).";
                String errorString;
                serializeJson(errorDoc, errorString);
                AsyncWebSocketClient * client = ws.client(wsClientIdForScan);
                if (client && client->status() == WS_CONNECTED) {
                    client->text(errorString);
                }
                WiFi.scanDelete();
                wsClientIdForScan = 0;
                return; 
            }
            
            Serial.printf("Serialized WiFi list is %d bytes.\n", jsonSize);

            AsyncWebSocketClient * client = ws.client(wsClientIdForScan);
            if (client && client->status() == WS_CONNECTED) {
                client->text(jsonString);
                Serial.println("WiFi list sent to client.");
            } else {
                Serial.println("Client who requested scan is no longer connected.");
            }
            
            WiFi.scanDelete(); // Clear the found networks from memory
            wsClientIdForScan = 0;
        } else if (n == WIFI_SCAN_FAILED) {
            Serial.println("WiFi scan failed.");
            scanInProgress = false;

            JsonDocument errorDoc;
            errorDoc["type"] = "wifi_scan_failed";
            errorDoc["message"] = "Device failed to scan for networks.";
            String jsonString;
            serializeJson(errorDoc, jsonString);

            AsyncWebSocketClient * client = ws.client(wsClientIdForScan);
            if (client && client->status() == WS_CONNECTED) {
                client->text(jsonString);
            }
            wsClientIdForScan = 0;
        }
        // If n is WIFI_SCAN_RUNNING (-2), we do nothing and wait for the next loop iteration.
    }
}

void handleFingerScan() {
  if (!sensorConnected) return;

  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    return;
  }
  
  finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_BLUE);

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    displayMessage("Image Error");
    delay(1000);
    displayMessage("Ready & Locked");
    setDefaultLed();
    return;
  }

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    String userName = getNameFromMap(finger.fingerID);
    String accessMessage = userName + "\nID: " + String(finger.fingerID);
    displayMessage(accessMessage);

    JsonDocument doc;
    doc["type"] = "access_event";
    doc["id"] = finger.fingerID;
    doc["name"] = userName;
    String jsonString;
    serializeJson(doc, jsonString);
    ws.textAll(jsonString);
    
    myServo.write(90);
    delay(1000);
    
    myServo.write(0);
    displayMessage("Ready & Locked");
    setDefaultLed();
  } 
  else if (p == FINGERPRINT_NOTFOUND) 
  {
    finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_RED, 3);
    
    displayMessage("Access Denied");
    JsonDocument doc;
    doc["type"] = "access_denied";
    doc["message"] = "Unknown finger detected";
    String jsonString;
    serializeJson(doc, jsonString);
    ws.textAll(jsonString);
    delay(1500);
    
    displayMessage("Ready & Locked");
    setDefaultLed();
  }
}

void displayMessage(const String& message) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(message);
    display.display();
}

bool isIdTaken(int id) {
  if (SPIFFS.exists("/fp_map.json")) {
    File file = SPIFFS.open("/fp_map.json", "r");
    if (file) {
      JsonDocument doc;
      deserializeJson(doc, file);
      file.close();
      return !doc[String(id)].isNull();
    }
  }
  return false;
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("Client #%u connected\n", client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("Client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    JsonDocument doc;
    deserializeJson(doc, (char*)data, len);
    String command = doc["command"];
    if (command == "enroll") {
      if (enrollState != IDLE) {
        notifyWebClient("error", "Enrollment busy.");
        return;
      }
      enrollId = doc["id"];
      if (isIdTaken(enrollId)) {
        notifyWebClient("error", "This ID is already registered.");
        return;
      }
      enrollName = doc["name"].as<String>();
      lastClient = client->id();
      finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_PURPLE);
      updateStatus("info", "place the finger");
      delay(500);
      enrollState = WAIT_FOR_FIRST_PRESS;
    } 
    else if (command == "delete") {
      int idToDelete = doc["id"];
      uint8_t p = finger.deleteModel(idToDelete);
      JsonDocument responseDoc;
      responseDoc["type"] = "delete_status";
      responseDoc["id"] = idToDelete;
      if (p == FINGERPRINT_OK) {
        responseDoc["status"] = "success";
        removeNameFromMap(idToDelete);
      } else {
        responseDoc["status"] = "error";
      }
      String jsonString;
      serializeJson(responseDoc, jsonString);
      ws.text(client->id(), jsonString);
    }
    else if (command == "delete_all") {
      Serial.println("Received command to delete all fingerprints.");
      uint8_t p = finger.emptyDatabase();
      
      JsonDocument responseDoc;
      responseDoc["type"] = "delete_all_status";
      if (p == FINGERPRINT_OK) {
        Serial.println("Fingerprint database cleared successfully.");
        SPIFFS.remove("/fp_map.json");
        responseDoc["status"] = "success";
        responseDoc["message"] = "All fingerprints have been deleted.";
      } else {
        Serial.println("Failed to clear fingerprint database.");
        responseDoc["status"] = "error";
        responseDoc["message"] = "Could not clear sensor memory.";
      }
      String jsonString;
      serializeJson(responseDoc, jsonString);
      ws.text(client->id(), jsonString);
    }
    else if (command == "unlock") {
      myServo.write(90);
      displayMessage("Unlocked via Web\nFor 10 seconds");
      unlockUntilTime = millis() + 10000;
    }
    else if (command == "lock") {
      myServo.write(0);
      displayMessage("Locked via Web");
      unlockUntilTime = 0;
    }
    else if (command == "get_status") {
      JsonDocument statusDoc;
      statusDoc["type"] = "status_update";
      statusDoc["sensorConnected"] = sensorConnected;
      statusDoc["currentSSID"] = WiFi.SSID();
      
      int count = 0;
      if (SPIFFS.exists("/fp_map.json")) {
        File file = SPIFFS.open("/fp_map.json", "r");
        if (file) {
          JsonDocument fpDoc;
          deserializeJson(fpDoc, file);
          file.close();
          count = fpDoc.as<JsonObject>().size();
        }
      }
      statusDoc["fingerprintCount"] = count;
      
      String jsonString;
      serializeJson(statusDoc, jsonString);
      ws.text(client->id(), jsonString);
    }
    else if (command == "get_fingerprints") {
      JsonDocument responseDoc;
      responseDoc["type"] = "fingerprint_list";
      JsonArray fingerprints = responseDoc["fingerprints"].to<JsonArray>();

      if (SPIFFS.exists("/fp_map.json")) {
        File file = SPIFFS.open("/fp_map.json", "r");
        if (file) {
          JsonDocument storedFps;
          deserializeJson(storedFps, file);
          file.close();
          for (JsonPair kv : storedFps.as<JsonObject>()) {
            JsonObject fp = fingerprints.add<JsonObject>();
            fp["id"] = kv.key().c_str();
            fp["name"] = kv.value().as<String>();
          }
        }
      }
      String jsonString;
      serializeJson(responseDoc, jsonString);
      ws.text(client->id(), jsonString);
    }
    else if (command == "scan_wifi") {
        Serial.println("WiFi scan command received.");
        if (scanInProgress) {
            Serial.println("Scan already in progress.");
            return;
        }
        scanInProgress = true;
        wsClientIdForScan = client->id();
        WiFi.scanNetworks(true, false); // Start async scan
        Serial.println("Async WiFi scan initiated.");
    }
    else if (command == "connect_wifi") {
        String newSsid = doc["ssid"];
        String newPassword = doc["password"];
        
        Serial.printf("Received request to connect to SSID: %s\n", newSsid.c_str());

        WiFi.disconnect();
        WiFi.begin(newSsid.c_str(), newPassword.c_str());

        int timeout = 0;
        while (WiFi.status() != WL_CONNECTED && timeout < 20) { // 10 second timeout
            delay(500);
            Serial.print(".");
            timeout++;
        }

        JsonDocument responseDoc;
        responseDoc["type"] = "wifi_status";
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nConnection successful!");
            responseDoc["status"] = "success";
            responseDoc["message"] = "Successfully connected to " + newSsid;
            saveWifiCredentials(newSsid.c_str(), newPassword.c_str());
            delay(1000); 
            ESP.restart();
        } else {
            Serial.println("\nConnection failed!");
            responseDoc["status"] = "error";
            responseDoc["message"] = "Failed to connect to " + newSsid;
            WiFi.begin(ssid, password);
        }
        String jsonString;
        serializeJson(responseDoc, jsonString);
        ws.text(client->id(), jsonString);
    }
  }
}

void handleEnrollment() {
  if (enrollState == IDLE) return;
  uint8_t p;
  switch (enrollState) {
    case WAIT_FOR_FIRST_PRESS:
      p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(1);
        if (p == FINGERPRINT_OK) {
          updateStatus("info", "remove finger");
          enrollState = WAIT_FOR_FIRST_LIFT;
        } else {
          updateStatus("error", "Image conversion failed");
          enrollState = ENROLL_FAILED;
        }
      } else if (p != FINGERPRINT_NOFINGER) {
        updateStatus("error", "Sensor error");
        enrollState = ENROLL_FAILED;
      }
      break;
    case WAIT_FOR_FIRST_LIFT:
      p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        updateStatus("info", "place same finger again");
        enrollState = WAIT_FOR_SECOND_PRESS;
      }
      break;
    case WAIT_FOR_SECOND_PRESS:
      p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(2);
        if (p == FINGERPRINT_OK) {
          p = finger.createModel();
          if (p == FINGERPRINT_OK) {
            p = finger.storeModel(enrollId);
            if (p == FINGERPRINT_OK) {
              finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 100, FINGERPRINT_LED_BLUE, 4);
              saveNameToMap(enrollId, enrollName);
              JsonDocument doc;
              doc["type"] = "enroll_success";
              doc["id"] = enrollId;
              doc["name"] = enrollName;
              String jsonString;
              serializeJson(doc, jsonString);
              ws.text(lastClient, jsonString);
              updateStatus("success", "successfully added finger");
              delay(1500);
              displayMessage("Ready & Locked");
              enrollState = IDLE;
            } else { updateStatus("error", "Failed to store model");
            enrollState = ENROLL_FAILED; }
          } else { updateStatus("error", "Fingerprints did not match");
          enrollState = ENROLL_FAILED; }
        } else { updateStatus("error", "Image conversion failed");
        enrollState = ENROLL_FAILED; }
      }
      break;
    case ENROLL_FAILED:
      finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 50, FINGERPRINT_LED_RED, 3);
      delay(2000);
      enrollState = IDLE;
      displayMessage("Ready & Locked");
      break;
  }
  
  if (enrollState == IDLE) {
    setDefaultLed();
  }
}
