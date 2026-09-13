#ifndef SHAPOGFX2D_PIXEL_HPP
#define SHAPOGFX2D_PIXEL_HPP

#include <cstdint>
#include <cstring>

#include "shapoco/gfx2d/config.hpp"
#include "shapoco/gfx2d/math2d.hpp"

// Pixel formats, colors and per-format pixel access shared by the 2D and 3D
// renderers. Everything here is inline so that it can be used inside per-pixel
// loops.
//
// "Native pixel" values used by the helpers below are the unpacked in-register
// representation of one pixel:
//   GRAY1     0 or 1
//   RGB444    0x0RGB (4 bits per channel)
//   ARGB4444  0xARGB (4 bits per channel)
//   RGB565BE  RRRRRGGGGGGBBBBB (5/6/5 bits; byte-swapped only in memory)

namespace shapoco::gfx2d {

enum class PixelFormat : uint8_t {
  GRAY1,     // 1 bit per pixel, MSB first, 1 = white
  RGB444,    // 12 bits per pixel, 2 pixels in 3 bytes: R1G1 B1R2 G2B2 (display
             // order)
  ARGB4444,  // 16 bits per pixel, native uint16_t 0xARGB; A = 15 is opaque
  RGB565BE,  // 16 bits per pixel, big-endian byte order in memory (display
             // order)
};

enum class BlendMode : uint8_t {
  NONE,   // no blending (overwrite)
  ALPHA,  // alpha blending
  ADD,    // additive blending
};

constexpr int bitsPerPixel(PixelFormat f) {
  switch (f) {
    case PixelFormat::GRAY1: return 1;
    case PixelFormat::RGB444: return 12;
    default: return 16;
  }
}

// Smallest row pitch (bytes) that holds `width` pixels
constexpr uint32_t minStride(PixelFormat f, int width) {
  return (uint32_t)((width * bitsPerPixel(f) + 7) / 8);
}

// ---------------------------------------------------------------------------
// Color: ARGB8888. A = 255 is opaque. This is the format-independent color type
// used by the 2D API; it is converted to the target format once per call.

using Color = uint32_t;

namespace Colors {
constexpr Color TRANSPARENT = 0x00000000;
constexpr Color BLACK = 0xFF000000;
constexpr Color GRAY = 0xFF808080;
constexpr Color SILVER = 0xFFC0C0C0;
constexpr Color WHITE = 0xFFFFFFFF;
constexpr Color RED = 0xFFFF0000;
constexpr Color GREEN = 0xFF00FF00;
constexpr Color BLUE = 0xFF0000FF;
constexpr Color YELLOW = 0xFFFFFF00;
constexpr Color CYAN = 0xFF00FFFF;
constexpr Color MAGENTA = 0xFFFF00FF;
}  // namespace Colors

constexpr int colorA(Color c) { return (int)(c >> 24) & 0xFF; }
constexpr int colorR(Color c) { return (int)(c >> 16) & 0xFF; }
constexpr int colorG(Color c) { return (int)(c >> 8) & 0xFF; }
constexpr int colorB(Color c) { return (int)c & 0xFF; }

constexpr Color makeColor(int r, int g, int b, int a = 255) {
  return ((uint32_t)clampInt(0, 255, a) << 24) |
         ((uint32_t)clampInt(0, 255, r) << 16) |
         ((uint32_t)clampInt(0, 255, g) << 8) | (uint32_t)clampInt(0, 255, b);
}

constexpr Color colorWithAlpha(Color c, int a) {
  return (c & 0x00FFFFFFu) | ((uint32_t)clampInt(0, 255, a) << 24);
}

static inline Color makeColorF(float r, float g, float b, float a = 1.0f) {
  return makeColor(
      (int)(clamp01(r) * 255.0f + 0.5f), (int)(clamp01(g) * 255.0f + 0.5f),
      (int)(clamp01(b) * 255.0f + 0.5f), (int)(clamp01(a) * 255.0f + 0.5f));
}

static inline Color makeColorF(const colorf &c) {
  return makeColorF(c.r, c.g, c.b, c.a);
}

// h: degrees (any value, wrapped), s and v: 0..255
static inline Color makeColorHsv(int h, int s, int v, int a = 255) {
  h = ((h % 360) + 360) % 360;
  s = clampInt(0, 255, s);
  v = clampInt(0, 255, v);
  int r, g, b;
  if (s == 0) {
    r = g = b = v;
  } else {
    int coarse = h / 60;
    int fine = (h % 60) * 256 / 60;
    int p = (v * (255 - s)) / 255;
    int q = (v * (255 - (s * fine) / 256)) / 255;
    int t = (v * (255 - (s * (255 - fine)) / 256)) / 255;
    switch (coarse) {
      case 0: r = v, g = t, b = p; break;
      case 1: r = q, g = v, b = p; break;
      case 2: r = p, g = v, b = t; break;
      case 3: r = p, g = q, b = v; break;
      case 4: r = t, g = p, b = v; break;
      default: r = v, g = p, b = q; break;
    }
  }
  return makeColor(r, g, b, a);
}

// Interpolate two colors; t in 0..256 (256 = fully b)
static inline Color lerpColor(Color a, Color b, int t) {
  int it = 256 - t;
  return makeColor((colorR(a) * it + colorR(b) * t) >> 8,
                   (colorG(a) * it + colorG(b) * t) >> 8,
                   (colorB(a) * it + colorB(b) * t) >> 8,
                   (colorA(a) * it + colorA(b) * t) >> 8);
}

static inline uint16_t bswap16(uint16_t v) {
  return (uint16_t)((v << 8) | (v >> 8));
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

// ---------------------------------------------------------------------------
// RGB565 (native 5/6/5 in a uint16_t)

static inline uint16_t makeRgb565(uint32_t r5, uint32_t g6, uint32_t b5) {
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// Float color (0..1, clamped) to RGB565 with rounding
static inline uint16_t packRgb565(float r, float g, float b) {
  uint32_t ri = (uint32_t)(clamp01(r) * 31.0f + 0.5f);
  uint32_t gi = (uint32_t)(clamp01(g) * 63.0f + 0.5f);
  uint32_t bi = (uint32_t)(clamp01(b) * 31.0f + 0.5f);
  return makeRgb565(ri, gi, bi);
}
static inline uint16_t packRgb565(const colorf &c) {
  return packRgb565(c.r, c.g, c.b);
}

// Same, but in the byte order stored in memory for PixelFormat::RGB565BE
static inline uint16_t packRgb565BE(float r, float g, float b) {
  return bswap16(packRgb565(r, g, b));
}
static inline uint16_t packRgb565BE(const colorf &c) {
  return bswap16(packRgb565(c));
}

constexpr uint16_t colorToRgb565(Color c) {
  return (uint16_t)(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) |
                    ((c >> 3) & 0x001Fu));
}

constexpr Color rgb565ToColor(uint16_t p) {
  uint32_t r5 = p >> 11, g6 = (p >> 5) & 63u, b5 = p & 31u;
  return 0xFF000000u | (((r5 << 3) | (r5 >> 2)) << 16) |
         (((g6 << 2) | (g6 >> 4)) << 8) | ((b5 << 3) | (b5 >> 2));
}

// Fill n uint16_t pixels (writes 32 bits at a time where possible)
static inline void fill16(uint16_t *dst, int n, uint16_t v) {
  if (n <= 0) return;
  if ((uintptr_t)dst & 2u) {
    *dst++ = v;
    n--;
  }
  uint32_t *d32 = (uint32_t *)dst;
  uint32_t v32 = ((uint32_t)v << 16) | v;
  for (int i = 0; i < (n >> 1); i++) d32[i] = v32;
  if (n & 1) dst[n - 1] = v;
}

// Alpha-blend src over dst. alpha64 is the opacity of src in 0..64.
// The R+B fields and the G field are interpolated separately in one multiply
// each; the products never carry into the neighboring field.
static inline uint16_t blendAlphaRgb565(uint16_t dst, uint16_t src,
                                        uint32_t alpha64) {
  uint32_t ia = 64u - alpha64;
  uint32_t rb =
      (((dst & 0xF81Fu) * ia + (src & 0xF81Fu) * alpha64) >> 6) & 0xF81Fu;
  uint32_t g =
      (((dst & 0x07E0u) * ia + (src & 0x07E0u) * alpha64) >> 6) & 0x07E0u;
  return (uint16_t)(rb | g);
}

// Add 5/6/5 components to dst with per-channel saturation
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

// ---------------------------------------------------------------------------
// RGB444 (native 0x0RGB)

static inline uint16_t makeRgb444(uint32_t r4, uint32_t g4, uint32_t b4) {
  return (uint16_t)((r4 << 8) | (g4 << 4) | b4);
}

constexpr uint16_t colorToRgb444(Color c) {
  return (uint16_t)(((c >> 12) & 0xF00u) | ((c >> 8) & 0x0F0u) |
                    ((c >> 4) & 0x00Fu));
}

constexpr Color rgb444ToColor(uint16_t p) {
  uint32_t r = (p >> 8) & 15u, g = (p >> 4) & 15u, b = p & 15u;
  return 0xFF000000u | ((r * 17u) << 16) | ((g * 17u) << 8) | (b * 17u);
}

static inline uint16_t rgb565ToRgb444(uint16_t p) {
  return (uint16_t)(((p >> 4) & 0xF00u) | ((p >> 3) & 0x0F0u) |
                    ((p >> 1) & 0x00Fu));
}

static inline uint16_t rgb444ToRgb565(uint16_t p) {
  uint32_t r = (p >> 8) & 15u, g = (p >> 4) & 15u, b = p & 15u;
  return makeRgb565((r << 1) | (r >> 3), (g << 2) | (g >> 2),
                    (b << 1) | (b >> 3));
}

// alpha64: opacity of src in 0..64. R+B and G are blended separately.
static inline uint16_t blendAlphaRgb444(uint16_t dst, uint16_t src,
                                        uint32_t alpha64) {
  uint32_t ia = 64u - alpha64;
  uint32_t rb =
      (((dst & 0xF0Fu) * ia + (src & 0xF0Fu) * alpha64) >> 6) & 0xF0Fu;
  uint32_t g = (((dst & 0x0F0u) * ia + (src & 0x0F0u) * alpha64) >> 6) & 0x0F0u;
  return (uint16_t)(rb | g);
}

static inline uint16_t addSaturateRgb444(uint16_t dst, uint32_t r4, uint32_t g4,
                                         uint32_t b4) {
  uint32_t r = ((dst >> 8) & 15u) + r4;
  uint32_t g = ((dst >> 4) & 15u) + g4;
  uint32_t b = (dst & 15u) + b4;
  if (r > 15u) r = 15u;
  if (g > 15u) g = 15u;
  if (b > 15u) b = 15u;
  return makeRgb444(r, g, b);
}

// ---------------------------------------------------------------------------
// ARGB4444 (native 0xARGB)

static inline uint16_t makeArgb4444(uint32_t a4, uint32_t r4, uint32_t g4,
                                    uint32_t b4) {
  return (uint16_t)((a4 << 12) | (r4 << 8) | (g4 << 4) | b4);
}

constexpr uint16_t colorToArgb4444(Color c) {
  return (uint16_t)(((c >> 16) & 0xF000u) | ((c >> 12) & 0x0F00u) |
                    ((c >> 8) & 0x00F0u) | ((c >> 4) & 0x000Fu));
}

constexpr Color argb4444ToColor(uint16_t p) {
  uint32_t a = (p >> 12) & 15u, r = (p >> 8) & 15u, g = (p >> 4) & 15u,
           b = p & 15u;
  return ((a * 17u) << 24) | ((r * 17u) << 16) | ((g * 17u) << 8) | (b * 17u);
}

// Alpha-blend src (RGB part, with external opacity alpha64) over an ARGB4444
// destination. The destination alpha becomes the union of both alphas.
static inline uint16_t blendAlphaArgb4444(uint16_t dst, uint16_t src,
                                          uint32_t alpha64) {
  uint32_t rgb = blendAlphaRgb444(dst & 0x0FFFu, src & 0x0FFFu, alpha64);
  uint32_t da = (dst >> 12) & 15u;
  uint32_t a = da + ((15u - da) * alpha64 + 32u) / 64u;
  return (uint16_t)((a << 12) | rgb);
}

static inline uint16_t addSaturateArgb4444(uint16_t dst, uint32_t r4,
                                           uint32_t g4, uint32_t b4) {
  return (uint16_t)((dst & 0xF000u) |
                    addSaturateRgb444(dst & 0x0FFFu, r4, g4, b4));
}

// ---------------------------------------------------------------------------
// GRAY1

// Luminance threshold (ITU-R BT.601 weights)
constexpr uint32_t colorToGray1(Color c) {
  return (colorR(c) * 306 + colorG(c) * 601 + colorB(c) * 117) >= (128 * 1024)
             ? 1u
             : 0u;
}

static inline uint32_t rgb565ToGray1(uint16_t p) {
  uint32_t r = p >> 11, g = (p >> 5) & 63u, b = p & 31u;
  return (r * 2 + g + b * 2) >= 96u ? 1u : 0u;
}

constexpr Color gray1ToColor(uint32_t v) {
  return v ? Colors::WHITE : Colors::BLACK;
}

// ---------------------------------------------------------------------------
// Generic conversions between Color and the native pixel of a format

static inline uint32_t colorToNative(PixelFormat f, Color c) {
  switch (f) {
    case PixelFormat::GRAY1: return colorToGray1(c);
    case PixelFormat::RGB444: return colorToRgb444(c);
    case PixelFormat::ARGB4444: return colorToArgb4444(c);
    default: return colorToRgb565(c);
  }
}

static inline Color nativeToColor(PixelFormat f, uint32_t p) {
  switch (f) {
    case PixelFormat::GRAY1: return gray1ToColor(p);
    case PixelFormat::RGB444: return rgb444ToColor((uint16_t)p);
    case PixelFormat::ARGB4444: return argb4444ToColor((uint16_t)p);
    default: return rgb565ToColor((uint16_t)p);
  }
}

// ---------------------------------------------------------------------------
// Pixel cursors: sequential read/write access to one row of a given format.
// init() positions the cursor at pixel x of the row starting at `line`.
// read()/write() access the current pixel as a native pixel; next() advances;
// fill() writes n pixels and advances past them.

#if SHAPOGFX_FORMAT_GRAY1
struct CursorGray1 {
  uint8_t *p;
  uint32_t bit;  // 0 = MSB
  void init(void *line, int x) {
    p = (uint8_t *)line + (x >> 3);
    bit = (uint32_t)(x & 7);
  }
  uint32_t read() const { return (*p >> (7u - bit)) & 1u; }
  void write(uint32_t v) {
    uint8_t m = (uint8_t)(0x80u >> bit);
    *p = v ? (*p | m) : (*p & (uint8_t)~m);
  }
  void next() {
    if (++bit == 8u) {
      bit = 0;
      p++;
    }
  }
  void fill(int n, uint32_t v) {
    while (n > 0 && bit != 0) {
      write(v);
      next();
      n--;
    }
    if (n >= 8) {
      std::memset(p, v ? 0xFF : 0x00, (size_t)(n >> 3));
      p += n >> 3;
      n &= 7;
    }
    while (n-- > 0) {
      write(v);
      next();
    }
  }
};
#endif

#if SHAPOGFX_FORMAT_RGB444
struct CursorRgb444 {
  uint8_t *p;  // byte holding the R nibble of the current pixel
  bool odd;
  void init(void *line, int x) {
    p = (uint8_t *)line + (x >> 1) * 3 + (x & 1);
    odd = (x & 1) != 0;
  }
  uint32_t read() const {
    if (!odd) return ((uint32_t)p[0] << 4) | (p[1] >> 4);
    return ((uint32_t)(p[0] & 0x0Fu) << 8) | p[1];
  }
  void write(uint32_t v) {
    if (!odd) {
      p[0] = (uint8_t)(v >> 4);
      p[1] = (uint8_t)((p[1] & 0x0Fu) | ((v & 0x0Fu) << 4));
    } else {
      p[0] = (uint8_t)((p[0] & 0xF0u) | ((v >> 8) & 0x0Fu));
      p[1] = (uint8_t)v;
    }
  }
  void next() {
    p += odd ? 2 : 1;
    odd = !odd;
  }
  void fill(int n, uint32_t v) {
    if (n <= 0) return;
    if (odd) {
      write(v);
      next();
      n--;
    }
    // Even-aligned pairs: 3 bytes per 2 pixels
    uint8_t b0 = (uint8_t)(v >> 4), b1 = (uint8_t)(((v & 15u) << 4) | (v >> 8)),
            b2 = (uint8_t)v;
    while (n >= 2) {
      p[0] = b0;
      p[1] = b1;
      p[2] = b2;
      p += 3;
      n -= 2;
    }
    if (n > 0) {
      write(v);
      next();
    }
  }
};
#endif

#if SHAPOGFX_FORMAT_ARGB4444
struct CursorArgb4444 {
  uint16_t *p;
  void init(void *line, int x) { p = (uint16_t *)line + x; }
  uint32_t read() const { return *p; }
  void write(uint32_t v) { *p = (uint16_t)v; }
  void next() { p++; }
  void fill(int n, uint32_t v) {
    fill16(p, n, (uint16_t)v);
    p += n;
  }
};
#endif

#if SHAPOGFX_FORMAT_RGB565BE
struct CursorRgb565BE {
  uint16_t *p;
  void init(void *line, int x) { p = (uint16_t *)line + x; }
  uint32_t read() const { return bswap16(*p); }
  void write(uint32_t v) { *p = bswap16((uint16_t)v); }
  void next() { p++; }
  void fill(int n, uint32_t v) {
    fill16(p, n, bswap16((uint16_t)v));
    p += n;
  }
};
#endif

// Blend a native pixel `src` (with opacity alpha64 in 0..64) over `dst`
template <PixelFormat F>
static inline uint32_t blendNative(uint32_t dst, uint32_t src,
                                   uint32_t alpha64);

#if SHAPOGFX_FORMAT_GRAY1
template <>
inline uint32_t blendNative<PixelFormat::GRAY1>(uint32_t dst, uint32_t src,
                                                uint32_t alpha64) {
  return alpha64 >= 32u ? src : dst;
}
#endif
#if SHAPOGFX_FORMAT_RGB444
template <>
inline uint32_t blendNative<PixelFormat::RGB444>(uint32_t dst, uint32_t src,
                                                 uint32_t alpha64) {
  return blendAlphaRgb444((uint16_t)dst, (uint16_t)src, alpha64);
}
#endif
#if SHAPOGFX_FORMAT_ARGB4444
template <>
inline uint32_t blendNative<PixelFormat::ARGB4444>(uint32_t dst, uint32_t src,
                                                   uint32_t alpha64) {
  return blendAlphaArgb4444((uint16_t)dst, (uint16_t)src, alpha64);
}
#endif
#if SHAPOGFX_FORMAT_RGB565BE
template <>
inline uint32_t blendNative<PixelFormat::RGB565BE>(uint32_t dst, uint32_t src,
                                                   uint32_t alpha64) {
  return blendAlphaRgb565((uint16_t)dst, (uint16_t)src, alpha64);
}
#endif

// Add the RGB of native pixel `src` to `dst` with saturation
template <PixelFormat F>
static inline uint32_t addNative(uint32_t dst, uint32_t src);

#if SHAPOGFX_FORMAT_GRAY1
template <>
inline uint32_t addNative<PixelFormat::GRAY1>(uint32_t dst, uint32_t src) {
  return dst | src;
}
#endif
#if SHAPOGFX_FORMAT_RGB444
template <>
inline uint32_t addNative<PixelFormat::RGB444>(uint32_t dst, uint32_t src) {
  return addSaturateRgb444((uint16_t)dst, (src >> 8) & 15u, (src >> 4) & 15u,
                           src & 15u);
}
#endif
#if SHAPOGFX_FORMAT_ARGB4444
template <>
inline uint32_t addNative<PixelFormat::ARGB4444>(uint32_t dst, uint32_t src) {
  return addSaturateArgb4444((uint16_t)dst, (src >> 8) & 15u, (src >> 4) & 15u,
                             src & 15u);
}
#endif
#if SHAPOGFX_FORMAT_RGB565BE
template <>
inline uint32_t addNative<PixelFormat::RGB565BE>(uint32_t dst, uint32_t src) {
  return addSaturateRgb565((uint16_t)dst, (uint16_t)src);
}
#endif

// Cursor type and Color conversions for a format
template <PixelFormat F>
struct FormatTraits;

#if SHAPOGFX_FORMAT_GRAY1
template <>
struct FormatTraits<PixelFormat::GRAY1> {
  using Cursor = CursorGray1;
  static uint32_t fromColor(Color c) { return colorToGray1(c); }
  static Color toColor(uint32_t p) { return gray1ToColor(p); }
  static uint32_t fromRgb565(uint16_t p) { return rgb565ToGray1(p); }
};
#endif
#if SHAPOGFX_FORMAT_RGB444
template <>
struct FormatTraits<PixelFormat::RGB444> {
  using Cursor = CursorRgb444;
  static uint32_t fromColor(Color c) { return colorToRgb444(c); }
  static Color toColor(uint32_t p) { return rgb444ToColor((uint16_t)p); }
  static uint32_t fromRgb565(uint16_t p) { return rgb565ToRgb444(p); }
};
#endif
#if SHAPOGFX_FORMAT_ARGB4444
template <>
struct FormatTraits<PixelFormat::ARGB4444> {
  using Cursor = CursorArgb4444;
  static uint32_t fromColor(Color c) { return colorToArgb4444(c); }
  static Color toColor(uint32_t p) { return argb4444ToColor((uint16_t)p); }
  static uint32_t fromRgb565(uint16_t p) { return 0xF000u | rgb565ToRgb444(p); }
};
#endif
#if SHAPOGFX_FORMAT_RGB565BE
template <>
struct FormatTraits<PixelFormat::RGB565BE> {
  using Cursor = CursorRgb565BE;
  static uint32_t fromColor(Color c) { return colorToRgb565(c); }
  static Color toColor(uint32_t p) { return rgb565ToColor((uint16_t)p); }
  static uint32_t fromRgb565(uint16_t p) { return p; }
};
#endif

constexpr bool isFormatEnabled(PixelFormat f) {
  switch (f) {
    case PixelFormat::GRAY1: return SHAPOGFX_FORMAT_GRAY1 != 0;
    case PixelFormat::RGB444: return SHAPOGFX_FORMAT_RGB444 != 0;
    case PixelFormat::ARGB4444: return SHAPOGFX_FORMAT_ARGB4444 != 0;
    case PixelFormat::RGB565BE: return SHAPOGFX_FORMAT_RGB565BE != 0;
  }
  return false;
}

}  // namespace shapoco::gfx2d

#endif
