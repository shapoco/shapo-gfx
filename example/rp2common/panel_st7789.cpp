#include "panel_st7789.hpp"

#include <cstdlib>
#include <cstring>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

namespace rp2common {

namespace {

// Initialization of the ST7789T3 of the RP2350-Touch-LCD-2, after
// Waveshare's demo code (LCD_2in.c): pixel format, the vendor's power and
// gamma settings between the command set unlock (F0 C3, F0 96) and lock
// (F0 3C, F0 69), and inversion on. Entries: command, data length, data;
// 0xFF ends the list.
const uint8_t INIT_SEQUENCE[] = {
    0x3A, 1, 0x05,  // COLMOD: 16 bits per pixel
    0xF0, 1, 0xC3,  // command set control
    0xF0, 1, 0x96,
    0xB4, 1, 0x01,
    0xB7, 1, 0xC6,
    0xC0, 2, 0x80, 0x45,
    0xC1, 1, 0x13,
    0xC2, 1, 0xA7,
    0xC5, 1, 0x0A,
    0xE8, 8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33,
    0xE0, 14, 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17,
    0x13, 0x13, 0x2B, 0x31,  // positive gamma
    0xE1, 14, 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38,
    0x15, 0x16, 0x2C, 0x32,  // negative gamma
    0xF0, 1, 0x3C,
    0xF0, 1, 0x69,
    0xFF,
};

// MADCTL bits
constexpr uint8_t MADCTL_MY = 0x80, MADCTL_MX = 0x40, MADCTL_MV = 0x20,
                  MADCTL_BGR = 0x08;

}  // namespace

bool St7789Panel::init(const PanelConfig &cfg, int stripH) {
  cfg_ = cfg;
  w_ = cfg.landscape ? cfg.nativeH : cfg.nativeW;
  h_ = cfg.landscape ? cfg.nativeW : cfg.nativeH;

  // Control pins; CS low for good
  const uint8_t outs[] = {cfg.csPin, cfg.dcPin, cfg.rstPin};
  for (uint8_t pin : outs) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 1);
  }

  // Backlight off until the panel shows something
  gpio_set_function(cfg.blPin, GPIO_FUNC_PWM);
  const uint slice = pwm_gpio_to_slice_num(cfg.blPin);
  pwm_set_wrap(slice, 255);
  pwm_set_clkdiv(slice, (float)clock_get_hz(clk_sys) / (256 * 20000.0f));  // 20 kHz
  pwm_set_gpio_level(cfg.blPin, 0);
  pwm_set_enabled(slice, true);

  // SPI mode 0, 8 bits; fast edges for 62.5 MHz
  baud_ = spi_init(cfg.spi, cfg.baud);
  spi_set_format(cfg.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  const uint8_t spiPins[] = {cfg.sckPin, cfg.mosiPin};
  for (uint8_t pin : spiPins) {
    gpio_set_function(pin, GPIO_FUNC_SPI);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
  }
  gpio_set_slew_rate(cfg.dcPin, GPIO_SLEW_RATE_FAST);

  // Hardware reset (the touch controller too, when it shares the pin)
  gpio_put(cfg.rstPin, 1);
  sleep_ms(10);
  gpio_put(cfg.rstPin, 0);
  sleep_ms(20);
  gpio_put(cfg.rstPin, 1);
  sleep_ms(120);
  gpio_put(cfg.csPin, 0);

  command(0x11);  // SLPOUT
  sleep_ms(120);
  uint8_t madctl = MADCTL_BGR;
  if (cfg.landscape) madctl |= MADCTL_MV;
  if (cfg.flip) madctl |= MADCTL_MY | MADCTL_MX;
  command(0x36, &madctl, 1);
  for (const uint8_t *p = INIT_SEQUENCE; *p != 0xFF; p += 2 + p[1]) {
    command(p[0], p + 2, p[1]);
  }
  sleep_ms(120);
  command(0x21);  // INVON
  command(0x29);  // DISPON

  // Strip buffers
  const size_t bytes = (size_t)w_ * stripH * 2;
  for (int i = 0; i < 2; i++) {
    buf_[i] = (uint16_t *)std::malloc(bytes);  // 8-byte aligned
    if (!buf_[i]) return false;
    std::memset(buf_[i], 0, bytes);
  }

  // DMA: bytes from memory to the SPI data register, paced by its TX DREQ
  dma_ = dma_claim_unused_channel(true);
  return true;
}

void St7789Panel::command(uint8_t cmd, const uint8_t *data, int n) {
  gpio_put(cfg_.dcPin, 0);
  spi_write_blocking(cfg_.spi, &cmd, 1);
  gpio_put(cfg_.dcPin, 1);
  if (n > 0) spi_write_blocking(cfg_.spi, data, (size_t)n);
}

void St7789Panel::beginFrame() {
  const uint8_t caset[] = {0, 0, (uint8_t)((w_ - 1) >> 8),
                           (uint8_t)((w_ - 1) & 0xFF)};
  const uint8_t raset[] = {0, 0, (uint8_t)((h_ - 1) >> 8),
                           (uint8_t)((h_ - 1) & 0xFF)};
  command(0x2A, caset, 4);
  command(0x2B, raset, 4);
  command(0x2C);  // RAMWR; DC stays high for the pixels
}

void St7789Panel::submit(int i, int rows) {
  dma_channel_config c = dma_channel_get_default_config((uint)dma_);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
  channel_config_set_read_increment(&c, true);
  channel_config_set_write_increment(&c, false);
  channel_config_set_dreq(&c, spi_get_dreq(cfg_.spi, true));
  dma_channel_configure((uint)dma_, &c, &spi_get_hw(cfg_.spi)->dr, buf_[i],
                        (uint32_t)w_ * rows * 2, true);
  pending_ = true;
}

void St7789Panel::drain() {
  if (!pending_) return;
  dma_channel_wait_for_finish_blocking((uint)dma_);
  // The last bytes leave the FIFO after the DMA is done
  while (spi_is_busy(cfg_.spi)) tight_loop_contents();
  // Nothing reads the RX side: empty it and clear the overrun
  while (spi_is_readable(cfg_.spi)) (void)spi_get_hw(cfg_.spi)->dr;
  spi_get_hw(cfg_.spi)->icr = SPI_SSPICR_RORIC_BITS;
  pending_ = false;
}

void St7789Panel::clear() {
  drain();
  beginFrame();
  const uint8_t zero[64] = {};
  for (int n = w_ * h_ * 2; n > 0; n -= (int)sizeof(zero)) {
    spi_write_blocking(cfg_.spi, zero,
                       (size_t)(n < (int)sizeof(zero) ? n : (int)sizeof(zero)));
  }
}

void St7789Panel::setBacklight(int level) {
  // 256 keeps the output high for the whole period
  const int v = level <= 0 ? 0 : level >= 255 ? 256 : level;
  pwm_set_gpio_level(cfg_.blPin, (uint16_t)v);
}

}  // namespace rp2common
