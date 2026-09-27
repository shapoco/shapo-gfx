#ifndef M5DEMORIG_PANEL_OUT_HPP
#define M5DEMORIG_PANEL_OUT_HPP

// Where the strips go: two buffers of RGB565_SWAPPED rows, one being drawn
// while the other is pushed. panel_spi.cpp (ESP32-S3: SPI DMA) and
// panel_ppa.cpp (ESP32-P4: PPA into the MIPI-DSI framebuffer) implement it;
// CMakeLists.txt compiles the one for the target.

#include <cstdint>

// M5GFX.h declares `using M5GFX = m5gfx::M5GFX`, so the class is named
// through its namespace here rather than forward declared as `class M5GFX`
namespace m5gfx {
class M5GFX;
}

namespace m5demorig {

class PanelOut {
 public:
  virtual ~PanelOut() = default;

  // Take the panel and allocate the buffers for frames of w x h pixels (the
  // panel's logical size divided by scale) pushed stripH rows at a time.
  // Call once, after M5.begin() and the rotation; false if anything failed.
  virtual bool init(m5gfx::M5GFX &gfx, int w, int h, int scale,
                    int stripH) = 0;

  // Push rows [y, y + rows) of the frame from buffer i. Returns at once; the
  // buffer must not be drawn into until drain() has returned.
  virtual void submit(int i, int y, int rows) = 0;

  // Wait for the push in flight, if any. Idempotent.
  virtual void drain() = 0;

  uint16_t *buffer(int i) const { return buf_[i]; }

 protected:
  uint16_t *buf_[2] = {nullptr, nullptr};
};

// The implementation for this target
PanelOut &panelOut();

}  // namespace m5demorig

#endif
