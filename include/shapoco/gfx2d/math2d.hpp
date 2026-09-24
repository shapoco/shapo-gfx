#ifndef SHAPOGFX2D_MATH2D_HPP
#define SHAPOGFX2D_MATH2D_HPP

#include <cmath>

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
// affine2f: 2D affine transform
//
//   x' = a * x + c * y + tx
//   y' = b * x + d * y + ty
//
// Coordinates are continuous: pixel (x, y) covers [x, x + 1) x [y, y + 1), so
// translation(10, 20) puts the top-left corner of an image at the top-left
// corner of pixel (10, 20). Angles are radians; with y pointing down a
// positive angle turns clockwise on screen.
//
// The member functions translate(), scale(), rotate() and shear() multiply on
// the right, like a canvas context: the transform given last applies to the
// image first.
//
//   // image centered on (x, y), rotated and scaled about its center
//   affine2f m = affine2f::translation(x, y);
//   m.rotate(angle).scale(2.0f).translate(-w * 0.5f, -h * 0.5f);
//   // the same
//   affine2f m2 = affine2f::placement(x, y, angle, 2, 2, w * 0.5f, h * 0.5f);

struct affine2f {
  float a, b, c, d, tx, ty;

  static affine2f identity() { return {1, 0, 0, 1, 0, 0}; }
  static affine2f translation(float x, float y) { return {1, 0, 0, 1, x, y}; }
  static affine2f scaling(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
  static affine2f scaling(float s) { return scaling(s, s); }
  static affine2f rotation(float angle) {
    const float cs = std::cos(angle), sn = std::sin(angle);
    return {cs, sn, -sn, cs, 0, 0};
  }
  // Rotation about the point (cx, cy)
  static affine2f rotation(float angle, float cx, float cy) {
    affine2f m = rotation(angle);
    m.tx = cx - m.a * cx - m.c * cy;
    m.ty = cy - m.b * cx - m.d * cy;
    return m;
  }
  // x' = x + kx * y, y' = ky * x + y
  static affine2f shearing(float kx, float ky) { return {1, ky, kx, 1, 0, 0}; }
  // The point (pivotX, pivotY) of an image to (x, y), scaled by (sx, sy)
  // and then rotated about it
  static affine2f placement(float x, float y, float angle, float sx = 1.0f,
                            float sy = 1.0f, float pivotX = 0.0f,
                            float pivotY = 0.0f) {
    const float cs = std::cos(angle), sn = std::sin(angle);
    affine2f m = {cs * sx, sn * sx, -sn * sy, cs * sy, 0, 0};
    m.tx = x - m.a * pivotX - m.c * pivotY;
    m.ty = y - m.b * pivotX - m.d * pivotY;
    return m;
  }

  float determinant() const { return a * d - b * c; }

  vec2f apply(float x, float y) const {
    return {a * x + c * y + tx, b * x + d * y + ty};
  }
  vec2f apply(const vec2f &p) const { return apply(p.x, p.y); }
  // Without the translation (for directions)
  vec2f applyLinear(const vec2f &v) const {
    return {a * v.x + c * v.y, b * v.x + d * v.y};
  }

  // The inverse transform; false (and `out` unchanged) if not invertible
  bool invert(affine2f &out) const {
    const float det = determinant();
    if (!(det != 0.0f) || !std::isfinite(det)) return false;
    const float r = 1.0f / det;
    const affine2f m = {d * r, -b * r, -c * r, a * r, 0, 0};
    out = m;
    out.tx = -(m.a * tx + m.c * ty);
    out.ty = -(m.b * tx + m.d * ty);
    return true;
  }

  // this = this * m (m applies first)
  affine2f &multiply(const affine2f &m) {
    *this = {a * m.a + c * m.b,        b * m.a + d * m.b,
             a * m.c + c * m.d,        b * m.c + d * m.d,
             a * m.tx + c * m.ty + tx, b * m.tx + d * m.ty + ty};
    return *this;
  }
  affine2f &translate(float x, float y) { return multiply(translation(x, y)); }
  affine2f &scale(float sx, float sy) { return multiply(scaling(sx, sy)); }
  affine2f &scale(float s) { return multiply(scaling(s)); }
  affine2f &rotate(float angle) { return multiply(rotation(angle)); }
  affine2f &shear(float kx, float ky) { return multiply(shearing(kx, ky)); }
};

// m * n: n applies first, then m
static inline affine2f operator*(const affine2f &m, const affine2f &n) {
  affine2f r = m;
  return r.multiply(n);
}
static inline vec2f operator*(const affine2f &m, const vec2f &p) {
  return m.apply(p);
}

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
