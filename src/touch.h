// FT6336 capacitive touch (Waveshare ESP32-S3-Touch-LCD-3.5), turned into tap /
// long-press / swipe events. A small task polls the controller (its INT line goes
// to the I/O expander, not a GPIO), so touches aren't lost while loop() is blocked
// in a BLE transaction. Registers from 0x02: finger count, then the first
// finger's x / y; coordinates already match the portrait screen, and setFlip()
// turns them round for a panel rotated 180 degrees.
#pragma once

#include <Arduino.h>
#include <Wire.h>

struct TouchEvent {
  enum Kind : uint8_t { Tap, Long, Swipe } kind;
  int16_t x, y;   // where the finger landed
  uint32_t down;  // millis() when the finger touched down
  int16_t dx, dy;  // Swipe: how far it had moved when it became one
};

class Touch {
 public:
  static const uint8_t ADDR = 0x38;
  static const uint32_t LONG_MS = 800;
  static const uint32_t LIFT_MS = 60;  // no reports for this long = finger lifted
  static const int MOVE_PX = 40;  // more movement than this is a swipe, not a tap
  static const int16_t W = 320, H = 480;

  // Wire must already be running; the controller is reset along with the display.
  bool begin() {
    Wire.beginTransmission(ADDR);
    bool ok = Wire.endTransmission() == 0;
    queue_ = xQueueCreate(8, sizeof(TouchEvent));
    xTaskCreatePinnedToCore(task, "touch", 3072, this, 2, nullptr, 1);
    return ok;
  }

  // Report coordinates for a screen turned upside down (rotation 2).
  void setFlip(bool f) { flip_ = f; }

  bool take(TouchEvent &e) { return xQueueReceive(queue_, &e, 0) == pdTRUE; }
  void clear() { xQueueReset(queue_); }
  // millis() of the most recent finger-down (0 = never), known as soon as the
  // finger lands, before the touch has become a tap / hold / swipe.
  uint32_t lastDown() const { return lastDown_; }
  // The finger that is down right now and hasn't become a swipe / hold yet:
  // where it landed and when. Lets the UI show a press before the tap is known.
  bool held(int16_t &x, int16_t &y, uint32_t &down) const {
    if (!held_) return false;
    x = heldX_;
    y = heldY_;
    down = lastDown_;
    return true;
  }

 private:
  QueueHandle_t queue_ = nullptr;
  volatile bool flip_ = false;
  volatile uint32_t lastDown_ = 0;
  volatile bool held_ = false;
  volatile int16_t heldX_ = 0, heldY_ = 0;

  // One finger: returns true while touching.
  bool read(int16_t &x, int16_t &y) {
    uint8_t buf[5];
    Wire.beginTransmission(ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)ADDR, (int)sizeof(buf)) != sizeof(buf)) return false;
    for (auto &b : buf) b = Wire.read();
    uint8_t n = buf[0] & 0x0F;
    if (n == 0 || n > 2) return false;
    x = ((buf[1] & 0x0F) << 8) | buf[2];
    y = ((buf[3] & 0x0F) << 8) | buf[4];
    if (flip_) {
      x = W - 1 - x;
      y = H - 1 - y;
    }
    return true;
  }

  void send(TouchEvent::Kind k, int16_t x, int16_t y, uint32_t down, int16_t dx = 0, int16_t dy = 0) {
    TouchEvent e{k, x, y, down, dx, dy};
    xQueueSend(queue_, &e, 0);
  }

  static void task(void *arg) {
    auto *self = (Touch *)arg;
    bool down = false, done = false;  // done: this touch already produced its event
    int16_t x0 = 0, y0 = 0, x = 0, y = 0;
    uint32_t t0 = 0, lastSeen = 0;
    for (;;) {
      uint32_t now = millis();
      bool touching = self->read(x, y);
      if (touching) lastSeen = now;
      if (touching && !down) {
        down = true;
        done = false;
        x0 = x;
        y0 = y;
        t0 = now;
        self->lastDown_ = now ? now : 1;
        self->heldX_ = x;
        self->heldY_ = y;
        self->held_ = true;
      } else if (touching && down && !done) {
        if (abs(x - x0) > MOVE_PX || abs(y - y0) > MOVE_PX) {
          self->held_ = false;
          self->send(TouchEvent::Swipe, x0, y0, t0, x - x0, y - y0);
          done = true;
        } else if (now - t0 >= LONG_MS) {
          self->held_ = false;
          self->send(TouchEvent::Long, x0, y0, t0);
          done = true;
        }
      } else if (!touching && down && now - lastSeen > LIFT_MS) {  // no report for a while: finger lifted
        down = false;
        self->held_ = false;
        if (!done) self->send(TouchEvent::Tap, x0, y0, t0);
      }
      vTaskDelay(pdMS_TO_TICKS(15));
    }
  }
};
