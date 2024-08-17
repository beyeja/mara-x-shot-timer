#include <ArduinoOTA.h>

#define D5 (21)                       // machine bus pin
#define D6 (20)                       // machine bus pin
#define D7 (4)                        // pump/reedswitch pin
#define PUMP_PIN D7                   // pump/reedswitch
#define TIME_SHOT_LIMIT 20            // limit in seconds when pump on is considered a shot

#define TIMER_INACTIVE 0              // value to be set when timer is inactive to not fall asleep again immediatly
#define SLEEP_TIME 1000 * 60 * 15     // time until sleep after wakeup/startup
#define MACHINE_SLEEP_TIME 1000 * 30  // time until sleep when machine is off

#define I2C_ADDRESS 0x3C  // 0x3c //0x3D
#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 64  // OLED display height, in pixels

// set to true/false when using another type of reed sensor
#define REED_OPEN true  // reed default open or default closed

#include <Adafruit_SSD1306.h>
#include <Adafruit_SH110X.h>
#include <splash.h>
#include <Adafruit_GFX.h>
#include <Wire.h>
#include <Timer.h>
#include <SoftwareSerial.h>
#include <ArduinoOTA.h>  // For enabling over-the-air updates
#include <WiFi.h>        // For connecting ESP32 to WiFi

#include "secrets.h"  // your secrets for wifi connection
#include "coffeeAnimation.h"

const char* ssid = WIFI_SSID;      // wifi name
const char* password = WIFI_PW;    // wifi pw
const char* otaPassword = OTA_PW;  // wifi pw

#define SCREEN_WHITE 1  // SSD1306_WHITE
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
// Adafruit_SH1106  display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
SoftwareSerial machineSerialInput(D5, D6);
Timer t;

// main states
int pumpOn = 0;                   // is pump on
bool displayOn = true;            // is display on
float pumpOnTimeSec = 0;          // time of pump on
long lastSerialUpdatedValue = 0;  // time of last update of machine serial
float lastShotTimeSec = 0;        // time of last pump on time considered a shot
bool isShotTimerMode = false;     // display is in shot timer mode
// machine message states
String coffeeSteamMode = "";
bool isHeating = false;
bool isHeatingBoost = false;
String temp = "";
String steamTemp = "";

long timerStartMillis = 0;
long timerStopMillis = 0;
long timerDisplayOffMillis = TIMER_INACTIVE;
long lastSerialUpdateMillis = 0;

// reading machine serial variables
const byte numChars = 32;
char receivedChars[numChars];
static byte ndx = 0;
const char endMarker = '\n';


void setup() {
  // Serial logging connection
  Serial.begin(115200);

  // setup wifi connection
  WiFi.disconnect();
  Serial.println("Wifi: Connecting...");
  // setup OTA updates
  WiFi.mode(WIFI_STA);
  WiFi.hostname("esp-lelit-mara-timer");
  WiFi.begin(ssid, password);  // Connect to WiFi - defaults to WiFi Station mode
  // Ensure WiFi is connected
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("not connected, trying to connect to wifi...");
    delay(500);
  }
  Serial.println("Wifi connected.");

  // setup ota
  ArduinoOTA.begin();  // Starts OTA
  ArduinoOTA.setHostname("esp-lelit-mara-timer");
  ArduinoOTA.setPassword(otaPassword);

  pinMode(PUMP_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // machine serial connection
  machineSerialInput.begin(9600);

  memset(receivedChars, 0, numChars);

  // set display pins
  // Wire.begin(6, 7); // 6 sck // 7 sda
  Wire.begin(7, 6);  // 6 sck // 7 sda

  delay(250);  // wait for the OLED to power up
  display.begin(0x3C, true);
  // display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SCREEN_WHITE);
  display.drawPixel(10, 10, SH110X_WHITE);
  display.display();

  t.every(32, updateDisplay);

  machineSerialInput.write(0x11);
}

void loop() {
  ArduinoOTA.handle();  // Handles a code update request

  t.update();

  detectSleep();
  detectPumpChanges();
  readMachineInput();
  evalMachineMessage();
  // display.dim(false);
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

  // check pump state
  pumpOn = REED_OPEN ? digitalRead(PUMP_PIN) : !digitalRead(PUMP_PIN);

  if (!isShotTimerMode && !pumpOn) {
    timerStartMillis = millis();
    isShotTimerMode = true;
    Serial.println("Start pump");
  }

  if (isShotTimerMode && pumpOn) {
    if (timerStopMillis == 0) {
      timerStopMillis = millis();
    }

    if (millis() - timerStopMillis > 500) {
      isShotTimerMode = false;
      timerStopMillis = 0;
      display.invertDisplay(false);
      Serial.println("Stop pump");
    }
  } else {
    timerStopMillis = 0;
  }
}

void detectSleep() {
  // I dont get this check
  if (!displayOn && millis() - lastSerialUpdatedValue <= MACHINE_SLEEP_TIME) {
    displayOn = true;

    // when woken up reset set last shot time to not cause sleep
    timerDisplayOffMillis = TIMER_INACTIVE;

    Serial.println("Wake up");
  }
  
  // update pump related display off timer when pump stops
  if (isShotTimerMode && pumpOn && millis() - timerStopMillis > 500) {
    timerDisplayOffMillis = millis();

    Serial.println("Pump stopped, update last pump stop time");
  }

  // go into sleep mode
  if (!isShotTimerMode && displayOn) {
    if (timerDisplayOffMillis != TIMER_INACTIVE && millis() - timerDisplayOffMillis >= SLEEP_TIME) {
      // go to sleep due to last shot time
    
      // reset values to be cleared after wakeup
      timerDisplayOffMillis = TIMER_INACTIVE;
      pumpOnTimeSec = 0;
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

void updatePumpOnTime() {
  if (isShotTimerMode) {
    pumpOnTimeSec = (float)(millis() - timerStartMillis) / 1000;

    // store last time when above threshold
    if (pumpOnTimeSec > TIME_SHOT_LIMIT) {
      lastShotTimeSec = pumpOnTimeSec;
    }
  } else {
    pumpOnTimeSec = lastShotTimeSec;
  }
}

void updateDisplay() {
  display.clearDisplay();

  if (displayOn) {
    updatePumpOnTime();

    if (isShotTimerMode) {
      double secFractions, seconds;
      secFractions = modf(pumpOnTimeSec, &seconds);
      secFractions = secFractions * 10;
      secFractions = static_cast<int>(secFractions);

      if (fmod(secFractions, 2) != 0) {
        secFractions = secFractions - 1;
        secFractions = secFractions < 0 ? 0 : secFractions;
      }

      // draw fullscreen time
      display.setTextSize(4);
      display.setCursor(20, 14);
      display.printf("%02.0f", seconds);

      display.setTextSize(2);
      display.printf(".%1.0f", secFractions);
      display.print("s");
    } else {
      // draw dashboard

      // divider
      display.drawLine(74, 0, 74, 63, SCREEN_WHITE);

      // draw time seconds
      display.setTextSize(4);
      display.setCursor(display.width() / 2 - 1 + 17, 20);
      display.printf("%02.0f", lastShotTimeSec);

      // show last machine serial update
      long lastValueUpdate = millis() - lastSerialUpdatedValue;
      if (lastValueUpdate > 1000) {
        // drawBitmap(x position, y position, bitmap data, bitmap width, bitmap height, color)
        display.drawBitmap(0, 0, coffeeFrames[coffeeFrame], COFFEE_FRAME_WIDTH, COFFEE_FRAME_HEIGHT, SCREEN_WHITE);
        coffeeFrame = (coffeeFrame + 1) % COFFEE_FRAME_COUNT;

        display.setTextSize(1);
        display.setCursor(display.width() - 20, 0);
        display.printf("%0.0f", float(lastValueUpdate / 1000));
      } else {
        // draw machine prio mode state C/S
        if (coffeeSteamMode.length() > 0) {
          display.setTextSize(2);
          display.setCursor(1, 1);
          display.print(coffeeSteamMode);
        }

        // draw heating mode
        if (isHeating) {
          // draw fill circle if heating on
          display.fillCircle(45, 7, 6, SCREEN_WHITE);
        } else {
          // draw empty circle if heating off
          display.drawCircle(45, 7, 6, SCREEN_WHITE);
        }
        if (isHeatingBoost) {
          // draw fill rectangle if heating on
          display.fillRect(51, 1, 12, 12, SCREEN_WHITE);
        } else {
          // draw empty rectangle if heating off
          display.drawRect(51, 1, 12, 12, SCREEN_WHITE);
        }

        // draw temperature
        if (temp.length() > 0) {
          display.setTextSize(3);
          display.setCursor(1, 20);
          display.print(temp);
          display.setTextSize(1);
          display.print((char)247);  // ˚-char
          display.print("C");
        }

        // draw steam temperature
        if (steamTemp.length() > 0) {
          display.setTextSize(2);
          display.setCursor(1, 48);
          display.print(steamTemp);
          display.setTextSize(1);
          display.print((char)247);  // ˚-char
          display.print("C");
        }
      }
    }
  }

  display.display();
}
