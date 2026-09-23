// demo3d: sample program for the ShapoGFX 3D renderer.
//
// Built with Emscripten it exports a small C API used by
// docs/example/demo3d/main.js to drive the renderer from a browser. Built
// natively it renders a single frame to a PPM file (useful for quick checks
// without a browser).

#include <cmath>
#include <cstdint>

#include "shapoco/gfx2d/fonts.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"
#include "shapoco/gfx3d/gfx3d.hpp"

#include "scene.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define DEMO3D_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DEMO3D_EXPORT
#endif

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;

static constexpr int SCREEN_W = 480;
static constexpr int SCREEN_H = 320;

static uint16_t fb[SCREEN_W * SCREEN_H];  // RGB565_SWAPPED
static const g2::Surface fbSurface = {g2::PixelFormat::RGB565_SWAPPED, SCREEN_W,
                                      SCREEN_H, SCREEN_W * 2, fb};
static uint8_t arena[128 * 1024];
static g3::Graphics3D g3d;

// ---------------------------------------------------------------------------
// Exported API

extern "C" {

DEMO3D_EXPORT uint16_t *demo3d_get_fb() { return fb; }
DEMO3D_EXPORT int demo3d_get_width() { return SCREEN_W; }
DEMO3D_EXPORT int demo3d_get_height() { return SCREEN_H; }

DEMO3D_EXPORT void demo3d_init() {
  demo3d::sceneInit();
  g3d.init(SCREEN_W, SCREEN_H, arena, sizeof(arena));
  g3d.disableClear();  // the 2D backdrop provides the background
}

// 2D backdrop: vertical gradient, twinkling stars and a caption
static void drawBackdrop(float t) {
  g2::Graphics2D g(fbSurface);
  constexpr int BANDS = 20;
  const g2::Color top = g2::makeColor(4, 6, 24),
                  horizon = g2::makeColor(40, 30, 70);
  for (int i = 0; i < BANDS; i++) {
    int y0 = i * SCREEN_H / BANDS, y1 = (i + 1) * SCREEN_H / BANDS;
    g.fillRect(0, y0, SCREEN_W, y1 - y0,
               g2::lerpColor(top, horizon, i * 256 / (BANDS - 1)));
  }
  for (int i = 0; i < 60; i++) {
    uint32_t h = (uint32_t)i * 2654435761u;
    int x = (int)(h % SCREEN_W), y = (int)((h >> 9) % (SCREEN_H * 2 / 3));
    int tw = 140 + (int)(100.0f * std::sin(t * 2.0f + i));
    g.setPixel(x, y, g2::makeColor(255, 255, 230, tw));
  }
  g.setFont(&ShapoSansP_s12c09a01w02);
  g.setTextColor(g2::makeColor(0, 0, 0, 160));
  g.drawString(9, 9, "ShapoGFX demo3d");
  g.setTextColor(g2::makeColor(220, 230, 255));
  g.drawString(8, 8, "ShapoGFX demo3d");
  g.setFont(&ShapoSansP_s08c07);
  g.setTextColor(g2::makeColor(160, 170, 200));
  g.drawString(8, 30, "3D scene rendered over a 2D backdrop (clear disabled)");
}

// t: elapsed seconds, yaw/pitch: camera angles (radians), dist: camera distance
DEMO3D_EXPORT void demo3d_frame(float t, float yaw, float pitch, float dist) {
  drawBackdrop(t);
  demo3d::sceneBuild(g3d, t, yaw, pitch, dist, (float)SCREEN_W / SCREEN_H);

  g3d.beginRender();
  // Render in 4 bands, as a device with a small transfer buffer would do
  constexpr int BAND_H = SCREEN_H / 4;
  for (int i = 0; i < 4; i++) {
    int y = i * BAND_H;
    g3d.render(0, (int16_t)y, SCREEN_W, BAND_H, fbSurface, 0, (int16_t)y);
  }
  g3d.endRender();
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Native entry point: render one frame and write it as a binary PPM

#ifndef __EMSCRIPTEN__

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "demo3d.ppm";
  float t = (argc > 2) ? (float)std::atof(argv[2]) : 1.0f;

  demo3d_init();
  demo3d_frame(t, demo3d::CAM_YAW_INIT, demo3d::CAM_PITCH_INIT,
               demo3d::CAM_DIST_INIT);

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

  g3::Stats st = g3d.getStats();
  std::printf("wrote %s (%dx%d)\n", path, SCREEN_W, SCREEN_H);
  std::printf("arena: %zu / %zu bytes used\n", st.arenaUsed, st.arenaSize);
  std::printf("triangles: %d in %zu / %zu bytes (dropped %d)\n", st.triCount,
              st.triBytes, st.triBytesTotal, st.triDropped);
  std::printf("spans: peak %d / %d (dropped %d)\n", st.spanPeak,
              st.spanCapacity, st.spanDropped);
  return 0;
}

#endif
