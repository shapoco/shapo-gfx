#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Generated from assets/2d/rgb_chan with `make -C example/wasm/demorig model`:
// the atlas, or the header a build names (model/rgb_chan_sep.hpp with one
// texture per image, which the M5Stack builds use)
#ifdef DEMORIG_MODEL_HEADER
#include DEMORIG_MODEL_HEADER
#else
#include "model/rgb_chan.hpp"
#endif
// Generated from assets/2d/pop_star.svg by the same make target
#include "model/pop_star.hpp"

namespace demorig {

namespace g2 = shapoco::gfx2d;
namespace rig = shapoco::gfx2d::rig;

void Scene::init(int width, int height) {
  width_ = width;
  height_ = height;
  scale_ = (float)height / 320;
  rig_.init(rgb_chan::armature, rigMemory_, sizeof(rigMemory_));
  handDrawIndex_ = rig_.drawIndexOf(rig_.slotIndex("l_arm"));
  for (int k = 0; k < NUM_POPS; k++) {
    popRigs_[k].init(pop_star::armature, popMemory_[k], sizeof(popMemory_[k]));
    pops_[k] = Pop();
  }
  lastBurst_ = -1;
  update(0.0f);
}

void Scene::update(float t) {
  t_ = t;
  const rig::Animation &anim = rgb_chan::anim_main;
  // 24 fps data, interpolated at any rate
  float t2 = ((int)(t * 1000) % 2000) / 1000.0f + 1.0f;
  rig_.pose(anim, rig::frameAt(anim, t2));
  updateStars(t);
  updateRing();
  updatePops(t);
}

// Bounding box of points in world coordinates
template <typename F>
static void grow(float &x0, float &y0, float &x1, float &y1, F &&point) {
  const g2::vec2f p = point();
  x0 = std::min(x0, p.x);
  y0 = std::min(y0, p.y);
  x1 = std::max(x1, p.x);
  y1 = std::max(y1, p.y);
}

// Colorful stars falling diagonally behind the character, outlined and
// filled alternately like those of demo2d

static float hash01(uint32_t n) {
  n = (n ^ 61u) ^ (n >> 16);
  n *= 9u;
  n ^= n >> 4;
  n *= 0x27d4eb2du;
  n ^= n >> 15;
  return (float)(n & 0xFFFFu) / 65535.0f;
}

// The vertices are computed once per frame rather than per band
void Scene::updateStars(float t) {
  for (int i = 0; i < NUM_STARS; i++) {
    // Nearer stars are larger and fall faster
    const float zInv = 1.0f / (hash01(i * 3 + 3) * 2.0f + 1.0f);
    const float r = 28.0f * zInv * scale_;
    const float w = width_ + 2 * r, h = height_ + 2 * r;
    float x = std::fmod(hash01(i * 3 + 1) * w - t * 40.0f * zInv * scale_, w);
    float y = std::fmod(hash01(i * 3 + 2) * h + t * 60.0f * zInv * scale_, h);
    if (x < 0) x += w;
    if (y < 0) y += h;
    x -= r;
    y -= r;
    const float angle = t * (i & 2 ? 1.2f : -1.2f) + i;
    Box &box = starBoxes_[i];
    box = {x, y, x, y};
    for (int k = 0; k < STAR_VERTS; k++) {
      const float dist = (k & 1) ? r : r * 0.45f;
      const float th = angle + k * (float)M_PI / 5.0f;
      stars_[i][k] = {x + std::cos(th) * dist, y + std::sin(th) * dist};
      grow(box.x0, box.y0, box.x1, box.y1, [&] { return stars_[i][k]; });
    }
    starColors_[i] =
        g2::makeColorHsv(i * 360 / NUM_STARS + (int)(t * 40), 200, 240);
  }
}

void Scene::drawStars(g2::Graphics2D &g, const Box &view) const {
  g.pushState();
  g.setBlendMode(g2::BlendMode::ADD);
  for (int i = 0; i < NUM_STARS; i++) {
    if (!starBoxes_[i].overlaps(view)) continue;
    const g2::Color col = starColors_[i];
    if (i & 1) {
      g.fillPolygon(stars_[i], STAR_VERTS, g2::colorWithAlpha(col, 200));
    } else {
      g.drawPolygon(stars_[i], STAR_VERTS, col);
    }
  }
  g.popState();
}

void Scene::drawBackground(g2::Graphics2D &g, const Box &view) const {
  const int sz = height_ / 16;
  const g2::Color col = g2::makeColor(80, 80, 80);
  const int shift = (int)(t_ * sz) % (sz * 2);
  // Up to and including width / sz: the last column (row) covers the edge
  // when the size is not a multiple of sz. Only the squares in view.
  auto first = [&](float v) {
    return std::max(-2, (int)std::floor((v - shift) / sz));
  };
  auto last = [&](float v, int n) {
    return std::min(n / sz, (int)std::floor((v - shift) / sz));
  };
  const int iy0 = first(view.y0), iy1 = last(view.y1, height_);
  const int ix0 = first(view.x0), ix1 = last(view.x1, width_);
  for (int iy = iy0; iy <= iy1; iy++) {
    for (int ix = ix0; ix <= ix1; ix++) {
      if ((ix + iy) % 2 == 0) {
        g.fillRect(ix * sz + shift, iy * sz + shift, sz, sz, col);
      }
    }
  }
}

// Pop stars: a burst every POP_EVERY seconds, the two instances in turn,
// each at a random place, size (x0.5 to x2) and angle from the burst number
void Scene::updatePops(float t) {
  const int burst = std::max(-1, (int)std::floor(t / POP_EVERY));
  if (burst - lastBurst_ > NUM_POPS) lastBurst_ = burst - NUM_POPS;  // a jump in time
  while (lastBurst_ < burst) {
    lastBurst_++;
    const uint32_t n = (uint32_t)lastBurst_ * 4u + 1000u;
    Pop &pop = pops_[lastBurst_ % NUM_POPS];
    pop.active = true;
    pop.start = lastBurst_ * POP_EVERY;
    const float x = hash01(n) * width_, y = hash01(n + 1) * height_;
    const float size = (0.5f + hash01(n + 2) * 1.5f) * scale_;
    const float angle = hash01(n + 3) * 2.0f * (float)M_PI;
    // The picture is 64 x 64 with the star at its center
    pop.placement = g2::affine2f::placement(x, y, angle, size, size, 32.0f, 32.0f);
  }
  const rig::Animation &anim = pop_star::anim_main;
  for (int k = 0; k < NUM_POPS; k++) {
    Pop &pop = pops_[k];
    if (!pop.active) continue;
    const float elapsed = t - pop.start;
    if (elapsed < 0.0f || elapsed * anim.frameRate > anim.duration) {
      pop.active = false;  // played to the end (no loop)
      continue;
    }
    popRigs_[k].pose(anim, rig::frameAt(anim, elapsed, false));
    const g2::RectF b = popRigs_[k].bounds(pop.placement);
    pop.box = {b.x, b.y, b.right(), b.bottom()};
  }
}

void Scene::drawPops(g2::Graphics2D &g, const Box &view) const {
  for (int k = 0; k < NUM_POPS; k++) {
    const Pop &pop = pops_[k];
    if (!pop.active || !pop.box.overlaps(view)) continue;
    g.pushState();
    g.applyTransform(pop.placement);
    popRigs_[k].draw(g);
    g.popState();
  }
}

// The angles of the ring's rectangles and their boxes in the world
void Scene::updateRing() {
  const int N = RING_RECTS;
  const g2::affine2f base =
      g2::affine2f::translation(width_ / 2, height_ / 2) *
      g2::affine2f::scaling(scale_) * g2::affine2f::rotation(M_PI / 8.0f) *
      g2::affine2f::scaling(1.0f, 0.3f);
  for (int i = 0; i < N; i++) {
    float a =
        ((int)(i * 65536 / N + t_ * 1024) % 65536) / 65536.0f * 2.0f * M_PI;
    ringAngles_[i] = a;
    const g2::affine2f m = base * g2::affine2f::rotation(a);
    Box &box = ringBoxes_[i];
    box = {1e9f, 1e9f, -1e9f, -1e9f};
    for (int k = 0; k < 4; k++) {
      grow(box.x0, box.y0, box.x1, box.y1, [&] {
        return m.apply((k & 1) ? 5.0f : -5.0f, (k & 2) ? 190.0f : 150.0f);
      });
    }
  }
}

void Scene::drawCircle(g2::Graphics2D &g, const Box &view, bool front) const {
  const int N = RING_RECTS;
  g.pushState();
  g.setBlendMode(g2::BlendMode::ADD);
  g.rotate(M_PI / 8.0f);
  g.scale(1.0f, 0.3f);
  for (int i = 0; i < N; i++) {
    const float a = ringAngles_[i];
    if (!ringBoxes_[i].overlaps(view)) continue;
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

// The world rectangle mapped by m, as the target pixels whose centers it
// covers, clamped far outside any target
static g2::Rect worldRect(const g2::affine2f &m, int w, int h) {
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  const float xs[2] = {0.0f, (float)w}, ys[2] = {0.0f, (float)h};
  for (float wx : xs) {
    for (float wy : ys) {
      const float x = m.a * wx + m.c * wy + m.tx;
      const float y = m.b * wx + m.d * wy + m.ty;
      x0 = std::min(x0, x);
      x1 = std::max(x1, x);
      y0 = std::min(y0, y);
      y1 = std::max(y1, y);
    }
  }
  auto px = [](float v) {
    return (int)std::ceil(std::clamp(v - 0.5f, -30000.0f, 30000.0f));
  };
  return {px(x0), px(y0), px(x1) - px(x0), px(y1) - px(y0)};
}

void Scene::draw(g2::Graphics2D &g) const {
  g.pushState();
  g.setBlend(g2::BlendMode::ALPHA, 255);
  // The whole clip rectangle, inside the world or not
  g.clear(g2::makeColor(64, 64, 64));

  const g2::Rect clip = g.clipRect();
  const g2::Rect world = worldRect(g.transform(), width_, height_);
  const int x0 = std::max(clip.x, world.x), y0 = std::max(clip.y, world.y);
  const int x1 = std::min(clip.right(), world.right());
  const int y1 = std::min(clip.bottom(), world.bottom());
  if (x1 <= x0 || y1 <= y0) {
    g.popState();
    return;
  }
  g.setClipRect(x0, y0, x1 - x0, y1 - y0);

  // The part of the world in the clip rectangle, a pixel wider
  Box view = {1e9f, 1e9f, -1e9f, -1e9f};
  g2::affine2f inv;
  if (!g.transform().invert(inv)) {
    g.popState();
    return;
  }
  for (int k = 0; k < 4; k++) {
    grow(view.x0, view.y0, view.x1, view.y1, [&] {
      return inv.apply((k & 1) ? x1 + 1.0f : x0 - 1.0f,
                       (k & 2) ? y1 + 1.0f : y0 - 1.0f);
    });
  }

  drawBackground(g, view);
  drawStars(g, view);
  drawPops(g, view);

  // Draw a scene where a ring of rectangles rotates around the character.
  // To make the character appear to be placed inside the ring,
  // draw the back side of the ring first, then the character, and finally the
  // front side of the ring. However, the character's left hand is drawn in
  // front of the ring.

  g.translate(width_ / 2, height_ / 2);
  g.scale(scale_, scale_);

  drawCircle(g, view, false);

  const float bob = sinf(t_ * 2.0f) * 10;

  g.pushState();
  g.translate(0, bob);
  rig_.draw(g, 0, handDrawIndex_);
  g.popState();

  drawCircle(g, view, true);

  g.pushState();
  g.translate(0, bob);
  rig_.draw(g, handDrawIndex_, rgb_chan::armature.slotCount);
  g.popState();

  g.popState();
}

}  // namespace demorig
