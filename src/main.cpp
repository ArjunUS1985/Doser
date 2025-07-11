#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
//#include <Ticker.h>
#include <NTPClient.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <ESP8266mDNS.h> // Include mDNS library
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <EEPROM.h>
#define SPIFFS LittleFS // Replace SPIFFS with LittleFS for compatibility

// Telnet server globals
//WiFiServer telnetServer(23);
WiFiClient telnetClient;

void telnetPrint(const String& msg) {
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(msg);
  }
}

#define HW_VERSION_ADDR 0
#define HW_VERSION_DEFAULT 0.0f
#define CHANNELS_ADDR 4
#define CHANNELS_DEFAULT 1

float readHWVersion() {
  float val = 0.0f;
  EEPROM.begin(16);
  EEPROM.get(HW_VERSION_ADDR, val);
  if (isnan(val) || val <= 0.0f || val > 100.0f) val = HW_VERSION_DEFAULT;
  return val;
}
void writeHWVersion(float version) {
  EEPROM.begin(16);
  EEPROM.put(HW_VERSION_ADDR, version);
  EEPROM.commit();
}

int readChannels() {
  int val = 0;
  EEPROM.begin(16);
  EEPROM.get(CHANNELS_ADDR, val);
  if (isnan(val) || val < 1 || val > 4) val = CHANNELS_DEFAULT;
  return val;
}
void writeChannels(int channels) {
  EEPROM.begin(16);
  EEPROM.put(CHANNELS_ADDR, channels);
  EEPROM.commit();
}

// Global variables that getFormattedTime needs
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);  // 19800 seconds = 5.5 hours for IST
const unsigned long jan1_2025_epoch = 1735689600; // Epoch time for Jan 1, 2025
// Time formatting function
String getFormattedTime() {
  timeClient.update();
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = localtime((time_t *)&epochTime); // Use localtime for timezone
  static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  int hour = ptm->tm_hour;
  int minute = ptm->tm_min;
  int day = ptm->tm_mday;
  int month = ptm->tm_mon;
  int year = ptm->tm_year + 1900;
  String ampm = (hour < 12) ? "AM" : "PM";
  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;
  char timeString[32];
  snprintf(timeString, sizeof(timeString), "%02d-%s-%04d %02d:%02d %s", day, months[month], year, hour12, minute, ampm.c_str());
  return String(timeString);
}

// Helper function to check if a timestamp is from today
bool isToday(unsigned long timestamp) {
  if (timestamp == 0) return false; // Never dosed
  timeClient.update();
  time_t now = timeClient.getEpochTime();
  //telnetPrint("[isToday] now epoch: " + String((long)now) + "\r\n");
  struct tm nowTmCopy;
  struct tm *nowTm = gmtime(&now);
  if (nowTm) nowTmCopy = *nowTm;
  time_t timestampTime = timestamp;
  struct tm timestampTmCopy;
  struct tm *timestampTm = gmtime(&timestampTime);
  if (timestampTm) timestampTmCopy = *timestampTm;
  // telnetPrint("[isToday] nowTm: year=" + String(nowTmCopy.tm_year+1900) + ", mon=" + String(nowTmCopy.tm_mon+1) + ", mday=" + String(nowTmCopy.tm_mday) + "\r\n");
  // telnetPrint("[isToday] timestamp: " + String((long)timestamp) + "\r\n");
  //telnetPrint("[isToday] timestampTm: year=" + String(timestampTmCopy.tm_year+1900) + ", mon=" + String(timestampTmCopy.tm_mon+1) + ", mday=" + String(timestampTmCopy.tm_mday) + "\r\n");
  bool sameDay = (nowTmCopy.tm_year == timestampTmCopy.tm_year && 
          nowTmCopy.tm_mon == timestampTmCopy.tm_mon && 
          nowTmCopy.tm_mday == timestampTmCopy.tm_mday);
  //telnetPrint("[isToday] sameDay: " + String(sameDay ? "true" : "false") + "\r\n");
  return sameDay;
}

// Pin Definitions
#define MOTOR1_PIN D1
#define MOTOR2_PIN D5
#define LED_PIN D2
#define CALIBRATE_BUTTON1_PIN D3
#define CALIBRATE_BUTTON2_PIN D4
#define WIFI_RESET_BUTTON_PIN D6
#define SYSTEM_RESET_BUTTON_PIN D7
#define NUM_LEDS 1

// RGB LED States
#define LED_RED 0xFF0000
#define LED_GREEN 0x00FF00
#define LED_BLUE 0x0000FF
#define LED_YELLOW 0xFFFF00
#define LED_PURPLE 0xFF00FF  // Adding purple color (mix of red and blue)

// Initialize NeoPixel strip
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
bool timeSynced = false;
// LED state management
enum LEDState {
  LED_OFF,
  LED_BLINK_GREEN,
  LED_BLINK_RED,
  LED_BLINK_BLUE,
  LED_BLINK_YELLOW
};

// Add global variables for LED brightness and blink status
uint8_t ledBrightness = 128; // Default 50% (0-255)
bool blinkAllOk = true; // Default Yes
bool resetButtonPressed = false; // Default No
// Function prototypes
void updateLEDState();
void setLEDState(LEDState state);
void updateLED(uint32_t color);
void handlePrimePump();
// Function declarations for header and footer generators
String generateHeader(const String& title);
String generateFooter();
// --- Weekly Schedule Data Structure ---
struct DaySchedule {
  bool enabled;
  int hour;
  int minute;
  float volume;
};

struct WeeklySchedule {
  String channelName;
  DaySchedule days[7]; // 0=Monday, 6=Sunday
  bool missedDoseCompensation;
};

// Global WeeklySchedule variables
WeeklySchedule weeklySchedule1;
WeeklySchedule weeklySchedule2;
// Function prototypes for helpers used before definition
int calculateDaysRemaining(float remainingML, WeeklySchedule* ws);
void sendNtfyNotification(const String& title, const String& message);

// Add a forward declaration for updateDaysRemaining
template<typename T>
void updateDaysRemaining(int channel, float remainingML, T* ws);

// Global Variables for LED
LEDState currentLEDState = LED_OFF;
unsigned long lastLEDUpdate = 0;
int blinkCount = 0;

void updateLED(uint32_t color) {
  strip.setBrightness(ledBrightness);
  strip.setPixelColor(0, color);
  strip.show();
}

// Implementation of LED functions
void setLEDState(LEDState state) {
  currentLEDState = state;
  lastLEDUpdate = millis();
  blinkCount = 0;
}

// Global Variables
ESP8266WebServer server(80);

// Channel names
String channel1Name = "Channel 1";
String channel2Name = "Channel 2";

// System Settings Variables

String deviceName; // Default device name with last 2 chars of MAC

// Calibration Variables
float calibrationFactor1 = 1;
float calibrationFactor2 = 1;

// Persistent Storage Variables
float remainingMLChannel1 = 0.0;
float remainingMLChannel2 = 0.0;

// Add timezone offset to global variables
int32_t timezoneOffset = 19800;  // Default to IST (UTC+5:30)

// Add last dispensed volume and time string for each channel
float lastDispensedVolume1 = 0.0;
float lastDispensedVolume2 = 0.0;
String lastDispensedTime1 = "N/A";
String lastDispensedTime2 = "N/A";

// Prime pump state variables
bool isPrimingChannel1 = false;
bool isPrimingChannel2 = false;

// Notification settings
bool notifyLowFert = true;
bool notifyStart = false;
bool notifyDose = false;

// Calibration status variables
bool calibratedChannel1 = false;
bool calibratedChannel2 = false;

// Add this global variable
String lastNotifiedIP = "";

// Add last scheduled dose timestamps for each channel (epoch time)
unsigned long lastScheduledDoseTime1 = 0;
unsigned long lastScheduledDoseTime2 = 0;

// Add global variable for number of channels
int numChannels = 1; // Default 1

// Add global flags and timer for pending resets
bool pendingWiFiReset = false;
bool pendingFactoryReset = false;
unsigned long resetRequestTime = 0;
const unsigned long RESET_DELAY_MS = 3500;

// Add global variables for days remaining
int daysRemainingChannel1 = 0;
int daysRemainingChannel2 = 0;

// Add global variable for calibration time
int calibrationTimeMs = 5000; // Default to 5 seconds

// Function Prototypes
void setupWiFi();
void setupWebServer();
void handleCalibration();
void handleManualDispense();
void updateLED(uint32_t color);
//void calibrateMotor(int channel, float &calibrationFactor);
void setupTimeSync();
void checkDailyDispense();
void loadPersistentDataFromSPIFFS();
void savePersistentDataToSPIFFS();
void updateRemainingML(int channel, float dispensedML);

//void handleSystemReset();
void setupOTA();
void blinkLED(uint32_t color, int times);
void runMotor(int channel, int durationMs);
void updateLEDState();
String getFormattedTime(); 
void handleRestartOnly();
void handleWiFiReset();
void handleFactoryReset();
void handleSystemSettingsSave();
void handleFirmwareUpdate();

// --- Helper: Day names ---
const char* dayNames[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

// --- Save/Load Weekly Schedules ---
void saveWeeklySchedulesToSPIFFS() {
  File file = LittleFS.open("/weekly_schedules.json", "w");
  if (!file) {
    Serial.println(F("Failed to open weekly_schedules.json for writing"));
    return;
  }
  JsonDocument doc;
  // Channel 1
  JsonObject ch1 = doc["ch1"].to<JsonObject>();
  ch1["channelName"] = weeklySchedule1.channelName;
  ch1["missedDoseCompensation"] = weeklySchedule1.missedDoseCompensation;
  JsonArray days1 = ch1["days"].to<JsonArray>();
  for (int i = 0; i < 7; ++i) {
    JsonObject d = days1.add<JsonObject>();
    d["enabled"] = weeklySchedule1.days[i].enabled;
    d["hour"] = weeklySchedule1.days[i].hour;
    d["minute"] = weeklySchedule1.days[i].minute;
    d["volume"] = weeklySchedule1.days[i].volume;
  }
  // Channel 2
  JsonObject ch2 = doc.createNestedObject("ch2");
  ch2["channelName"] = weeklySchedule2.channelName;
  ch2["missedDoseCompensation"] = weeklySchedule2.missedDoseCompensation;
  JsonArray days2 = ch2.createNestedArray("days");
  for (int i = 0; i < 7; ++i) {
    JsonObject d = days2.createNestedObject();
    d["enabled"] = weeklySchedule2.days[i].enabled;
    d["hour"] = weeklySchedule2.days[i].hour;
    d["minute"] = weeklySchedule2.days[i].minute;
    d["volume"] = weeklySchedule2.days[i].volume;
  }
  serializeJson(doc, file);
  file.close();
}

void loadWeeklySchedulesFromSPIFFS() {
  File file = LittleFS.open("/weekly_schedules.json", "r");
  if (!file) {
    // Set defaults
    weeklySchedule1.channelName = channel1Name;
    weeklySchedule2.channelName = channel2Name;
    for (int i = 0; i < 7; ++i) {
      weeklySchedule1.days[i] = {false, 0, 0, 0.0f};
      weeklySchedule2.days[i] = {false, 0, 0, 0.0f};
    }
    weeklySchedule1.missedDoseCompensation = false;
    weeklySchedule2.missedDoseCompensation = false;
    return;
  }
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    file.close();
    // Set defaults
    weeklySchedule1.channelName = channel1Name;
    weeklySchedule2.channelName = channel2Name;
    for (int i = 0; i < 7; ++i) {
      weeklySchedule1.days[i] = {false, 0, 0, 0.0f};
      weeklySchedule2.days[i] = {false, 0, 0, 0.0f};
    }
    weeklySchedule1.missedDoseCompensation = false;
    weeklySchedule2.missedDoseCompensation = false;
    return;
  }
  // Channel 1
  JsonObject ch1 = doc["ch1"];
  weeklySchedule1.channelName = ch1["channelName"] | channel1Name;
  weeklySchedule1.missedDoseCompensation = ch1["missedDoseCompensation"] | false;
  JsonArray days1 = ch1["days"];
  for (int i = 0; i < 7; ++i) {
    if (i < days1.size()) {
      weeklySchedule1.days[i].enabled = days1[i]["enabled"] | false;
      weeklySchedule1.days[i].hour = days1[i]["hour"] | 0;
      weeklySchedule1.days[i].minute = days1[i]["minute"] | 0;
      weeklySchedule1.days[i].volume = days1[i]["volume"] | 0.0f;
    } else {
      weeklySchedule1.days[i] = {false, 0, 0, 0.0f};
    }
  }
  // Channel 2
  JsonObject ch2 = doc["ch2"];
  weeklySchedule2.channelName = ch2["channelName"] | channel2Name;
  weeklySchedule2.missedDoseCompensation = ch2["missedDoseCompensation"] | false;
  JsonArray days2 = ch2["days"];
  for (int i = 0; i < 7; ++i) {
    if (i < days2.size()) {
      weeklySchedule2.days[i].enabled = days2[i]["enabled"] | false;
      weeklySchedule2.days[i].hour = days2[i]["hour"] | 0;
      weeklySchedule2.days[i].minute = days2[i]["minute"] | 0;
      weeklySchedule2.days[i].volume = days2[i]["volume"] | 0.0f;
    } else {
      weeklySchedule2.days[i] = {false, 0, 0, 0.0f};
    }
  }
  file.close();
}

// WiFi/Time retry variables
unsigned long wifiRetryStart = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastTimeSyncRetry = 0;
bool apModeActive = false;
int wifiRetryCount = 0;
const int WIFI_RETRY_LIMIT = 30; // 30 minutes

void setupWiFiWithRetry() {
  WiFiManager wifiManager;
  wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println(F("Entered config mode"));
    Serial.println(WiFi.softAPIP());
    Serial.println(myWiFiManager->getConfigPortalSSID());
    // Set LED to purple when AP mode is entered via callback
    updateLED(LED_PURPLE);
  });
  wifiManager.setConfigPortalTimeout(300);
  wifiManager.setMinimumSignalQuality(10);
  wifiManager.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  wifiManager.setDebugOutput(true);
  wifiManager.setCaptivePortalEnable(true);
  wifiManager.setBreakAfterConfig(true);

  wifiRetryStart = millis();
  wifiRetryCount = 0;
  while (wifiRetryCount < WIFI_RETRY_LIMIT) {
    if (wifiManager.autoConnect(deviceName.c_str())) {
      Serial.println(F("Connected to WiFi."));
      Serial.print(F("IP Address: "));
      Serial.println(WiFi.localIP());
      apModeActive = false;
      return;
    } else {
      wifiRetryCount++;
      Serial.print(F("WiFi connect failed, retry "));
      Serial.println(wifiRetryCount);
      delay(60000); // 1 minute
    }
  }
  // If we reach here, go to AP mode and stay
  Serial.println(F("Failed to connect after retries, entering AP mode."));
  wifiManager.startConfigPortal(deviceName.c_str());
  apModeActive = true;
  updateLED(LED_PURPLE); // Set LED to purple when AP mode is entered after retries
}

float hwVersion = 0.0f; // Global variable for H/W version

void setup() {
 // writeHWVersion(1.0f);
 //writeChannels(2);
 numChannels = readChannels(); // Read number of channels from EEPROM
  // Check for factory reset button (D7 pulled low for 5 seconds continuously)
  pinMode(SYSTEM_RESET_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(SYSTEM_RESET_BUTTON_PIN) == LOW) {
    unsigned long resetStart = millis();
    bool stillLow = true;
    while (millis() - resetStart < 5000) {
      if (digitalRead(SYSTEM_RESET_BUTTON_PIN) != LOW) {
        stillLow = false;
        break;
      }
      delay(10); // check every 10ms
    }
    if (stillLow) {
     // LittleFS.format();
      //WiFiManager wifiManager;
      //wifiManager.resetSettings();
      //pendingFactoryReset = false;
      //ESP.restart();
      //set global boolean var reset button pressed 
      resetButtonPressed = true;
      
    }
  }
 // Initialize Serial
  Serial.begin(9600);
  // Initialize WS2812B LED
  strip.begin();
  strip.show(); // Ensure all LEDs are off initially
  String mac1 = WiFi.macAddress();
  mac1.replace(":", ""); // Update to use mac1
  deviceName = "Doser_" + mac1.substring(9, 11); 
  // Set LED to Red on Startup
  updateLED(LED_RED);
  // Initialize Pins
  pinMode(MOTOR1_PIN, OUTPUT);
  pinMode(MOTOR2_PIN, OUTPUT);
  pinMode(CALIBRATE_BUTTON1_PIN, INPUT_PULLUP);
  pinMode(CALIBRATE_BUTTON2_PIN, INPUT_PULLUP);
  pinMode(WIFI_RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SYSTEM_RESET_BUTTON_PIN, INPUT_PULLUP);

  // Ensure pumps are off on boot
  digitalWrite(MOTOR1_PIN, LOW);
  digitalWrite(MOTOR2_PIN, LOW);

  // Initialize SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println(F("Failed to mount file system"));
    return;
  }

  // Load Persistent Data from SPIFFS
  loadPersistentDataFromSPIFFS();
  loadWeeklySchedulesFromSPIFFS();
  Serial.print(F("[BOOT] lastDispensedVolume1: ")); Serial.println(lastDispensedVolume1);
  Serial.print(F("[BOOT] lastDispensedTime1: ")); Serial.println(lastDispensedTime1);
  Serial.print(F("[BOOT] lastDispensedVolume2: ")); Serial.println(lastDispensedVolume2);
  Serial.print(F("[BOOT] lastDispensedTime2: ")); Serial.println(lastDispensedTime2);

  // Update days remaining at startup for both channels
  updateDaysRemaining(1, remainingMLChannel1, &weeklySchedule1);
  updateDaysRemaining(2, remainingMLChannel2, &weeklySchedule2);

  // Setup WiFi
  setupWiFiWithRetry();

  // Setup Web Server
  setupWebServer();

  // Setup Time Sync
  setupTimeSync();

  timeClient.setTimeOffset(timezoneOffset); // Ensure NTP client uses IST by default

  // Initialize OTA
  setupOTA();

  // Initialize mDNS
  String sanitizedDeviceName = deviceName;
  sanitizedDeviceName.replace(" ", "-"); // Replace spaces with hyphens for mDNS compatibility
  if (MDNS.begin(sanitizedDeviceName.c_str())) { // Use sanitized device name
    Serial.println(F("mDNS responder started with hostname: ") + sanitizedDeviceName);
  } else {
    Serial.println(F("Error setting up mDNS responder!"));
  }

  // Set LED to Green at the end of setup
  updateLED(LED_GREEN);
//print the values in if condition wifi status trime
  

   // After WiFi connects, send System Start notification if enabled
   if (WiFi.status() == WL_CONNECTED && timeSynced && notifyStart) {
    String msg = "IP: " + WiFi.localIP().toString();
    msg += "\n";
    msg += "Device: " + deviceName + "\n";
    msg += channel1Name + ": " + String(remainingMLChannel1) + "ml, Days: " + String(calculateDaysRemaining(remainingMLChannel1, &weeklySchedule1)) + "\n";
    msg += channel2Name + ": " + String(remainingMLChannel2) + "ml, Days: " + String(calculateDaysRemaining(remainingMLChannel2, &weeklySchedule2)) + "\n";
    msg += resetButtonPressed ? "D7:Y" : "D7:N \n";
  
    Serial.println(F("Sending System Start notification: ") + msg);
    sendNtfyNotification(deviceName+" Start", msg);
  }

  // Send Welcome notification when device comes out of AP mode and connects to WiFi
  String currentIP = WiFi.localIP().toString();
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() != "" && WiFi.getMode() != WIFI_AP && lastNotifiedIP != currentIP) {
    String mDnsHost = deviceName;
    mDnsHost.replace(" ", "-");
    String welcomeMsg = F("Wifi Connection Successful. IP: ");
    welcomeMsg += currentIP;
    welcomeMsg += F("\nIf setting up for first time or resetting it's recommended to unplug the device and plug back again after 5 seconds. Post that you can manage the device at :\nhttp://");
    welcomeMsg += mDnsHost;
    welcomeMsg += F(".local/ OR http://");
    welcomeMsg += currentIP;
    welcomeMsg += F("/");
    sendNtfyNotification(F("Your Doser got a new IP"), welcomeMsg);
    lastNotifiedIP = currentIP;
    savePersistentDataToSPIFFS();
  }
  // serial print time synced notifystart and wifistatus
  Serial.println(F("[BOOT] Time Synced: ") + String(timeSynced));
  Serial.println(F("[BOOT] WiFi Status: ") + String(WiFi.status() == WL_CONNECTED ? F("Connected") : F("Disconnected")));
  Serial.println(F("[BOOT] Notification: ") + String(notifyStart ? F("Enabled") : F("Disabled")));
  //writeHWVersion(HW_VERSION_DEFAULT);
  // HW version check/init
  float hwVer = readHWVersion();
  hwVersion = hwVer; // Store in global variable for footer
 
  // Set calibration time based on HW version
  if (hwVer == 1.0f) {
    calibrationTimeMs = 5000;
  } else if (hwVer == 0.9f) {
    calibrationTimeMs = 15000;
  }

  // Start Telnet server
  //telnetServer.begin();
  //telnetServer.setNoDelay(true);
}

void loop() {
  // Handle Web Server
  server.handleClient();

  // Telnet client connection management
 // if (telnetServer.hasClient()) {
 //   if (!telnetClient || !telnetClient.connected()) {
 //     telnetClient = telnetServer.available();
 //     telnetPrint(F("Telnet connected!\r\n"));
 //   } else {
 //     // Only allow one client
 //     WiFiClient newClient = telnetServer.available();
 //     newClient.println(F("Another Telnet client is already connected."));
 //     newClient.stop();
 //   }
 // }
 // if (telnetClient && !telnetClient.connected()) {
 //   telnetClient.stop();
 // }

  // Handle OTA
  ArduinoOTA.handle();

  // Handle prime pump operations
  if (isPrimingChannel1) {
    digitalWrite(MOTOR1_PIN, HIGH);
    updateLED(LED_BLUE);
  } else if (isPrimingChannel2) {
    digitalWrite(MOTOR2_PIN, HIGH);
    updateLED(LED_YELLOW);
  } else {
    digitalWrite(MOTOR1_PIN, LOW);
    digitalWrite(MOTOR2_PIN, LOW);
  }

  // Check Buttons
  if (digitalRead(WIFI_RESET_BUTTON_PIN) == LOW) {
    // Handle WiFi Reset
    WiFi.disconnect();
    ESP.restart();
  }

  // Check Daily Dispense Schedule (only if not priming)
  static unsigned long lastDailyDispenseCheck = 0;
  if (!isPrimingChannel1 && !isPrimingChannel2) {
    if (millis() - lastDailyDispenseCheck > 30000) { // 30 seconds
      lastDailyDispenseCheck = millis();
      checkDailyDispense();
    }
  }

  // Only update LED state if not priming
  if (!isPrimingChannel1 && !isPrimingChannel2) {
    if (currentLEDState == LED_OFF) {
      if (WiFi.status() == WL_CONNECTED) {
        setLEDState(LED_BLINK_GREEN);
      } else {
        setLEDState(LED_BLINK_RED);
      }
    } else if (WiFi.status() != WL_CONNECTED && currentLEDState != LED_BLINK_RED) {
      setLEDState(LED_BLINK_RED);
    } else if (WiFi.status() == WL_CONNECTED && currentLEDState == LED_BLINK_RED) {
      setLEDState(LED_BLINK_GREEN);
    }
    // Update LED state
    updateLEDState();
  }
  
  // WiFi reconnect logic if lost after boot
  static unsigned long lastWifiCheck = 0;
  if (!apModeActive && WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiCheck > 60000) {
      lastWifiCheck = millis();
      Serial.println(F("WiFi lost, retrying connect..."));
      WiFi.reconnect();
    }
  }
  // Time sync retry logic
  static unsigned long lastTimeSyncCheck = 0;
  if (!timeSynced && WiFi.status() == WL_CONNECTED) {
    if (millis() - lastTimeSyncCheck > 60000) {
      lastTimeSyncCheck = millis();
      timeClient.update();
      if (timeClient.getEpochTime() > 100000) {
        timeSynced = true;
        Serial.println(F("Time sync successful (retry)"));
      } else {
        Serial.println(F("Time sync failed, will retry in 1 min"));
      }
    }
  }
  
  // Ensure LED stays purple in AP mode
  if (apModeActive) {
    updateLED(LED_PURPLE);
  }

  // Handle pending resets after delay
  if (pendingWiFiReset && millis() - resetRequestTime > RESET_DELAY_MS) {
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    lastNotifiedIP = "";
    savePersistentDataToSPIFFS();
    delay(1000);
    pendingWiFiReset = false;
    ESP.restart();
  }
  if (pendingFactoryReset && millis() - resetRequestTime > RESET_DELAY_MS) {
    LittleFS.format();
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    pendingFactoryReset = false;
    ESP.restart();
  }

  ArduinoOTA.handle();
}

void setupWiFi() {
  WiFiManager wifiManager;

  // Configure WiFiManager for better captive portal experience
  wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println(F("Entered config mode"));
    Serial.println(WiFi.softAPIP());
    Serial.println(myWiFiManager->getConfigPortalSSID());
  });

  // Set timeout for config portal (0 = no timeout)
  wifiManager.setConfigPortalTimeout(300); // 5 minutes timeout
  
  // Set minimum signal quality
  wifiManager.setMinimumSignalQuality(10);
  
  // Configure custom parameters for better captive portal
  wifiManager.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  
  // Set debug output
  wifiManager.setDebugOutput(true);
  
  // Configure captive portal
  wifiManager.setCaptivePortalEnable(true);
  wifiManager.setBreakAfterConfig(true);

  // Automatically start configuration portal if no WiFi is configured
  if (!wifiManager.autoConnect(deviceName.c_str())) {
    Serial.println(F("Failed to connect to WiFi and hit timeout"));
    ESP.restart();
  }

  Serial.println(F("Connected to WiFi."));
  Serial.print(F("IP Address: "));
  Serial.println(WiFi.localIP());
}

void setupWebServer() {
 

  server.on("/calibrate", HTTP_GET, []() {
    int channel = 1;
    if (server.hasArg("channel")) channel = server.arg("channel").toInt();
    String channelName = (channel == 1) ? channel1Name : channel2Name;
        
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Send HTML header
    String chunk = F("<html><head><title>Calibrate</title>");
    chunk += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    chunk += F("<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;} ");
    
    // Send CSS in smaller chunks
    chunk += F(".card{margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);} ");
    chunk += F(".card h2{margin-top:0;color:#007BFF;} ");
    chunk += F(".calib-warning{color:#b30000;background:#fff3cd;border:1px solid #ffeeba;border-radius:6px;padding:10px;margin-bottom:18px;font-size:1.05em;} ");
    chunk += F(".calib-btn{width:100%;padding:14px 0;font-size:1.1em;background:#dc3545;color:#fff;border:none;border-radius:6px;margin-bottom:10px;cursor:pointer;} ");
    chunk += F(".calib-btn:disabled{background:#aaa;cursor:not-allowed;} ");
    chunk += F(".home-btn{width:100%;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;} ");
    chunk += F(".back-btn{width:100%;padding:12px 0;font-size:1.1em;background:#aaa;color:#fff;border:none;border-radius:6px;margin-top:10px;} ");
    chunk += F("#countdown{font-size:1.2em;color:#007BFF;margin-bottom:10px;text-align:center;} ");
    chunk += F(".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } ");
    
    chunk += F(".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } ");
    chunk += F(".prime-btn.stop:hover { background-color: #218838 !important; } ");
    chunk += F(".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }");
    chunk += F("</style>");
    server.sendContent(chunk);
    // Send JavaScript
    chunk = F("<script>\n");
    chunk += F("var calibrationTime = ") + String(calibrationTimeMs / 1000) + F(";\n");
    chunk += F("function startCountdown() {\n");
    chunk += F("  var btn = document.getElementById('calibBtn');\n");
    chunk += F("  var homeBtn = document.getElementById('homeBtn');\n");
    chunk += F("  var backBtn = document.getElementById('backBtn');\n");
    chunk += F("  var countdown = document.getElementById('countdown');\n");
    chunk += F("  btn.disabled = true;\n");
    chunk += F("  homeBtn.disabled = true;\n");
    chunk += F("  backBtn.disabled = true;\n");
    chunk += F("  var timeLeft = calibrationTime;\n");
    chunk += F("  countdown.innerText = 'Calibrating... ' + timeLeft + 's remaining';\n");
    chunk += F("  var interval = setInterval(function() {\n");
    chunk += F("    timeLeft--;\n");
    chunk += F("    countdown.innerText = 'Calibrating... ' + timeLeft + 's remaining';\n");
    chunk += F("    if (timeLeft <= 0) {\n");
    chunk += F("      clearInterval(interval);\n");
    chunk += F("      countdown.innerText = '';\n");
    chunk += F("      btn.disabled = false;\n");
    chunk += F("      homeBtn.disabled = false;\n");
    chunk += F("      backBtn.disabled = false;\n");
    chunk += F("    }\n");
    chunk += F("  }, 1000);\n");
    chunk += F("}\n");
    chunk += F("function onSubmitCalib(e){\n");
    chunk += F("  startCountdown();\n");
        chunk += F("}\n");
    chunk += F("</script>");
    chunk += F("</head><body>");
    chunk += generateHeader("Calibrate: " + channelName);
    chunk += F("<div class='card'>");
    chunk += F("<div class='calib-warning'>Warning: The motor will run for ") + String(calibrationTimeMs / 1000) + F(" seconds and dispense liquid. Hold the measuring tube near the dispensing tube before proceeding.</div>");
    chunk += F("<div id='countdown'></div>");
        chunk += F("<form action='/calibrate?channel=") + String(channel) + F("' method='POST' onsubmit='onSubmitCalib(event)'>");
    chunk += F("<input type='hidden' name='channel' value='") + String(channel) + F("'>");
    chunk += F("<button type='submit' class='calib-btn' id='calibBtn'>Start Calibration</button>");
    chunk += F("</form>");
    chunk += F("<button class='home-btn' id='homeBtn' onclick=\"window.location.href='/summary'\">Home</button>");
    chunk += F("<button class='back-btn' id='backBtn' onclick=\"history.back()\">Back</button>");
    chunk += F("</div>");
    chunk += generateFooter();
    chunk += F("</body></html>");
    server.sendContent(chunk);
    
    // End the response
    server.sendContent("");
  });

  

  server.on("/reset", HTTP_GET, []() {
    String html = F("<html><head><title>System Reset</title></head><body>");
    html += F("<h1>System Reset</h1>");
    html += F("<p>Click the button below to reset the system.</p>");
    html += F("<form action='/reset' method='POST'>");
    html += F("<input type='submit' value='Reset System'>");
    html += F("</form>");
    html += F("<p><a href='/'>Back to Home</a></p>");
    html += F("</body></html>");
    server.send(200, "text/html", html);
  });

 

  server.on("/timezone", HTTP_POST, []() {
    if (server.hasArg("offset")) {
      timezoneOffset = server.arg("offset").toInt();
      timeClient.setTimeOffset(timezoneOffset);
      savePersistentDataToSPIFFS();
      server.send(200, "application/json", F("{\"status\":\"timezone updated\"}"));
    } else {
      server.send(400, "application/json", F("{\"error\":\"missing parameters\"}"));
    }
  });

  server.on("/calibrate", HTTP_POST, handleCalibration);
  server.on("/manual", HTTP_POST, handleManualDispense);
  //server.on("/daily", HTTP_POST, handleDailyDispense);
  //server.on("/bottle", HTTP_POST, handleBottleTracking);
  //server.on("/reset", HTTP_POST, handleSystemReset);
  server.on("/prime", HTTP_POST, handlePrimePump);

  server.on("/prime", HTTP_GET, []() {
    int channel = 1;
    if (server.hasArg("channel")) channel = server.arg("channel").toInt();
    String channelName = (channel == 1) ? channel1Name : channel2Name;
    
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Send HTML header
    String chunk = F("<html><head>");
    chunk += F("<title>Prime Pump</title>");
    chunk += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    chunk += F("<style>body { font-family: Arial, sans-serif; background-color: #f4f4f9; color: #333; } ");
    
    // CSS in chunks
    chunk += F(".card { margin: 20px auto; padding: 20px; max-width: 500px; background: #fff; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); } ");
    chunk += F(".card h2 { margin-top: 0; color: #007BFF; } .prime-warning { color: #b30000; background: #fff3cd; border: 1px solid #ffeeba; border-radius: 6px; padding: 10px; margin-bottom: 18px; font-size: 1.05em; } ");
    chunk += F(".prime-btn { width: 100%; padding: 14px 0; font-size: 1.1em; background: #dc3545; color: #fff; border: none; border-radius: 6px; margin-bottom: 10px; cursor: pointer; transition: background 0.2s; } ");
    chunk += F(".prime-btn.stop { background: #28a745; } ");
    chunk += F(".prime-btn:active { opacity: 0.9; } ");
    chunk += F(".home-btn { width: 100%; padding: 12px 0; font-size: 1.1em; background: #007BFF; color: #fff; border: none; border-radius: 6px; } ");
    
    chunk += F(".back-btn { width: 100%; padding: 12px 0; font-size: 1.1em; background: #aaa; color: #fff; border: none; border-radius: 6px; margin-top: 10px; } ");
    chunk += F(".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } ");
    chunk += F(".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } ");
    
    chunk += F(".prime-btn.stop:hover { background-color: #218838 !important; } ");
    chunk += F(".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }</style>");
    server.sendContent(chunk);
    // JavaScript
    chunk = F("<script>\n");
    chunk += F("function togglePrime() {\n");
    chunk += F("  var btn = document.getElementById('primeButton');\n");
    chunk += F("  var state = btn.getAttribute('data-state') === '1' ? '0' : '1';\n");
    chunk += F("  var xhr = new XMLHttpRequest();\n");
    chunk += F("  xhr.open('POST', '/prime', true);\n");
    chunk += F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n");
    chunk += F("  xhr.send('channel=") + String(channel) + F("&state=' + state);\n");
    chunk += F("  btn.setAttribute('data-state', state);\n");
    chunk += F("  btn.value = state === '1' ? 'Done' : 'Start';\n");
    chunk += F("  btn.className = state === '1' ? 'prime-btn stop' : 'prime-btn';\n");
    chunk += F("}\n");
    
    chunk += F("window.onload = function() {\n");
    chunk += F("  var btn = document.getElementById('primeButton');\n");
    chunk += F("  btn.value = 'Start';\n");
    chunk += F("  btn.className = 'prime-btn';\n");
    chunk += F("}\n");
    chunk += F("</script>");
    chunk += F("</head><body>");
    
    // Body content
    chunk += generateHeader("Prime Pump: " + channelName);
    chunk += F("<div class='card'>");
    chunk += F("<div class='prime-warning'>Warning: This action will turn on the pump and liquid will flow. Please ensure tubing is connected and ready.</div>");
    chunk += F("<input type='button' id='primeButton' data-state='0' value='Start' class='prime-btn' onclick='togglePrime()'>");
    chunk += F("<button class='home-btn' onclick=\"window.location.href='/summary'\">Home</button>");
    chunk += F("<button class='back-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#aaa;color:#fff;border:none;border-radius:6px;margin-top:10px;' onclick=\"history.back()\">Back</button>");
    chunk += F("</div>");
    chunk += generateFooter();
    chunk += F("</body></html>");
    server.sendContent(chunk);
    
    // End chunked response
    server.sendContent("");
  });

  server.on("/summary", HTTP_GET, []() {
    Serial.print(F("[SUMMARY] lastDispensedVolume1: ")); Serial.println(lastDispensedVolume1);
    Serial.print(F("[SUMMARY] lastDispensedTime1: ")); Serial.println(lastDispensedTime1);
    Serial.print(F("[SUMMARY] lastDispensedVolume2: ")); Serial.println(lastDispensedVolume2);
    Serial.print(F("[SUMMARY] lastDispensedTime2: ")); Serial.println(lastDispensedTime2);
    
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Send HTML header
    String chunk = F("<html><head><title>Doser Summary</title>");
    chunk += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    chunk += F("<style>body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f9; color: #333; } ");
    
    // Send CSS in chunks
    chunk += F(".card { margin: 20px auto; padding: 20px; max-width: 500px; background: #fff; border-radius: 10px; box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1); } ");
    chunk += F(".card h2 { margin-top: 0; color: #007BFF; } .card p { margin: 10px 0; } ");
    chunk += F(".status-chip { display: inline-block; padding: 4px 8px; border-radius: 12px; font-size: 0.5em; font-weight: bold; margin-left: 8px; } ");
    chunk += F(".chip-running-low { background: #dc3545; color: #fff; } ");
    
    chunk += F(".card button { display: block; width: 100%; margin: 10px 0; padding: 10px; font-size: 16px; color: #fff; background-color: #007BFF; ");
    chunk += F("border: none; border-radius: 5px; cursor: pointer; } .card button:hover { background-color: #0056b3; } ");
    
    chunk += F(".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } ");
    chunk += F(".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } ");
    chunk += F(".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }</style></head><body>");
    
    // Generate header
    chunk += generateHeader("Doser Summary");

    // Warning banner
   
    
    // Channel 1 Summary
    int daysRemaining1 = daysRemainingChannel1;
    bool moreThanYear1 = (daysRemaining1 >= 365);
    
    chunk += F("<div class='card'>");
    chunk += F("<h2 style='display:flex;align-items:center;gap:8px;'>") + channel1Name;
    if (!calibratedChannel1) {
      chunk += F("<span class='status-chip chip-running-low'>Not Calibrated</span>");
    }
    if (!moreThanYear1 && daysRemaining1 <= 7) {
      chunk += F("<span class='status-chip chip-running-low'>Running Low</span>");
    }
    chunk += F("</h2>");
    chunk += F("<p>Last Dosed Time: ") + lastDispensedTime1 + F("</p>");
    chunk += F("<p>Last Dispensed Volume: ") + String(lastDispensedVolume1) + F(" ml</p>");
    chunk += F("<p>Remaining Volume: ") + String(remainingMLChannel1) + F(" ml</p>");
    chunk += F("<p>Days Remaining: ");
    if (moreThanYear1) {
      chunk += F("More than a year");
    } else {
      String daysColor = (daysRemaining1 <= 7) ? F("color:#dc3545;font-weight:bold;") : F("");
      chunk += F("<span style='") + daysColor + F("'>") + String(daysRemaining1) + F("</span>");
    }
    chunk += F("</p>");
    
    chunk += F("<div id='manualDoseSection1'>");
    chunk += F("<button class='card-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#28a745;color:#fff;border:none;border-radius:6px;margin-bottom:10px;' onclick='showManualDose1()'>Manual Dose</button>");
    chunk += F("</div>");
    chunk += F("<button onclick=\"location.href='/manageChannel?channel=1'\">Manage Channel 1</button>");
    chunk += F("</div>");
    
    // Channel 2 Summary
    if (numChannels == 2) {
      int daysRemaining2 = daysRemainingChannel2;
      bool moreThanYear2 = (daysRemaining2 >= 365);
      
      chunk += F("<div class='card'>");
      chunk += F("<h2 style='display:flex;align-items:center;gap:8px;'>") + channel2Name;
      if (!calibratedChannel2) {
        chunk += F("<span class='status-chip chip-running-low'>Not Calibrated</span>");
      }
      if (!moreThanYear2 && daysRemaining2 <= 7) {
        chunk += F("<span class='status-chip chip-running-low'>Running Low</span>");
      }
      chunk += F("</h2>");
      chunk += F("<p>Last Dosed Time: ") + lastDispensedTime2 + F("</p>");
      chunk += F("<p>Last Dispensed Volume: ") + String(lastDispensedVolume2) + F(" ml</p>");
      chunk += F("<p>Remaining Volume: ") + String(remainingMLChannel2) + F(" ml</p>");
      chunk += F("<p>Days Remaining: ");
      if (moreThanYear2) {
        chunk += F("More than a year");
      } else {
        String daysColor = (daysRemaining2 <= 7) ? F("color:#dc3545;font-weight:bold;") : F("");
        chunk += F("<span style='") + daysColor + F("'>") + String(daysRemaining2) + F("</span>");
      }
      chunk += F("</p>");
      
      chunk += F("<div id='manualDoseSection2'>");
      chunk += F("<button class='card-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#28a745;color:#fff;border:none;border-radius:6px;margin-bottom:10px;' onclick='showManualDose2()'>Manual Dose</button>");
      chunk += F("</div>");
      chunk += F("<button onclick=\"location.href='/manageChannel?channel=2'\">Manage Channel 2</button>");
      chunk += F("</div>");
    }
    
    // System Time and Actions
    chunk += F("<div class='card'>");
    chunk += F("<button onclick=\"location.href='/systemSettings'\">System Settings</button>");
    chunk += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;'><span style='font-size:0.95em;color:#666;'>System Time:</span><span style='font-size:0.95em;color:#333;'>") + getFormattedTime() + F("</span></div>");
    chunk += F("</div>");
    
    // Generate footer
    chunk += generateFooter();
    server.sendContent(chunk);
    // JavaScript
    chunk = F("<script>\n");
    chunk += F("function showManualDose1() {\n");
    chunk += F("  var s = document.getElementById('manualDoseSection1');\n");
    chunk += F("  s.innerHTML = `<div style='display:flex;gap:8px;align-items:center;justify-content:center;'><input id='doseVol1' type='number' min='0.1' step='0.1' placeholder='Volume (ml)' style='width:40%;padding:8px;font-size:1em;border-radius:6px;border:1px solid #ccc;'><button id='doseBtn1' style=\"width:25%;padding:10px 0;font-size:1em;background:#007BFF;color:#fff;border:none;border-radius:6px;\" onclick='doseNow1()'>Dose</button><button id='cancelBtn1' style=\"width:25%;padding:10px 0;font-size:1em;background:#aaa;color:#fff;border:none;border-radius:6px;\" onclick='cancelManualDose1()'>Cancel</button></div><div id='doseCountdown1' style='margin-top:8px;font-size:1.1em;color:#007BFF;'></div>`;\n");
    chunk += F("}\n");
    
    chunk += F("function showManualDose2() {\n");
    chunk += F("  var s = document.getElementById('manualDoseSection2');\n");
    chunk += F("  s.innerHTML = `<div style='display:flex;gap:8px;align-items:center;justify-content:center;'><input id='doseVol2' type='number' min='0.1' step='0.1' placeholder='Volume (ml)' style='width:40%;padding:8px;font-size:1em;border-radius:6px;border:1px solid #ccc;'><button id='doseBtn2' style=\"width:25%;padding:10px 0;font-size:1em;background:#007BFF;color:#fff;border:none;border-radius:6px;\" onclick='doseNow2()'>Dose</button><button id='cancelBtn2' style=\"width:25%;padding:10px 0;font-size:1em;background:#aaa;color:#fff;border:none;border-radius:6px;\" onclick='cancelManualDose2()'>Cancel</button></div><div id='doseCountdown2' style='margin-top:8px;font-size:1.1em;color:#007BFF;'></div>`;\n");
    chunk += F("}\n");
    
    chunk += F("function cancelManualDose1() {\n");
    chunk += F("  var s = document.getElementById('manualDoseSection1');\n");
    chunk += F("  s.innerHTML = `<button class='card-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#28a745;color:#fff;border:none;border-radius:6px;margin-bottom:10px;' onclick='showManualDose1()'>Manual Dose</button>`;\n");
    chunk += F("}\n");
    chunk += F("function cancelManualDose2() {\n");
    chunk += F("  var s = document.getElementById('manualDoseSection2');\n");
    chunk += F("  s.innerHTML = `<button class='card-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#28a745;color:#fff;border:none;border-radius:6px;margin-bottom:10px;' onclick='showManualDose2()'>Manual Dose</button>`;\n");
    chunk += F("}\n");
    
    chunk += F("function doseNow1() {\n");
    chunk += F("  var vol = parseFloat(document.getElementById('doseVol1').value);\n");
    chunk += F("  if (!vol || vol <= 0) { alert('Enter a valid volume'); return; }\n");
    chunk += F("  var btn = document.getElementById('doseBtn1');\n");
    chunk += F("  var cancel = document.getElementById('cancelBtn1');\n");
    chunk += F("  btn.disabled = true; cancel.disabled = true;\n");
    chunk += F("  var countdown = document.getElementById('doseCountdown1');\n");
    chunk += F("  var duration = Math.ceil(vol * ") + String(calibrationFactor1) + F(" / 1000);\n");
    chunk += F("  countdown.innerText = 'Dosing... ' + duration + 's remaining';\n");
    
    chunk += F("  var interval = setInterval(function() {\n");
    chunk += F("    duration--;\n");
    chunk += F("    countdown.innerText = 'Dosing... ' + duration + 's remaining';\n");
    chunk += F("    if (duration <= 0) { clearInterval(interval); countdown.innerText = ''; window.location.reload(); }\n");
    chunk += F("  }, 1000);\n");
    chunk += F("  var xhr = new XMLHttpRequest();\n");
    chunk += F("  xhr.open('POST', '/manual', true);\n");
    chunk += F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n");
    chunk += F("  xhr.send('channel=1&ml=' + encodeURIComponent(vol));\n");
    chunk += F("}\n");
    server.sendContent(chunk);
    chunk = F("function doseNow2() {\n");
    chunk += F("  var vol = parseFloat(document.getElementById('doseVol2').value);\n");
    chunk += F("  if (!vol || vol <= 0) { alert('Enter a valid volume'); return; }\n");
    chunk += F("  var btn = document.getElementById('doseBtn2');\n");
    chunk += F("  var cancel = document.getElementById('cancelBtn2');\n");
    chunk += F("  btn.disabled = true; cancel.disabled = true;\n");
    chunk += F("  var countdown = document.getElementById('doseCountdown2');\n");
    chunk += F("  var duration = Math.ceil(vol * ") + String(calibrationFactor2) + F(" / 1000);\n");
    chunk += F("  countdown.innerText = 'Dosing... ' + duration + 's remaining';\n");
    
    chunk += F("  var interval = setInterval(function() {\n");
    chunk += F("    duration--;\n");
    chunk += F("    countdown.innerText = 'Dosing... ' + duration + 's remaining';\n");
    chunk += F("    if (duration <= 0) { clearInterval(interval); countdown.innerText = ''; window.location.reload(); }\n");
    chunk += F("  }, 1000);\n");
    chunk += F("  var xhr = new XMLHttpRequest();\n");
    chunk += F("  xhr.open('POST', '/manual', true);\n");
    chunk += F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n");
    chunk += F("  xhr.send('channel=2&ml=' + encodeURIComponent(vol));\n");
    chunk += F("}\n");
    chunk += F("</script>\n");
    chunk += F("</body></html>");
    server.sendContent(chunk);
    
    // End the chunked response
    server.sendContent("");
  });

  server.on("/manageChannel", HTTP_GET, []() {
    int channel = 1;
    if (server.hasArg("channel")) {
      channel = server.arg("channel").toInt();
    }
    // Select channel-specific variables
    String channelName = (channel == 1) ? channel1Name : channel2Name;
    float lastDispensedVolume = (channel == 1) ? lastDispensedVolume1 : lastDispensedVolume2;
    String lastDispensedTime = (channel == 1) ? lastDispensedTime1 : lastDispensedTime2;
    float remainingML = (channel == 1) ? remainingMLChannel1 : remainingMLChannel2;
    int daysRemaining = (channel == 1) ? daysRemainingChannel1 : daysRemainingChannel2;
    
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Send HTML header
    String chunk = F("<html><head><title>Channel Management: ") + channelName + F("</title>");
    chunk += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    chunk += F("<style>body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f9; color: #333; } ");
    
    // CSS in chunks
    chunk += F(".card { margin: 20px auto; padding: 20px; max-width: 500px; background: #fff; border-radius: 10px; box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1); } ");
    chunk += F(".card h2 { margin-top: 0; color: #007BFF; } .card p { margin: 10px 0; } ");
    
    chunk += F(".card button { display: block; width: 100%; margin: 10px 0; padding: 10px; font-size: 16px; color: #fff; background-color: #007BFF; ");
    chunk += F("border: none; border-radius: 5px; cursor: pointer; } .card button:hover { background-color: #0056b3; } ");
    
    chunk += F(".header-action { float:right; margin-top:-8px; } .rename-row { display:flex; gap:8px; } ");
    chunk += F(".rename-input { flex:1; padding:8px 12px; font-size:1em; border-radius:4px; border:1px solid #ccc; height: 2.2em; box-sizing: border-box; } ");
    
    chunk += F(".rename-btn { padding:8px 16px; font-size:1em; border-radius:4px; border:none; background:#007BFF; color:#fff; cursor:pointer; } ");
    chunk += F(".rename-btn.cancel { background:#aaa; } .rename-row { display:flex; gap:8px; align-items:center; justify-content:center; } ");
    
    chunk += F(".rename-input { flex:1; padding:8px; font-size:1em; border-radius:6px; border:1px solid #ccc; height: 2.2em; box-sizing: border-box; margin:0; } ");
    chunk += F(".rename-btn { width:25%; padding:10px 0; font-size:1em; border-radius:6px; border:none; background:#007BFF; color:#fff; cursor:pointer; margin:0; transition: background 0.2s; } ");
    chunk += F(".rename-btn.cancel { background:#aaa; transition: background 0.2s; } .rename-btn.cancel:hover { background:#888; } ");
    chunk += F("button.cancel { background:#aaa; transition: background 0.2s; } button.cancel:hover { background:#888; } ");
    
    chunk += F(".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } ");
    chunk += F(".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } ");
    chunk += F(".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }</style>");
    
    // JavaScript
    chunk += F("<script>\n");
    chunk += F("function showRenameBox() {\n");
    chunk += F("  document.getElementById('rename-row').style.display = 'flex';\n");
    chunk += F("  document.getElementById('rename-btn-row').style.display = 'none';\n");
    chunk += F("}\n");
    
    chunk += F("function cancelRename() {\n");
    chunk += F("  document.getElementById('rename-row').style.display = 'none';\n");
    chunk += F("  document.getElementById('rename-btn-row').style.display = 'block';\n");
    chunk += F("}\n");
    
chunk += F("function saveRename(channel) {\n");
    chunk += F("  var newName = document.getElementById('rename-input').value;\n");
    chunk += F("  if (!newName) { alert('Name cannot be empty'); return; }\n");
    chunk += F("  var xhr = new XMLHttpRequest();\n");
    chunk += F("  xhr.open('POST', '/renameChannel', true);\n");
    chunk += F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n");
    chunk += F("  xhr.onreadystatechange = function() {\n");
    chunk += F("    if (xhr.readyState == 4 && xhr.status == 200) { location.reload(); }\n");
    chunk += F("  };\n");
    chunk += F("  xhr.send('channel=' + channel + '&name=' + encodeURIComponent(newName));\n");
    chunk += F("}\n");
    
    chunk += F("function showUpdateVolumeBox() {\n");
    chunk += F("  document.getElementById('update-volume-row').style.display = 'flex';\n");
    chunk += F("  document.getElementById('update-volume-btn-row').style.display = 'none';\n");
    chunk += F("}\n");
    
    chunk += F("function cancelUpdateVolume() {\n");
    chunk += F("  document.getElementById('update-volume-row').style.display = 'none';\n");
    chunk += F("  document.getElementById('update-volume-btn-row').style.display = 'inline';\n");
    chunk += F("}\n");
    
    chunk += F("function saveUpdateVolume(channel) {\n");
    chunk += F("  var newVol = document.getElementById('update-volume-input').value;\n");
    chunk += F("  if (!newVol || isNaN(newVol) || Number(newVol) < 0) { alert('Enter a valid volume'); return; }\n");
    chunk += F("  var xhr = new XMLHttpRequest();\n");
    chunk += F("  xhr.open('POST', '/updateVolume', true);\n");
    chunk += F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n");
    chunk += F("  xhr.onreadystatechange = function() {\n");
    chunk += F("    if (xhr.readyState == 4 && xhr.status == 200) { location.reload(); }\n");
    chunk += F("  };\n");
    chunk += F("  xhr.send('channel=' + channel + '&volume=' + encodeURIComponent(newVol));\n");
    chunk += F("}\n");
    chunk += F("</script>\n");
    chunk += F("</head><body>");
    
    // Header and card open
    chunk += generateHeader("Channel Management: " + channelName);
    server.sendContent(chunk);
    // Calculate days remaining for this channel
    bool moreThanYear = (daysRemaining >= 365);
    
    // Status Card
    chunk = F("<div class='card'>");
    chunk += F("<h2>Status</h2>");
    chunk += F("<p>Last Dosed: ") + lastDispensedTime + F("</p>");
    chunk += F("<p>Last Dispensed Volume: ") + String(lastDispensedVolume) + F(" ml</p>");
    chunk += F("<p>Remaining Volume: <span id='remaining-volume-label'>") + String(remainingML) + F(" ml (");
    if (moreThanYear) {
      chunk += F("More than a year");
    } else {
      String daysColor = (daysRemaining <= 7) ? F("color:#dc3545;font-weight:bold;") : F("");
      chunk += F("<span style='") + daysColor + F("'>") + String(daysRemaining) + F(" days</span>");
    }
    chunk += F(")</span> ");
    
    chunk += F("<span id='update-volume-btn-row'><button style='margin-left:8px;' onclick=\"showUpdateVolumeBox()\">Update Volume</button></span>");
    chunk += F("<span id='update-volume-row' class='rename-row' style='display:none;'>");
    chunk += F("<input id='update-volume-input' class='rename-input' type='number' min='0' step='0.01' value='") + String(remainingML) + F("'>");
    chunk += F("<button class='rename-btn' onclick='saveUpdateVolume(") + String(channel) + F(")'>Save</button>");
    chunk += F("<button class='rename-btn cancel' onclick='cancelUpdateVolume()'>Cancel</button>");
    chunk += F("</span></p>");
    chunk += F("</div>");
    
    // Schedule Card
    chunk += F("<div class='card'>");
    chunk += F("<h2>Schedule</h2>");
    
    // Show next dose from weekly schedule
    int today = (timeClient.getDay() + 6) % 7; // 0=Monday, 6=Sunday
    WeeklySchedule* ws = (channel == 1) ? &weeklySchedule1 : &weeklySchedule2;
    int nowHour = timeClient.getHours();
    int nowMinute = timeClient.getMinutes();
    int nextDay = -1, nextHour = -1, nextMinute = -1;
    float nextVol = 0.0f;
    for (int offset = 0; offset < 7; ++offset) {
      int d = (today + offset) % 7;
      if (ws->days[d].enabled) {
        if (offset == 0 && (ws->days[d].hour > nowHour || (ws->days[d].hour == nowHour && ws->days[d].minute > nowMinute))) {
          nextDay = d;
          nextHour = ws->days[d].hour;
          nextMinute = ws->days[d].minute;
          nextVol = ws->days[d].volume;
          break;
        } else if (offset > 0) {
          nextDay = d;
          nextHour = ws->days[d].hour;
          nextMinute = ws->days[d].minute;
          nextVol = ws->days[d].volume;
          break;
        }
      }
    }
    
    if (nextDay >= 0) {
      String ampm = (nextHour < 12) ? F("AM") : F("PM");
      int hour12 = nextHour % 12 == 0 ? 12 : nextHour % 12;
      chunk += F("<p>Next Dose: ") + String(dayNames[nextDay]) + F(", ") + String(hour12) + F(":") + (nextMinute < 10 ? F("0") : F("")) + String(nextMinute) + F(" ") + ampm + F("</p>");
      chunk += F("<p>Next Dose Volume: ") + String(nextVol) + F(" ml</p>");
    } else {
      chunk += F("<p>Next Dose: N/A</p>");
      chunk += F("<p>Next Dose Volume: N/A</p>");
    }
    chunk += F("<button onclick=\"location.href='/manageSchedule?channel=") + String(channel) + F("'\">Manage Schedule</button>");
    chunk += F("</div>");
    
    // Actions Card
    chunk += F("<div class='card'>");
    chunk += F("<button style='background:#dc3545;color:#fff;' onclick=\"location.href='/prime?channel=") + String(channel) + F("'\">Prime Pump</button>");
    chunk += F("<button style='background:#dc3545;color:#fff;' onclick=\"location.href='/calibrate?channel=") + String(channel) + F("'\">Calibrate</button>");
    
    // Rename UI
    chunk += F("<div id='rename-btn-row' style='display:block;'><button onclick=\"showRenameBox()\">Rename</button></div>");
    chunk += F("<div id='rename-row' class='rename-row' style='display:none;'>");
    chunk += F("<input id='rename-input' class='rename-input' type='text' value='") + channelName + F("' maxlength='15'>");
    chunk += F("<button class='rename-btn' onclick='saveRename(") + String(channel) + F(")'>Save</button>");
    chunk += F("<button class='rename-btn cancel' onclick='cancelRename()'>Cancel</button>");
   
    chunk += F("</div>");
    chunk += F("<button style='width:100%;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;' onclick=\"window.location.href='/summary'\">Home</button>");
   
    chunk += F("</div>");
    
    // Back and Home buttons row
   // chunk += F("<div style='display:flex;gap:10px;max-width:500px;margin:20px auto 0 auto;'>");
    //chunk += F("</div>");
    
    // Footer
    chunk += generateFooter();
    chunk += F("</body></html>");
    server.sendContent(chunk);
    
    // End chunked response
    server.sendContent("");
  });

  // Add endpoint to handle rename POST
  server.on("/renameChannel", HTTP_POST, []() {
    if (server.hasArg("channel") && server.hasArg("name")) {
      int channel = server.arg("channel").toInt();
      String newName = server.arg("name");
      if (channel == 1) {
        channel1Name = newName;
      } else if (channel == 2) {
        channel2Name = newName;
      }
      savePersistentDataToSPIFFS();
      server.send(200, "application/json", F("{\"status\":\"renamed\"}"));
    } else {
      server.send(400, "application/json", F("{\"error\":\"missing parameters\"}"));
    }
  });

  // Add endpoint to handle update volume POST
  server.on("/updateVolume", HTTP_POST, []() {
    if (server.hasArg("channel") && server.hasArg("volume")) {
      int channel = server.arg("channel").toInt();
      float newVol = server.arg("volume").toFloat();
      if (channel == 1) {
        remainingMLChannel1 = newVol;
        updateDaysRemaining(1, remainingMLChannel1, &weeklySchedule1);
      } else if (channel == 2) {
        remainingMLChannel2 = newVol;
        updateDaysRemaining(2, remainingMLChannel2, &weeklySchedule2);
      }
      savePersistentDataToSPIFFS();
      server.send(200, "application/json", F("{\"status\":\"updated\"}"));
    } else {
      server.send(400, "application/json", F("{\"error\":\"missing parameters\"}"));
    }
  });

  // --- Manage Schedule UI ---
  server.on("/manageSchedule", HTTP_GET, []() {
    int channel = 1;
    if (server.hasArg("channel")) channel = server.arg("channel").toInt();
    WeeklySchedule* ws = (channel == 2) ? &weeklySchedule2 : &weeklySchedule1;
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    // HTML head and style
    String chunk = F("<html><head><title>Manage Schedule: ") + ws->channelName + F("</title>");
    chunk += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    chunk += F("<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;}\n");
    chunk += F(".card{margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);}\n");
    chunk += F(".card h2{margin-top:0;color:#007BFF;}\n");
    chunk += F(".form-row{margin-bottom:16px;}\n");
    chunk += F("label{display:block;margin-bottom:6px;font-weight:500;}\n");
    chunk += F(".schedule-table-wrapper{width:100%;overflow-x:auto;margin:0 auto;box-sizing:border-box;}\n");
    chunk += F("table.schedule-table{width:100%;max-width:100%;margin:0 auto;box-sizing:border-box;border-collapse:collapse;}\n");
    chunk += F("th,td{padding:8px;text-align:center;box-sizing:border-box;}\n");
    chunk += F("th{background:#007BFF;color:#fff;}\n");
    chunk += F("tr:nth-child(even){background:#f9f9f9;}\n");
    chunk += F("input[type=number]{width:70px;}input[type=time]{width:120px;}label{margin-left:8px;}button{margin:8px 4px;padding:10px 20px;font-size:1em;border-radius:5px;border:none;background:#007BFF;color:#fff;cursor:pointer;}button.cancel{background:#aaa;}button:disabled,input:disabled{background:#eee;color:#888;}\n");
    chunk += F(".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; }\n");
    chunk += F(".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; }\n");
    chunk += F(".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }\n");
    chunk += F("@media (max-width:600px){.card{padding:10px;}th,td{font-size:0.95em;padding:6px;}input[type=number],input[type=time]{width:90%;min-width:60px;}}\n");
    chunk += F("</style>");
    // JS chunk
    chunk += F("<script>\n");
    chunk += F("function copyMondayToOthers() {\n");
    chunk += F("  var enabled=document.getElementById('enabled0').checked;\n");
    chunk += F("  var time=document.getElementById('time0').value;\n");
    chunk += F("  var vol=document.getElementById('vol0').value;\n");
    chunk += F("  for(var i=1;i<7;i++){\n");
    chunk += F("    document.getElementById('enabled'+i).checked=enabled;\n");
    chunk += F("    document.getElementById('time'+i).value=time;\n");
    chunk += F("    document.getElementById('vol'+i).value=vol;\n");
    chunk += F("  }\n");
    chunk += F("}\n");
    chunk += F("function uncopyMonday() {\n");
    chunk += F("  // No disabling, just allow editing\n");
    chunk += F("}\n");
    chunk += F("function onCopyChange(cb){if(cb.checked){copyMondayToOthers();}else{uncopyMonday();}}\n");
    chunk += F("window.addEventListener('DOMContentLoaded',function(){\n");
    chunk += F("  document.getElementById('enabled0').addEventListener('change',function(){if(document.getElementById('copyMonday').checked){copyMondayToOthers();}});\n");
    chunk += F("  document.getElementById('time0').addEventListener('change',function(){if(document.getElementById('copyMonday').checked){copyMondayToOthers();}});\n");
    chunk += F("  document.getElementById('vol0').addEventListener('input',function(){if(document.getElementById('copyMonday').checked){copyMondayToOthers();}});\n");
    chunk += F("});\n");
    chunk += F("</script>\n");
    chunk += F("</head><body>");
    server.sendContent(chunk);
    // Header and card open
    chunk += generateHeader("Manage Schedule : " + ws->channelName);
    chunk += F("<div class='card' style='margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);'>");
    chunk += F("<form id='scheduleForm' method='POST' action='/manageSchedule?channel=") + String(channel) + F("'>");
    chunk += F("<div class='schedule-table-wrapper'>");
    chunk += F("<table class='schedule-table'>");
    chunk += F("<tr style='background:#007BFF;color:#fff;'><th>Day</th><th>Enabled</th><th>Time</th><th>Volume (ml)</th></tr>");
    
    // Table rows chunked
    for (int i = 0; i < 7; ++i) {
      String rowShade = (i % 2 == 0) ? F("background:#f9f9f9;") : F("background:#fff;");
      chunk += F("<tr style='") + rowShade + F("'>");
      chunk += F("<td>") + String(dayNames[i]) + F("</td>");
      chunk += F("<td><input type='checkbox' id='enabled") + String(i) + F("' name='enabled") + String(i) + F("'") + (ws->days[i].enabled ? F(" checked") : F("")) + F("></td>");
      char timebuf[6];
      snprintf(timebuf, sizeof(timebuf), "%02d:%02d", ws->days[i].hour, ws->days[i].minute);
      chunk += F("<td><input type='time' id='time") + String(i) + F("' name='time") + String(i) + F("' value='") + String(timebuf) + F("'></td>");
      chunk += F("<td><input type='number' id='vol") + String(i) + F("' name='vol") + String(i) + F("' step='0.01' min='0' value='") + String(ws->days[i].volume, 2) + F("'></td>");
      chunk += F("</tr>");
    }
    
    // After table
    chunk += F("</table></div>");
    chunk += F("<div style='margin:16px 0 0 0;'><input type='checkbox' id='copyMonday' name='copyMonday' onchange='onCopyChange(this)' style='margin-right:8px;vertical-align:middle;'><label for='copyMonday' style='display:inline;margin:0;white-space:nowrap;vertical-align:middle;'>All day as Monday</label></div>");
    chunk += String(F("<div style='max-width:500px;margin:20px auto;'><input type='checkbox' id='missedDose' name='missedDose'")) + (ws->missedDoseCompensation ? F(" checked") : F("")) + F(" style='margin-right:8px;vertical-align:middle;'><label for='missedDose' style='display:inline;margin:0;white-space:nowrap;vertical-align:middle;'>Missed Dose Compensation</label></div>");
    chunk += F("<div style='display:flex;flex-direction:column;gap:10px;margin-top:20px;'>");
    chunk += F("<button type='submit' style='width:100%;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;'>Save</button>");
    chunk += F("<button type='button' id='cancelBtn' class='cancel' style='width:100%;padding:12px 0;font-size:1.1em;background:#aaa;color:#fff;border:none;border-radius:6px;' onclick=\"window.location.href='/manageChannel?channel=") + String(channel) + F("'\">Cancel</button>");
    chunk += F("</div>");
    chunk += F("</form>");
    chunk += F("</div>");
    
    // Footer and scripts
    chunk += generateFooter();
    chunk += F("<script>\n");
    chunk += F("document.getElementById('scheduleForm').addEventListener('submit',function(e){\n");
    chunk += F("  if(document.getElementById('copyMonday').checked){\n");
    chunk += F("    var enabled=document.getElementById('enabled0').checked;\n");
    chunk += F("    var time=document.getElementById('time0').value;\n");
    chunk += F("    var vol=document.getElementById('vol0').value;\n");
    chunk += F("    for(var i=1;i<7;i'){document.getElementById('enabled'+i).disabled=false;document.getElementById('time'+i).disabled=false;document.getElementById('vol'+i).disabled=false;document.getElementById('enabled'+i).checked=enabled;document.getElementById('time'+i).value=time;document.getElementById('vol'+i).value=vol;}\n");
    chunk += F("  }\n");
    chunk += F("});\n");
    chunk += F("</script>\n");
    chunk += F("</form></body></html>");
    server.sendContent(chunk);
    // End chunked response
    server.sendContent("");
  });

  server.on("/manageSchedule", HTTP_POST, []() {
    int channel = 1;
    if (server.hasArg("channel")) channel = server.arg("channel").toInt();
    WeeklySchedule* ws = (channel == 2) ? &weeklySchedule2 : &weeklySchedule1;
    for (int i = 0; i < 7; ++i) {
      ws->days[i].enabled = server.hasArg("enabled" + String(i));
      String t = server.arg("time" + String(i));
      int h = 0, m = 0;
      if (t.length() == 5) {
        h = t.substring(0,2).toInt();
        m = t.substring(3,5).toInt();
      }
      ws->days[i].hour = h;
      ws->days[i].minute = m;
      ws->days[i].volume = server.arg("vol" + String(i)).toFloat();
    }
    ws->missedDoseCompensation = server.hasArg("missedDose");
    
    // Send response early for better user experience
    server.sendHeader("Location", "/manageChannel?channel=" + String(channel));
    server.send(302, "text/plain", "");
    
    // File I/O operations after response
    saveWeeklySchedulesToSPIFFS();
    updateDaysRemaining(channel, (channel == 1) ? remainingMLChannel1 : remainingMLChannel2, ws);
  });

  server.on("/systemSettings", HTTP_GET, []() {
    // Get MAC address for default device name
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String defaultDeviceName = "Doser";
    if (deviceName == "") deviceName = defaultDeviceName;
    // Use global ntfyChannel
    // Use global notification variables directly instead of local ones
    // Calibration factors
    float calib1 = calibrationFactor1;
    float calib2 = calibrationFactor2;
    
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // HTML head
    String chunk = F("<html><head><title>System Settings</title>");
    chunk += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    chunk += F("<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;} ");
    
    // CSS in smaller chunks
    chunk += F(".card{margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);} ");
    chunk += F(".card h2{margin-top:0;color:#007BFF;} .form-row{margin-bottom:16px;} ");
    chunk += F("label{display:block;margin-bottom:6px;font-weight:500;} ");
    chunk += F("input[type=text],input[type=number],input[type=password],select{width:100%;padding:10px;font-size:1.1em;border-radius:6px;border:1px solid #ccc;box-sizing:border-box;} ");
    chunk += F(".section-title{font-size:1.1em;font-weight:600;margin:18px 0 8px 0;color:#007BFF;} ");
    chunk += F(".checkbox-row{display:flex;align-items:center;gap:10px;margin-bottom:8px;flex-wrap:nowrap;}\n");
    chunk += F(".checkbox-row input[type=checkbox]{margin:0 6px 0 0;flex-shrink:0;}\n");
    chunk += F(".checkbox-row label{margin:0;white-space:nowrap;display:inline-block;vertical-align:middle;}\n");
    chunk += F(".btn-row{display:flex;gap:10px;margin-top:18px;} ");
    chunk += F(".btn{flex:1;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;cursor:pointer;transition:background 0.2s;} ");
    chunk += F(".btn.cancel{background:#aaa;} .btn.danger{background:#dc3545;} .btn.update{background:#28a745;} ");
    chunk += F(".btn:hover{background:#0056b3;} .btn.cancel:hover{background:#888;} .btn.danger:hover{background:#b30000;} .btn.update:hover{background:#218838;} ");
    chunk += F("</style></head><body>");
    
    // Header
    chunk += generateHeader("System Settings");
    
    // Form start and device settings
    chunk += F("<form method='POST' action='/systemSettings'>");
    chunk += F("<div class='card'>");
    chunk += F("<div class='form-row'><label for='deviceName'>Device Name:</label><input type='text' id='deviceName' name='deviceName' value='") + deviceName + F("' maxlength='15'></div>");

    // Timezone dropdown
    chunk += F("<div class='form-row'><label for='timezone'>Time Zone:</label><select name='timezone'>");
    int tzOffsets[] = {-43200,-39600,-36000,-32400,-28800,-25200,-21600,-18000,-14400,-10800,-7200,-3600,0,3600,7200,10800,14400,18000,19800,21600,25200,28800,32400,36000,39600,43200};
    String tzLabels[] = {"UTC-12:00","UTC-11:00","UTC-10:00","UTC-09:00","UTC-08:00 (PST)","UTC-07:00 (MST)","UTC-06:00 (CST)","UTC-05:00 (EST)","UTC-04:00","UTC-03:00","UTC-02:00","UTC-01:00","UTC+00:00","UTC+01:00","UTC+02:00","UTC+03:00","UTC+04:00","UTC+05:00","UTC+05:30 (IST)","UTC+06:00","UTC+07:00","UTC+08:00","UTC+09:00 (JST)","UTC+10:00","UTC+11:00","UTC+12:00"};
    
    // Send timezone options in smaller chunks
    for (int i = 0; i < 26; i += 5) {
      chunk += F("");
      for (int j = i; j < i + 5 && j < 26; j++) {
        chunk += F("<option value='") + String(tzOffsets[j]) + F("'") + (timezoneOffset == tzOffsets[j] ? F(" selected") : F("")) + F(">") + tzLabels[j] + F("</option>");
      }
    }
    chunk += F("</select></div>");
    server.sendContent(chunk);
    
    //chunk += F("<div class='form-row'><label for='numChannels'>Number of Channels:</label><select id='numChannels' name='numChannels' onchange='onNumChannelsChange()'>");
    //chunk += F("<option value='1'") + String(numChannels == 1 ? F(" selected") : F("")) + F(">1</option>");
    //chunk += F("<option value='2'") + String(numChannels == 2 ? F(" selected") : F("")) + F(">2</option>");
    //chunk += F("</select></div>");
    //// ...existing code...
    //// Add password prompt JS for numChannels
    //chunk += F("<script>\n");
    //chunk += F("var prevNumChannels = ") + String(numChannels) + F(";\n");
    //chunk += F("function onNumChannelsChange() {\n");
    //chunk += F("  var sel = document.getElementById('numChannels');\n");
    //chunk += F("  var newVal = sel.value;\n");
    //chunk += F("  var pwd = prompt('Enter admin password to change number of channels:');\n");
    //chunk += F("  if (pwd === 'admin1985') {\n");
    //chunk += F("    prevNumChannels = newVal;\n");
    //chunk += F("  } else {\n");
    //chunk += F("    alert('Incorrect password. Change reverted.');\n");
    //chunk += F("    sel.value = prevNumChannels;\n");
    //chunk += F("  }\n");
    //chunk += F("}\n");
    //chunk += F("</script>\n");
    //// ...existing code...
    chunk = F("<div class='section-title'>Calibration Factor</div>");
    chunk += F("<div class='form-row'>Channel 1: <span style='font-weight:600;'>") + String(calib1, 2) + F("</span></div>");
    if (numChannels == 2) {
      chunk += F("<div class='form-row'>Channel 2: <span style='font-weight:600;'>") + String(calib2, 2) + F("</span></div>");
    }
    
    // Notifications section
    chunk += F("<div class='section-title'>Notifications</div>");
    // NTFY Channel as read-only, default to MAC if empty
    String ntfyDefault = mac;
    chunk += F("<div class='form-row'><label for='ntfyChannel'>NTFY Channel:</label><input type='text' id='ntfyChannel' name='ntfyChannel' value='") + ntfyDefault + F("' readonly></div>");
    chunk += F("<div class='form-row'>Events to Notify:</div>");
    
    chunk += F("<div class='checkbox-row'><input type='checkbox' id='notifyLowFert' name='notifyLowFert'") + String(notifyLowFert ? F(" checked") : F("")) + F("><label for='notifyLowFert'>Low Fertilizer Volume</label></div>");
    chunk += F("<div class='checkbox-row'><input type='checkbox' id='notifyStart' name='notifyStart'") + String(notifyStart ? F(" checked") : F("")) + F("><label for='notifyStart'>System Start</label></div>");
    chunk += F("<div class='checkbox-row'><input type='checkbox' id='notifyDose' name='notifyDose'") + String(notifyDose ? F(" checked") : F("")) + F("><label for='notifyDose'>Dose</label></div>");
    
    // LED Settings section
    chunk += F("<div class='section-title'>LED Settings</div>");
    chunk += F("<div class='form-row'><label for='ledBrightness'>LED Brightness:</label><input type='range' id='ledBrightness' name='ledBrightness' min='0' max='255' value='") + String(ledBrightness) + F("' style='width:100%;'><span id='ledBrightnessValue'>") + String(ledBrightness * 100 / 255) + F("%</span></div>");
    chunk += F("<div class='form-row checkbox-row' style='display:flex;align-items:center;gap:10px;margin-bottom:8px;flex-wrap:nowrap;'>");
    chunk += F("<label for='blinkAllOk' style='margin:0;white-space:nowrap;display:inline-block;vertical-align:middle;'>Power ON LED</label>");
    chunk += F("<input type='checkbox' id='blinkAllOk' name='blinkAllOk' value='1' ") + String((blinkAllOk ? F("checked") : F(""))) + F("> <span>Yes</span>");
    chunk += F("</div>");
    chunk += F("<script>document.getElementById('ledBrightness').addEventListener('input',function(){document.getElementById('ledBrightnessValue').innerText=Math.round(this.value*100/255)+'%';});</script>");

    // Buttons
    chunk += F("<div class='btn-row'>");
    chunk += F("<button type='submit' class='btn btn-main'>Save</button>");
    chunk += F("<button type='button' class='btn btn-cancel' onclick=\"window.location.href='/summary'\">Cancel</button>");
    chunk += F("</div></div></form>"); // Close main form and card

    // Action buttons outside the main form
    chunk += F("<div class='btn-row card-action-row' style='flex-direction:column;gap:4px;'>");
    chunk += F("<form method='POST' action='/restart' style='width:100%;' onsubmit='return handleRestart(event)'><button id='restartBtn' type='submit' class='btn btn-main' style='width:100%;margin-bottom:0;'>Restart</button></form>");
    chunk += F("<form method='POST' action='/wifiReset' style='width:100%;'><button type='submit' class='btn btn-danger' style='width:100%;margin-bottom:0;' onclick=\"return confirm('Reset WiFi settings? Device will reboot in AP mode.')\">WiFi Reset</button></form>");
    chunk += F("<form method='POST' action='/factoryReset' style='width:100%;'><button type='submit' class='btn btn-danger' style='width:100%;margin-bottom:0;' onclick=\"return confirm('Factory reset will erase ALL data. Are you sure?')\">Factory Reset</button></form>");
    chunk += F("<form style='width:100%;'><button type='button' class='btn btn-update' style='width:100%;margin-bottom:0;' onclick=\"showFirmwareUpdate()\">FW Update</button></form>");
    chunk += F("</div>");
    server.sendContent(chunk);
    // Add JS for restart button
    chunk = F("<script>\nfunction handleRestart(e) {\n  var btn = document.getElementById('restartBtn');\n  btn.disabled = true;\n  btn.innerText = 'Restarting..';\n  setTimeout(function() { window.location.href = '/summary'; }, 10000);\n  return true;\n}\n<\/script>");

    // Update CSS for button consistency and centering
    chunk += F("<style> ");
    chunk += F(".btn-row.card-action-row { display: flex; justify-content: center; align-items: center; gap: 10px; max-width: 500px; margin: 0 auto 16px auto; } ");
    chunk += F(".btn-row.card-action-row .btn { flex: unset; min-width: 120px; } ");
    chunk += F(".btn { padding:12px 0; font-size:1.1em; border:none; border-radius:6px; cursor:pointer; transition:background 0.2s; min-width:120px; margin:0 4px 8px 0; } ");
    chunk += F(".btn-main { background:#007BFF; color:#fff; } ");
    chunk += F(".btn-cancel { background:#aaa; color:#fff; } ");
    chunk += F(".btn-danger { background:#dc3545; color:#fff; } ");
    chunk += F(".btn-update { background:#28a745; color:#fff; } ");
    chunk += F(".btn-main:hover { background:#0056b3; } ");
    chunk += F(".btn-cancel:hover { background:#888; } ");
    chunk += F(".btn-danger:hover { background:#b30000; } ");
    chunk += F(".btn-update:hover { background:#218838; } ");
    chunk += F("</style>");

    // Firmware update section
    chunk += F("<div id='firmwareUpdateSection' style='display:none;margin-top:20px;'>");
    chunk += F("<div class='card'>");
    chunk += F("<h3>FW Update</h3>");
    chunk += F("<div class='form-row'><label for='firmwareUrl'>Firmware URL:</label>");
    chunk += F("<input type='text' id='firmwareUrl' value='https://arjunus1985.github.io/Doser/firmware.bin' style='width:100%;padding:10px;font-size:1.1em;border-radius:6px;border:1px solid #ccc;'></div>");
    
    chunk += F("<div class='btn-row'>");
    chunk += F("<button type='button' class='btn btn-update' onclick=\"updateFirmware()\">Update</button>");
    chunk += F("<button type='button' class='btn btn-cancel' onclick=\"hideFirmwareUpdate()\">Cancel</button>");
    chunk += F("</div>");
    
    chunk += F("<div id='updateProgress' style='margin-top:10px;display:none;'>");
    chunk += F("<div>Downloading firmware...</div>");
    chunk += F("<div id='progressBar' style='width:100%;max-width:100%;background:#ddd;border-radius:6px;margin-top:5px;box-sizing:border-box;'>");
    chunk += F("<div id='progressFill' style='width:0%;max-width:100%;height:20px;background:#007BFF;border-radius:6px;transition:width 0.3s;'></div>");
    chunk += F("</div><div id='progressText'>0%</div></div></div></div>");
    // Responsive style for progress bar
    chunk += F("<style>@media (max-width:600px){#progressBar{width:100% !important;max-width:100% !important;}#progressFill{max-width:100% !important;}}</style>");

    // JavaScript in smaller chunks
    chunk += F("<script>");
    chunk += F("function showFirmwareUpdate() {");
    chunk += F("  document.getElementById('firmwareUpdateSection').style.display = 'block';");
    chunk += F("}");
    chunk += F("function hideFirmwareUpdate() {");
    chunk += F("  document.getElementById('firmwareUpdateSection').style.display = 'none';");
    chunk += F("  document.getElementById('updateProgress').style.display = 'none';");
    chunk += F("}");
    chunk += F("async function updateFirmware() {");
    chunk += F("  const url = document.getElementById('firmwareUrl').value;");
    chunk += F("  if (!url) { alert('Please enter firmware URL'); return; }");
    chunk += F("  document.getElementById('updateProgress').style.display = 'block';");
    chunk += F("  const progressFill = document.getElementById('progressFill');");
    chunk += F("  const progressText = document.getElementById('progressText');");
    chunk += F("  try {");
    chunk += F("    const response = await fetch(url);");
    chunk += F("    if (!response.ok) throw new Error('Failed to download firmware');");
    chunk += F("    const total = parseInt(response.headers.get('content-length') || '0');");
    chunk += F("    const reader = response.body.getReader();");
    chunk += F("    const chunks = []; let loaded = 0;");
    
    chunk += F("    while (true) {");
    chunk += F("      const { done, value } = await reader.read();");
    chunk += F("      if (done) break;");
    chunk += F("      chunks.push(value); loaded += value.length;");
    chunk += F("      if (total) {");
    chunk += F("        const progress = (loaded / total) * 100;");
    chunk += F("        progressFill.style.width = progress + '%';");
    chunk += F("        progressText.textContent = Math.round(progress) + '%';");
    chunk += F("      }}");
    
    chunk += F("    const firmwareData = new Uint8Array(loaded);");
    chunk += F("    let offset = 0;");
    chunk += F("    for (const chunk of chunks) {");
    chunk += F("      firmwareData.set(chunk, offset); offset += chunk.length;");
    chunk += F("    }");
    chunk += F("    progressText.textContent = 'Flashing firmware...';");
    chunk += F("    const formData = new FormData();");
    chunk += F("    formData.append('firmware', new Blob([firmwareData]), 'firmware.bin');");
    
    chunk += F("    const uploadResponse = await fetch('/update', {");
    chunk += F("      method: 'POST', body: formData");
    chunk += F("    });");
    chunk += F("    if (uploadResponse.ok) {");
    chunk += F("      progressText.textContent = 'Firmware updated successfully! Device will restart...';");
    chunk += F("      setTimeout(() => { window.location.href = '/summary'; }, 3000);");
    chunk += F("    } else { throw new Error('Failed to flash firmware'); }");
    
    chunk += F("  } catch (error) {");
    chunk += F("    alert('Firmware update failed: ' + error.message);");
    chunk += F("    hideFirmwareUpdate();");
    chunk += F("  }");
    chunk += F("}");
    chunk += F("</script>");

    // Footer
    chunk += generateFooter();
    chunk += F("</body></html>");
    server.sendContent(chunk);
    
    // End chunked response
    server.sendContent("");
  });

  server.on("/restart", HTTP_POST, handleRestartOnly);
  server.on("/wifiReset", HTTP_POST, handleWiFiReset);
  server.on("/factoryReset", HTTP_POST, handleFactoryReset);
  server.on("/systemSettings", HTTP_POST, handleSystemSettingsSave);

  // Root access should redirect to summary
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Location", "/summary");
    server.send(302, "text/plain", "");
  });

  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", F("OK"));
    delay(100);
    ESP.restart();
  }, handleFirmwareUpdate);

  server.begin();
}

void handleCalibration() {
  if (server.hasArg("channel")) {
    int channel = server.arg("channel").toInt();
    
    // If we have the dispensed amount, complete calibration
    if (server.hasArg("dispensedML")) {
      float dispensedML = server.arg("dispensedML").toFloat();
      float &calibrationFactor = (channel == 1) ? calibrationFactor1 : calibrationFactor2;
      calibrationFactor = calibrationTimeMs / dispensedML;
if (channel == 1) calibratedChannel1 = true;
      if (channel == 2) calibratedChannel2 = true;
      savePersistentDataToSPIFFS();
      // Show toast and redirect to channel management
      String html = F("<html><head><meta http-equiv='refresh' content='2;url=/manageChannel?channel=") + String(channel) + F("'>");
      html += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
      html += F("<style>.toast{position:fixed;top:30px;left:50%;transform:translateX(-50%);background:#28a745;color:#fff;padding:18px 32px;border-radius:8px;font-size:1.2em;box-shadow:0 2px 8px rgba(0,0,0,0.15);z-index:9999;}</style>");
      html += F("</head><body>");
      html += F("<div class='toast'>Calibration complete!</div>");
      html += F("<script>setTimeout(function(){window.location.href='/manageChannel?channel=") + String(channel) + F("';},1800);</script>");
      html += F("</body></html>");
      server.send(200, "text/html", html);
      return;
    }
    
    // First phase - run the motor and show input form
    if (channel == 1 || channel == 2) {
      // Run motor for calibrationTimeMs
      runMotor(channel, calibrationTimeMs);
      
      // Show form to input dispensed amount
      String html = F("");
      
      html += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
      html += F("<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;} .card{margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);} .card h2{margin-top:0;color:#007BFF;} .calib-label{font-size:1.1em;margin-bottom:8px;display:block;} .calib-input{width:100%;padding:10px;font-size:1.1em;border-radius:6px;border:1px solid #ccc;margin-bottom:16px;} .calib-submit{width:100%;padding:14px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;cursor:pointer;} .home-btn{width:100%;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;} .back-btn{width:100%;padding:12px 0;font-size:1.1em;background:#aaa;color:#fff;border:none;border-radius:6px;margin-top:10px;} .card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } .card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } .prime-btn.stop:hover { background-color: #218838 !important; } .rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }</style>");
      html += F("</head><body>");
      html += generateHeader("Calibration Measurement");
      html += F("<div class='card'>");
     // html += "<h2>Calibration Measurement</h2>";
      html += F("<p style='margin-bottom:18px;'>Motor has run for 15 seconds. Please measure the dispensed liquid and enter the amount below:</p>");
      html += F("<form action='/calibrate' method='POST'>");
      html += F("<input type='hidden' name='channel' value='") + String(channel) + F("'>");
      html += F("<label for='dispensedML' class='calib-label'>Amount dispensed (ml):</label>");
      html += F("<input type='number' name='dispensedML' step='0.1' required class='calib-input'><br>");
      html += F("<button type='submit' class='calib-submit'>Submit Measurement</button>");
      html += F("</form>");
      html += F("<button class='home-btn' onclick=\"window.location.href='/summary'\">Home</button>");
      html += F("<button class='back-btn' onclick=\"history.back()\">Back</button>");
      html += F("</div>");
      html += generateFooter();
      html += F("</body></html>");
      server.send(200, "text/html", html);
      return;
    }
  }
  
  server.send(400, "application/json", F("{\"error\":\"missing or invalid parameters\"}"));
}



void handleManualDispense() {
  if (server.hasArg("channel") && server.hasArg("ml")) {
    int channel = server.arg("channel").toInt();
    float ml = server.arg("ml").toFloat();
    float calibrationFactor = (channel == 1) ? calibrationFactor1 : calibrationFactor2;

    // Calculate dispense time (calibrationFactor is already in ms/mL)
    int dispenseTime = ml * calibrationFactor;
    
    // Run motor through central function
    runMotor(channel, dispenseTime);
    
    // Update ML and publish
    updateRemainingML(channel, ml);
   

    // Update last dispensed volume and time for the channel
    if (channel == 1) {
      lastDispensedVolume1 = ml;
      lastDispensedTime1 = getFormattedTime();
      Serial.print(F("[MANUAL DOSE] lastDispensedVolume1 set: ")); Serial.println(lastDispensedVolume1);
      Serial.print(F("[MANUAL DOSE] lastDispensedTime1 set: ")); Serial.println(lastDispensedTime1);
    } else if (channel == 2) {
      lastDispensedVolume2 = ml;
      lastDispensedTime2 = getFormattedTime();
      Serial.print(F("[MANUAL DOSE] lastDispensedVolume2 set: ")); Serial.println(lastDispensedVolume2);
      Serial.print(F("[MANUAL DOSE] lastDispensedTime2 set: ")); Serial.println(lastDispensedTime2);
    }
    updateDaysRemaining(channel, (channel == 1) ? remainingMLChannel1 : remainingMLChannel2, (channel == 1) ? &weeklySchedule1 : &weeklySchedule2);
    savePersistentDataToSPIFFS();
    Serial.println(F("[MANUAL DOSE] savePersistentDataToSPIFFS called"));

    // After dosing, send notifications if enabled
    // Use global notification variables instead of reading from form arguments
    String chName = (channel == 1) ? channel1Name : channel2Name;
    float remML = (channel == 1) ? remainingMLChannel1 : remainingMLChannel2;
    WeeklySchedule* ws = (channel == 1) ? &weeklySchedule1 : &weeklySchedule2;
    int daysLeft = calculateDaysRemaining(remML, ws);
    
    if (notifyLowFert && daysLeft <= 7) {
      String msg = "Running low on " + chName + " Refill!!";
      sendNtfyNotification("Low Fertilizer Alert", msg);
    }

    server.send(200, "application/json", F("{\"status\":\"dispensed\"}"));
  } else {
    server.send(400, "application/json", F("{\"error\":\"missing parameters\"}"));
  }
}



void checkDailyDispense() {
  timeClient.update();
  int today = (timeClient.getDay() + 6) % 7; // 0=Monday, 6=Sunday
  int nowHour = timeClient.getHours();
  int nowMinute = timeClient.getMinutes();
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime((time_t *)&epochTime);
  int todayYear = ptm->tm_year + 1900;
  int todayMonth = ptm->tm_mon + 1;
  int todayDay = ptm->tm_mday;
  String todayStr = String(todayDay) + "-" + String(todayMonth) + "-" + String(todayYear);

  // Reset scheduledDoseCompleted flags at midnight - REMOVED: No longer needed with timestamp approach

  // Channel 1
  bool missed1 = false;
  if (weeklySchedule1.days[today].enabled) {
    int schedHour = weeklySchedule1.days[today].hour;
    int schedMinute = weeklySchedule1.days[today].minute;
    float dose = weeklySchedule1.days[today].volume;
    bool timePast = (nowHour > schedHour) || (nowHour == schedHour && nowMinute > schedMinute);
   // bool notDosedToday = (lastDispensedTime1.indexOf(String(todayYear)) == -1 || lastDispensedTime1.indexOf(String(todayMonth)) == -1 || lastDispensedTime1.indexOf(String(todayDay)) == -1);
    if (weeklySchedule1.missedDoseCompensation && timePast && !isToday(lastScheduledDoseTime1) && dose > 0.0f && lastScheduledDoseTime1 != jan1_2025_epoch) {
      missed1 = true;
      int dispenseTime = (int)(dose * calibrationFactor1);
      runMotor(1, dispenseTime);
      updateRemainingML(1, dose);
      lastDispensedVolume1 = dose;
      lastDispensedTime1 = getFormattedTime();
      lastScheduledDoseTime1 = timeClient.getEpochTime();
      savePersistentDataToSPIFFS();
      Serial.print(F("[MISSED DOSE COMPENSATION] Channel 1: Dispensed ")); Serial.print(dose); Serial.println(F(" ml"));
      if (notifyDose) {
        String msg = String(F("Missed scheduled dose given on ")) + channel1Name + F(". Remaining: ") + String(remainingMLChannel1) + F("ml, Days left: ") + String(calculateDaysRemaining(remainingMLChannel1, &weeklySchedule1));
        sendNtfyNotification(F("Dose Notification"), msg);
      }
      if (notifyLowFert && calculateDaysRemaining(remainingMLChannel1, &weeklySchedule1) <= 7) {
        String msg = String(F("Low fertilizer on ")) + channel1Name + F(" Refill!!");
        sendNtfyNotification(F("Low Fertilizer Alert"), msg);
      }
    }
  }

  // Channel 2
  bool missed2 = false;
  if (weeklySchedule2.days[today].enabled) {
    int schedHour = weeklySchedule2.days[today].hour;
    int schedMinute = weeklySchedule2.days[today].minute;
    float dose = weeklySchedule2.days[today].volume;
    bool timePast = (nowHour > schedHour) || (nowHour == schedHour && nowMinute > schedMinute);
    //bool notDosedToday = (lastDispensedTime2.indexOf(String(todayYear)) == -1 || lastDispensedTime2.indexOf(String(todayMonth)) == -1 || lastDispensedTime2.indexOf(String(todayDay)) == -1);
    if (weeklySchedule2.missedDoseCompensation && timePast && !isToday(lastScheduledDoseTime2) && dose > 0.0f && lastScheduledDoseTime2 != jan1_2025_epoch) {
      missed2 = true;
      int dispenseTime = (int)(dose * calibrationFactor2);
      runMotor(2, dispenseTime);
      updateRemainingML(2, dose);
      lastDispensedVolume2 = dose;
      lastDispensedTime2 = getFormattedTime();
      lastScheduledDoseTime2 = timeClient.getEpochTime();
      savePersistentDataToSPIFFS();
      Serial.print(F("[MISSED DOSE COMPENSATION] Channel 2: Dispensed ")); Serial.print(dose); Serial.println(F(" ml"));
      if (notifyDose) {
        String msg = String(F("Missed scheduled dose given on ")) + channel2Name + F(". Remaining: ") + String(remainingMLChannel2) + F("ml, Days left: ") + String(calculateDaysRemaining(remainingMLChannel2, &weeklySchedule2));
        sendNtfyNotification(F("Dose Notification"), msg);
      }
      if (notifyLowFert && calculateDaysRemaining(remainingMLChannel2, &weeklySchedule2) <= 7) {
        String msg = String(F("Low fertilizer on ")) + channel2Name + F(" Refill!!");
        sendNtfyNotification(F("Low Fertilizer Alert"), msg);
      }
    }
  }

  // ...existing scheduled dosing logic (do not double dose if missed dose already handled)...
  if (!missed1 && weeklySchedule1.days[today].enabled && weeklySchedule1.days[today].hour == nowHour && weeklySchedule1.days[today].minute == nowMinute && !isToday(lastScheduledDoseTime1)) {
    float dose = weeklySchedule1.days[today].volume;
    if (dose > 0.0f ) {
      int dispenseTime = (int)(dose * calibrationFactor1); // ms
      runMotor(1, dispenseTime);
      updateRemainingML(1, dose);
      lastDispensedVolume1 = dose;
      lastDispensedTime1 = getFormattedTime();
      lastScheduledDoseTime1 = timeClient.getEpochTime();
      savePersistentDataToSPIFFS();
      Serial.print(F("[SCHEDULED DOSE] Channel 1: Dispensed ")); Serial.print(dose); Serial.println(F(" ml"));
      if (notifyDose) {
        String msg = String(F("Scheduled dose given on ")) + channel1Name + F(". Remaining: ") + String(remainingMLChannel1) + F("ml, Days left: ") + String(calculateDaysRemaining(remainingMLChannel1, &weeklySchedule1));
        sendNtfyNotification(F("Dose Notification"), msg);
      }
      if (notifyLowFert && calculateDaysRemaining(remainingMLChannel1, &weeklySchedule1) <= 7) {
        String msg = String(F("Low fertilizer on ")) + channel1Name + F(" Refill!!");
        sendNtfyNotification(F("Low Fertilizer Alert"), msg);
      }
    }
  }
  if (!missed2 && weeklySchedule2.days[today].enabled && weeklySchedule2.days[today].hour == nowHour && weeklySchedule2.days[today].minute == nowMinute && !isToday(lastScheduledDoseTime2)) {
    float dose = weeklySchedule2.days[today].volume;
    if (dose > 0.0f ) {
      int dispenseTime = (int)(dose * calibrationFactor2); // ms
      runMotor(2, dispenseTime);
      updateRemainingML(2, dose);
      lastDispensedVolume2 = dose;
      lastDispensedTime2 = getFormattedTime();
      lastScheduledDoseTime2 = timeClient.getEpochTime();
      savePersistentDataToSPIFFS();
      Serial.print(F("[SCHEDULED DOSE] Channel 2: Dispensed ")); Serial.print(dose); Serial.println(F(" ml"));
      if (notifyDose) {
        String msg = String(F("Scheduled dose given on ")) + channel2Name + F(". Remaining: ") + String(remainingMLChannel2) + F("ml, Days left: ") + String(calculateDaysRemaining(remainingMLChannel2, &weeklySchedule2));
        sendNtfyNotification(F("Dose Notification"), msg);
      }
      if (notifyLowFert && calculateDaysRemaining(remainingMLChannel2, &weeklySchedule2) <= 7) {
        String msg = String(F("Low fertilizer on ")) + channel2Name + F(" Refill!!");
        sendNtfyNotification(F("Low Fertilizer Alert"), msg);
      }
    }
  }
}

void setupTimeSync() {
  timeClient.begin();
  timeClient.setTimeOffset(timezoneOffset);
  Serial.println(F("Syncing time..."));
  
  int retries = 0;
  while (!timeClient.update() && retries < 10) {
    timeClient.forceUpdate();
    retries++;
    delay(500);
  }
  
  if (retries >= 10) {
    Serial.println(F("Time sync failed!"));
  } else {
    //set global flag
    timeSynced = true;
    Serial.println(F("Time synced successfully"));
    Serial.println(F("Current time: ") + getFormattedTime());
  }
}

#define JSON_BUFFER_SIZE 1024

// JSON handling functions updated to use JsonDocument
void loadPersistentDataFromSPIFFS() {
  File file = LittleFS.open("/data.json", "r");
  if (!file) {
    Serial.println(F("Failed to open file for reading"));
    return;
  }

  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println(F("Failed to parse JSON"));
    return;
  }

  // Load channel names
  channel1Name = doc["name1"] | "Channel 1";
  channel2Name = doc["name2"] | "Channel 2";

  // Load remaining ML values
  remainingMLChannel1 = doc["channel1"].as<float>();
  remainingMLChannel2 = doc["channel2"].as<float>();
  timezoneOffset = doc["timezone"] | 19800;  // Default to UTC if not set

  // Load calibration factors
  calibrationFactor1 = doc["calibration1"] | 1.0f;  // Default to 1 if not set
  calibrationFactor2 = doc["calibration2"] | 1.0f;  // Default to 1 if not set

  // Load last dispensed volume and time
  lastDispensedVolume1 = doc["lastDispensedVolume1"] | 0.0f;
  lastDispensedTime1 = doc["lastDispensedTime1"] | "N/A";
  lastDispensedVolume2 = doc["lastDispensedVolume2"] | 0.0f;
  lastDispensedTime2 = doc["lastDispensedTime2"] | "N/A";

  // Load device name
  deviceName = doc["deviceName"] | "";

  // Load notification settings
  notifyLowFert = doc["notifyLowFert"] | true;
  notifyStart = doc["notifyStart"] | false;
  notifyDose = doc["notifyDose"] | false;

  // Load calibration status
  calibratedChannel1 = doc["calibratedChannel1"] | false;
  calibratedChannel2 = doc["calibratedChannel2"] | false;

  // Load last notified IP (default to empty string)
  lastNotifiedIP = doc["lastNotifiedIP"] | "";

  // Load last scheduled dose timestamps
  //create a variable to hold the epoch of Jan1 2025
  

  lastScheduledDoseTime1 = doc["lastScheduledDoseTime1"] | jan1_2025_epoch; // Default to 0 if not set
  lastScheduledDoseTime2 = doc["lastScheduledDoseTime2"] | jan1_2025_epoch;
  
    // Load LED settings
  ledBrightness = doc["ledBrightness"] | 128;
  blinkAllOk = doc["blinkAllOk"] | true;

  // Load number of channels
  //numChannels = doc["numChannels"] | 1;

  // Load days remaining
  daysRemainingChannel1 = doc["daysRemainingChannel1"] | 0;
  daysRemainingChannel2 = doc["daysRemainingChannel2"] | 0;

  file.close();
  Serial.println(F("Loaded configuration from filesystem"));
}

// JSON handling functions updated to use JsonDocument
void savePersistentDataToSPIFFS() {
  File file = LittleFS.open("/data.json", "w");
  if (!file) {
    Serial.println(F("Failed to open file for writing"));
    return;
  }

  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  
  // Save channel names
  doc["name1"] = channel1Name;
  doc["name2"] = channel2Name;
  
  // Save remaining ML values
  doc["channel1"] = remainingMLChannel1;
  doc["channel2"] = remainingMLChannel2;
  doc["timezone"] = timezoneOffset;
  
  // Save calibration factors
  doc["calibration1"] = calibrationFactor1;
  doc["calibration2"] = calibrationFactor2;

  // Save last dispensed volume and time
  doc["lastDispensedVolume1"] = lastDispensedVolume1;
  doc["lastDispensedTime1"] = lastDispensedTime1;
  doc["lastDispensedVolume2"] = lastDispensedVolume2;
  doc["lastDispensedTime2"] = lastDispensedTime2;

  // Save device name
  doc["deviceName"] = deviceName;

  // Save notification settings
  doc["notifyLowFert"] = notifyLowFert;
  doc["notifyStart"] = notifyStart;
  doc["notifyDose"] = notifyDose;

  // Save calibration status
  doc["calibratedChannel1"] = calibratedChannel1;
  doc["calibratedChannel2"] = calibratedChannel2;

  // Save last notified IP
  doc["lastNotifiedIP"] = lastNotifiedIP;

  // Save last scheduled dose timestamps
  doc["lastScheduledDoseTime1"] = lastScheduledDoseTime1;
  doc["lastScheduledDoseTime2"] = lastScheduledDoseTime2;

  // Save LED settings
  doc["ledBrightness"] = ledBrightness;
  doc["blinkAllOk"] = blinkAllOk;

  // Save number of channels
  //doc["numChannels"] = numChannels;

  // Save days remaining
  doc["daysRemainingChannel1"] = daysRemainingChannel1;
  doc["daysRemainingChannel2"] = daysRemainingChannel2;

  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Failed to write JSON to file"));
  }

  file.close();
  Serial.println(F("Saved configuration to filesystem"));
}



void updateRemainingML(int channel, float dispensedML) {
  if (channel == 1) {
    remainingMLChannel1 -= dispensedML;
  } else if (channel == 2) {
    remainingMLChannel2 -= dispensedML;
  }
  savePersistentDataToSPIFFS();

 
}



//void handleSystemReset() {
//  // Reset system settings
//  LittleFS.remove("/data.json"); // Example reset logic
//  ESP.restart();
//}

void setupOTA() {
  ArduinoOTA.setPassword("admin1985");
  ArduinoOTA.onStart([]() {
    // Start with red
    updateLED(LED_RED);
    Serial.println(F("[OTA] Start updating"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    float pct = (float)progress / (float)total;
    if (pct < 0.33) {
      updateLED(LED_RED);
    } else if (pct < 0.66) {
      updateLED(LED_YELLOW);
    } else {
      updateLED(LED_GREEN);
    }
    Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onEnd([]() {
    updateLED(LED_GREEN);
    Serial.println(F("[OTA] End"));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    updateLED(LED_RED);
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println(F("Auth Failed"));
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println(F("Begin Failed"));
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println(F("Connect Failed"));
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println(F("Receive Failed"));
    } else if (error == OTA_END_ERROR) {
      Serial.println(F("End Failed"));
    }
  });
  ArduinoOTA.begin();
  Serial.println(F("[OTA] Ready for updates"));
}

void blinkLED(uint32_t color, int times) {
  for (int i = 0; i < times; i++) {
    updateLED(color);
    delay(500);
    updateLED(0x000000); // Turn off LED
    delay(500);
  }
}

void updateLEDState() {
  unsigned long currentMillis = millis();

  switch (currentLEDState) {
    case LED_BLINK_GREEN:
      if (!blinkAllOk) {
        updateLED(0x000000); // Turn off LED if blinkAllOk is false
        break;
      }
      if (currentMillis - lastLEDUpdate >= 5000) { // Main cycle every 5 seconds
        lastLEDUpdate = currentMillis;
        updateLED(LED_GREEN);
        blinkCount = 0;
      } else if (currentMillis - lastLEDUpdate >= 100) { // Turn off after 500ms
        updateLED(0x000000);
      }
      break;

    case LED_BLINK_RED:
      if (currentMillis - lastLEDUpdate >= 500) { // Blink every 500 ms
        lastLEDUpdate = currentMillis;
        if (blinkCount % 2 == 0) {
          updateLED(LED_RED);
        } else {
          updateLED(0x000000);
        }
        blinkCount++;
      }
      break;

    case LED_BLINK_BLUE:
      if (currentMillis - lastLEDUpdate >= 500) {
        lastLEDUpdate = currentMillis;
        if (blinkCount % 2 == 0) {
          updateLED(LED_BLUE);
        } else {
          updateLED(0x000000);
        }
        blinkCount++;
        if (blinkCount >= 6) { // Blink at least 3 times
          currentLEDState = LED_BLINK_GREEN;
          lastLEDUpdate = currentMillis; // Reset timing for green state
        }
      }
      break;

    case LED_BLINK_YELLOW:
      if (currentMillis - lastLEDUpdate >= 500) {
        lastLEDUpdate = currentMillis;
        if (blinkCount % 2 == 0) {
          updateLED(LED_YELLOW);
        } else {
          updateLED(0x000000);
        }
        blinkCount++;
        if (blinkCount >= 6) { // Blink at least 3 times
          currentLEDState = LED_BLINK_GREEN;
          lastLEDUpdate = currentMillis; // Reset timing for green state
        }
      }
      break;

    case LED_OFF:
      updateLED(0x000000);
      break;
  }
}

// Add this new central motor control function
void runMotor(int channel, int durationMs) {
  // Save current LED state
  LEDState previousState = currentLEDState;
  
  // Set LED color based on channel
  if (channel == 1) {
    updateLED(LED_BLUE);
  } else if (channel == 2) {
    updateLED(LED_YELLOW);
  }
  
  // Run motor
  int motorPin = (channel == 1) ? MOTOR1_PIN : MOTOR2_PIN;
  digitalWrite(motorPin, HIGH);
  delay(durationMs);
  digitalWrite(motorPin, LOW);
  
  // Restore previous LED state
  setLEDState(previousState);
}

void handlePrimePump() {
  if (server.hasArg("channel") && server.hasArg("state")) {
    int channel = server.arg("channel").toInt();
    bool state = server.arg("state") == "1";
    
    if (channel == 1) {
      isPrimingChannel1 = state;
    } else if (channel == 2) {
      isPrimingChannel2 = state;
    }
    
    String msg = String(F("{\"status\":\"prime pump ")) + (state ? F("started") : F("stopped")) + F("\"}");
    server.send(200, "application/json", msg);
  } else {
    server.send(400, "application/json", F("{\"error\":\"missing parameters\"}"));
  }
}

// Common header and footer generators
String generateHeader(const String& title) {
  String html = F("<div style='max-width:550px;width:100%;margin:0 auto 10px auto;background:#007BFF;color:#fff;padding:16px 20px;text-align:center;font-size:1.5em;border-radius:10px 10px 0 0;box-shadow:0 2px 4px rgba(0,0,0,0.05);box-sizing:border-box;'>");
  html += title;
  html += F("</div>");
  return html;
}

// Helper function to get WiFi signal strength description
String getWiFiSignalStrength() {
  if (WiFi.status() != WL_CONNECTED) {
    return F("Disconnected");
  }
  
  int32_t rssi = WiFi.RSSI();
  
  if (rssi >= -50) {
    return F("Excellent");
  } else if (rssi >= -60) {
    return F("Strong");
  } else if (rssi >= -70) {
    return F("Medium");
  } else if (rssi >= -80) {
    return F("Low");
  } else {
    return F("Poor");
  }
}

String generateFooter() {
  String html = F("<div style='width:100%;background:#f1f1f1;color:#333;padding:10px 0;text-align:center;font-size:1em;border-radius:0 0 10px 10px;box-shadow:0 -2px 4px rgba(0,0,0,0.03);margin-top:20px;'>");
  html += F("S/W version : 25.07.12  mymail.arjun@gmail.com");
  html += F("<br>H/W version: ") + String(hwVersion, 1);
  html += F("<br>Available RAM: ") + String(ESP.getFreeHeap() / 1024.0, 2) + F(" KB");
  html += F("<br>WiFi Signal: ") + getWiFiSignalStrength();
  html += F("</div>");
  return html;
}

void handleRestartOnly() {
  
 Serial.println(F("Restarting system..."));

  // Give browser time to receive response, then restart
  delay(500);
  ESP.restart();
}

void handleWiFiReset() {
 
  String redirectUrl = "http://" + deviceName;
  redirectUrl.replace(" ", "-");
  redirectUrl += ".local/";
  String html = F("<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>WiFi Reset</title></head><body style='font-family:Arial,sans-serif;background:#f4f4f9;color:#333;'><div style='max-width:500px;margin:40px auto;padding:24px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.08);'><h2 style='color:#007BFF;'>WiFi Reset</h2><p>Since you have reset Wifi/System connect to <b>")+ deviceName + F("</b> access point from Wifi settings once device led glows purple and proceed with setting up WiFi again. Once WiFi connected click on link below:</p><div style='margin:18px 0;'><a href='");
  html += redirectUrl;
  html += F("' style='display:block;padding:14px 0;background:#007BFF;color:#fff;text-align:center;border-radius:6px;font-size:1.1em;text-decoration:none;'>");
  html += redirectUrl;
  html += F("</a></div></div></body></html>");
  server.send(200, "text/html", html);
  // Set flag and timer for reset in loop
  pendingWiFiReset = true;
  resetRequestTime = millis();
}

void handleFactoryReset() {

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String suffix = mac.substring(9,11);
  String redirectUrl = "http://doser_" + suffix + ".local/";
  String html = F("<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Factory Reset</title></head><body style='font-family:Arial,sans-serif;background:#f4f4f9;color:#333;'><div style='max-width:500px;margin:40px auto;padding:24px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.08);'><h2 style='color:#dc3545;'>Factory Reset</h2><p>Since you have reset System connect to <b>")+deviceName+F("</b> access point from Wifi settings once device led glows purple and proceed with setting up WiFi again. Once WiFi connected click on link below:</p><div style='margin:18px 0;'><a href='");
  html += redirectUrl;
  html += F("' style='display:block;padding:14px 0;background:#007BFF;color:#fff;text-align:center;border-radius:6px;font-size:1.1em;text-decoration:none;'>");
  html += redirectUrl;
  html += F("</a></div></div></body></html>");
  server.send(200, "text/html", html);
  // Set flag and timer for reset in loop
  pendingFactoryReset = true;
  resetRequestTime = millis();
}

void handleSystemSettingsSave() {
  bool updated = false;
  
  // Save timezone if provided
  if (server.hasArg("timezone")) {
    int newTimezone = server.arg("timezone").toInt();
    if (newTimezone != timezoneOffset) {
      timezoneOffset = newTimezone;
      timeClient.setTimeOffset(timezoneOffset); // Ensure NTP client uses new offset immediately
      updated = true;
    }
  }
  
  // Save device name if provided
  if (server.hasArg("deviceName")) {
    String newDeviceName = server.arg("deviceName");
    if (newDeviceName != deviceName) {
      deviceName = newDeviceName;
      updated = true;
    }
  }

  // Save notification settings - always mark as updated since these are important
  bool oldNotifyLowFert = notifyLowFert;
  bool oldNotifyStart = notifyStart;
  bool oldNotifyDose = notifyDose;
  
  notifyLowFert = server.hasArg("notifyLowFert");
  notifyStart = server.hasArg("notifyStart");
  notifyDose = server.hasArg("notifyDose");
  
  // Check if notification settings changed
  if (oldNotifyLowFert != notifyLowFert || oldNotifyStart != notifyStart || oldNotifyDose != notifyDose) {
    updated = true;
  }
  if (server.hasArg("ledBrightness")) {
    int newBrightness = server.arg("ledBrightness").toInt();
    if (newBrightness < 0) newBrightness = 0;
    if (newBrightness > 255) newBrightness = 255;
    ledBrightness = (uint8_t)newBrightness;
    updated = true;
  }

  // Save blinkAllOk if provided
  blinkAllOk = server.hasArg("blinkAllOk");

  // Save number of channels if provided
  //if (server.hasArg("numChannels")) {
  //  int newNumChannels = server.arg("numChannels").toInt();
  //  if (newNumChannels != numChannels && (newNumChannels == 1 || newNumChannels == 2)) {
  //    numChannels = newNumChannels;
  //    updated = true;
  //  }
  //}

  // Save other settings here as needed (device name, NTFY settings, etc.)
  
  // Always save to persist notification settings
  savePersistentDataToSPIFFS();
  server.sendHeader("Location", "/summary");
  server.send(302, "text/plain", "");
}

// Helper: Send NTFY notification
void sendNtfyNotification(const String& title, const String& message) {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String ntfyUrl = F("http://ntfy.sh/") + mac;
  Serial.println(F("Sending NTFY notification to: ") + ntfyUrl);
  WiFiClient wifiClient;
  HTTPClient http;
  http.begin(wifiClient, ntfyUrl);
  http.addHeader(F("Title"), title);
  http.POST(message);
  http.end();
}

// Helper: Calculate days remaining for a channel
int calculateDaysRemaining(float remainingML, WeeklySchedule* ws) {
  int days = 0;
  int dayIdx = (timeClient.getDay() + 6) % 7;
  float rem = remainingML;
  for (int i = 0; i < 365; ++i) {
    int d = (dayIdx + i) % 7;
    if (ws->days[d].enabled) {
      float dose = ws->days[d].volume;
      if (rem < dose || dose <= 0.0f) break;
      rem -= dose;
      days++;
    }
  }
  return days;
}

void handleFirmwareUpdate() {
    HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    updateLED(LED_BLUE); // Set LED to purple at start
    Serial.setDebugOutput(true);
    Serial.printf("[OTA] Update: %s\n", upload.filename.c_str());
    if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[OTA] Update Success: %u bytes\nRebooting...\n", upload.totalSize);
   //   updateLED(LED_GREEN); // Set LED to green when done
   //   delay(1000); // Show green for 1 second
    } else {
      Update.printError(Serial);
    }
    Serial.setDebugOutput(false);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    Serial.println(F("[OTA] Update was aborted"));
  }
  yield();
}

// New function to update days remaining and persist
template<typename T>
void updateDaysRemaining(int channel, float remainingML, T* ws) {
  int days = 0;
  int dayIdx = (timeClient.getDay() + 6) % 7;
  float rem = remainingML;
  for (int i = 0; i < 365; ++i) {
    int d = (dayIdx + i) % 7;
    if (ws->days[d].enabled) {
      float dose = ws->days[d].volume;
      if (rem < dose || dose <= 0.0f) break;
      rem -= dose;
      days++;
    }
  }
  if (channel == 1) {
    daysRemainingChannel1 = days;
  } else if (channel == 2) {
    daysRemainingChannel2 = days;
  }
  savePersistentDataToSPIFFS();
}

