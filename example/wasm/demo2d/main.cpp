// demo2d: sample program for the ShapoGFX 2D API.
//
// Built with Emscripten it exports a small C API used by
// docs/example/viewer.js. Built natively it renders a single frame to a PPM
// file.

#include <cstdint>

#include "shapoco/gfx2d/graphics2d.hpp"

#include "scene.hpp"  // example/common/demo2d/

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define DEMO2D_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DEMO2D_EXPORT
#endif

namespace g2 = shapoco::gfx2d;

// 480x320; the native build may choose another size up to that (the compact
// layout of the RP2350 build is 320x240)
static constexpr int MAX_W = demo2d::LARGE_W;
static constexpr int MAX_H = demo2d::LARGE_H;

static uint16_t fb[MAX_W * MAX_H];  // RGB565_SWAPPED
static int screenW = MAX_W, screenH = MAX_H;
static g2::Graphics2D gfx;
// State stack and scratch memory of gfx
static uint8_t arena[4096];

// ---------------------------------------------------------------------------
// Exported API

extern "C" {

DEMO2D_EXPORT uint16_t *demo2d_get_fb() { return fb; }
DEMO2D_EXPORT int demo2d_get_width() { return screenW; }
DEMO2D_EXPORT int demo2d_get_height() { return screenH; }

DEMO2D_EXPORT void demo2d_init() {
  demo2d::sceneInit(screenW, screenH);
  gfx.init(arena, sizeof(arena));
  gfx.setTarget({g2::PixelFormat::RGB565_SWAPPED, (int16_t)screenW,
                 (int16_t)screenH, (uint32_t)(screenW * 2), fb});
}

// t: elapsed seconds
DEMO2D_EXPORT void demo2d_frame(float t) {
  demo2d::sceneUpdate(t);
  demo2d::sceneDraw(gfx, 0);
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Native entry point: render one frame and write it as a binary PPM

#ifndef __EMSCRIPTEN__

#include <cstdio>
#include <cstdlib>

//   demo2d [out.ppm] [seconds] [WxH] [bands]
//
// WxH up to 480x320 (default), bands the number of bands the frame is drawn
// in (default 1, the way the RP2350 build draws it when greater).
int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "demo2d.ppm";
  float t = (argc > 2) ? (float)std::atof(argv[2]) : 1.0f;
  if (argc > 3 && (std::sscanf(argv[3], "%dx%d", &screenW, &screenH) != 2 ||
                   screenW < 64 || screenH < 64 || screenW > MAX_W ||
                   screenH > MAX_H)) {
    std::fprintf(stderr, "bad screen size: %s\n", argv[3]);
    return 1;
  }
  const int bands = (argc > 4) ? std::atoi(argv[4]) : 1;

  demo2d_init();
  if (bands <= 1) {
    demo2d_frame(t);
  } else {
    demo2d::sceneUpdate(t);
    for (int b = 0; b < bands; b++) {
      const int y0 = screenH * b / bands, y1 = screenH * (b + 1) / bands;
      gfx.setTarget({g2::PixelFormat::RGB565_SWAPPED, (int16_t)screenW,
                     (int16_t)(y1 - y0), (uint32_t)(screenW * 2),
                     fb + y0 * screenW});
      demo2d::sceneDraw(gfx, y0);
    }
  }

  FILE *fp = std::fopen(path, "wb");
  if (!fp) {
    std::perror(path);
    return 1;
  }
  std::fprintf(fp, "P6\n%d %d\n255\n", screenW, screenH);
  for (int i = 0; i < screenW * screenH; i++) {
    uint16_t p = g2::bswap16(fb[i]);  // RGB565_SWAPPED -> native
    uint8_t rgb[3] = {
        (uint8_t)(((p >> 11) & 31) * 255 / 31),
        (uint8_t)(((p >> 5) & 63) * 255 / 63),
        (uint8_t)((p & 31) * 255 / 31),
    };
    std::fwrite(rgb, 1, 3, fp);
  }
  std::fclose(fp);
  std::printf("wrote %s (%dx%d)\n", path, screenW, screenH);
  return 0;
}

#endif
