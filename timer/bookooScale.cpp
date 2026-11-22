#include "bookooScale.h"

#include <BLE.h>

// BOOKOO Themis Ultra GATT layout
static const BLEUUID BOOKOO_SERVICE_UUID((uint16_t)0x0FFE);
static const BLEUUID BOOKOO_WEIGHT_CHAR_UUID((uint16_t)0xFF11);
static const BLEUUID BOOKOO_COMMAND_CHAR_UUID((uint16_t)0xFF12);

// how long without a notification before the value is considered stale
#define BOOKOO_DATA_TIMEOUT_MS 3000
// wait time before restarting a scan after a failed/lost connection
#define BOOKOO_RECONNECT_DELAY_MS 2000
// legacy fallback: scale timer not increasing for this long = extraction done
#define BOOKOO_EXTRACTION_STOP_MS 700

bool bookooConnected = false;
bool bookooHasData = false;
float bookooWeightG = 0;
uint32_t bookooElapsedMs = 0;
uint8_t bookooBatteryPct = 0;

bool bookooLastShotValid = false;
uint32_t bookooLastShotMs = 0;
float bookooLastShotG = 0;
uint16_t bookooAvgFlowCgS = 0;

static BLEClient client;
static BLERemoteCharacteristic commandChar;
static BTAddress scaleAddress;
static volatile bool doConnect = false;
static unsigned long lastPacketMillis = 0;
static unsigned long lostConnectionMillis = 0;

// extraction state machine
static bool sawAutoEvents = false;    // scale sent a 0x0D event (fw >= 4.0.0)
static bool runningFromEvents = false; // extraction running per 0x0D events
static unsigned long lastElapsedIncreaseMillis = 0; // legacy "timer running"
static bool prevExtractionRunning = false;
static bool extractionStartedEdge = false;
static bool extractionStoppedEdge = false;

// ---- notification parsing -------------------------------------------------

static uint32_t u24be(const uint8_t *p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

// automatic-mode event and settlement packets (scale firmware >= 4.0.0):
// [0x03, 0x0D, event, ms(u24), weight sign, weight cg(u24), result sign,
//  avg flow/ratio(u16), 5x00, xor]
// event: 00 extraction stopped (carries settlement data), 01 extraction
// started, 02 ready, 03 exit ready, 04 exit done
static void handleAutoModeEvent(const uint8_t *data) {
  sawAutoEvents = true;

  if (data[2] == 0x01) { // extraction started
    runningFromEvents = true;
    bookooLastShotValid = false;
  } else if (data[2] == 0x00) { // extraction stopped -> settlement data
    runningFromEvents = false;

    int32_t weightCg = (int32_t)u24be(&data[7]);
    if (data[6] == 0x2D) { // ASCII '-': negative weight
      weightCg = -weightCg;
    }
    bookooLastShotMs = u24be(&data[3]);
    bookooLastShotG = weightCg / 100.0f;
    bookooAvgFlowCgS = ((uint16_t)data[11] << 8) | data[12];
    bookooLastShotValid = true;
  }
}

// weight notification packets are 20 bytes:
// [0x03, 0x0B, elapsedMs(u24), unit, sign, weight cg(u24), flow sign,
//  flow cg/s(u16), battery, standby(2), beep, smoothing, 00, xor]
static void onNotification(BLERemoteCharacteristic chr, const uint8_t *data,
                           size_t len, BLESubscription kind) {
  if (len != 20 || data[0] != 0x03) {
    return;
  }

  // last byte is the XOR of all preceding bytes
  uint8_t checksum = 0;
  for (size_t i = 0; i < len - 1; i++) {
    checksum ^= data[i];
  }
  if (checksum != data[len - 1]) {
    return;
  }

  if (data[1] == 0x0D) { // automatic-mode event / settlement
    handleAutoModeEvent(data);
    bookooHasData = true;
    lastPacketMillis = millis();
    return;
  }

  if (data[1] != 0x0B) {
    return; // unknown packet (powder weight etc.)
  }

  int32_t weightCg = (int32_t)u24be(&data[7]);
  if (data[6] == 0x2D) { // ASCII '-': negative weight
    weightCg = -weightCg;
  }

  uint32_t newElapsedMs = u24be(&data[2]);
  // a strictly increasing scale timer means an extraction is running -
  // fallback detection for scales without 0x0D event packets
  if (newElapsedMs > bookooElapsedMs) {
    lastElapsedIncreaseMillis = millis();
  }
  bookooElapsedMs = newElapsedMs;
  bookooWeightG = weightCg / 100.0f;
  bookooBatteryPct = data[13];
  bookooHasData = true;
  lastPacketMillis = millis();
}

// ---- scan -----------------------------------------------------------------

// NOTE: runs in the BLE stack task - keep it lightweight. Blocking BLE
// operations (connect etc.) must not be called here, they are done from
// bookooLoop() instead.
static void onDeviceFound(BLEAdvertisedDevice device) {
  bool isBookoo = device.isAdvertisingService(BOOKOO_SERVICE_UUID);
  if (!isBookoo) {
    // fallback: match the advertised name ("BOOKOO_SC U xxxx" for the Ultra)
    String name = device.getName();
    isBookoo = name.indexOf("OOKOO_SC") >= 0;
  }
  if (!isBookoo) {
    return;
  }

  Serial.printf("Bookoo scale found: \"%s\" %s (RSSI %d)\n",
                device.getName().c_str(), device.getAddress().toString().c_str(),
                device.getRSSI());
  scaleAddress = device.getAddress();
  doConnect = true;
  BLE.getScan().stop();
}

static void startScan() {
  Serial.println("Bookoo: starting scan...");
  BLEScan scan = BLE.getScan();
  scan.setActiveScan(true);
  // low duty cycle (~19%): keeps the shared 2.4GHz radio (WiFi coexistence)
  // and the single CPU core responsive; still finds the scale in seconds
  scan.setInterval(160); // 160 * 0.625 ms = 100 ms
  scan.setWindow(30);    //  30 * 0.625 ms = ~19 ms
  scan.onResult(onDeviceFound);
  scan.start(0); // scan until stopped
}

// ---- connect --------------------------------------------------------------

static bool connectToScale() {
  Serial.printf("Bookoo: connecting to %s...\n",
                scaleAddress.toString().c_str());

  client = BLE.createClient();
  if (!client) {
    Serial.println("Bookoo: createClient failed");
    return false;
  }

  // note: connect/discovery can block for a few seconds - this mirrors the
  // existing "retry with cooldown" style of the sketch; it only happens on
  // (re)connect, never while a shot is running (link is up then)
  BTStatus status = client.connect(scaleAddress);
  if (!status) {
    Serial.printf("Bookoo: connect failed (%s)\n", status.toString());
    return false;
  }

  if (!client.discoverServices()) {
    Serial.println("Bookoo: service discovery failed");
    client.disconnect();
    return false;
  }

  BLERemoteService svc = client.getService(BOOKOO_SERVICE_UUID);
  if (!svc) {
    Serial.println("Bookoo: service 0x0FFE not found");
    client.disconnect();
    return false;
  }

  BLERemoteCharacteristic weightChar = svc.getCharacteristic(BOOKOO_WEIGHT_CHAR_UUID);
  if (!weightChar || !weightChar.canNotify()) {
    Serial.println("Bookoo: weight characteristic 0xFF11 not usable");
    client.disconnect();
    return false;
  }

  status = weightChar.subscribe(BLESubscription::Notify, onNotification);
  if (!status) {
    Serial.printf("Bookoo: subscribe failed (%s)\n", status.toString());
    client.disconnect();
    return false;
  }

  // optional, only needed for sending commands (tare etc.)
  commandChar = svc.getCharacteristic(BOOKOO_COMMAND_CHAR_UUID);

  Serial.println("Bookoo: connected and subscribed!");
  return true;
}

// ---- public API ------------------------------------------------------------

void bookooSetup() {
  BTStatus status = BLE.begin("mara-timer");
  if (!status) {
    Serial.printf("Bookoo: BLE init failed (%s)\n", status.toString());
    return;
  }
  startScan();
}

void bookooLoop() {
  // consider data stale after BOOKOO_DATA_TIMEOUT_MS without packets
  if (bookooHasData && millis() - lastPacketMillis > BOOKOO_DATA_TIMEOUT_MS) {
    bookooHasData = false;
  }

  // a scale was found while scanning -> connect
  if (doConnect) {
    doConnect = false;
    bookooConnected = connectToScale();
    if (!bookooConnected) {
      lostConnectionMillis = millis();
      startScan();
    }
    return;
  }

  // detect a lost link (poll-based, same style as the WiFi watchdog)
  if (bookooConnected && !client.isConnected()) {
    Serial.println("Bookoo: connection lost");
    bookooConnected = false;
    bookooHasData = false;
    runningFromEvents = false;
    lostConnectionMillis = millis();
    startScan();
  }

  // derive "extraction running": real 0x0D events when the scale sends them
  // (firmware >= 4.0.0), otherwise the legacy heuristic "timer is counting"
  bool extractionRunning;
  if (bookooConnected && bookooHasData) {
    if (sawAutoEvents) {
      extractionRunning = runningFromEvents;
    } else {
      extractionRunning =
          millis() - lastElapsedIncreaseMillis < BOOKOO_EXTRACTION_STOP_MS;
    }
  } else {
    extractionRunning = false;
  }

  if (extractionRunning != prevExtractionRunning) {
    if (extractionRunning) {
      Serial.println("Bookoo: extraction started");
      extractionStartedEdge = true;
      bookooLastShotValid = false;
    } else {
      Serial.println("Bookoo: extraction stopped");
      extractionStoppedEdge = true;
      // the 0x0D event path latches settlement data already; only latch the
      // live values as a fallback
      if (!bookooLastShotValid) {
        bookooLastShotMs = bookooElapsedMs;
        bookooLastShotG = bookooWeightG;
        bookooLastShotValid = true;
      }
    }
    prevExtractionRunning = extractionRunning;
  }
}

bool bookooConsumeExtractionStarted() {
  bool edge = extractionStartedEdge;
  extractionStartedEdge = false;
  return edge;
}

bool bookooConsumeExtractionStopped() {
  bool edge = extractionStoppedEdge;
  extractionStoppedEdge = false;
  return edge;
}

bool bookooSendCommand(uint8_t cmd, uint8_t data1, uint8_t data2) {
  if (!bookooConnected || !commandChar) {
    return false;
  }
  // command frames are 6 bytes: [0x03, 0x0A, cmd, data1, data2, xor]
  uint8_t frame[6] = {0x03, 0x0A, cmd, data1, data2, 0};
  frame[5] = frame[0] ^ frame[1] ^ frame[2] ^ frame[3] ^ frame[4];
  BTStatus status = commandChar.writeValue(frame, sizeof(frame), true);
  return (bool)status;
}
