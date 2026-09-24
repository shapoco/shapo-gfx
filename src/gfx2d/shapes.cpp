// Graphics2D: ellipses, arcs, sectors, rounded rectangles and polygons.

#include "arch.hpp"
#include "internal.hpp"

namespace shapoco::gfx2d {

using namespace detail;

namespace {

// ---------------------------------------------------------------------------
// Shapes described by the horizontal extent [l, r] (inclusive) of each row.
// Filling walks the rows; outlines draw the pixels of a row that are not
// covered by both neighboring rows, plus the row's end pixels.

// Ellipse inscribed in rect (normalized, non-empty), in target pixels
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
        const uint32_t s = arch::isqrt32(m);
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

// Rectangle with elliptical corners rx wide and ry high (0: square corners)
struct RoundRectExtent {
  Rect rect;
  int ry;
  EllipseExtent tl, br;  // corners (tr and bl are mirrors of these)
  RoundRectExtent(const Rect &r, int rx, int ry)
      : rect(r),
        ry(rx > 0 ? ry : 0),
        tl(Rect{r.x, r.y, rx * 2, ry * 2}),
        br(Rect{r.right() - rx * 2, r.bottom() - ry * 2, rx * 2, ry * 2}) {}
  bool operator()(int y, int &l, int &r) const {
    if (y < rect.y || y >= rect.bottom()) return false;
    int cl, cr;
    if (y < rect.y + ry) {
      if (!tl(y, cl, cr)) return false;
      l = cl;
      r = rect.right() - 1 - (cl - rect.x);
    } else if (y >= rect.bottom() - ry) {
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

// Any ellipse: the points c + U cos t + V sin t, in target pixel coordinates
// where pixel (x, y) is centered on (x, y). Each row is solved in float: one
// square root.
struct ConicExtent {
  float cx, cy;
  float a, b, c;  // |inverse of [U V] d|^2 = a dx^2 + 2 b dx dy + c dy^2
  int yLo, yHi;   // rows of the bounding box
  float hx;       // half width of the bounding box

  bool init(const vec2f &center, const vec2f &u, const vec2f &v) {
    cx = center.x;
    cy = center.y;
    const float det = u.x * v.y - v.x * u.y;
    if (!(std::fabs(det) > 1e-6f) || !std::isfinite(det) ||
        !std::isfinite(cx) || !std::isfinite(cy))
      return false;
    const float k = 1.0f / (det * det);
    a = (v.y * v.y + u.y * u.y) * k;
    b = -(v.y * v.x + u.y * u.x) * k;
    c = (v.x * v.x + u.x * u.x) * k;
    hx = std::sqrt(u.x * u.x + v.x * v.x);
    const float hy = std::sqrt(u.y * u.y + v.y * v.y);
    constexpr float LIM = (float)LINE_INPUT_MAX;
    const float y0 = std::ceil(cy - hy), y1 = std::floor(cy + hy);
    if (!(y0 <= y1) || y1 < -LIM || y0 > LIM) return false;
    yLo = (int)std::max(y0, -LIM);
    yHi = (int)std::min(y1, LIM);
    return true;
  }

  // The ends are rounded to half pixels and then down to pixels, like
  // EllipseExtent does, so that a quarter turn gives the same pixels
  bool operator()(int y, int &l, int &r) const {
    if (y < yLo || y > yHi) return false;
    const float dy = (float)y - cy;
    // Rows of the bounding box touch the ellipse: a slightly negative
    // discriminant there is rounding
    const float disc = std::max((b * b - a * c) * dy * dy + a, 0.0f);
    const float s = std::sqrt(disc), m = -b * dy;
    constexpr float LIM = (float)LINE_INPUT_MAX;
    const float lo = 2.0f * (cx + (m - s) / a), hi = 2.0f * (cx + (m + s) / a);
    if (!(lo <= hi) || hi < -LIM || lo > LIM) return false;
    l = floorDiv(roundToInt(std::max(lo, -LIM)), 2);
    r = floorDiv(roundToInt(std::min(hi, LIM)), 2);
    return true;
  }
};

// The angle range of an arc or a sector: the pixels whose direction from the
// center of the ellipse lies within it. Each edge is a half-plane
// a * px + b * py > 0 in coordinates relative to the center (in 1/16
// pixels), with the direction of the edge rounded to integers once; a range
// up to pi is the intersection of the half-plane after the start and the one
// before the end, a larger one the complement of the range from the end to
// the start. Ties are broken as if every pixel were moved by (e, e^2) for an
// infinitesimal e, so that no pixel lies on an edge: two sectors that share
// an angle share the rounded edge and every pixel belongs to one of them
// (the center included).
struct Wedge {
  static constexpr int S = 16;  // subpixels
  bool empty = false, full = false, reflex = false;
  int32_t cxS, cyS;  // center, in 1/S pixels (pixel x is centered on x * S)
  int lim;
  int a0 = 0, b0 = 0, a1 = 0, b1 = 0;

  // dir(t, dx, dy): the direction of the parametric angle t on the target;
  // flip: the transform mirrors (turns clockwise into counterclockwise).
  // size bounds the ellipse's extent in pixels.
  template <typename Dir>
  Wedge(int32_t cxS, int32_t cyS, int size, float start, float end, bool flip,
        Dir &&dir)
      : cxS(cxS), cyS(cyS) {
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
    // An integer length of the directions that keeps b * py (|py| < S times
    // the size) within 2^30
    int mag = 8192;
    while (mag > 1 && (int64_t)mag * S * (size + 2) > (1 << 30)) mag >>= 1;
    lim = (size + 2) * (S / 2);
    auto idir = [&](float t, int &dx, int &dy) {
      float x, y;
      dir(t, x, y);
      const float len = std::sqrt(x * x + y * y);
      if (!(len > 0.0f) || !std::isfinite(len)) {
        dx = mag, dy = 0;
        return;
      }
      const float k = (float)mag / len;
      dx = roundToInt(x * k);
      dy = roundToInt(y * k);
    };
    int sdx, sdy, edx, edy;
    idir(start, sdx, sdy);
    idir(end, edx, edy);
    if (reflex != flip) {
      std::swap(sdx, edx);
      std::swap(sdy, edy);
    }
    // after the start: cross(s, p) > 0; before the end: cross(p, e) > 0
    a0 = -sdy, b0 = sdx;
    a1 = edy, b1 = -edx;
  }

  // Columns [lo, hi] of row pyS on the positive side of a * px + b * py
  void halfRow(int a, int b, int pyS, int &lo, int &hi) const {
    const int k = b * pyS;
    lo = INT_MIN / 2;
    hi = INT_MAX / 2;
    if (a > 0) {
      // a * pxS + k >= 0 (a tie counts as positive)
      const int t = clampInt(-lim, lim, ceilDiv(-k, a));
      lo = ceilDiv(cxS + t, S);
    } else if (a < 0) {
      // -a * pxS < k
      const int t = clampInt(-lim, lim, ceilDiv(k, -a) - 1);
      hi = floorDiv(cxS + t, S);
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
    const int pyS = y * S - cyS;
    int lo0, hi0, lo1, hi1;
    halfRow(a0, b0, pyS, lo0, hi0);
    halfRow(a1, b1, pyS, lo1, hi1);
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

// Where the rows of a shape go: spans clipped to the clip rectangle, and cut
// by a wedge if there is one
struct SpanOut {
  const Raster &ras;
  const Paint &p;
  const Wedge *wedge;
  void operator()(int y, int x0, int x1) const {
    if (!wedge) {
      ras.span(y, x0, x1, p);
      return;
    }
    wedge->clip(y, x0, x1,
                [this](int y, int x0, int x1) { ras.span(y, x0, x1, p); });
  }
};

// Outlines from the extents of consecutive rows, fed from the row above the
// first one to the row below the last one; a row is emitted once the row
// below it is known.
struct OutlineRows {
  const SpanOut &emit;
  int yFirst, yEnd;  // rows [yFirst, yEnd) are emitted
  bool hasPrev = false, has = false;
  int pl = 0, pr = 0, l = 0, r = 0;

  OutlineRows(const SpanOut &emit, int yFirst, int yEnd)
      : emit(emit), yFirst(yFirst), yEnd(yEnd) {}

  void feed(int y, bool hasNext, int nl, int nr) {
    const int yc = y - 1;
    if (has && yc >= yFirst && yc < yEnd) {
      if (!hasPrev || !hasNext) {
        emit(yc, l, r + 1);  // cap row
      } else {
        // The outline of this row has to reach far enough inwards to meet
        // the row above and the row below, or a shape whose edge is nearly
        // flat -- the top and bottom of a circle -- comes out as a dotted
        // line: there the neighbouring rows' ends are many columns away,
        // and drawing only this row's own end pixels leaves the gap between
        // them empty.
        //
        // So each side runs from this row's end to just short of the NEARER
        // of the two neighbours' ends on that side (the narrower row, the
        // one the outline has to bridge to), which is the max on the left
        // and the min on the right. Where the edge is steep the neighbours
        // are a column away and this is the single end pixel it was before.
        // The clamps keep a degenerate extent from painting across the
        // shape.
        const int innerL = std::max(pl, nl), innerR = std::min(pr, nr);
        int le = std::max(l, innerL - 1), rs = std::min(r, innerR + 1);
        if (le > r) le = r;
        if (rs < l) rs = l;
        emit(yc, l, le + 1);
        if (rs > le) emit(yc, rs, r + 1);
      }
    }
    hasPrev = has, pl = l, pr = r;
    has = hasNext, l = nl, r = nr;
  }
};

// Fill or outline a shape over rows [y0, y1), optionally cut by a wedge
template <typename Extent>
void rasterExtent(const Raster &ras, int y0, int y1, const Extent &ext,
                  bool outline, const Wedge *wedge, const Paint &p) {
  y0 = std::max(y0, ras.clip.y);
  y1 = std::min(y1, ras.clip.bottom());
  if (y0 >= y1) return;
  const SpanOut out = {ras, p, wedge};
  int l = 0, r = 0;
  if (!outline) {
    for (int y = y0; y < y1; y++) {
      if (ext(y, l, r)) out(y, l, r + 1);
    }
    return;
  }
  OutlineRows rows(out, y0, y1);
  for (int y = y0 - 1; y <= y1; y++) {
    const bool has = ext(y, l, r);
    rows.feed(y, has, l, r);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Ellipses, arcs and sectors

namespace {

constexpr int WEDGE_MAX = 1 << 24;  // arcs and sectors beyond draw nothing

struct EllipseCall {
  const Rect *ri;    // the rectangle as given: integer,
  const RectF *rf;   // or continuous
  bool outline;
  bool wedge;
  float start, end;
};

void ellipseShape(Graphics2D &g, const EllipseCall &e, Color c) {
  if (!g.hasTarget()) return;
  Paint p;
  if (!G2Impl::makePaint(g, c, p)) return;
  const Raster ras = G2Impl::raster(g);
  const TransformKind kind = G2Impl::kind(g);
  const affine2f &m = G2Impl::matrix(g);

  if (!TRANSFORM || kind <= TransformKind::SCALE) {
    // Still an ellipse inscribed in a rectangle of whole pixels
    Rect r;
    int sgx = 1, sgy = 1;
    if (e.ri && kind <= TransformKind::TRANSLATE) {
      r = e.ri->normalized().offset(G2Impl::offsetX(g), G2Impl::offsetY(g));
    } else {
      r = G2Impl::mapRectSigned(g, (e.ri ? RectF(*e.ri) : *e.rf).normalized());
      if (r.width < 0) sgx = -1;
      if (r.height < 0) sgy = -1;
      r = r.normalized();
    }
    if (r.isEmpty()) return;
    const EllipseExtent ext(r);
    if (!e.wedge) {
      rasterExtent(ras, r.y, r.bottom(), ext, e.outline, nullptr, p);
      return;
    }
    if (std::abs(r.x) > WEDGE_MAX || std::abs(r.y) > WEDGE_MAX ||
        r.width > WEDGE_MAX || r.height > WEDGE_MAX)
      return;
    const float fx = (float)(sgx * std::max(r.width - 1, 1));
    const float fy = (float)(sgy * std::max(r.height - 1, 1));
    const Wedge w((r.x * 2 + r.width - 1) * (Wedge::S / 2),
                  (r.y * 2 + r.height - 1) * (Wedge::S / 2),
                  std::max(r.width, r.height), e.start, e.end, sgx != sgy,
                  [&](float t, float &dx, float &dy) {
                    dx = fx * std::cos(t);
                    dy = fy * std::sin(t);
                  });
    if (w.empty) return;
    rasterExtent(ras, r.y, r.bottom(), ext, e.outline, &w, p);
    return;
  }

  // Turned or sheared: a general ellipse. Its conjugate semi-axes are those
  // of the rectangle through the transform, each shortened by half a pixel,
  // which puts the ends of an unturned one on the centers of the pixels
  // there, like EllipseExtent.
  const RectF f = (e.ri ? RectF(*e.ri) : *e.rf).normalized();
  if (f.isEmpty()) return;
  const vec2f center = m.apply(f.x + f.width * 0.5f, f.y + f.height * 0.5f);
  auto shorten = [](vec2f v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y);
    return len > 0.0f ? v * (std::max(len - 0.5f, 0.5f) / len) : v;
  };
  const vec2f u = shorten(m.applyLinear({f.width * 0.5f, 0.0f}));
  const vec2f v = shorten(m.applyLinear({0.0f, f.height * 0.5f}));
  ConicExtent ext;
  if (!ext.init({center.x - 0.5f, center.y - 0.5f}, u, v)) return;
  if (!e.wedge) {
    rasterExtent(ras, ext.yLo, ext.yHi + 1, ext, e.outline, nullptr, p);
    return;
  }
  const float hy = (float)(ext.yHi - ext.yLo) * 0.5f + 1.0f;
  const float size = 2.0f * std::max(ext.hx, hy) + 2.0f;
  if (!(std::fabs(ext.cx) < (float)WEDGE_MAX) ||
      !(std::fabs(ext.cy) < (float)WEDGE_MAX) || !(size < (float)WEDGE_MAX))
    return;
  const Wedge w(roundToInt(ext.cx * Wedge::S), roundToInt(ext.cy * Wedge::S),
                (int)size, e.start, e.end, m.determinant() < 0.0f,
                [&](float t, float &dx, float &dy) {
                  const float cs = std::cos(t), sn = std::sin(t);
                  dx = u.x * cs + v.x * sn;
                  dy = u.y * cs + v.y * sn;
                });
  if (w.empty) return;
  rasterExtent(ras, ext.yLo, ext.yHi + 1, ext, e.outline, &w, p);
}

}  // namespace

void Graphics2D::fillEllipse(const Rect &r, Color c) {
  ellipseShape(*this, {&r, nullptr, false, false, 0, 0}, c);
}
void Graphics2D::fillEllipse(const RectF &r, Color c) {
  ellipseShape(*this, {nullptr, &r, false, false, 0, 0}, c);
}
void Graphics2D::drawEllipse(const Rect &r, Color c) {
  ellipseShape(*this, {&r, nullptr, true, false, 0, 0}, c);
}
void Graphics2D::drawEllipse(const RectF &r, Color c) {
  ellipseShape(*this, {nullptr, &r, true, false, 0, 0}, c);
}
void Graphics2D::drawArc(const Rect &r, float a0, float a1, Color c) {
  ellipseShape(*this, {&r, nullptr, true, true, a0, a1}, c);
}
void Graphics2D::drawArc(const RectF &r, float a0, float a1, Color c) {
  ellipseShape(*this, {nullptr, &r, true, true, a0, a1}, c);
}
void Graphics2D::fillSector(const Rect &r, float a0, float a1, Color c) {
  ellipseShape(*this, {&r, nullptr, false, true, a0, a1}, c);
}
void Graphics2D::fillSector(const RectF &r, float a0, float a1, Color c) {
  ellipseShape(*this, {nullptr, &r, false, true, a0, a1}, c);
}

// ---------------------------------------------------------------------------
// Rounded rectangles

namespace {

// Segments per quarter circle of `radius` target pixels, so that the chords
// stay within a quarter pixel of the arc
int arcSegments(float radius) {
  if (!(radius > 0.25f)) return 1;
  const float step = 2.0f * std::acos(1.0f - 0.25f / radius);
  const float n = std::ceil(1.5707964f / step);
  return n < 1.0f ? 1 : (n > 16.0f ? 16 : (int)n);
}

// Axis-aligned rounded rectangle r (target pixels) with corners rx x ry
void roundRectRaw(const Raster &ras, const Rect &r, int rx, int ry,
                  bool outline, const Paint &p) {
  if (r.isEmpty()) return;
  if (rx <= 0 || ry <= 0) rx = ry = 0;
  if (!outline && rx == 0) {
    ras.rect(r, p);
    return;
  }
  rasterExtent(ras, r.y, r.bottom(), RoundRectExtent(r, rx, ry), outline,
               nullptr, p);
}

// The Rect API (ri, radius) or the RectF API (rf, radiusF)
void roundRectShape(Graphics2D &g, const Rect *ri, int radius, const RectF *rf,
                    float radiusF, bool outline, Color c) {
  if (!g.hasTarget()) return;
  Paint p;
  if (!G2Impl::makePaint(g, c, p)) return;
  const TransformKind kind = G2Impl::kind(g);
  if (ri && (!TRANSFORM || kind <= TransformKind::TRANSLATE)) {
    // Integer only (no float on a core without an FPU)
    const Rect r = ri->normalized();
    if (r.isEmpty()) return;
    // At most half the shorter side
    const int rad =
        std::max(0, std::min(radius, std::min(r.width, r.height) / 2));
    roundRectRaw(G2Impl::raster(g),
                 r.offset(G2Impl::offsetX(g), G2Impl::offsetY(g)), rad, rad,
                 outline, p);
    return;
  }
  const RectF f = (ri ? RectF(*ri) : *rf).normalized();
  if (f.isEmpty()) return;
  if (ri) radiusF = (float)radius;
  // A circle of the user's coordinates, at most half the shorter side
  radiusF = std::min(radiusF, std::min(f.width, f.height) * 0.5f);
  if (!(radiusF > 0.0f)) radiusF = 0.0f;

  if (!TRANSFORM || kind <= TransformKind::SCALE) {
    const affine2f &m = G2Impl::matrix(g);
    const Rect r = G2Impl::mapRectSigned(g, f).normalized();
    const int rx =
        std::min((int)roundToInt(radiusF * std::fabs(m.a)), r.width / 2);
    const int ry =
        std::min((int)roundToInt(radiusF * std::fabs(m.d)), r.height / 2);
    roundRectRaw(G2Impl::raster(g), r, rx, ry, outline, p);
    return;
  }

  // Turned or sheared: a polygon with the corners made of chords, in the
  // scratch memory if they need more than 4 each
  const affine2f &m = G2Impl::matrix(g);
  const float scale = std::max(std::sqrt(m.a * m.a + m.b * m.b),
                               std::sqrt(m.c * m.c + m.d * m.d));
  int segs = radiusF > 0.0f ? arcSegments(radiusF * scale) : 0;
  G2Impl::ScratchMark mark(g);
  constexpr int LOCAL_SEGS = 4;
  vec2f local[4 * (LOCAL_SEGS + 1)];
  vec2f *v = local;
  if (segs > LOCAL_SEGS) {
    v = (vec2f *)G2Impl::scratchAlloc(g, sizeof(vec2f) * 4 * (segs + 1));
    if (!v) {
      v = local;
      segs = LOCAL_SEGS;
    }
  }
  int n = 0;
  if (segs == 0) {
    v[n++] = {f.x, f.y};
    v[n++] = {f.right(), f.y};
    v[n++] = {f.right(), f.bottom()};
    v[n++] = {f.x, f.bottom()};
  } else {
    const float x0 = f.x + radiusF, x1 = f.right() - radiusF;
    const float y0 = f.y + radiusF, y1 = f.bottom() - radiusF;
    const vec2f centers[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    constexpr float HALF_PI = 1.5707964f;
    for (int k = 0; k < 4; k++) {
      // top-left from pi to 3 pi / 2, and on clockwise
      const float a0 = (float)(k + 2) * HALF_PI;
      for (int i = 0; i <= segs; i++) {
        const float a = a0 + HALF_PI * (float)i / (float)segs;
        v[n++] = {centers[k].x + radiusF * std::cos(a),
                  centers[k].y + radiusF * std::sin(a)};
      }
    }
  }
  if (outline)
    G2Impl::outlineConvex(g, v, n, p);
  else
    G2Impl::fillPolygon(g, nullptr, v, n, p);
}

}  // namespace

void Graphics2D::fillRoundRect(const Rect &r, int radius, Color c) {
  roundRectShape(*this, &r, radius, nullptr, 0.0f, false, c);
}
void Graphics2D::fillRoundRect(const RectF &r, float radius, Color c) {
  roundRectShape(*this, nullptr, 0, &r, radius, false, c);
}
void Graphics2D::drawRoundRect(const Rect &r, int radius, Color c) {
  roundRectShape(*this, &r, radius, nullptr, 0.0f, true, c);
}
void Graphics2D::drawRoundRect(const RectF &r, float radius, Color c) {
  roundRectShape(*this, nullptr, 0, &r, radius, true, c);
}

// ---------------------------------------------------------------------------
// Polygons
//
// Vertices are taken in 1/16 pixels relative to the center of the clip
// rectangle and clamped to +-LINE_SAFE pixels of it. A pixel is inside where
// its center is (the even-odd rule, an edge covering the centers of rows
// y0 <= y < y1 and the pixels whose center is at or right of it). Each edge
// finds its column on its first row with one division and then steps
// exactly, with an integer and a remainder (a DDA), so that edges shared by
// two polygons give both the same columns.

namespace {

constexpr int SUB = 16;  // subpixels
constexpr int32_t SUB_MAX = LINE_SAFE * SUB;

struct SubVertex {
  int32_t x, y;
};

struct PolyEdge {
  int16_t yTop, yEnd;  // rows [yTop, yEnd), relative
  int32_t c, e;        // column of the current row, and the remainder
  int32_t q, r, den;   // per row: c += q, e -= r, carrying den
};

// First column on or right of edge a-b (a above b) on row y, which the edge
// crosses; e receives the remainder (0 <= e < den)
int32_t crossingAt(const SubVertex &a, const SubVertex &b, int y,
                   int32_t &e) {
  const int32_t dx = b.x - a.x, dy = b.y - a.y, den = dy * SUB;
  const int32_t ax = a.x >> 4, af = a.x & (SUB - 1);
  // x(y) = a.x + (Y - a.y) dx / dy at the row center Y; the column is
  // ceil((x - SUB / 2) / SUB) = ax + ceil(num / den)
  const int32_t off = y * SUB + SUB / 2 - a.y;  // >= 0
  if (off < SUB) {
    // The first row of the edge: |num| < 2^24
    const int32_t num = (af - SUB / 2) * dy + off * dx;
    const int32_t k = ceilDiv(num, den);
    e = k * den - num;
    return ax + k;
  }
  const int64_t num = (int64_t)(af - SUB / 2) * dy + (int64_t)off * dx;
  int64_t k = num / den;
  if (num % den > 0) k++;
  e = (int32_t)(k * den - num);
  return ax + (int32_t)k;
}

// The vertices of a polygon: integer ones under a translation (the fast
// path), or either kind through the transform, with the origin subtracted.
// The vertices of the polygon API are pixels like the end points of lines:
// each lands on the pixel its center is transformed into (as in drawLine()),
// and the edges run through the centers of those pixels, so that the fill
// stays within the outline drawPolygon() draws. The polygons of areas
// (rectangles and the like) have continuous vertices.
struct Vertices {
  const vec2i *pi;
  const vec2f *pf;
  bool translated;  // pi only: add dx, dy
  int dx, dy;
  int32_t half;     // translated: SUB / 2 for pixel vertices
  affine2f m;

  static int32_t sub(float v) {
    v *= (float)SUB;
    if (!(v > (float)-SUB_MAX)) return -SUB_MAX;
    if (!(v < (float)SUB_MAX)) return SUB_MAX;
    return roundToInt(v);
  }
  SubVertex operator()(int i) const {
    if (translated) {
      auto cl = [](int v) {
        return clampInt(-LINE_SAFE, LINE_SAFE, v) * SUB;
      };
      return {cl(clampInput(pi[i].x) + dx) + half,
              cl(clampInput(pi[i].y) + dy) + half};
    }
    const vec2f v = pi ? vec2f{(float)pi[i].x, (float)pi[i].y} : pf[i];
    const float x = m.a * v.x + m.c * v.y + m.tx;
    const float y = m.b * v.x + m.d * v.y + m.ty;
    if (half) {
      auto px = [this](float c) {
        return clampInt(-LINE_SAFE, LINE_SAFE, snap(c - 0.5f)) * SUB + half;
      };
      return {px(x), px(y)};
    }
    return {sub(x), sub(y)};
  }

  Vertices(const Graphics2D &g, const Raster &ras, const vec2i *pi,
           const vec2f *pf, bool pixels)
      : pi(pi), pf(pf), m(G2Impl::matrix(g)) {
    const int ox = ras.originX(), oy = ras.originY();
    translated = pi && G2Impl::kind(g) <= TransformKind::TRANSLATE;
    dx = G2Impl::offsetX(g) - ox;
    dy = G2Impl::offsetY(g) - oy;
    half = pixels ? SUB / 2 : 0;
    // A pixel vertex (x, y) stands for the point (x + 0.5, y + 0.5)
    const float h = pixels ? 0.5f : 0.0f;
    m.tx += (m.a + m.c) * h - (float)ox;
    m.ty += (m.b + m.d) * h - (float)oy;
  }
};

// What a scan does with each row: xs[0..m) are the sorted columns where the
// edges cross row y (relative to the origin)
struct RowSink {
  void (*fn)(void *ctx, int y, const int32_t *xs, int m);
  void *ctx;
  template <typename F>
  static RowSink of(F &f) {
    return {[](void *c, int y, const int32_t *xs, int m) { (*(F *)c)(y, xs, m); },
            &f};
  }
  void operator()(int y, const int32_t *xs, int m) const { fn(ctx, y, xs, m); }
};

// Call row() for each row in [rowLo, rowHi] the polygon covers, from the
// top. The edges live on the stack up to LOCAL_EDGES, else in the scratch
// memory; without either, every row evaluates every edge from its vertices.
__attribute__((noinline)) void scanPolygon(Graphics2D &g, int n,
                                           const Vertices &vtx, int rowLo,
                                           int rowHi, RowSink row) {
  constexpr int LOCAL_EDGES = 12, MAX_CROSSES = 32;
  G2Impl::ScratchMark mark(g);
  PolyEdge local[LOCAL_EDGES];
  PolyEdge *edges =
      n <= LOCAL_EDGES
          ? local
          : (PolyEdge *)G2Impl::scratchAlloc(g, sizeof(PolyEdge) * (size_t)n);
  int32_t xs[MAX_CROSSES];
  auto insert = [&](int &m, int32_t x) {
    if (m >= MAX_CROSSES) return;
    int k = m++;
    while (k > 0 && xs[k - 1] > x) {
      xs[k] = xs[k - 1];
      k--;
    }
    xs[k] = x;
  };
  // The edge from vertex i - 1 to vertex i, top first, and the rows whose
  // centers it crosses: [top, end)
  auto edge = [](SubVertex &a, SubVertex &b, int &top, int &end) {
    if (a.y > b.y) std::swap(a, b);
    top = ceilDiv(a.y - SUB / 2, SUB);
    end = ceilDiv(b.y - SUB / 2, SUB);
    return top < end;
  };

  if (!edges) {
    int yMin = INT_MAX, yMax = INT_MIN;
    for (int i = 0; i < n; i++) {
      const int y = vtx(i).y;
      yMin = std::min(yMin, y);
      yMax = std::max(yMax, y);
    }
    const int y0 = std::max(ceilDiv(yMin - SUB / 2, SUB), rowLo);
    const int y1 = std::min(ceilDiv(yMax - SUB / 2, SUB) - 1, rowHi);
    for (int y = y0; y <= y1; y++) {
      int m = 0;
      SubVertex p = vtx(n - 1);
      for (int i = 0; i < n; i++) {
        SubVertex a = p, b = vtx(i);
        p = b;
        int top, end;
        if (!edge(a, b, top, end) || y < top || y >= end) continue;
        int32_t e;
        insert(m, crossingAt(a, b, y, e));
      }
      row(y, xs, m);
    }
    return;
  }

  // The edges' rows and end points (kept in c, e, q, r until the first row
  // is known)
  int count = 0, yMin = INT_MAX, yMax = INT_MIN;
  SubVertex p = vtx(n - 1);
  for (int i = 0; i < n; i++) {
    SubVertex a = p, b = vtx(i);
    p = b;
    int top, end;
    if (!edge(a, b, top, end)) continue;
    PolyEdge &ed = edges[count++];
    ed.yTop = (int16_t)top;
    ed.yEnd = (int16_t)end;
    ed.c = a.x, ed.e = a.y, ed.q = b.x, ed.r = b.y;
    yMin = std::min(yMin, top);
    yMax = std::max(yMax, end - 1);
  }
  const int y0 = std::max(yMin, rowLo), y1 = std::min(yMax, rowHi);
  if (y0 > y1) return;
  for (int k = 0; k < count; k++) {
    PolyEdge &ed = edges[k];
    const SubVertex a = {ed.c, ed.e}, b = {ed.q, ed.r};
    const int top = std::max((int)ed.yTop, y0);
    if (top >= ed.yEnd) {
      ed.yEnd = ed.yTop;  // never reached
      continue;
    }
    ed.yTop = (int16_t)top;
    ed.c = crossingAt(a, b, top, ed.e);
    ed.den = (b.y - a.y) * SUB;
    const int32_t inc = (b.x - a.x) * SUB;  // per row: |inc| < 2^24
    ed.q = floorDiv(inc, ed.den);
    ed.r = inc - ed.q * ed.den;
  }
  for (int y = y0; y <= y1; y++) {
    int m = 0;
    for (int k = 0; k < count; k++) {
      PolyEdge &ed = edges[k];
      if (y < ed.yTop || y >= ed.yEnd) continue;
      insert(m, ed.c);
      ed.c += ed.q;
      ed.e -= ed.r;
      if (ed.e < 0) {
        ed.c++;
        ed.e += ed.den;
      }
    }
    row(y, xs, m);
  }
}

}  // namespace

void detail::G2Impl::fillPolygon(Graphics2D &g, const vec2i *pi,
                                 const vec2f *pf, int n, const Paint &p,
                                 bool pixels) {
  if (n < 3) return;
  const Raster ras = raster(g);
  if (ras.clip.isEmpty()) return;
  const int ox = ras.originX(), oy = ras.originY();
  auto row = [&](int y, const int32_t *xs, int m) {
    for (int i = 0; i + 1 < m; i += 2)
      ras.spanRaw(y + oy, std::max((int)xs[i] + ox, ras.clip.x),
                  std::min((int)xs[i + 1] + ox, ras.clip.right()), p);
  };
  scanPolygon(g, n, Vertices(g, ras, pi, pf, pixels), ras.clip.y - oy,
              ras.clip.bottom() - 1 - oy, RowSink::of(row));
}

void detail::G2Impl::outlineConvex(Graphics2D &g, const vec2f *pts, int n,
                                   const Paint &p) {
  if (n < 3) return;
  const Raster ras = raster(g);
  if (ras.clip.isEmpty()) return;
  const int ox = ras.originX(), oy = ras.originY();
  const SpanOut out = {ras, p, nullptr};
  OutlineRows rows(out, ras.clip.y, ras.clip.bottom());
  // The extent of a row: from the leftmost to the rightmost crossing. The
  // rows next to the clip rectangle are scanned too, as neighbors.
  int next = INT_MIN;  // the row to feed next (absolute)
  auto row = [&](int y, const int32_t *xs, int k) {
    y += oy;
    if (next == INT_MIN) rows.feed(y - 1, false, 0, 0);
    const bool has = k >= 2 && xs[0] < xs[k - 1];
    rows.feed(y, has, has ? xs[0] + ox : 0, has ? xs[k - 1] - 1 + ox : 0);
    next = y + 1;
  };
  scanPolygon(g, n, Vertices(g, ras, nullptr, pts, false), ras.clip.y - 1 - oy,
              ras.clip.bottom() - oy, RowSink::of(row));
  if (next != INT_MIN) rows.feed(next, false, 0, 0);
}

}  // namespace shapoco::gfx2d
