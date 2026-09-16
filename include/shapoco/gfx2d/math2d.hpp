#ifndef SHAPOGFX2D_MATH2D_HPP
#define SHAPOGFX2D_MATH2D_HPP

#include "shapoco/gfx2d/config.hpp"

namespace shapoco::gfx2d {

// ---------------------------------------------------------------------------
// Scalar helpers

static inline float clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static constexpr int clampInt(int lo, int hi, int v) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// vec2f

struct vec2f {
  float x, y;
};

static inline vec2f operator+(const vec2f &a, const vec2f &b) {
  return {a.x + b.x, a.y + b.y};
}
static inline vec2f operator-(const vec2f &a, const vec2f &b) {
  return {a.x - b.x, a.y - b.y};
}
static inline vec2f operator-(const vec2f &a) { return {-a.x, -a.y}; }
static inline vec2f operator*(const vec2f &a, float s) {
  return {a.x * s, a.y * s};
}

static inline float dot(const vec2f &a, const vec2f &b) {
  return a.x * b.x + a.y * b.y;
}

static inline vec2f lerp(const vec2f &a, const vec2f &b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// ---------------------------------------------------------------------------
// vec2i (integer pixel coordinates)

struct vec2i {
  int x, y;
};

static inline vec2i operator+(const vec2i &a, const vec2i &b) {
  return {a.x + b.x, a.y + b.y};
}
static inline vec2i operator-(const vec2i &a, const vec2i &b) {
  return {a.x - b.x, a.y - b.y};
}
static inline vec2i operator-(const vec2i &a) { return {-a.x, -a.y}; }
static inline vec2i operator*(const vec2i &a, int s) {
  return {a.x * s, a.y * s};
}
static inline bool operator==(const vec2i &a, const vec2i &b) {
  return a.x == b.x && a.y == b.y;
}
static inline bool operator!=(const vec2i &a, const vec2i &b) {
  return !(a == b);
}

// ---------------------------------------------------------------------------
// Rect (integer, half-open: [x, x + width) x [y, y + height))

struct Rect {
  int x, y;
  int width, height;

  int right() const { return x + width; }
  int bottom() const { return y + height; }
  bool isEmpty() const { return width <= 0 || height <= 0; }

  bool contains(int px, int py) const {
    return px >= x && px < x + width && py >= y && py < y + height;
  }

  // Make width and height non-negative, keeping the covered area
  Rect normalized() const {
    Rect r = *this;
    if (r.width < 0) {
      r.x += r.width;
      r.width = -r.width;
    }
    if (r.height < 0) {
      r.y += r.height;
      r.height = -r.height;
    }
    return r;
  }

  Rect intersect(const Rect &o) const {
    Rect r;
    r.x = x > o.x ? x : o.x;
    r.y = y > o.y ? y : o.y;
    int rgt = right() < o.right() ? right() : o.right();
    int btm = bottom() < o.bottom() ? bottom() : o.bottom();
    r.width = rgt - r.x;
    r.height = btm - r.y;
    if (r.width < 0) r.width = 0;
    if (r.height < 0) r.height = 0;
    return r;
  }

  Rect offset(int dx, int dy) const { return {x + dx, y + dy, width, height}; }
};

// ---------------------------------------------------------------------------
// colorf (float color; components nominally in 0..1; a = opacity)

struct colorf {
  float r, g, b, a;
};

static inline colorf operator+(const colorf &x, const colorf &y) {
  return {x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a};
}
static inline colorf operator*(const colorf &x, const colorf &y) {
  return {x.r * y.r, x.g * y.g, x.b * y.b, x.a * y.a};
}
static inline colorf operator*(const colorf &x, float s) {
  return {x.r * s, x.g * s, x.b * s, x.a * s};
}

static inline colorf lerp(const colorf &x, const colorf &y, float t) {
  return {
      x.r + (y.r - x.r) * t,
      x.g + (y.g - x.g) * t,
      x.b + (y.b - x.b) * t,
      x.a + (y.a - x.a) * t,
  };
}

}  // namespace shapoco::gfx2d

#endif
