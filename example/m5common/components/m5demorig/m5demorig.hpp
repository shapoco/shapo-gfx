#ifndef M5DEMORIG_HPP
#define M5DEMORIG_HPP

// demorig on M5Stack devices: brings the board up with M5Unified and runs
// the frame loop. The scene is example/common/demorig/; each project under
// example/m5*/demorig/ only chooses the configuration.
//
// A frame is drawn in strips of rows. Each strip is split between the two
// cores (core0 the upper half, core1 the lower one), and while the next
// strip is drawn into the other buffer the previous one goes out to the
// panel (SPI DMA, or the PPA on the Tab5). The last strip of a frame is left
// in flight across the frame boundary.

namespace m5demorig {

struct Config {
  const char *name;  // for the serial log
  int rotation;      // of M5.Display (a landscape one)
  int scale;         // panel pixels per frame pixel: 1, or 2 on the Tab5
  int stripH;        // frame rows per strip
  bool controls;     // zoom buttons and scrolling by touch
};

// Never returns
void run(const Config &cfg);

}  // namespace m5demorig

#endif
