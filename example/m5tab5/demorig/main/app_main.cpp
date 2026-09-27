// demorig on M5Stack Tab5: a 640x360 frame (the 1280x720 panel in landscape,
// halved) drawn in six strips of 60 rows, each scaled up twice by the PPA
// on its way into the panel's framebuffer. Zoom buttons and scrolling by
// touch; the touch coordinates are halved into the frame.

#include "m5demorig.hpp"

extern "C" void app_main(void) {
  m5demorig::Config cfg = {};
  cfg.name = "M5Stack Tab5";
  // 3 comes out the right way up on hardware (Devour Sphere, 2026-09-20)
  cfg.rotation = 3;
  cfg.scale = 2;
  cfg.stripH = 60;
  cfg.controls = true;
  m5demorig::run(cfg);
}
