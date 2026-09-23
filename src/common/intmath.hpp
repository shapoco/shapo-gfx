#ifndef SHAPOGFX_COMMON_INTMATH_HPP
#define SHAPOGFX_COMMON_INTMATH_HPP

// Integer helpers shared by the 2D and the 3D renderer (internal; included by
// src/*.cpp only).

#include <cstdint>

namespace shapoco::gfx::intmath {

// floor(sqrt(v)), bit by bit: no division and no library call, 16 steps
static inline uint32_t isqrt32(uint32_t v) {
  uint32_t r = 0, b = 1u << 30;
  while (b > v) b >>= 2;
  while (b != 0) {
    if (v >= r + b) {
      v -= r + b;
      r = (r >> 1) + b;
    } else {
      r >>= 1;
    }
    b >>= 2;
  }
  return r;
}

}  // namespace shapoco::gfx::intmath

#endif
