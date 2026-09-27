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

// rig::Instance::bytes(rgb_chan::armature) is 1235
alignas(4) static uint8_t rigMemory[2048];
static rig::Instance rigInst;

void sceneInit() {
  rigInst.init(rgb_chan::armature, rigMemory, sizeof(rigMemory));
}

// Colorful stars falling diagonally behind the character, outlined and
// filled alternately like those of demo2d
static constexpr int NUM_STARS = 24;

static float hash01(uint32_t n) {
  n = (n ^ 61u) ^ (n >> 16);
  n *= 9u;
  n ^= n >> 4;
  n *= 0x27d4eb2du;
  n ^= n >> 15;
  return (float)(n & 0xFFFFu) / 65535.0f;
}

static void drawStars(g2::Graphics2D &g, float t) {
  g.pushState();
  g.setBlendMode(g2::BlendMode::ADD);
  for (int i = 0; i < NUM_STARS; i++) {
    // Nearer stars are larger and fall faster
    const float zInv = 1.0f / (hash01(i * 3 + 3) * 2.0f + 1.0f);
    const float r = 28.0f * zInv;
    const float w = SCREEN_W + 2 * r, h = SCREEN_H + 2 * r;
    float x = std::fmod(hash01(i * 3 + 1) * w - t * 40.0f * zInv, w);
    float y = std::fmod(hash01(i * 3 + 2) * h + t * 60.0f * zInv, h);
    if (x < 0) x += w;
    if (y < 0) y += h;
    x -= r;
    y -= r;
    const float angle = t * (i & 2 ? 1.2f : -1.2f) + i;
    g2::vec2f v[10];
    for (int k = 0; k < 10; k++) {
      const float dist = (k & 1) ? r : r * 0.45f;
      const float th = angle + k * (float)M_PI / 5.0f;
      v[k] = {x + std::cos(th) * dist, y + std::sin(th) * dist};
    }
    const g2::Color col =
        g2::makeColorHsv(i * 360 / NUM_STARS + (int)(t * 40), 200, 240);
    if (i & 1) {
      g.fillPolygon(v, 10, g2::colorWithAlpha(col, 200));
    } else {
      g.drawPolygon(v, 10, col);
    }
  }
  g.popState();
}

static void drawBackground(g2::Graphics2D &g, float t) {
  g.clear(g2::makeColor(64, 64, 64));
  const int sz = SCREEN_H / 16;
  const g2::Color col = g2::makeColor(80, 80, 80);
  const int shift = (int)(t * sz) % (sz * 2);
  for (int iy = -2; iy < SCREEN_H / sz; iy++) {
    for (int ix = -2; ix < SCREEN_W / sz; ix++) {
      if ((ix + iy) % 2 == 0) {
        g.fillRect(ix * sz + shift, iy * sz + shift, sz, sz, col);
      }
    }
  }
}

static void drawCircle(g2::Graphics2D &g, float t, bool front) {
  const int N = 40;
  g.pushState();
  g.setBlendMode(g2::BlendMode::ADD);
  g.rotate(M_PI / 8.0f);
  g.scale(1.0f, 0.3f);
  for (int i = 0; i < N; i++) {
    float a =
        ((int)(i * 65536 / N + t * 1024) % 65536) / 65536.0f * 2.0f * M_PI;
    if (front ^ (M_PI / 2.0f < a && a < M_PI * 3.0f / 2.0f)) {
      g.pushState();
      g.rotate(a);
      g.fillRect(-5, 150, 10, 40,
                 g2::makeColorHsv((i * (360 / N)) % 360, 200, 255));
      g.popState();
    }
  }
  g.popState();
}

void sceneRender(g2::Graphics2D &g, float t) {
  g.resetClipRect();
  g.resetTransform();
  g.setBlend(g2::BlendMode::ALPHA, 255);
  drawBackground(g, t);
  drawStars(g, t);

  const rig::Animation &anim = rgb_chan::anim_animtion0;
  // 24 fps data, interpolated at any rate
  float t2 = ((int)(t * 1000) % 2000) / 1000.0f + 1.0f;
  rigInst.pose(anim, rig::frameAt(anim, t2));

  // Draw a scene where a ring of rectangles rotates around the character.
  // To make the character appear to be placed inside the ring,
  // draw the back side of the ring first, then the character, and finally the
  // front side of the ring. However, the character's left hand is drawn in
  // front of the ring.

  const int handSlot = rigInst.slotIndex("l_arm");
  const int handDrawIndex = rigInst.drawIndexOf(handSlot);

  g.pushState();
  g.resetTransform();
  g.translate(SCREEN_W / 2, SCREEN_H / 2);

  const float scale = (float)SCREEN_H / 320;
  g.scale(scale, scale);

  drawCircle(g, t, false);

  g.pushState();
  g.translate(0, sinf(t * 2.0f) * scale * 10);
  rigInst.draw(g, 0, handDrawIndex);
  g.popState();

  drawCircle(g, t, true);

  g.pushState();
  g.translate(0, sinf(t * 2.0f) * scale * 10);
  rigInst.draw(g, handDrawIndex, rgb_chan::armature.slotCount);
  g.popState();

  g.popState();

  // g.resetTransform();
  // g.setFont(&ShapoSansP_s12c09a01w02);
  // g.setTextColor(g2::Colors::WHITE);
  // g.drawString(8, 6, "rig::Instance: DragonBones -> dbones2cpp");
}

} // namespace demorig
