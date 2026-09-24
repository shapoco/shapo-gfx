#include "shapoco/gfx2d/graphics2d.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <type_traits>

#include "../common/intmath.hpp"
#include "arch.hpp"

namespace shapoco::gfx2d {

// ---------------------------------------------------------------------------
// Per-format row operations. Each function switches on the format once per
// row (or once per call), never per pixel.

// Fill [x, x + n) of `line` with a native color, blending when alpha64 < 64
template <PixelFormat F>
static void fillSpanT(uint8_t *line, int x, int n, uint32_t native,
                      uint32_t alpha64) {
  typename FormatTraits<F>::Cursor cur;
  cur.init(line, x);
  if (alpha64 >= 64) {
    cur.fill(n, native);
    return;
  }
  for (int i = 0; i < n; i++) {
    cur.write(blendNative<F>(cur.read(), native, alpha64));
    cur.next();
  }
}

// Add a native color (already scaled by its weight) onto [x, x + n) of
// `line`, saturating
template <PixelFormat F>
static void fillSpanAddT(uint8_t *line, int x, int n, uint32_t native) {
  typename FormatTraits<F>::Cursor cur;
  cur.init(line, x);
  for (int i = 0; i < n; i++) {
    cur.write(addNative<F>(cur.read(), native));
    cur.next();
  }
}

// Read n pixels of `line` starting at x as Colors
template <PixelFormat F>
static void readColorsT(const uint8_t *line, int x, int n, Color *out) {
  typename FormatTraits<F>::Cursor cur;
  cur.init((void *)line, x);
  for (int i = 0; i < n; i++) {
    out[i] = FormatTraits<F>::toColor(cur.read());
    cur.next();
  }
}

// Write n Colors to `line` starting at x. mode: NONE = convert and overwrite,
// ALPHA = blend with (source alpha x opacity), ADD = additive with the same
// weight.
template <PixelFormat F>
static void writeColorsT(uint8_t *line, int x, int n, const Color *src,
                         BlendMode mode, uint32_t opacity64) {
  typename FormatTraits<F>::Cursor cur;
  cur.init(line, x);
  for (int i = 0; i < n; i++) {
    const Color c = src[i];
    if (mode == BlendMode::NONE) {
      cur.write(FormatTraits<F>::fromColor(c));
    } else {
      const uint32_t a = (colorAlpha64(c) * opacity64) >> 6;
      if (a != 0) {
        if (mode == BlendMode::ALPHA) {
          cur.write(blendNative<F>(
              cur.read(), FormatTraits<F>::fromColor(c | 0xFF000000u), a));
        } else {
          // Additive: scale the color by its weight, then saturating add
          const Color scaled = makeColor(
              (colorR(c) * a) >> 6, (colorG(c) * a) >> 6, (colorB(c) * a) >> 6);
          cur.write(
              addNative<F>(cur.read(), FormatTraits<F>::fromColor(scaled)));
        }
      }
    }
    cur.next();
  }
}

// ---------------------------------------------------------------------------
// Direct format-to-format row conversions (no detour through Color). They
// produce exactly the pixels of the Color path: all formats convert through
// RGB565 (which is lossless for their color depth), except GRAY1 targets,
// whose luminance threshold is defined on Color.

// Native pixel of format S as native RGB565 plus its 4-bit alpha (15 = opaque)
template <PixelFormat S>
static inline uint32_t nativeToRgb565(uint32_t p, uint32_t &a4) {
  a4 = 15;
  if constexpr (S == PixelFormat::GRAY1) {
    return p ? 0xFFFFu : 0u;
  } else if constexpr (S == PixelFormat::RGB444) {
    return rgb444ToRgb565((uint16_t)p);
  } else if constexpr (S == PixelFormat::ARGB4444) {
    a4 = p >> 12;
    return rgb444ToRgb565((uint16_t)(p & 0x0FFFu));
  } else {
    return p;
  }
}

// Convert a native pixel of S to an opaque native pixel of D
template <PixelFormat S, PixelFormat D>
static inline uint32_t convertPixel(uint32_t p) {
  if constexpr (S == D) {
    if constexpr (D == PixelFormat::ARGB4444) return p | 0xF000u;
    return p;
  } else if constexpr (D == PixelFormat::GRAY1) {
    return FormatTraits<D>::fromColor(FormatTraits<S>::toColor(p));
  } else {
    uint32_t a4;
    return FormatTraits<D>::fromRgb565(nativeToRgb565<S>(p, a4));
  }
}

// Copy n pixels of S at (sl, sx) to D at (dl, dx), converting the format
template <PixelFormat S, PixelFormat D>
static void copyRowT(uint8_t *dl, int dx, const uint8_t *sl, int sx, int n) {
  typename FormatTraits<S>::Cursor src;
  typename FormatTraits<D>::Cursor dst;
  src.init((void *)sl, sx);
  dst.init(dl, dx);
  for (int i = 0; i < n; i++) {
    dst.write(convertPixel<S, D>(src.read()));
    src.next();
    dst.next();
  }
}

// Blend n pixels of S over D with (source alpha x opacity64)
template <PixelFormat S, PixelFormat D>
static void blendRowT(uint8_t *dl, int dx, const uint8_t *sl, int sx, int n,
                      uint32_t opacity64) {
  typename FormatTraits<S>::Cursor src;
  typename FormatTraits<D>::Cursor dst;
  src.init((void *)sl, sx);
  dst.init(dl, dx);
  if constexpr (S == PixelFormat::ARGB4444) {
    // Opacity of each 4-bit alpha, as the Color path computes it
    uint32_t alpha[16];
    for (uint32_t a4 = 0; a4 < 16; a4++)
      alpha[a4] = (alpha255To64(a4 * 17u) * opacity64) >> 6;
    for (int i = 0; i < n; i++) {
      const uint32_t p = src.read();
      const uint32_t a = alpha[p >> 12];
      if (a != 0) {
        dst.write(blendNative<D>(dst.read(), convertPixel<S, D>(p), a));
      }
      src.next();
      dst.next();
    }
  } else {
    for (int i = 0; i < n; i++) {
      dst.write(blendNative<D>(dst.read(), convertPixel<S, D>(src.read()),
                               opacity64));
      src.next();
      dst.next();
    }
  }
}

// Dispatch on the source format for a given target format
template <PixelFormat D>
static void copyRowD(PixelFormat srcFmt, uint8_t *dl, int dx, const uint8_t *sl,
                     int sx, int n) {
  switch (srcFmt) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1:
      copyRowT<PixelFormat::GRAY1, D>(dl, dx, sl, sx, n);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      copyRowT<PixelFormat::RGB444, D>(dl, dx, sl, sx, n);
      break;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444:
      copyRowT<PixelFormat::ARGB4444, D>(dl, dx, sl, sx, n);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      copyRowT<PixelFormat::RGB565_SWAPPED, D>(dl, dx, sl, sx, n);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      copyRowT<PixelFormat::RGB565, D>(dl, dx, sl, sx, n);
      break;
#endif
    default: break;
  }
}

// Blending is specialized for ARGB4444 sources (sprites) and same-format
// sources with an opacity; other combinations return false and go through
// Color.
template <PixelFormat D>
static bool blendRowD(PixelFormat srcFmt, uint8_t *dl, int dx,
                      const uint8_t *sl, int sx, int n, uint32_t opacity64) {
#if SHAPOGFX_FORMAT_ARGB4444
  if (srcFmt == PixelFormat::ARGB4444) {
    blendRowT<PixelFormat::ARGB4444, D>(dl, dx, sl, sx, n, opacity64);
    return true;
  }
#endif
  if (srcFmt == D) {
    blendRowT<D, D>(dl, dx, sl, sx, n, opacity64);
    return true;
  }
  return false;
}

static void copyRowFmt(PixelFormat dstFmt, PixelFormat srcFmt, uint8_t *dl,
                       int dx, const uint8_t *sl, int sx, int n) {
  switch (dstFmt) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1:
      copyRowD<PixelFormat::GRAY1>(srcFmt, dl, dx, sl, sx, n);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      copyRowD<PixelFormat::RGB444>(srcFmt, dl, dx, sl, sx, n);
      break;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444:
      copyRowD<PixelFormat::ARGB4444>(srcFmt, dl, dx, sl, sx, n);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      copyRowD<PixelFormat::RGB565_SWAPPED>(srcFmt, dl, dx, sl, sx, n);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      copyRowD<PixelFormat::RGB565>(srcFmt, dl, dx, sl, sx, n);
      break;
#endif
    default: break;
  }
}

static bool blendRowFmt(PixelFormat dstFmt, PixelFormat srcFmt, uint8_t *dl,
                        int dx, const uint8_t *sl, int sx, int n,
                        uint32_t opacity64) {
  switch (dstFmt) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1:
      return blendRowD<PixelFormat::GRAY1>(srcFmt, dl, dx, sl, sx, n,
                                           opacity64);
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      return blendRowD<PixelFormat::RGB444>(srcFmt, dl, dx, sl, sx, n,
                                            opacity64);
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444:
      return blendRowD<PixelFormat::ARGB4444>(srcFmt, dl, dx, sl, sx, n,
                                              opacity64);
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      return blendRowD<PixelFormat::RGB565_SWAPPED>(srcFmt, dl, dx, sl, sx, n,
                                                    opacity64);
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      return blendRowD<PixelFormat::RGB565>(srcFmt, dl, dx, sl, sx, n,
                                            opacity64);
#endif
    default: return false;
  }
}

// Glyph rows of a GFXfont: set bits become fg pixels (scale 1) or scale x
// scale blocks. `bits` is the font bitmap, rows are `width` bits, contiguous.
template <PixelFormat F>
static void drawGlyphT(const Surface &target, const Rect &clip,
                       const GFXglyph &g, const uint8_t *bits, int gx, int gy,
                       int s, uint32_t native, uint32_t alpha64) {
  using Cursor = typename FormatTraits<F>::Cursor;
  const int w = g.width, h = g.height;
  uint32_t bitIndex = (uint32_t)g.bitmapOffset * 8u;
  auto bitAt = [bits](uint32_t bi) -> bool {
    return (bits[bi >> 3] >> (7u - (bi & 7u))) & 1u;
  };
  if (s == 1) {
    const int i0 = std::max(0, clip.x - gx);
    const int i1 = std::min(w, clip.right() - gx);
    if (i0 >= i1) return;
    for (int j = 0; j < h; j++, bitIndex += (uint32_t)w) {
      const int y = gy + j;
      if (y < clip.y || y >= clip.bottom()) continue;
      Cursor cur;
      cur.init(target.linePtr(y), gx + i0);
      uint32_t bi = bitIndex + (uint32_t)i0;
      if (alpha64 >= 64) {
        for (int i = i0; i < i1; i++, bi++) {
          if (bitAt(bi)) cur.write(native);
          cur.next();
        }
      } else {
        for (int i = i0; i < i1; i++, bi++) {
          if (bitAt(bi)) cur.write(blendNative<F>(cur.read(), native, alpha64));
          cur.next();
        }
      }
    }
    return;
  }
  // Magnified: runs of set bits become filled blocks of s x s pixels each
  for (int j = 0; j < h; j++, bitIndex += (uint32_t)w) {
    const int y0 = std::max(gy + j * s, clip.y);
    const int y1 = std::min(gy + (j + 1) * s, clip.bottom());
    if (y0 >= y1) continue;
    int runStart = -1;
    for (int i = 0; i <= w; i++) {
      const bool on = (i < w) && bitAt(bitIndex + (uint32_t)i);
      if (on && runStart < 0) runStart = i;
      if (!on && runStart >= 0) {
        const int x0 = std::max(gx + runStart * s, clip.x);
        const int x1 = std::min(gx + i * s, clip.right());
        if (x0 < x1) {
          for (int y = y0; y < y1; y++)
            fillSpanT<F>(target.linePtr(y), x0, x1 - x0, native, alpha64);
        }
        runStart = -1;
      }
    }
  }
}

static void drawGlyphFmt(const Surface &target, const Rect &clip,
                         const GFXglyph &g, const uint8_t *bits, int gx, int gy,
                         int s, uint32_t native, uint32_t alpha64) {
  switch (target.format) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1:
      drawGlyphT<PixelFormat::GRAY1>(target, clip, g, bits, gx, gy, s, native,
                                     alpha64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      drawGlyphT<PixelFormat::RGB444>(target, clip, g, bits, gx, gy, s, native,
                                      alpha64);
      break;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444:
      drawGlyphT<PixelFormat::ARGB4444>(target, clip, g, bits, gx, gy, s,
                                        native, alpha64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      drawGlyphT<PixelFormat::RGB565_SWAPPED>(target, clip, g, bits, gx, gy, s,
                                              native, alpha64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      drawGlyphT<PixelFormat::RGB565>(target, clip, g, bits, gx, gy, s, native,
                                      alpha64);
      break;
#endif
    default: break;
  }
}

static void fillSpanFmt(PixelFormat fmt, uint8_t *line, int x, int n,
                        uint32_t native, uint32_t alpha64) {
  switch (fmt) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1:
      fillSpanT<PixelFormat::GRAY1>(line, x, n, native, alpha64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      fillSpanT<PixelFormat::RGB444>(line, x, n, native, alpha64);
      break;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444:
      fillSpanT<PixelFormat::ARGB4444>(line, x, n, native, alpha64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      fillSpanT<PixelFormat::RGB565_SWAPPED>(line, x, n, native, alpha64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      fillSpanT<PixelFormat::RGB565>(line, x, n, native, alpha64);
      break;
#endif
    default: break;
  }
}

static void fillSpanAddFmt(PixelFormat fmt, uint8_t *line, int x, int n,
                           uint32_t native) {
  switch (fmt) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1:
      fillSpanAddT<PixelFormat::GRAY1>(line, x, n, native);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      fillSpanAddT<PixelFormat::RGB444>(line, x, n, native);
      break;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444:
      fillSpanAddT<PixelFormat::ARGB4444>(line, x, n, native);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      fillSpanAddT<PixelFormat::RGB565_SWAPPED>(line, x, n, native);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      fillSpanAddT<PixelFormat::RGB565>(line, x, n, native);
      break;
#endif
    default: break;
  }
}

static void readColorsFmt(PixelFormat fmt, const uint8_t *line, int x, int n,
                          Color *out) {
  switch (fmt) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1:
      readColorsT<PixelFormat::GRAY1>(line, x, n, out);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      readColorsT<PixelFormat::RGB444>(line, x, n, out);
      break;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444:
      readColorsT<PixelFormat::ARGB4444>(line, x, n, out);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      readColorsT<PixelFormat::RGB565_SWAPPED>(line, x, n, out);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      readColorsT<PixelFormat::RGB565>(line, x, n, out);
      break;
#endif
    default:
      for (int i = 0; i < n; i++) out[i] = Colors::TRANSPARENT;
      break;
  }
}

static void writeColorsFmt(PixelFormat fmt, uint8_t *line, int x, int n,
                           const Color *src, BlendMode mode,
                           uint32_t opacity64) {
  switch (fmt) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1:
      writeColorsT<PixelFormat::GRAY1>(line, x, n, src, mode, opacity64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      writeColorsT<PixelFormat::RGB444>(line, x, n, src, mode, opacity64);
      break;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444:
      writeColorsT<PixelFormat::ARGB4444>(line, x, n, src, mode, opacity64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      writeColorsT<PixelFormat::RGB565_SWAPPED>(line, x, n, src, mode,
                                                opacity64);
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      writeColorsT<PixelFormat::RGB565>(line, x, n, src, mode, opacity64);
      break;
#endif
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Target and basic fills

void Graphics2D::setTarget(const Surface &target) {
  target_ = target;
  if (!isFormatEnabled(target_.format)) target_.pixels = nullptr;
  // Larger than SHAPOGFX_COORD_BITS allows: treated like a disabled format
  if (target_.width > SHAPOGFX_COORD_MAX || target_.height > SHAPOGFX_COORD_MAX)
    target_.pixels = nullptr;
  resetClipRect();
}

void Graphics2D::fillSpanRaw(int y, int x0, int x1, uint32_t native,
                             uint32_t alpha64) {
  if (x1 <= x0) return;
  fillSpanFmt(target_.format, target_.linePtr(y), x0, x1 - x0, native, alpha64);
}

void Graphics2D::fillSpan(int y, int x0, int x1, uint32_t native,
                          uint32_t alpha64) {
  const Rect &c = state_.clip;
  if (y < c.y || y >= c.bottom()) return;
  x0 = std::max(x0, c.x);
  x1 = std::min(x1, c.right());
  fillSpanRaw(y, x0, x1, native, alpha64);
}

void Graphics2D::plot(int x, int y, uint32_t native, uint32_t alpha64) {
  if (!state_.clip.contains(x, y)) return;
  fillSpanRaw(y, x, x + 1, native, alpha64);
}

void Graphics2D::fillRectRaw(const Rect &r, uint32_t native, uint32_t alpha64) {
  for (int y = r.y; y < r.bottom(); y++)
    fillSpanRaw(y, r.x, r.right(), native, alpha64);
}

void Graphics2D::setPixel(int x, int y, Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  if (a == 0) return;
  plot(x, y, colorToNative(target_.format, c), a);
}

Color Graphics2D::getPixel(int x, int y) const {
  if (!hasTarget() || !bounds().contains(x, y)) return Colors::TRANSPARENT;
  Color c;
  readColorsFmt(target_.format, target_.linePtr(y), x, 1, &c);
  return c;
}

void Graphics2D::fillRect(const Rect &rect, Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  if (a == 0) return;
  Rect r = rect.normalized().intersect(state_.clip);
  if (r.isEmpty()) return;
  fillRectRaw(r, colorToNative(target_.format, c), a);
}

void Graphics2D::fillRect(const Rect &rect, Color c, BlendMode mode,
                          int opacity) {
  if (!hasTarget()) return;
  if (opacity < 0) opacity = 0;
  if (opacity > 255) opacity = 255;
  if (mode == BlendMode::NONE) {
    fillRect(rect, c | 0xFF000000u);
    return;
  }
  const uint32_t a = (colorAlpha64(c) * alpha255To64((uint32_t)opacity)) >> 6;
  if (a == 0) return;
  Rect r = rect.normalized().intersect(state_.clip);
  if (r.isEmpty()) return;
  if (mode == BlendMode::ALPHA) {
    fillRectRaw(r, colorToNative(target_.format, c), a);
    return;
  }
  // Additive: the color scaled by its weight, then a saturating add
  const Color scaled = makeColor((colorR(c) * a) >> 6, (colorG(c) * a) >> 6,
                                 (colorB(c) * a) >> 6);
  const uint32_t native = colorToNative(target_.format, scaled);
  for (int y = r.y; y < r.bottom(); y++) {
    fillSpanAddFmt(target_.format, target_.linePtr(y), r.x, r.width, native);
  }
}

void Graphics2D::drawRect(const Rect &rect, Color c, int thickness) {
  Rect r = rect.normalized();
  if (r.isEmpty() || thickness <= 0) return;
  if (thickness * 2 >= r.width || thickness * 2 >= r.height) {
    fillRect(r, c);
    return;
  }
  fillRect(r.x, r.y, r.width, thickness, c);
  fillRect(r.x, r.bottom() - thickness, r.width, thickness, c);
  fillRect(r.x, r.y + thickness, thickness, r.height - 2 * thickness, c);
  fillRect(r.right() - thickness, r.y + thickness, thickness,
           r.height - 2 * thickness, c);
}

// ---------------------------------------------------------------------------
// Ellipses and rounded rectangles
//
// Shapes are described by the horizontal extent [l, r] (inclusive) of each row.
// Filling walks the rows; outlines draw the pixels of a row that are not
// covered by both neighboring rows, plus the row's end pixels.

namespace {

// Ellipse inscribed in rect (normalized, non-empty)
struct EllipseExtent {
  int cx2, cy2, rx2, ry2;  // doubled center / radii (exact for even sizes)
  explicit EllipseExtent(const Rect &r)
      : cx2(r.x * 2 + r.width - 1),
        cy2(r.y * 2 + r.height - 1),
        rx2(r.width - 1),
        ry2(r.height - 1) {}
  // Half-width of row y: dx2 = round(rx2 * sqrt(ry2^2 - dy2^2) / ry2), in
  // 32-bit integers. The radicand is scaled by a power of 4 into [2^30,
  // 2^32) before the square root, and the root refined by its remainder,
  // which gives the exact rounding (checked for every size up to 300 x 300,
  // where the float formula this replaces was off in 342 rows); radii beyond
  // 2^15 (far larger than any target) are scaled down first.
  bool operator()(int y, int &l, int &r) const {
    const int64_t dy2l = (int64_t)y * 2 - cy2;
    const uint64_t ady = dy2l < 0 ? (uint64_t)-dy2l : (uint64_t)dy2l;
    int dx2;
    if (ry2 == 0) {
      if (ady != 0) return false;
      dx2 = rx2;
    } else {
      if (ady > (uint64_t)ry2) return false;
      uint32_t ry = (uint32_t)ry2, dy = (uint32_t)ady;
      while (ry >= 32768u) ry >>= 1, dy >>= 1;
      const uint32_t n = ry * ry - dy * dy;  // < 2^30
      if (n == 0) {
        dx2 = 0;
      } else {
        const int sh = __builtin_clz(n) & ~1;  // n << sh in [2^30, 2^32)
        const uint32_t m = n << sh;
        const uint32_t s = gfx::intmath::isqrt32(m);
        // sqrt(m) - s ~= (m - s^2) / 2s, in Q15: without it the truncated
        // root would bias the rounding of dx2 low
        const uint32_t frac = ((m - s * s) << 14) / s;
        const uint32_t den = ry << (sh >> 1);  // < 2^30
        uint32_t rx = (uint32_t)rx2;
        int rs = 0;
        while (rx >= 32768u) rx >>= 1, rs++;
        // rx * s < 2^31, rx * frac < 2^30
        dx2 = (int)(((rx * s + ((rx * frac) >> 15) + (den >> 1)) / den) << rs);
      }
    }
    l = (cx2 - dx2) / 2;
    r = (cx2 + dx2) / 2;
    return true;
  }
};

// Rectangle with circular corners of the given radius
struct RoundRectExtent {
  Rect rect;
  int radius;
  EllipseExtent tl,
      br;  // corner circles (top-left and bottom-right; tr/bl are mirrors)
  RoundRectExtent(const Rect &r, int rad)
      : rect(r),
        radius(rad),
        tl(Rect{r.x, r.y, rad * 2, rad * 2}),
        br(Rect{r.right() - rad * 2, r.bottom() - rad * 2, rad * 2, rad * 2}) {}
  bool operator()(int y, int &l, int &r) const {
    if (y < rect.y || y >= rect.bottom()) return false;
    int cl, cr;
    if (y < rect.y + radius) {
      if (!tl(y, cl, cr)) return false;
      l = cl;
      r = rect.right() - 1 - (cl - rect.x);
    } else if (y >= rect.bottom() - radius) {
      if (!br(y, cl, cr)) return false;
      r = cr;
      l = rect.x + (rect.right() - 1 - cr);
    } else {
      l = rect.x;
      r = rect.right() - 1;
    }
    return true;
  }
};

}  // namespace

// Nearest integer (halves away from zero), without the library call lrint()
// takes even on a core with an FPU
static inline int32_t roundToInt(float v) {
  return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

// floor(a / b) and ceil(a / b) for any signs
static inline int floorDiv(int a, int b) {
  const int q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
static inline int ceilDiv(int a, int b) {
  const int q = a / b;
  return (a % b != 0 && ((a < 0) == (b < 0))) ? q + 1 : q;
}

namespace {

// The angle range of an arc or a sector: the pixels whose direction from the
// center of the ellipse lies within it. Each edge is a half-plane
// a * px + b * py > 0 in doubled coordinates relative to the center, with
// the direction of the edge rounded to integers once; a range up to pi is
// the intersection of the half-plane after the start and the one before the
// end, a larger one the complement of the range from the end to the start.
// Ties are broken as if every pixel were moved by (e, e^2) for an
// infinitesimal e, so that no pixel lies on an edge: two sectors that share
// an angle share the rounded edge and every pixel belongs to one of them
// (the center included).
struct Wedge {
  bool empty = false, full = false, reflex = false;
  int cx2, cy2, lim;
  int a0, b0, a1, b1;

  Wedge(const Rect &r, float start, float end)
      : cx2(r.x * 2 + r.width - 1), cy2(r.y * 2 + r.height - 1) {
    constexpr float TWO_PI = 6.28318531f, PI = 3.14159265f;
    if (!std::isfinite(start) || !std::isfinite(end)) {
      empty = true;
      return;
    }
    float sweep = end - start;
    if (sweep >= TWO_PI) {
      full = true;
      return;
    }
    sweep = std::fmod(sweep, TWO_PI);
    if (sweep < 0.0f) sweep += TWO_PI;
    if (sweep >= TWO_PI) {
      full = true;
      return;
    }
    if (!(sweep > 0.0f)) {
      empty = true;
      return;
    }
    reflex = sweep > PI;
    // Parametric angles: a direction on the unit circle stretched to the
    // ellipse. Its integer length keeps b * py (|py| < the height) and
    // a * px within 2^29.
    const int size = std::max(r.width, r.height);
    int mag = 8192;
    for (int m = size; m > 65536 && mag > 1; m >>= 1) mag >>= 1;
    lim = size + 2;
    const float sx = (float)std::max(r.width - 1, 1);
    const float sy = (float)std::max(r.height - 1, 1);
    auto dir = [&](float t, int &dx, int &dy) {
      const float x = sx * std::cos(t), y = sy * std::sin(t);
      const float k = (float)mag / std::sqrt(x * x + y * y);
      dx = roundToInt(x * k);
      dy = roundToInt(y * k);
    };
    int sdx, sdy, edx, edy;
    dir(start, sdx, sdy);
    dir(end, edx, edy);
    if (reflex) {
      std::swap(sdx, edx);
      std::swap(sdy, edy);
    }
    // after the start: cross(s, p) > 0; before the end: cross(p, e) > 0
    a0 = -sdy, b0 = sdx;
    a1 = edy, b1 = -edx;
  }

  // Columns [lo, hi] of row py2 on the positive side of a * px + b * py
  void halfRow(int a, int b, int py2, int &lo, int &hi) const {
    const int k = b * py2;
    lo = INT_MIN / 2;
    hi = INT_MAX / 2;
    if (a > 0) {
      // a * px2 + k >= 0 (a tie counts as positive)
      const int t = clampInt(-lim, lim, ceilDiv(-k, a));
      lo = ceilDiv(cx2 + t, 2);
    } else if (a < 0) {
      // -a * px2 < k
      const int t = clampInt(-lim, lim, ceilDiv(k, -a) - 1);
      hi = floorDiv(cx2 + t, 2);
    } else if (!(k > 0 || (k == 0 && b > 0))) {
      lo = 1, hi = 0;
    }
  }

  // Emit the parts of [x0, x1) of row y inside the angle range
  template <typename Emit>
  void clip(int y, int x0, int x1, Emit &&emit) const {
    if (full) {
      emit(y, x0, x1);
      return;
    }
    const int py2 = y * 2 - cy2;
    int lo0, hi0, lo1, hi1;
    halfRow(a0, b0, py2, lo0, hi0);
    halfRow(a1, b1, py2, lo1, hi1);
    const int lo = std::max(lo0, lo1), hi = std::min(hi0, hi1);
    if (!reflex) {
      const int s0 = std::max(x0, lo), s1 = std::min(x1, hi + 1);
      if (s0 < s1) emit(y, s0, s1);
    } else if (lo > hi) {
      emit(y, x0, x1);
    } else {
      if (x0 < lo) emit(y, x0, std::min(x1, lo));
      if (hi + 1 < x1) emit(y, std::max(x0, hi + 1), x1);
    }
  }
};

}  // namespace

// Emit [l, r + 1) of every row of the shape
template <typename Extent, typename Emit>
static void fillExtent(const Rect &rows, const Extent &ext, Emit &&emit) {
  int l, r;
  for (int y = rows.y; y < rows.bottom(); y++) {
    if (ext(y, l, r)) emit(y, l, r + 1);
  }
}

template <typename Extent, typename Emit>
static void outlineExtent(const Rect &rows, const Extent &ext, Emit &&emit) {
  int l, r, pl, pr, nl, nr;
  for (int y = rows.y; y < rows.bottom(); y++) {
    if (!ext(y, l, r)) continue;
    bool hasPrev = ext(y - 1, pl, pr);
    bool hasNext = ext(y + 1, nl, nr);
    if (!hasPrev || !hasNext) {
      emit(y, l, r + 1);  // cap row
      continue;
    }
    // The outline of this row has to reach far enough inwards to meet the
    // row above and the row below, or a shape whose edge is nearly flat --
    // the top and bottom of a circle -- comes out as a dotted line: there
    // the neighbouring rows' ends are many columns away, and drawing only
    // this row's own end pixels leaves the gap between them empty.
    //
    // So each side runs from this row's end to just short of the NEARER of
    // the two neighbours' ends on that side (the narrower row, the one the
    // outline has to bridge to), which is the max on the left and the min on
    // the right. Where the edge is steep the neighbours are a column away
    // and this is the single end pixel it was before. The clamps keep a
    // degenerate extent from painting across the shape.
    int innerL = std::max(pl, nl), innerR = std::min(pr, nr);
    int le = std::max(l, innerL - 1), rs = std::min(r, innerR + 1);
    if (le > r) le = r;
    if (rs < l) rs = l;
    emit(y, l, le + 1);
    if (rs > le) emit(y, rs, r + 1);
  }
}

void Graphics2D::fillEllipse(const Rect &rect, Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  Rect r = rect.normalized();
  if (a == 0 || r.isEmpty()) return;
  Rect rows = r.intersect(state_.clip);
  if (rows.isEmpty()) return;
  const uint32_t native = colorToNative(target_.format, c);
  fillExtent(rows, EllipseExtent(r),
             [&](int y, int x0, int x1) { fillSpan(y, x0, x1, native, a); });
}

void Graphics2D::drawEllipse(const Rect &rect, Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  Rect r = rect.normalized();
  if (a == 0 || r.isEmpty()) return;
  Rect rows = r.intersect(state_.clip);
  if (rows.isEmpty()) return;
  const uint32_t native = colorToNative(target_.format, c);
  outlineExtent(rows, EllipseExtent(r),
                [&](int y, int x0, int x1) { fillSpan(y, x0, x1, native, a); });
}

void Graphics2D::drawArc(const Rect &rect, float startAngle, float endAngle,
                         Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  Rect r = rect.normalized();
  if (a == 0 || r.isEmpty()) return;
  Rect rows = r.intersect(state_.clip);
  if (rows.isEmpty()) return;
  const Wedge wedge(r, startAngle, endAngle);
  if (wedge.empty) return;
  const uint32_t native = colorToNative(target_.format, c);
  auto span = [&](int y, int x0, int x1) { fillSpan(y, x0, x1, native, a); };
  outlineExtent(rows, EllipseExtent(r),
                [&](int y, int x0, int x1) { wedge.clip(y, x0, x1, span); });
}

void Graphics2D::fillSector(const Rect &rect, float startAngle, float endAngle,
                            Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  Rect r = rect.normalized();
  if (a == 0 || r.isEmpty()) return;
  Rect rows = r.intersect(state_.clip);
  if (rows.isEmpty()) return;
  const Wedge wedge(r, startAngle, endAngle);
  if (wedge.empty) return;
  const uint32_t native = colorToNative(target_.format, c);
  auto span = [&](int y, int x0, int x1) { fillSpan(y, x0, x1, native, a); };
  fillExtent(rows, EllipseExtent(r),
             [&](int y, int x0, int x1) { wedge.clip(y, x0, x1, span); });
}

void Graphics2D::fillRoundRect(const Rect &rect, int radius, Color c) {
  Rect r = rect.normalized();
  radius = std::min(radius, std::min(r.width, r.height) / 2);
  if (radius <= 0) {
    fillRect(r, c);
    return;
  }
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  if (a == 0 || r.isEmpty()) return;
  Rect rows = r.intersect(state_.clip);
  if (rows.isEmpty()) return;
  const uint32_t native = colorToNative(target_.format, c);
  fillExtent(rows, RoundRectExtent(r, radius),
             [&](int y, int x0, int x1) { fillSpan(y, x0, x1, native, a); });
}

void Graphics2D::drawRoundRect(const Rect &rect, int radius, Color c) {
  Rect r = rect.normalized();
  radius = std::min(radius, std::min(r.width, r.height) / 2);
  if (radius <= 0) {
    drawRect(r, c);
    return;
  }
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  if (a == 0 || r.isEmpty()) return;
  Rect rows = r.intersect(state_.clip);
  if (rows.isEmpty()) return;
  const uint32_t native = colorToNative(target_.format, c);
  outlineExtent(rows, RoundRectExtent(r, radius),
                [&](int y, int x0, int x1) { fillSpan(y, x0, x1, native, a); });
}

// ---------------------------------------------------------------------------
// Lines and polygons

// Lines and polygons are walked in coordinates relative to the center of the
// clip rectangle, where the clip rectangle (at most SHAPOGFX_COORD_MAX <
// 32768 pixels wide) lies within +-LINE_SAFE. Within that range every 16.16
// value and every product the walkers form fits 32 bits.
static constexpr int LINE_SAFE = (1 << 14) - 1;
static constexpr int LINE_INPUT_MAX = 1 << 29;  // inputs are clamped to this
static inline int clampInput(int v) {
  return v < -LINE_INPUT_MAX ? -LINE_INPUT_MAX
                             : (v > LINE_INPUT_MAX ? LINE_INPUT_MAX : v);
}
static inline bool lineSafe(int v) { return v >= -LINE_SAFE && v <= LINE_SAFE; }

void Graphics2D::drawLine(int x0, int y0, int x1, int y1, Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  if (a == 0) return;
  const Rect &clip = state_.clip;
  if (clip.isEmpty()) return;
  const uint32_t native = colorToNative(target_.format, c);
  const int ox = clip.x + clip.width / 2, oy = clip.y + clip.height / 2;
  // The clip rectangle, relative
  const int cx0 = clip.x - ox, cx1 = clip.right() - 1 - ox;
  const int cy0 = clip.y - oy, cy1 = clip.bottom() - 1 - oy;

  // A segment reaching beyond +-LINE_SAFE is halved until its parts either
  // miss the clip rectangle or fit (a split point is rounded to a whole
  // pixel, which a line reaching thousands of pixels off screen does not
  // show). Each level leaves one half pending, so the stack stays short.
  struct Seg {
    int x0, y0, x1, y1;
  };
  Seg stack[40];
  int sp = 0;
  stack[sp++] = {clampInput(x0) - ox, clampInput(y0) - oy, clampInput(x1) - ox,
                 clampInput(y1) - oy};
  while (sp > 0) {
    Seg s = stack[--sp];
    if (std::max(s.x0, s.x1) < cx0 || std::min(s.x0, s.x1) > cx1 ||
        std::max(s.y0, s.y1) < cy0 || std::min(s.y0, s.y1) > cy1)
      continue;
    if (!lineSafe(s.x0) || !lineSafe(s.y0) || !lineSafe(s.x1) ||
        !lineSafe(s.y1)) {
      if (std::abs(s.x1 - s.x0) <= 2 && std::abs(s.y1 - s.y0) <= 2) {
        // A few pixels at the far edge of a clip rectangle nearly 32768
        // pixels wide: clamping cannot bend it visibly
        auto cl = [](int v) {
          return std::max(-LINE_SAFE, std::min(LINE_SAFE, v));
        };
        s = {cl(s.x0), cl(s.y0), cl(s.x1), cl(s.y1)};
      } else if (sp + 2 <= (int)(sizeof(stack) / sizeof(stack[0]))) {
        const int mx = (s.x0 >> 1) + (s.x1 >> 1) + (s.x0 & s.x1 & 1);
        const int my = (s.y0 >> 1) + (s.y1 >> 1) + (s.y0 & s.y1 & 1);
        stack[sp++] = {mx, my, s.x1, s.y1};
        stack[sp++] = {s.x0, s.y0, mx, my};
        continue;
      } else {
        continue;  // unreachable: every split halves the extent
      }
    }
    drawLineSafe(s.x0, s.y0, s.x1, s.y1, ox, oy, native, a);
  }
}

// A segment within +-LINE_SAFE of (ox, oy)
void Graphics2D::drawLineSafe(int x0, int y0, int x1, int y1, int ox, int oy,
                              uint32_t native, uint32_t a) {
  const Rect &clip = state_.clip;
  // Walk the major axis (i) with a 16.16 fixed-point minor coordinate (j),
  // skipping the parts outside the clip rectangle along the major axis.
  const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
  int oi = ox, oj = oy;
  if (steep) {
    std::swap(x0, y0);
    std::swap(x1, y1);
    std::swap(oi, oj);
  }
  if (x0 > x1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
  }
  const int iMin = (steep ? clip.y : clip.x) - oi;
  const int iMax = (steep ? clip.bottom() : clip.right()) - 1 - oi;
  const int jMin = (steep ? clip.x : clip.y) - oj;
  const int jMax = (steep ? clip.right() : clip.bottom()) - 1 - oj;
  const int di = x1 - x0;
  const int dj = y1 - y0;  // |dj| <= di <= 2 * LINE_SAFE < 32768
  const int32_t slope = di ? (int32_t)((dj * 65536) / di) : 0;
  int iStart = std::max(x0, iMin), iEnd = std::min(x1, iMax);
  if (iStart > iEnd) return;
  int32_t jf = (int32_t)y0 * 65536 + 0x8000 + slope * (iStart - x0);

  // Group consecutive major-axis steps with the same minor coordinate into runs
  int i = iStart;
  while (i <= iEnd) {
    const int j = jf >> 16;
    int k = i;
    while (k < iEnd && ((jf + slope) >> 16) == j) {
      jf += slope;
      k++;
    }
    jf += slope;
    if (j >= jMin && j <= jMax) {
      if (steep) {
        for (int r = i; r <= k; r++)
          fillSpanRaw(r + oi, j + oj, j + oj + 1, native, a);
      } else {
        fillSpanRaw(j + oj, i + oi, k + oi + 1, native, a);
      }
    }
    i = k + 1;
  }
}

void Graphics2D::drawPolyline(const vec2i *pts, int n, Color c) {
  for (int i = 1; i < n; i++) drawLine(pts[i - 1], pts[i], c);
}

void Graphics2D::drawPolygon(const vec2i *pts, int n, Color c) {
  if (n < 2) return;
  drawPolyline(pts, n, c);
  if (n > 2) drawLine(pts[n - 1], pts[0], c);
}

void Graphics2D::fillPolygon(const vec2i *pts, int n, Color c) {
  static constexpr int MAX_CROSSES = 16;
  if (!hasTarget() || n < 3) return;
  const uint32_t a = colorAlpha64(c);
  if (a == 0) return;
  const Rect &clip = state_.clip;
  if (clip.isEmpty()) return;
  const uint32_t native = colorToNative(target_.format, c);

  // Vertices relative to the center of the clip rectangle and clamped to
  // +-LINE_SAFE (a vertex that far outside only tilts edges that are off
  // screen nearly everywhere), so that (y - p.y) * (q.x - p.x) fits 32 bits.
  const int ox = clip.x + clip.width / 2, oy = clip.y + clip.height / 2;
  auto vtx = [&](int i) -> vec2i {
    const vec2i &p = pts[i];
    return {std::max(-LINE_SAFE, std::min(LINE_SAFE, clampInput(p.x) - ox)),
            std::max(-LINE_SAFE, std::min(LINE_SAFE, clampInput(p.y) - oy))};
  };

  int yMin = vtx(0).y, yMax = yMin;
  for (int i = 1; i < n; i++) {
    yMin = std::min(yMin, vtx(i).y);
    yMax = std::max(yMax, vtx(i).y);
  }
  yMin = std::max(yMin, clip.y - oy);
  yMax = std::min(yMax, clip.bottom() - 1 - oy);

  int xs[MAX_CROSSES];
  for (int y = yMin; y <= yMax; y++) {
    int m = 0;
    for (int i = 0; i < n && m < MAX_CROSSES; i++) {
      const vec2i p = vtx(i);
      const vec2i q = vtx(i + 1 < n ? i + 1 : 0);
      if ((p.y <= y && q.y > y) || (q.y <= y && p.y > y)) {
        const int x = p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y);
        // Insertion sort (m is small)
        int k = m++;
        while (k > 0 && xs[k - 1] > x) {
          xs[k] = xs[k - 1];
          k--;
        }
        xs[k] = x;
      }
    }
    for (int i = 0; i + 1 < m; i += 2)
      fillSpan(y + oy, xs[i] + ox, xs[i + 1] + ox, native, a);
  }
}

// ---------------------------------------------------------------------------
// Images

void Graphics2D::drawImage(const Texture &img, int dx, int dy,
                           const Rect &srcRect, BlendMode mode, int opacity) {
  if (!hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  // Clip the source rectangle to the image, then the destination to the clip
  // rect
  Rect src = srcRect.normalized().intersect({0, 0, img.width, img.height});
  Rect dst = Rect{dx, dy, src.width, src.height}.intersect(state_.clip);
  if (dst.isEmpty()) return;
  src.x += dst.x - dx;
  src.y += dst.y - dy;
  src.width = dst.width;
  src.height = dst.height;
  const uint32_t op64 = alpha255To64((uint32_t)clampInt(0, 255, opacity));
  if (mode != BlendMode::NONE && op64 == 0) return;

  const bool srcHasAlpha = (img.format == PixelFormat::ARGB4444);
  const bool sameFormat = (img.format == target_.format);
  // A format without alpha drawn with ALPHA at full opacity is a plain copy
  if (mode == BlendMode::ALPHA && !srcHasAlpha && op64 >= 64)
    mode = BlendMode::NONE;
  // Fast path: plain copy of 16-bit formats
  if (sameFormat && bitsPerPixel(img.format) == 16 && mode == BlendMode::NONE) {
    for (int j = 0; j < dst.height; j++) {
      std::memcpy(target_.linePtr(dst.y + j) + (size_t)dst.x * 2,
                  img.linePtr(src.y + j) + (size_t)src.x * 2,
                  (size_t)dst.width * 2);
    }
    return;
  }

  if (mode == BlendMode::NONE) {
    for (int j = 0; j < dst.height; j++) {
      copyRowFmt(target_.format, img.format, target_.linePtr(dst.y + j), dst.x,
                 img.linePtr(src.y + j), src.x, dst.width);
    }
    return;
  }
  if (mode == BlendMode::ALPHA &&
      blendRowFmt(target_.format, img.format, target_.linePtr(dst.y), dst.x,
                  img.linePtr(src.y), src.x, dst.width, op64)) {
    for (int j = 1; j < dst.height; j++) {
      blendRowFmt(target_.format, img.format, target_.linePtr(dst.y + j), dst.x,
                  img.linePtr(src.y + j), src.x, dst.width, op64);
    }
    return;
  }

  // Additive, and alpha blends of other format pairs: convert through Color
  // in chunks
  static constexpr int CHUNK = 64;
  Color tmp[CHUNK];
  for (int j = 0; j < dst.height; j++) {
    const uint8_t *sl = img.linePtr(src.y + j);
    uint8_t *dl = target_.linePtr(dst.y + j);
    for (int x = 0; x < dst.width; x += CHUNK) {
      int n = std::min(CHUNK, dst.width - x);
      readColorsFmt(img.format, sl, src.x + x, n, tmp);
      writeColorsFmt(target_.format, dl, dst.x + x, n, tmp, mode, op64);
    }
  }
}

// ---------------------------------------------------------------------------
// Scaled and transformed images
//
// A row is drawn by a walker, which fetches the source pixels in target order,
// and an op, which writes them. The walkers: runs of target pixels showing
// the same source pixel (magnification), one source pixel per target pixel
// (minification), the affine walk and its RP2 interpolator version. The ops:
// a plain copy within a format, alpha blending of ARGB4444 (sprites) and of
// a format onto itself, and for everything else the Color conversion of
// drawImage(). The format switch happens once per row.

namespace {

// Pixel x of a row as stored: the uint16_t as it is in memory for the 16-bit
// formats (a copy needs no swap), the native pixel for the others
template <PixelFormat S>
inline uint32_t loadRaw(const uint8_t *line, int x) {
  if constexpr (S == PixelFormat::GRAY1) {
    return (line[x >> 3] >> (7 - (x & 7))) & 1u;
  } else if constexpr (S == PixelFormat::RGB444) {
    const uint8_t *p = line + (x >> 1) * 3;
    if (x & 1) return ((uint32_t)(p[1] & 0x0Fu) << 8) | p[2];
    return ((uint32_t)p[0] << 4) | (p[1] >> 4);
  } else {
    return ((const uint16_t *)line)[x];
  }
}

template <PixelFormat S>
inline uint32_t rawToNative(uint32_t raw) {
  if constexpr (S == PixelFormat::RGB565_SWAPPED) {
    return bswap16((uint16_t)raw);
  } else {
    return raw;
  }
}

// Ops: put() writes one target pixel, run() n pixels of the same source
// pixel. SRC is the source format they read.

// Same 16-bit format: the stored values unchanged (the ARGB4444 alpha too)
struct OpCopy16 {
  static constexpr PixelFormat SRC = PixelFormat::RGB565;  // any 16-bit one
  uint16_t *p;
  void put(uint32_t raw) { *p++ = (uint16_t)raw; }
  void run(uint32_t raw, int n) {
    fill16(p, n, (uint16_t)raw);
    p += n;
  }
};

// Same format, GRAY1 or RGB444
template <PixelFormat D>
struct OpCopy {
  static constexpr PixelFormat SRC = D;
  typename FormatTraits<D>::Cursor cur;
  void put(uint32_t raw) {
    cur.write(raw);
    cur.next();
  }
  void run(uint32_t raw, int n) { cur.fill(n, raw); }
};

#if SHAPOGFX_FORMAT_ARGB4444
// ARGB4444 blended with its alpha x opacity; opaque runs become fills
template <PixelFormat D>
struct OpBlendArgb {
  static constexpr PixelFormat SRC = PixelFormat::ARGB4444;
  typename FormatTraits<D>::Cursor cur;
  const uint32_t *alpha;  // weight (0..64) of each 4-bit alpha
  void put(uint32_t raw) {
    const uint32_t a = alpha[raw >> 12];
    if (a != 0) {
      const uint32_t s = convertPixel<SRC, D>(raw);
      cur.write(a >= 64 ? s : blendNative<D>(cur.read(), s, a));
    }
    cur.next();
  }
  void run(uint32_t raw, int n) {
    const uint32_t a = alpha[raw >> 12];
    if (a == 0) {
      cur.skip(n);
      return;
    }
    const uint32_t s = convertPixel<SRC, D>(raw);
    if (a >= 64) {
      cur.fill(n, s);
      return;
    }
    for (; n > 0; n--) {
      cur.write(blendNative<D>(cur.read(), s, a));
      cur.next();
    }
  }
};
#endif

// A format without alpha onto itself with an opacity
template <PixelFormat D>
struct OpBlendSame {
  static constexpr PixelFormat SRC = D;
  typename FormatTraits<D>::Cursor cur;
  uint32_t a;
  void put(uint32_t raw) {
    cur.write(blendNative<D>(cur.read(), rawToNative<D>(raw), a));
    cur.next();
  }
  void run(uint32_t raw, int n) {
    const uint32_t s = rawToNative<D>(raw);
    for (; n > 0; n--) {
      cur.write(blendNative<D>(cur.read(), s, a));
      cur.next();
    }
  }
};

// Anything else: into Colors, written by writeColorsFmt()
template <PixelFormat S>
struct OpColor {
  static constexpr PixelFormat SRC = S;
  Color *out;
  void put(uint32_t raw) {
    *out++ = FormatTraits<S>::toColor(rawToNative<S>(raw));
  }
  void run(uint32_t raw, int n) {
    const Color c = FormatTraits<S>::toColor(rawToNative<S>(raw));
    for (; n > 0; n--) *out++ = c;
  }
};

enum class BlitPath : uint8_t { COPY16, COPY, BLEND_ARGB, BLEND_SAME, COLOR };

// What a scaled or transformed drawImage() does per pixel
struct ImageBlit {
  PixelFormat dst, src;
  BlendMode mode;
  BlitPath path;
  uint32_t op64;
  uint32_t alpha[16];  // BLEND_ARGB

  // false if nothing is drawn. The modes behave as in drawImage().
  bool init(PixelFormat d, PixelFormat s, BlendMode m, int opacity) {
    dst = d, src = s, mode = m;
    op64 = alpha255To64((uint32_t)clampInt(0, 255, opacity));
    if (mode != BlendMode::NONE && op64 == 0) return false;
    const bool srcAlpha = (s == PixelFormat::ARGB4444);
    if (mode == BlendMode::ALPHA && !srcAlpha && op64 >= 64)
      mode = BlendMode::NONE;
    if (mode == BlendMode::NONE) {
      path = s != d
                 ? BlitPath::COLOR
                 : (bitsPerPixel(d) == 16 ? BlitPath::COPY16 : BlitPath::COPY);
    } else if (mode == BlendMode::ALPHA && srcAlpha) {
      path = BlitPath::BLEND_ARGB;
      for (uint32_t a4 = 0; a4 < 16; a4++)
        alpha[a4] = (alpha255To64(a4 * 17u) * op64) >> 6;
    } else if (mode == BlendMode::ALPHA && s == d) {
      path = BlitPath::BLEND_SAME;
    } else {
      path = BlitPath::COLOR;
    }
    return true;
  }
};

template <PixelFormat F>
using FormatTag = std::integral_constant<PixelFormat, F>;

// Call fn(FormatTag<f>) for an enabled format
template <typename Fn>
void withFormat(PixelFormat f, Fn &&fn) {
  switch (f) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1: fn(FormatTag<PixelFormat::GRAY1>{}); break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444: fn(FormatTag<PixelFormat::RGB444>{}); break;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444: fn(FormatTag<PixelFormat::ARGB4444>{}); break;
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    case PixelFormat::RGB565_SWAPPED:
      fn(FormatTag<PixelFormat::RGB565_SWAPPED>{});
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565: fn(FormatTag<PixelFormat::RGB565>{}); break;
#endif
    default: break;
  }
}

constexpr int IMAGE_CHUNK = 64;

// Draw n pixels of the target row `dl` from x, fetched by `walk`. A walker
// with SRC16_ONLY reads 16-bit sources only (the caller guarantees one); one
// with RUNS takes only the paths that write a run at once, a copy or an
// ARGB4444 sprite (the others gain little from runs, and the caller walks
// them pixel by pixel instead).
template <typename Walk>
__attribute__((noinline)) void blitRow(const ImageBlit &b, uint8_t *dl, int x,
                                       int n, Walk &walk) {
  switch (b.path) {
    case BlitPath::COPY16: {
      OpCopy16 op{(uint16_t *)dl + x};
      walk(op, n);
      break;
    }
    case BlitPath::COPY:
      if constexpr (!Walk::SRC16_ONLY) {
        withFormat(b.dst, [&](auto tag) {
          constexpr PixelFormat D = decltype(tag)::value;
          if constexpr (bitsPerPixel(D) != 16) {
            OpCopy<D> op;
            op.cur.init(dl, x);
            walk(op, n);
          }
        });
      }
      break;
    case BlitPath::BLEND_ARGB:
#if SHAPOGFX_FORMAT_ARGB4444
      withFormat(b.dst, [&](auto tag) {
        OpBlendArgb<decltype(tag)::value> op;
        op.cur.init(dl, x);
        op.alpha = b.alpha;
        walk(op, n);
      });
#endif
      break;
    case BlitPath::BLEND_SAME:
      if constexpr (!Walk::RUNS) {
        withFormat(b.dst, [&](auto tag) {
          constexpr PixelFormat D = decltype(tag)::value;
          if constexpr (D != PixelFormat::ARGB4444 &&
                        (!Walk::SRC16_ONLY || bitsPerPixel(D) == 16)) {
            OpBlendSame<D> op;
            op.cur.init(dl, x);
            op.a = b.op64;
            walk(op, n);
          }
        });
      }
      break;
    case BlitPath::COLOR:
      if constexpr (!Walk::RUNS) {
        withFormat(b.src, [&](auto tag) {
          constexpr PixelFormat S = decltype(tag)::value;
          if constexpr (!Walk::SRC16_ONLY || bitsPerPixel(S) == 16) {
            Color tmp[IMAGE_CHUNK];
            for (int i = 0; i < n; i += IMAGE_CHUNK) {
              const int k = std::min(IMAGE_CHUNK, n - i);
              OpColor<S> op{tmp};
              walk(op, k);
              writeColorsFmt(b.dst, dl, x + i, k, tmp, b.mode, b.op64);
            }
          }
        });
      }
      break;
  }
}

// Walkers. They keep their position between calls, so that the Color path
// can take a row in chunks.

// Minification: one source pixel per target pixel, pos advancing by step,
// plus dir whenever the remainder reaches den
struct StepWalk {
  static constexpr bool SRC16_ONLY = false, RUNS = false;
  const uint8_t *line;
  int pos, step, dir;
  uint32_t rem, rStep, den;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) {
      op.put(loadRaw<Op::SRC>(line, pos));
      pos += step;
      rem += rStep;
      if (rem >= den) {
        rem -= den;
        pos += dir;
      }
    }
  }
};

// Magnification: runs of q or q + 1 target pixels per source pixel
// (Bresenham's run-slice), `left` pixels left of the current one
struct RunWalk {
  static constexpr bool SRC16_ONLY = false, RUNS = true;
  const uint8_t *line;
  int pos, dir, left;
  uint32_t e, q, r, den;
  template <typename Op>
  void operator()(Op &op, int n) {
    while (n > 0) {
      if (left == 0) {
        pos += dir;
        left = (int)q;
        if (r > e) {
          left++;
          e += den - r;
        } else {
          e -= r;
        }
      }
      const int k = left < n ? left : n;
      op.run(loadRaw<Op::SRC>(line, pos), k);
      left -= k;
      n -= k;
    }
  }
};

// Affine: 16.16 source coordinates, inside the image for every pixel
struct AffineWalk {
  static constexpr bool SRC16_ONLY = false, RUNS = false;
  const uint8_t *pixels;
  uint32_t stride;
  int32_t u, v, du, dv;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) {
      op.put(loadRaw<Op::SRC>(pixels + (uint32_t)(v >> 16) * stride, u >> 16));
      u += du;
      v += dv;
    }
  }
};

#if SHAPOGFX2D_RP2_INTERP
struct InterpWalk {
  static constexpr bool SRC16_ONLY = true, RUNS = false;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) op.put(arch::rp2::InterpAffine::fetch());
  }
};
#endif

// One axis of a scaled image: target pixel t of dn takes source pixel
// k(t) = floor((2t + 1) sn / 2dn) of sn (the one under its center), counted
// from the far end when mirrored. Exact, in 32 bits: sn, dn <= 32767.
struct ScaleAxis {
  int start, count;  // visible target pixels
  int pos, dir;      // source pixel of the first one, +1 or -1
  // per target pixel: pos += step, and dir more when rem reaches den
  int step;
  uint32_t rem, rStep, den;
  // magnification (sn < dn): the first run is run0 long
  bool runs;
  int run0;
  uint32_t e, q, r, rden;
};

constexpr int SCALE_MAX = 32767;

// Map dn target pixels at dstPos onto sn source pixels at srcPos, keeping the
// target pixels inside [clip0, clip1) whose source pixel lies in [0, size)
bool mapAxis(int srcPos, int sn, int size, int dstPos, int dn, int clip0,
             int clip1, bool mirror, ScaleAxis &m) {
  // Beyond this, the source or target range is out of reach anyway
  srcPos = clampInt(-(1 << 20), 1 << 20, srcPos);
  dstPos = clampInt(-(1 << 20), 1 << 20, dstPos);
  int kLo, kHi;  // usable values of k
  if (!mirror) {
    kLo = std::max(0, -srcPos);
    kHi = std::min(sn, size - srcPos) - 1;
  } else {
    kLo = std::max(0, srcPos + sn - size);
    kHi = std::min(sn, srcPos + sn) - 1;
  }
  if (kLo > kHi) return false;
  // First target pixel whose k is k or more: 2k dn <= (2t + 1) sn
  auto firstOf = [&](int k) -> int {
    const int a = 2 * k * dn - sn;  // < 2^31
    return a <= 0 ? 0 : (int)(((uint32_t)a + 2u * sn - 1u) / (2u * sn));
  };
  const int t0 = std::max(firstOf(kLo), clip0 - dstPos);
  const int t1 = std::min(firstOf(kHi + 1), clip1 - dstPos);
  if (t0 >= t1) return false;
  m.start = dstPos + t0;
  m.count = t1 - t0;
  m.dir = mirror ? -1 : 1;
  const uint32_t n0 = (2u * t0 + 1u) * (uint32_t)sn;
  m.den = 2u * dn;
  const int k0 = (int)(n0 / m.den);
  m.rem = n0 % m.den;
  m.pos = (mirror ? srcPos + sn - 1 : srcPos) + m.dir * k0;
  m.step = m.dir * (sn / dn);
  m.rStep = 2u * (uint32_t)(sn % dn);
  m.runs = sn < dn;
  if (m.runs) {
    // Run k starts at s(k) = ceil(a(k) / 2sn), a(k) = 2k dn - sn; e(k) =
    // s(k) 2sn - a(k) decides whether run k is q or q + 1 long
    const uint32_t a = 2u * (uint32_t)(k0 + 1) * dn - sn;
    const uint32_t s1 = (a + 2u * sn - 1u) / (2u * sn);
    m.run0 = (int)s1 - t0;
    m.e = s1 * 2u * sn - a;
    m.q = (uint32_t)(dn / sn);
    m.r = 2u * (uint32_t)(dn % sn);
    m.rden = 2u * sn;
  }
  return true;
}

// Narrow [k0, k1] to the k with lo <= c + k d <= hi. Callers keep lo - c
// and hi - c within 32 bits.
bool narrowSpan(int32_t c, int32_t d, int32_t lo, int32_t hi, int &k0,
                int &k1) {
  const int32_t p = lo - c, q = hi - c;
  if (d > 0) {
    k0 = std::max(k0, ceilDiv(p, d));
    k1 = std::min(k1, floorDiv(q, d));
  } else if (d < 0) {
    k0 = std::max(k0, ceilDiv(q, d));
    k1 = std::min(k1, floorDiv(p, d));
  } else if (p > 0 || q < 0) {
    return false;
  }
  return k0 <= k1;
}

// Float estimate of the columns x where c + g x lies in [lo, hi), a pixel
// wider on either side (ig = 1 / g)
bool estimateSpan(float c, float g, float ig, float lo, float hi, float &x0,
                  float &x1) {
  if (g == 0.0f) return c > lo - 1.0f && c < hi + 1.0f;
  float t0 = (lo - c) * ig, t1 = (hi - c) * ig;
  if (g < 0.0f) std::swap(t0, t1);
  x0 = std::max(x0, t0 - 1.0f);
  x1 = std::min(x1, t1 + 1.0f);
  return x0 <= x1;
}

}  // namespace

void Graphics2D::drawImage(const Texture &img, const Rect &dstRect,
                           const Rect &srcRect, BlendMode mode, int opacity) {
  if (!hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  const bool mirrorX = dstRect.width < 0, mirrorY = dstRect.height < 0;
  const Rect d = dstRect.normalized(), s = srcRect.normalized();
  if (d.isEmpty() || s.isEmpty() || d.width > SCALE_MAX ||
      d.height > SCALE_MAX || s.width > SCALE_MAX || s.height > SCALE_MAX)
    return;
  if (!mirrorX && !mirrorY && d.width == s.width && d.height == s.height) {
    drawImage(img, d.x, d.y, s, mode, opacity);
    return;
  }
  const Rect &clip = state_.clip;
  ScaleAxis ax, ay;
  if (!mapAxis(s.x, s.width, img.width, d.x, d.width, clip.x, clip.right(),
               mirrorX, ax) ||
      !mapAxis(s.y, s.height, img.height, d.y, d.height, clip.y, clip.bottom(),
               mirrorY, ay))
    return;
  ImageBlit b;
  if (!b.init(target_.format, img.format, mode, opacity)) return;

  // Runs of equal pixels where they become fills
  const bool runs =
      ax.runs && (b.path == BlitPath::COPY16 || b.path == BlitPath::COPY ||
                  b.path == BlitPath::BLEND_ARGB);
  // A plain copy repeats the previous target row for the same source row
  const bool repeat =
      b.mode == BlendMode::NONE && bitsPerPixel(target_.format) == 16;
  const size_t xOff = (size_t)ax.start * 2, bytes = (size_t)ax.count * 2;
  int row = ay.pos, prevRow = -1;
  uint32_t rem = ay.rem;
  const uint8_t *prevLine = nullptr;
  for (int j = 0; j < ay.count; j++) {
    uint8_t *dl = target_.linePtr(ay.start + j);
    if (repeat && row == prevRow) {
      std::memcpy(dl + xOff, prevLine + xOff, bytes);
    } else {
      const uint8_t *sl = img.linePtr(row);
      if (runs) {
        RunWalk w{sl, ax.pos, ax.dir, ax.run0, ax.e, ax.q, ax.r, ax.rden};
        blitRow(b, dl, ax.start, ax.count, w);
      } else {
        StepWalk w{sl, ax.pos, ax.step, ax.dir, ax.rem, ax.rStep, ax.den};
        blitRow(b, dl, ax.start, ax.count, w);
      }
    }
    prevRow = row;
    prevLine = dl;
    row += ay.step;
    rem += ay.rStep;
    if (rem >= ay.den) {
      rem -= ay.den;
      row += ay.dir;
    }
  }
}

// Limits of the affine walk: 16.16 source coordinates of an image up to
// 16384 pixels and steps of up to 4096 pixels (one pixel of the target
// spanning 4096 of the source) stay within 31 bits, and so do the offsets
// narrowSpan() divides.
static constexpr int AFFINE_SIZE_MAX = 16384;
static constexpr float AFFINE_STEP_MAX = 4096.0f;

static inline bool wholePixel(float v) {
  return std::fabs(v) < (float)(1 << 24) && v == std::floor(v);
}

void Graphics2D::drawImage(const Texture &img, const affine2f &m,
                           const Rect &srcRect, BlendMode mode, int opacity) {
  if (!hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  const Rect s = srcRect.normalized();
  if (s.isEmpty()) return;

  // No rotation or shear, and the corners on whole pixels: the exact scaled
  // path, which also takes a mirroring scale
  if (m.b == 0.0f && m.c == 0.0f) {
    const float w = m.a * (float)s.width, h = m.d * (float)s.height;
    if (wholePixel(m.tx) && wholePixel(m.ty) && wholePixel(w) &&
        wholePixel(h) && w != 0.0f && h != 0.0f &&
        std::fabs(w) <= (float)SCALE_MAX && std::fabs(h) <= (float)SCALE_MAX) {
      drawImage(img, Rect{(int)m.tx, (int)m.ty, (int)w, (int)h}, s, mode,
                opacity);
      return;
    }
  }

  const Rect in = s.intersect(Rect{0, 0, img.width, img.height});
  if (in.isEmpty() || in.right() > AFFINE_SIZE_MAX ||
      in.bottom() > AFFINE_SIZE_MAX)
    return;
  affine2f inv;
  if (!m.invert(inv)) return;
  // Source coordinates of the center of target pixel (x, y), absolute in
  // the image: u = A x + C y + E, v = B x + D y + F
  const float A = inv.a, B = inv.b, C = inv.c, D = inv.d;
  if (!(std::fabs(A) <= AFFINE_STEP_MAX && std::fabs(B) <= AFFINE_STEP_MAX &&
        std::fabs(C) <= AFFINE_STEP_MAX && std::fabs(D) <= AFFINE_STEP_MAX))
    return;  // also NaN
  const float E = inv.tx + (float)s.x + 0.5f * (A + C);
  const float F = inv.ty + (float)s.y + 0.5f * (B + D);

  // Bounding box of the drawn part of the image, a pixel wider
  const Rect &clip = state_.clip;
  float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  for (int i = 0; i < 4; i++) {
    const vec2f p = m.apply((float)(in.x - s.x + ((i & 1) ? in.width : 0)),
                            (float)(in.y - s.y + ((i & 2) ? in.height : 0)));
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
    x0 = i ? std::min(x0, p.x) : p.x;
    x1 = i ? std::max(x1, p.x) : p.x;
    y0 = i ? std::min(y0, p.y) : p.y;
    y1 = i ? std::max(y1, p.y) : p.y;
  }
  const float lim = (float)(1 << 20);
  const int bx0 = std::max(clip.x, (int)std::floor(std::max(x0, -lim)) - 1);
  const int bx1 =
      std::min(clip.right() - 1, (int)std::ceil(std::min(x1, lim)) + 1);
  const int by0 = std::max(clip.y, (int)std::floor(std::max(y0, -lim)) - 1);
  const int by1 =
      std::min(clip.bottom() - 1, (int)std::ceil(std::min(y1, lim)) + 1);
  if (bx0 > bx1 || by0 > by1) return;

  ImageBlit b;
  if (!b.init(target_.format, img.format, mode, opacity)) return;
  const int32_t du = roundToInt(A * 65536.0f);
  const int32_t dv = roundToInt(B * 65536.0f);
  const float iA = A != 0.0f ? 1.0f / A : 0.0f;
  const float iB = B != 0.0f ? 1.0f / B : 0.0f;
  const int32_t uLo = in.x << 16, uHi = (in.right() << 16) - 1;
  const int32_t vLo = in.y << 16, vHi = (in.bottom() << 16) - 1;
  const float margin = AFFINE_STEP_MAX * 2.0f;
#if SHAPOGFX2D_RP2_INTERP
  arch::rp2::InterpAffine interp;
  const bool useInterp = arch::rp2::InterpAffine::usable(img);
  if (useInterp) interp.begin(img, du, dv);
#endif

  for (int y = by0; y <= by1; y++) {
    // Each row is clipped exactly in fixed point around a reference column
    // that the float estimate puts inside it, so that the walk never leaves
    // the image
    const float uc = C * (float)y + E, vc = D * (float)y + F;
    float ex0 = (float)bx0, ex1 = (float)bx1;
    if (!estimateSpan(uc, A, iA, (float)in.x, (float)in.right(), ex0, ex1) ||
        !estimateSpan(vc, B, iB, (float)in.y, (float)in.bottom(), ex0, ex1))
      continue;
    const int xr = clampInt(bx0, bx1, (int)std::floor((ex0 + ex1) * 0.5f));
    const float ur = A * (float)xr + uc, vr = B * (float)xr + vc;
    if (!(ur > (float)in.x - margin && ur < (float)in.right() + margin &&
          vr > (float)in.y - margin && vr < (float)in.bottom() + margin))
      continue;
    const int32_t u = roundToInt(ur * 65536.0f);
    const int32_t v = roundToInt(vr * 65536.0f);
    int k0 = bx0 - xr, k1 = bx1 - xr;
    if (!narrowSpan(u, du, uLo, uHi, k0, k1) ||
        !narrowSpan(v, dv, vLo, vHi, k0, k1))
      continue;
    const int32_t u0 = u + k0 * du, v0 = v + k0 * dv;
    uint8_t *dl = target_.linePtr(y);
#if SHAPOGFX2D_RP2_INTERP
    if (useInterp) {
      interp.row(u0, v0);
      InterpWalk w;
      blitRow(b, dl, xr + k0, k1 - k0 + 1, w);
      continue;
    }
#endif
    AffineWalk w{(const uint8_t *)img.pixels, img.stride, u0, v0, du, dv};
    blitRow(b, dl, xr + k0, k1 - k0 + 1, w);
  }
#if SHAPOGFX2D_RP2_INTERP
  if (useInterp) interp.end();
#endif
}

void Graphics2D::drawBitmap(const Texture &bmp, int dx, int dy,
                            const Rect &srcRect, Color fg, Color bg) {
  if (!hasTarget() || !bmp.pixels || bmp.format != PixelFormat::GRAY1) return;
#if SHAPOGFX_FORMAT_GRAY1
  Rect src = srcRect.normalized().intersect({0, 0, bmp.width, bmp.height});
  Rect dst = Rect{dx, dy, src.width, src.height}.intersect(state_.clip);
  if (dst.isEmpty()) return;
  src.x += dst.x - dx;
  src.y += dst.y - dy;
  const uint32_t fa = colorAlpha64(fg), ba = colorAlpha64(bg);
  if (fa == 0 && ba == 0) return;
  const uint32_t fn = colorToNative(target_.format, fg),
                 bn = colorToNative(target_.format, bg);

  for (int j = 0; j < dst.height; j++) {
    CursorGray1 cur;
    cur.init((void *)bmp.linePtr(src.y + j), src.x);
    const int y = dst.y + j;
    // Run-length walk: consecutive equal bits become one span
    int runStart = 0;
    uint32_t runBit = cur.read();
    for (int i = 1; i <= dst.width; i++) {
      uint32_t bit = 0;
      if (i < dst.width) {
        cur.next();
        bit = cur.read();
        if (bit == runBit) continue;
      }
      if (runBit) {
        if (fa) fillSpanRaw(y, dst.x + runStart, dst.x + i, fn, fa);
      } else {
        if (ba) fillSpanRaw(y, dst.x + runStart, dst.x + i, bn, ba);
      }
      runStart = i;
      runBit = bit;
    }
  }
#else
  (void)dx, (void)dy, (void)srcRect, (void)fg, (void)bg;
#endif
}

// ---------------------------------------------------------------------------
// Text

void Graphics2D::setFont(const GFXfont *font, int scale) {
  TextState &t = state_.text;
  t.font = font;
  t.scale = scale < 1 ? 1 : scale;
  t.ascent = 0;
  t.lineHeight = 0;
  if (!font) return;
  int maxBottom = 0;
  const int count = font->last - font->first + 1;
  for (int i = 0; i < count; i++) {
    const GFXglyph &g = font->glyph[i];
    int top = -g.yOffset;  // pixels above the baseline
    if (i == 0 || top > t.ascent) t.ascent = top;
    int bottom = g.yOffset + g.height;  // pixels below the baseline
    if (i == 0 || bottom > maxBottom) maxBottom = bottom;
  }
  t.lineHeight = t.ascent + maxBottom;
}

int Graphics2D::lineAdvance() const {
  const TextState &t = state_.text;
  return t.font ? t.font->yAdvance * t.scale : 0;
}

int Graphics2D::charAdvance(int code) const {
  const TextState &t = state_.text;
  if (!t.font || code < t.font->first || code > t.font->last) return 0;
  return t.font->glyph[code - t.font->first].xAdvance * t.scale;
}

int Graphics2D::measureText(const char *str) const {
  if (!str || !state_.text.font) return 0;
  int best = 0, w = 0;
  for (const char *p = str; *p; p++) {
    if (*p == '\n') {
      best = std::max(best, w);
      w = 0;
    } else {
      w += charAdvance((unsigned char)*p);
    }
  }
  return std::max(best, w);
}

int Graphics2D::drawChar(int x, int y, int code) {
  const TextState &t = state_.text;
  if (!t.font || code < t.font->first || code > t.font->last) return 0;
  const GFXglyph &g = t.font->glyph[code - t.font->first];
  const int s = t.scale;

  if (colorA(t.background) != 0)
    fillRect(x, y, g.xAdvance * s, t.lineHeight * s, t.background);
  if (!hasTarget()) return g.xAdvance * s;
  const uint32_t fa = colorAlpha64(t.color);
  if (fa == 0 || g.width == 0 || g.height == 0) return g.xAdvance * s;

  // Skip glyphs entirely outside the clip rect
  const int gx = x + g.xOffset * s;
  const int gy = y + (t.ascent + g.yOffset) * s;
  if (Rect{gx, gy, g.width * s, g.height * s}
          .intersect(state_.clip)
          .isEmpty()) {
    return g.xAdvance * s;
  }

  drawGlyphFmt(target_, state_.clip, g, t.font->bitmap, gx, gy, s,
               colorToNative(target_.format, t.color), fa);
  return g.xAdvance * s;
}

void Graphics2D::drawString(const char *str) {
  TextState &t = state_.text;
  if (!t.font || !str) return;
  for (const char *p = str; *p; p++) {
    if (*p == '\n') {
      t.cursorX = t.lineStartX;
      t.cursorY += lineAdvance();
    } else {
      t.cursorX += drawChar(t.cursorX, t.cursorY, (unsigned char)*p);
    }
  }
}

}  // namespace shapoco::gfx2d
