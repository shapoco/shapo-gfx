#ifndef DEMO2D_SCENE_HPP
#define DEMO2D_SCENE_HPP

// demo2d scene: exercises the Graphics2D API (shapes, lines, polygons, images
// in several pixel formats, alpha and additive blending, bitmap fonts).
// Only shapoco::gfx2d is used; the 3D renderer is not referenced.

#include "shapoco/gfx2d/graphics2d.hpp"

namespace demo2d {

constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 320;

// Generate the procedural images (call once at startup)
void sceneInit();

// Draw one frame at time t (seconds) into the target of g
void sceneRender(shapoco::gfx2d::Graphics2D &g, float t);

}  // namespace demo2d

#endif
