// Graphics2D: per-format row operations, the state (arena, stack, transform,
// blend, color key), pixels, rectangles, lines and text. Ellipses, rounded
// rectangles and polygons are in shapes.cpp, images and masks in images.cpp.

#include "internal.hpp"

namespace shapoco::gfx2d {

using namespace detail;

// ---------------------------------------------------------------------------
// Per-format row operations. Each function switches on the format once per
// row (or once per call), never per pixel.

// Paint [x, x + n) of `line`. FILL comes first: it is the path of every
// opaque shape, and a plain fill of the cursor.
template <PixelFormat F>
static void fillSpanT(uint8_t *line, int x, int n, const Paint &p) {
  typename FormatTraits<F>::Cursor cur;
  cur.init(line, x);
  if (p.op == PaintOp::FILL) {
    cur.fill(n, p.native);
    return;
  }
  if (BLEND && p.op == PaintOp::ADD) {
    for (int i = 0; i < n; i++) {
      cur.write(addNative<F>(cur.read(), p.native));
      cur.next();
    }
    return;
  }
  for (int i = 0; i < n; i++) {
    cur.write(blendNative<F>(cur.read(), p.native, p.alpha64));
    cur.next();
  }
}

template <PixelFormat F>
static void readColorsT(const uint8_t *line, int x, int n, Color *out) {
  typename FormatTraits<F>::Cursor cur;
  cur.init((void *)line, x);
  for (int i = 0; i < n; i++) {
    out[i] = FormatTraits<F>::toColor(cur.read());
    cur.next();
  }
}

template <PixelFormat F>
static void writeColorsT(uint8_t *line, int x, int n, const Color *src,
                         WriteMode mode, uint32_t opacity64) {
  typename FormatTraits<F>::Cursor cur;
  cur.init(line, x);
  for (int i = 0; i < n; i++) {
    const Color c = src[i];
    if (mode == WriteMode::COPY) {
      cur.write(FormatTraits<F>::fromColor(c));
    } else if (mode == WriteMode::COPY_KEYED) {
      if (c != 0) cur.write(FormatTraits<F>::fromColor(c));
    } else {
      const uint32_t a = (colorAlpha64(c) * opacity64) >> 6;
      if (a != 0) {
        if (!BLEND || mode == WriteMode::ALPHA) {
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

namespace detail {

void fillSpanFmt(PixelFormat fmt, uint8_t *line, int x, int n,
                 const Paint &p) {
  withFormat(fmt, [&](auto tag) {
    fillSpanT<decltype(tag)::value>(line, x, n, p);
  });
}

void readColorsFmt(PixelFormat fmt, const uint8_t *line, int x, int n,
                   Color *out) {
  if (!isFormatEnabled(fmt)) {
    for (int i = 0; i < n; i++) out[i] = Colors::TRANSPARENT;
    return;
  }
  withFormat(fmt, [&](auto tag) {
    readColorsT<decltype(tag)::value>(line, x, n, out);
  });
}

void plotRaw(const Surface &target, int x, int y, const Paint &p) {
  withFormat(target.format, [&](auto tag) {
    constexpr PixelFormat F = decltype(tag)::value;
    typename FormatTraits<F>::Cursor cur;
    cur.init(target.linePtr(y), x);
    if (p.op == PaintOp::FILL)
      cur.write(p.native);
    else if (BLEND && p.op == PaintOp::ADD)
      cur.write(addNative<F>(cur.read(), p.native));
    else
      cur.write(blendNative<F>(cur.read(), p.native, p.alpha64));
  });
}

void writeColorsFmt(PixelFormat fmt, uint8_t *line, int x, int n,
                    const Color *src, WriteMode mode, uint32_t opacity64) {
  withFormat(fmt, [&](auto tag) {
    writeColorsT<decltype(tag)::value>(line, x, n, src, mode, opacity64);
  });
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Memory and state

static constexpr size_t stackBytes() {
  return ((size_t)STACK_DEPTH * sizeof(GraphicsState2D) + 7u) & ~(size_t)7u;
}

size_t Graphics2D::arenaBytes(size_t scratchBytes) {
  return stackBytes() + 7u + scratchBytes;  // 7: alignment of the arena
}

bool Graphics2D::init(const Config &cfg, void *arena, size_t arenaSize) {
  (void)cfg;
  if (!arena) return false;
  const uintptr_t p0 = (uintptr_t)arena, p = (p0 + 7u) & ~(uintptr_t)7u;
  const size_t lost = (size_t)(p - p0);
  if (arenaSize < lost + stackBytes()) return false;
  const size_t rest = arenaSize - lost - stackBytes();
  stack_ = (GraphicsState2D *)p;
  stackTop_ = 0;
  scratch_ = (uint8_t *)(p + stackBytes());
  scratchSize_ = (uint32_t)std::min(rest, (size_t)UINT32_MAX);
  scratchTop_ = 0;
  return true;
}

void Graphics2D::deinit() {
  stack_ = nullptr;
  stackTop_ = 0;
  scratch_ = nullptr;
  scratchSize_ = scratchTop_ = 0;
}

void Graphics2D::setTarget(const Surface &target) {
  target_ = target;
  if (!isFormatEnabled(target_.format)) target_.pixels = nullptr;
  // Larger than SHAPOGFX_COORD_BITS allows: treated like a disabled format
  if (target_.width > SHAPOGFX_COORD_MAX || target_.height > SHAPOGFX_COORD_MAX)
    target_.pixels = nullptr;
  resetClipRect();
}

void Graphics2D::setClipRect(const Rect &r) {
  const Rect c = r.normalized().intersect(bounds());
  state_.clipX = (ucoord_t)c.x;
  state_.clipY = (ucoord_t)c.y;
  state_.clipWidth = (ucoord_t)c.width;
  state_.clipHeight = (ucoord_t)c.height;
}

bool Graphics2D::pushState() {
  if (!stack_ || stackTop_ >= STACK_DEPTH) return false;
  std::memcpy((void *)(stack_ + stackTop_), &state_, sizeof(state_));
  stackTop_++;
  return true;
}

void Graphics2D::popState() {
  if (!stack_ || stackTop_ <= 0) return;
  stackTop_--;
  const TextState t = state_.text;
  std::memcpy(&state_, (const void *)(stack_ + stackTop_), sizeof(state_));
  state_.text.cursorX = t.cursorX;
  state_.text.cursorY = t.cursorY;
  state_.text.lineStartX = t.lineStartX;
  setClipRect(clipRect());  // the target may have changed
  updateTransform();
}

void Graphics2D::setState(const GraphicsState2D &s) {
  state_ = s;
  if (!TRANSFORM) state_.transform = affine2f::identity();
  if (!BLEND) {
    state_.blendMode = BlendMode::ALPHA;
    state_.opacity = 255;
  }
  if (!COLOR_KEY) state_.colorKeyEnabled = false;
  setClipRect(clipRect());
  updateTransform();
}

// --- Transform ---------------------------------------------------------------

void Graphics2D::updateTransform() {
  const affine2f &m = state_.transform;
  kind_ = TransformKind::IDENTITY;
  ox_ = oy_ = 0;
  if (!TRANSFORM) return;
  // Rotations by multiples of pi leave sin() residues around 1e-7: those
  // are not worth the rotated code paths
  constexpr float EPS = 1e-6f;
  if (!(std::fabs(m.b) < EPS && std::fabs(m.c) < EPS)) {
    kind_ = TransformKind::AFFINE;
  } else if (m.a == 1.0f && m.d == 1.0f) {
    ox_ = snap(m.tx);
    oy_ = snap(m.ty);
    kind_ = (m.tx == 0.0f && m.ty == 0.0f) ? TransformKind::IDENTITY
                                           : TransformKind::TRANSLATE;
  } else {
    kind_ = TransformKind::SCALE;
  }
}

void Graphics2D::setTransform(const affine2f &m) {
  if (!TRANSFORM) return;
  state_.transform = m;
  updateTransform();
}

void Graphics2D::resetTransform() { setTransform(affine2f::identity()); }

void Graphics2D::applyTransform(const affine2f &m) {
  if (!TRANSFORM) return;
  state_.transform.multiply(m);
  updateTransform();
}

void Graphics2D::translate(float x, float y) {
  if (!TRANSFORM) return;
  affine2f &m = state_.transform;
  m.tx += m.a * x + m.c * y;
  m.ty += m.b * x + m.d * y;
  updateTransform();
}

void Graphics2D::scale(float sx, float sy) {
  if (!TRANSFORM) return;
  affine2f &m = state_.transform;
  m.a *= sx;
  m.b *= sx;
  m.c *= sy;
  m.d *= sy;
  updateTransform();
}

void Graphics2D::rotate(float angle) {
  applyTransform(affine2f::rotation(angle));
}

void Graphics2D::rotate(float angle, float cx, float cy) {
  applyTransform(affine2f::rotation(angle, cx, cy));
}

namespace detail {

void G2Impl::mapPixel(const Graphics2D &g, float x, float y, int &px,
                      int &py) {
  const vec2f v = g.state_.transform.apply(x + 0.5f, y + 0.5f);
  px = snap(v.x - 0.5f);
  py = snap(v.y - 0.5f);
}

Rect G2Impl::mapRectSigned(const Graphics2D &g, const RectF &r) {
  const affine2f &m = g.state_.transform;
  const int x0 = snap(m.a * r.x + m.tx), x1 = snap(m.a * r.right() + m.tx);
  const int y0 = snap(m.d * r.y + m.ty), y1 = snap(m.d * r.bottom() + m.ty);
  return {x0, y0, x1 - x0, y1 - y0};
}

// --- Blend -------------------------------------------------------------------

bool G2Impl::makePaint(const Graphics2D &g, Color c, Paint &p) {
  const PixelFormat f = g.target_.format;
  const GraphicsState2D &s = g.state_;
  if (BLEND && s.blendMode == BlendMode::NONE) {
    p = {colorToNative(f, c), 64, PaintOp::FILL};
    return true;
  }
  uint32_t a = colorAlpha64(c);
  if (BLEND && s.opacity != 255) a = (a * alpha255To64(s.opacity)) >> 6;
  if (a == 0) return false;
  if (BLEND && s.blendMode == BlendMode::ADD) {
    // The color weighted by its alpha, added with saturation
    const Color scaled = makeColor((colorR(c) * a) >> 6, (colorG(c) * a) >> 6,
                                   (colorB(c) * a) >> 6);
    p = {colorToNative(f, scaled), a, PaintOp::ADD};
  } else if (a >= 64) {
    p = {colorToNative(f, c | 0xFF000000u), 64, PaintOp::FILL};
  } else {
    p = {colorToNative(f, c), a, PaintOp::BLEND};
  }
  return true;
}

}  // namespace detail

void Graphics2D::setBlend(BlendMode mode, int opacity) {
  if (!BLEND) return;
  state_.blendMode = mode;
  state_.opacity = (uint8_t)clampInt(0, 255, opacity);
}

// --- Color key -----------------------------------------------------------------

void Graphics2D::setColorKey(Color key) {
  if (!COLOR_KEY) return;
  state_.colorKey = key;
  state_.colorKeyEnabled = true;
}

void Graphics2D::clearColorKey() { state_.colorKeyEnabled = false; }

// ---------------------------------------------------------------------------
// Pixels and rectangles

void Graphics2D::clear(Color c) {
  if (!hasTarget()) return;
  G2Impl::raster(*this).rect(clipRect(),
                             {colorToNative(target_.format, c), 64,
                              PaintOp::FILL});
}

void Graphics2D::setPixel(int x, int y, Color c, bool transformed) {
  if (!hasTarget()) return;
  Paint p;
  if (!G2Impl::makePaintInline(*this, c, p)) return;
  if (transformed) {
    if (kind_ <= TransformKind::TRANSLATE) {
      x += ox_;
      y += oy_;
    } else {
      G2Impl::mapPixel(*this, (float)x, (float)y, x, y);
    }
  }
  if (clipRect().contains(x, y)) plotRaw(target_, x, y, p);
}

Color Graphics2D::getPixel(int x, int y, bool transformed) const {
  if (!hasTarget()) return Colors::TRANSPARENT;
  if (transformed) {
    if (kind_ <= TransformKind::TRANSLATE) {
      x += ox_;
      y += oy_;
    } else {
      G2Impl::mapPixel(*this, (float)x, (float)y, x, y);
    }
  }
  if (!bounds().contains(x, y)) return Colors::TRANSPARENT;
  Color c;
  readColorsFmt(target_.format, target_.linePtr(y), x, 1, &c);
  return c;
}

// A continuous rectangle through the transform (normalized)
static void fillRectF(Graphics2D &g, const RectF &r, const Paint &p) {
  if (!TRANSFORM || G2Impl::kind(g) <= TransformKind::SCALE) {
    G2Impl::raster(g).rect(G2Impl::mapRectSigned(g, r).normalized(), p);
    return;
  }
  const vec2f v[4] = {
      {r.x, r.y}, {r.right(), r.y}, {r.right(), r.bottom()}, {r.x, r.bottom()}};
  G2Impl::fillPolygon(g, nullptr, v, 4, p);
}

void Graphics2D::fillRect(const Rect &r, Color c) {
  if (!hasTarget()) return;
  Paint p;
  if (!G2Impl::makePaintInline(*this, c, p)) return;
  if (kind_ <= TransformKind::TRANSLATE) {
    fillRectRaw(target_, clipRect(), r.normalized().offset(ox_, oy_), p);
    return;
  }
  fillRectF(*this, RectF(r).normalized(), p);
}

void Graphics2D::fillRect(const RectF &r, Color c) {
  if (!hasTarget()) return;
  Paint p;
  if (!G2Impl::makePaint(*this, c, p)) return;
  fillRectF(*this, r.normalized(), p);
}

// The pixels of `outer` outside `inner` (target pixels, normalized)
static void fillFrame(const Raster &ras, const Rect &outer, const Rect &inner,
                      const Paint &p) {
  if (inner.isEmpty()) {
    ras.rect(outer, p);
    return;
  }
  ras.rect({outer.x, outer.y, outer.width, inner.y - outer.y}, p);
  ras.rect({outer.x, inner.bottom(), outer.width, outer.bottom() - inner.bottom()},
           p);
  ras.rect({outer.x, inner.y, inner.x - outer.x, inner.height}, p);
  ras.rect({inner.right(), inner.y, outer.right() - inner.right(), inner.height},
           p);
}

// Outline of a continuous rectangle (normalized), `t` wide inside it
static void drawRectF(Graphics2D &g, const RectF &r, float t, const Paint &p) {
  const RectF in = {r.x + t, r.y + t, r.width - 2.0f * t, r.height - 2.0f * t};
  if (!TRANSFORM || G2Impl::kind(g) <= TransformKind::SCALE) {
    const Rect outer = G2Impl::mapRectSigned(g, r).normalized();
    const Rect inner = in.isEmpty() ? Rect{0, 0, 0, 0}
                                    : G2Impl::mapRectSigned(g, in).normalized();
    fillFrame(G2Impl::raster(g), outer, inner, p);
    return;
  }
  if (in.isEmpty()) {
    fillRectF(g, r, p);
    return;
  }
  // Both outlines as one polygon, joined by an edge walked there and back
  // (the even-odd rule cancels it)
  const vec2f v[10] = {{r.x, r.y},         {r.right(), r.y},
                       {r.right(), r.bottom()}, {r.x, r.bottom()},
                       {r.x, r.y},         {in.x, in.y},
                       {in.x, in.bottom()}, {in.right(), in.bottom()},
                       {in.right(), in.y}, {in.x, in.y}};
  G2Impl::fillPolygon(g, nullptr, v, 10, p);
}

void Graphics2D::drawRect(const Rect &rect, Color c, int thickness) {
  if (!hasTarget() || thickness <= 0) return;
  const Rect r = rect.normalized();
  if (r.isEmpty()) return;
  Paint p;
  if (!G2Impl::makePaint(*this, c, p)) return;
  if (kind_ <= TransformKind::TRANSLATE) {
    const Rect outer = r.offset(ox_, oy_);
    Rect inner = {outer.x + thickness, outer.y + thickness,
                  outer.width - 2 * thickness, outer.height - 2 * thickness};
    if (inner.isEmpty()) inner = {0, 0, 0, 0};
    fillFrame(G2Impl::raster(*this), outer, inner, p);
    return;
  }
  drawRectF(*this, RectF(r), (float)thickness, p);
}

void Graphics2D::drawRect(const RectF &rect, Color c, float thickness) {
  if (!hasTarget() || !(thickness > 0.0f)) return;
  const RectF r = rect.normalized();
  if (r.isEmpty()) return;
  Paint p;
  if (!G2Impl::makePaint(*this, c, p)) return;
  drawRectF(*this, r, thickness, p);
}

void Graphics2D::drawHLine(int x, int y, int w, Color c) {
  if (!TRANSFORM || kind_ <= TransformKind::TRANSLATE) {
    fillRect(x, y, w, 1, c);
    return;
  }
  if (w == 0) return;
  if (w < 0) {
    x += w;
    w = -w;
  }
  drawLine(x, y, x + w - 1, y, c);
}

void Graphics2D::drawVLine(int x, int y, int h, Color c) {
  if (!TRANSFORM || kind_ <= TransformKind::TRANSLATE) {
    fillRect(x, y, 1, h, c);
    return;
  }
  if (h == 0) return;
  if (h < 0) {
    y += h;
    h = -h;
  }
  drawLine(x, y, x, y + h - 1, c);
}

// ---------------------------------------------------------------------------
// Lines

// A segment within +-LINE_SAFE of (ox, oy)
static void drawLineSafe(const Raster &ras, int x0, int y0, int x1, int y1,
                         int ox, int oy, const Paint &p) {
  const Rect &clip = ras.clip;
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
  const int iStart = std::max(x0, iMin), iEnd = std::min(x1, iMax);
  if (iStart > iEnd) return;
  int32_t jf = (int32_t)y0 * 65536 + 0x8000 + slope * (iStart - x0);

  // Group consecutive major-axis steps with the same minor coordinate into
  // runs
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
        for (int r = i; r <= k; r++) plotRaw(ras.target, j + oj, r + oi, p);
      } else {
        ras.spanRaw(j + oj, i + oi, k + oi + 1, p);
      }
    }
    i = k + 1;
  }
}

void detail::drawLineRaw(const Raster &ras, int x0, int y0, int x1, int y1,
                         const Paint &p) {
  const Rect &clip = ras.clip;
  if (clip.isEmpty()) return;
  const int ox = ras.originX(), oy = ras.originY();
  // The clip rectangle, relative
  const int cx0 = clip.x - ox, cx1 = clip.right() - 1 - ox;
  const int cy0 = clip.y - oy, cy1 = clip.bottom() - 1 - oy;
  auto lineSafe = [](int v) { return v >= -LINE_SAFE && v <= LINE_SAFE; };

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
    drawLineSafe(ras, s.x0, s.y0, s.x1, s.y1, ox, oy, p);
  }
}

void Graphics2D::drawLine(int x0, int y0, int x1, int y1, Color c) {
  if (!hasTarget()) return;
  Paint p;
  if (!G2Impl::makePaint(*this, c, p)) return;
  if (kind_ <= TransformKind::TRANSLATE) {
    drawLineRaw(G2Impl::raster(*this), clampInput(x0) + ox_,
                clampInput(y0) + oy_, clampInput(x1) + ox_,
                clampInput(y1) + oy_, p);
    return;
  }
  G2Impl::mapPixel(*this, (float)x0, (float)y0, x0, y0);
  G2Impl::mapPixel(*this, (float)x1, (float)y1, x1, y1);
  drawLineRaw(G2Impl::raster(*this), x0, y0, x1, y1, p);
}

void Graphics2D::drawLine(const vec2f &a, const vec2f &b, Color c) {
  if (!hasTarget()) return;
  Paint p;
  if (!G2Impl::makePaint(*this, c, p)) return;
  int x0, y0, x1, y1;
  G2Impl::mapPixel(*this, a.x, a.y, x0, y0);
  G2Impl::mapPixel(*this, b.x, b.y, x1, y1);
  drawLineRaw(G2Impl::raster(*this), x0, y0, x1, y1, p);
}

void Graphics2D::drawPolyline(const vec2i *pts, int n, Color c) {
  for (int i = 1; i < n; i++) drawLine(pts[i - 1], pts[i], c);
}

void Graphics2D::drawPolyline(const vec2f *pts, int n, Color c) {
  for (int i = 1; i < n; i++) drawLine(pts[i - 1], pts[i], c);
}

void Graphics2D::drawPolygon(const vec2i *pts, int n, Color c) {
  if (n < 2) return;
  drawPolyline(pts, n, c);
  if (n > 2) drawLine(pts[n - 1], pts[0], c);
}

void Graphics2D::drawPolygon(const vec2f *pts, int n, Color c) {
  if (n < 2) return;
  drawPolyline(pts, n, c);
  if (n > 2) drawLine(pts[n - 1], pts[0], c);
}

void Graphics2D::fillPolygon(const vec2i *pts, int n, Color c) {
  if (!hasTarget() || !pts || n < 3) return;
  Paint p;
  if (!G2Impl::makePaint(*this, c, p)) return;
  G2Impl::fillPolygon(*this, pts, nullptr, n, p, true);
}

void Graphics2D::fillPolygon(const vec2f *pts, int n, Color c) {
  if (!hasTarget() || !pts || n < 3) return;
  Paint p;
  if (!G2Impl::makePaint(*this, c, p)) return;
  G2Impl::fillPolygon(*this, nullptr, pts, n, p, true);
}

// ---------------------------------------------------------------------------
// Text

void Graphics2D::setFont(const GFXfont *font) {
  TextState &t = state_.text;
  t.font = font;
  t.ascent = 0;
  t.lineHeight = 0;
  if (!font) return;
  int ascent = 0, maxBottom = 0;
  const int count = font->last - font->first + 1;
  for (int i = 0; i < count; i++) {
    const GFXglyph &g = font->glyph[i];
    const int top = -g.yOffset;  // pixels above the baseline
    if (i == 0 || top > ascent) ascent = top;
    const int bottom = g.yOffset + g.height;  // pixels below the baseline
    if (i == 0 || bottom > maxBottom) maxBottom = bottom;
  }
  t.ascent = (int16_t)ascent;
  t.lineHeight = (int16_t)(ascent + maxBottom);
}

// Metrics of the font without a width
static TextMetrics lineMetrics(const Graphics2D &g) {
  TextMetrics m;
  const TextState &t = g.textState();
  if (!t.font) return m;
  m.ascent = t.ascent;
  m.height = t.lineHeight;
  m.lineAdvance = t.font->yAdvance;
  return m;
}

static void deviceSize(const Graphics2D &g, TextMetrics &m) {
  const affine2f &x = g.transform();
  m.deviceWidth = m.width * std::sqrt(x.a * x.a + x.b * x.b);
  m.deviceHeight = m.height * std::sqrt(x.c * x.c + x.d * x.d);
}

TextMetrics Graphics2D::charMetrics(int code) const {
  TextMetrics m = lineMetrics(*this);
  const GFXfont *f = state_.text.font;
  if (f && code >= f->first && code <= f->last)
    m.width = f->glyph[code - f->first].xAdvance;
  deviceSize(*this, m);
  return m;
}

TextMetrics Graphics2D::textMetrics(const char *str) const {
  TextMetrics m = lineMetrics(*this);
  const GFXfont *f = state_.text.font;
  if (!f) return m;
  int best = 0, w = 0, lines = 1;
  for (const char *p = str ? str : ""; *p; p++) {
    const int code = (unsigned char)*p;
    if (code == '\n') {
      best = std::max(best, w);
      w = 0;
      lines++;
    } else if (code >= f->first && code <= f->last) {
      w += f->glyph[code - f->first].xAdvance;
    }
  }
  m.width = (float)std::max(best, w);
  m.height += (float)(lines - 1) * m.lineAdvance;
  deviceSize(*this, m);
  return m;
}

int Graphics2D::drawChar(int x, int y, int code) {
  const TextState &t = state_.text;
  if (!t.font || code < t.font->first || code > t.font->last) return 0;
  const GFXglyph &g = t.font->glyph[code - t.font->first];
  if (colorA(t.background) != 0)
    fillRect(x, y, g.xAdvance, t.lineHeight, t.background);
  Paint fg;
  if (!hasTarget() || g.width == 0 || g.height == 0 ||
      !G2Impl::makePaint(*this, t.color, fg))
    return g.xAdvance;
  const MaskSource m = {t.font->bitmap, (uint32_t)g.bitmapOffset * 8u,
                        g.width};
  G2Impl::drawMask(*this, m, Rect{0, 0, g.width, g.height}, x + g.xOffset,
                   y + t.ascent + g.yOffset, &fg, nullptr);
  return g.xAdvance;
}

void Graphics2D::drawString(const char *str) {
  TextState &t = state_.text;
  if (!t.font || !str) return;
  for (const char *p = str; *p; p++) {
    if (*p == '\n') {
      t.cursorX = t.lineStartX;
      t.cursorY += t.font->yAdvance;
    } else {
      t.cursorX += drawChar(t.cursorX, t.cursorY, (unsigned char)*p);
    }
  }
}

}  // namespace shapoco::gfx2d
