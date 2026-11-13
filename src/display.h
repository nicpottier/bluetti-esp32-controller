// Display for the Waveshare ESP32-S3-Touch-LCD-3.5: ST7796 320x480 IPS on SPI
// (CS tied low on the board), backlight through a transistor on GPIO6. The
// panel's reset (shared with the touch controller) is on a TCA9554 I/O expander,
// pin EXIO1.
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>

static const int PIN_I2C_SDA = 8, PIN_I2C_SCL = 7;

class Display : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;

  static const uint8_t TCA9554 = 0x20;
  static void expander(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(TCA9554);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
  }

 public:
  Display() {
    {
      auto c = bus_.config();
      c.spi_host = SPI2_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 5;
      c.pin_mosi = 1;
      c.pin_miso = 2;
      c.pin_dc = 3;
      bus_.config(c);
      panel_.setBus(&bus_);
    }
    {
      auto c = panel_.config();
      c.pin_cs = -1;
      c.pin_rst = -1;  // on the expander, see start()
      c.pin_busy = -1;
      c.panel_width = 320;
      c.panel_height = 480;
      c.readable = false;
      c.invert = true;
      c.rgb_order = false;
      c.dlen_16bit = false;
      c.bus_shared = false;
      panel_.config(c);
    }
    {
      auto c = light_.config();
      c.pin_bl = 6;
      c.invert = false;
      c.freq = 22000;
      c.pwm_channel = 7;
      light_.config(c);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }

  // Starts the shared I2C bus, pulses the panel reset through the expander, then
  // initialises the panel.
  bool start() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    expander(1, 0x00);  // outputs low
    expander(3, 0xFD);  // EXIO1 output, the rest inputs
    delay(20);
    expander(1, 0x02);  // release reset
    delay(150);
    return init();
  }
};
