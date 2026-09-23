#ifndef SHAPOGFX3D_ARCH_RP2_HPP
#define SHAPOGFX3D_ARCH_RP2_HPP

// RP2040 / RP2350 (Pico SDK) hooks: the SIO interpolators. See arch.hpp.

#include <cstdint>

#include "shapoco/gfx2d/pixel.hpp"

#if SHAPOGFX3D_RP2_INTERP
#include "hardware/interp.h"

namespace shapoco::gfx3d::arch::rp2 {

// Texture coordinate walker through interp0: lane 0 turns u (16.16 texels)
// into the byte offset of the texel in its row, lane 1 turns v into the byte
// offset of the row, and POP_FULL returns the texel address and steps both
// accumulators. Only for 16-bit texels (RGB565BE, or ARGB4444 when ARGB) with
// a power-of-two stride; returns the texel as native RGB565 and its 4-bit
// alpha, like the portable walker.
template <bool ARGB>
struct InterpTex {
  static constexpr int FIX_SHIFT = 16;
  static bool usable(const gfx2d::Texture &tex) {
    const uint32_t s = tex.stride;
    return tex.width >= 2 && tex.height >= 2 && s >= 2 && (s & (s - 1)) == 0 &&
           gfx2d::log2Floor((int)s) <= FIX_SHIFT;
  }
  void init(const gfx2d::Texture &tex, int32_t u0, int32_t v0, int32_t du0,
            int32_t dv0) {
    const int log2w = gfx2d::log2Floor(tex.width);
    const int log2h = gfx2d::log2Floor(tex.height);
    const int log2s = gfx2d::log2Floor((int)tex.stride);
    interp_config c = interp_default_config();
    interp_config_set_add_raw(&c, true);
    interp_config_set_shift(&c, FIX_SHIFT - 1);  // texel index x 2 bytes
    interp_config_set_mask(&c, 1, log2w);
    interp_set_config(interp0, 0, &c);
    interp_config_set_shift(&c, FIX_SHIFT - log2s);  // row index x stride
    interp_config_set_mask(&c, log2s, log2s + log2h - 1);
    interp_set_config(interp0, 1, &c);
    interp0->base[0] = (uint32_t)du0;
    interp0->base[1] = (uint32_t)dv0;
    interp0->base[2] = (uintptr_t)tex.pixels;
    interp0->accum[0] = (uint32_t)u0;
    interp0->accum[1] = (uint32_t)v0;
  }
  void setStep(int32_t du0, int32_t dv0) {
    interp0->base[0] = (uint32_t)du0;
    interp0->base[1] = (uint32_t)dv0;
  }
  inline uint32_t fetchNext(uint32_t &a4) {
    const uint32_t p = *(const uint16_t *)(uintptr_t)(interp0->pop[2]);
    if constexpr (ARGB) {
      a4 = p >> 12;
      return gfx2d::rgb444ToRgb565((uint16_t)(p & 0x0FFFu));
    } else {
      a4 = 15;
      return gfx2d::bswap16((uint16_t)p);
    }
  }
};

// render() saves and restores interp0 of the calling core
struct RenderState {
  interp_hw_save_t save;
  void begin() { interp_save(interp0, &save); }
  void end() { interp_restore(interp0, &save); }
};

}  // namespace shapoco::gfx3d::arch::rp2

#endif  // SHAPOGFX3D_RP2_INTERP

#endif
