#ifndef DEMORIG_SCENE_HPP
#define DEMORIG_SCENE_HPP

// demorig scene: a DragonBones character (converted by bin/dbones2cpp) posed
// by shapoco::gfx2d::rig in a turning ring of additive rectangles, with
// falling stars over a scrolling checkerboard.
//
// The scene is laid out in world coordinates of width x height pixels (the
// screen size, so that at zoom 1 a world pixel is a screen pixel) and scaled
// with the height, 320 being the reference.

#include <cstdint>

#include "shapoco/gfx2d/graphics2d.hpp"
#include "shapoco/gfx2d/rig.hpp"

namespace demorig {

class Scene {
 public:
  // Set up the rig instance for a world of width x height pixels
  void init(int width, int height);

  // Pose the character and move the stars to time t (seconds). Call once per
  // frame, before draw().
  void update(float t);

  // Draw the scene with g's transform as the view (world -> target, scale
  // and translation only). The background color fills g's clip rectangle;
  // everything else is clipped to the world rectangle as well. Const, so
  // several contexts may draw different bands of one frame at once.
  void draw(shapoco::gfx2d::Graphics2D &g) const;

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  static constexpr int NUM_STARS = 24;
  static constexpr int STAR_VERTS = 10;
  static constexpr int RING_RECTS = 40;

  // Bounding box in world coordinates, to skip what a band does not show
  struct Box {
    float x0, y0, x1, y1;
    bool overlaps(const Box &b) const {
      return x0 < b.x1 && b.x0 < x1 && y0 < b.y1 && b.y0 < y1;
    }
  };

  int width_ = 0, height_ = 0;
  float scale_ = 1.0f;  // height / 320
  float t_ = 0.0f;

  // rig::Instance::bytes(rgb_chan::armature) is 1235
  alignas(4) uint8_t rigMemory_[2048];
  shapoco::gfx2d::rig::Instance rig_;
  int handDrawIndex_ = 0;

  shapoco::gfx2d::vec2f stars_[NUM_STARS][STAR_VERTS];
  shapoco::gfx2d::Color starColors_[NUM_STARS];
  Box starBoxes_[NUM_STARS];

  float ringAngles_[RING_RECTS];
  Box ringBoxes_[RING_RECTS];

  void updateStars(float t);
  void updateRing();
  void drawStars(shapoco::gfx2d::Graphics2D &g, const Box &view) const;
  void drawBackground(shapoco::gfx2d::Graphics2D &g, const Box &view) const;
  void drawCircle(shapoco::gfx2d::Graphics2D &g, const Box &view,
                  bool front) const;
};

}  // namespace demorig

#endif
