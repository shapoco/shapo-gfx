// demorig on the Waveshare RP2350-Touch-LCD-2: demorig::Demo of
// example/common/demorig/ (the scene, the zoom and antialiasing buttons,
// scrolling by touch, the frame rate), drawn in strips by both cores
// (example/rp2common/).

#include <cstdint>

#include "demorig.hpp"
#include "runner.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"

namespace g2 = shapoco::gfx2d;

namespace {

// The antialiased drawing keeps edges and coverage here, as on the M5Stack
constexpr size_t ARENA_BYTES = 16384;

class DemorigApp : public rp2common::App {
 public:
  void init(int width, int height) override {
    width_ = width;
    demo_.init(width, height, true);
    for (int i = 0; i < 2; i++) gfx_[i].init(arena_[i], ARENA_BYTES);
  }

  void pointerDown(int x, int y) override { demo_.pointerDown(x, y); }
  void pointerMove(int x, int y) override { demo_.pointerMove(x, y); }
  void pointerUp() override { demo_.pointerUp(); }

  void update(float t) override { demo_.update(t); }

  void drawRows(int core, uint16_t *pixels, int y, int rows) override {
    if (rows <= 0) return;
    g2::Graphics2D &g = gfx_[core];
    g.setTarget({g2::PixelFormat::RGB565_SWAPPED, (int16_t)width_,
                 (int16_t)rows, (uint32_t)(width_ * 2), pixels});
    demo_.draw(g, y);
  }

 private:
  int width_ = 0;
  demorig::Demo demo_;
  // One context per core, each with its own arena
  g2::Graphics2D gfx_[2];
  alignas(8) uint8_t arena_[2][ARENA_BYTES];
};

DemorigApp app;

}  // namespace

int main() {
  rp2common::Config cfg = {};
  cfg.name = "demorig";
  cfg.stripH = 60;
  cfg.touch = true;
  rp2common::run(app, cfg);
}
