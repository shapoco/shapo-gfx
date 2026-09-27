// The frame loop of demorig on M5Stack devices (see m5demorig.hpp).

#include "m5demorig.hpp"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>

#include "demorig.hpp"
#include "panel_out.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"

// 0 draws every strip on core0 alone, for comparison
#ifndef M5DEMORIG_DUAL_CORE
#define M5DEMORIG_DUAL_CORE 1
#endif

namespace m5demorig {

namespace g2 = shapoco::gfx2d;

namespace {

constexpr int ARENA_BYTES = 4096;
constexpr uint32_t WORKER_STACK_BYTES = 12 * 1024;
constexpr int64_t LOG_INTERVAL_US = 2000000;

demorig::Demo g_demo;
int g_w = 0, g_h = 0;

// One context per core, each with its own arena
g2::Graphics2D g_gfx[2];
alignas(8) uint8_t g_arena[2][ARENA_BYTES];

#if M5DEMORIG_DUAL_CORE
// The rows core1 draws: written by core0 before the notification, read by
// core1 after it, and not touched by core0 again until core1 notifies back
struct Job {
  uint16_t *pixels;
  int y, rows;
};
Job g_job;
uint32_t g_workerUs = 0;  // core1 -> core0, likewise
TaskHandle_t g_mainTask = nullptr;
TaskHandle_t g_workerTask = nullptr;
#endif

void drawRows(g2::Graphics2D &g, uint16_t *pixels, int y, int rows) {
  if (rows <= 0) return;
  g.setTarget({g2::PixelFormat::RGB565_SWAPPED, (int16_t)g_w, (int16_t)rows,
               (uint32_t)(g_w * 2), pixels});
  g_demo.draw(g, y);
}

#if M5DEMORIG_DUAL_CORE
void workerTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const int64_t t0 = esp_timer_get_time();
    drawRows(g_gfx[1], g_job.pixels, g_job.y, g_job.rows);
    g_workerUs = (uint32_t)(esp_timer_get_time() - t0);
    xTaskNotifyGive(g_mainTask);
  }
}
#endif

// One pointer, from the first touch point, in frame pixels
void pollTouch(int scale) {
  static bool wasDown = false;
  bool down = false;
  int x = 0, y = 0;
  if (M5.Touch.getCount() > 0) {
    const auto &d = M5.Touch.getDetail(0);
    if (d.isPressed()) {
      down = true;
      x = d.x / scale;
      y = d.y / scale;
    }
  }
  if (down && !wasDown) {
    g_demo.pointerDown(x, y);
  } else if (down) {
    g_demo.pointerMove(x, y);
  } else if (wasDown) {
    g_demo.pointerUp();
  }
  wasDown = down;
}

// Microseconds per frame, averaged over the log interval
struct Stats {
  int frames = 0;
  int64_t updateUs = 0, draw0Us = 0, draw1Us = 0, joinUs = 0, drainUs = 0;
  int64_t since = 0;
};

void logStats(Stats &s, int64_t now) {
  if (s.frames == 0) return;
  const int n = s.frames;
  const float fps = n * 1e6f / (float)(now - s.since);
  const int fps10 = (int)(fps * 10 + 0.5f);
  std::printf(
      "%d.%d fps, zoom x%.3f, us/frame: update %lld, draw core0 %lld core1 "
      "%lld, wait core1 %lld, wait panel %lld\n",
      fps10 / 10, fps10 % 10, (double)g_demo.zoom(), s.updateUs / n,
      s.draw0Us / n, s.draw1Us / n, s.joinUs / n, s.drainUs / n);
  s = Stats();
  s.since = now;
}

}  // namespace

void run(const Config &cfg) {
  auto m5cfg = M5.config();
  m5cfg.internal_spk = false;
  m5cfg.internal_mic = false;
  m5cfg.internal_imu = false;
  m5cfg.internal_rtc = false;
  M5.begin(m5cfg);
  M5.Display.setRotation(cfg.rotation);
  M5.Display.setBrightness(200);
  M5.Display.fillScreen(TFT_BLACK);

  g_w = M5.Display.width() / cfg.scale;
  g_h = M5.Display.height() / cfg.scale;
  const int stripH = std::min(cfg.stripH, g_h);
  std::printf("demorig on %s: frame %dx%d, strips of %d rows, %s\n", cfg.name,
              g_w, g_h, stripH, M5DEMORIG_DUAL_CORE ? "two cores" : "one core");

  PanelOut &out = panelOut();
  if (!out.init(M5.Display, g_w, g_h, cfg.scale, stripH)) {
    std::printf("panel output failed\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  for (int i = 0; i < 2; i++) g_gfx[i].init(g_arena[i], ARENA_BYTES);
  g_demo.init(g_w, g_h, cfg.controls);

#if M5DEMORIG_DUAL_CORE
  g_mainTask = xTaskGetCurrentTaskHandle();
  // Pinned to the core app_main does not run on (it runs on core0)
  if (xTaskCreatePinnedToCore(workerTask, "demorig_draw", WORKER_STACK_BYTES,
                              nullptr, 5, &g_workerTask, 1) != pdPASS) {
    std::printf("worker task not created\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
#endif
  std::printf("internal RAM free %u, largest block %u\n",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  const int64_t start = esp_timer_get_time();
  Stats stats;
  stats.since = start;
  // Free running across frames: the last strip of a frame is still going out
  // when the next frame starts, so its first strip takes the other buffer
  int cur = 0;
  for (;;) {
    const int64_t t0 = esp_timer_get_time();
    M5.update();
    if (cfg.controls) pollTouch(cfg.scale);
    g_demo.update((float)(t0 - start) * 1e-6f);
    const int64_t t1 = esp_timer_get_time();
    stats.updateUs += t1 - t0;

    for (int y = 0; y < g_h; y += stripH) {
      const int rows = std::min(stripH, g_h - y);
      const int idx = cur;
      cur ^= 1;
      uint16_t *buf = out.buffer(idx);

      // The buffer was last pushed two strips ago, and the drain before the
      // previous strip's submit waited for that
      const int64_t d0 = esp_timer_get_time();
#if M5DEMORIG_DUAL_CORE
      const int upper = rows / 2;
      g_job = {buf + upper * g_w, y + upper, rows - upper};
      xTaskNotifyGive(g_workerTask);
      drawRows(g_gfx[0], buf, y, upper);
      const int64_t d1 = esp_timer_get_time();
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      stats.draw1Us += g_workerUs;
#else
      drawRows(g_gfx[0], buf, y, rows);
      const int64_t d1 = esp_timer_get_time();
#endif
      const int64_t d2 = esp_timer_get_time();
      // Only now wait for the previous strip: it went out while this one
      // was drawn
      out.drain();
      const int64_t d3 = esp_timer_get_time();
      out.submit(idx, y, rows);

      stats.draw0Us += d1 - d0;
      stats.joinUs += d2 - d1;
      stats.drainUs += d3 - d2;
    }
    stats.frames++;

    const int64_t now = esp_timer_get_time();
    if (now - stats.since >= LOG_INTERVAL_US) logStats(stats, now);
  }
}

}  // namespace m5demorig
