#ifndef SHAPOGFX2D_PIXEL_HPP
#define SHAPOGFX2D_PIXEL_HPP

#include <cstdint>

#include "shapoco/gfx2d/math2d.hpp"

// RGB565 pixel helpers shared by the 2D and 3D renderers.
// All functions are inline and branch-free where possible so that they can be
// used inside per-pixel loops.

namespace shapoco::gfx2d {

enum class BlendMode : uint8_t {
  NONE,   // no blending (overwrite)
  ALPHA,  // alpha blending
  ADD,    // additive blending
};

// Assemble an RGB565 pixel from 5/6/5-bit components (no range checking).
static inline uint16_t makeRgb565(uint32_t r5, uint32_t g6, uint32_t b5) {
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// Convert a float color (0..1, clamped) to RGB565 with rounding.
static inline uint16_t packRgb565(float r, float g, float b) {
  uint32_t ri = (uint32_t)(clamp01(r) * 31.0f + 0.5f);
  uint32_t gi = (uint32_t)(clamp01(g) * 63.0f + 0.5f);
  uint32_t bi = (uint32_t)(clamp01(b) * 31.0f + 0.5f);
  return makeRgb565(ri, gi, bi);
}

static inline uint16_t packRgb565(const colorf &c) {
  return packRgb565(c.r, c.g, c.b);
}

// Fill n pixels with color (writes 32 bits at a time where possible).
static inline void fillRgb565(uint16_t *dst, int n, uint16_t color) {
  if (n <= 0) return;
  if ((uintptr_t)dst & 2u) {
    *dst++ = color;
    n--;
  }
  uint32_t *d32 = (uint32_t *)dst;
  uint32_t c32 = ((uint32_t)color << 16) | color;
  for (int i = 0; i < (n >> 1); i++) d32[i] = c32;
  if (n & 1) dst[n - 1] = color;
}

// Alpha-blend src over dst. alpha64 is the opacity of src in 0..64 (64 = fully
// opaque). The R+B fields and the G field are interpolated separately in one
// multiply each; the products never carry into the neighboring field.
static inline uint16_t blendAlphaRgb565(uint16_t dst, uint16_t src,
                                        uint32_t alpha64) {
  uint32_t ia = 64u - alpha64;
  uint32_t rb =
      (((dst & 0xF81Fu) * ia + (src & 0xF81Fu) * alpha64) >> 6) & 0xF81Fu;
  uint32_t g =
      (((dst & 0x07E0u) * ia + (src & 0x07E0u) * alpha64) >> 6) & 0x07E0u;
  return (uint16_t)(rb | g);
}

// Add 5/6/5-bit components to dst with per-channel saturation.
static inline uint16_t addSaturateRgb565(uint16_t dst, uint32_t r5, uint32_t g6,
                                         uint32_t b5) {
  uint32_t r = (dst >> 11) + r5;
  uint32_t g = ((dst >> 5) & 63u) + g6;
  uint32_t b = (dst & 31u) + b5;
  if (r > 31u) r = 31u;
  if (g > 63u) g = 63u;
  if (b > 31u) b = 31u;
  return makeRgb565(r, g, b);
}

static inline uint16_t addSaturateRgb565(uint16_t dst, uint16_t src) {
  return addSaturateRgb565(dst, src >> 11, (src >> 5) & 63u, src & 31u);
}

// floor(log2(v)) for v >= 1 (returns 0 for v <= 1).
static inline int log2Floor(int v) {
  int n = 0;
  while (v > 1) {
    v >>= 1;
    n++;
  }
  return n;
}

}  // namespace shapoco::gfx2d

#endif
