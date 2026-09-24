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
// accumulators. Only for 16-bit texels (ARGB4444 when ARGB, else RGB565_SWAPPED
// when SWAP or native RGB565) with a power-of-two stride of at most 2^16
// bytes and at least 2 texels each way, which the record of the primitive
// states with its log2s (the logarithms of the sizes come from the record as
// well; see PartTex in gfx3d.cpp); returns the texel as native RGB565 and its
// 4-bit alpha, like the portable walker.
template <bool ARGB, bool SWAP>
struct InterpTex {
  static constexpr int FIX_SHIFT = 16;
  void init(const void *pixels, int log2w, int log2h, int log2s, int32_t u0,
            int32_t v0, int32_t du0, int32_t dv0) {
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
    interp0->base[2] = (uintptr_t)pixels;
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
    } else if constexpr (SWAP) {
      a4 = 15;
      return gfx2d::bswap16((uint16_t)p);
    } else {
      a4 = 15;
      return p;
    }
  }
};

// GouraudRG (see generic.hpp) through interp1: lane 0 steps r and lane 1
// steps g (ADD_RAW adds the step to the accumulator on every pop), and the
// shifted and masked lanes sum to (r5 << 11) | (g6 << 5) in the FULL result,
// so a pixel's red and green are one load.
struct GouraudRG {
  void init(int32_t r0, int32_t g0, int32_t dr0, int32_t dg0) {
    interp_config c = interp_default_config();
    interp_config_set_add_raw(&c, true);
    interp_config_set_shift(&c, 8);  // bits 19..23 of r -> 11..15
    interp_config_set_mask(&c, 11, 15);
    interp_set_config(interp1, 0, &c);
    interp_config_set_shift(&c, 13);  // bits 18..23 of g -> 5..10
    interp_config_set_mask(&c, 5, 10);
    interp_set_config(interp1, 1, &c);
    interp1->base[0] = (uint32_t)dr0;
    interp1->base[1] = (uint32_t)dg0;
    interp1->base[2] = 0;
    interp1->accum[0] = (uint32_t)r0;
    interp1->accum[1] = (uint32_t)g0;
  }
  inline uint32_t next() { return (uint32_t)interp1->pop[2]; }
};

// render() saves and restores both interpolators of the calling core
struct RenderState {
  interp_hw_save_t save0, save1;
  void begin() {
    interp_save(interp0, &save0);
    interp_save(interp1, &save1);
  }
  void end() {
    interp_restore(interp0, &save0);
    interp_restore(interp1, &save1);
  }
};

}  // namespace shapoco::gfx3d::arch::rp2

#endif  // SHAPOGFX3D_RP2_INTERP

#endif
