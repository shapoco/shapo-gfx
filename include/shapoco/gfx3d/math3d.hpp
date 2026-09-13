#ifndef SHAPOGFX3D_MATH3D_HPP
#define SHAPOGFX3D_MATH3D_HPP

#include <cmath>
#include <cstdint>

#include "shapoco/gfx2d/math2d.hpp"

namespace shapoco::gfx3d {

// 2D types are shared with shapoco::gfx2d and re-exported here for convenience.
using gfx2d::clamp01;
using gfx2d::colorf;
using gfx2d::lerp;
using gfx2d::vec2f;

// ---------------------------------------------------------------------------
// vec3f

struct vec3f {
  float x, y, z;
};

static inline vec3f operator+(const vec3f &a, const vec3f &b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
static inline vec3f operator-(const vec3f &a, const vec3f &b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
static inline vec3f operator-(const vec3f &a) { return {-a.x, -a.y, -a.z}; }
static inline vec3f operator*(const vec3f &a, float s) {
  return {a.x * s, a.y * s, a.z * s};
}

static inline float dot(const vec3f &a, const vec3f &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3f cross(const vec3f &a, const vec3f &b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static inline float length(const vec3f &a) { return std::sqrt(dot(a, a)); }

static inline vec3f normalize(const vec3f &a) {
  float len = length(a);
  if (len <= 0.0f) return {0, 0, 0};
  return a * (1.0f / len);
}

static inline vec3f lerp(const vec3f &a, const vec3f &b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// ---------------------------------------------------------------------------
// mat4f (column-major, OpenGL compatible)

struct mat4f {
  float m[16];  // m[col * 4 + row]

  static mat4f identity() {
    mat4f r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }

  static mat4f translation(float x, float y, float z) {
    mat4f r = identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
  }

  // angle: radians. axis need not be normalized (normalized internally).
  static mat4f rotation(float angle, const vec3f &axis) {
    vec3f n = normalize(axis);
    float c = std::cos(angle), s = std::sin(angle), ic = 1.0f - c;
    mat4f r = identity();
    r.m[0] = c + n.x * n.x * ic;
    r.m[1] = n.y * n.x * ic + n.z * s;
    r.m[2] = n.z * n.x * ic - n.y * s;
    r.m[4] = n.x * n.y * ic - n.z * s;
    r.m[5] = c + n.y * n.y * ic;
    r.m[6] = n.z * n.y * ic + n.x * s;
    r.m[8] = n.x * n.z * ic + n.y * s;
    r.m[9] = n.y * n.z * ic - n.x * s;
    r.m[10] = c + n.z * n.z * ic;
    return r;
  }

  static mat4f scaling(float x, float y, float z) {
    mat4f r = identity();
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    return r;
  }

  // fovY: radians
  static mat4f perspective(float fovY, float aspect, float zNear, float zFar) {
    float f = 1.0f / std::tan(fovY * 0.5f);
    mat4f r = {};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zFar + zNear) / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * zFar * zNear / (zNear - zFar);
    return r;
  }

  static mat4f orthographic(float left, float right, float bottom, float top,
                            float zNear, float zFar) {
    mat4f r = identity();
    r.m[0] = 2.0f / (right - left);
    r.m[5] = 2.0f / (top - bottom);
    r.m[10] = -2.0f / (zFar - zNear);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -(zFar + zNear) / (zFar - zNear);
    return r;
  }

  // View matrix looking from eye towards target (like gluLookAt)
  static mat4f lookAt(const vec3f &eye, const vec3f &target, const vec3f &up) {
    vec3f f = normalize(target - eye);
    vec3f s = normalize(cross(f, up));
    vec3f u = cross(s, f);
    mat4f r = identity();
    r.m[0] = s.x, r.m[4] = s.y, r.m[8] = s.z;
    r.m[1] = u.x, r.m[5] = u.y, r.m[9] = u.z;
    r.m[2] = -f.x, r.m[6] = -f.y, r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
  }

  // Rotation from a unit quaternion (x, y, z, w)
  static mat4f fromQuaternion(float x, float y, float z, float w) {
    mat4f r = identity();
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + z * w);
    r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);
    r.m[9] = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
  }

  mat4f operator*(const mat4f &o) const {
    mat4f r;
    for (int col = 0; col < 4; col++) {
      for (int row = 0; row < 4; row++) {
        float sum = 0.0f;
        for (int k = 0; k < 4; k++) {
          sum += m[k * 4 + row] * o.m[col * 4 + k];
        }
        r.m[col * 4 + row] = sum;
      }
    }
    return r;
  }

  // Transform a point (w = 1 assumed, no perspective divide).
  vec3f transformPoint(const vec3f &v) const {
    return {
        m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12],
        m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13],
        m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14],
    };
  }

  // Transform a point and also return the resulting w.
  vec3f transformPoint4(const vec3f &v, float &wOut) const {
    wOut = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
    return transformPoint(v);
  }

  // Transform a direction vector (translation ignored).
  vec3f transformDir(const vec3f &v) const {
    return {
        m[0] * v.x + m[4] * v.y + m[8] * v.z,
        m[1] * v.x + m[5] * v.y + m[9] * v.z,
        m[2] * v.x + m[6] * v.y + m[10] * v.z,
    };
  }
};

}  // namespace shapoco::gfx3d

#endif
