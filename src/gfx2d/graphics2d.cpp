#include "shapoco/gfx2d/graphics2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

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
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      copyRowT<PixelFormat::RGB565BE, D>(dl, dx, sl, sx, n);
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
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      copyRowD<PixelFormat::RGB565BE>(srcFmt, dl, dx, sl, sx, n);
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
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      return blendRowD<PixelFormat::RGB565BE>(srcFmt, dl, dx, sl, sx, n,
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
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      drawGlyphT<PixelFormat::RGB565BE>(target, clip, g, bits, gx, gy, s,
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
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      fillSpanT<PixelFormat::RGB565BE>(line, x, n, native, alpha64);
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
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      fillSpanAddT<PixelFormat::RGB565BE>(line, x, n, native);
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
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      readColorsT<PixelFormat::RGB565BE>(line, x, n, out);
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
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      writeColorsT<PixelFormat::RGB565BE>(line, x, n, src, mode, opacity64);
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
  bool operator()(int y, int &l, int &r) const {
    int dy2 = y * 2 - cy2;
    int dx2;
    if (ry2 == 0) {
      if (dy2 != 0) return false;
      dx2 = rx2;
    } else {
      float t = 1.0f - (float)(dy2 * dy2) / (float)(ry2 * ry2);
      if (t < 0.0f) return false;
      dx2 = (int)std::lround(rx2 * std::sqrt(t));
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

template <typename Extent>
static void fillExtent(Graphics2D &g, const Rect &rows, const Extent &ext,
                       void (Graphics2D::*span)(int, int, int, uint32_t,
                                                uint32_t),
                       uint32_t native, uint32_t a) {
  int l, r;
  for (int y = rows.y; y < rows.bottom(); y++) {
    if (ext(y, l, r)) (g.*span)(y, l, r + 1, native, a);
  }
}

template <typename Extent>
static void outlineExtent(Graphics2D &g, const Rect &rows, const Extent &ext,
                          void (Graphics2D::*span)(int, int, int, uint32_t,
                                                   uint32_t),
                          uint32_t native, uint32_t a) {
  int l, r, pl, pr, nl, nr;
  for (int y = rows.y; y < rows.bottom(); y++) {
    if (!ext(y, l, r)) continue;
    bool hasPrev = ext(y - 1, pl, pr);
    bool hasNext = ext(y + 1, nl, nr);
    if (!hasPrev || !hasNext) {
      (g.*span)(y, l, r + 1, native, a);  // cap row
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
    (g.*span)(y, l, le + 1, native, a);
    if (rs > le) (g.*span)(y, rs, r + 1, native, a);
  }
}

// fillSpan / plot are private; expose them to the helpers through a friend-free
// trick: member function pointers taken inside the class.
#define SHAPOGFX2D_SPAN (&Graphics2D::fillSpan)

void Graphics2D::fillEllipse(const Rect &rect, Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  Rect r = rect.normalized();
  if (a == 0 || r.isEmpty()) return;
  Rect rows = r.intersect(state_.clip);
  if (rows.isEmpty()) return;
  fillExtent(*this, rows, EllipseExtent(r), SHAPOGFX2D_SPAN,
             colorToNative(target_.format, c), a);
}

void Graphics2D::drawEllipse(const Rect &rect, Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  Rect r = rect.normalized();
  if (a == 0 || r.isEmpty()) return;
  Rect rows = r.intersect(state_.clip);
  if (rows.isEmpty()) return;
  outlineExtent(*this, rows, EllipseExtent(r), SHAPOGFX2D_SPAN,
                colorToNative(target_.format, c), a);
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
  fillExtent(*this, rows, RoundRectExtent(r, radius), SHAPOGFX2D_SPAN,
             colorToNative(target_.format, c), a);
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
  outlineExtent(*this, rows, RoundRectExtent(r, radius), SHAPOGFX2D_SPAN,
                colorToNative(target_.format, c), a);
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
