# mara x shot timer

## features

- show machine boiler and brew water temps
- shows machine heating mode
- auto start of timer
- milliseconds timer when brewing
- showing of last shot time
- standby mode for display (to avoid oled burnin and stuff...)
  - sleep countdown starts when machine stops sending messages
- OTA updates
- case for sticking to back of machine
- **MQTT integration** for home automation (see below for details)
- screen‑saver: automatic dimming, sleep and daily white‑flash to reduce OLED burn‑in

## MQTT Integration

The timer can publish state to an MQTT broker using the ArduinoHA library.
This is optional – if no broker address is provided, the sketch runs standalone.

To enable it, create a `secrets.h` file (see "initial setup" below) and add
these defines:

```c
#define MQTT_BROKER_ADDR "192.168.1.100"  // or hostname
#define MQTT_BROKER_PORT 1883
#define MQTT_USERNAME "user"            // optional
#define MQTT_PASSWORD "pass"            // optional
```

When connected the device exposes several sensors:

- `temperature` – boiler/brew water temperature (string)
- `steamTemp` – steam wand temperature (string)
- `lastShotTime` – duration of the most recent shot in seconds
- `pumpSensor` – `"ON"`/`"OFF"` on pump transitions
- `heating` – `"ON"`/`"OFF"` heating element state
- `heatingBoost` – indicates boost heating mode
- `machineMode` – `"C"`, `"S"` or `"X"` for coffee/steam/unknown
- `sleep` – `"OFF"` when display is awake, `"ON"` when asleep

Sensors update once per second and the device uses its unique ID
(e.g. `esp-mara-timer`) so they can be integrated easily with Home
Assistant or any other MQTT consumer.

For more info on the data format see the comments in `timer.ino`.

## Screen saver & burn‑in protection

The firmware includes several features designed to slow OLED aging:

- display dims to very low contrast 10 s after a wake event
- screen turns fully off after a configurable idle period (default 15 min)
- pump activity or mode changes immediately restore full brightness
- on cold boot the display flashes white for about 5 s to equalize wear
- at midnight the display also flashes white for a few minutes to even out
  ageing (this requires a working Wi‑Fi/NTP time; offline units will skip the
  midnight flash)

## notes

- fork and rewrite of https://github.com/alexrus/marax_timer
- written for ESP32C3
- needs ArduinoOTA Dependency

## initial setup

- install arduino IDE
  - install board support for esp32c3 dev module
- add WIFI credentials to code
  - add `secrets.h` file and add the following:

    ```C++
    #define WIFI_SSID "your wifi name/ssid"
    #define WIFI_PW "your wifi password"
    #define OTA_PW "your password for OTA updates"
    ```

  - alternatively you can add your credentials directly in timer.ino but be sure to not commit them to git

- initial upload to devboard needs to happen via usb
  build and upload the sketch

- connect

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
