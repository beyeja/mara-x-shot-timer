# mara x shot timer

## notes

- fork and rewrite of https://github.com/alexrus/marax_timer
- written for ESP32C3
- needs ArduinoOTA Dependency
- 3d printed case will be added later

## initial setup

- initial upload to devboard needs to happen via usb
- add WIFI credentials to code
  - add `secrets.h` file and add the following:
  ```C++
  #define WIFI_SSID "your wifi name/ssid"
  #define WIFI_PW "your wifi password"
  #define OTA_PW "your password for OTA updates"
  ```
  - alternatively you can add your credentials directly in timer.ino but be sure to not commit them to git

## features

- OTA updates
- standby mode for display (oled burnin and stuff...)
- milliseconds timer when brewing

## Hardware

The following hardware is needed:

- ESP32C3 (or similar others)
  - can be adjusted for other boards by changing pins
- 1.3" OLED display (sh1106)
  - or other similar screens but library needs to be adjusted
- Reed sensor
  - Normally closed and normally opened works
- Wires
- 3D printed case (see 3dmodels folder for files or design your own)

## wiring

- TODO
