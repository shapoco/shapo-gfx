#ifndef SHAPOGFX3D_ARCH_ARMV6M_HPP
#define SHAPOGFX3D_ARCH_ARMV6M_HPP

// ARMv6-M (Cortex-M0 / M0+, e.g. RP2040) hooks. See arch.hpp. The core has a
// 32 x 32 -> 32-bit multiplier only, so a 64-bit product is a library call
// (__aeabi_lmul) of a few dozen cycles.

#include <cstdint>

namespace shapoco::gfx3d::arch::armv6m {

// (a * b) >> sh for b < 2^16 and 0 <= sh <= 16, the result fitting 32 bits,
// from two 16 x 16-bit products: with a = ah * 2^16 + al (al unsigned),
// (a * b) >> sh = ((ah * b) << (16 - sh)) + ((al * b) >> sh) exactly, and
// both products fit 32 bits.
static inline int32_t mulShiftU16(int32_t a, uint32_t b, int sh) {
  const int32_t hi = (a >> 16) * (int32_t)b;
  const uint32_t lo = ((uint32_t)a & 0xFFFFu) * b;
  return (int32_t)(((uint32_t)hi << (16 - sh)) + (lo >> sh));
}

}  // namespace shapoco::gfx3d::arch::armv6m

#endif
