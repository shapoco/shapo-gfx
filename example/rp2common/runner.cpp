// The frame loop of the RP2350 demos (see runner.hpp).

#include "runner.hpp"

#include <algorithm>
#include <cstdio>

#include "clocks.hpp"
#include "diag.hpp"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "panel_st7789.hpp"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "shapoco/gfx2d/fonts.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"
#include "touch_cst816.hpp"

// The pins come from the board header (PICO_BOARD)
#ifndef RP2COMMON_LCD_SPI
#error "the board header does not name the panel pins (RP2COMMON_LCD_*)"
#endif

// 0 draws every strip on core0 alone, for comparison
#ifndef RP2COMMON_DUAL_CORE
#define RP2COMMON_DUAL_CORE 1
#endif
// SPI clock of the panel: clk_peri (clk_sys / 2, 125 MHz) / 2
#ifndef RP2COMMON_LCD_BAUD
#define RP2COMMON_LCD_BAUD 62500000
#endif
// 1 turns the screen by 180 degrees (the USB connector on the other side)
#ifndef RP2COMMON_LCD_FLIP
#define RP2COMMON_LCD_FLIP 0
#endif
#ifndef RP2COMMON_BACKLIGHT
#define RP2COMMON_BACKLIGHT 200
#endif

namespace rp2common {

namespace g2 = shapoco::gfx2d;

namespace {

constexpr uint64_t LOG_INTERVAL_US = 2000000;

App *g_app = nullptr;
St7789Panel g_panel;
Cst816Touch g_touch;
bool g_touchOk = false;
int g_w = 0, g_h = 0;

// Frame rate for drawFpsLabel(), every half second
float g_fps = 0.0f;
char g_fpsLabel[24] = "";

#if RP2COMMON_DUAL_CORE
// The rows core1 draws: written by core0 before the FIFO push, read by core1
// after the pop, and not touched by core0 again until core1 pushes back
struct Job {
  uint16_t *pixels;
  int y, rows;
};
Job g_job;

// core1's stack, larger than the SDK's 2 KB (PICO_CORE1_STACK_SIZE is 0, so
// SCRATCH_X is left to core0's stack, which grows down into it from
// SCRATCH_Y: 8 KB)
constexpr size_t CORE1_STACK_BYTES = 8192;
uint32_t g_core1Stack[CORE1_STACK_BYTES / 4];

void core1Main() {
  for (;;) {
    diagMark(1, Phase::IDLE, 0);
    (void)multicore_fifo_pop_blocking();
    __dmb();  // see g_job as core0 wrote it
    diagMark(1, Phase::DRAW, g_job.y);
    const uint64_t t0 = time_us_64();
    if (g_job.rows > 0) g_app->drawRows(1, g_job.pixels, g_job.y, g_job.rows);
    __dmb();  // the pixels written before core0 hands them to the DMA
    multicore_fifo_push_blocking((uint32_t)(time_us_64() - t0));
  }
}
#endif

// One pointer, in screen pixels. The touch reports the panel's portrait
// coordinates; the landscape screen has x along the touch's y (Waveshare's
// demo maps them so too).
void pollTouch() {
  static bool wasDown = false;
  int tx = 0, ty = 0;
  const bool down = g_touch.read(tx, ty);
  int x = std::clamp(ty, 0, g_w - 1);
  int y = std::clamp(g_h - 1 - tx, 0, g_h - 1);
  if (RP2COMMON_LCD_FLIP) {
    x = g_w - 1 - x;
    y = g_h - 1 - y;
  }
  if (down && !wasDown) {
    g_app->pointerDown(x, y);
  } else if (down) {
    g_app->pointerMove(x, y);
  } else if (wasDown) {
    g_app->pointerUp();
  }
  wasDown = down;
}

// Microseconds per frame, averaged over the log interval
struct Stats {
  int frames = 0;
  uint64_t touchUs = 0, updateUs = 0, draw0Us = 0, draw1Us = 0, joinUs = 0,
           drainUs = 0, endUs = 0;
  uint64_t since = 0;
};

void logStats(Stats &s, uint64_t now) {
  if (s.frames == 0) return;
  const int n = s.frames;
  const int fps10 = (int)(n * 1e7 / (double)(now - s.since) + 0.5);
  std::printf(
      "%d.%d fps, us/frame: touch %llu, update %llu, draw core0 %llu core1 "
      "%llu, wait core1 %llu, wait panel %llu, end %llu\n",
      fps10 / 10, fps10 % 10, s.touchUs / n, s.updateUs / n, s.draw0Us / n,
      s.draw1Us / n, s.joinUs / n, s.drainUs / n, s.endUs / n);
  s = Stats();
  s.since = now;
}

void halt(const char *msg) {
  for (;;) {
    std::printf("%s\n", msg);
    sleep_ms(1000);
  }
}

}  // namespace

float fps() { return g_fps; }

void drawFpsLabel(uint16_t *pixels, int y, int rows) {
  g2::Graphics2D g({g2::PixelFormat::RGB565_SWAPPED, (int16_t)g_w,
                    (int16_t)rows, (uint32_t)(g_w * 2), pixels});
  g.setTransform(g2::affine2f::translation(0.0f, (float)-y));
  g.setFont(&g2::ShapoSansMono_s08c07);
  const g2::TextMetrics m = g.textMetrics(g_fpsLabel);
  const int x0 = 2, y0 = g_h - 2 - m.height - 2;
  g.fillRect(x0, y0, m.width + 4, m.height + 2, g2::makeColor(0, 0, 0, 160));
  g.setTextColor(g2::makeColor(255, 255, 255));
  g.drawString(x0 + 2, y0 + 1, g_fpsLabel);
}

void run(App &app, const Config &cfg) {
  const bool clockOk = initClocks(defaultClockConfig());
  stdio_init_all();
  g_app = &app;

  // The panel first: its reset also resets the touch controller
  PanelConfig pc = {};
  pc.spi = SPI_INSTANCE(RP2COMMON_LCD_SPI);
  pc.sckPin = RP2COMMON_LCD_SCK_PIN;
  pc.mosiPin = RP2COMMON_LCD_MOSI_PIN;
  pc.csPin = RP2COMMON_LCD_CS_PIN;
  pc.dcPin = RP2COMMON_LCD_DC_PIN;
  pc.rstPin = RP2COMMON_LCD_RST_PIN;
  pc.blPin = RP2COMMON_LCD_BL_PIN;
  pc.nativeW = RP2COMMON_LCD_WIDTH;
  pc.nativeH = RP2COMMON_LCD_HEIGHT;
  pc.baud = RP2COMMON_LCD_BAUD;
  pc.landscape = true;
  pc.flip = RP2COMMON_LCD_FLIP != 0;
  pc.backlight = RP2COMMON_BACKLIGHT;
  g_w = RP2COMMON_LCD_HEIGHT;
  g_h = RP2COMMON_LCD_WIDTH;
  const int stripH = std::min(cfg.stripH, g_h);
  const bool panelOk = g_panel.init(pc, stripH);
  if (panelOk) {
    g_panel.clear();
    g_panel.setBacklight(pc.backlight);
  }

  if (cfg.touch) {
    TouchConfig tc = {};
    tc.i2c = I2C_INSTANCE(RP2COMMON_TOUCH_I2C);
    tc.sdaPin = RP2COMMON_TOUCH_SDA_PIN;
    tc.sclPin = RP2COMMON_TOUCH_SCL_PIN;
    tc.addr = RP2COMMON_TOUCH_ADDR;
    tc.baud = 400000;
    g_touchOk = g_touch.init(tc);
  }

  // Give a USB terminal a moment to attach, so that the header is seen
  sleep_ms(1500);
  const ClockReport &cr = clockReport();
  std::printf("%s on RP2350: frame %dx%d, strips of %d rows, %s\n", cfg.name,
              g_w, g_h, stripH,
              RP2COMMON_DUAL_CORE ? "two cores" : "one core");
  std::printf("clk_sys %lu Hz%s, clk_peri %lu Hz, SPI %lu Hz, QMI timing "
              "0x%08lx -> 0x%08lx\n",
              (unsigned long)cr.sysHz, clockOk ? "" : " (not as asked)",
              (unsigned long)cr.periHz, (unsigned long)g_panel.baud(),
              (unsigned long)cr.qmiTimingBefore,
              (unsigned long)cr.qmiTimingAfter);
  if (cfg.touch) {
    std::printf("touch: %s, chip id 0x%02x\n", g_touchOk ? "ok" : "no answer",
                g_touch.chipId());
  }
  diagReport();
  if (!panelOk) halt("strip buffers did not fit");

  app.init(g_w, g_h);
#if RP2COMMON_DUAL_CORE
  multicore_launch_core1_with_stack(core1Main, g_core1Stack,
                                    sizeof(g_core1Stack));
#endif

  const uint64_t start = time_us_64();
  Stats stats;
  stats.since = start;
  uint64_t fpsSince = start;
  int fpsFrames = 0;
  // Free running across frames: the last strip of a frame is still going out
  // when the next frame starts, so its first strip takes the other buffer
  int cur = 0;
  diagStartWatchdog();
  for (;;) {
    diagFeedWatchdog();
    diagMark(0, Phase::UPDATE, 0);
    const uint64_t t0 = time_us_64();
    if (cfg.touch && g_touchOk) pollTouch();
    const uint64_t t1 = time_us_64();
    app.update((float)(t1 - start) * 1e-6f);
    const uint64_t t2 = time_us_64();
    stats.touchUs += t1 - t0;
    stats.updateUs += t2 - t1;

    for (int y = 0; y < g_h; y += stripH) {
      const int rows = std::min(stripH, g_h - y);
      const int idx = cur;
      cur ^= 1;
      uint16_t *buf = g_panel.buffer(idx);

      // The buffer was last pushed two strips ago, and the drain before the
      // previous strip's submit waited for that
      const uint64_t d0 = time_us_64();
#if RP2COMMON_DUAL_CORE
      const int upper = rows / 2;
      g_job = {buf + upper * g_w, y + upper, rows - upper};
      __dmb();
      multicore_fifo_push_blocking(0);
      diagMark(0, Phase::DRAW, y);
      app.drawRows(0, buf, y, upper);
      const uint64_t d1 = time_us_64();
      diagMark(0, Phase::WAIT_CORE1, y);
      stats.draw1Us += multicore_fifo_pop_blocking();
      __dmb();
#else
      diagMark(0, Phase::DRAW, y);
      app.drawRows(0, buf, y, rows);
      const uint64_t d1 = time_us_64();
#endif
      const uint64_t d2 = time_us_64();
      diagMark(0, Phase::WAIT_PANEL, y);
      // Only now wait for the previous strip: it went out while this one
      // was drawn
      g_panel.drain();
      if (y == 0) g_panel.beginFrame();
      const uint64_t d3 = time_us_64();
      __dmb();  // the pixels in memory before the DMA reads them
      g_panel.submit(idx, rows);

      stats.draw0Us += d1 - d0;
      stats.joinUs += d2 - d1;
      stats.drainUs += d3 - d2;
    }
    const uint64_t e0 = time_us_64();
    diagMark(0, Phase::END_FRAME, 0);
    app.endFrame();
    const uint64_t now = time_us_64();
    stats.endUs += now - e0;
    stats.frames++;

    fpsFrames++;
    if (now - fpsSince >= 500000) {
      g_fps = fpsFrames * 1e6f / (float)(now - fpsSince);
      fpsFrames = 0;
      fpsSince = now;
      const int fps10 = (int)(g_fps * 10 + 0.5f);
      std::snprintf(g_fpsLabel, sizeof(g_fpsLabel), "%d.%d fps", fps10 / 10,
                    fps10 % 10);
    }
    if (now - stats.since >= LOG_INTERVAL_US) logStats(stats, now);
  }
}

}  // namespace rp2common
