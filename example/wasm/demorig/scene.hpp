#ifndef DEMORIG_SCENE_HPP
#define DEMORIG_SCENE_HPP

// demorig scene: a DragonBones character (converted by bin/dbones2cpp) posed
// and drawn with shapoco::gfx2d::rig, three times with different placements,
// with a ball drawn between two slots and the bounding boxes.

#include "shapoco/gfx2d/graphics2d.hpp"

namespace demorig {

constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 320;

// Set up the rig instances (call once at startup)
void sceneInit();

// Draw one frame at time t (seconds) into the target of g
void sceneRender(shapoco::gfx2d::Graphics2D &g, float t);

}  // namespace demorig

#endif
