// demo2d on the Waveshare RP2350-Touch-LCD-2: the scene of
// example/common/demo2d/ in its compact (320x240) layout, drawn in strips by
// both cores (example/rp2common/), with the frame rate at the bottom left.

#include <cstdint>

#include "runner.hpp"
#include "scene.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"

namespace g2 = shapoco::gfx2d;

namespace {

class Demo2dApp : public rp2common::App {
 public:
  void init(int width, int height) override {
    width_ = width;
    demo2d::sceneInit(width, height);
    for (int i = 0; i < 2; i++) gfx_[i].init(arena_[i], sizeof(arena_[i]));
  }

  void update(float t) override { demo2d::sceneUpdate(t); }

  void drawRows(int core, uint16_t *pixels, int y, int rows) override {
    if (rows <= 0) return;
    g2::Graphics2D &g = gfx_[core];
    g.setTarget({g2::PixelFormat::RGB565_SWAPPED, (int16_t)width_,
                 (int16_t)rows, (uint32_t)(width_ * 2), pixels});
    demo2d::sceneDraw(g, y);
    rp2common::drawFpsLabel(pixels, y, rows);
  }

 private:
  int width_ = 0;
  // One context per core, each with its own arena (state stack and scratch)
  g2::Graphics2D gfx_[2];
  alignas(8) uint8_t arena_[2][4096];
};

Demo2dApp app;

}  // namespace

int main() {
  rp2common::Config cfg = {};
  cfg.name = "demo2d";
  cfg.stripH = 60;
  cfg.touch = false;
  rp2common::run(app, cfg);
}
