#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <Ticker.h>
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
  char timeString[32];  // Buffer of 32 bytes
  snprintf(timeString, sizeof(timeString), "%02d:%02d:%02d %02d/%02d/%04d", 
          ptm->tm_hour, ptm->tm_min, ptm->tm_sec,
          ptm->tm_mday, ptm->tm_mon+1, ptm->tm_year+1900);
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
Ticker ledTicker;

// Channel names
String channel1Name = "Channel 1";
String channel2Name = "Channel 2";

// Calibration Variables
float calibrationFactor1 = 1;
float calibrationFactor2 = 1;

struct DailySchedule {
  int hour;
  int minute;
  float ml;
};

DailySchedule channel1Schedule = {0, 0, 0.0};
DailySchedule channel2Schedule = {0, 0, 0.0};

// Persistent Storage Variables
float remainingMLChannel1 = 0.0;
float remainingMLChannel2 = 0.0;

// MQTT Variables
WiFiClient espClient;
PubSubClient mqttClient(espClient);
const char* mqttServer = "homeassistant.local"; // Example public broker
const int mqttPort = 1883;
const char* mqttTopicLastDosed = "doser/lastDosed";
const char* mqttTopicRemainingML = "doser/remainingML";
const char* mqttTopicManualDose = "doser/manualDose";

// Add timezone offset to global variables
int32_t timezoneOffset = 0;  // Default to UTC

// Add these variables after other global declarations
int lastDispenseHour1 = -1;
int lastDispenseMinute1 = -1;
int lastDispenseHour2 = -1;
int lastDispenseMinute2 = -1;
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
void setupMQTT();
void handleMQTTConnection();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishLastDosed(int channel, float ml);
void publishRemainingML();
void handleSystemReset();
void setupOTA();
void blinkLED(uint32_t color, int times);
void runMotor(int channel, int durationMs);
void updateLEDState();
String getFormattedTime(); 

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

  // Setup WiFi
  setupWiFi();

  // Setup Web Server
  setupWebServer();

  // Setup Time Sync
  setupTimeSync();

  // Setup MQTT
  //setupMQTT();

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

  // Handle MQTT
  //mqttClient.loop();
  //handleMQTTConnection();

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
  server.on("/", HTTP_GET, []() {
    String html = "<html><head>";
    html += "<title>Doser Control</title>";
    html += "<meta http-equiv='refresh' content='5'>"; // Refresh page every 5 seconds
    html += "</head><body>";
    html += "<h1>Doser Control</h1>";
    html += "<h2>Current Time: " + getFormattedTime() + "</h2>";
    html += "<p><a href='/names'>Set Channel Names</a></p>";
    html += "<p><a href='/calibrate'>Calibrate</a></p>";
    html += "<p><a href='/manual'>Manual Dispense</a></p>";
    html += "<p><a href='/daily'>Set Daily Schedule</a></p>";
    html += "<p><a href='/bottle'>Set Bottle Capacity</a></p>";
    html += "<p><a href='/prime'>Prime Pump</a></p>";  // Added Prime Pump link
    html += "<p><a href='/timezone'>Set Timezone</a></p>";
    html += "<p><a href='/view'>View Parameters</a></p>";
    html += "<p><a href='/reset'>System Reset</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/calibrate", HTTP_GET, []() {
    String html = "<html><head><title>Calibrate</title></head><body>";
    html += "<h1>Calibrate</h1>";
    html += "<form action='/calibrate' method='POST'>";
    html += "<label for='channel'>Channel:</label>";
    html += "<select name='channel'>";
    html += "<option value='1'>" + channel1Name + "</option>";
    html += "<option value='2'>" + channel2Name + "</option>";
    html += "</select><br><br>";
    html += "<input type='submit' value='Calibrate'>";
    html += "</form>";
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/manual", HTTP_GET, []() {
    String html = "<html><head><title>Manual Dispense</title></head><body>";
    html += "<h1>Manual Dispense</h1>";
    html += "<form action='/manual' method='POST'>";
    html += "<label for='channel'>Channel:</label>";
    html += "<select name='channel'>";
    html += "<option value='1'>" + channel1Name + "</option>";
    html += "<option value='2'>" + channel2Name + "</option>";
    html += "</select><br><br>";
    html += "<label for='ml'>Amount (ml):</label>";
    html += "<input type='number' name='ml' step='0.1'><br><br>";
    html += "<input type='submit' value='Dispense'>";
    html += "</form>";
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/daily", HTTP_GET, []() {
    String html = "<html><head><title>Set Daily Schedule</title></head><body>";
    html += "<h1>Set Daily Schedule</h1>";
    
    // Show current schedules
    html += "<h2>Current Schedules:</h2>";
    html += "<p>" + channel1Name + ": " + String(channel1Schedule.hour) + ":" + 
            String(channel1Schedule.minute, 2) + " - " + String(channel1Schedule.ml) + "ml</p>";
    html += "<p>" + channel2Name + ": " + String(channel2Schedule.hour) + ":" + 
            String(channel2Schedule.minute, 2) + " - " + String(channel2Schedule.ml) + "ml</p>";
    
    // Form to set new schedule
    html += "<h2>Set New Schedule:</h2>";
    html += "<form action='/daily' method='POST'>";
    html += "<label for='channel'>Channel:</label>";
    html += "<select name='channel'>";
    html += "<option value='1'>" + channel1Name + "</option>";
    html += "<option value='2'>" + channel2Name + "</option>";
    html += "</select><br><br>";
    html += "<label for='hour'>Hour:</label>";
    html += "<input type='number' name='hour' min='0' max='23'><br><br>";
    html += "<label for='minute'>Minute:</label>";
    html += "<input type='number' name='minute' min='0' max='59'><br><br>";
    html += "<label for='ml'>Amount (ml):</label>";
    html += "<input type='number' name='ml' step='0.1'><br><br>";
    html += "<input type='submit' value='Set Schedule'>";
    html += "</form>";
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/bottle", HTTP_GET, []() {
    String html = "<html><head><title>Set Bottle Capacity</title></head><body>";
    html += "<h1>Set Bottle Capacity</h1>";
    html += "<form action='/bottle' method='POST'>";
    html += "<label for='channel'>Channel:</label>";
    html += "<select name='channel'>";
    html += "<option value='1'>" + channel1Name + "</option>";
    html += "<option value='2'>" + channel2Name + "</option>";
    html += "</select><br><br>";
    html += "<label for='ml'>Capacity (ml):</label>";
    html += "<input type='number' name='ml' step='0.1'><br><br>";
    html += "<input type='submit' value='Set Capacity'>";
    html += "</form>";
    html += "<p>Current Levels:</p>";
    html += "<p>" + channel1Name + ": " + String(remainingMLChannel1) + " ml</p>";
    html += "<p>" + channel2Name + ": " + String(remainingMLChannel2) + " ml</p>";
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
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

  server.on("/view", HTTP_GET, []() {
    String html = "<html><head>";
    html += "<title>View Parameters</title>";
    html += "<meta http-equiv='refresh' content='5'>";
    html += "</head><body>";
    html += "<h1>Current Parameters</h1>";
    
    // Display Channel Names
    html += "<h2>Channel Names</h2>";
    html += "<p>" + channel1Name + " (Channel 1)</p>";
    html += "<p>" + channel2Name + " (Channel 2)</p>";
    
    // Display Remaining ML
    html += "<h2>Remaining Liquid</h2>";
    html += "<p>" + channel1Name + ": " + String(remainingMLChannel1) + "ml</p>";
    html += "<p>" + channel2Name + ": " + String(remainingMLChannel2) + "ml</p>";
    
    // Display Daily Schedule
    html += "<h2>Daily Schedule</h2>";
    html += "<p>" + channel1Name + ": " + String(channel1Schedule.hour) + ":" + 
            String(channel1Schedule.minute) + " - " + String(channel1Schedule.ml) + "ml</p>";
    html += "<p>" + channel2Name + ": " + String(channel2Schedule.hour) + ":" + 
            String(channel2Schedule.minute) + " - " + String(channel2Schedule.ml) + "ml</p>";
    
    // Display Last Dispense Info
    html += "<h2>Last Dispense</h2>";
    html += "<p>" + channel1Name + ": " + String(lastDispensedVolume1) + "ml at " + lastDispensedTime1 + "</p>";
    html += "<p>" + channel2Name + ": " + String(lastDispensedVolume2) + "ml at " + lastDispensedTime2 + "</p>";
    
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/timezone", HTTP_GET, []() {
    String html = "<html><head><title>Set Timezone</title></head><body>";
    html += "<h1>Set Timezone</h1>";
    html += "<form action='/timezone' method='POST'>";
    html += "<label for='offset'>Timezone offset (in seconds):</label><br>";
    html += "<select name='offset'>";
    html += "<option value='-43200'" + String((timezoneOffset == -43200) ? " selected" : "") + ">UTC-12:00</option>";
    html += "<option value='-39600'" + String((timezoneOffset == -39600) ? " selected" : "") + ">UTC-11:00</option>";
    html += "<option value='-36000'" + String((timezoneOffset == -36000) ? " selected" : "") + ">UTC-10:00</option>";
    html += "<option value='-32400'" + String((timezoneOffset == -32400) ? " selected" : "") + ">UTC-09:00</option>";
    html += "<option value='-28800'" + String((timezoneOffset == -28800) ? " selected" : "") + ">UTC-08:00 (PST)</option>";
    html += "<option value='-25200'" + String((timezoneOffset == -25200) ? " selected" : "") + ">UTC-07:00 (MST)</option>";
    html += "<option value='-21600'" + String((timezoneOffset == -21600) ? " selected" : "") + ">UTC-06:00 (CST)</option>";
    html += "<option value='-18000'" + String((timezoneOffset == -18000) ? " selected" : "") + ">UTC-05:00 (EST)</option>";
    html += "<option value='-14400'" + String((timezoneOffset == -14400) ? " selected" : "") + ">UTC-04:00</option>";
    html += "<option value='-10800'" + String((timezoneOffset == -10800) ? " selected" : "") + ">UTC-03:00</option>";
    html += "<option value='-7200'" + String((timezoneOffset == -7200) ? " selected" : "") + ">UTC-02:00</option>";
    html += "<option value='-3600'" + String((timezoneOffset == -3600) ? " selected" : "") + ">UTC-01:00</option>";
    html += "<option value='0'" + String((timezoneOffset == 0) ? " selected" : "") + ">UTC+00:00</option>";
    html += "<option value='3600'" + String((timezoneOffset == 3600) ? " selected" : "") + ">UTC+01:00</option>";
    html += "<option value='7200'" + String((timezoneOffset == 7200) ? " selected" : "") + ">UTC+02:00</option>";
    html += "<option value='10800'" + String((timezoneOffset == 10800) ? " selected" : "") + ">UTC+03:00</option>";
    html += "<option value='14400'" + String((timezoneOffset == 14400) ? " selected" : "") + ">UTC+04:00</option>";
    html += "<option value='18000'" + String((timezoneOffset == 18000) ? " selected" : "") + ">UTC+05:00</option>";
    html += "<option value='19800'" + String((timezoneOffset == 19800) ? " selected" : "") + ">UTC+05:30 (IST)</option>";
    html += "<option value='21600'" + String((timezoneOffset == 21600) ? " selected" : "") + ">UTC+06:00</option>";
    html += "<option value='25200'" + String((timezoneOffset == 25200) ? " selected" : "") + ">UTC+07:00</option>";
    html += "<option value='28800'" + String((timezoneOffset == 28800) ? " selected" : "") + ">UTC+08:00</option>";
    html += "<option value='32400'" + String((timezoneOffset == 32400) ? " selected" : "") + ">UTC+09:00 (JST)</option>";
    html += "<option value='36000'" + String((timezoneOffset == 36000) ? " selected" : "") + ">UTC+10:00</option>";
    html += "<option value='39600'" + String((timezoneOffset == 39600) ? " selected" : "") + ">UTC+11:00</option>";
    html += "<option value='43200'" + String((timezoneOffset == 43200) ? " selected" : "") + ">UTC+12:00</option>";
    html += "</select><br><br>";
    html += "<input type='submit' value='Set Timezone'>";
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
  server.on("/daily", HTTP_POST, handleDailyDispense);
  server.on("/bottle", HTTP_POST, handleBottleTracking);
  server.on("/reset", HTTP_POST, handleSystemReset);
  server.on("/prime", HTTP_POST, handlePrimePump);

  server.on("/prime", HTTP_GET, []() {
    String html = "<html><head>";
    html += "<title>Prime Pump</title>";
    html += "<script>";
    html += "function updateButton(channel) {";
    html += "  var btn = document.getElementById('primeButton');";
    html += "  var state = btn.getAttribute('data-state') === '1' ? '0' : '1';";
    html += "  var xhr = new XMLHttpRequest();";
    html += "  xhr.open('POST', '/prime', true);";
    html += "  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
    html += "  xhr.send('channel=' + channel + '&state=' + state);";
    html += "  btn.setAttribute('data-state', state);";
    html += "  btn.value = state === '1' ? 'Stop' : 'Start';";
    html += "}";
    html += "</script>";
    html += "</head><body>";
    html += "<h1>Prime Pump</h1>";
    html += "<label for='channel'>Select Channel:</label>";
    html += "<select name='channel' id='channel'>";
    html += "<option value='1'>" + channel1Name + "</option>";
    html += "<option value='2'>" + channel2Name + "</option>";
    html += "</select><br><br>";
    html += "<input type='button' id='primeButton' data-state='0' value='Start' onclick='updateButton(document.getElementById(\"channel\").value)'>";
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/names", HTTP_GET, []() {
    String html = "<html><head><title>Set Channel Names</title></head><body>";
    html += "<h1>Set Channel Names</h1>";
    html += "<form action='/names' method='POST'>";
    html += "<label for='name1'>Name for Channel 1:</label><br>";
    html += "<input type='text' name='name1' value='" + channel1Name + "'><br><br>";
    html += "<label for='name2'>Name for Channel 2:</label><br>";
    html += "<input type='text' name='name2' value='" + channel2Name + "'><br><br>";
    html += "<input type='submit' value='Set Names'>";
    html += "</form>";
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/names", HTTP_POST, []() {
    if (server.hasArg("name1") && server.hasArg("name2")) {
      channel1Name = server.arg("name1");
      channel2Name = server.arg("name2");
      savePersistentDataToSPIFFS();
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "");
    } else {
      server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
    }
  });

  // In setupWebServer() function, update the /bottleconfig endpoint
  server.on("/bottleconfig", HTTP_GET, []() {
    String html = "<html><head><title>Bottle Configuration</title></head><body>";
    html += "<h1>Bottle Configuration</h1>";
    html += "<form action='/bottleconfig' method='POST'>";
    html += "<label for='channel'>Select Channel:</label>";
    html += "<select name='channel'>";
    html += "<option value='1'>" + channel1Name + "</option>";
    html += "<option value='2'>" + channel2Name + "</option>";
    html += "</select><br><br>";
    html += "<label for='capacity'>Bottle Capacity (ml):</label>";
    html += "<input type='number' name='capacity' step='0.1'><br><br>";
    html += "<input type='submit' value='Set Capacity'>";
    html += "</form>";
    html += "<p><a href='/'>Back to Home</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.begin();
}

void handleCalibration() {
  if (server.hasArg("channel")) {
    int channel = server.arg("channel").toInt();
    
    // If we have the dispensed amount, complete calibration
    if (server.hasArg("dispensedML")) {
      float dispensedML = server.arg("dispensedML").toFloat();
      float &calibrationFactor = (channel == 1) ? calibrationFactor1 : calibrationFactor2;
      
      // Calculate milliseconds needed per mL (2000ms produced dispensedML)
      calibrationFactor = 10000.0 / dispensedML;  // This gives us ms/mL directly
      
      // Save calibration factor
      savePersistentDataToSPIFFS();
      
      // Redirect back to home
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "");
      return;
    }
    
    // First phase - run the motor and show input form
    if (channel == 1 || channel == 2) {
      // Run motor for exactly 2 seconds
      runMotor(channel, 10000);
      
      // Show form to input dispensed amount
      String html = "<html><head><title>Calibration Measurement</title></head><body>";
      html += "<h1>Calibration Measurement</h1>";
      html += "<p>Motor has run for 2 seconds. Please measure the dispensed liquid and enter the amount below:</p>";
      html += "<form action='/calibrate' method='POST'>";
      html += "<input type='hidden' name='channel' value='" + String(channel) + "'>";
      html += "<label for='dispensedML'>Amount dispensed (ml):</label><br>";
      html += "<input type='number' name='dispensedML' step='0.1' required><br><br>";
      html += "<input type='submit' value='Submit Measurement'>";
      html += "</form>";
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
    publishLastDosed(channel, ml);

    // Update last dispensed volume and time for the channel
    if (channel == 1) {
      lastDispensedVolume1 = ml;
      lastDispensedTime1 = getFormattedTime();
    } else if (channel == 2) {
      lastDispensedVolume2 = ml;
      lastDispensedTime2 = getFormattedTime();
    }

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

    if (channel == 1) {
      channel1Schedule = {hour, minute, ml};
    } else if (channel == 2) {
      channel2Schedule = {hour, minute, ml};
    }

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
  
  // Check both channels first
  if (currentHour == channel1Schedule.hour && currentMinute == channel1Schedule.minute &&
      (currentHour != lastDispenseHour1 || currentMinute != lastDispenseMinute1)) {
    runCh1 = true;
  }
  
  if (currentHour == channel2Schedule.hour && currentMinute == channel2Schedule.minute &&
      (currentHour != lastDispenseHour2 || currentMinute != lastDispenseMinute2)) {
    runCh2 = true;
  }
  
  // If both channels need to run, indicate this with LED
  if (runCh1 && runCh2) {
    blinkLED(LED_PURPLE, 2);  // Blink purple twice to indicate dual channel operation
  }
  
  // Run channel 1 if needed
  if (runCh1) {
    int dispenseTime = channel1Schedule.ml * calibrationFactor1;
    runMotor(1, dispenseTime);
    updateRemainingML(1, channel1Schedule.ml);
    publishLastDosed(1, channel1Schedule.ml);
    lastDispenseHour1 = currentHour;
    lastDispenseMinute1 = currentMinute;
    lastDispensedVolume1 = channel1Schedule.ml;
    lastDispensedTime1 = getFormattedTime();
  }
  
  // Run channel 2 if needed
  if (runCh2) {
    int dispenseTime = channel2Schedule.ml * calibrationFactor2;
    runMotor(2, dispenseTime);
    updateRemainingML(2, channel2Schedule.ml);
    publishLastDosed(2, channel2Schedule.ml);
    lastDispenseHour2 = currentHour;
    lastDispenseMinute2 = currentMinute;
    lastDispensedVolume2 = channel2Schedule.ml;
    lastDispensedTime2 = getFormattedTime();
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

  // Load schedule for channel 1
  if (doc.containsKey("schedule1")) {
    channel1Schedule.hour = doc["schedule1"]["hour"] | 0;
    channel1Schedule.minute = doc["schedule1"]["minute"] | 0;
    channel1Schedule.ml = doc["schedule1"]["ml"] | 0.0f;
  }

  // Load schedule for channel 2
  if (doc.containsKey("schedule2")) {
    channel2Schedule.hour = doc["schedule2"]["hour"] | 0;
    channel2Schedule.minute = doc["schedule2"]["minute"] | 0;
    channel2Schedule.ml = doc["schedule2"]["ml"] | 0.0f;
  }

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

  // Save schedule for channel 1
  JsonObject schedule1 = doc.createNestedObject("schedule1");
  schedule1["hour"] = channel1Schedule.hour;
  schedule1["minute"] = channel1Schedule.minute;
  schedule1["ml"] = channel1Schedule.ml;

  // Save schedule for channel 2
  JsonObject schedule2 = doc.createNestedObject("schedule2");
  schedule2["hour"] = channel2Schedule.hour;
  schedule2["minute"] = channel2Schedule.minute;
  schedule2["ml"] = channel2Schedule.ml;

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

  // Publish updated remaining ML
  publishRemainingML();
}

void setupMQTT() {
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
}

void handleMQTTConnection() {
  if (!mqttClient.connected()) {
    Serial.println("Attempting MQTT connection...");
    if (mqttClient.connect("ESP8266Client")) {
      Serial.println("Connected to MQTT");
      mqttClient.subscribe(mqttTopicManualDose);
    } else {
      Serial.print("Failed MQTT connection, rc=");
      Serial.println(mqttClient.state());
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  if (String(topic) == mqttTopicManualDose) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, message);
    int channel = doc["channel"];
    float ml = doc["ml"];
    float calibrationFactor = (channel == 1) ? calibrationFactor1 : calibrationFactor2;
    
    int dispenseTime = ml * calibrationFactor * 1000;
    runMotor(channel, dispenseTime);
    
    updateRemainingML(channel, ml);
    publishRemainingML();
  }
}

void publishLastDosed(int channel, float ml) {
  if (!mqttClient.connected()) return;
  
  DynamicJsonDocument doc(200);
  doc["channel"] = (channel == 1) ? channel1Name : channel2Name;
  doc["ml"] = ml;
  doc["timestamp"] = getFormattedTime();
  
  char buffer[200];
  serializeJson(doc, buffer);
  mqttClient.publish(mqttTopicLastDosed, buffer);
}

void publishRemainingML() {
  if (!mqttClient.connected()) return;
  
  DynamicJsonDocument doc(200);
  JsonObject ch1 = doc.createNestedObject(channel1Name);
  ch1["ml"] = remainingMLChannel1;
  
  JsonObject ch2 = doc.createNestedObject(channel2Name);
  ch2["ml"] = remainingMLChannel2;
  
  char buffer[200];
  serializeJson(doc, buffer);
  mqttClient.publish(mqttTopicRemainingML, buffer);
}

void handleSystemReset() {
  // Reset system settings
  LittleFS.remove("/data.json"); // Example reset logic
  ESP.restart();
}

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
    
    server.send(200, "application/json", "{\"status\":\"prime pump " + String(state ? "started" : "stopped") + "\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
  }
}

