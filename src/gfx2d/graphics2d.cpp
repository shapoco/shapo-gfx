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
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Target and basic fills

void Graphics2D::setTarget(const Surface &target) {
  target_ = target;
  if (!isFormatEnabled(target_.format)) target_.pixels = nullptr;
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
    // Left part not covered by both neighbors, right part likewise; at least
    // the end pixels
    int innerL = std::min(pl, nl), innerR = std::max(pr, nr);
    int le = std::max(l, innerL - 1), rs = std::min(r, innerR + 1);
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

void Graphics2D::drawLine(int x0, int y0, int x1, int y1, Color c) {
  if (!hasTarget()) return;
  const uint32_t a = colorAlpha64(c);
  if (a == 0) return;
  const uint32_t native = colorToNative(target_.format, c);
  const Rect &clip = state_.clip;

  // Walk the major axis (i) with a 16.16 fixed-point minor coordinate (j),
  // skipping the parts outside the clip rectangle along the major axis.
  const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
  if (steep) {
    std::swap(x0, y0);
    std::swap(x1, y1);
  }
  if (x0 > x1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
  }
  const int iMin = steep ? clip.y : clip.x;
  const int iMax = (steep ? clip.bottom() : clip.right()) - 1;
  const int jMin = steep ? clip.x : clip.y;
  const int jMax = (steep ? clip.right() : clip.bottom()) - 1;
  const int di = x1 - x0;
  const int32_t slope = di ? (int32_t)(((int64_t)(y1 - y0) * 65536) / di) : 0;
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
        for (int r = i; r <= k; r++) fillSpanRaw(r, j, j + 1, native, a);
      } else {
        fillSpanRaw(j, i, k + 1, native, a);
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
  const uint32_t native = colorToNative(target_.format, c);

  int yMin = pts[0].y, yMax = pts[0].y;
  for (int i = 1; i < n; i++) {
    yMin = std::min(yMin, pts[i].y);
    yMax = std::max(yMax, pts[i].y);
  }
  yMin = std::max(yMin, state_.clip.y);
  yMax = std::min(yMax, state_.clip.bottom() - 1);

  int xs[MAX_CROSSES];
  for (int y = yMin; y <= yMax; y++) {
    int m = 0;
    for (int i = 0; i < n && m < MAX_CROSSES; i++) {
      const vec2i &p = pts[i];
      const vec2i &q = pts[(i + 1) % n];
      if ((p.y <= y && q.y > y) || (q.y <= y && p.y > y)) {
        xs[m++] = p.x + (int)((int64_t)(y - p.y) * (q.x - p.x) / (q.y - p.y));
      }
    }
    std::sort(xs, xs + m);
    for (int i = 0; i + 1 < m; i += 2) fillSpan(y, xs[i], xs[i + 1], native, a);
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
  const uint32_t op64 =
      ((uint32_t)clampInt(0, 255, opacity) * 64u + 127u) / 255u;
  if (mode != BlendMode::NONE && op64 == 0) return;

  const bool srcHasAlpha = (img.format == PixelFormat::ARGB4444);
  const bool sameFormat = (img.format == target_.format);
  // Fast path: plain copy of 16-bit formats
  if (sameFormat && bitsPerPixel(img.format) == 16 &&
      (mode == BlendMode::NONE ||
       (mode == BlendMode::ALPHA && !srcHasAlpha && op64 >= 64))) {
    for (int j = 0; j < dst.height; j++) {
      std::memcpy(target_.linePtr(dst.y + j) + (size_t)dst.x * 2,
                  img.linePtr(src.y + j) + (size_t)src.x * 2,
                  (size_t)dst.width * 2);
    }
    return;
  }
  // A format without alpha drawn with ALPHA at full opacity is a plain copy
  if (mode == BlendMode::ALPHA && !srcHasAlpha && op64 >= 64)
    mode = BlendMode::NONE;

  // Generic path: convert through Color in chunks
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

  const uint32_t fn = colorToNative(target_.format, t.color);
  const uint8_t *bits = t.font->bitmap;
  uint32_t bitIndex = (uint32_t)g.bitmapOffset * 8u;
  for (int j = 0; j < g.height; j++) {
    // Runs of set bits become filled rectangles (s x s pixels per glyph pixel)
    int runStart = -1;
    for (int i = 0; i <= g.width; i++) {
      bool on = false;
      if (i < g.width) {
        on = (bits[bitIndex >> 3] >> (7u - (bitIndex & 7u))) & 1u;
        bitIndex++;
      }
      if (on && runStart < 0) runStart = i;
      if (!on && runStart >= 0) {
        Rect r = Rect{gx + runStart * s, gy + j * s, (i - runStart) * s, s}
                     .intersect(state_.clip);
        if (!r.isEmpty()) fillRectRaw(r, fn, fa);
        runStart = -1;
      }
    }
  }
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
