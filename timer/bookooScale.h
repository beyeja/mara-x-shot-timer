#pragma once

// BOOKOO Themis Ultra espresso scale integration over Bluetooth Low Energy.
//
// Protocol: https://github.com/BooKooCode/OpenSource (bookoo_ultra_scale)
// Service 0x0FFE, weight+timer notifications on 0xFF11, commands on 0xFF12.

#include <Arduino.h>

// state exposed by the scale client
extern bool bookooConnected;     // BLE link up and notifications subscribed
extern bool bookooHasData;       // valid weight packet received recently
extern float bookooWeightG;      // last reported weight in grams
extern uint32_t bookooElapsedMs; // scale's own timer value in milliseconds
extern uint8_t bookooBatteryPct; // battery level 0-100

// finished-shot summary (latched at extraction stop, valid until the next
// extraction starts). from settlement events on scale firmware >= 4.0.0,
// otherwise latched from the live values when the scale timer freezes.
extern bool bookooLastShotValid;
extern uint32_t bookooLastShotMs;  // final extraction time
extern float bookooLastShotG;      // final extraction weight
extern uint16_t bookooAvgFlowCgS;  // avg flow rate in centi-g/s (0 legacy)

// extraction edges - each returns true exactly once per occurrence
bool bookooConsumeExtractionStarted();
bool bookooConsumeExtractionStopped();

void bookooSetup(); // init BLE and start scanning - call once from setup()
void bookooLoop();  // connect/reconnect state machine - call from loop()

// send a command frame to the scale (tare, timer control, ...).
// returns false when not currently connected.
bool bookooSendCommand(uint8_t cmd, uint8_t data1 = 0, uint8_t data2 = 0);
