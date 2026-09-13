#ifndef SHAPOGFX2D_MATH2D_HPP
#define SHAPOGFX2D_MATH2D_HPP

namespace shapoco::gfx2d {

// ---------------------------------------------------------------------------
// Scalar helpers

static inline float clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
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
// colorf (components nominally in 0..1; a = opacity)

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
