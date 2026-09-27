// demorig: sample program for the ShapoGFX 2D skeletal animation (rig.hpp).
//
// Built with Emscripten it exports a small C API used by
// docs/example/viewer.js. Built natively it renders a single frame to a PPM
// file.

#include <cstdint>

#include "shapoco/gfx2d/graphics2d.hpp"

#include "scene.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define DEMORIG_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DEMORIG_EXPORT
#endif

namespace g2 = shapoco::gfx2d;

static constexpr int SCREEN_W = demorig::SCREEN_W;
static constexpr int SCREEN_H = demorig::SCREEN_H;

static uint16_t fb[SCREEN_W * SCREEN_H];  // RGB565_SWAPPED
static const g2::Surface fbSurface = {g2::PixelFormat::RGB565_SWAPPED, SCREEN_W,
                                      SCREEN_H, SCREEN_W * 2, fb};
static g2::Graphics2D gfx;
// State stack and scratch memory of gfx
static uint8_t arena[4096];

// ---------------------------------------------------------------------------
// Exported API

extern "C" {

DEMORIG_EXPORT uint16_t *demorig_get_fb() { return fb; }
DEMORIG_EXPORT int demorig_get_width() { return SCREEN_W; }
DEMORIG_EXPORT int demorig_get_height() { return SCREEN_H; }

DEMORIG_EXPORT void demorig_init() {
  demorig::sceneInit();
  gfx.init(arena, sizeof(arena));
  gfx.setTarget(fbSurface);
}

// t: elapsed seconds
DEMORIG_EXPORT void demorig_frame(float t) { demorig::sceneRender(gfx, t); }

}  // extern "C"

// ---------------------------------------------------------------------------
// Native entry point: render one frame and write it as a binary PPM

#ifndef __EMSCRIPTEN__

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "demorig.ppm";
  float t = (argc > 2) ? (float)std::atof(argv[2]) : 1.0f;

  demorig_init();
  demorig_frame(t);

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
