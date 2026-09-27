#include "scene.hpp"

#include <cmath>
#include <cstdint>

#include "shapoco/gfx2d/fonts.hpp"
#include "shapoco/gfx2d/rig.hpp"

// Generated from assets/2d/rgb_chan with `make model`
#include "model/rgb_chan.hpp"

namespace demorig {

namespace g2 = shapoco::gfx2d;
namespace rig = shapoco::gfx2d::rig;
using g2::affine2f;

// One pose per character: they play the animation at different times
static constexpr int NUM_CHARAS = 3;
// rig::Instance::bytes(rgb_chan::armature) is 1235
alignas(4) static uint8_t rigMemory[NUM_CHARAS][1236];
static rig::Instance charas[NUM_CHARAS];
static int handBone = -1, handDrawIndex = 0;

void sceneInit() {
  for (int i = 0; i < NUM_CHARAS; i++) {
    charas[i].init(rgb_chan::armature, rigMemory[i], sizeof(rigMemory[i]));
  }
  handBone = charas[0].boneIndex("l_hand");
}

// The armature's origin is near its feet's left; this centers the bind pose
static affine2f placement(float x, float y, float angle, float sx, float sy) {
  const g2::RectF &b = rgb_chan::armature.bounds;
  return affine2f::placement(x, y, angle, sx, sy, b.x + b.width * 0.5f,
                             b.y + b.height * 0.5f);
}

static void drawBackground(g2::Graphics2D &g, float t) {
  for (int y = 0; y < SCREEN_H; y += 8) {
    const int v = 28 + y * 40 / SCREEN_H;
    g.fillRect(0, y, SCREEN_W, 8, g2::makeColor(v / 2, v * 3 / 4, v + 20));
  }
  // Floor tiles scrolling to the left
  const int off = (int)(t * 40.0f) % 40;
  for (int x = -off; x < SCREEN_W; x += 40) {
    g.fillRect(x, SCREEN_H - 24, 20, 24, g2::makeColor(52, 60, 84));
    g.fillRect(x + 20, SCREEN_H - 24, 20, 24, g2::makeColor(44, 50, 72));
  }
}

static void drawBounds(g2::Graphics2D &g, const rig::Instance &inst,
                       const affine2f &m) {
  const g2::RectF b = inst.bounds(m);
  g.drawRect(g2::Rect{(int)std::floor(b.x), (int)std::floor(b.y),
                      (int)std::ceil(b.width), (int)std::ceil(b.height)},
             g2::makeColor(255, 255, 255, 64));
}

void sceneRender(g2::Graphics2D &g, float t) {
  g.resetClipRect();
  g.resetTransform();
  g.setBlend(g2::BlendMode::ALPHA, 255);
  drawBackground(g, t);

  const rig::Animation &anim = rgb_chan::anim_animtion0;
  // 24 fps data, interpolated at any rate
  for (int i = 0; i < NUM_CHARAS; i++) {
    charas[i].pose(anim, rig::frameAt(anim, t + i * 0.9f));
  }

  // Left: as converted
  const affine2f m0 = placement(110, 100, 0.0f, 0.9f, 0.9f);
  // Right: mirrored (a negative scale), slightly smaller
  const affine2f m1 = placement(380, 96, 0.0f, -0.8f, 0.8f);
  // Center: enlarged and swaying
  const affine2f m2 =
      placement(250, 210, std::sin(t * 1.3f) * 0.25f, 1.25f, 1.25f);

  g.setTransform(m0);
  charas[0].draw(g);
  g.setTransform(m1);
  charas[1].draw(g);

  // A ball held in the left hand of the center one: drawn between the slots
  // before and after the hand, so the hand is in front of it
  rig::Instance &c = charas[2];
  const int handSlot = c.slotIndex("l_hand");
  handDrawIndex = c.drawIndexOf(handSlot);
  g.setTransform(m2);
  c.draw(g, 0, handDrawIndex);
  const affine2f &hand = c.boneTransform(handBone);
  const g2::vec2f p = (m2 * hand).apply(30.0f, -4.0f);  // on the palm
  g.resetTransform();
  const float r = 22.0f + std::sin(t * 4.0f) * 2.0f;
  g.fillCircle(p, r, g2::makeColorHsv((int)(t * 60.0f) % 360, 200, 255));
  g.fillCircle({p.x - r * 0.3f, p.y - r * 0.35f}, r * 0.3f,
               g2::makeColor(255, 255, 255, 160));
  g.setTransform(m2);
  c.draw(g, handDrawIndex, rgb_chan::armature.slotCount);

  g.resetTransform();
  drawBounds(g, charas[0], m0);
  drawBounds(g, charas[1], m1);
  drawBounds(g, charas[2], m2);

  g.setFont(&ShapoSansP_s12c09a01w02);
  g.setTextColor(g2::Colors::WHITE);
  g.drawString(8, 6, "rig::Instance: DragonBones -> dbones2cpp");
}

}  // namespace demorig
