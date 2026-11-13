// UI for the 320x480 portrait LCD. Included from main.cpp.
// Layout follows tools/mock_rect.py: a segmented charge bar across the top, charge
// and time remaining in the middle, inputs, then AC / DC output buttons (tap AC to
// toggle it) and a status bar at the bottom. The picker lists stations as rows.
#include "fonts.h"

// Palette: LCARS layout with a modern flat colour set. RGB565 via rgb().
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t L_FRAME = rgb(245, 158, 11);    // amber: top elbow, heading
static const uint16_t L_BAR = rgb(71, 85, 105);       // slate: heading bar
static const uint16_t L_AC = rgb(56, 189, 248);       // sky: AC
static const uint16_t L_DC = rgb(167, 139, 250);      // violet: DC
static const uint16_t L_WARN = rgb(252, 211, 77);     // amber-300: searching etc.
static const uint16_t L_RED = rgb(244, 63, 94);       // rose: errors, low battery
static const uint16_t L_LABEL = rgb(148, 163, 184);   // slate-400: field labels
static const uint16_t L_EMPTY = rgb(30, 41, 59);      // slate-800: unfilled segments
static const uint16_t L_OFF = rgb(51, 65, 85);        // slate-700: inactive block / off output
static const uint16_t L_STALE = rgb(71, 85, 105);     // slate-600: stale readings
static const uint16_t L_TEXT = rgb(241, 245, 249);    // slate-100

struct SmoothFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  void load(const uint8_t *vlw) {
    data.set(vlw);
    font.loadFont(&data);
  }
};
static void text(const String &s, int x, int y, SmoothFont &f, uint16_t color, textdatum_t datum) {
  canvas.setFont(&f.font);
  canvas.setTextColor(color);
  canvas.setTextDatum(datum);
  canvas.drawString(s, x, y);
}

static int textW(const String &s, SmoothFont &f) {
  canvas.setFont(&f.font);
  return canvas.textWidth(s);
}

// ---- status mapping ----
static String lcarsStatus(const String &m, uint16_t &color) {
  color = L_WARN;
  if (m == "Connected") { color = L_BAR; return "LINK ACTIVE"; }  // calm slate when all is well
  if (m.startsWith("AC turning")) {
    color = L_AC;
    return m.endsWith("on...") ? "AC TURNING ON" : "AC TURNING OFF";
  }
  if (m.indexOf("fail") >= 0 || m.indexOf("error") >= 0) color = L_RED;
  if (m == "Starting...") return "INITIALIZING";
  if (m == "Scanning...") return "SEARCHING";
  if (m == "Connecting...") return "CONNECTING";
  if (m == "Handshake...") return "SECURING LINK";
  if (m == "Disconnected") return "LINK LOST";
  if (m == "Reconnecting...") return "RECONNECTING";
  if (m == "Read error") return "READ ERROR";
  if (m.endsWith("write failed")) return "AC COMMAND FAILED";
  String u = m;
  u.toUpperCase();
  u.replace("...", "");
  return u;
}

// Charge colour: blends rose -> orange -> amber -> emerald as the battery fills.
static uint16_t socColor(int soc) {
  struct Stop { int at; uint8_t r, g, b; };
  static const Stop stops[] = {
      {0, 244, 63, 94},     // rose
      {20, 249, 115, 22},   // orange
      {45, 251, 191, 36},   // amber
      {75, 52, 211, 153},   // emerald
      {100, 52, 211, 153},
  };
  soc = constrain(soc, 0, 100);
  for (size_t i = 1; i < sizeof(stops) / sizeof(stops[0]); i++) {
    if (soc <= stops[i].at) {
      const Stop &a = stops[i - 1], &b = stops[i];
      float t = float(soc - a.at) / (b.at - a.at);
      return rgb(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t);
    }
  }
  return rgb(52, 211, 153);
}

static SmoothFont fSoc, fPct, fTime, fBtn, fVal40, fModel, fVal30, f22, f20, fHdr, f18, f17;

static void loadFonts() {
  fSoc.load(PF_SOC);
  fPct.load(PF_PCT);
  fTime.load(PF_TIME);
  fBtn.load(PF_BTN);
  fVal40.load(PF_VAL40);
  fModel.load(PF_MODEL);
  fVal30.load(PF_VAL30);
  f22.load(PF_22);
  f20.load(PF_20);  // "W" only
  fHdr.load(PF_HDR);
  f18.load(PF_18);
  f17.load(PF_17);
}

// ---- geometry ----
static const int W = 320, CX = W / 2;
static const int MARGIN = 10;
static const int BTN_Y0 = 354, BTN_Y1 = 434;           // output buttons
static const int BAR_Y0 = 444, BAR_Y1 = 470;           // status bar
static const int ROW_Y0 = 52, ROW_H = 84, ROW_STEP = 92, ROWS = 4;  // picker
static const int RADIUS = 13;  // every rounded corner; bars are 26 tall, so they are pills
static const int GAUGE_Y0 = 10, GAUGE_Y1 = 36;

// Rounded rectangle (x1, y1 exclusive) with a choice of which corners are rounded.
enum Corners : uint8_t {
  RC_TL = 1, RC_TR = 2, RC_BR = 4, RC_BL = 8,
  RC_ALL = 15, RC_LEFT = RC_TL | RC_BL, RC_RIGHT = RC_TR | RC_BR,
};
static void box(int x0, int y0, int x1, int y1, int r, uint16_t color, uint8_t round = RC_ALL) {
  int w = x1 - x0, h = y1 - y0;
  r = min(r, min(w, h) / 2);
  canvas.fillRoundRect(x0, y0, w, h, r, color);
  if (!(round & RC_TL)) canvas.fillRect(x0, y0, r, r, color);
  if (!(round & RC_TR)) canvas.fillRect(x1 - r, y0, r, r, color);
  if (!(round & RC_BR)) canvas.fillRect(x1 - r, y1 - r, r, r, color);
  if (!(round & RC_BL)) canvas.fillRect(x0, y1 - r, r, r, color);
}

// Blend an RGB565 colour towards white by t (0..1).
static uint16_t lighten(uint16_t c, float t) {
  uint8_t r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
  return rgb(r + (255 - r) * t, g + (255 - g) * t, b + (255 - b) * t);
}

// 20 segments across the top, with rounded outer ends. `head` is the position
// (in segments) of the sweep highlight; it brightens the lit segments it passes.
static const int GAUGE_SEGS = 20;
static const float SWEEP_WIDTH = 2.0f;   // highlight half-width, in segments
static const float SWEEP_LIGHT = 0.22f;  // peak blend towards white; kept subtle
static void gauge(int soc, uint16_t fill, float head = -100) {
  const int gap = 3, x0 = MARGIN, x1 = W - MARGIN;
  const float w = float(x1 - x0 - gap * (GAUGE_SEGS - 1)) / GAUGE_SEGS;
  int lit = soc * GAUGE_SEGS / 100;
  if (soc > 0 && lit == 0) lit = 1;
  for (int i = 0; i < GAUGE_SEGS; i++) {
    int a = x0 + lroundf(i * (w + gap)), b = x0 + lroundf(i * (w + gap) + w);
    uint8_t round = i == 0 ? RC_LEFT : i == GAUGE_SEGS - 1 ? RC_RIGHT : 0;
    uint16_t c = L_EMPTY;
    if (i < lit) {
      float glow = max(0.0f, 1 - fabsf(head - i) / SWEEP_WIDTH);
      c = glow > 0 ? lighten(fill, SWEEP_LIGHT * glow) : fill;
    }
    box(a, GAUGE_Y0, b, GAUGE_Y1, RADIUS, c, round);
  }
}

// ---- new-reading sweep: a highlight runs along the lit charge segments ----
static const uint32_t SWEEP_SEG_MS = 50;  // per segment, so a full bar takes about 1 s
static uint32_t sweepStart = 0;           // 0 = not sweeping
static int sweepSoc = 0;
static uint16_t sweepColor = 0;
static bool sweepBack = false;            // discharging: run right to left

static void startSweep() { sweepStart = millis() ? millis() : 1; }

// Advance the sweep, redrawing only the top strip of the screen. Call often.
static void sweepTick() {
  if (!sweepStart) return;
  float head = float(millis() - sweepStart) / SWEEP_SEG_MS - 2;
  int lit = max(1, sweepSoc * GAUGE_SEGS / 100);
  bool done = head > lit + 2.5f;
  if (sweepBack) head = lit - 1 - head;  // mirror: start past the last lit segment
  gauge(sweepSoc, sweepColor, done ? -100 : head);
  // The strip spans the full width, so its rows are contiguous in the sprite.
  lcd.pushImage(0, 0, W, GAUGE_Y1 + 4, (const lgfx::swap565_t *)canvas.getBuffer());
  if (done) sweepStart = 0;
}

// Value with a smaller "W" after it: centred on x, or right-aligned to x.
static void watts(int x, int y, const String &v, SmoothFont &vf, SmoothFont &uf, uint16_t color, bool center = true) {
  int vw = textW(v, vf), uw = textW("W", uf);
  int x0 = center ? x - (vw + 3 + uw) / 2 : x - (vw + 3 + uw);
  text(v, x0, y, vf, color, baseline_left);
  text("W", x0 + vw + 3, y, uf, color, baseline_left);
}

// Pill across the bottom: status on the left, station on the right, black text.
static void statusBar(const String &left, const String &right, uint16_t color) {
  box(MARGIN, BAR_Y0, W - MARGIN, BAR_Y1, RADIUS, color);
  int base = (BAR_Y0 + BAR_Y1) / 2 + 6;
  text(left, MARGIN + 16, base, f17, TFT_BLACK, baseline_left);
  text(right, W - MARGIN - 16, base, f17, TFT_BLACK, baseline_right);
}

static const float PRESS_LIGHT = 0.35f;  // how much a finger on a button lightens it
static bool acPressed = false;

// LCARS output button: rounded on the outer side, square where the two meet.
static void outButton(int x0, int x1, const char *label, int on, uint16_t col, const String &w, bool left,
                      bool pressed = false) {
  uint16_t fill = on == 1 ? col : L_OFF;
  if (pressed) fill = lighten(fill, PRESS_LIGHT);
  box(x0, BTN_Y0, x1, BTN_Y1, RADIUS, fill, left ? RC_LEFT : RC_RIGHT);
  uint16_t ink = on == 1 ? (uint16_t)TFT_BLACK : L_LABEL;
  int base = BTN_Y1 - 16;
  text(label, left ? x0 + 18 : x0 + 14, base, fBtn, ink, baseline_left);
  watts(left ? x1 - 14 : x1 - 18, base, w, fVal30, f20, ink, false);
}

static void acButton() {
  bool valid = status.valid;
  outButton(MARGIN, CX - 3, "AC", valid ? status.acOn : -1, L_AC, valid ? String(status.acOut) : String("-"), true,
            acPressed);
}

// Redraw just the AC button and push only its rows to the panel.
static void pushACButton() {
  acButton();
  const int y0 = BTN_Y0, h = BTN_Y1 - BTN_Y0 + 1;  // full-width rows are contiguous in the sprite
  lcd.pushImage(0, y0, W, h, (const lgfx::swap565_t *)canvas.getBuffer() + y0 * W);
}

static void renderStatus() {
  canvas.fillScreen(TFT_BLACK);

  bool valid = status.valid;
  bool normal = connMsg == "Connected";
  bool busy = connMsg.startsWith("AC turning");
  bool stale = valid && !normal && !busy;
  uint16_t stColor;
  String st = lcarsStatus(connMsg, stColor);
  uint16_t accent = !valid || stale ? L_STALE : socColor(status.soc);
  uint16_t value = !valid || stale ? L_STALE : L_TEXT;

  gauge(valid ? status.soc : 0, accent);
  sweepSoc = valid ? status.soc : 0;
  sweepColor = accent;
  sweepBack = valid && status.acIn + status.dcIn <= status.acOut + status.dcOut;  // idle drains too

  // Charge, with time remaining centred below it.
  const int cb = 196;
  String pct = valid ? String(status.soc) : String("--");
  int sw = textW(pct, fSoc), pw = textW("%", fPct);
  int x = CX - (sw + 4 + pw) / 2;
  text(pct, x, cb, fSoc, accent, baseline_left);
  text("%", x + sw + 4, cb, fPct, accent, baseline_left);
  if (valid) {
    String tv, tu;
    remainingParts(status.minutes, tv, tu);
    tu.toUpperCase();
    int vw = textW(tv, fTime), uw = textW(tu, f22);
    x = CX - (vw + 8 + uw) / 2;
    text(tv, x, cb + 50, fTime, value, baseline_left);
    text(tu, x + vw + 8, cb + 50, f22, accent, baseline_left);
  }

  // Inputs
  struct In { int cx; const char *label; uint16_t col; uint16_t w; };
  In ins[] = {{85, "AC IN", L_AC, status.acIn}, {235, "DC IN", L_DC, status.dcIn}};
  for (auto &in : ins) {
    text(in.label, in.cx, 290, f22, in.col, baseline_center);
    uint16_t col = !valid || stale ? L_STALE : in.w ? L_TEXT : L_LABEL;
    watts(in.cx, 334, valid ? String(in.w) : String("-"), fVal40, f22, col);
  }

  // Outputs
  int dcOn = valid ? status.dcOn : -1;
  acButton();
  outButton(CX + 3, W - MARGIN, "DC", dcOn, L_DC, valid ? String(status.dcOut) : String("-"), false);

  statusBar(st, current.valid() ? deviceLabel(current) : String(""), stColor);
  canvas.pushSprite(0, 0);
}

// ---- device picker: one row per station; tap a row to connect ----
static int pickerFirst() {
  int n = found.size();
  return constrain(cursor - (ROWS - 1), 0, max(0, n - ROWS));
}

static void renderSelect() {
  canvas.fillScreen(TFT_BLACK);
  int n = found.size();
  box(MARGIN, 10, W - MARGIN, 36, RADIUS, n ? L_FRAME : L_WARN);
  text(n ? "SELECT STATION" : "SEARCHING", 26, 30, fHdr, TFT_BLACK, baseline_left);
  int first = pickerFirst();
  for (int r = 0; r < ROWS && first + r < n; r++) {
    int i = first + r;
    bool sel = i == cursor;
    int y = ROW_Y0 + r * ROW_STEP;
    box(MARGIN, y, W - MARGIN, y + ROW_H, RADIUS, sel ? L_FRAME : L_EMPTY);
    text(deviceLabel(found[i]), 32, y + 46, fModel, sel ? (uint16_t)TFT_BLACK : L_TEXT, baseline_left);
    String sn = serialOf(found[i].name);
    if (sn.length()) text(sn, 32, y + 72, f18, sel ? (uint16_t)TFT_BLACK : L_LABEL, baseline_left);
  }
  if (!n) text("LOOKING FOR STATIONS", CX, 240, f22, L_LABEL, baseline_center);
  if (n) statusBar("TAP TO CONNECT", String(n) + " FOUND", L_FRAME);
  else statusBar("SWIPE TO RESCAN", "", L_BAR);
  canvas.pushSprite(0, 0);
}

static void render() {
  if (state == State::Select) renderSelect();
  else renderStatus();
}

// ---- touch targets (screen coordinates) ----
static bool hitAC(int x, int y) {
  return x < CX && y >= BTN_Y0 - 6 && y < BTN_Y1 + 6;
}

// Picker row under (x, y), or -1.
static int hitRow(int x, int y) {
  int first = pickerFirst();
  for (int r = 0; r < ROWS && first + r < (int)found.size(); r++) {
    int y0 = ROW_Y0 + r * ROW_STEP;
    if (y >= y0 - 4 && y < y0 + ROW_H + 4) return first + r;
  }
  return -1;
}
