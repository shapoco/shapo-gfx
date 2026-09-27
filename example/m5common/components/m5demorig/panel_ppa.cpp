// Strips -> PPA -> the MIPI-DSI framebuffer (M5Stack Tab5), after Devour
// Sphere's Tab5 front end (~/repo/2026/devour-sphere/impl/m5tab5/).
//
// The frame is a landscape picture of (panel height / scale) x (panel width /
// scale) pixels, RGB565 big-endian; the panel is a 720x1280 portrait raster,
// RGB565 little-endian, scanned out continuously from PSRAM. The PPA scales
// each strip, turns it a quarter and swaps the bytes on its way into the
// framebuffer:
//
//   M5GFX's rotation 1 maps landscape (u, v) to panel (X, Y) = (719 - v, u),
//   a quarter turn clockwise: PPA_SRM_ROTATION_ANGLE_270 (the PPA's angles
//   run counter-clockwise), the strip landing at X = 720 - v0 - rows. Rotation
//   3 is the other way round: ANGLE_90 at X = v0.
//
// Using the rotation the display is set to is what makes M5.Touch's
// coordinates land on the buttons drawn.
//
// Like Devour Sphere, this reaches into M5GFX's Panel_DSI (not re-exported
// by M5GFX.h) for the framebuffer, so M5GFX is pinned to the version checked
// there (0.2.25, see example/m5tab5/demorig/main/idf_component.yml).

#include <M5GFX.h>
#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>

#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"
#include "panel_out.hpp"

namespace m5demorig {

namespace {

// The PPA reads the strip as a bus master, which does not see the CPU's data
// cache, so what was just drawn has to be written back first
void flushForDma(const void *p, size_t bytes) {
  static bool complained = false;
  const esp_err_t err = esp_cache_msync(
      (void *)p, bytes,
      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  if (err != ESP_OK && !complained) {
    complained = true;
    std::printf("cache msync refused the strip buffer, err %d\n", (int)err);
  }
}

bool IRAM_ATTR onPpaDone(ppa_client_handle_t, ppa_event_data_t *, void *user) {
  auto sem = (SemaphoreHandle_t)user;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(sem, &woken);
  return woken == pdTRUE;
}

class PpaPanelOut : public PanelOut {
 public:
  bool init(m5gfx::M5GFX &gfx, int w, int h, int scale, int stripH) override {
    auto *panel = static_cast<lgfx::Panel_DSI *>(gfx.getPanel());
    if (!panel || !panel->config_detail().buffer) {
      std::printf("no DSI panel framebuffer\n");
      return false;
    }
    panelW_ = panel->config().panel_width;
    panelH_ = panel->config().panel_height;
    rotation_ = gfx.getRotation() & 3;
    if ((rotation_ != 1 && rotation_ != 3) || w * scale != panelH_ ||
        h * scale != panelW_) {
      std::printf("unexpected geometry: panel %dx%d, rotation %d, frame %dx%d\n",
                  panelW_, panelH_, rotation_, w, h);
      return false;
    }
    // Panel_DSI::init() pads the rows to 4 bytes, which 16-bit pixels of an
    // even width never need: the framebuffer is a plain raster
    panelFb_ = (uint16_t *)panel->config_detail().buffer;
    panelBytes_ = (uint32_t)panelW_ * panelH_ * 2;
    w_ = w;
    scale_ = scale;
    stripH_ = stripH;

    ppa_client_config_t cfg = {};
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 2;
    ppa_client_handle_t ppa = nullptr;
    if (ppa_register_client(&cfg, &ppa) != ESP_OK) {
      std::printf("ppa_register_client failed\n");
      return false;
    }
    ppa_ = ppa;
    done_ = xSemaphoreCreateBinary();
    if (!done_) return false;
    ppa_event_callbacks_t cbs = {};
    cbs.on_trans_done = onPpaDone;
    if (ppa_client_register_event_callbacks(ppa, &cbs) != ESP_OK) {
      std::printf("ppa callbacks failed\n");
      return false;
    }

    // Internal SRAM, so that the PPA's reads do not share the PSRAM bus with
    // its writes and the DSI's scan-out; aligned to the L2 cache line for
    // the msync
    const size_t bytes = (size_t)w * stripH * 2;
    for (int i = 0; i < 2; i++) {
      buf_[i] = (uint16_t *)heap_caps_aligned_alloc(
          CONFIG_CACHE_L2_CACHE_LINE_SIZE, bytes,
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (!buf_[i]) {
        std::printf("strip buffer (%u bytes) did not fit, largest block %u\n",
                    (unsigned)bytes,
                    (unsigned)heap_caps_get_largest_free_block(
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return false;
      }
      memset(buf_[i], 0, bytes);
    }
    // M5GFX drew into the framebuffer through the cache on its way up; those
    // lines must reach PSRAM now, not later over what the PPA puts there
    esp_cache_msync(panelFb_, panelBytes_,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    return true;
  }

  void submit(int i, int y, int rows) override {
    const uint32_t v0 = (uint32_t)y * scale_;
    const uint32_t strip = (uint32_t)rows * scale_;

    ppa_srm_oper_config_t op = {};
    op.in.buffer = buf_[i];
    op.in.pic_w = w_;
    op.in.pic_h = rows;
    op.in.block_w = w_;
    op.in.block_h = rows;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    op.out.buffer = panelFb_;
    op.out.buffer_size = panelBytes_;
    op.out.pic_w = panelW_;
    op.out.pic_h = panelH_;
    op.out.block_offset_x = rotation_ == 1 ? (panelW_ - v0 - strip) : v0;
    op.out.block_offset_y = 0;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    op.rotation_angle = rotation_ == 1 ? PPA_SRM_ROTATION_ANGLE_270
                                       : PPA_SRM_ROTATION_ANGLE_90;
    op.scale_x = (float)scale_;
    op.scale_y = (float)scale_;
    // RGB565 big-endian in, little-endian in the DSI framebuffer
    op.byte_swap = true;
    op.mode = PPA_TRANS_MODE_NON_BLOCKING;
    op.user_data = done_;

    flushForDma(buf_[i], (size_t)w_ * rows * 2);
    const esp_err_t err =
        ppa_do_scale_rotate_mirror((ppa_client_handle_t)ppa_, &op);
    if (err != ESP_OK) {
      // Dropped, not retried; traced once (the geometry never changes)
      static bool complained = false;
      if (!complained) {
        complained = true;
        std::printf("ppa refused the strip, err %d, x %u\n", (int)err,
                    (unsigned)op.out.block_offset_x);
      }
      return;
    }
    pending_ = true;
  }

  void drain() override {
    if (!pending_) return;
    xSemaphoreTake((SemaphoreHandle_t)done_, portMAX_DELAY);
    pending_ = false;
  }

 private:
  void *ppa_ = nullptr;   // ppa_client_handle_t
  void *done_ = nullptr;  // SemaphoreHandle_t, given by the PPA callback
  uint16_t *panelFb_ = nullptr;
  uint32_t panelBytes_ = 0;
  int panelW_ = 0, panelH_ = 0, rotation_ = 0;
  int w_ = 0, scale_ = 1, stripH_ = 0;
  bool pending_ = false;
};

}  // namespace

PanelOut &panelOut() {
  static PpaPanelOut out;
  return out;
}

}  // namespace m5demorig
