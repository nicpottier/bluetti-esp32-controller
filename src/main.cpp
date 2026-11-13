// BLUETTI power station BLE controller (Elite 300, Handsfree 1, ...).
// Connects, runs the V2 encryption handshake, and polls SOC + AC/DC status
// over Modbus-over-BLE. When several stations are in range and none has been
// chosen yet, shows a picker; the choice is saved to flash.
//
// Runs on the Waveshare ESP32-S3-Touch-LCD-3.5 (320x480 portrait LCD, touch).
// Tap AC to toggle it; swipe or press BOOT to open the station picker.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <vector>

#include "bluetti_crypto.h"
#include "display.h"
#include "touch.h"

// LovyanGFX copies each glyph's bitmap onto the stack; the 150 px charge digits
// need ~9 KB each, more than the default 8 KB loop task stack.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

static const NimBLEUUID SVC_UUID((uint16_t)0xFF00);
static const NimBLEUUID NOTIFY_UUID((uint16_t)0xFF01);
static const NimBLEUUID WRITE_UUID((uint16_t)0xFF02);

static const uint32_t POLL_INTERVAL_MS = 5000;
static const uint32_t RESPONSE_TIMEOUT_MS = 3000;
static const uint32_t CHALLENGE_WAIT_MS = 4000;  // no challenge by then => unencrypted device
static const uint32_t HANDSHAKE_TIMEOUT_MS = 20000;
static const uint32_t SESSION_SETTLE_MS = 1500;

// ---- notification queue (BLE host task -> loop) ----
struct Chunk {
  uint16_t len;
  uint8_t data[512];
};
static QueueHandle_t rxQueue;

static void onNotify(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  Chunk c;
  c.len = min(len, sizeof(c.data));
  memcpy(c.data, data, c.len);
  if (xQueueSend(rxQueue, &c, 0) != pdTRUE) log_w("rx queue full, dropped %u bytes", len);
}

// ---- state ----
enum class State { Scan, Select, Connect, Handshake, Poll };
static State state = State::Scan;
static void waitTick();  // UI upkeep pumped while waiting on the station
static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *writeChr = nullptr;
static BluettiCrypto crypto;
static bool encrypted = false;
static bool challengeSeen = false;
static uint32_t stateSince = 0;
static uint32_t lastPoll = 0;

// ---- BOOT button (active low): opens the station picker ----
// The ISR only latches "an edge happened" so a short press isn't missed while a
// BLE transaction blocks loop(). takePress() then confirms it's a real press
// (still held after 30 ms, i.e. not release bounce) and requires a release
// before it can fire again.
static const int PIN_BOOT = 0;
static volatile bool bootEdge = false;
static bool bootArmed = true;       // false until released after a press
static bool bootSimulated = false;  // press injected over serial (debug)
static void IRAM_ATTR onBoot() { bootEdge = true; }

static bool takePress() {
  if (bootSimulated) {
    bootSimulated = false;
    return true;
  }
  if (digitalRead(PIN_BOOT) == HIGH) bootArmed = true;
  if (!bootEdge) return false;
  bootEdge = false;
  if (!bootArmed) return false;
  delay(30);
  if (digitalRead(PIN_BOOT) != LOW) return false;  // bounce, not a press
  bootArmed = false;
  return true;
}

// Forget a press that had no meaning (e.g. it only woke the screen).
static void discardPress() {
  bootEdge = false;
  bootSimulated = false;
  if (digitalRead(PIN_BOOT) == LOW) bootArmed = false;
}

// ---- devices ----
struct Device {
  String addr;  // "aa:bb:cc:dd:ee:ff"
  uint8_t type = BLE_ADDR_PUBLIC;
  String name;  // advertised name, e.g. "EL3001234567890123" (may be empty)
  int rssi = 0;
  bool valid() const { return addr.length() > 0; }
};
static Device current;               // device we connect to (saved choice or the only one in range)
static std::vector<Device> found;    // devices seen since the scan started
static int cursor = 0;               // highlighted row on the picker
static bool cursorMoved = false;     // user has tapped a row since the picker opened
static bool pickedFromLink = false;  // picker opened over a working link, which is kept up
static Preferences prefs;

static void loadPreferred() {
  prefs.begin("bluetti", false);  // read-write so a first boot creates the namespace instead of logging an error
  current.addr = prefs.getString("addr", "");
  current.type = prefs.getUChar("type", BLE_ADDR_PUBLIC);
  current.name = prefs.getString("name", "");
  prefs.end();
}

static void savePreferred(const Device &d) {
  prefs.begin("bluetti", false);
  prefs.putString("addr", d.addr);
  prefs.putUChar("type", d.type);
  prefs.putString("name", d.name);
  prefs.end();
}

static void forgetPreferred() {
  prefs.begin("bluetti", false);
  prefs.clear();
  prefs.end();
  current = Device();
}

// "EL3001234567890123" -> "ELITE 300", "Handsfree 19876543210987" -> "HANDSFREE 1".
// Names are model + 13-digit serial.
static String modelOf(const String &name) {
  String m = name;
  if (m.length() > 13) {
    bool digits = true;
    for (size_t i = m.length() - 13; i < m.length(); i++) digits = digits && isdigit(m[i]);
    if (digits) m = m.substring(0, m.length() - 13);
  }
  m.trim();
  m.toUpperCase();
  if (m.startsWith("EL") && m.length() > 2 && isdigit(m[2])) {
    String rest = m.substring(2);
    rest.replace("V2", " V2");
    m = "ELITE " + rest;
  }
  return m;
}

static String serialOf(const String &name) {
  if (name.length() > 13) return name.substring(name.length() - 13);
  return "";
}

// Display label for a picker row: model, falling back to the address tail.
static String deviceLabel(const Device &d) {
  if (d.name.length()) return modelOf(d.name);
  return "BLUETTI " + d.addr.substring(12);
}

static void setState(State s) {
  state = s;
  stateSince = millis();
}

static String hex(const uint8_t *p, size_t n) {
  String s;
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02x", p[i]);
    s += b;
  }
  return s;
}

static void bleWrite(const Bytes &b) {
  bool withResponse = writeChr->canWrite();
  if (!writeChr->writeValue(b.data(), b.size(), withResponse))
    Serial.printf("[ble] write of %u bytes failed\n", b.size());
}

// ---- Modbus ----
static uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

static Bytes readRequest(uint16_t addr, uint16_t count) {
  Bytes r = {0x01, 0x03, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(count >> 8), (uint8_t)count};
  uint16_t c = crc16(r.data(), r.size());
  r.push_back(c & 0xFF);
  r.push_back(c >> 8);
  return r;
}

// Collect one Modbus response, decrypting as needed. Returns true on a full frame.
static Bytes plainBuf;
static bool pumpRx(Bytes &frame) {
  Chunk c;
  while (xQueueReceive(rxQueue, &c, 0) == pdTRUE) {
    if (!encrypted && !challengeSeen && c.len >= 2 && c.data[0] == '*' && c.data[1] == '*') {
      challengeSeen = true;
      encrypted = true;
    }
    if (encrypted) {
      Bytes out;
      switch (crypto.feed(c.data, c.len, out)) {
        case BluettiCrypto::Result::Reply:
          bleWrite(out);
          break;
        case BluettiCrypto::Result::Ready:
          Serial.println("[ble] encrypted session ready");
          setState(State::Poll);
          // Give the device time to switch to the session key before the first
          // request; sending immediately sometimes leaves the session silent.
          lastPoll = millis() - POLL_INTERVAL_MS + SESSION_SETTLE_MS;
          break;
        case BluettiCrypto::Result::Data:
          frame = std::move(out);
          return true;
        case BluettiCrypto::Result::Error:
          Serial.println("[ble] handshake/decrypt error, disconnecting");
          client->disconnect();
          break;
        default:
          break;
      }
    } else {
      plainBuf.insert(plainBuf.end(), c.data, c.data + c.len);
      if (plainBuf.size() >= 5) {
        size_t need = (plainBuf[1] & 0x80) ? 5 : plainBuf[1] == 0x06 ? 8 : 5 + plainBuf[2];
        if (plainBuf.size() >= need) {
          frame.assign(plainBuf.begin(), plainBuf.begin() + need);
          plainBuf.erase(plainBuf.begin(), plainBuf.begin() + need);
          return true;
        }
      }
    }
  }
  return false;
}

// Send a Modbus request and wait for a CRC-valid, non-exception response.
static bool transact(const Bytes &req, Bytes &frame) {
  uint16_t addr = (req[2] << 8) | req[3];
  // Discard anything left over from a previous (timed out / partial) exchange.
  Bytes stale;
  while (pumpRx(stale)) {}
  plainBuf.clear();
  if (encrypted) crypto.clearBuffer();
  bleWrite(encrypted ? crypto.encrypt(req) : req);
  uint32_t t0 = millis();
  while (millis() - t0 < RESPONSE_TIMEOUT_MS) {
    if (!client->isConnected()) return false;
    if (pumpRx(frame)) {
      if (frame.size() < 5 || crc16(frame.data(), frame.size() - 2) !=
                                  (frame[frame.size() - 2] | (frame[frame.size() - 1] << 8))) {
        Serial.printf("[modbus] bad frame for %u: %s\n", addr, hex(frame.data(), frame.size()).c_str());
        return false;
      }
      if (frame[1] & 0x80) {
        Serial.printf("[modbus] exception 0x%02x (fn %u, reg %u)\n", frame[2], req[1], addr);
        return false;
      }
      return true;
    }
    waitTick();
    delay(5);
  }
  Serial.printf("[modbus] timeout (fn %u, reg %u), %u bytes buffered\n", req[1], addr,
                encrypted ? crypto.buffered() : plainBuf.size());
  return false;
}

// Blocking register read. Returns register payload (2 bytes per register).
static bool readRegisters(uint16_t addr, uint16_t count, Bytes &payload) {
  Bytes frame;
  if (!transact(readRequest(addr, count), frame)) return false;
  payload.assign(frame.begin() + 3, frame.end() - 2);
  return payload.size() == count * 2u;
}

// Modbus function 06: write single register. Device echoes the request.
static bool writeRegister(uint16_t addr, uint16_t value) {
  Bytes req = {0x01, 0x06, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(value >> 8), (uint8_t)value};
  uint16_t c = crc16(req.data(), req.size());
  req.push_back(c & 0xFF);
  req.push_back(c >> 8);
  Bytes frame;
  if (!transact(req, frame)) return false;
  return frame.size() == 8 && frame[1] == 0x06 && ((frame[4] << 8) | frame[5]) == value;
}

static uint16_t u16(const Bytes &p, uint16_t base, uint16_t reg) {
  size_t i = (reg - base) * 2;
  return (p[i] << 8) | p[i + 1];
}

static String swapString(const Bytes &p, size_t off, size_t n) {
  String s;
  for (size_t i = off; i + 1 < off + n; i += 2) {
    if (p[i + 1]) s += (char)p[i + 1];
    if (p[i]) s += (char)p[i];
  }
  return s;
}

static const char *onOff(uint16_t v) { return v == 1 ? "ON" : v == 0 ? "OFF" : "?"; }

// ---- display ----
struct Status {
  bool valid = false;
  String model;
  uint16_t soc = 0, minutes = 0;
  uint16_t dcOut = 0, acOut = 0, dcIn = 0, acIn = 0;
  int acOn = -1, dcOn = -1;  // -1 unknown
};
static Status status;
static String connMsg = "Starting...";
static Display lcd;
static LGFX_Sprite canvas(&lcd);  // each screen is drawn here, then pushed in one go

// Split remaining time into value + unit: "9.9" "Days" above 24 h, else "5.4" "Hours".
static void remainingParts(uint16_t minutes, String &value, String &unit) {
  char buf[12];
  if (minutes >= 24 * 60) {
    snprintf(buf, sizeof(buf), "%.1f", minutes / 1440.0f);
    unit = "Days";
  } else {
    snprintf(buf, sizeof(buf), "%.1f", minutes / 60.0f);
    unit = "Hours";
  }
  value = buf;
}

static String formatRemaining(uint16_t minutes) {
  String v, u;
  remainingParts(minutes, v, u);
  return v + " " + u;
}

// ---- UI ----
#include "ui.inc"
static Touch touch;

// ---- input ----
// Touch and the BOOT button become one of these. Menu: open the picker (or move
// to the next station on it). ToggleAC: tap on the AC button. Choose: tap on
// picker row `index`.
struct Input {
  enum Kind { None, Menu, ToggleAC, Choose } kind = None;
  int index = -1;
};

static TouchEvent simTouch;  // touch injected over serial (debug)
static bool simTouchPending = false;

// ---- screen fading ----
// With AC on the screen stays lit. Otherwise it fades out after 30 s without
// input. The first touch or press while it is fading or dark only brings it back.
// A small task does the fading, so it stays smooth while loop() waits on BLE.
static const uint32_t OFF_AFTER_MS = 30000;
static const uint32_t FADE_OUT_MS = 1500, FADE_IN_MS = 300;
static const uint8_t BRIGHT_ON = 160;  // backlight, 0-255

static volatile bool screenOn = true;  // target; the fade task follows it
static uint32_t lastActivity = 0;
static uint32_t seenDown = 0;  // last finger-down already counted as activity
static uint32_t wakeDown = 0;  // finger-down that woke the screen; its gesture is dropped

static void fadeTask(void *) {
  const uint32_t stepMs = 15;
  float level = 1;  // 0..1, the setup brightness
  uint8_t applied = BRIGHT_ON;
  for (;;) {
    float target = screenOn ? 1 : 0;
    if (level != target) {
      float step = float(stepMs) / (screenOn ? FADE_IN_MS : FADE_OUT_MS);
      level = screenOn ? min(1.0f, level + step) : max(0.0f, level - step);
      // Squared, so the fade looks even to the eye rather than dropping off early.
      uint8_t b = lroundf(BRIGHT_ON * level * level);
      if (b != applied) {
        lcd.setBrightness(b);
        applied = b;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(stepMs));
  }
}

static void setScreen(bool on) {
  if (on == screenOn) return;
  Serial.printf("[screen] %s\n", on ? "fade in" : "fade out");
  screenOn = on;
}

// Called every loop: notices input as soon as it starts (a finger landing, BOOT
// going down) and picks the target. Input that wakes a dark screen is dropped.
static void updateScreen() {
  bool dark = !screenOn;
  bool active = false;
  if (bootEdge || digitalRead(PIN_BOOT) == LOW) {
    if (dark) discardPress();
    active = true;
  }
  uint32_t d = touch.lastDown();
  if (d != seenDown) {
    seenDown = d;
    if (dark) wakeDown = d;
    active = true;
  }
  if (active) lastActivity = millis();
  bool acOn = status.valid && status.acOn == 1;
  setScreen(acOn || millis() - lastActivity < OFF_AFTER_MS);
}

// Light the AC button while a finger is on it, before we know it's a tap. Cheap
// to call often; also runs inside BLE waits so a poll in flight doesn't delay it.
static void pressTick() {
  int16_t x, y;
  uint32_t down;
  bool want = state == State::Poll && screenOn && touch.held(x, y, down) && down != wakeDown && hitAC(x, y) &&
              status.valid && status.acOn >= 0;
  if (want == acPressed) return;
  acPressed = want;
  pushACButton();
}

static Input takeInput() {
  Input in;
  if (takePress()) {
    in.kind = Input::Menu;
    return in;
  }
  TouchEvent e;
  if (simTouchPending) {
    e = simTouch;
    simTouchPending = false;
  } else if (!touch.take(e)) {
    return in;
  }
  if (wakeDown && e.down == wakeDown) {
    Serial.println("[touch] ignored (woke the screen)");
    return in;
  }
  static const char *kinds[] = {"tap", "hold", "swipe"};
  Serial.printf("[touch] %s at %d,%d\n", kinds[e.kind], e.x, e.y);
  if (e.kind == TouchEvent::Swipe) {  // swipe opens the station picker
    in.kind = Input::Menu;
  } else if (state == State::Select) {
    if (e.kind == TouchEvent::Tap) in.index = hitRow(e.x, e.y);
    if (in.index >= 0) in.kind = Input::Choose;
  } else if (hitAC(e.x, e.y)) {  // tap or hold
    in.kind = Input::ToggleAC;
  }
  return in;
}

// Drop input that arrived while it had no meaning (e.g. during a connect).
static void discardInput() {
  touch.clear();
  simTouchPending = false;
}

static void setConn(const char *msg) {
  connMsg = msg;
  render();
}

// Disconnect and wait for the link to actually go down (disconnect() is async).
static void dropLink() {
  if (!client || !client->isConnected()) return;
  client->disconnect();
  for (int i = 0; i < 100 && client->isConnected(); i++) delay(20);
}

static int failedPolls = 0;
static bool sessionHasData = false;  // reset on each new connection
static const int MAX_FAILED_POLLS = 3;  // then drop the link and reconnect

static void poll() {
  Bytes a, b, c;
  bool okA = readRegisters(100, 20, a);   // 102 SOC, 104 time remaining, 110 model
  // A fresh session that can't answer the first read is dead; don't wait out two more timeouts.
  bool skip = !okA && !sessionHasData;
  bool okB = !skip && readRegisters(140, 8, b);    // DC out, AC out, DC in, AC in power
  bool okC = !skip && readRegisters(2011, 2, c);   // AC / DC output switches (only AC is controlled; DC powers this board)

  Serial.printf("---------------- %s ----------------\n", deviceLabel(current).c_str());
  if (okA) {
    status.model = swapString(a, (110 - 100) * 2, 12);
    status.soc = u16(a, 100, 102);
    status.minutes = u16(a, 100, 104);  // minutes at current load
    Serial.printf("Model:            %s\n", status.model.c_str());
    Serial.printf("State of charge:  %u %%\n", status.soc);
    Serial.printf("Time remaining:   %s\n", formatRemaining(status.minutes).c_str());
    Serial.printf("  raw 100-119: %s\n", hex(a.data(), a.size()).c_str());
  }
  if (okB) {
    status.dcOut = u16(b, 140, 140);
    status.acOut = u16(b, 140, 142);
    status.dcIn = u16(b, 140, 144);
    status.acIn = u16(b, 140, 146);
    Serial.printf("DC output power:  %u W\n", status.dcOut);
    Serial.printf("AC output power:  %u W\n", status.acOut);
    Serial.printf("DC input power:   %u W\n", status.dcIn);
    Serial.printf("AC input power:   %u W\n", status.acIn);
  }
  if (okC) {
    uint16_t ac = u16(c, 2011, 2011), dc = u16(c, 2011, 2012);
    status.acOn = ac <= 1 ? ac : -1;
    status.dcOn = dc <= 1 ? dc : -1;
    Serial.printf("AC inverter:      %s (raw %u)\n", onOff(ac), ac);
    Serial.printf("DC output:        %s (raw %u)\n", onOff(dc), dc);
  }
  status.valid = status.valid || okA;
  bool any = okA || okB || okC;
  failedPolls = any ? 0 : failedPolls + 1;
  // Occasionally a session completes the handshake but the device never answers.
  // If a fresh session's first poll gets nothing, retry right away.
  bool deadSession = !any && !sessionHasData;
  sessionHasData = sessionHasData || any;
  if (deadSession || failedPolls >= MAX_FAILED_POLLS) {
    Serial.println("[ble] reads keep failing, reconnecting");
    failedPolls = 0;
    connMsg = "Reconnecting...";
    render();
    dropLink();
    setState(State::Scan);
    return;
  }
  connMsg = (okA && okB && okC) ? "Connected" : "Read error";
  render();
  if (okA && screenOn) startSweep();  // fresh charge reading: run the highlight along the bar
}

// ---- scanning / connecting ----
// Scanning runs in the background (NimBLE host task); adverts from anything that
// looks like a BLUETTI station are queued here and merged into `found` by loop().
static const uint32_t SCAN_DECIDE_MS = 5000;  // listen this long before auto-picking / showing the picker

struct Advert {
  char addr[18];
  char name[40];
  uint8_t type;
  int8_t rssi;
};
static QueueHandle_t advQueue;
static uint32_t scanStarted = 0;

static bool isBluettiName(const std::string &n) {
  static const char *prefixes[] = {"EL", "AC", "EB", "EP", "PR", "AP", "Handsfree"};
  for (auto p : prefixes)
    if (n.rfind(p, 0) == 0 && n.size() > 8) return true;
  return false;
}

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *d) override {
    std::string name = d->getName();
    if (!isBluettiName(name) && !d->isAdvertisingService(SVC_UUID)) return;
    Advert a = {};
    strlcpy(a.addr, d->getAddress().toString().c_str(), sizeof(a.addr));
    strlcpy(a.name, name.c_str(), sizeof(a.name));
    a.type = d->getAddress().getType();
    a.rssi = d->getRSSI();
    xQueueSend(advQueue, &a, 0);
  }
};
static ScanCB scanCB;

static void startScan() {
  found.clear();
  xQueueReset(advQueue);
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan->isScanning()) scan->stop();
  scan->clearResults();
  scan->start(0, nullptr, false);  // until stopped
  scanStarted = millis();
  Serial.println("[ble] scanning...");
}

static void stopScan() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan->isScanning()) scan->stop();
  scan->clearResults();
}

static int findDevice(const String &addr) {
  for (size_t i = 0; i < found.size(); i++)
    if (found[i].addr == addr) return i;
  return -1;
}

// Merge queued adverts into `found`. Returns true if the list changed visibly.
static bool drainAdverts() {
  bool changed = false;
  Advert a;
  while (xQueueReceive(advQueue, &a, 0) == pdTRUE) {
    int i = findDevice(a.addr);
    if (i < 0) {
      Device d;
      d.addr = a.addr;
      d.type = a.type;
      // Scan responses (which carry the name) sometimes go missing; reuse a name we know.
      d.name = a.name[0] ? String(a.name) : d.addr == current.addr ? current.name : String();
      found.push_back(d);
      i = found.size() - 1;
      changed = true;
      Serial.printf("  found %s '%s' rssi=%d\n", a.addr, a.name, a.rssi);
    } else if (a.name[0] && found[i].name != a.name) {
      found[i].name = a.name;
      changed = true;
    }
    found[i].rssi = a.rssi;
    // Until the user moves it, keep the cursor on the station we were using.
    if (!cursorMoved && found[i].addr == current.addr) cursor = i;
  }
  return changed;
}

// Show the picker. rescan=false keeps the scan (and list) already in progress.
static void enterSelect(bool rescan) {
  Serial.println("[ui] device picker");
  // Keep a working link up so swiping back (or picking the same station) is
  // instant; anything half-done (connecting, handshaking) is dropped.
  pickedFromLink = state == State::Poll && client && client->isConnected();
  if (!pickedFromLink) dropLink();
  cursor = 0;
  cursorMoved = false;
  if (rescan) startScan();
  // A connected station stops advertising, so the scan won't find it: list it.
  if (pickedFromLink && findDevice(current.addr) < 0) found.push_back(current);
  setState(State::Select);
  for (size_t i = 0; i < found.size(); i++)
    if (found[i].addr == current.addr) cursor = i;
  render();
}

// Leave the picker for the station we were on, as we left it.
static void leaveSelect() {
  stopScan();
  if (pickedFromLink && client && client->isConnected()) {
    Serial.println("[ui] back to the station");
    setState(State::Poll);
    render();
  } else {
    setState(State::Connect);  // the link went down meanwhile (or was never up): reconnect
  }
}

static void choose(const Device &d, bool save) {
  if (d.addr == current.addr && pickedFromLink && client && client->isConnected()) {
    leaveSelect();
    return;
  }
  dropLink();
  stopScan();
  if (d.addr != current.addr) status = Status();  // don't show the old station's numbers
  current = d;
  if (save) {
    savePreferred(d);
    Serial.printf("[ui] saved preferred device %s (%s)\n", d.addr.c_str(), d.name.c_str());
  }
  setState(State::Connect);
}

static bool doConnect() {
  setConn("Connecting...");
  Serial.printf("[ble] connecting to %s (%s)\n", current.name.c_str(), current.addr.c_str());
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setConnectTimeout(10);
  }
  if (!client->connect(NimBLEAddress(current.addr.c_str(), current.type))) {
    Serial.println("[ble] connect failed");
    return false;
  }
  Serial.printf("[ble] connected, MTU %u\n", client->getMTU());
  NimBLERemoteService *svc = client->getService(SVC_UUID);
  if (!svc) {
    Serial.println("[ble] service FF00 not found");
    client->disconnect();
    return false;
  }
  NimBLERemoteCharacteristic *notifyChr = svc->getCharacteristic(NOTIFY_UUID);
  writeChr = svc->getCharacteristic(WRITE_UUID);
  if (!notifyChr || !writeChr) {
    Serial.println("[ble] characteristics not found");
    client->disconnect();
    return false;
  }
  xQueueReset(rxQueue);
  crypto.reset();
  sessionHasData = false;
  failedPolls = 0;
  encrypted = false;
  challengeSeen = false;
  plainBuf.clear();
  // Acknowledged CCCD write: the Handsfree 1 ignores an unacknowledged one and never
  // sends the encryption challenge (the Elite 300 accepts either).
  if (!notifyChr->subscribe(true, onNotify, true)) {
    Serial.println("[ble] subscribe failed");
    client->disconnect();
    return false;
  }
  return true;
}

// While an AC write is in flight: after SLOW_MS without an answer, say so in the pill.
static const uint32_t SLOW_MS = 600;
static uint32_t slowSince = 0;  // 0 = nothing in flight
static const char *slowMsg = "";
static void slowTick() {
  if (slowSince && millis() - slowSince > SLOW_MS && connMsg != slowMsg) setConn(slowMsg);
}

static void toggleAC() {
  if (status.acOn < 0) {
    Serial.println("[ctrl] AC state unknown, ignoring tap");
    return;
  }
  uint16_t target = status.acOn ? 0 : 1;
  const char *msg = target ? "AC turning on..." : "AC turning off...";
  Serial.printf("[ctrl] %s\n", msg);
  // Flip the button straight away so the tap feels instant. The pill only
  // mentions the change if the station is slow to echo it (see slowTick), so a
  // normal toggle doesn't flash it.
  int was = status.acOn;
  status.acOn = target;
  acPressed = false;
  render();
  slowMsg = msg;
  slowSince = millis();
  bool ok = writeRegister(2011, target);
  slowSince = 0;
  if (ok) {
    if (connMsg != "Connected") setConn("Connected");  // trust the echo until the next poll
  } else {
    Serial.println("[ctrl] AC write failed");
    status.acOn = was;  // put the button back
    setConn("AC write failed");
  }
  // Re-read in ~1.5 s; an immediate read can still return the old state.
  lastPoll = millis() - POLL_INTERVAL_MS + 1500;
}

static void waitTick() {
  pressTick();
  slowTick();
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("\nBLUETTI controller");
  loadPreferred();
  if (current.valid())
    Serial.printf("preferred device: %s (%s)\n", current.addr.c_str(), current.name.c_str());

  if (!lcd.start()) Serial.println("[lcd] init failed");
  lcd.setBrightness(BRIGHT_ON);
  canvas.setPsram(true);  // 320x480x2 = 300 KB
  canvas.setColorDepth(16);
  canvas.createSprite(lcd.width(), lcd.height());
  if (!touch.begin()) Serial.println("[touch] FT6336 not found");
  loadFonts();
  render();
  xTaskCreatePinnedToCore(fadeTask, "fade", 3072, nullptr, 1, nullptr, 1);

  pinMode(PIN_BOOT, INPUT_PULLUP);
  attachInterrupt(PIN_BOOT, onBoot, FALLING);

  rxQueue = xQueueCreate(16, sizeof(Chunk));
  advQueue = xQueueCreate(32, sizeof(Advert));
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCB, true);  // repeats keep RSSI and late names fresh
  scan->setMaxResults(0);                             // report via callback only, don't store
  scan->setActiveScan(true);                          // names arrive in scan responses
  scan->setInterval(100);
  scan->setWindow(99);
}

// Debug over serial: 'S' dumps the screen as raw RGB565, 'D' drops the BLE link,
// 'P' + 3 digits previews a charge level (e.g. P042) until the next poll,
// 'B' presses BOOT, 'F' forgets the saved device and reboots,
// 'X' + 6 digits taps at x,y (e.g. X100080), 'W' swipes.
static void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'D' && client && client->isConnected()) client->disconnect();  // test reconnect UI
    if (c == 'B') bootSimulated = true;
    if (c == 'W') {
      simTouch = {TouchEvent::Swipe, (int16_t)(canvas.width() / 2), (int16_t)(canvas.height() / 2), millis()};
      lastActivity = millis();
      simTouchPending = true;
    }
    if (c == 'X') {
      char d[7] = {0};
      if (Serial.readBytes(d, 6) == 6) {
        int y = atoi(d + 3);
        d[3] = 0;
        simTouch = {TouchEvent::Tap, (int16_t)atoi(d), (int16_t)y, millis()};
        lastActivity = millis();
        simTouchPending = true;
      }
    }
    if (c == 'F') {
      forgetPreferred();
      Serial.println("[ui] forgot preferred device, rebooting");
      Serial.flush();
      ESP.restart();
    }
    if (c == 'P') {
      char d[4] = {0};
      if (Serial.readBytes(d, 3) == 3) {
        status.soc = atoi(d);
        render();
      }
    }
    if (c != 'S') continue;
    size_t len = canvas.width() * canvas.height() * 2;
    Serial.printf("SHOT %d %d %u\n", canvas.width(), canvas.height(), len);
    // Write in chunks and retry: one large write drops bytes on USB CDC.
    const uint8_t *p = (const uint8_t *)canvas.getBuffer();
    while (len) {
      size_t n = Serial.write(p, min(len, (size_t)4096));
      p += n;
      len -= n;
      if (!n) delay(2);
    }
    Serial.flush();
  }
}

void loop() {
  handleSerial();
  updateScreen();
  switch (state) {
    case State::Scan: {
      if (!NimBLEDevice::getScan()->isScanning()) {
        setConn("Scanning...");
        startScan();
      }
      drainAdverts();
      if (takeInput().kind == Input::Menu) {
        enterSelect(true);
        break;
      }
      int i = current.valid() ? findDevice(current.addr) : -1;
      if (i >= 0) {
        // The saved (or last used) station is in range: always prefer it.
        Device d = found[i];
        if (!d.name.length()) d.name = current.name;
        choose(d, false);
      } else if (!current.valid() && millis() - scanStarted > SCAN_DECIDE_MS) {
        // Nothing chosen yet: use the only station around, or let the user pick.
        if (found.size() == 1) choose(found[0], false);
        else if (found.size() > 1) enterSelect(false);
      }
      delay(20);
      break;
    }

    case State::Select: {
      bool changed = drainAdverts();
      Input in = takeInput();
      if (in.kind == Input::Menu) {  // swipe or BOOT: back, unchanged
        if (current.valid()) {
          leaveSelect();
          break;
        }
      }
      if (in.kind == Input::Choose && in.index < (int)found.size()) {
        cursor = in.index;
        cursorMoved = true;
        render();  // show the tapped row highlighted while connecting
        choose(found[in.index], true);
        break;
      }
      if (changed) render();
      delay(20);
      break;
    }

    case State::Connect:
      discardInput();
      if (doConnect()) {
        Serial.println("[ble] waiting for handshake");
        setConn("Handshake...");
        setState(State::Handshake);
      } else {
        delay(2000);
        setState(State::Scan);
      }
      break;

    case State::Handshake: {
      if (takeInput().kind == Input::Menu) {
        enterSelect(true);
        break;
      }
      Bytes ignored;
      pumpRx(ignored);
      if (!client->isConnected()) {
        Serial.println("[ble] disconnected during handshake");
        setState(State::Scan);
      } else if (!challengeSeen && millis() - stateSince > CHALLENGE_WAIT_MS) {
        Serial.println("[ble] no challenge, assuming unencrypted device");
        setState(State::Poll);
        lastPoll = 0;
      } else if (millis() - stateSince > HANDSHAKE_TIMEOUT_MS) {
        Serial.println("[ble] handshake timeout");
        dropLink();
        setState(State::Scan);
      }
      delay(5);
      break;
    }

    case State::Poll: {
      if (!client->isConnected()) {
        Serial.println("[ble] disconnected");
        setConn("Disconnected");
        setState(State::Scan);
        break;
      }
      Input in = takeInput();
      if (in.kind == Input::Menu) {
        enterSelect(true);
        break;
      }
      if (in.kind == Input::ToggleAC) toggleAC();
      pressTick();
      sweepTick();
      if (lastPoll == 0 || millis() - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = millis();
        poll();
      }
      for (int i = 0; i < 4; i++) {  // 20 ms, watching for a press
        pressTick();
        delay(5);
      }
      break;
    }
  }
}
