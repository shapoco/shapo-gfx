#ifndef SHAPOGFX3D_ARCH_GENERIC_HPP
#define SHAPOGFX3D_ARCH_GENERIC_HPP

// Portable implementations of the architecture hooks (see arch.hpp). An
// architecture header may replace a hook by defining its SHAPOGFX3D_ARCH_HAS_*
// macro before this point; everything not replaced falls back to these.

#include <cstdint>

namespace shapoco::gfx3d::arch::generic {

// a * b as 64 bits
static inline int64_t mul64(int32_t a, int32_t b) { return (int64_t)a * b; }

// The same for b < 2^16 and 0 <= sh <= 16
static inline int32_t mulShiftU16(int32_t a, uint32_t b, int sh) {
  return (int32_t)(((int64_t)a * (int64_t)b) >> sh);
}

// Red and green of an untextured, smoothly shaded RGB565 span: r and g are
// 8.16 (0..255) stepped by dr and dg per pixel, next() returns
// (r5 << 11) | (g6 << 5) of the current pixel and steps to the next one
struct GouraudRG {
  int32_t r, g, dr, dg;
  void init(int32_t r0, int32_t g0, int32_t dr0, int32_t dg0) {
    r = r0, g = g0, dr = dr0, dg = dg0;
  }
  inline uint32_t next() {
    const uint32_t v =
        (((uint32_t)r >> 8) & 0xF800u) | (((uint32_t)g >> 13) & 0x07E0u);
    r += dr;
    g += dg;
    return v;
  }
};

// Hardware state render() has to preserve for the caller
struct RenderState {
  void begin() {}
  void end() {}
};

}  // namespace shapoco::gfx3d::arch::generic

#endif
