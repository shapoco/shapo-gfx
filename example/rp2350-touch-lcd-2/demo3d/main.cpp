// demo3d on the Waveshare RP2350-Touch-LCD-2: the scene of
// example/common/demo3d/ with the camera turned by dragging and the zoom
// buttons of demo3d::Demo, rendered in strips by both cores (one render
// context each) over the 2D backdrop.

#include <cstdint>

#include "demo3d.hpp"
#include "runner.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"
#include "shapoco/gfx3d/gfx3d.hpp"

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;

namespace {

constexpr size_t ARENA3_BYTES = 128 * 1024;

class Demo3dApp : public rp2common::App {
 public:
  void init(int width, int height) override {
    width_ = width;
    demo_.init(width, height);
    g3::Config cfg = g3::defaultConfig((int16_t)width, (int16_t)height,
                                       arena3_, sizeof(arena3_));
    cfg.renderContexts = 2;  // one per core
    g3d_.init(cfg);
    g3d_.disableClear();  // the 2D backdrop provides the background
    for (int i = 0; i < 2; i++) gfx_[i].init(arena2_[i], sizeof(arena2_[i]));
  }

  void pointerDown(int x, int y) override { demo_.pointerDown(x, y); }
  void pointerMove(int x, int y) override { demo_.pointerMove(x, y); }
  void pointerUp() override { demo_.pointerUp(); }

  void update(float t) override {
    demo_.update(t, g3d_);
    g3d_.beginRender();
  }

  void drawRows(int core, uint16_t *pixels, int y, int rows) override {
    if (rows <= 0) return;
    const g2::Surface s = {g2::PixelFormat::RGB565_SWAPPED, (int16_t)width_,
                           (int16_t)rows, (uint32_t)(width_ * 2), pixels};
    g2::Graphics2D &g = gfx_[core];
    g.setTarget(s);
    demo_.drawBackdrop(g, y);
    g3d_.render(core, 0, (int16_t)y, (int16_t)width_, (int16_t)rows, s, 0, 0);
    demo_.drawOverlay(g, y);
  }

  void endFrame() override { g3d_.endRender(); }

 private:
  int width_ = 0;
  demo3d::Demo demo_;
  g3::Graphics3D g3d_;
  alignas(8) uint8_t arena3_[ARENA3_BYTES];
  // One 2D context per core for the backdrop and the overlay
  g2::Graphics2D gfx_[2];
  alignas(8) uint8_t arena2_[2][2048];
};

Demo3dApp app;

}  // namespace

int main() {
  rp2common::Config cfg = {};
  cfg.name = "demo3d";
  cfg.stripH = 60;
  cfg.touch = true;
  rp2common::run(app, cfg);
}
