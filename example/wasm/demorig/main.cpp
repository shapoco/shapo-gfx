// demorig: sample program for the ShapoGFX 2D skeletal animation (rig.hpp).
// The scene, the view and the overlay are example/common/demorig/, shared
// with the M5Stack builds.
//
// Built with Emscripten it exports a small C API used by
// docs/example/viewer.js. Built natively it renders a single frame to a PPM
// file.

#include <cstdint>

#include "shapoco/gfx2d/graphics2d.hpp"

#include "demorig.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define DEMORIG_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DEMORIG_EXPORT
#endif

namespace g2 = shapoco::gfx2d;

static constexpr int DEFAULT_W = 480;
static constexpr int DEFAULT_H = 320;
static constexpr int MIN_DIM = 64;
static constexpr int MAX_DIM = 1280;
static constexpr int MAX_PIXELS = 1280 * 720;

static uint16_t fb[MAX_PIXELS];  // RGB565_SWAPPED
static int screenW = DEFAULT_W;
static int screenH = DEFAULT_H;
static g2::Graphics2D gfx;
// State stack and scratch memory of gfx (the antialiased drawing keeps the
// edges of a shape and the coverage of a row there: 16 KB draws the pop
// stars and the buttons at full quality)
static uint8_t arena[16384];
static demorig::Demo demo;

// ---------------------------------------------------------------------------
// Exported API

extern "C" {

DEMORIG_EXPORT uint16_t *demorig_get_fb() { return fb; }
DEMORIG_EXPORT int demorig_get_width() { return screenW; }
DEMORIG_EXPORT int demorig_get_height() { return screenH; }

// Choose the screen size (viewer.js takes it from ?screen=WxH). Returns 1
// when the size was taken, 0 when it was rejected (too small, too large, or
// more pixels than the buffer holds). Call it before demorig_init().
DEMORIG_EXPORT int demorig_set_screen(int w, int h) {
  if (w < MIN_DIM || h < MIN_DIM || w > MAX_DIM || h > MAX_DIM) return 0;
  if ((long)w * h > MAX_PIXELS) return 0;
  screenW = w;
  screenH = h;
  return 1;
}

DEMORIG_EXPORT void demorig_init() {
  demo.init(screenW, screenH, true);
  gfx.init(arena, sizeof(arena));
  gfx.setTarget({g2::PixelFormat::RGB565_SWAPPED, (int16_t)screenW,
                 (int16_t)screenH, (uint32_t)(screenW * 2), fb});
}

// t: elapsed seconds
DEMORIG_EXPORT void demorig_frame(float t) {
  demo.update(t);
  demo.draw(gfx, 0);
}

// Pointer (mouse or touch) in frame buffer pixels; hover: a mouse moving
// with no button down, leave: it left the canvas
DEMORIG_EXPORT void demorig_pointer_down(int x, int y) { demo.pointerDown(x, y); }
DEMORIG_EXPORT void demorig_pointer_move(int x, int y) { demo.pointerMove(x, y); }
DEMORIG_EXPORT void demorig_pointer_up() { demo.pointerUp(); }
DEMORIG_EXPORT void demorig_pointer_hover(int x, int y) { demo.pointerHover(x, y); }
DEMORIG_EXPORT void demorig_pointer_leave() { demo.pointerLeave(); }

}  // extern "C"

// ---------------------------------------------------------------------------
// Native entry point: render one frame and write it as a binary PPM
//
//   demorig [out.ppm] [seconds] [WxH] [zoom [centerX centerY]] [bands] [aa]
//           [pointerX pointerY]
//
// zoom is the factor (0.25 to 16 or beyond), the center the scene point at
// the middle of the screen (default: the middle of the scene), bands the
// number of bands the frame is drawn in (default 1, the way the M5Stack
// builds draw it when greater), aa 1 to draw with antialiasing and the
// pointer a screen pixel the mouse hovers (the part of the character under
// it lights up).

#ifndef __EMSCRIPTEN__

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "demorig.ppm";
  float t = (argc > 2) ? (float)std::atof(argv[2]) : 1.0f;
  if (argc > 3) {
    int w = 0, h = 0;
    if (std::sscanf(argv[3], "%dx%d", &w, &h) != 2 ||
        !demorig_set_screen(w, h)) {
      std::fprintf(stderr, "bad screen size: %s\n", argv[3]);
      return 1;
    }
  }
  demorig_init();
  if (argc > 4) {
    const float zoom = (float)std::atof(argv[4]);
    const float cx = (argc > 6) ? (float)std::atof(argv[5]) : screenW / 2.0f;
    const float cy = (argc > 6) ? (float)std::atof(argv[6]) : screenH / 2.0f;
    demo.setView(zoom, cx, cy);
  }
  const int bands = (argc > 7) ? std::atoi(argv[7]) : 1;
  if (argc > 8) demo.setAntialias(std::atoi(argv[8]) != 0);
  if (argc > 10) demo.pointerHover(std::atoi(argv[9]), std::atoi(argv[10]));
  demo.update(t);
  if (bands <= 1) {
    demo.draw(gfx, 0);
  } else {
    for (int b = 0; b < bands; b++) {
      const int y0 = screenH * b / bands, y1 = screenH * (b + 1) / bands;
      gfx.setTarget({g2::PixelFormat::RGB565_SWAPPED, (int16_t)screenW,
                     (int16_t)(y1 - y0), (uint32_t)(screenW * 2),
                     fb + y0 * screenW});
      demo.draw(gfx, y0);
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
