// demorig on M5Stack CoreS3: the 320x240 panel in landscape, drawn in four
// strips of 60 rows (two buffers of 37.5 KB), zoom buttons and scrolling by
// touch.

#include "m5demorig.hpp"

extern "C" void app_main(void) {
  m5demorig::Config cfg = {};
  cfg.name = "M5Stack CoreS3";
  cfg.rotation = 1;
  cfg.scale = 1;
  cfg.stripH = 60;
  cfg.controls = true;
  m5demorig::run(cfg);
}
