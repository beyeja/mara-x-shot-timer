#pragma once

// Proof-of-concept integration for the BOOKOO Themis Ultra espresso scale
// via Bluetooth Low Energy.
//
// Protocol: https://github.com/BooKooCode/OpenSource (bookoo_ultra_scale)
// Service 0x0FFE, weight+timer notifications on 0xFF11, commands on 0xFF12.
//
// Set this to 0 to build without the BLE scale support (saves flash and
// allows building for the default partition scheme again).
#define BOOKOO_ENABLED 1

#include <Arduino.h>

#if BOOKOO_ENABLED

// state exposed by the scale client
extern bool bookooConnected;     // BLE link up and notifications subscribed
extern bool bookooHasData;       // valid weight packet received recently
extern float bookooWeightG;      // last reported weight in grams
extern uint32_t bookooElapsedMs; // scale's own timer value in milliseconds
extern uint8_t bookooBatteryPct; // battery level 0-100

void bookooSetup(); // init BLE and start scanning - call once from setup()
void bookooLoop();  // connect/reconnect state machine - call from loop()

// send a command frame to the scale (tare, timer control, ...).
// returns false when not currently connected.
bool bookooSendCommand(uint8_t cmd, uint8_t data1 = 0, uint8_t data2 = 0);

#endif // BOOKOO_ENABLED
