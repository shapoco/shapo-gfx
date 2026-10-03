#ifndef DEMO2D_SCENE_HPP
#define DEMO2D_SCENE_HPP

// demo2d scene: exercises the Graphics2D API (shapes, lines, polygons, images
// in several pixel formats, alpha and additive blending, bitmap fonts).
// Only shapoco::gfx2d is used; the 3D renderer is not referenced. Shared by
// the WASM build (example/wasm/demo2d/) and the RP2350 one
// (example/rp2350-touch-lcd-2/demo2d/).
//
//   sceneInit(w, h);
//   every frame:
//     sceneUpdate(seconds);
//     sceneDraw(g, 0);              // the whole screen, or
//     sceneDraw(gBand, bandY);      // one band of it per call

#include "shapoco/gfx2d/graphics2d.hpp"

namespace demo2d {

// The screen sizes the layouts are made for: 480x320, and a compact one for
// 320x240 (any smaller screen than 480x320 gets the compact layout)
constexpr int LARGE_W = 480, LARGE_H = 320;
constexpr int COMPACT_W = 320, COMPACT_H = 240;

// Generate the procedural images and place the objects for a screen of
// width x height pixels (call once at startup)
void sceneInit(int width, int height);

// Advance to time t (seconds): move the balls and draw the off-screen panel.
// Call once per frame, before sceneDraw().
void sceneUpdate(float t);

// Draw the screen rows [bandY, bandY + g.target().height) into g, which
// needs an arena (Graphics2D::init) for pushState(). Only reads the scene,
// so several contexts may draw different bands of one frame at once.
void sceneDraw(shapoco::gfx2d::Graphics2D &g, int bandY);

}  // namespace demo2d

#endif
