#ifndef RP2COMMON_PANEL_ST7789_HPP
#define RP2COMMON_PANEL_ST7789_HPP

// ST7789 (ST7789T3 on the RP2350-Touch-LCD-2) over SPI, written by DMA in
// strips of rows.
//
// beginFrame() opens a window of the whole screen (CASET / RASET / RAMWR);
// the strips of the frame then follow one another as one long write, so a
// strip costs no commands. Two strip buffers of RGB565_SWAPPED rows, which
// is the byte order the panel takes over an 8-bit SPI: one is drawn into
// while the other goes out. CS stays low (nothing else is on the bus).

#include <cstdint>

#include "hardware/spi.h"

namespace rp2common {

struct PanelConfig {
  spi_inst_t *spi;
  uint8_t sckPin, mosiPin, csPin, dcPin, rstPin, blPin;
  int nativeW, nativeH;  // of the panel, portrait (240x320)
  uint32_t baud;         // SPI clock; you get clk_peri / an even divider
  bool landscape;        // turn the panel by 90 degrees
  bool flip;             // and by 180 more
  int backlight;         // 0..255
};

class St7789Panel {
 public:
  // Reset (the reset pin may be shared: call before the other devices on it
  // are set up), configure and turn on the panel, and allocate two strip
  // buffers of stripH rows. False if the buffers did not fit.
  bool init(const PanelConfig &cfg, int stripH);

  // Screen size as drawn (landscape: nativeH x nativeW)
  int width() const { return w_; }
  int height() const { return h_; }
  uint32_t baud() const { return baud_; }

  uint16_t *buffer(int i) const { return buf_[i]; }

  // Clear the panel to black (blocking)
  void clear();

  // Open the window of the whole screen. Call after drain(), before the
  // first strip of a frame.
  void beginFrame();

  // Push `rows` rows from buffer i (the next rows of the frame). Returns at
  // once; the buffer must not be drawn into until drain() has returned.
  void submit(int i, int rows);

  // Wait for the push in flight, if any, and for the bus to go idle.
  // Idempotent.
  void drain();

  void setBacklight(int level);  // 0..255

 private:
  PanelConfig cfg_ = {};
  int w_ = 0, h_ = 0;
  uint32_t baud_ = 0;
  int dma_ = -1;
  bool pending_ = false;
  uint16_t *buf_[2] = {nullptr, nullptr};

  void command(uint8_t cmd, const uint8_t *data = nullptr, int n = 0);
};

}  // namespace rp2common

#endif
