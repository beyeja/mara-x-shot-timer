// Core / network
#include <ArduinoOTA.h>

#define D5 (21)            // machine bus pin
#define D6 (20)            // machine bus pin
#define D7 (4)             // pump/reedswitch pin
#define PUMP_PIN D7        // pump/reedswitch
#define TIME_SHOT_LIMIT 20 // limit in seconds when pump on is considered a shot

// shot timer background animation tuning
// Circle is 0 until TIME_SHOT_LIMIT, grows to max between
// TIME_SHOT_LIMIT–30s, then shrinks back to 0 by 35s.
#define SHOT_ANIM_GROW_START_SECONDS TIME_SHOT_LIMIT
#define SHOT_ANIM_MAX_SECONDS 30.0f   // time when circle reaches max size
#define SHOT_ANIM_TOTAL_SECONDS 35.0f // time when circle shrinks back to 0
#define SHOT_ANIM_CENTER_X (SCREEN_WIDTH / 2)
#define SHOT_ANIM_CENTER_Y (SCREEN_HEIGHT / 2)
// Chosen so a full circle covers the display (approx half diagonal)
#define SHOT_ANIM_MAX_RADIUS 72

// value to be set when timer is inactive to not fall asleep again
// immediatly
#define TIMER_INACTIVE 0
#define SLEEP_TIME 1000 * 60 * 15    // time until sleep after wakeup/startup
#define MACHINE_SLEEP_TIME 1000 * 30 // time until sleep when machine is off

#define I2C_ADDRESS 0x3C // 0x3c //0x3D
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels

// set to true/false when using another type of reed sensor
#define REED_OPEN true // reed default open or default closed

#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_SSD1306.h>
#include <SoftwareSerial.h>
#include <Timer.h>
#include <WiFi.h> // For connecting ESP32 to WiFi
#include <Wire.h>
#include <splash.h>
#include <time.h> // for NTP / local time support

WiFiClient client;

#include "secrets.h" // your secrets for wifi connection
#include <ArduinoHA.h>

#include "bookooScale.h" // BLE scale integration

// icons and animations
#include "coffeeAnimation.h"
#include "coffeeBeanIcon.h"
#include "modeUnknownIcon.h"
#include "steamIcon.h"
#include "tempIcon.h"
#include "wifiIcon.h"

const char *ssid = WIFI_SSID;     // wifi name
const char *password = WIFI_PW;   // wifi pw
const char *otaPassword = OTA_PW; // wifi pw

#define SCREEN_WHITE 1 // SSD1306_WHITE

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// contrast levels for 'normal' and 'dim' modes (0-255)
#define CONTRAST_NORMAL 0xCF
#define CONTRAST_DIM 0x08

// helper to send a raw command byte over I2C to the OLED
static void sendDisplayCommand(uint8_t cmd) {
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(0x00); // Co = 0, D/C# = 0 => command
  Wire.write(cmd);
  Wire.endTransmission();
}

// contrast setter uses two commands
static void setDisplayContrast(uint8_t level) {
  sendDisplayCommand(0x81);
  sendDisplayCommand(level);
}

SoftwareSerial machineSerialInput(D5, D6);
Timer t;

// mqtt setup
HADevice device;
HAMqtt mqtt(client, device);
HASensor mqttTemperature("temperature");
HASensor mqttSteamTemp("steamTemp");
HASensor mqttLastShotTime("lastShotTime");
HASensor mqttPumpSensor("pumpSensor");
HASensor mqttHeating("heating");
HASensor mqttHeatingBoost("heatingBoost");
HASensor mqttMachineMode("machineMode");
HASensor mqttSleep("sleep");

// main states
int pumpOn = 0;        // is pump on
bool displayOn = true; // is display on
// pump state save for activity detection (previously handled within
// detectPumpChanges)
bool lastPumpOn = false;
// pump measurement - independent of display/shot-timer state
unsigned long pumpStartMillis = 0; // timestamp of the pump-on edge
float pumpRunSec = 0;              // live duration of the current pump run
float lastPumpRunSec = 0;          // duration latched when the pump stops
long lastSerialUpdatedValue = 0; // time of last update of machine serial
float lastShotTimeSec = 0;       // time of last pump on time considered a shot
bool isShotTimerMode = false;    // display is in shot timer mode
// scale-driven: extraction is running (shot mode stays on until the scale
// finishes); stays false when no scale data arrives so nothing gets stuck
bool extractionActive = false;

// track when the screen was last woken and whether it's been dimmed
long displayWakeMillis = 0;
bool displayDimmed = false;

// track when WiFi connected to display IP briefly
long wifiConnectMillis = 0;
#define WIFI_IP_DISPLAY_DURATION 5000 // show IP for 5 seconds after connect

// previous machine mode used to detect changes
String prevCoffeeSteamMode = "";

// helper to reset dim state and treat as a wake event
static void wakeForActivity() {
  // only undim if display is currently on and has been dimmed
  if (displayOn) {
    setDisplayContrast(CONTRAST_NORMAL);
    displayDimmed = false;
    displayWakeMillis = millis();
  }
}

// machine message states
String coffeeSteamMode = "";
bool isHeating = false;
bool isHeatingBoost = false;
String temp = "";
String steamTemp = "";

long timerStopMillis = 0;
long timerDisplayOffMillis = TIMER_INACTIVE;
long lastSerialUpdateMillis = 0;

// midnight flash tracking
int lastFlashDay = -1;
long flashStartMillis = 0;
bool flashing = false;

// whether MQTT configuration is valid (set in setup based on secrets.h)
bool mqttConfigured = false;

// current radius of the shot timer background circle animation
int shotAnimRadius = 0;

// time when pump sensor last changed, used to detect pump activity and when
// pump turned off
unsigned long timerLastPumpSensorChange = 0;
// previous pump state to detect pump activity
bool lastPumpSensorState = false;

// reading machine serial variables
const byte numChars = 32;
char receivedChars[numChars];
static byte ndx = 0;
const char endMarker = '\n';

void onMqttConnected() { Serial.println("MQTT connected!"); }
void onMqttDisconnected() { Serial.println("MQTT disconnected"); }

void publishMQTTState() {
  if (!mqttConfigured) {
    return;
  }
  mqttTemperature.setValue(temp.c_str());
  mqttSteamTemp.setValue(steamTemp.c_str());
  mqttLastShotTime.setValue(String(lastShotTimeSec).c_str());
  mqttPumpSensor.setValue(pumpOn ? "ON" : "OFF");
  mqttHeating.setValue(isHeating ? "ON" : "OFF");
  mqttHeatingBoost.setValue(isHeatingBoost ? "ON" : "OFF");
  mqttMachineMode.setValue(coffeeSteamMode.c_str());
  mqttSleep.setValue(displayOn ? "OFF" : "ON");
}

void setupMQTT() {
  byte uniqueId[] = {'e', 's', 'p', '-', 'm', 'a', 'r',
                     'a', '-', 't', 'i', 'm', 'e', 'r'};
  device.setUniqueId(uniqueId, sizeof(uniqueId));
  device.setName("Lelit Mara X Timer");
  device.setSoftwareVersion("1.0.0");
  device.setModel("ESP32");

  mqttTemperature.setName("Temperature");
  mqttTemperature.setUnitOfMeasurement("C");

  mqttSteamTemp.setName("Steam Temperature");
  mqttSteamTemp.setUnitOfMeasurement("C");

  mqttLastShotTime.setName("Last Shot Time");
  mqttLastShotTime.setUnitOfMeasurement("s");

  mqttPumpSensor.setName("Pump State");
  mqttPumpSensor.setIcon("mdi:water");

  mqttHeating.setName("Heating");
  mqttHeating.setIcon("mdi:thermometer");

  mqttHeatingBoost.setName("Heating Boost");
  mqttHeatingBoost.setIcon("mdi:thermometer-high");

  mqttMachineMode.setName("Machine Mode");
  mqttMachineMode.setIcon("mdi:coffee");

  mqttSleep.setName("Sleep");
  mqttSleep.setIcon("mdi:sleep");

  mqtt.onConnected(onMqttConnected);
  mqtt.onDisconnected(onMqttDisconnected);
  Serial.println("MQTT configured; connection will start after WiFi connects");
}

void setup() {
  // Serial logging connection
  Serial.begin(115200);

  pinMode(PUMP_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // machine serial connection
  machineSerialInput.begin(9600);

  // clears serial buffer and variables for reading machine serial messages
  memset(receivedChars, 0, numChars);

  // set display pins
  // Wire.begin(6, 7); // 6 sck // 7 sda
  Wire.begin(7, 6); // 6 sck // 7 sda

  delay(250); // wait for the OLED to power up
  display.begin(0x3C, true);
  // display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SCREEN_WHITE);
  display.drawPixel(10, 10, SH110X_WHITE);
  display.display();

  // white-out for 5 seconds on cold start to even out pixel wear
  display.fillScreen(SCREEN_WHITE);
  display.display();
  delay(5000);
  display.clearDisplay();
  display.display();

  // ensure screen starts at full brightness
  setDisplayContrast(CONTRAST_NORMAL);
  displayWakeMillis = millis();
  displayDimmed = false;

  // setup wifi connection (after white flash so IP is visible)
  WiFi.disconnect();
  Serial.println("Wifi: Connecting...");

  // setup OTA updates
  WiFi.mode(WIFI_STA);
  WiFi.hostname("esp-lelit-mara-timer");
  WiFi.begin(ssid, password); // Connect to WiFi - defaults to WiFi Station mode
  // configure NTP (UTC, adjust offset later with timezone if needed)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  Serial.println("WiFi connection is starting asynchronously");

  // setup ota
  ArduinoOTA.begin(); // Starts OTA
  ArduinoOTA.setHostname("esp-lelit-mara-timer");
  ArduinoOTA.setPassword(otaPassword);

  // setup MQTT if a valid broker address is configured
#ifdef MQTT_BROKER_ADDR
  if (MQTT_BROKER_ADDR[0] != 0 || MQTT_BROKER_ADDR[1] != 0 ||
      MQTT_BROKER_ADDR[2] != 0 || MQTT_BROKER_ADDR[3] != 0) {
    mqttConfigured = true;
    setupMQTT();
  } else {
    Serial.println("MQTT disabled: broker IP is 0.0.0.0");
  }
#else
  Serial.println("MQTT disabled: MQTT_BROKER_ADDR not defined");
#endif

  bookooSetup();

  t.every(32, updateDisplay);
  t.every(1000, publishMQTTState);

  machineSerialInput.write(0x11);
}

// track last WiFi check time
unsigned long lastWifiCheck = 0;
#define WIFI_CHECK_INTERVAL 30000 // check every 30 seconds
bool lastWifiConnected = false;
bool mqttStarted = false;

void loop() {
  ArduinoOTA.handle();

  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  // MQTT can only start after Wi-Fi is available. Its loop then manages
  // broker reconnection automatically, including after Home Assistant restarts.
  if (wifiConnected && !lastWifiConnected) {
    Serial.println("WiFi connected.");
    wifiConnectMillis = millis();

    if (mqttConfigured) {
      if (!mqttStarted) {
        Serial.println("Starting MQTT...");
        mqtt.begin(MQTT_BROKER_ADDR, MQTT_BROKER_PORT, MQTT_USERNAME,
                   MQTT_PASSWORD);
        mqttStarted = true;
        Serial.print("MQTT broker: ");
        Serial.print(MQTT_BROKER_ADDR);
        Serial.print(":");
        Serial.println(MQTT_BROKER_PORT);
      }
    }
  }

  lastWifiConnected = wifiConnected;

  if (mqttConfigured && wifiConnected) {
    mqtt.loop();
  }

  // Check WiFi periodically without blocking the machine timer or display.
  if (millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();

    if (!wifiConnected) {
      Serial.println("WiFi disconnected, attempting to reconnect...");
      WiFi.reconnect();
    }
  }

  t.update();

  detectSleep();
  detectPumpChanges();
  updatePumpTimer();
  updateShotTimerMode();
  readMachineInput();
  evalMachineMessage();

  bookooLoop();
}

// read machine serial message
void readMachineInput() {
  char rc;
  // read available serial
  while (machineSerialInput.available() > 0) {
    lastSerialUpdateMillis = millis();
    rc = machineSerialInput.read();

    if (rc != endMarker) {
      receivedChars[ndx] = rc;
      ndx++;
      if (ndx >= numChars) {
        ndx = numChars - 1;
      }
    } else {
      receivedChars[ndx] = '\0';
      ndx = 0;
      lastSerialUpdatedValue = millis();
      Serial.println(receivedChars);
    }
  }

  // send updaate command to machine?
  if (millis() - lastSerialUpdateMillis > 5000) {
    lastSerialUpdateMillis = millis();
    memset(receivedChars, 0, numChars);
    Serial.println("Request serial update");
    machineSerialInput.write(0x11);
  }
}

// read values from machine serial message
void evalMachineMessage() {
  // detect mode change before we overwrite the variable
  String oldMode = coffeeSteamMode;

  // reset states
  coffeeSteamMode = "";
  isHeating = false;
  isHeatingBoost = false;
  temp = "";
  steamTemp = "";

  // eval machine prio mode from serial
  if (receivedChars[0] && String(receivedChars[0]) == "C") {
    coffeeSteamMode = "C";
  } else if (receivedChars[0] && String(receivedChars[0]) == "V") {
    coffeeSteamMode = "S";
  } else if (receivedChars[0]) {
    coffeeSteamMode = "X";
  }

  // if mode changed, wake display
  if (coffeeSteamMode != oldMode) {
    wakeForActivity();
  }

  // eval heating mode
  if (String(receivedChars).substring(18, 22) == "0000") {
    // not in boost heating mode
    if (String(receivedChars[23]) == "1") {
      isHeating = true;
      isHeatingBoost = false;
    } else if (String(receivedChars[23]) == "0") {
      isHeating = false;
      isHeatingBoost = false;
    }
  } else {
    if (String(receivedChars[23]) == "1") {
      // in boost heating mode
      isHeating = true;
      isHeatingBoost = true;
    } else if (String(receivedChars[23]) == "0") {
      // not in boost heating mode
      isHeating = false;
      isHeatingBoost = true;
    }
  }

  // read temp
  if (receivedChars[14] && receivedChars[15] && receivedChars[16]) {
    if (String(receivedChars[14]) != "0") {
      temp = String(receivedChars[14]);
    }
    temp += String(receivedChars[15]);
    temp += String(receivedChars[16]);
  }

  // read steam temp
  if (receivedChars[6] && receivedChars[7] && receivedChars[8]) {
    if (String(receivedChars[6]) != "0") {
      steamTemp = String(receivedChars[6]);
    }
    steamTemp += String(receivedChars[7]);
    steamTemp += String(receivedChars[8]);
  }
}

void detectPumpChanges() {
  digitalWrite(LED_BUILTIN, digitalRead(PUMP_PIN));

  // read raw sensor value (inverts if REED_OPEN is false)
  bool raw = REED_OPEN ? digitalRead(PUMP_PIN) : !digitalRead(PUMP_PIN);

  // detect pump activity
  if (raw != lastPumpSensorState) {
    // when pump state changes, store new state and time, consider pump to be
    // running

    lastPumpSensorState = raw;
    timerLastPumpSensorChange = millis();

    // emit event that pump turned on
    if (!pumpOn) {
      Serial.println("Pump ON");
      if (mqttConfigured) {
        mqttPumpSensor.setValue("ON");
      }

      pumpOn = true;
      pumpStartMillis = millis(); // start the independent pump measurement

      // wake display for activity
      wakeForActivity();
    }
  } else if (pumpOn && millis() - timerLastPumpSensorChange > 500) {
    // when pump was on but now off for >500ms, consider pump off

    pumpOn = false;
    lastPumpRunSec = pumpRunSec; // latch the finished pump run duration
    Serial.println("Pump OFF");
    if (mqttConfigured) {
      mqttPumpSensor.setValue("OFF");
    }

    wakeForActivity();
  }
}

void updateShotTimerMode() {
  // track extraction state from the scale (shot mode extends until the
  // scale finishes its shot)
  if (bookooConsumeExtractionStarted()) {
    extractionActive = true;
  }
  if (bookooConsumeExtractionStopped()) {
    extractionActive = false;
  }
  // a lost/stale scale must never keep the shot timer stuck on screen
  if (!bookooConnected || !bookooHasData) {
    extractionActive = false;
  }

  if (!isShotTimerMode && (pumpOn || extractionActive)) {
    isShotTimerMode = true;
    timerStopMillis = 0;
  }

  if (isShotTimerMode && !pumpOn && !extractionActive) {
    timerStopMillis = millis();
    isShotTimerMode = false;
  }
}

// pump measurement - fully independent of the screen state. advances while
// the pump is on, freezes when it stops (lastPumpRunSec is latched in
// detectPumpChanges). no interaction with the shot timer mode.
void updatePumpTimer() {
  if (pumpOn) {
    pumpRunSec = (float)(millis() - pumpStartMillis) / 1000;

    // classic "long pump run = shot" bookkeeping (dashboard + MQTT)
    if (pumpRunSec > TIME_SHOT_LIMIT) {
      lastShotTimeSec = pumpRunSec;
    }
  }
}

void detectSleep() {
  // wake up when machine updates or pump turns on
  if (!displayOn && millis() - lastSerialUpdatedValue <= MACHINE_SLEEP_TIME) {
    displayOn = true;

    // reset sleeping timers so we don't immediately go back to sleep
    timerDisplayOffMillis = TIMER_INACTIVE;

    // record when display woke up so we can dim later
    displayWakeMillis = millis();
    displayDimmed = false;
    // ensure full brightness on wake
    setDisplayContrast(CONTRAST_NORMAL);

    Serial.println("Wake up");
  }

  // update pump related display off timer when pump stops
  if (isShotTimerMode && pumpOn && millis() - timerStopMillis > 500) {
    timerDisplayOffMillis = millis();

    Serial.println("Pump stopped, update last pump stop time");
  }

  // go into sleep mode
  if (!isShotTimerMode && displayOn) {
    if (timerDisplayOffMillis != TIMER_INACTIVE &&
        millis() - timerDisplayOffMillis >= SLEEP_TIME) {
      // go to sleep due to last shot time

      // reset values to be cleared after wakeup
      timerDisplayOffMillis = TIMER_INACTIVE;
      pumpRunSec = 0;
      lastShotTimeSec = 0;

      // turn off screen
      displayOn = false;

      Serial.println("Last Shot Sleep");
    } else if (millis() - lastSerialUpdatedValue >= MACHINE_SLEEP_TIME) {
      // go to sleep due to last machine message update

      // turn off screen
      displayOn = false;

      Serial.println("Last Message Sleep");
    }
  }
}

void updateShotAnimation() {
  if (isShotTimerMode) {
    // compute animation radius based on the dominant shot clock (the
    // extraction time when the scale is timing, otherwise the pump time)
    float t = pumpRunSec;
    if (extractionActive) {
      t = (float)bookooElapsedMs / 1000.0f;
    }
    float progress = 0.0f;

    if (t <= SHOT_ANIM_GROW_START_SECONDS) {
      // no circle before grow window
      progress = 0.0f;
    } else if (t <= SHOT_ANIM_MAX_SECONDS) {
      // grow from 0 to max between SHOT_ANIM_GROW_START_SECONDS and
      // SHOT_ANIM_MAX_SECONDS
      progress = (t - SHOT_ANIM_GROW_START_SECONDS) /
                 (SHOT_ANIM_MAX_SECONDS - SHOT_ANIM_GROW_START_SECONDS);
    } else if (t <= SHOT_ANIM_TOTAL_SECONDS) {
      // shrink back to 0 between SHOT_ANIM_MAX_SECONDS and
      // SHOT_ANIM_TOTAL_SECONDS
      progress = (SHOT_ANIM_TOTAL_SECONDS - t) /
                 (SHOT_ANIM_TOTAL_SECONDS - SHOT_ANIM_MAX_SECONDS);
    } else {
      progress = 0.0f;
    }

    if (progress < 0.0f) {
      progress = 0.0f;
    } else if (progress > 1.0f) {
      progress = 1.0f;
    }

    int radius = (int)(progress * SHOT_ANIM_MAX_RADIUS);
    // ensure we see at least a tiny circle once the timer has started
    if (radius == 0 && t > 0.0f) {
      radius = 1;
    }
    shotAnimRadius = radius;
  } else {
    shotAnimRadius = 0;
  }
}

bool isMidnightFlashPeriod() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return false;
  }
  int day = timeinfo.tm_mday;
  // start flash when hour==0 and new day
  if (day != lastFlashDay && timeinfo.tm_hour == 0) {
    lastFlashDay = day;
    flashStartMillis = millis();
    flashing = true;
    return true;
  }
  return false;
}

void updateDisplay() {
  display.clearDisplay();

  // check for start of midnight flash
  if (!flashing && isMidnightFlashPeriod()) {
    // nothing else, will draw white below
  }

  if (displayOn) {
    // if we're in the flash window, just show white and skip everything else
    if (flashing) {
      if (millis() - flashStartMillis < (5 * 60 * 1000)) {
        // keep white for 5 minutes
        display.fillScreen(SCREEN_WHITE);
        display.display();
        return;
      } else {
        flashing = false;
      }
    }

    // dim screen after 10s awake (not while pump or extraction is running)
    if (!displayDimmed && !pumpOn && !extractionActive &&
        millis() - displayWakeMillis >= 10000) {
      setDisplayContrast(CONTRAST_DIM);
      displayDimmed = true;
    }

    updateShotAnimation();

    if (isShotTimerMode) {
      // dominant clock: extraction time while the scale is timing (and
      // connected), otherwise the pump time
      float dominantSec = pumpRunSec;
      bool showExtraction = extractionActive && bookooHasData;
      if (showExtraction) {
        dominantSec = (float)bookooElapsedMs / 1000.0f;
      }

      // prepare time components once for this frame
      double secFractions, seconds;
      secFractions = modf(dominantSec, &seconds);
      secFractions = secFractions * 10;
      secFractions = static_cast<int>(secFractions);

      if (fmod(secFractions, 2) != 0) {
        secFractions = secFractions - 1;
        secFractions = secFractions < 0 ? 0 : secFractions;
      }

      // Use off-screen buffers and XOR combine to render the animated
      // background circle behind the time while keeping digits readable.
      const size_t bufferSize = (SCREEN_WIDTH * SCREEN_HEIGHT) / 8;
      uint8_t *framebuffer = display.getBuffer();
      static uint8_t circleBuffer[bufferSize];
      static uint8_t timeBuffer[bufferSize];

      // Draw only the animated circle ring (2px wide) into the framebuffer
      display.clearDisplay();
      if (shotAnimRadius >= 2) {
        // outer ring
        display.drawCircle(SHOT_ANIM_CENTER_X, SHOT_ANIM_CENTER_Y,
                           shotAnimRadius, SCREEN_WHITE);
        // inner ring for 2px stroke
        display.drawCircle(SHOT_ANIM_CENTER_X, SHOT_ANIM_CENTER_Y,
                           shotAnimRadius - 1, SCREEN_WHITE);
      }
      memcpy(circleBuffer, framebuffer, bufferSize);

      display.clearDisplay();

      // Draw primary timevalue into the framebuffer
      if (showExtraction) {
        display.setTextSize(1);
        display.setCursor(0, 2);
        display.print("Extr:");

        display.setTextSize(3);
        display.setCursor(0, 14);
        display.printf("%02.0f", seconds);

        display.setTextSize(2);
        display.printf("%1.0f", secFractions);
        display.print("s");
      } else {
        display.setTextSize(4);
        display.setCursor(20, 14);
        display.printf("%02.0f", seconds);

        display.setTextSize(2);
        display.printf(".%1.0fs", secFractions);
      }

      // render small pump timer
      if (showExtraction) {
        display.setTextSize(1);
        display.setCursor(SCREEN_WIDTH - 40, 2);
        display.printf("Pump", pumpRunSec);

        display.setCursor(SCREEN_WIDTH - 40, 14);
        display.setTextSize(2);
        display.printf("%1.0f", pumpRunSec);
        display.setTextSize(1);
        display.print("S");
      }

      // live weight, bottom center - placeholder when nothing is arriving
      char weightBuf[12];
      if (bookooHasData) {
        snprintf(weightBuf, sizeof(weightBuf), "%0.1f g", bookooWeightG);
      } else {
        snprintf(weightBuf, sizeof(weightBuf), "--.- g");
      }
      display.setTextSize(2);
      display.setCursor((SCREEN_WIDTH - (int)strlen(weightBuf) * 12) / 2, 48);
      display.print(weightBuf);

      memcpy(timeBuffer, framebuffer, bufferSize);
      // XOR circle and time into the final framebuffer
      for (size_t i = 0; i < bufferSize; i++) {
        framebuffer[i] = circleBuffer[i] ^ timeBuffer[i];
      }
    } else {
      // draw dashboard

      // divider - start below IP if IP is showing
      bool showingIp =
          (WiFi.status() == WL_CONNECTED &&
           millis() - wifiConnectMillis < WIFI_IP_DISPLAY_DURATION);
      int dividerStartY = showingIp ? 10 : 0;
      display.drawLine(74, dividerStartY, 74, 63, SCREEN_WHITE);

      // draw last shot/extraction time
      display.setTextSize(4);
      display.setCursor(display.width() / 2 - 1 + 17, 20);
      display.printf("%02.0f", lastShotTimeSec);
      
      // scale rows, bottom right of the divider
      display.setTextSize(1);
      if (bookooLastShotValid) {
        display.setCursor(76, 49);
        display.print("Shot");
        display.setCursor(85, 49);
        display.printf("%0.0fs", bookooLastShotG);

        display.setCursor(85, 57);
        display.printf("%0.1f g", bookooLastShotG);
      } else {
        display.setCursor(76, 57);
        if (bookooHasData) {
          display.printf("%0.1f g", bookooWeightG);
        } else {
          display.print("--,--g");
        }
      }

      // Machine not responding for >1s - show coffee animation and time since
      // last contact This acts as a "going to sleep" / connection lost
      // indicator
      long lastValueUpdate = millis() - lastSerialUpdatedValue;
      if (lastValueUpdate > 1000) {
        // drawBitmap(x position, y position, bitmap data, bitmap width,
        // bitmap height, color)
        display.drawBitmap(0, 0, coffeeFrames[coffeeFrame], COFFEE_FRAME_WIDTH,
                           COFFEE_FRAME_HEIGHT, SCREEN_WHITE);
        coffeeFrame = (coffeeFrame + 1) % COFFEE_FRAME_COUNT;

        display.setTextSize(1);
        display.setCursor(display.width() - 20, 0);
        display.printf("%0.0f", float(lastValueUpdate / 1000));
      } else {
        // Draw WiFi indicator or IP address on right, aligned with C/S letter
        if (WiFi.status() == WL_CONNECTED) {
          if (millis() - wifiConnectMillis < WIFI_IP_DISPLAY_DURATION) {
            // Show IP address for 5 seconds after connecting
            display.setTextSize(1);
            display.setCursor(display.width() - 78, 0);
            display.print(WiFi.localIP());
          } else {
            display.drawBitmap(display.width() - 16, 0, wifiIcon,
                               WIFI_ICON_WIDTH, WIFI_ICON_HEIGHT, SCREEN_WHITE);
          }
        }

        // draw machine prio mode state icon
        if (coffeeSteamMode == "C") {
          display.drawBitmap(0, 0, coffeeBeanIcon, COFFEE_ICON_WIDTH,
                             COFFEE_ICON_HEIGHT, SCREEN_WHITE);
        } else if (coffeeSteamMode == "S") {
          // display.drawBitmap(0, 0, modeUnknownIcon,
          // MODE_UNKNOWN_ICON_WIDTH,
          //                    MODE_UNKNOWN_ICON_HEIGHT, SCREEN_WHITE);
        } else if (coffeeSteamMode == "X") {
          display.drawBitmap(0, 0, steamIcon, STEAM_ICON_WIDTH,
                             STEAM_ICON_HEIGHT, SCREEN_WHITE);
        }

        // draw heating mode
        if (isHeating) {
          display.drawBitmap(20, 1, tempIcon, TEMP_ICON_WIDTH, TEMP_ICON_HEIGHT,
                             SCREEN_WHITE);
        }
        if (isHeatingBoost) {
          display.drawBitmap(40, 1, tempIcon, TEMP_ICON_WIDTH, TEMP_ICON_HEIGHT,
                             SCREEN_WHITE);
        }

        // draw temperature
        if (temp.length() > 0) {
          display.setTextSize(3);
          display.setCursor(1, 20);
          display.print(temp);
          display.setTextSize(1);
          display.print((char)247); // ˚-char
          display.print("C");
        }

        // draw steam temperature
        if (steamTemp.length() > 0) {
          display.setTextSize(2);
          display.setCursor(1, 48);
          display.print(steamTemp);
          display.setTextSize(1);
          display.print((char)247); // ˚-char
          display.print("C");
        }
      }
    }
  }

  display.display();
}
