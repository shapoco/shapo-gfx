#ifndef RP2COMMON_RUNNER_HPP
#define RP2COMMON_RUNNER_HPP

// The frame loop of the ShapoGFX demos on RP2350 boards with an SPI panel
// and a touch controller (the board header names the pins, see
// boards/waveshare_rp2350_touch_lcd_2.h).
//
// A frame is drawn in strips of rows. Each strip is split between the two
// cores (core0 the upper half, core1 the lower one, at the same time), and
// while the next strip is drawn into the other buffer the previous one goes
// out to the panel by DMA. The last strip of a frame is left in flight while
// the next frame is updated. The same as the M5Stack builds of demorig
// (example/m5common/).
//
// Every 2 seconds the frame rate and the time per frame spent in each part
// go to stdio (USB).

#include <cstdint>

namespace rp2common {

// A demo, driven by run()
class App {
 public:
  virtual ~App() = default;

  // Once, on core0, before the loop: the screen is width x height pixels
  virtual void init(int width, int height) = 0;

  // Touch, in screen pixels (one pointer), on core0 before update()
  virtual void pointerDown(int x, int y) { (void)x, (void)y; }
  virtual void pointerMove(int x, int y) { (void)x, (void)y; }
  virtual void pointerUp() {}

  // Advance to time t (seconds) on core0, once per frame before its strips
  virtual void update(float t) = 0;

  // Draw the screen rows [y, y + rows) into pixels (RGB565_SWAPPED, a row
  // is the screen width). Called on core `core` (0 or 1); both cores draw
  // their own rows of a strip at the same time.
  virtual void drawRows(int core, uint16_t *pixels, int y, int rows) = 0;

  // On core0 after the last strip of a frame has been drawn (its push may
  // still be in flight)
  virtual void endFrame() {}
};

struct Config {
  const char *name;  // for the log
  int stripH;        // screen rows per strip
  bool touch;        // poll the touch controller
};

// Bring the board up (clocks, panel, touch), start core1 and run the frame
// loop. Never returns.
[[noreturn]] void run(App &app, const Config &cfg);

// Draw the frame rate label (e.g. "41.7 fps") at the bottom left of rows
// [y, y + rows) of a strip, for demos that have no display of their own.
// Uses a Graphics2D of its own (no arena); call after the scene.
void drawFpsLabel(uint16_t *pixels, int y, int rows);

// Frames per second of the last log interval (half a second)
float fps();

}  // namespace rp2common

#endif
