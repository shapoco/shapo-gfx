// demo3d on the Waveshare RP2350-Touch-LCD-2: the scene of
// example/common/demo3d/ with the camera turned by dragging and the zoom
// buttons of demo3d::Demo, rendered in strips by both cores (one render
// context each) over the 2D backdrop.
//
// With DEMO3D_PROFILE (CMake option, on for now) it also measures where a
// frame goes, every 2 seconds on stdio (see "Profile" below).

#include <cstdint>
#include <cstdio>

#include "demo3d.hpp"
#include "runner.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"
#include "shapoco/gfx3d/gfx3d.hpp"

#if DEMO3D_PROFILE
#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "pico/stdlib.h"
#endif

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;

namespace {

constexpr size_t ARENA3_BYTES = 128 * 1024;

#if DEMO3D_PROFILE
// ---------------------------------------------------------------------------
// Profile: cycles of the parts of sceneBuild() (demo3dProfileMark), of
// beginRender() (the sort) and of the 2D / 3D work of each core per frame,
// and the XIP cache hit rate while building and while drawing.
namespace prof {

volatile uint32_t &reg(uintptr_t addr) {
  return *reinterpret_cast<volatile uint32_t *>(addr);
}
// The cycle counter of the calling core's DWT
constexpr uintptr_t DEMCR = 0xE000EDFC, DWT_CTRL = 0xE0001000,
                    DWT_CYCCNT = 0xE0001004;
void enableCycles() {
  reg(DEMCR) |= 1u << 24;  // TRCENA
  reg(DWT_CYCCNT) = 0;
  reg(DWT_CTRL) |= 1u;  // CYCCNTENA
}
inline uint32_t cycles() { return reg(DWT_CYCCNT); }

// The XIP cache's counters of accesses and hits (both cores, all masters)
struct Xip {
  uint32_t hit, acc;
  static Xip now() {
    return {reg(XIP_CTRL_BASE + 0x0c), reg(XIP_CTRL_BASE + 0x10)};
  }
  Xip operator-(const Xip &o) const { return {hit - o.hit, acc - o.acc}; }
  Xip &operator+=(const Xip &o) {
    hit += o.hit;
    acc += o.acc;
    return *this;
  }
};

constexpr int MAX_PARTS = 12;
struct Parts {
  const char *name[MAX_PARTS] = {};
  uint64_t sum[MAX_PARTS] = {};
  int count = 0;  // builds summed
};

Parts g_build;
Parts *g_cur = nullptr;
int g_index = 0;
uint32_t g_last = 0;

void beginBuild(Parts &p) {
  g_cur = &p;
  g_index = 0;
  g_last = cycles();
}
void endBuild() {
  g_cur->count++;
  g_cur = nullptr;
}

}  // namespace prof
#endif

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
#if DEMO3D_PROFILE
    prof::enableCycles();
    cyclesPerUs_ = (float)clock_get_hz(clk_sys) * 1e-6f;
    reportSince_ = time_us_64();
#endif
  }

  void pointerDown(int x, int y) override { demo_.pointerDown(x, y); }
  void pointerMove(int x, int y) override { demo_.pointerMove(x, y); }
  void pointerUp() override { demo_.pointerUp(); }

  void update(float t) override {
#if DEMO3D_PROFILE
    const prof::Xip x0 = prof::Xip::now();
    const uint32_t c0 = prof::cycles();
    prof::beginBuild(prof::g_build);
    demo_.update(t, g3d_);
    prof::endBuild();
    const uint32_t c1 = prof::cycles();
    const prof::Xip x1 = prof::Xip::now();
    buildCycles_ += c1 - c0;
    xipBuild_ += x1 - x0;
    const uint32_t c2 = prof::cycles();
    g3d_.beginRender();
    sortCycles_ += prof::cycles() - c2;
    xipDrawStart_ = prof::Xip::now();
#else
    demo_.update(t, g3d_);
    g3d_.beginRender();
#endif
  }

  void drawRows(int core, uint16_t *pixels, int y, int rows) override {
    if (rows <= 0) return;
#if DEMO3D_PROFILE
    if (!cyclesOn_[core]) {
      prof::enableCycles();  // core1's DWT on its first strip
      cyclesOn_[core] = true;
    }
    const uint32_t c0 = prof::cycles();
#endif
    const g2::Surface s = {g2::PixelFormat::RGB565_SWAPPED, (int16_t)width_,
                           (int16_t)rows, (uint32_t)(width_ * 2), pixels};
    g2::Graphics2D &g = gfx_[core];
    g.setTarget(s);
    demo_.drawBackdrop(g, y);
#if DEMO3D_PROFILE
    const uint32_t c1 = prof::cycles();
#endif
    g3d_.render(core, 0, (int16_t)y, (int16_t)width_, (int16_t)rows, s, 0, 0);
#if DEMO3D_PROFILE
    const uint32_t c2 = prof::cycles();
#endif
    demo_.drawOverlay(g, y);
#if DEMO3D_PROFILE
    const uint32_t c3 = prof::cycles();
    drawCycles_[core][0] += c1 - c0;
    drawCycles_[core][1] += c2 - c1;
    drawCycles_[core][2] += c3 - c2;
#endif
  }

  void endFrame() override {
#if DEMO3D_PROFILE
    // core1 is idle here (it has handed back the last strip)
    xipDraw_ += prof::Xip::now() - xipDrawStart_;
    frames_++;
    const uint64_t now = time_us_64();
    if (now - reportSince_ >= 2000000) {
      report();
      reportSince_ = now;
    }
#endif
    g3d_.endRender();
  }

 private:
  int width_ = 0;
  demo3d::Demo demo_;
  g3::Graphics3D g3d_;
  alignas(8) uint8_t arena3_[ARENA3_BYTES];
  // One 2D context per core for the backdrop and the overlay
  g2::Graphics2D gfx_[2];
  alignas(8) uint8_t arena2_[2][2048];

#if DEMO3D_PROFILE
  float cyclesPerUs_ = 250.0f;
  uint64_t reportSince_ = 0;
  int frames_ = 0;
  bool cyclesOn_[2] = {true, false};
  uint64_t buildCycles_ = 0, sortCycles_ = 0;
  uint64_t drawCycles_[2][3] = {};  // per core: backdrop, 3D, overlay
  prof::Xip xipBuild_ = {}, xipDraw_ = {}, xipDrawStart_ = {};

  float us(uint64_t cycles, int n) const {
    return n > 0 ? (float)cycles / cyclesPerUs_ / (float)n : 0.0f;
  }
  static float rate(const prof::Xip &x) {
    return x.acc ? 100.0f * (float)x.hit / (float)x.acc : 0.0f;
  }
  static void printParts(const char *label, const prof::Parts &p,
                         float cyclesPerUs) {
    std::printf("[prof] %s (%d builds), us:", label, p.count);
    for (int i = 0; i < prof::MAX_PARTS && p.name[i]; i++) {
      std::printf(" %s %d", p.name[i],
                  (int)(p.count ? (float)p.sum[i] / cyclesPerUs / p.count
                                : 0.0f));
    }
    std::printf("\n");
  }

  void report() {
    const int n = frames_;
    // Integers only: the floating point printf costs time and flash
    std::printf(
        "[prof] %d frames, us/frame: build %d (incl. Demo::update), sort %d; "
        "core0 backdrop %d 3D %d overlay %d; core1 backdrop %d 3D %d overlay "
        "%d\n",
        n, (int)us(buildCycles_, n), (int)us(sortCycles_, n),
        (int)us(drawCycles_[0][0], n), (int)us(drawCycles_[0][1], n),
        (int)us(drawCycles_[0][2], n), (int)us(drawCycles_[1][0], n),
        (int)us(drawCycles_[1][1], n), (int)us(drawCycles_[1][2], n));
    printParts("build", prof::g_build, cyclesPerUs_);
    std::printf(
        "[prof] XIP cache hits: build %d%% of %lu/frame, draw %d%% of "
        "%lu/frame\n",
        (int)rate(xipBuild_), (unsigned long)(n ? xipBuild_.acc / n : 0),
        (int)rate(xipDraw_), (unsigned long)(n ? xipDraw_.acc / n : 0));
    const g3::Stats st = g3d_.getStats();
    std::printf(
        "[prof] scene: %d triangles (%u bytes), arena %u / %u, span peak %d / "
        "%d, dropped: tris %d spans %d\n",
        st.triCount, (unsigned)st.triBytes, (unsigned)st.arenaUsed,
        (unsigned)st.arenaSize, st.spanPeak, st.spanCapacity, st.triDropped,
        st.spanDropped);

    frames_ = 0;
    buildCycles_ = sortCycles_ = 0;
    for (auto &c : drawCycles_) c[0] = c[1] = c[2] = 0;
    xipBuild_ = xipDraw_ = {};
    prof::g_build = prof::Parts();
  }
#endif
};

Demo3dApp app;

}  // namespace

#if DEMO3D_PROFILE
// Called by sceneBuild() after each part (core0, inside update()); declared
// in example/common/demo3d/scene.cpp
namespace demo3d {
void demo3dProfileMark(const char *part) {
  prof::Parts *p = prof::g_cur;
  if (!p || prof::g_index >= prof::MAX_PARTS) return;
  const uint32_t c = prof::cycles();
  p->name[prof::g_index] = part;
  p->sum[prof::g_index] += c - prof::g_last;
  prof::g_index++;
  prof::g_last = prof::cycles();  // the mark itself not counted
}
}  // namespace demo3d
#endif

int main() {
  rp2common::Config cfg = {};
  cfg.name = "demo3d";
  cfg.stripH = 60;
  cfg.touch = true;
  rp2common::run(app, cfg);
}
