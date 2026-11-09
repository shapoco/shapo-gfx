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
//
// SHAPOGFX_ARCH_WORD_LOADS (ESP8266, see arch_detect.hpp): loadBit() and
// loadPixel16() read the aligned 32-bit word that holds the pixel and
// extract it, instead of loading 8 or 16 bits. This needs a little-endian
// CPU and 16-bit pixels on even addresses, and reads up to three bytes
// before or after the pixel: bytes of the same aligned word, so they are
// in the same page and memory region and the load cannot fault, whatever
// the buffer.

#include <cmath>
#include <cstdint>

#include "../common/arch_detect.hpp"
#include "../common/intmath.hpp"
#include "shapoco/gfx2d/surface.hpp"

#ifndef SHAPOGFX2D_FPU_SQRT
#define SHAPOGFX2D_FPU_SQRT SHAPOGFX_ARCH_FPU
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

#if SHAPOGFX_ARCH_WORD_LOADS && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "SHAPOGFX_ARCH_WORD_LOADS needs a little-endian target"
#endif

namespace shapoco::gfx2d::arch {

#if SHAPOGFX_ARCH_WORD_LOADS
// The aligned 32-bit word holding the byte at `addr`
// (not instrumented by the address sanitizer: the word may reach past the
// object the byte belongs to)
typedef uint32_t __attribute__((may_alias)) AliasedWord;
__attribute__((no_sanitize_address)) static inline uint32_t loadWordAt(
    uintptr_t addr) {
  return *(const AliasedWord *)(addr & ~(uintptr_t)3);
}
#endif

// Bit i of `bits`, MSB first (a GRAY1 pixel, a bit of a mask)
static inline uint32_t loadBit(const uint8_t *bits, uint32_t i) {
#if SHAPOGFX_ARCH_WORD_LOADS
  const uintptr_t a = (uintptr_t)bits + (i >> 3);
  return (loadWordAt(a) >> ((a & 3u) * 8u + (7u - (i & 7u)))) & 1u;
#else
  return (bits[i >> 3] >> (7u - (i & 7u))) & 1u;
#endif
}

// Pixel x of a row of 16-bit pixels, as stored
static inline uint32_t loadPixel16(const uint8_t *line, int x) {
#if SHAPOGFX_ARCH_WORD_LOADS
  // (the pixel is on an even address, so it lies within one word)
  const uintptr_t a = (uintptr_t)line + (uintptr_t)x * 2u;
  return (loadWordAt(a) >> ((a & 2u) * 8u)) & 0xFFFFu;
#else
  return ((const uint16_t *)line)[x];
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
// (ADD_RAW). u and v stay within the image (the caller clips every row).
// The lanes' SHIFT is a right rotation, not a shift: the fraction bits come
// back at the top, so each mask ends where the integer part does (bit 16 of
// u >> 15, bit 15 + log2s of v >> (16 - log2s)).
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
    interp_config_set_mask(&c, 1, 16);
    interp_set_config(interp0, 0, &c);
    interp_config_set_shift(&c, 16 - log2s);  // row index x stride
    interp_config_set_mask(&c, log2s, 15 + log2s);
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
