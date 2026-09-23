#ifndef SHAPOGFX3D_ARCH_SPLIT_MUL_HPP
#define SHAPOGFX3D_ARCH_SPLIT_MUL_HPP

// Products for cores whose multiplier yields only the low 32 bits of a
// product (SHAPOGFX_ARCH_SPLIT_MUL64, see arch.hpp): Cortex-M0/M0+ (RP2040),
// Xtensa lx106 (ESP8266). There a 64-bit product is a library call of a few
// dozen cycles (__aeabi_lmul, __muldi3); these build it from 16 x 16-bit
// products inline, with the same results as the generic versions.

#include <cstdint>

namespace shapoco::gfx3d::arch::split {

// a * b as 64 bits: with a = ah * 2^16 + al and b = bh * 2^16 + bl (al, bl
// unsigned), a * b = ah * bh * 2^32 + (ah * bl + al * bh) * 2^16 + al * bl,
// each product fitting 32 bits
static inline int64_t mul64(int32_t a, int32_t b) {
  const int32_t ah = a >> 16, bh = b >> 16;
  const uint32_t al = (uint32_t)a & 0xFFFFu, bl = (uint32_t)b & 0xFFFFu;
  const int32_t hh = ah * bh;
  const int32_t m1 = ah * (int32_t)bl, m2 = (int32_t)al * bh;
  const uint64_t r = ((uint64_t)(uint32_t)hh << 32) +
                     ((uint64_t)((int64_t)m1 + m2) << 16) + (uint64_t)(al * bl);
  return (int64_t)r;
}

// (a * b) >> sh for b < 2^16 and 0 <= sh <= 16, the result fitting 32 bits,
// from two 16 x 16-bit products: with a = ah * 2^16 + al (al unsigned),
// (a * b) >> sh = ((ah * b) << (16 - sh)) + ((al * b) >> sh) exactly
static inline int32_t mulShiftU16(int32_t a, uint32_t b, int sh) {
  const int32_t hi = (a >> 16) * (int32_t)b;
  const uint32_t lo = ((uint32_t)a & 0xFFFFu) * b;
  return (int32_t)(((uint32_t)hi << (16 - sh)) + (lo >> sh));
}

}  // namespace shapoco::gfx3d::arch::split

#endif
