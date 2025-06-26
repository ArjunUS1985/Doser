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
#define SPIFFS LittleFS // Replace SPIFFS with LittleFS for compatibility

// Global variables that getFormattedTime needs
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);  // 19800 seconds = 5.5 hours for IST

// Time formatting function
String getFormattedTime() {
  timeClient.update();
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime);
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

// LED state management
enum LEDState {
  LED_OFF,
  LED_BLINK_GREEN,
  LED_BLINK_RED,
  LED_BLINK_BLUE,
  LED_BLINK_YELLOW
};

// Function prototypes
void updateLEDState();
void setLEDState(LEDState state);
void updateLED(uint32_t color);
void handlePrimePump();
// Function declarations for header and footer generators
String generateHeader(String title);
String generateFooter();

// Global Variables for LED
LEDState currentLEDState = LED_OFF;
unsigned long lastLEDUpdate = 0;
int blinkCount = 0;

void updateLED(uint32_t color) {
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
String deviceName = "";

// Calibration Variables
float calibrationFactor1 = 1;
float calibrationFactor2 = 1;

// Persistent Storage Variables
float remainingMLChannel1 = 0.0;
float remainingMLChannel2 = 0.0;

// Add timezone offset to global variables
int32_t timezoneOffset = 0;  // Default to UTC

// Add last dispensed volume and time string for each channel
float lastDispensedVolume1 = 0.0;
float lastDispensedVolume2 = 0.0;
String lastDispensedTime1 = "N/A";
String lastDispensedTime2 = "N/A";

// Prime pump state variables
bool isPrimingChannel1 = false;
bool isPrimingChannel2 = false;

// Function Prototypes
void setupWiFi();
void setupWebServer();
void handleCalibration();
void handleManualDispense();
void handleDailyDispense();
void updateLED(uint32_t color);
//void calibrateMotor(int channel, float &calibrationFactor);
void setupTimeSync();
void checkDailyDispense();
void loadPersistentDataFromSPIFFS();
void savePersistentDataToSPIFFS();
void handleBottleTracking();
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

WeeklySchedule weeklySchedule1;
WeeklySchedule weeklySchedule2;

// --- Helper: Day names ---
const char* dayNames[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

// --- Save/Load Weekly Schedules ---
void saveWeeklySchedulesToSPIFFS() {
  File file = LittleFS.open("/weekly_schedules.json", "w");
  if (!file) {
    Serial.println("Failed to open weekly_schedules.json for writing");
    return;
  }
  DynamicJsonDocument doc(2048);
  // Channel 1
  JsonObject ch1 = doc.createNestedObject("ch1");
  ch1["channelName"] = weeklySchedule1.channelName;
  ch1["missedDoseCompensation"] = weeklySchedule1.missedDoseCompensation;
  JsonArray days1 = ch1.createNestedArray("days");
  for (int i = 0; i < 7; ++i) {
    JsonObject d = days1.createNestedObject();
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

void setup() {
  // Initialize Serial
  Serial.begin(9600);
  // Initialize WS2812B LED
  strip.begin();
  strip.show(); // Ensure all LEDs are off initially

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
    Serial.println("Failed to mount file system");
    return;
  }

  // Load Persistent Data from SPIFFS
  loadPersistentDataFromSPIFFS();
  loadWeeklySchedulesFromSPIFFS();
  Serial.print("[BOOT] lastDispensedVolume1: "); Serial.println(lastDispensedVolume1);
  Serial.print("[BOOT] lastDispensedTime1: "); Serial.println(lastDispensedTime1);
  Serial.print("[BOOT] lastDispensedVolume2: "); Serial.println(lastDispensedVolume2);
  Serial.print("[BOOT] lastDispensedTime2: "); Serial.println(lastDispensedTime2);

  // Setup WiFi
  setupWiFi();

  // Setup Web Server
  setupWebServer();

  // Setup Time Sync
  setupTimeSync();



  // Initialize OTA
  setupOTA();

  // Initialize mDNS
  if (MDNS.begin("doser")) { // Replace "doser" with your desired hostname
    Serial.println("mDNS responder started");
  } else {
    Serial.println("Error setting up mDNS responder!");
  }

  // Set LED to Green at the end of setup
  updateLED(LED_GREEN);
}

void loop() {
  // Handle Web Server
  server.handleClient();




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
  if (!isPrimingChannel1 && !isPrimingChannel2) {
    checkDailyDispense();
  }

  // Only update LED state if not priming
  if (!isPrimingChannel1 && !isPrimingChannel2) {
    if (currentLEDState == LED_OFF) {
      if (WiFi.status() == WL_CONNECTED) {
        setLEDState(LED_BLINK_GREEN);
      } else {
        setLEDState(LED_BLINK_RED);
      }
    }
    // Update LED state
    updateLEDState();
  }
  
  ArduinoOTA.handle();
}

void setupWiFi() {
  WiFiManager wifiManager;

  // Automatically start configuration portal if no WiFi is configured
  if (!wifiManager.autoConnect("Doser_AP")) {
    Serial.println("Failed to connect to WiFi and hit timeout");
    ESP.restart();
  }

  Serial.println("Connected to WiFi.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Update LED to green after WiFi connection
 // updateLED(LED_GREEN);
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
    String chunk = "<html><head><title>Calibrate</title>";
    chunk += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    chunk += "<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;} ";
    server.sendContent(chunk);
    
    // Send CSS in smaller chunks
    chunk = ".card{margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);} ";
    chunk += ".card h2{margin-top:0;color:#007BFF;} ";
    chunk += ".calib-warning{color:#b30000;background:#fff3cd;border:1px solid #ffeeba;border-radius:6px;padding:10px;margin-bottom:18px;font-size:1.05em;} ";
    server.sendContent(chunk);
    
    chunk = ".calib-btn{width:100%;padding:14px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;margin-bottom:10px;cursor:pointer;} ";
    chunk += ".calib-btn:disabled{background:#aaa;cursor:not-allowed;} ";
    chunk += ".home-btn{width:100%;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;} ";
    server.sendContent(chunk);
    
    chunk = ".back-btn{width:100%;padding:12px 0;font-size:1.1em;background:#aaa;color:#fff;border:none;border-radius:6px;margin-top:10px;} ";
    chunk += "#countdown{font-size:1.2em;color:#007BFF;margin-bottom:10px;text-align:center;} ";
    chunk += ".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } ";
    server.sendContent(chunk);
    
    chunk = ".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } ";
    chunk += ".prime-btn.stop:hover { background-color: #218838 !important; } ";
    chunk += ".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }";
    chunk += "</style>";
    server.sendContent(chunk);
    
    // Send JavaScript
    chunk = "<script>\n";
    chunk += "function startCountdown() {\n";
    chunk += "  var btn = document.getElementById('calibBtn');\n";
    chunk += "  var homeBtn = document.getElementById('homeBtn');\n";
    chunk += "  var backBtn = document.getElementById('backBtn');\n";
    chunk += "  var countdown = document.getElementById('countdown');\n";
    server.sendContent(chunk);
    
    chunk = "  btn.disabled = true;\n";
    chunk += "  homeBtn.disabled = true;\n";
    chunk += "  backBtn.disabled = true;\n";
    chunk += "  var timeLeft = 15;\n";
    chunk += "  countdown.innerText = 'Calibrating... ' + timeLeft + 's remaining';\n";
    chunk += "  var interval = setInterval(function() {\n";
    server.sendContent(chunk);
    
    chunk = "    timeLeft--;\n";
    chunk += "    countdown.innerText = 'Calibrating... ' + timeLeft + 's remaining';\n";
    chunk += "    if (timeLeft <= 0) {\n";
    chunk += "      clearInterval(interval);\n";
    chunk += "      countdown.innerText = '';\n";
    chunk += "      btn.disabled = false;\n";
    chunk += "      homeBtn.disabled = false;\n";
    chunk += "      backBtn.disabled = false;\n";
    server.sendContent(chunk);
    
    chunk = "    }\n";
    chunk += "  }, 1000);\n";
    chunk += "}\n";
    chunk += "function onSubmitCalib(e){\n";
    chunk += "  startCountdown();\n";
    chunk += "}\n";
    chunk += "</script>";
    chunk += "</head><body>";
    chunk += generateHeader("Calibrate: " + channelName);
    chunk += "<div class='card'>";
    chunk += "<div class='calib-warning'>Warning: The motor will run for 15 seconds and dispense liquid. Hold the measuring tube near the dispensing tube before proceeding.</div>";
    server.sendContent(chunk);
    
    chunk = "<div id='countdown'></div>";
    chunk += "<form action='/calibrate?channel=" + String(channel) + "' method='POST' onsubmit='onSubmitCalib(event)'>";
    chunk += "<input type='hidden' name='channel' value='" + String(channel) + "'>";
    chunk += "<button type='submit' class='calib-btn' id='calibBtn'>Start Calibration</button>";
    chunk += "</form>";
    chunk += "<button class='home-btn' id='homeBtn' onclick=\"window.location.href='/newUI/summary'\">Home</button>";
    chunk += "<button class='back-btn' id='backBtn' onclick=\"history.back()\">Back</button>";
    chunk += "</div>";
    chunk += generateFooter();
    chunk += "</body></html>";
    server.sendContent(chunk);
    
    // End the response
    server.sendContent("");
  });

  

  server.on("/reset", HTTP_GET, []() {
    String html = "<html><head><title>System Reset</title></head><body>";
    html += "<h1>System Reset</h1>";
    html += "<p>Click the button below to reset the system.</p>";
    html += "<form action='/reset' method='POST'>";
    html += "<input type='submit' value='Reset System'>";
    html += "</form>";
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

 

  server.on("/timezone", HTTP_POST, []() {
    if (server.hasArg("offset")) {
      timezoneOffset = server.arg("offset").toInt();
      timeClient.setTimeOffset(timezoneOffset);
      savePersistentDataToSPIFFS();
      server.send(200, "application/json", "{\"status\":\"timezone updated\"}");
    } else {
      server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
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
    String chunk = "<html><head>";
    chunk += "<title>Prime Pump</title>";
    chunk += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    chunk += "<style>body { font-family: Arial, sans-serif; background-color: #f4f4f9; color: #333; } ";
    server.sendContent(chunk);
    
    // CSS in chunks
    chunk = ".card { margin: 20px auto; padding: 20px; max-width: 500px; background: #fff; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); } ";
    chunk += ".card h2 { margin-top: 0; color: #007BFF; } .prime-warning { color: #b30000; background: #fff3cd; border: 1px solid #ffeeba; border-radius: 6px; padding: 10px; margin-bottom: 18px; font-size: 1.05em; } ";
    server.sendContent(chunk);
    
    chunk = ".prime-btn { width: 100%; padding: 14px 0; font-size: 1.1em; background: #dc3545; color: #fff; border: none; border-radius: 6px; margin-bottom: 10px; cursor: pointer; transition: background 0.2s; } ";
    chunk += ".prime-btn.stop { background: #28a745; } ";
    chunk += ".prime-btn:active { opacity: 0.9; } ";
    chunk += ".home-btn { width: 100%; padding: 12px 0; font-size: 1.1em; background: #007BFF; color: #fff; border: none; border-radius: 6px; } ";
    server.sendContent(chunk);
    
    chunk = ".back-btn { width: 100%; padding: 12px 0; font-size: 1.1em; background: #aaa; color: #fff; border: none; border-radius: 6px; margin-top: 10px; } ";
    chunk += ".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } ";
    chunk += ".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } ";
    server.sendContent(chunk);
    
    chunk = ".prime-btn.stop:hover { background-color: #218838 !important; } ";
    chunk += ".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }";
    chunk += "</style>";
    server.sendContent(chunk);
    
    // JavaScript
    chunk = "<script>\n";
    chunk += "function togglePrime() {\n";
    chunk += "  var btn = document.getElementById('primeButton');\n";
    chunk += "  var state = btn.getAttribute('data-state') === '1' ? '0' : '1';\n";
    chunk += "  var xhr = new XMLHttpRequest();\n";
    chunk += "  xhr.open('POST', '/prime', true);\n";
    chunk += "  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n";
    chunk += "  xhr.send('channel=" + String(channel) + "&state=' + state);\n";
    chunk += "  btn.setAttribute('data-state', state);\n";
    chunk += "  btn.value = state === '1' ? 'Done' : 'Start';\n";
    chunk += "  btn.className = state === '1' ? 'prime-btn stop' : 'prime-btn';\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "window.onload = function() {\n";
    chunk += "  var btn = document.getElementById('primeButton');\n";
    chunk += "  btn.value = 'Start';\n";
    chunk += "  btn.className = 'prime-btn';\n";
    chunk += "}\n";
    chunk += "</script>";
    chunk += "</head><body>";
    server.sendContent(chunk);
    
    // Body content
    chunk = generateHeader("Prime Pump: " + channelName);
    chunk += "<div class='card'>";
    chunk += "<div class='prime-warning'>Warning: This action will turn on the pump and liquid will flow. Please ensure tubing is connected and ready.</div>";
    chunk += "<input type='button' id='primeButton' data-state='0' value='Start' class='prime-btn' onclick='togglePrime()'>";
    chunk += "<button class='home-btn' onclick=\"window.location.href='/newUI/summary'\">Home</button>";
    chunk += "<button class='back-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#aaa;color:#fff;border:none;border-radius:6px;margin-top:10px;' onclick=\"history.back()\">Back</button>";
    chunk += "</div>";
    chunk += generateFooter();
    chunk += "</body></html>";
    server.sendContent(chunk);
    
    // End chunked response
    server.sendContent("");
  });


  server.on("/newUI/summary", HTTP_GET, []() {
    Serial.print("[SUMMARY] lastDispensedVolume1: "); Serial.println(lastDispensedVolume1);
    Serial.print("[SUMMARY] lastDispensedTime1: "); Serial.println(lastDispensedTime1);
    Serial.print("[SUMMARY] lastDispensedVolume2: "); Serial.println(lastDispensedVolume2);
    Serial.print("[SUMMARY] lastDispensedTime2: "); Serial.println(lastDispensedTime2);
    
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Send HTML header
    String chunk = "<html><head><title>Dosing Summary</title>";
    chunk += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    chunk += "<style>body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f9; color: #333; } ";
    server.sendContent(chunk);
    
    // Send CSS in chunks
    chunk = ".card { margin: 20px auto; padding: 20px; max-width: 500px; background: #fff; border-radius: 10px; box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1); } ";
    chunk += ".card h2 { margin-top: 0; color: #007BFF; } .card p { margin: 10px 0; } ";
    server.sendContent(chunk);
    
    chunk = ".card button { display: block; width: 100%; margin: 10px 0; padding: 10px; font-size: 16px; color: #fff; background-color: #007BFF; ";
    chunk += "border: none; border-radius: 5px; cursor: pointer; } .card button:hover { background-color: #0056b3; } ";
    server.sendContent(chunk);
    
    chunk = ".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } ";
    chunk += ".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } ";
    chunk += ".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }</style></head><body>";
    server.sendContent(chunk);
    
    // Generate header
    chunk = generateHeader("Dosing Summary");
    server.sendContent(chunk);
    
    // Channel 1 Summary
    int daysRemaining1 = 0;
    float rem1 = remainingMLChannel1;
    int dayIdx1 = (timeClient.getDay() + 6) % 7; // 0=Monday, 6=Sunday
    int simulatedDays1 = 0;
    for (int i = 0; i < 365; ++i) { // max 1 year
      int d = (dayIdx1 + i) % 7;
      if (weeklySchedule1.days[d].enabled) {
        float dose = weeklySchedule1.days[d].volume;
        if (rem1 < dose || dose <= 0.0f) break;
        rem1 -= dose;
        daysRemaining1++;
      }
      simulatedDays1++;
    }
    bool moreThanYear1 = false;
    if (rem1 > 0.0f && simulatedDays1 == 365) {
      daysRemaining1 = 366;
      moreThanYear1 = true;
    }
    
    chunk = "<div class='card'>";
    chunk += "<h2>" + channel1Name + "</h2>";
    chunk += "<p>Last Dosed Time: " + lastDispensedTime1 + "</p>";
    chunk += "<p>Last Volume: " + String(lastDispensedVolume1) + " ml</p>";
    chunk += "<p>Remaining Volume: " + String(remainingMLChannel1) + " ml</p>";
    chunk += "<p>Days Remaining: ";
    if (moreThanYear1) chunk += "More than a year";
    else chunk += String(simulatedDays1);
    chunk += "</p>";
    server.sendContent(chunk);
    
    chunk = "<div id='manualDoseSection1'>";
    chunk += "<button class='card-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#28a745;color:#fff;border:none;border-radius:6px;margin-bottom:10px;' onclick='showManualDose1()'>Manual Dose</button>";
    chunk += "</div>";
    chunk += "<button onclick=\"location.href='/newUI/manageChannel?channel=1'\">Manage Channel 1</button>";
    chunk += "</div>";
    server.sendContent(chunk);
    
    // Channel 2 Summary
    int daysRemaining2 = 0;
    float rem2 = remainingMLChannel2;
    int dayIdx2 = (timeClient.getDay() + 6) % 7;
    int simulatedDays2 = 0;
    for (int i = 0; i < 365; ++i) {
      int d = (dayIdx2 + i) % 7;
      if (weeklySchedule2.days[d].enabled) {
        float dose = weeklySchedule2.days[d].volume;
        if (rem2 < dose || dose <= 0.0f) break;
        rem2 -= dose;
        daysRemaining2++;
      }
      simulatedDays2++;
    }
    bool moreThanYear2 = false;
    if (rem2 > 0.0f && simulatedDays2 == 365) {
      daysRemaining2 = 366;
      moreThanYear2 = true;
    }
    
    chunk = "<div class='card'>";
    chunk += "<h2>" + channel2Name + "</h2>";
    chunk += "<p>Last Dosed Time: " + lastDispensedTime2 + "</p>";
    chunk += "<p>Last Volume: " + String(lastDispensedVolume2) + " ml</p>";
    chunk += "<p>Remaining Volume: " + String(remainingMLChannel2) + " ml</p>";
    chunk += "<p>Days Remaining: ";
    if (moreThanYear2) chunk += "More than a year";
    else chunk += String(simulatedDays2);
    chunk += "</p>";
    server.sendContent(chunk);
    
    chunk = "<div id='manualDoseSection2'>";
    chunk += "<button class='card-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#28a745;color:#fff;border:none;border-radius:6px;margin-bottom:10px;' onclick='showManualDose2()'>Manual Dose</button>";
    chunk += "</div>";
    chunk += "<button onclick=\"location.href='/newUI/manageChannel?channel=2'\">Manage Channel 2</button>";
    chunk += "</div>";
    server.sendContent(chunk);
    
    // System Time and Actions
    chunk = "<div class='card'>";
    chunk += "<button onclick=\"location.href='/newUI/systemSettings'\">System Settings</button>";
    chunk += "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;'><span style='font-size:0.95em;color:#666;'>System Time:</span><span style='font-size:0.95em;color:#333;'>" + getFormattedTime() + "</span></div>";
    chunk += "</div>";
    server.sendContent(chunk);
    
    // Generate footer
    chunk = generateFooter();
    server.sendContent(chunk);
    
    // JavaScript
    chunk = "<script>\n";
    chunk += "function showManualDose1() {\n";
    chunk += "  var s = document.getElementById('manualDoseSection1');\n";
    chunk += "  s.innerHTML = `<div style='display:flex;gap:8px;align-items:center;justify-content:center;'><input id='doseVol1' type='number' min='0.1' step='0.1' placeholder='Volume (ml)' style='width:40%;padding:8px;font-size:1em;border-radius:6px;border:1px solid #ccc;'><button id='doseBtn1' style=\"width:25%;padding:10px 0;font-size:1em;background:#007BFF;color:#fff;border:none;border-radius:6px;\" onclick='doseNow1()'>Dose</button><button id='cancelBtn1' style=\"width:25%;padding:10px 0;font-size:1em;background:#aaa;color:#fff;border:none;border-radius:6px;\" onclick='cancelManualDose1()'>Cancel</button></div><div id='doseCountdown1' style='margin-top:8px;font-size:1.1em;color:#007BFF;'></div>`;\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function showManualDose2() {\n";
    chunk += "  var s = document.getElementById('manualDoseSection2');\n";
    chunk += "  s.innerHTML = `<div style='display:flex;gap:8px;align-items:center;justify-content:center;'><input id='doseVol2' type='number' min='0.1' step='0.1' placeholder='Volume (ml)' style='width:40%;padding:8px;font-size:1em;border-radius:6px;border:1px solid #ccc;'><button id='doseBtn2' style=\"width:25%;padding:10px 0;font-size:1em;background:#007BFF;color:#fff;border:none;border-radius:6px;\" onclick='doseNow2()'>Dose</button><button id='cancelBtn2' style=\"width:25%;padding:10px 0;font-size:1em;background:#aaa;color:#fff;border:none;border-radius:6px;\" onclick='cancelManualDose2()'>Cancel</button></div><div id='doseCountdown2' style='margin-top:8px;font-size:1.1em;color:#007BFF;'></div>`;\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function cancelManualDose1() {\n";
    chunk += "  var s = document.getElementById('manualDoseSection1');\n";
    chunk += "  s.innerHTML = `<button class='card-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#28a745;color:#fff;border:none;border-radius:6px;margin-bottom:10px;' onclick='showManualDose1()'>Manual Dose</button>`;\n";
    chunk += "}\n";
    chunk += "function cancelManualDose2() {\n";
    chunk += "  var s = document.getElementById('manualDoseSection2');\n";
    chunk += "  s.innerHTML = `<button class='card-btn' style='width:100%;padding:12px 0;font-size:1.1em;background:#28a745;color:#fff;border:none;border-radius:6px;margin-bottom:10px;' onclick='showManualDose2()'>Manual Dose</button>`;\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function doseNow1() {\n";
    chunk += "  var vol = parseFloat(document.getElementById('doseVol1').value);\n";
    chunk += "  if (!vol || vol <= 0) { alert('Enter a valid volume'); return; }\n";
    chunk += "  var btn = document.getElementById('doseBtn1');\n";
    chunk += "  var cancel = document.getElementById('cancelBtn1');\n";
    chunk += "  btn.disabled = true; cancel.disabled = true;\n";
    chunk += "  var countdown = document.getElementById('doseCountdown1');\n";
    chunk += "  var duration = Math.ceil(vol * " + String(calibrationFactor1) + " / 1000);\n";
    chunk += "  countdown.innerText = 'Dosing... ' + duration + 's remaining';\n";
    server.sendContent(chunk);
    
    chunk = "  var interval = setInterval(function() {\n";
    chunk += "    duration--;\n";
    chunk += "    countdown.innerText = 'Dosing... ' + duration + 's remaining';\n";
    chunk += "    if (duration <= 0) { clearInterval(interval); countdown.innerText = ''; window.location.reload(); }\n";
    chunk += "  }, 1000);\n";
    chunk += "  var xhr = new XMLHttpRequest();\n";
    chunk += "  xhr.open('POST', '/manual', true);\n";
    chunk += "  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n";
    chunk += "  xhr.send('channel=1&ml=' + encodeURIComponent(vol));\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function doseNow2() {\n";
    chunk += "  var vol = parseFloat(document.getElementById('doseVol2').value);\n";
    chunk += "  if (!vol || vol <= 0) { alert('Enter a valid volume'); return; }\n";
    chunk += "  var btn = document.getElementById('doseBtn2');\n";
    chunk += "  var cancel = document.getElementById('cancelBtn2');\n";
    chunk += "  btn.disabled = true; cancel.disabled = true;\n";
    chunk += "  var countdown = document.getElementById('doseCountdown2');\n";
    chunk += "  var duration = Math.ceil(vol * " + String(calibrationFactor2) + " / 1000);\n";
    chunk += "  countdown.innerText = 'Dosing... ' + duration + 's remaining';\n";
    server.sendContent(chunk);
    
    chunk = "  var interval = setInterval(function() {\n";
    chunk += "    duration--;\n";
    chunk += "    countdown.innerText = 'Dosing... ' + duration + 's remaining';\n";
    chunk += "    if (duration <= 0) { clearInterval(interval); countdown.innerText = ''; window.location.reload(); }\n";
    chunk += "  }, 1000);\n";
    chunk += "  var xhr = new XMLHttpRequest();\n";
    chunk += "  xhr.open('POST', '/manual', true);\n";
    chunk += "  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n";
    chunk += "  xhr.send('channel=2&ml=' + encodeURIComponent(vol));\n";
    chunk += "}\n";
    chunk += "</script>\n";
    chunk += "</body></html>";
    server.sendContent(chunk);
    
    // End the chunked response
    server.sendContent("");
  });

  server.on("/newUI/manageChannel", HTTP_GET, []() {
    int channel = 1;
    if (server.hasArg("channel")) {
      channel = server.arg("channel").toInt();
    }
    // Select channel-specific variables
    String channelName = (channel == 1) ? channel1Name : channel2Name;
    float lastDispensedVolume = (channel == 1) ? lastDispensedVolume1 : lastDispensedVolume2;
    String lastDispensedTime = (channel == 1) ? lastDispensedTime1 : lastDispensedTime2;
    float remainingML = (channel == 1) ? remainingMLChannel1 : remainingMLChannel2;
    
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Send HTML header
    String chunk = "<html><head><title>Channel Management: " + channelName + "</title>";
    chunk += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    chunk += "<style>body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f9; color: #333; } ";
    server.sendContent(chunk);
    
    // CSS in chunks
    chunk = ".card { margin: 20px auto; padding: 20px; max-width: 500px; background: #fff; border-radius: 10px; box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1); } ";
    chunk += ".card h2 { margin-top: 0; color: #007BFF; } .card p { margin: 10px 0; } ";
    server.sendContent(chunk);
    
    chunk = ".card button { display: block; width: 100%; margin: 10px 0; padding: 10px; font-size: 16px; color: #fff; background-color: #007BFF; ";
    chunk += "border: none; border-radius: 5px; cursor: pointer; } .card button:hover { background-color: #0056b3; } ";
    server.sendContent(chunk);
    
    chunk = ".header-action { float:right; margin-top:-8px; } .rename-row { display:flex; gap:8px; } ";
    chunk += ".rename-input { flex:1; padding:8px 12px; font-size:1em; border-radius:4px; border:1px solid #ccc; height: 2.2em; box-sizing: border-box; } ";
    server.sendContent(chunk);
    
    chunk = ".rename-btn { padding:8px 16px; font-size:1em; border-radius:4px; border:none; background:#007BFF; color:#fff; cursor:pointer; } ";
    chunk += ".rename-btn.cancel { background:#aaa; } .rename-row { display:flex; gap:8px; align-items:center; justify-content:center; } ";
    server.sendContent(chunk);
    
    chunk = ".rename-input { flex:1; padding:8px; font-size:1em; border-radius:6px; border:1px solid #ccc; height: 2.2em; box-sizing: border-box; margin:0; } ";
    chunk += ".rename-btn { width:25%; padding:10px 0; font-size:1em; border-radius:6px; border:none; background:#007BFF; color:#fff; cursor:pointer; margin:0; transition: background 0.2s; } ";
    server.sendContent(chunk);
    
    chunk = ".rename-btn.cancel { background:#aaa; transition: background 0.2s; } .rename-btn.cancel:hover { background:#888; } ";
    chunk += "button.cancel { background:#aaa; transition: background 0.2s; } button.cancel:hover { background:#888; } ";
    server.sendContent(chunk);
    
    chunk = ".card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } ";
    chunk += ".card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } ";
    chunk += ".rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }</style>";
    server.sendContent(chunk);
    
    // JavaScript
    chunk = "<script>\n";
    chunk += "function showRenameBox() {\n";
    chunk += "  document.getElementById('rename-row').style.display = 'flex';\n";
    chunk += "  document.getElementById('rename-btn-row').style.display = 'none';\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function cancelRename() {\n";
    chunk += "  document.getElementById('rename-row').style.display = 'none';\n";
    chunk += "  document.getElementById('rename-btn-row').style.display = 'block';\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function saveRename(channel) {\n";
    chunk += "  var newName = document.getElementById('rename-input').value;\n";
    chunk += "  if (!newName) { alert('Name cannot be empty'); return; }\n";
    chunk += "  var xhr = new XMLHttpRequest();\n";
    chunk += "  xhr.open('POST', '/newUI/renameChannel', true);\n";
    chunk += "  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n";
    chunk += "  xhr.onreadystatechange = function() {\n";
    chunk += "    if (xhr.readyState == 4 && xhr.status == 200) { location.reload(); }\n";
    chunk += "  };\n";
    chunk += "  xhr.send('channel=' + channel + '&name=' + encodeURIComponent(newName));\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function showUpdateVolumeBox() {\n";
    chunk += "  document.getElementById('update-volume-row').style.display = 'flex';\n";
    chunk += "  document.getElementById('update-volume-btn-row').style.display = 'none';\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function cancelUpdateVolume() {\n";
    chunk += "  document.getElementById('update-volume-row').style.display = 'none';\n";
    chunk += "  document.getElementById('update-volume-btn-row').style.display = 'inline';\n";
    chunk += "}\n";
    server.sendContent(chunk);
    
    chunk = "function saveUpdateVolume(channel) {\n";
    chunk += "  var newVol = document.getElementById('update-volume-input').value;\n";
    chunk += "  if (!newVol || isNaN(newVol) || Number(newVol) < 0) { alert('Enter a valid volume'); return; }\n";
    chunk += "  var xhr = new XMLHttpRequest();\n";
    chunk += "  xhr.open('POST', '/newUI/updateVolume', true);\n";
    chunk += "  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n";
    chunk += "  xhr.onreadystatechange = function() {\n";
    chunk += "    if (xhr.readyState == 4 && xhr.status == 200) { location.reload(); }\n";
    chunk += "  };\n";
    chunk += "  xhr.send('channel=' + channel + '&volume=' + encodeURIComponent(newVol));\n";
    chunk += "}\n";
    chunk += "</script>\n";
    chunk += "</head><body>";
    server.sendContent(chunk);
    
    // Header
    chunk = generateHeader("Channel Management: " + channelName);
    server.sendContent(chunk);
    
    // Calculate days remaining for this channel
    int daysRemaining = 0;
    float rem = remainingML;
    int dayIdx = (timeClient.getDay() + 6) % 7;
    int simulatedDays = 0;
    WeeklySchedule* wsDays = (channel == 1) ? &weeklySchedule1 : &weeklySchedule2;
    for (int i = 0; i < 365; ++i) {
      int d = (dayIdx + i) % 7;
      if (wsDays->days[d].enabled) {
        float dose = wsDays->days[d].volume;
        if (rem < dose || dose <= 0.0f) break;
        rem -= dose;
        daysRemaining++;
      }
      simulatedDays++;
    }
    bool moreThanYear = false;
    if (rem > 0.0f && simulatedDays == 365) {
      daysRemaining = 366;
      moreThanYear = true;
    }
    
    // Status Card
    chunk = "<div class='card'>";
    chunk += "<h2>Status</h2>";
    chunk += "<p>Last Dosed: " + lastDispensedTime + "</p>";
    chunk += "<p>Last Volume: " + String(lastDispensedVolume) + " ml</p>";
    chunk += "<p>Remaining Volume: <span id='remaining-volume-label'>" + String(remainingML) + " ml (";
    if (moreThanYear) chunk += "More than a year";
    else chunk += String(simulatedDays) + " days";
    chunk += ")</span> ";
    server.sendContent(chunk);
    
    chunk = "<span id='update-volume-btn-row'><button style='margin-left:8px;' onclick=\"showUpdateVolumeBox()\">Update Volume</button></span>";
    chunk += "<span id='update-volume-row' class='rename-row' style='display:none;'>";
    chunk += "<input id='update-volume-input' class='rename-input' type='number' min='0' step='0.01' value='" + String(remainingML) + "'>";
    chunk += "<button class='rename-btn' onclick='saveUpdateVolume(" + String(channel) + ")'>Save</button>";
    chunk += "<button class='rename-btn cancel' onclick='cancelUpdateVolume()'>Cancel</button>";
    chunk += "</span></p>";
    chunk += "</div>";
    server.sendContent(chunk);
    
    // Schedule Card
    chunk = "<div class='card'>";
    chunk += "<h2>Schedule</h2>";
    server.sendContent(chunk);
    
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
      String ampm = (nextHour < 12) ? "AM" : "PM";
      int hour12 = nextHour % 12 == 0 ? 12 : nextHour % 12;
      chunk = "<p>Next Dose: " + String(dayNames[nextDay]) + ", " + String(hour12) + ":" + (nextMinute < 10 ? "0" : "") + String(nextMinute) + " " + ampm + "</p>";
      chunk += "<p>Next Dose Volume: " + String(nextVol) + " ml</p>";
    } else {
      chunk = "<p>Next Dose: N/A</p>";
      chunk += "<p>Next Dose Volume: N/A</p>";
    }
    chunk += "<button onclick=\"location.href='/newUI/manageSchedule?channel=" + String(channel) + "'\">Manage Schedule</button>";
    chunk += "</div>";
    server.sendContent(chunk);
    
    // Actions Card
    chunk = "<div class='card'>";
    chunk += "<button onclick=\"location.href='/prime?channel=" + String(channel) + "'\">Prime Pump</button>";
    chunk += "<button onclick=\"location.href='/calibrate?channel=" + String(channel) + "'\">Calibrate</button>";
    server.sendContent(chunk);
    
    // Rename UI
    chunk = "<div id='rename-btn-row' style='display:block;'><button onclick=\"showRenameBox()\">Rename</button></div>";
    chunk += "<div id='rename-row' class='rename-row' style='display:none;'>";
    chunk += "<input id='rename-input' class='rename-input' type='text' value='" + channelName + "'>";
    chunk += "<button class='rename-btn' onclick='saveRename(" + String(channel) + ")'>Save</button>";
    chunk += "<button class='rename-btn cancel' onclick='cancelRename()'>Cancel</button>";
    chunk += "</div>";
    chunk += "</div>";
    server.sendContent(chunk);
    
    // Back and Home buttons row
    chunk = "<div style='display:flex;gap:10px;max-width:500px;margin:20px auto 0 auto;'>";
    chunk += "<button style='flex:1;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;' onclick=\"window.location.href='/newUI/summary'\">Home</button>";
    chunk += "</div>";
    server.sendContent(chunk);
    
    // Footer
    chunk = generateFooter();
    chunk += "</body></html>";
    server.sendContent(chunk);
    
    // End chunked response
    server.sendContent("");
  });

  // Add endpoint to handle rename POST
  server.on("/newUI/renameChannel", HTTP_POST, []() {
    if (server.hasArg("channel") && server.hasArg("name")) {
      int channel = server.arg("channel").toInt();
      String newName = server.arg("name");
      if (channel == 1) {
        channel1Name = newName;
      } else if (channel == 2) {
        channel2Name = newName;
      }
      savePersistentDataToSPIFFS();
      server.send(200, "application/json", "{\"status\":\"renamed\"}");
    } else {
      server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
    }
  });

  // Add endpoint to handle update volume POST
  server.on("/newUI/updateVolume", HTTP_POST, []() {
    if (server.hasArg("channel") && server.hasArg("volume")) {
      int channel = server.arg("channel").toInt();
      float newVol = server.arg("volume").toFloat();
      if (channel == 1) {
        remainingMLChannel1 = newVol;
      } else if (channel == 2) {
        remainingMLChannel2 = newVol;
      }
      savePersistentDataToSPIFFS();
      server.send(200, "application/json", "{\"status\":\"updated\"}");
    } else {
      server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
    }
  });

  // --- Manage Schedule UI ---
  server.on("/newUI/manageSchedule", HTTP_GET, []() {
    int channel = 1;
    if (server.hasArg("channel")) channel = server.arg("channel").toInt();
    WeeklySchedule* ws = (channel == 2) ? &weeklySchedule2 : &weeklySchedule1;
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    // HTML head and style
    String chunk = "<html><head><title>Manage Schedule: " + ws->channelName + "</title>";
    chunk += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    chunk += "<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;}table{width:100%;max-width:500px;margin:20px auto;border-collapse:collapse;}th,td{padding:8px;text-align:center;}th{background:#007BFF;color:#fff;}tr:nth-child(even){background:#f9f9f9;}input[type=number]{width:70px;}input[type=time]{width:120px;}label{margin-left:8px;}button{margin:8px 4px;padding:10px 20px;font-size:1em;border-radius:5px;border:none;background:#007BFF;color:#fff;cursor:pointer;}button.cancel{background:#aaa;}button:disabled,input:disabled{background:#eee;color:#888;} .card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } .card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } .rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }</style>";
    server.sendContent(chunk);
    // JS chunk
    chunk = "<script>\n";
    chunk += "function copyMondayToOthers() {\n";
    chunk += "  var enabled=document.getElementById('enabled0').checked;\n";
    chunk += "  var time=document.getElementById('time0').value;\n";
    chunk += "  var vol=document.getElementById('vol0').value;\n";
    chunk += "  for(var i=1;i<7;i++){\n";
    chunk += "    document.getElementById('enabled'+i).checked=enabled;\n";
    chunk += "    document.getElementById('time'+i).value=time;\n";
    chunk += "    document.getElementById('vol'+i).value=vol;\n";
    chunk += "    document.getElementById('enabled'+i).disabled=true;\n";
    chunk += "    document.getElementById('time'+i).disabled=true;\n";
    chunk += "    document.getElementById('vol'+i).disabled=true;\n";
    chunk += "  }\n";
    chunk += "}\n";
    chunk += "function uncopyMonday() {\n";
    chunk += "  for(var i=1;i<7;i++){\n";
    chunk += "    document.getElementById('enabled'+i).disabled=false;\n";
    chunk += "    document.getElementById('time'+i).disabled=false;\n";
    chunk += "    document.getElementById('vol'+i).disabled=false;\n";
    chunk += "  }\n";
    chunk += "}\n";
    chunk += "function onCopyChange(cb){if(cb.checked){copyMondayToOthers();}else{uncopyMonday();}}\n";
    chunk += "window.addEventListener('DOMContentLoaded',function(){\n";
    chunk += "  document.getElementById('enabled0').addEventListener('change',function(){if(document.getElementById('copyMonday').checked){copyMondayToOthers();}});\n";
    chunk += "  document.getElementById('time0').addEventListener('change',function(){if(document.getElementById('copyMonday').checked){copyMondayToOthers();}});\n";
    chunk += "  document.getElementById('vol0').addEventListener('input',function(){if(document.getElementById('copyMonday').checked){copyMondayToOthers();}});\n";
    chunk += "});\n";
    chunk += "</script>\n";
    chunk += "</head><body>";
    server.sendContent(chunk);
    // Header and card open
    chunk = generateHeader("Manage Schedule : " + ws->channelName);
    chunk += "<div class='card' style='margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);'>";
    chunk += "<form id='scheduleForm' method='POST' action='/newUI/manageSchedule?channel=" + String(channel) + "'>";
    chunk += "<table style='width:100%;border-collapse:collapse;'>";
    chunk += "<tr style='background:#007BFF;color:#fff;'><th>Day</th><th>Enabled</th><th>Time</th><th>Volume (ml)</th></tr>";
    server.sendContent(chunk);
    // Table rows chunked
    for (int i = 0; i < 7; ++i) {
      String rowShade = (i % 2 == 0) ? "background:#f9f9f9;" : "background:#fff;";
      chunk = "<tr style='" + rowShade + "'>";
      chunk += "<td>" + String(dayNames[i]) + "</td>";
      chunk += "<td><input type='checkbox' id='enabled" + String(i) + "' name='enabled" + String(i) + "'" + (ws->days[i].enabled ? " checked" : "") + "></td>";
      char timebuf[6];
      snprintf(timebuf, sizeof(timebuf), "%02d:%02d", ws->days[i].hour, ws->days[i].minute);
      chunk += "<td><input type='time' id='time" + String(i) + "' name='time" + String(i) + "' value='" + String(timebuf) + "'></td>";
      chunk += "<td><input type='number' id='vol" + String(i) + "' name='vol" + String(i) + "' step='0.01' min='0' value='" + String(ws->days[i].volume, 2) + "'></td>";
      chunk += "</tr>";
      server.sendContent(chunk);
    }
    // After table
    chunk = "</table>";
    chunk += "<div style='margin:16px 0 0 0;'><input type='checkbox' id='copyMonday' name='copyMonday' onchange='onCopyChange(this)'><label for='copyMonday' style='margin-left:8px;'>All day as Monday</label></div>";
    chunk += String("<div style='max-width:500px;margin:20px auto;'><input type='checkbox' id='missedDose' name='missedDose'") + (ws->missedDoseCompensation ? " checked" : "") + "><label for='missedDose'>Missed Dose Compensation</label></div>";
    chunk += "<div style='display:flex;flex-direction:column;gap:10px;margin-top:20px;'>";
    chunk += "<button type='submit' style='width:100%;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;'>Save</button>";
    chunk += "<button type='button' id='cancelBtn' class='cancel' style='width:100%;padding:12px 0;font-size:1.1em;background:#aaa;color:#fff;border:none;border-radius:6px;'>Cancel</button>";
    chunk += "</div>";
    chunk += "</form>";
    chunk += "</div>";
    server.sendContent(chunk);
    // Footer and scripts
    chunk = generateFooter();
    chunk += "<script>\n";
    chunk += "document.getElementById('scheduleForm').addEventListener('submit',function(e){\n";
    chunk += "  if(document.getElementById('copyMonday').checked){\n";
    chunk += "    var enabled=document.getElementById('enabled0').checked;\n";
    chunk += "    var time=document.getElementById('time0').value;\n";
    chunk += "    var vol=document.getElementById('vol0').value;\n";
    chunk += "    for(var i=1;i<7;i++){document.getElementById('enabled'+i).disabled=false;document.getElementById('time'+i).disabled=false;document.getElementById('vol'+i).disabled=false;document.getElementById('enabled'+i).checked=enabled;document.getElementById('time'+i).value=time;document.getElementById('vol'+i).value=vol;}\n";
    chunk += "  }\n";
    chunk += "});\n";
    chunk += "document.getElementById('cancelBtn').addEventListener('click',function(){\n";
    chunk += "  var url = new URL(window.location.href);\n";
    chunk += "  var channel = url.searchParams.get('channel') || '1';\n";
    chunk += "  window.location.href = '/newUI/manageChannel?channel=' + channel;\n";
    chunk += "});\n";
    chunk += "</script>\n";
    chunk += "</form></body></html>";
    server.sendContent(chunk);
    // End chunked response
    server.sendContent("");
  });

  server.on("/newUI/manageSchedule", HTTP_POST, []() {
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
    saveWeeklySchedulesToSPIFFS();
    server.sendHeader("Location", "/newUI/manageChannel?channel=" + String(channel));
    server.send(302, "text/plain", "");
  });

  server.on("/newUI/systemSettings", HTTP_GET, []() {
    // Get MAC address for default device name
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String defaultDeviceName = "Doser_" + mac;
    if (deviceName == "") deviceName = defaultDeviceName;
    // Use global ntfyChannel
    // Load current values (replace with actual persistent values if available)
    bool notifyLowFert = true;
    bool notifyStart = false;
    bool notifyDose = false;
    // Calibration factors
    float calib1 = calibrationFactor1;
    float calib2 = calibrationFactor2;
    
    // Start chunked response
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // HTML head
    String chunk = "<html><head><title>System Settings</title>";
    chunk += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    chunk += "<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;} ";
    server.sendContent(chunk);
    
    // CSS in chunks
    chunk = ".card{margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);} ";
    chunk += ".card h2{margin-top:0;color:#007BFF;} .form-row{margin-bottom:16px;} ";
    chunk += "label{display:block;margin-bottom:6px;font-weight:500;} ";
    server.sendContent(chunk);
    
    chunk = "input[type=text],input[type=number],input[type=password],select{width:100%;padding:10px;font-size:1.1em;border-radius:6px;border:1px solid #ccc;box-sizing:border-box;} ";
    chunk += ".section-title{font-size:1.1em;font-weight:600;margin:18px 0 8px 0;color:#007BFF;} ";
    server.sendContent(chunk);
    
    chunk = ".checkbox-row{display:flex;align-items:center;gap:10px;margin-bottom:8px;} ";
    chunk += ".btn-row{display:flex;gap:10px;margin-top:18px;} ";
    chunk += ".btn{flex:1;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;cursor:pointer;transition:background 0.2s;} ";
    server.sendContent(chunk);
    
    chunk = ".btn.cancel{background:#aaa;} .btn.danger{background:#dc3545;} .btn.update{background:#28a745;} ";
    chunk += ".btn:hover{background:#0056b3;} .btn.cancel:hover{background:#888;} .btn.danger:hover{background:#b30000;} .btn.update:hover{background:#218838;} ";
    server.sendContent(chunk);
    
    chunk = "</style></head><body>";
    server.sendContent(chunk);
    
    // Header
    chunk = generateHeader("System Settings");
    server.sendContent(chunk);
    
    // Form start and device settings
    chunk = "<form method='POST' action='/newUI/systemSettings'>";
    chunk += "<div class='card'>";
    chunk += "<div class='form-row'><label for='deviceName'>Device Name:</label><input type='text' id='deviceName' name='deviceName' value='" + deviceName + "'></div>";
    server.sendContent(chunk);

    // Timezone dropdown
    chunk = "<div class='form-row'><label for='timezone'>Time Zone:</label><select name='timezone'>";
    int tzOffsets[] = {-43200,-39600,-36000,-32400,-28800,-25200,-21600,-18000,-14400,-10800,-7200,-3600,0,3600,7200,10800,14400,18000,19800,21600,25200,28800,32400,36000,39600,43200};
    String tzLabels[] = {"UTC-12:00","UTC-11:00","UTC-10:00","UTC-09:00","UTC-08:00 (PST)","UTC-07:00 (MST)","UTC-06:00 (CST)","UTC-05:00 (EST)","UTC-04:00","UTC-03:00","UTC-02:00","UTC-01:00","UTC+00:00","UTC+01:00","UTC+02:00","UTC+03:00","UTC+04:00","UTC+05:00","UTC+05:30 (IST)","UTC+06:00","UTC+07:00","UTC+08:00","UTC+09:00 (JST)","UTC+10:00","UTC+11:00","UTC+12:00"};
    server.sendContent(chunk);
    
    // Send timezone options in smaller chunks
    for (int i = 0; i < 26; i += 5) {
      chunk = "";
      for (int j = i; j < i + 5 && j < 26; j++) {
        chunk += "<option value='" + String(tzOffsets[j]) + "'" + (timezoneOffset == tzOffsets[j] ? " selected" : "") + ">" + tzLabels[j] + "</option>";
      }
      server.sendContent(chunk);
    }
    
    chunk = "</select></div>";
    chunk += "<div class='section-title'>Calibration Factor</div>";
    chunk += "<div class='form-row'>Channel 1: <span style='font-weight:600;'>" + String(calib1, 2) + "</span></div>";
    chunk += "<div class='form-row'>Channel 2: <span style='font-weight:600;'>" + String(calib2, 2) + "</span></div>";
    server.sendContent(chunk);
    
    // Notifications section
    chunk = "<div class='section-title'>Notifications</div>";
    // NTFY Channel as read-only, default to MAC if empty
    String ntfyDefault = mac;
    chunk += "<div class='form-row'><label for='ntfyChannel'>NTFY Channel:</label><input type='text' id='ntfyChannel' name='ntfyChannel' value='" + ntfyDefault + "' readonly></div>";
    chunk += "<div class='form-row'>Events to Notify:</div>";
    server.sendContent(chunk);
    
    chunk = "<div class='checkbox-row'><input type='checkbox' id='notifyLowFert' name='notifyLowFert' checked><label for='notifyLowFert'>Low Fertilizer Volume</label></div>";
    chunk += "<div class='checkbox-row'><input type='checkbox' id='notifyStart' name='notifyStart' " + String(notifyStart ? "checked" : "") + "><label for='notifyStart'>System Start</label></div>";
    chunk += "<div class='checkbox-row'><input type='checkbox' id='notifyDose' name='notifyDose' " + String(notifyDose ? "checked" : "") + "><label for='notifyDose'>Dose</label></div>";
    server.sendContent(chunk);
    
    // Buttons
    chunk = "<div class='btn-row'>";
    chunk += "<button type='submit' class='btn btn-main'>Save</button>";
    chunk += "<button type='button' class='btn btn-cancel' onclick=\"window.location.href='/newUI/summary'\">Cancel</button>";
    chunk += "</div></div></form>"; // Close main form and card
    server.sendContent(chunk);

    // Action buttons outside the main form
    chunk = "<div class='btn-row card-action-row'>";
    chunk += "<form style='display:inline;'><button type='button' class='btn btn-update' style='min-width:120px;' onclick=\"showFirmwareUpdate()\">FW Update</button></form>";
    chunk += "<form method='POST' action='/restart' style='display:inline;'><button type='submit' class='btn btn-main'>Restart</button></form>";
    chunk += "<form method='POST' action='/wifiReset' style='display:inline;'><button type='submit' class='btn btn-danger' onclick=\"return confirm('Reset WiFi settings? Device will reboot in AP mode.')\">WiFi Reset</button></form>";
    chunk += "<form method='POST' action='/factoryReset' style='display:inline;'><button type='submit' class='btn btn-danger' onclick=\"return confirm('Factory reset will erase ALL data. Are you sure?')\">Factory Reset</button></form>";
    chunk += "</div>";
    server.sendContent(chunk);

    // Update CSS for button consistency and centering
    chunk = "<style> ";
    chunk += ".btn-row.card-action-row { display: flex; justify-content: center; align-items: center; gap: 10px; max-width: 500px; margin: 0 auto 16px auto; } ";
    chunk += ".btn-row.card-action-row .btn { flex: unset; min-width: 120px; } ";
    chunk += ".btn { padding:12px 0; font-size:1.1em; border:none; border-radius:6px; cursor:pointer; transition:background 0.2s; min-width:120px; margin:0 4px 8px 0; } ";
    chunk += ".btn-main { background:#007BFF; color:#fff; } ";
    chunk += ".btn-cancel { background:#aaa; color:#fff; } ";
    chunk += ".btn-danger { background:#dc3545; color:#fff; } ";
    chunk += ".btn-update { background:#28a745; color:#fff; } ";
    chunk += ".btn-main:hover { background:#0056b3; } ";
    chunk += ".btn-cancel:hover { background:#888; } ";
    chunk += ".btn-danger:hover { background:#b30000; } ";
    chunk += ".btn-update:hover { background:#218838; } ";
    chunk += "</style>";
    server.sendContent(chunk);

    // Firmware update section
    chunk = "<div id='firmwareUpdateSection' style='display:none;margin-top:20px;'>";
    chunk += "<div class='card'>";
    chunk += "<h3>FW Update</h3>";
    chunk += "<div class='form-row'><label for='firmwareUrl'>Firmware URL:</label>";
    chunk += "<input type='text' id='firmwareUrl' value='ABC.com/fw.bin' style='width:100%;padding:10px;font-size:1.1em;border-radius:6px;border:1px solid #ccc;'></div>";
    server.sendContent(chunk);
    
    chunk = "<div class='btn-row'>";
    chunk += "<button type='button' class='btn btn-update' onclick=\"updateFirmware()\">Update</button>";
    chunk += "<button type='button' class='btn btn-cancel' onclick=\"hideFirmwareUpdate()\">Cancel</button>";
    chunk += "</div>";
    server.sendContent(chunk);
    
    chunk = "<div id='updateProgress' style='margin-top:10px;display:none;'>";
    chunk += "<div>Downloading firmware...</div>";
    chunk += "<div id='progressBar' style='width:100%;background:#ddd;border-radius:6px;margin-top:5px;'>";
    chunk += "<div id='progressFill' style='width:0%;height:20px;background:#007BFF;border-radius:6px;transition:width 0.3s;'></div>";
    chunk += "</div><div id='progressText'>0%</div></div></div></div>";
    server.sendContent(chunk);
    
    // JavaScript in smaller chunks
    chunk = "<script>";
    chunk += "function showFirmwareUpdate() {";
    chunk += "  document.getElementById('firmwareUpdateSection').style.display = 'block';";
    chunk += "}";
    chunk += "function hideFirmwareUpdate() {";
    chunk += "  document.getElementById('firmwareUpdateSection').style.display = 'none';";
    chunk += "  document.getElementById('updateProgress').style.display = 'none';";
    chunk += "}";
    server.sendContent(chunk);
    
    chunk = "async function updateFirmware() {";
    chunk += "  const url = document.getElementById('firmwareUrl').value;";
    chunk += "  if (!url) { alert('Please enter firmware URL'); return; }";
    chunk += "  document.getElementById('updateProgress').style.display = 'block';";
    chunk += "  const progressFill = document.getElementById('progressFill');";
    chunk += "  const progressText = document.getElementById('progressText');";
    server.sendContent(chunk);
    
    chunk = "  try {";
    chunk += "    const response = await fetch(url);";
    chunk += "    if (!response.ok) throw new Error('Failed to download firmware');";
    chunk += "    const contentLength = response.headers.get('content-length');";
    chunk += "    const total = parseInt(contentLength, 10);";
    chunk += "    let loaded = 0; const reader = response.body.getReader(); const chunks = [];";
    server.sendContent(chunk);
    
    chunk = "    while (true) {";
    chunk += "      const { done, value } = await reader.read();";
    chunk += "      if (done) break;";
    chunk += "      chunks.push(value); loaded += value.length;";
    chunk += "      if (total) {";
    chunk += "        const progress = (loaded / total) * 100;";
    chunk += "        progressFill.style.width = progress + '%';";
    chunk += "        progressText.textContent = Math.round(progress) + '%';";
    chunk += "      }}";
    server.sendContent(chunk);
    
    chunk = "    const firmwareData = new Uint8Array(loaded);";
    chunk += "    let offset = 0;";
    chunk += "    for (const chunk of chunks) {";
    chunk += "      firmwareData.set(chunk, offset); offset += chunk.length;";
    chunk += "    }";
    chunk += "    progressText.textContent = 'Flashing firmware...';";
    chunk += "    const formData = new FormData();";
    chunk += "    formData.append('firmware', new Blob([firmwareData]), 'firmware.bin');";
    server.sendContent(chunk);
    
    chunk = "    const uploadResponse = await fetch('/update', {";
    chunk += "      method: 'POST', body: formData";
    chunk += "    });";
    chunk += "    if (uploadResponse.ok) {";
    chunk += "      progressText.textContent = 'Firmware updated successfully! Device will restart...';";
    chunk += "      setTimeout(() => { window.location.href = '/newUI/summary'; }, 3000);";
    chunk += "    } else { throw new Error('Failed to flash firmware'); }";
    server.sendContent(chunk);
    
    chunk = "  } catch (error) {";
    chunk += "    alert('Firmware update failed: ' + error.message);";
    chunk += "    hideFirmwareUpdate();";
    chunk += "  }";
    chunk += "}";
    chunk += "</script>";
    server.sendContent(chunk);
    
    // Footer
    chunk = generateFooter();
    chunk += "</body></html>";
    server.sendContent(chunk);
    
    // End chunked response
    server.sendContent("");
  });

  server.on("/restart", HTTP_POST, handleRestartOnly);
  server.on("/wifiReset", HTTP_POST, handleWiFiReset);
  server.on("/factoryReset", HTTP_POST, handleFactoryReset);
  server.on("/newUI/systemSettings", HTTP_POST, handleSystemSettingsSave);

  server.begin();
}

void handleCalibration() {
  if (server.hasArg("channel")) {
    int channel = server.arg("channel").toInt();
    
    // If we have the dispensed amount, complete calibration
    if (server.hasArg("dispensedML")) {
      float dispensedML = server.arg("dispensedML").toFloat();
      float &calibrationFactor = (channel == 1) ? calibrationFactor1 : calibrationFactor2;
      calibrationFactor = 15000.0 / dispensedML;
      savePersistentDataToSPIFFS();
      // Show toast and redirect to channel management
      String html = "<html><head><meta http-equiv='refresh' content='2;url=/newUI/manageChannel?channel=" + String(channel) + "'>";
      html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
      html += "<style>.toast{position:fixed;top:30px;left:50%;transform:translateX(-50%);background:#28a745;color:#fff;padding:18px 32px;border-radius:8px;font-size:1.2em;box-shadow:0 2px 8px rgba(0,0,0,0.15);z-index:9999;}</style>";
      html += "</head><body>";
      html += "<div class='toast'>Calibration complete!</div>";
      html += "<script>setTimeout(function(){window.location.href='/newUI/manageChannel?channel=" + String(channel) + "';},1800);</script>";
      html += "</body></html>";
      server.send(200, "text/html", html);
      return;
    }
    
    // First phase - run the motor and show input form
    if (channel == 1 || channel == 2) {
      // Run motor for exactly 15 seconds
      runMotor(channel, 15000);
      
      // Show form to input dispensed amount
      String html = "";
      html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
      html += "<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;} .card{margin:20px auto;padding:20px;max-width:500px;background:#fff;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);} .card h2{margin-top:0;color:#007BFF;} .calib-label{font-size:1.1em;margin-bottom:8px;display:block;} .calib-input{width:100%;padding:10px;font-size:1.1em;border-radius:6px;border:1px solid #ccc;margin-bottom:16px;} .calib-submit{width:100%;padding:14px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;cursor:pointer;} .home-btn{width:100%;padding:12px 0;font-size:1.1em;background:#007BFF;color:#fff;border:none;border-radius:6px;} .back-btn{width:100%;padding:12px 0;font-size:1.1em;background:#aaa;color:#fff;border:none;border-radius:6px;margin-top:10px;} .card button, .card-btn, .dispense-btn, .calib-btn, .prime-btn, .home-btn, .back-btn, .rename-btn, button.cancel { transition: background 0.2s; } .card button:hover, .card-btn:hover, .dispense-btn:hover, .calib-btn:hover, .prime-btn:hover, .home-btn:hover, .rename-btn:hover { background-color: #0056b3 !important; } .prime-btn.stop:hover { background-color: #218838 !important; } .rename-btn.cancel:hover, button.cancel:hover, .back-btn:hover { background-color: #888 !important; }</style>";
      html += "</head><body>";
      html += generateHeader("Calibration Measurement");
      html += "<div class='card'>";
     // html += "<h2>Calibration Measurement</h2>";
      html += "<p style='margin-bottom:18px;'>Motor has run for 15 seconds. Please measure the dispensed liquid and enter the amount below:</p>";
      html += "<form action='/calibrate' method='POST'>";
      html += "<input type='hidden' name='channel' value='" + String(channel) + "'>";
      html += "<label for='dispensedML' class='calib-label'>Amount dispensed (ml):</label>";
      html += "<input type='number' name='dispensedML' step='0.1' required class='calib-input'><br>";
      html += "<button type='submit' class='calib-submit'>Submit Measurement</button>";
      html += "</form>";
      html += "<button class='home-btn' onclick=\"window.location.href='/newUI/summary'\">Home</button>";
      html += "<button class='back-btn' onclick=\"history.back()\">Back</button>";
      html += "</div>";
      html += generateFooter();
      html += "</body></html>";
      server.send(200, "text/html", html);
      return;
    }
  }
  
  server.send(400, "application/json", "{\"error\":\"missing or invalid parameters\"}");
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
      Serial.print("[MANUAL DOSE] lastDispensedVolume1 set: "); Serial.println(lastDispensedVolume1);
      Serial.print("[MANUAL DOSE] lastDispensedTime1 set: "); Serial.println(lastDispensedTime1);
    } else if (channel == 2) {
      lastDispensedVolume2 = ml;
      lastDispensedTime2 = getFormattedTime();
      Serial.print("[MANUAL DOSE] lastDispensedVolume2 set: "); Serial.println(lastDispensedVolume2);
      Serial.print("[MANUAL DOSE] lastDispensedTime2 set: "); Serial.println(lastDispensedTime2);
    }
    savePersistentDataToSPIFFS();
    Serial.println("[MANUAL DOSE] savePersistentDataToSPIFFS called");
    server.send(200, "application/json", "{\"status\":\"dispensed\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
  }
}

void handleDailyDispense() {
  if (server.hasArg("channel") && server.hasArg("hour") && server.hasArg("minute") && server.hasArg("ml")) {
    int channel = server.arg("channel").toInt();
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    float ml = server.arg("ml").toFloat();

    // Save the updated schedule
    savePersistentDataToSPIFFS();

    server.send(200, "application/json", "{\"status\":\"schedule set\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
  }
}

void checkDailyDispense() {
  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  
  // Track if we need to run either channel
  bool runCh1 = false;
  bool runCh2 = false;
  
  // If both channels need to run, indicate this with LED
  if (runCh1 && runCh2) {
    blinkLED(LED_PURPLE, 2);  // Blink purple twice to indicate dual channel operation
  }
  
  // Run channel 1 if needed
  if (runCh1) {
    updateRemainingML(1, 0);
   
    lastDispensedVolume1 = 0;
    lastDispensedTime1 = getFormattedTime();
    Serial.print("[SCHEDULED DOSE] lastDispensedVolume1 set: "); Serial.println(lastDispensedVolume1);
    Serial.print("[SCHEDULED DOSE] lastDispensedTime1 set: "); Serial.println(lastDispensedTime1);
    savePersistentDataToSPIFFS();
    Serial.println("[SCHEDULED DOSE] savePersistentDataToSPIFFS called for channel 1");
  }
  
  // Run channel 2 if needed
  if (runCh2) {
    updateRemainingML(2, 0);
   
    lastDispensedVolume2 = 0;
    lastDispensedTime2 = getFormattedTime();
    Serial.print("[SCHEDULED DOSE] lastDispensedVolume2 set: "); Serial.println(lastDispensedVolume2);
    Serial.print("[SCHEDULED DOSE] lastDispensedTime2 set: "); Serial.println(lastDispensedTime2);
    savePersistentDataToSPIFFS();
    Serial.println("[SCHEDULED DOSE] savePersistentDataToSPIFFS called for channel 2");
  }
}

void setupTimeSync() {
  timeClient.begin();
  timeClient.setTimeOffset(timezoneOffset);
  Serial.println("Syncing time...");
  
  int retries = 0;
  while (!timeClient.update() && retries < 10) {
    timeClient.forceUpdate();
    retries++;
    delay(500);
  }
  
  if (retries >= 10) {
    Serial.println("Time sync failed!");
  } else {
    Serial.println("Time synced successfully");
    Serial.println("Current time: " + getFormattedTime());
  }
}

#define JSON_BUFFER_SIZE 1024

// JSON handling functions updated to use JsonDocument
void loadPersistentDataFromSPIFFS() {
  File file = LittleFS.open("/data.json", "r");
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }

  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println("Failed to parse JSON");
    return;
  }

  // Load channel names
  channel1Name = doc["name1"] | "Channel 1";
  channel2Name = doc["name2"] | "Channel 2";

  // Load remaining ML values
  remainingMLChannel1 = doc["channel1"].as<float>();
  remainingMLChannel2 = doc["channel2"].as<float>();
  timezoneOffset = doc["timezone"] | 0;  // Default to UTC if not set

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

  file.close();
  Serial.println("Loaded configuration from filesystem");
}

// JSON handling functions updated to use JsonDocument
void savePersistentDataToSPIFFS() {
  File file = LittleFS.open("/data.json", "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
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

  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to write JSON to file");
  }

  file.close();
  Serial.println("Saved configuration to filesystem");
}

void handleBottleTracking() {
  if (server.hasArg("channel") && server.hasArg("ml")) {
    int channel = server.arg("channel").toInt();
    float ml = server.arg("ml").toFloat();

    if (channel == 1) {
      remainingMLChannel1 = ml;
    } else if (channel == 2) {
      remainingMLChannel2 = ml;
    }

    savePersistentDataToSPIFFS();
    server.send(200, "application/json", "{\"status\":\"bottle updated\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
  }
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
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.begin();
  Serial.println("OTA Ready");
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
    
    String msg = String("{\"status\":\"prime pump ") + (state ? "started" : "stopped") + "\"}";
    server.send(200, "application/json", msg);
  } else {
    server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
  }
}

// Common header and footer generators
String generateHeader(String title) {
  String html = "<div style='width:100%;background:#007BFF;color:#fff;padding:16px 0;text-align:center;font-size:1.5em;border-radius:10px 10px 0 0;box-shadow:0 2px 4px rgba(0,0,0,0.05);margin-bottom:10px;'>";
  html += title;
  html += "</div>";
  return html;
}

String generateFooter() {
  String html = "<div style='width:100%;background:#f1f1f1;color:#333;padding:10px 0;text-align:center;font-size:1em;border-radius:0 0 10px 10px;box-shadow:0 -2px 4px rgba(0,0,0,0.03);margin-top:20px;'>";
  html += "S/W version : 1.0  Support : mymail.arjun@gmail.com";
  html += "<br>Available RAM: " + String(ESP.getFreeHeap() / 1024.0, 2) + " KB";
  html += "</div>";
  return html;
}

void handleRestartOnly() {
  
 Serial.println("Restarting system...");

  // Give browser time to receive response, then restart
  delay(500);
  ESP.restart();
}

void handleWiFiReset() {
  // Clear WiFiManager credentials
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  
  // Restart ESP to enter AP mode
  ESP.restart();
}

void handleFactoryReset() {
  // Format the entire filesystem to remove all files and orphans
  LittleFS.format();
  // Clear WiFiManager credentials
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  // Restart ESP
  ESP.restart();
}

void handleSystemSettingsSave() {
  bool updated = false;
  
  // Save timezone if provided
  if (server.hasArg("timezone")) {
    int newTimezone = server.arg("timezone").toInt();
    if (newTimezone != timezoneOffset) {
      timezoneOffset = newTimezone;
      timeClient.setTimeOffset(timezoneOffset);
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

  // Save other settings here as needed (device name, NTFY settings, etc.)
  
  if (updated) {
    savePersistentDataToSPIFFS();
  }
  // Redirect to summary page after saving
  server.sendHeader("Location", "/newUI/summary");
  server.send(302, "text/plain", "");
}

