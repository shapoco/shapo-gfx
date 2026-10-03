#ifndef DEMO3D_SCENE_HPP
#define DEMO3D_SCENE_HPP

// Demo scene shared by the demo3d sample programs (example/wasm/demo3d/ and
// example/rp2350-touch-lcd-2/demo3d/): a floor, a chrome torus, three cubes
// (opaque, translucent, additive) and a windmill over a 2D backdrop.

#include "shapoco/gfx2d/graphics2d.hpp"
#include "shapoco/gfx3d/gfx3d.hpp"

namespace demo3d {

// Initial camera and its limits (kept in sync with
// docs/example/demo3d/index.html and demo3d.cpp)
constexpr float CAM_YAW_INIT = 0.6f;
constexpr float CAM_PITCH_INIT = 0.35f;
constexpr float CAM_DIST_INIT = 7.0f;
constexpr float CAM_PITCH_MIN = -0.2f, CAM_PITCH_MAX = 1.4f;
constexpr float CAM_DIST_MIN = 3.0f, CAM_DIST_MAX = 20.0f;

// Generate the textures (call once at startup)
void sceneInit();

// Build the scene (projection setup and beginScene() .. endScene()).
// The background (clear color) is left to the caller.
// Rendering (beginRender() / render() / endRender()) is left to the caller.
// t: elapsed seconds, yaw/pitch: camera angles (radians), dist: camera
// distance, aspect: screen aspect ratio (width / height)
void sceneBuild(shapoco::gfx3d::Graphics3D &r, float t, float yaw, float pitch,
                float dist, float aspect);

// Draw the 2D backdrop (gradient, twinkling stars and a caption) of a screen
// of width x height pixels at time t into g, through g's transform (a
// translation that moves the band to draw to the top of g's target, if any).
// The 3D scene is rendered over it with the clear disabled.
void drawBackdrop(shapoco::gfx2d::Graphics2D &g, int width, int height,
                  float t);

}  // namespace demo3d

#endif
