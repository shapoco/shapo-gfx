// demo3d: sample program for the ShapoGFX 3D renderer.
//
// Built with Emscripten it exports a small C API used by
// docs/example/demo3d/main.js to drive the renderer from a browser. Built
// natively it renders a single frame to a PPM file (useful for quick checks
// without a browser).

#include <cstdint>

#include "shapoco/gfx3d/gfx3d.hpp"

#include "scene.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define DEMO3D_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DEMO3D_EXPORT
#endif

namespace g3 = shapoco::gfx3d;

static constexpr int SCREEN_W = 480;
static constexpr int SCREEN_H = 320;

static uint16_t fb[SCREEN_W * SCREEN_H];
static uint8_t arena[128 * 1024];
static g3::Renderer renderer;

// ---------------------------------------------------------------------------
// Exported API

extern "C" {

DEMO3D_EXPORT uint16_t *demo3d_get_fb() { return fb; }
DEMO3D_EXPORT int demo3d_get_width() { return SCREEN_W; }
DEMO3D_EXPORT int demo3d_get_height() { return SCREEN_H; }

DEMO3D_EXPORT void demo3d_init() {
  demo3d::sceneInit();
  renderer.init(SCREEN_W, SCREEN_H, arena, sizeof(arena));
}

// t: elapsed seconds, yaw/pitch: camera angles (radians), dist: camera distance
DEMO3D_EXPORT void demo3d_frame(float t, float yaw, float pitch, float dist) {
  demo3d::sceneBuild(renderer, t, yaw, pitch, dist, (float)SCREEN_W / SCREEN_H);

  renderer.beginRender();
  // Render in 4 bands, as a device with a small transfer buffer would do
  constexpr int BAND_H = SCREEN_H / 4;
  for (int i = 0; i < 4; i++) {
    int y = i * BAND_H;
    renderer.render(0, (int16_t)y, SCREEN_W, BAND_H, fb + (size_t)y * SCREEN_W,
                    SCREEN_W);
  }
  renderer.endRender();
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
    uint16_t p = fb[i];
    uint8_t rgb[3] = {
        (uint8_t)(((p >> 11) & 31) * 255 / 31),
        (uint8_t)(((p >> 5) & 63) * 255 / 63),
        (uint8_t)((p & 31) * 255 / 31),
    };
    std::fwrite(rgb, 1, 3, fp);
  }
  std::fclose(fp);

  g3::Stats st = renderer.getStats();
  std::printf("wrote %s (%dx%d)\n", path, SCREEN_W, SCREEN_H);
  std::printf("arena: %zu / %zu bytes used\n", st.arenaUsed, st.arenaSize);
  std::printf("triangles: %d / %d (dropped %d)\n", st.triCount, st.triCapacity,
              st.triDropped);
  std::printf("spans: peak %d / %d (dropped %d)\n", st.spanPeak,
              st.spanCapacity, st.spanDropped);
  return 0;
}

#endif
