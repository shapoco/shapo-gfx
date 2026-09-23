// demo2d: sample program for the ShapoGFX 2D API.
//
// Built with Emscripten it exports a small C API used by
// docs/example/viewer.js. Built natively it renders a single frame to a PPM
// file.

#include <cstdint>

#include "shapoco/gfx2d/graphics2d.hpp"

#include "scene.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define DEMO2D_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DEMO2D_EXPORT
#endif

namespace g2 = shapoco::gfx2d;

static constexpr int SCREEN_W = demo2d::SCREEN_W;
static constexpr int SCREEN_H = demo2d::SCREEN_H;

static uint16_t fb[SCREEN_W * SCREEN_H];  // RGB565_SWAPPED
static const g2::Surface fbSurface = {g2::PixelFormat::RGB565_SWAPPED, SCREEN_W,
                                      SCREEN_H, SCREEN_W * 2, fb};
static g2::Graphics2D gfx;

// ---------------------------------------------------------------------------
// Exported API

extern "C" {

DEMO2D_EXPORT uint16_t *demo2d_get_fb() { return fb; }
DEMO2D_EXPORT int demo2d_get_width() { return SCREEN_W; }
DEMO2D_EXPORT int demo2d_get_height() { return SCREEN_H; }

DEMO2D_EXPORT void demo2d_init() {
  demo2d::sceneInit();
  gfx.setTarget(fbSurface);
}

// t: elapsed seconds
DEMO2D_EXPORT void demo2d_frame(float t) { demo2d::sceneRender(gfx, t); }

}  // extern "C"

// ---------------------------------------------------------------------------
// Native entry point: render one frame and write it as a binary PPM

#ifndef __EMSCRIPTEN__

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "demo2d.ppm";
  float t = (argc > 2) ? (float)std::atof(argv[2]) : 1.0f;

  demo2d_init();
  demo2d_frame(t);

  FILE *fp = std::fopen(path, "wb");
  if (!fp) {
    std::perror(path);
    return 1;
  }
  std::fprintf(fp, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
    uint16_t p = g2::bswap16(fb[i]);  // RGB565_SWAPPED -> native
    uint8_t rgb[3] = {
        (uint8_t)(((p >> 11) & 31) * 255 / 31),
        (uint8_t)(((p >> 5) & 63) * 255 / 63),
        (uint8_t)((p & 31) * 255 / 31),
    };
    std::fwrite(rgb, 1, 3, fp);
  }
  std::fclose(fp);
  std::printf("wrote %s (%dx%d)\n", path, SCREEN_W, SCREEN_H);
  return 0;
}

#endif
