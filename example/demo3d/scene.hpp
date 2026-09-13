#ifndef DEMO3D_SCENE_HPP
#define DEMO3D_SCENE_HPP

// Demo scene shared by the demo3d sample programs:
// a floor, a chrome torus and three cubes (opaque, translucent, additive).

#include "shapoco/gfx3d/gfx3d.hpp"

namespace demo3d {

// Initial camera and its limits (kept in sync with docs/example/demo3d/main.js)
constexpr float CAM_YAW_INIT = 0.6f;
constexpr float CAM_PITCH_INIT = 0.35f;
constexpr float CAM_DIST_INIT = 7.0f;
constexpr float CAM_PITCH_MIN = -0.2f, CAM_PITCH_MAX = 1.4f;
constexpr float CAM_DIST_MIN = 3.0f, CAM_DIST_MAX = 20.0f;

// Generate the textures and the torus (call once at startup)
void sceneInit();

// Build the scene (projection setup and beginScene() .. endScene()).
// Rendering (beginRender() / render() / endRender()) is left to the caller.
// t: elapsed seconds, yaw/pitch: camera angles (radians), dist: camera
// distance, aspect: screen aspect ratio (width / height)
void sceneBuild(shapoco::gfx3d::Renderer &r, float t, float yaw, float pitch,
                float dist, float aspect);

}  // namespace demo3d

#endif
