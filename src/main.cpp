// BLUETTI power station BLE controller (Elite 300, Handsfree 1, ...).
// Connects, runs the V2 encryption handshake, and polls SOC + AC/DC status
// over Modbus-over-BLE. When several stations are in range and none has been
// chosen yet, shows a picker; the choice is saved to flash.
//
// Runs on the Waveshare ESP32-S3-Touch-LCD-3.5 (320x480 portrait LCD, touch).
// Two tasks: the Bluetooth side (scan, connect, handshake, poll) runs in its own
// task on core 0, next to the NimBLE host, so its long waits never hold up the
// screen. loop() on core 1 is the UI: input, drawing and the screen fading. The
// UI sends the Bluetooth task commands; the Bluetooth task asks for redraws.
// Tap AC to toggle it. Swipe right (or press BOOT) for the station picker, swipe
// left for settings: screen timeout and a 180 degree flip.

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
static volatile State state = State::Scan;
static void waitTick();  // upkeep pumped while waiting on the station

// Shared between the Bluetooth task and the UI: the station list, the current
// station, the readings and the status message. Hold this while changing them
// (Bluetooth task) or drawing them (UI). Recursive, so helpers can nest.
static SemaphoreHandle_t dataLock;
struct Lock {
  bool held = true;
  Lock() { xSemaphoreTakeRecursive(dataLock, portMAX_DELAY); }
  ~Lock() { release(); }
  void release() {
    if (held) xSemaphoreGiveRecursive(dataLock);
    held = false;
  }
};
static volatile bool needRender = true;    // set by the Bluetooth task; the UI redraws
static volatile bool needSweep = false;    // fresh charge reading: run the highlight
static volatile bool pickerOpen = false;   // station picker showing
static void requestRender() { needRender = true; }
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
  Lock l;
  prefs.begin("bluetti", false);
  prefs.clear();
  prefs.end();
  current = Device();
}

// ---- settings, kept in their own namespace so forgetting the station keeps them ----
struct Timeout {
  uint32_t ms;  // 0 = never
  const char *label;
};
static const Timeout TIMEOUTS[] = {{15000, "15 SEC"}, {30000, "30 SEC"}, {60000, "1 MIN"}, {120000, "2 MIN"},
                                   {300000, "5 MIN"}, {600000, "10 MIN"}, {0, "NEVER"}};
static const int N_TIMEOUTS = sizeof(TIMEOUTS) / sizeof(TIMEOUTS[0]);
static const int DEFAULT_TIMEOUT = 1;  // 30 s
static const int NEVER = N_TIMEOUTS - 1;
// Three screen timeouts, indexes into TIMEOUTS: the usual one, and ones that
// apply instead while the station is charging or its AC is on. The defaults
// keep the old behaviour: 30 s, except never while AC is on.
struct Settings {
  int timeout = DEFAULT_TIMEOUT;
  int chargeTimeout = DEFAULT_TIMEOUT;
  int acTimeout = NEVER;
  bool flip = false;  // screen turned 180 degrees
};
static Settings settings;
static bool settingsOpen = false;  // settings page showing (over the status screen)

static void loadSettings() {
  prefs.begin("settings", false);
  settings.timeout = constrain(prefs.getUChar("timeout", DEFAULT_TIMEOUT), 0, N_TIMEOUTS - 1);
  settings.chargeTimeout = constrain(prefs.getUChar("chgTimeout", DEFAULT_TIMEOUT), 0, N_TIMEOUTS - 1);
  settings.acTimeout = constrain(prefs.getUChar("acTimeout", NEVER), 0, N_TIMEOUTS - 1);
  settings.flip = prefs.getBool("flip", false);
  prefs.end();
}

static void saveSettings() {
  prefs.begin("settings", false);
  prefs.putUChar("timeout", settings.timeout);
  prefs.putUChar("chgTimeout", settings.chargeTimeout);
  prefs.putUChar("acTimeout", settings.acTimeout);
  prefs.putBool("flip", settings.flip);
  prefs.end();
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
  requestRender();
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

// Split remaining time into value + unit, rounded to a whole number: minutes
// under three hours, hours under a day, then days. "95" "Minutes", "6" "Hours".
static void remainingParts(uint16_t minutes, String &value, String &unit) {
  long n;
  if (minutes < 3 * 60) {
    n = minutes;
    unit = "Minute";
  } else if (minutes < 24 * 60 - 30) {  // anything that rounds to 24 h shows as a day
    n = lround(minutes / 60.0);
    unit = "Hour";
  } else {
    n = lround(minutes / 1440.0);
    unit = "Day";
  }
  if (n != 1) unit += "s";
  value = String(n);
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
// Touch and the BOOT button become one of these. Menu: BOOT or an up / down
// swipe, opens or closes the picker (or closes settings). Left / Right: swipes,
// which move between the pages: picker | status | settings. ToggleAC: tap on the
// AC button. Choose: tap on picker row `index`. Setting: tap on settings row `index`.
struct Input {
  enum Kind { None, Menu, Left, Right, ToggleAC, Choose, Setting } kind = None;
  int index = -1;
};

static TouchEvent simTouch;  // touch injected over serial (debug)
static bool simTouchPending = false;

// ---- screen fading ----
// With AC on the screen stays lit. Otherwise it fades out after the timeout set
// on the settings page (30 s unless changed) without input. The first touch or
// press while it is fading or dark only brings it back. A small task does the
// fading.
static const uint32_t FADE_OUT_MS = 1500, FADE_IN_MS = 300;
static const uint8_t BRIGHT_ON = 160;  // backlight, 0-255

static volatile bool screenOn = true;  // target; the fade task follows it
static uint32_t lastActivity = 0;
static uint32_t seenDown = 0;  // last finger-down already counted as activity
static uint32_t wakeDown = 0;  // finger-down that woke the screen; its gesture is dropped

static void fadeTask(void *) {
  const uint32_t stepMs = 15;
  float level = 0;  // 0..1; starts dark, so the first frame fades in at power-up
  uint8_t applied = 0;
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

// The timeout that applies now, in ms (0 = never). While the station is charging
// or its AC is on, that setting replaces the usual one; with both, the longer.
static uint32_t screenTimeout() {
  bool live = state == State::Poll && status.valid;
  bool acOn = live && status.acOn == 1;
  bool charging = live && status.acIn + status.dcIn > status.acOut + status.dcOut;
  uint32_t ac = TIMEOUTS[settings.acTimeout].ms, chg = TIMEOUTS[settings.chargeTimeout].ms;
  if (acOn && charging) return (!ac || !chg) ? 0 : max(ac, chg);
  if (acOn) return ac;
  if (charging) return chg;
  return TIMEOUTS[settings.timeout].ms;
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
  uint32_t offAfter = screenTimeout();
  setScreen(!offAfter || millis() - lastActivity < offAfter);
}

// Light the AC button while a finger is on it, before we know it's a tap.
static void pressTick() {
  int16_t x, y;
  uint32_t down;
  bool want = state == State::Poll && !settingsOpen && !pickerOpen && screenOn && touch.held(x, y, down) && down != wakeDown && hitAC(x, y) &&
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
  if (e.kind == TouchEvent::Swipe) {
    if (abs(e.dx) > abs(e.dy)) in.kind = e.dx < 0 ? Input::Left : Input::Right;
    else in.kind = Input::Menu;
  } else if (pickerOpen) {
    Lock l;
    if (e.kind == TouchEvent::Tap) in.index = hitRow(e.x, e.y);
    if (in.index >= 0) in.kind = Input::Choose;
  } else if (settingsOpen) {
    if (e.kind == TouchEvent::Tap) in.index = hitSetting(e.x, e.y);
    if (in.index >= 0) in.kind = Input::Setting;
  } else if (hitAC(e.x, e.y)) {  // tap or hold
    in.kind = Input::ToggleAC;
  }
  return in;
}

static void setConn(const char *msg) {
  Lock l;
  connMsg = msg;
  requestRender();
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

  Lock l;
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
    requestRender();
    l.release();  // dropLink() can wait a couple of seconds
    dropLink();
    setState(State::Scan);
    return;
  }
  connMsg = (okA && okB && okC) ? "Connected" : "Read error";
  requestRender();
  if (okA) needSweep = true;  // fresh charge reading: run the highlight along the bar
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
  Lock l;
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
  Lock l;
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
  pickerOpen = true;
  // Keep a working link up so swiping back (or picking the same station) is
  // instant; anything half-done (connecting, handshaking) is dropped.
  pickedFromLink = state == State::Poll && client && client->isConnected();
  if (!pickedFromLink) dropLink();
  Lock l;
  cursor = 0;
  cursorMoved = false;
  if (rescan) startScan();
  // A connected station stops advertising, so the scan won't find it: list it.
  if (pickedFromLink && findDevice(current.addr) < 0) found.push_back(current);
  setState(State::Select);
  for (size_t i = 0; i < found.size(); i++)
    if (found[i].addr == current.addr) cursor = i;
  requestRender();
}

// Leave the picker for the station we were on, as we left it.
static void leaveSelect() {
  stopScan();
  pickerOpen = false;
  if (pickedFromLink && client && client->isConnected()) {
    Serial.println("[ui] back to the station");
    setState(State::Poll);
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
  Lock l;
  pickerOpen = false;
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

// Runs in the Bluetooth task. The UI has already flipped the button (so the tap
// feels instant) from `was` to `target`; this sends it and puts the button back
// if the station doesn't take it. The pill only mentions the change if the
// station is slow to echo it (see slowTick), so a normal toggle doesn't flash it.
static void toggleAC(int target, int was) {
  const char *msg = target ? "AC turning on..." : "AC turning off...";
  Serial.printf("[ctrl] %s\n", msg);
  slowMsg = msg;
  slowSince = millis();
  bool ok = writeRegister(2011, target);
  slowSince = 0;
  if (ok) {
    if (connMsg != "Connected") setConn("Connected");  // trust the echo until the next poll
  } else {
    Serial.println("[ctrl] AC write failed");
    {
      Lock l;
      status.acOn = was;  // put the button back
    }
    setConn("AC write failed");
  }
  // Re-read in ~1.5 s; an immediate read can still return the old state.
  lastPoll = millis() - POLL_INTERVAL_MS + 1500;
}

// ---- settings page ----
static void applyFlip() {
  lcd.setRotation(settings.flip ? 2 : 0);
  touch.setFlip(settings.flip);
  touch.clear();  // anything queued was measured the other way up
}

static void waitTick() { slowTick(); }

// ---- commands from the UI to the Bluetooth task ----
struct Cmd {
  enum Kind : uint8_t { OpenPicker, ClosePicker, Choose, ToggleAC } kind;
  int16_t a, b;  // Choose: row. ToggleAC: target, previous state
};
static QueueHandle_t cmdQueue;

static void send(Cmd::Kind k, int a = 0, int b = 0) {
  Cmd c{k, (int16_t)a, (int16_t)b};
  xQueueSend(cmdQueue, &c, 0);
}

static void handleCmd(const Cmd &c) {
  switch (c.kind) {
    case Cmd::OpenPicker:
      if (state != State::Select) enterSelect(true);
      break;
    case Cmd::ClosePicker:
      if (state == State::Select) leaveSelect();
      break;
    case Cmd::Choose: {
      if (state != State::Select) break;
      Device d;
      {
        Lock l;
        if (c.a < 0 || c.a >= (int)found.size()) break;
        d = found[c.a];
      }
      choose(d, true);
      break;
    }
    case Cmd::ToggleAC:
      if (state == State::Poll) toggleAC(c.a, c.b);
      break;
  }
}

// Wait up to ms for a command; true (and the command left queued) if one came.
static bool waitForCmd(uint32_t ms) {
  Cmd c;
  return xQueuePeek(cmdQueue, &c, pdMS_TO_TICKS(ms)) == pdTRUE;
}

// ---- UI navigation (loop(), core 1) ----
static void openSettings(bool open) {
  Serial.printf("[ui] settings %s\n", open ? "open" : "closed");
  settingsOpen = open;
  requestRender();
}

static void changeSetting(int row) {
  if (row == SET_TIMEOUT) settings.timeout = (settings.timeout + 1) % N_TIMEOUTS;
  if (row == SET_CHARGE) settings.chargeTimeout = (settings.chargeTimeout + 1) % N_TIMEOUTS;
  if (row == SET_AC) settings.acTimeout = (settings.acTimeout + 1) % N_TIMEOUTS;
  if (row == SET_FLIP) {
    settings.flip = !settings.flip;
    applyFlip();
  }
  saveSettings();
  Serial.printf("[ui] timeout %s, charging %s, AC on %s, flip %s\n", TIMEOUTS[settings.timeout].label,
                TIMEOUTS[settings.chargeTimeout].label, TIMEOUTS[settings.acTimeout].label, settings.flip ? "on" : "off");
  requestRender();
}

static bool haveStation() {
  Lock l;
  return current.valid();
}

static void openPicker() {
  settingsOpen = false;
  pickerOpen = true;  // show it now; the Bluetooth task catches up
  {
    Lock l;
    cursorMoved = false;
  }
  // A connect attempt can take 10 s to time out: cancel it rather than wait.
  if (state == State::Connect) ble_gap_conn_cancel();
  send(Cmd::OpenPicker);
  requestRender();
}

static void handleInput(const Input &in) {
  if (in.kind == Input::None) return;
  if (pickerOpen) {
    if (in.kind == Input::Choose) {
      {
        Lock l;
        cursor = in.index;
        cursorMoved = true;
      }
      requestRender();  // the tapped row lights while it connects
      send(Cmd::Choose, in.index);
    } else if ((in.kind == Input::Menu || in.kind == Input::Left) && haveStation()) {
      pickerOpen = false;  // back to the station, unchanged
      send(Cmd::ClosePicker);
      requestRender();
    }
    return;
  }
  if (settingsOpen) {
    if (in.kind == Input::Right || in.kind == Input::Menu) openSettings(false);
    else if (in.kind == Input::Setting) changeSetting(in.index);
    return;
  }
  if (in.kind == Input::Left) openSettings(true);
  else if (in.kind == Input::Right || in.kind == Input::Menu) openPicker();
  else if (in.kind == Input::ToggleAC && state == State::Poll) {
    Lock l;
    if (!status.valid || status.acOn < 0) {
      Serial.println("[ctrl] AC state unknown, ignoring tap");
      return;
    }
    int was = status.acOn, target = was ? 0 : 1;
    status.acOn = target;  // flip the button now; the Bluetooth task sends it
    acPressed = false;
    send(Cmd::ToggleAC, target, was);
    requestRender();
  }
}

static void bleTask(void *);

void setup() {
  // Backlight off first thing: until the panel has been initialised and drawn,
  // anything it shows is noise or white. The first frame then fades in.
  pinMode(6, OUTPUT);
  digitalWrite(6, LOW);
  dataLock = xSemaphoreCreateRecursiveMutex();
  loadPreferred();
  loadSettings();
  bool lcdOk = lcd.start();  // clears the panel to black
  lcd.setBrightness(0);
  canvas.setPsram(true);  // 320x480x2 = 300 KB
  canvas.setColorDepth(16);
  canvas.createSprite(lcd.width(), lcd.height());
  bool touchOk = touch.begin();
  applyFlip();
  loadFonts();
  render();
  xTaskCreatePinnedToCore(fadeTask, "fade", 3072, nullptr, 1, nullptr, 1);

  // Give a serial monitor a moment to attach, now the screen is up.
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("\nBLUETTI controller " FW_BUILD);
  if (!lcdOk) Serial.println("[lcd] init failed");
  if (!touchOk) Serial.println("[touch] FT6336 not found");
  if (current.valid())
    Serial.printf("preferred device: %s (%s)\n", current.addr.c_str(), current.name.c_str());

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

  cmdQueue = xQueueCreate(8, sizeof(Cmd));
  // Core 0, beside the NimBLE host task. The handshake's ECDH and signature
  // checks want a roomy stack.
  xTaskCreatePinnedToCore(bleTask, "ble", 16 * 1024, nullptr, 1, nullptr, 0);
}

// Debug over serial: 'S' dumps the screen as raw RGB565, 'D' drops the BLE link,
// 'P' + 3 digits previews a charge level (e.g. P042) until the next poll,
// 'B' presses BOOT, 'F' forgets the saved device and reboots,
// 'X' + 6 digits taps at x,y (e.g. X100080), 'W' swipes right (picker),
// 'L' swipes left (settings).
static void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'D' && client && client->isConnected()) client->disconnect();  // test reconnect UI
    if (c == 'B') bootSimulated = true;
    if (c == 'W' || c == 'L') {
      simTouch = {TouchEvent::Swipe, (int16_t)(canvas.width() / 2), (int16_t)(canvas.height() / 2), millis(),
                  (int16_t)(c == 'W' ? 100 : -100), 0};
      lastActivity = millis();
      simTouchPending = true;
    }
    if (c == 'X') {
      char d[7] = {0};
      if (Serial.readBytes(d, 6) == 6) {
        int y = atoi(d + 3);
        d[3] = 0;
        simTouch = {TouchEvent::Tap, (int16_t)atoi(d), (int16_t)y, millis(), 0, 0};
        lastActivity = millis();
        simTouchPending = true;
      }
    }
    if (c == 'T') {  // 'T0' / 'T1': show sample readings, discharging / charging
      int ch = Serial.read();
      Lock l;
      status.valid = true;
      status.soc = 64;
      status.acOn = 0;
      status.dcOn = 1;
      bool charging = ch == '1';
      status.minutes = charging ? 324 : 20160;
      status.acIn = charging ? 412 : 0;
      status.dcIn = 0;
      status.dcOut = 18;
      status.acOut = 0;
      connMsg = "Connected";  // draw them as live
      requestRender();
    }
    if (c == 'I') {  // list devices on the I2C bus
      for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) Serial.printf("[i2c] device at 0x%02x\n", a);
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
        Lock l;
        status.soc = atoi(d);
        requestRender();
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

// ---- Bluetooth task (core 0) ----
static void bleStep() {
  Cmd cmd;
  if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
    handleCmd(cmd);
    return;
  }
  switch (state) {
    case State::Scan: {
      if (!NimBLEDevice::getScan()->isScanning()) {
        setConn("Scanning...");
        startScan();
      }
      drainAdverts();
      int i;
      Device d;
      {
        Lock l;
        i = current.valid() ? findDevice(current.addr) : -1;
        if (i >= 0) d = found[i];
      }
      if (i >= 0) {
        // The saved (or last used) station is in range: always prefer it.
        if (!d.name.length()) d.name = current.name;
        choose(d, false);
      } else if (!current.valid() && millis() - scanStarted > SCAN_DECIDE_MS) {
        // Nothing chosen yet: use the only station around, or let the user pick.
        size_t n;
        {
          Lock l;
          n = found.size();
          if (n == 1) d = found[0];
        }
        if (n == 1) choose(d, false);
        else if (n > 1) enterSelect(false);
      }
      waitForCmd(20);
      break;
    }

    case State::Select:
      if (drainAdverts()) requestRender();
      waitForCmd(20);
      break;

    case State::Connect:
      if (doConnect()) {
        Serial.println("[ble] waiting for handshake");
        setConn("Handshake...");
        setState(State::Handshake);
      } else {
        waitForCmd(2000);  // a command (e.g. the picker opening) cuts this short
        if (state == State::Connect) setState(State::Scan);
      }
      break;

    case State::Handshake: {
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
      waitForCmd(5);
      break;
    }

    case State::Poll:
      if (!client->isConnected()) {
        Serial.println("[ble] disconnected");
        setConn("Disconnected");
        setState(State::Scan);
        break;
      }
      if (lastPoll == 0 || millis() - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = millis();
        poll();
      }
      waitForCmd(20);
      break;
  }
}

static void bleTask(void *) {
  for (;;) bleStep();
}

// ---- UI (loop(), core 1) ----
void loop() {
  handleSerial();
  updateScreen();
  handleInput(takeInput());
  pressTick();
  if (needSweep) {
    needSweep = false;
    if (screenOn && !settingsOpen && !pickerOpen) startSweep();
  }
  sweepTick();
  if (needRender) {
    needRender = false;
    Lock l;
    render();
  }
  delay(10);
}
