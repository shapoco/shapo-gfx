#ifndef SHAPOGFX2D_ARCH_HPP
#define SHAPOGFX2D_ARCH_HPP

// Architecture layer of the 2D renderer (internal; included by src/gfx2d/*.cpp
// only). The portable code is the reference; a hook here computes the same
// pixels faster.
//
// SHAPOGFX2D_RP2_INTERP (RP2040 / RP2350): the transformed drawImage() of a
// 16-bit image whose stride is a power of two fetches the pixels through the
// SIO interpolator interp0 of the calling core, which it saves and restores.
// On by default where the Pico SDK's hardware_interp is available; define it
// as 0 to use the portable code.
//
// SHAPOGFX2D_FPU_SQRT: the integer square root of the ellipse extents is
// seeded by the FPU's sqrtf() and corrected to the exact floor (the same
// value as the pure integer root, which a core without an FPU keeps). On by
// default where the compiler reports hardware float; define it as 0 or 1 to
// choose.

#include <cmath>
#include <cstdint>

#include "../common/arch_detect.hpp"
#include "../common/intmath.hpp"
#include "shapoco/gfx2d/surface.hpp"

// (__ARM_FP is defined only with a hardware FPU in use, unlike __VFP_FP__,
// which names the float format and is defined on a Cortex-M0+ as well)
#ifndef SHAPOGFX2D_FPU_SQRT
#if defined(__ARM_FP) || defined(__riscv_flen) ||                          \
    defined(SHAPOGFX_ARCH_ESP32S3) || defined(SHAPOGFX_ARCH_ESP32P4) ||    \
    defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) ||    \
    defined(__wasm__)
#define SHAPOGFX2D_FPU_SQRT 1
#else
#define SHAPOGFX2D_FPU_SQRT 0
#endif
#endif

namespace shapoco::gfx2d::arch {

// floor(sqrt(m)), equal to gfx::intmath::isqrt32(m) for every m
static inline uint32_t isqrt32(uint32_t m) {
#if SHAPOGFX2D_FPU_SQRT
  // The float root is within one of the answer ((float)m is off by less
  // than 2^-23 m, so its root by less than 2^-24 sqrt(m) + rounding); the
  // loops settle it. s <= 65535 keeps every product in 32 bits.
  uint32_t s = (uint32_t)std::sqrt((float)m);
  if (s > 65535u) s = 65535u;
  while (s * s > m) s--;
  while (s < 65535u && (s + 1u) * (s + 1u) <= m) s++;
  return s;
#else
  return gfx::intmath::isqrt32(m);
#endif
}

}  // namespace shapoco::gfx2d::arch

#ifndef SHAPOGFX2D_RP2_INTERP
#if defined(SHAPOGFX_ARCH_RP2) && __has_include("hardware/interp.h")
#define SHAPOGFX2D_RP2_INTERP 1
#else
#define SHAPOGFX2D_RP2_INTERP 0
#endif
#endif

#if SHAPOGFX2D_RP2_INTERP
#include "hardware/interp.h"

namespace shapoco::gfx2d::arch::rp2 {

// Affine texel walk through interp0: lane 0 turns u (16.16 texels) into the
// byte offset of the texel in its row, lane 1 turns v into the byte offset
// of the row, and POP_FULL returns the address of the texel and steps both
// (ADD_RAW). u and v stay within the image (the caller clips every row), so
// the masks only drop the fraction bits.
struct InterpAffine {
  static bool usable(const Texture &img) {
    const uint32_t s = img.stride;
    return bitsPerPixel(img.format) == 16 && s >= 2 && (s & (s - 1)) == 0 &&
           log2Floor((int)s) <= 16;
  }
  interp_hw_save_t saved;
  void begin(const Texture &img, int32_t du, int32_t dv) {
    interp_save(interp0, &saved);
    const uint32_t log2s = (uint32_t)log2Floor((int)img.stride);
    interp_config c = interp_default_config();
    interp_config_set_add_raw(&c, true);
    interp_config_set_shift(&c, 15);  // texel index x 2 bytes
    interp_config_set_mask(&c, 1, 31);
    interp_set_config(interp0, 0, &c);
    interp_config_set_shift(&c, 16 - log2s);  // row index x stride
    interp_config_set_mask(&c, log2s, 31);
    interp_set_config(interp0, 1, &c);
    interp0->base[0] = (uint32_t)du;
    interp0->base[1] = (uint32_t)dv;
    interp0->base[2] = (uintptr_t)img.pixels;
  }
  void row(int32_t u, int32_t v) {
    interp0->accum[0] = (uint32_t)u;
    interp0->accum[1] = (uint32_t)v;
  }
  static inline uint32_t fetch() {
    return *(const uint16_t *)(uintptr_t)(interp0->pop[2]);
  }
  void end() { interp_restore(interp0, &saved); }
};

}  // namespace shapoco::gfx2d::arch::rp2

#endif  // SHAPOGFX2D_RP2_INTERP

#endif
