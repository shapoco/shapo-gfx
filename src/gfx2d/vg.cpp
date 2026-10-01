// Vector graphics (vg.hpp): paths flattened into edges and filled by a
// scanline with the nonzero or even-odd rule, antialiased by coverage,
// through brushes (a color or a gradient); strokes outlined into polygons
// and filled the same way; pictures.
//
// The scanline works on sub-rows: AA_ROWS per pixel row with antialiasing
// (one without), each sampled at its center, with the columns in 1/16 pixel
// like the polygons of shapes.cpp. The coverage of a pixel is the sum over
// its sub-rows of the 1/16 columns inside, 0..64, which is directly the
// alpha64 of the blend. Edges are kept in the scratch memory of the arena
// (or a few on the stack) and a path that does not fit is drawn in parts.

#include "shapoco/gfx2d/vg.hpp"

#include "arch.hpp"
#include "internal.hpp"

namespace shapoco::gfx2d {

using namespace detail;

// ---------------------------------------------------------------------------
// Path helpers

namespace vg {

RectF pathBounds(const Path &p) {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool any = false;
  const float *c = p.coords;
  int left = p.coordCount;
  for (int i = 0; i < p.opCount; i++) {
    const int n = pathOpCoords((PathOp)p.ops[i]);
    if (n > left) break;
    for (int k = 0; k < n; k += 2) {
      const float x = c[k], y = c[k + 1];
      if (!any) {
        x0 = x1 = x;
        y0 = y1 = y;
        any = true;
      } else {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
      }
    }
    c += n;
    left -= n;
  }
  return any ? RectF{x0, y0, x1 - x0, y1 - y0} : RectF{0, 0, 0, 0};
}

PathBuilder &PathBuilder::op(PathOp o, float a, float b, float c, float d,
                             float e, float f) {
  const int n = pathOpCoords(o);
  if (nOps_ >= opCap_ || nCoords_ + n > coordCap_) {
    overflow_ = true;
    return *this;
  }
  ops_[nOps_++] = (uint8_t)o;
  const float v[6] = {a, b, c, d, e, f};
  for (int i = 0; i < n; i++) coords_[nCoords_++] = v[i];
  return *this;
}

PathBuilder &PathBuilder::rect(float x, float y, float w, float h) {
  moveTo(x, y).lineTo(x + w, y).lineTo(x + w, y + h).lineTo(x, y + h);
  return close();
}

// A quarter circle as a cubic: the control points at KAPPA of the radius
static constexpr float KAPPA = 0.5522847f;

PathBuilder &PathBuilder::roundRect(float x, float y, float w, float h,
                                    float rx, float ry) {
  rx = std::min(std::fabs(rx), std::fabs(w) * 0.5f);
  ry = std::min(std::fabs(ry), std::fabs(h) * 0.5f);
  if (!(rx > 0.0f) || !(ry > 0.0f)) return rect(x, y, w, h);
  const float kx = rx * KAPPA, ky = ry * KAPPA;
  const float x1 = x + w, y1 = y + h;
  moveTo(x + rx, y);
  lineTo(x1 - rx, y);
  cubicTo(x1 - rx + kx, y, x1, y + ry - ky, x1, y + ry);
  lineTo(x1, y1 - ry);
  cubicTo(x1, y1 - ry + ky, x1 - rx + kx, y1, x1 - rx, y1);
  lineTo(x + rx, y1);
  cubicTo(x + rx - kx, y1, x, y1 - ry + ky, x, y1 - ry);
  lineTo(x, y + ry);
  cubicTo(x, y + ry - ky, x + rx - kx, y, x + rx, y);
  return close();
}

PathBuilder &PathBuilder::ellipse(float cx, float cy, float rx, float ry) {
  const float kx = rx * KAPPA, ky = ry * KAPPA;
  moveTo(cx + rx, cy);
  cubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
  cubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
  cubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
  cubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
  return close();
}

PathBuilder &PathBuilder::arc(float cx, float cy, float rx, float ry,
                              float a0, float a1) {
  constexpr float TWO_PI = 6.2831853f;
  float sweep = a1 - a0;
  if (sweep >= TWO_PI) {
    sweep = TWO_PI;
  } else {
    sweep = std::fmod(sweep, TWO_PI);
    if (sweep < 0.0f) sweep += TWO_PI;
  }
  const float sx = cx + rx * std::cos(a0), sy = cy + ry * std::sin(a0);
  if (nOps_ == 0) moveTo(sx, sy);
  else lineTo(sx, sy);
  if (!(sweep > 0.0f)) return *this;
  // Quarter turns at most per cubic
  const int n = (int)std::ceil(sweep / (TWO_PI / 4.0f) - 1e-4f);
  const float step = sweep / (float)std::max(n, 1);
  const float k = 4.0f / 3.0f * std::tan(step / 4.0f);
  float a = a0;
  for (int i = 0; i < std::max(n, 1); i++) {
    const float b = a + step;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b),
                sb = std::sin(b);
    cubicTo(cx + rx * (ca - k * sa), cy + ry * (sa + k * ca),
            cx + rx * (cb + k * sb), cy + ry * (sb - k * cb), cx + rx * cb,
            cy + ry * sb);
    a = b;
  }
  return *this;
}

PathBuilder &PathBuilder::polyline(const vec2f *pts, int n, bool closed) {
  if (n <= 0) return *this;
  moveTo(pts[0].x, pts[0].y);
  for (int i = 1; i < n; i++) lineTo(pts[i].x, pts[i].y);
  if (closed) close();
  return *this;
}

Path PathBuilder::path(FillRule rule) const {
  Path p = {ops_, coords_, (uint16_t)nOps_, (uint16_t)nCoords_, rule,
            {0, 0, 0}, RectF{0, 0, 0, 0}};
  p.bounds = pathBounds(p);
  return p;
}

Gradient linearGradient(const vec2f &p0, const vec2f &p1,
                        const GradientStop *stops, int stopCount,
                        Spread spread) {
  Gradient g = {GradientKind::LINEAR, spread, (uint8_t)clampInt(0, 255, stopCount),
                0, stops, affine2f::identity()};
  // t = (p - p0) . d / |d|^2; the second row is unused
  const float dx = p1.x - p0.x, dy = p1.y - p0.y;
  const float l2 = dx * dx + dy * dy;
  const float k = l2 > 0.0f ? 1.0f / l2 : 0.0f;
  g.toGradient = {dx * k, 0, dy * k, 0, -(p0.x * dx + p0.y * dy) * k, 0};
  return g;
}

Gradient radialGradient(const vec2f &center, float radius,
                        const GradientStop *stops, int stopCount,
                        Spread spread) {
  Gradient g = {GradientKind::RADIAL, spread, (uint8_t)clampInt(0, 255, stopCount),
                0, stops, affine2f::identity()};
  const float k = radius > 0.0f ? 1.0f / radius : 0.0f;
  g.toGradient = {k, 0, 0, k, -center.x * k, -center.y * k};
  return g;
}

}  // namespace vg

// ---------------------------------------------------------------------------
// Edges and the scanline

namespace {

using namespace vg;

constexpr int SUB = 16;  // columns per pixel
constexpr int32_t SUB_MAX = LINE_SAFE * SUB;
#if SHAPOGFX2D_ANTIALIAS
constexpr int AA_ROWS = 4;  // sub-rows per pixel row with antialiasing
#else
constexpr int AA_ROWS = 1;
#endif
constexpr int COV_FULL = 64;  // coverage of a whole pixel (= alpha64 opaque)
static_assert(AA_ROWS * SUB == COV_FULL || AA_ROWS == 1,
              "the coverage of a pixel must come to 64");
constexpr int X_FRAC = 10;  // fraction bits of Edge::x below the column
constexpr int MAX_CROSSES = 64;  // crossings per sub-row (the rest dropped)
constexpr int STACK_EDGES = 24;  // without an arena
constexpr int LOOP_RESERVE = 40;  // edges kept free for the next loop
constexpr int STACK_COV = 128;   // pixels of coverage without an arena
constexpr int LUT_SIZE = 64;     // colors of a gradient
constexpr int CHUNK = 32;        // pixels of gradient colors made at once

struct Edge {
  int32_t x;   // column (1/16 px, from the clip center) << X_FRAC, on the
               // sub-row the scan is at
  int32_t dx;  // per sub-row
  int16_t yTop, yEnd;  // sub-rows [yTop, yEnd) whose centers it crosses
  int8_t dir;          // +1 going down the screen, -1 up
  uint8_t pad[3];
};

struct Crossing {
  int32_t x;  // Edge::x: the column in 1/16 px << X_FRAC
  int dir;
};

// Where a drawing call puts its pixels: the target with the blend of the
// state, through a solid paint or a gradient
struct Sink {
  Raster ras;
  PixelFormat fmt;
  BlendMode blend;
  uint32_t opacity64;
  bool aa;
  // Solid
  bool solid;
  Color color;
  Paint paint;       // of `color` under the blend
  uint32_t alpha64;  // of `color` x opacity
  uint32_t native;   // of `color`, opaque, in the target's format
  // Gradient: pixel (x + 0.5, y + 0.5) -> (u, v) of the gradient space
  const Gradient *grad;
  float gA, gB, gC, gD, gTx, gTy;
  Color lut[LUT_SIZE];

  bool init(const Graphics2D &g, const Brush &brush, const affine2f &m,
            bool antialias);
  // A whole span [x0, x1) of row y (clipped already)
  void span(int y, int x0, int x1) const;
  // Row y from x0 on, with the coverage (0..64) of n pixels
  void covered(int y, int x0, const int16_t *cov, int n) const;
  // The paint of the solid color at a partial coverage (0..64)
  bool paintAt(uint32_t cov, Paint &p) const;

 private:
  void gradientColors(int x, int y, int n, const int16_t *cov,
                      Color *out) const;
  void writeColors(int y, int x, int n, const Color *c) const {
    const WriteMode mode = blend == BlendMode::NONE  ? WriteMode::COPY
                           : blend == BlendMode::ADD ? WriteMode::ADD
                                                     : WriteMode::ALPHA;
    writeColorsFmt(fmt, ras.target.linePtr(y), x, n, c, mode, opacity64);
  }
};

bool Sink::init(const Graphics2D &g, const Brush &brush, const affine2f &m,
                bool antialias) {
  ras = G2Impl::raster(g);
  fmt = g.format();
  blend = g.blendMode();
  opacity64 = alpha255To64((uint32_t)g.opacity());
  aa = ANTIALIAS && antialias && blend != BlendMode::NONE;
  grad = brush.gradient;
  solid = grad == nullptr;
  if (solid) {
    color = brush.color;
    if (!G2Impl::makePaint(g, color, paint)) return false;
    alpha64 = (colorAlpha64(color) * opacity64) >> 6;
    native = colorToNative(fmt, color | 0xFF000000u);
    return true;
  }
  if (grad->stopCount == 0 || !grad->stops) return false;
  const uint32_t ba = (uint32_t)colorA(brush.color);
  if (ba == 0 && blend != BlendMode::NONE) return false;
  // Colors at (i + 0.5) / LUT_SIZE
  const GradientStop *st = grad->stops;
  const int ns = grad->stopCount;
  for (int i = 0; i < LUT_SIZE; i++) {
    const float t = ((float)i + 0.5f) / (float)LUT_SIZE;
    Color c;
    if (t <= st[0].offset) {
      c = st[0].color;
    } else if (t >= st[ns - 1].offset) {
      c = st[ns - 1].color;
    } else {
      int k = 0;
      while (k + 1 < ns && st[k + 1].offset < t) k++;
      const float o0 = st[k].offset, o1 = st[k + 1].offset;
      const float f = o1 > o0 ? (t - o0) / (o1 - o0) : 1.0f;
      c = lerpColor(st[k].color, st[k + 1].color,
                    clampInt(0, 256, (int)(f * 256.0f + 0.5f)));
    }
    if (ba != 255) c = colorWithAlpha(c, (int)(((uint32_t)colorA(c) * ba + 127u) / 255u));
    lut[i] = c;
  }
  affine2f inv;
  if (!m.invert(inv)) return false;
  const affine2f gm = grad->toGradient * inv;
  gA = gm.a, gB = gm.b, gC = gm.c, gD = gm.d, gTx = gm.tx, gTy = gm.ty;
  return true;
}

bool Sink::paintAt(uint32_t cov, Paint &p) const {
  if (cov >= COV_FULL) {
    p = paint;
    return true;
  }
  const uint32_t a = (alpha64 * cov + COV_FULL / 2) / COV_FULL;
  if (a == 0) return false;
  if (BLEND && blend == BlendMode::ADD) {
    const Color scaled = makeColor((colorR(color) * (int)a) >> 6,
                                   (colorG(color) * (int)a) >> 6,
                                   (colorB(color) * (int)a) >> 6);
    p = {colorToNative(fmt, scaled), a, PaintOp::ADD};
    return true;
  }
  p = {native, a, a >= 64 ? PaintOp::FILL : PaintOp::BLEND};
  return true;
}

// The LUT index of a Q16 position along the gradient
static inline int lutIndex(int32_t t16, Spread spread) {
  if (spread == Spread::REPEAT) return (t16 & 0xFFFF) >> 10;
  if (spread == Spread::REFLECT) {
    int32_t w = t16 & 0x1FFFF;
    if (w > 0xFFFF) w = std::min<int32_t>(0x20000 - w, 0xFFFF);
    return w >> 10;
  }
  return t16 < 0 ? 0 : (t16 > 0xFFFF ? LUT_SIZE - 1 : t16 >> 10);
}

static inline int32_t clampQ(float v) {
  constexpr float LIM = 1073741824.0f;  // 2^30
  return v < -LIM ? (int32_t)-LIM : (v > LIM ? (int32_t)LIM : (int32_t)v);
}

void Sink::gradientColors(int x, int y, int n, const int16_t *cov,
                          Color *out) const {
  const float px = (float)x + 0.5f, py = (float)y + 0.5f;
  // Q16, from the exact start of the chunk (the step's rounding does not
  // accumulate across chunks)
  int32_t u = clampQ((gA * px + gC * py + gTx) * 65536.0f);
  const int32_t du = clampQ(gA * 65536.0f);
  const Spread spread = grad->spread;
  if (grad->kind == GradientKind::LINEAR) {
    for (int i = 0; i < n; i++) {
      out[i] = lut[lutIndex(u, spread)];
      u += du;
    }
  } else {
    int32_t v = clampQ((gB * px + gD * py + gTy) * 65536.0f);
    const int32_t dv = clampQ(gB * 65536.0f);
    for (int i = 0; i < n; i++) {
      // Q12 components, within 8 radii, so that the squares fit 32 bits
      const int32_t u12 = clampInt(-32767, 32767, u >> 4);
      const int32_t v12 = clampInt(-32767, 32767, v >> 4);
      const uint32_t r12 =
          arch::isqrt32((uint32_t)(u12 * u12) + (uint32_t)(v12 * v12));
      out[i] = lut[lutIndex((int32_t)r12 << 4, spread)];
      u += du;
      v += dv;
    }
  }
  if (cov) {
    for (int i = 0; i < n; i++) {
      const uint32_t c = (uint32_t)cov[i];
      if (c >= COV_FULL) continue;
      out[i] = colorWithAlpha(out[i], (int)(((uint32_t)colorA(out[i]) * c +
                                           COV_FULL / 2) / COV_FULL));
    }
  }
}

void Sink::span(int y, int x0, int x1) const {
  if (x1 <= x0) return;
  if (solid) {
    ras.spanRaw(y, x0, x1, paint);
    return;
  }
  Color buf[CHUNK];
  for (int x = x0; x < x1; x += CHUNK) {
    const int n = std::min(CHUNK, x1 - x);
    gradientColors(x, y, n, nullptr, buf);
    writeColors(y, x, n, buf);
  }
}

void Sink::covered(int y, int x0, const int16_t *cov, int n) const {
  if (solid) {
    // Runs of equal coverage (long where the row is inside the shape)
    int i = 0;
    while (i < n) {
      const int16_t c = cov[i];
      int j = i + 1;
      while (j < n && cov[j] == c) j++;
      Paint p;
      if (c > 0 && paintAt((uint32_t)c, p)) ras.spanRaw(y, x0 + i, x0 + j, p);
      i = j;
    }
    return;
  }
  Color buf[CHUNK];
  int i = 0;
  while (i < n) {
    while (i < n && cov[i] <= 0) i++;
    if (i >= n) break;
    int j = i;
    while (j < n && j - i < CHUNK && cov[j] > 0) j++;
    gradientColors(x0 + i, y, j - i, cov + i, buf);
    writeColors(y, x0 + i, j - i, buf);
    i = j;
  }
}

// Scans a set of edges: the sub-rows [rowLo, rowHi] from the top, the
// crossings of each sorted, the spans by the fill rule, into the Sink
// directly (one sub-row per pixel row) or through the coverage buffer.
struct Scan {
  const Sink &sink;
  FillRule rule;
  int ox, oy;        // the clip center (the origin of columns and rows)
  int rows;          // sub-rows per pixel row (AA_ROWS with coverage, else 1)
  int rowLo, rowHi;  // sub-rows
  int16_t *cov;      // coverage deltas of columns [covX0, covX1), or nullptr
  int covX0, covX1;

  void run(Edge *edges, int n, int16_t *active) const;
  void subRow(int r, Crossing *xs, int m) const;
  void addSpan(int r, int32_t xa, int32_t xb) const;
  void flushRow(int y) const;
};

void Scan::addSpan(int r, int32_t xa, int32_t xb) const {
  // xa, xb: Edge::x values from the origin, [xa, xb)
  if (xb <= xa) return;
  if (!cov) {
    // The pixels whose center is inside: ceil((x - 8) / 16), exactly
    constexpr int32_t HALF = (SUB / 2) << X_FRAC, PIX = SUB << X_FRAC;
    const int x0 = ((xa - HALF + PIX - 1) >> (X_FRAC + 4)) + ox;
    const int x1 = ((xb - HALF + PIX - 1) >> (X_FRAC + 4)) + ox;
    sink.span(r + oy, std::max(x0, sink.ras.clip.x),
              std::min(x1, sink.ras.clip.right()));
    return;
  }
  (void)r;
  // Coverage of the columns (the nearest 1/16), as deltas
  constexpr int32_t ROUND = 1 << (X_FRAC - 1);
  xa = (xa + ROUND) >> X_FRAC;
  xb = (xb + ROUND) >> X_FRAC;
  if (xb <= xa) return;
  int32_t a = xa + ox * SUB - covX0 * SUB, b = xb + ox * SUB - covX0 * SUB;
  const int32_t lim = (covX1 - covX0) * SUB;
  if (a < 0) a = 0;
  if (b > lim) b = lim;
  if (b <= a) return;
  const int pa = a >> 4, fa = a & 15, pb = b >> 4, fb = b & 15;
  if (pa == pb) {
    cov[pa] += (int16_t)(fb - fa);
    cov[pa + 1] -= (int16_t)(fb - fa);
    return;
  }
  cov[pa] += (int16_t)(16 - fa);
  cov[pa + 1] += (int16_t)fa;
  cov[pb] += (int16_t)(fb - 16);
  cov[pb + 1] -= (int16_t)fb;
}

void Scan::flushRow(int y) const {
  const int n = covX1 - covX0;
  int16_t acc = 0;
  bool any = false;
  for (int i = 0; i < n; i++) {
    acc += cov[i];
    cov[i] = acc > COV_FULL ? COV_FULL : acc;
    if (acc) any = true;
  }
  if (any) sink.covered(y + oy, covX0, cov, n);
  std::memset(cov, 0, sizeof(int16_t) * (size_t)(n + 2));
}

void Scan::subRow(int r, Crossing *xs, int m) const {
  if (rule == FillRule::EVEN_ODD) {
    for (int i = 0; i + 1 < m; i += 2) addSpan(r, xs[i].x, xs[i + 1].x);
    return;
  }
  int wind = 0;
  int32_t start = 0;
  for (int i = 0; i < m; i++) {
    const int before = wind;
    wind += xs[i].dir;
    if (before == 0 && wind != 0) start = xs[i].x;
    else if (before != 0 && wind == 0) addSpan(r, start, xs[i].x);
  }
}

void Scan::run(Edge *edges, int n, int16_t *active) const {
  if (n == 0) return;
  std::sort(edges, edges + n,
            [](const Edge &a, const Edge &b) { return a.yTop < b.yTop; });
  int r0 = std::max(rowLo, (int)edges[0].yTop), r1 = rowLo - 1;
  for (int i = 0; i < n; i++) r1 = std::max(r1, (int)edges[i].yEnd - 1);
  r1 = std::min(r1, rowHi);
  if (r0 > r1) return;
  Crossing xs[MAX_CROSSES];
  int next = 0, nActive = 0;
  int pixelRow = cov ? floorDiv(r0, rows) : 0;
  for (int r = r0; r <= r1; r++) {
    // Edges starting at or above this sub-row
    while (next < n && edges[next].yTop <= r) {
      Edge &e = edges[next];
      if (e.yEnd > r) {
        if (e.yTop < r) e.x += (int32_t)((int64_t)e.dx * (r - e.yTop));
        active[nActive++] = (int16_t)next;
      }
      next++;
    }
    int m = 0, k = 0;
    for (int i = 0; i < nActive; i++) {
      Edge &e = edges[active[i]];
      if (e.yEnd <= r) continue;
      active[k++] = active[i];
      const int32_t x = e.x;
      e.x += e.dx;
      if (m >= MAX_CROSSES) continue;
      int j = m++;
      while (j > 0 && xs[j - 1].x > x) {
        xs[j] = xs[j - 1];
        j--;
      }
      xs[j] = {x, e.dir};
    }
    nActive = k;
    if (cov) {
      const int py = floorDiv(r, rows);
      if (py != pixelRow) {
        flushRow(pixelRow);
        pixelRow = py;
      }
      subRow(r, xs, m);
    } else {
      subRow(r, xs, m);
    }
  }
  if (cov) flushRow(pixelRow);
}

// Collects edges from the loops of a path or a stroke, in device space,
// and scans them in parts whenever the buffer is full
struct EdgeSink {
  Scan scan;
  Edge *edges;
  int16_t *active;
  int capacity, count = 0;
  int sign = 1;  // multiplies the direction (a loop turned the other way)
  bool open = false;
  int32_t firstX = 0, firstY = 0, lastX = 0, lastY = 0;

  // In device pixels
  void moveTo(float x, float y) {
    closeLoop();
    // Room for the loop, so that it is not cut (see edge())
    if (count > capacity - LOOP_RESERVE) flush();
    firstX = lastX = subX(x);
    firstY = lastY = subY(y);
    open = true;
  }
  void lineTo(float x, float y) {
    const int32_t sx = subX(x), sy = subY(y);
    if (!open) {
      moveTo(x, y);
      return;
    }
    edge(lastX, lastY, sx, sy);
    lastX = sx;
    lastY = sy;
  }
  void closeLoop() {
    if (open && (lastX != firstX || lastY != firstY))
      edge(lastX, lastY, firstX, firstY);
    open = false;
  }
  void finish() {
    closeLoop();
    flush();
  }
  void flush() {
    scan.run(edges, count, active);
    count = 0;
  }

 private:
  int32_t subX(float x) const {
    const float v = (x - (float)scan.ox) * (float)SUB;
    if (!(v > (float)-SUB_MAX)) return -SUB_MAX;
    if (!(v < (float)SUB_MAX)) return SUB_MAX;
    return roundToInt(v);
  }
  int32_t subY(float y) const {
    const float v = (y - (float)scan.oy) * (float)(SUB * scan.rows);
    if (!(v > (float)-SUB_MAX)) return -SUB_MAX;
    if (!(v < (float)SUB_MAX)) return SUB_MAX;
    return roundToInt(v);
  }
  void edge(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    if (ay == by) return;
    int dir = sign;
    if (ay > by) {
      std::swap(ax, bx);
      std::swap(ay, by);
      dir = -dir;
    }
    const int top = ceilDiv(ay - SUB / 2, SUB), end = ceilDiv(by - SUB / 2, SUB);
    if (top >= end || end <= scan.rowLo || top > scan.rowHi) return;
    if (count >= capacity) {
      // The loop does not fit: cut it here, closing both parts with the
      // chord to its first point. Exact for a convex loop (every loop of a
      // stroke); a concave one may show the chord.
      const bool wasOpen = open;
      const int32_t fx = firstX, fy = firstY, lx = lastX, ly = lastY;
      if (wasOpen) {
        open = false;  // closeLoop() must not add the chord again
        if (count >= capacity) flush();
        edge(ax, ay, fx, fy);  // (ax, ay) is the last point of the part
        flush();
        edge(fx, fy, lx, ly);
        open = true;
        firstX = fx;
        firstY = fy;
      } else {
        flush();
      }
    }
    Edge &e = edges[count++];
    e.yTop = (int16_t)top;
    e.yEnd = (int16_t)end;
    e.dir = (int8_t)dir;
    const int64_t dx = (int64_t)(bx - ax) * (1 << X_FRAC), dy = by - ay;
    const int64_t num = dx * SUB;
    e.dx = (int32_t)((num >= 0 ? num + dy / 2 : num - dy / 2) / dy);
    // x at the center of the first sub-row
    const int64_t off = (int64_t)top * SUB + SUB / 2 - ay;
    e.x = (int32_t)((int64_t)ax * (1 << X_FRAC) + (dx * off) / dy);
  }
};

// ---------------------------------------------------------------------------
// Flattening

// Segments a curve of `devLen` pixels (its control polygon) is cut into;
// `quality` 1 is the finest, less when the edges would not fit the memory
static inline int segmentsFor(float devLen, float quality) {
  devLen *= quality;
  if (!(devLen > 1.0f)) return 1;
  // Within a tenth of a pixel of the curve (a thin stroke shows a kink
  // sooner than a fill shows an offset). The coefficient sets the density:
  // the segment count goes with its square root.
  const int n = (int)std::ceil(std::sqrt(devLen * 1.5f));
  return clampInt(1, 64, n);
}

// The largest factor by which a transform stretches a length
static inline float scaleOf(const affine2f &m) {
  const float sx = std::sqrt(m.a * m.a + m.b * m.b);
  const float sy = std::sqrt(m.c * m.c + m.d * m.d);
  return std::max(sx, sy);
}

// Walks a path, calling the sink with the flattened points in the path's
// own coordinates: begin(p), vertex(p), end(closed)
template <typename S>
void walkPath(const Path &path, float scale, float quality, S &sink) {
  const float *c = path.coords;
  int left = path.coordCount;
  vec2f cur = {0, 0}, start = {0, 0};
  bool open = false;
  auto endSub = [&](bool closed) {
    if (open) sink.end(closed);
    open = false;
  };
  auto ensure = [&]() {
    if (!open) {
      sink.begin(start);
      cur = start;
      open = true;
    }
  };
  for (int i = 0; i < path.opCount; i++) {
    const PathOp op = (PathOp)path.ops[i];
    const int n = pathOpCoords(op);
    if (n > left) break;
    switch (op) {
      case PathOp::MOVE:
        endSub(false);
        start = cur = {c[0], c[1]};
        break;
      case PathOp::LINE:
        ensure();
        cur = {c[0], c[1]};
        sink.vertex(cur);
        break;
      case PathOp::QUAD: {
        ensure();
        const vec2f p0 = cur, p1 = {c[0], c[1]}, p2 = {c[2], c[3]};
        const float len = (std::hypot(p1.x - p0.x, p1.y - p0.y) +
                           std::hypot(p2.x - p1.x, p2.y - p1.y)) * scale;
        const int k = segmentsFor(len, quality);
        for (int j = 1; j <= k; j++) {
          const float t = (float)j / (float)k, s = 1.0f - t;
          const float a = s * s, b = 2.0f * s * t, d = t * t;
          sink.vertex({a * p0.x + b * p1.x + d * p2.x,
                       a * p0.y + b * p1.y + d * p2.y});
        }
        cur = p2;
        break;
      }
      case PathOp::CUBIC: {
        ensure();
        const vec2f p0 = cur, p1 = {c[0], c[1]}, p2 = {c[2], c[3]},
                    p3 = {c[4], c[5]};
        const float len = (std::hypot(p1.x - p0.x, p1.y - p0.y) +
                           std::hypot(p2.x - p1.x, p2.y - p1.y) +
                           std::hypot(p3.x - p2.x, p3.y - p2.y)) * scale;
        const int k = segmentsFor(len, quality);
        for (int j = 1; j <= k; j++) {
          const float t = (float)j / (float)k, s = 1.0f - t;
          const float a = s * s * s, b = 3.0f * s * s * t, d = 3.0f * s * t * t,
                      e = t * t * t;
          sink.vertex({a * p0.x + b * p1.x + d * p2.x + e * p3.x,
                       a * p0.y + b * p1.y + d * p2.y + e * p3.y});
        }
        cur = p3;
        break;
      }
      case PathOp::CLOSE:
        endSub(true);
        cur = start;
        break;
      default:
        break;
    }
    c += n;
    left -= n;
  }
  endSub(false);
}

// The edges a path will make, roughly: to coarsen the curves when they
// would not fit the edge buffer
static int estimateEdges(const Path &path, float scale, float quality,
                         bool stroke) {
  const float *c = path.coords;
  int left = path.coordCount, edges = 0;
  vec2f cur = {0, 0};
  for (int i = 0; i < path.opCount; i++) {
    const PathOp op = (PathOp)path.ops[i];
    const int n = pathOpCoords(op);
    if (n > left) break;
    switch (op) {
      case PathOp::MOVE:
        cur = {c[0], c[1]};
        edges += 1;
        break;
      case PathOp::LINE:
        cur = {c[0], c[1]};
        edges += 1;
        break;
      case PathOp::QUAD:
      case PathOp::CUBIC: {
        float len = 0.0f;
        vec2f p = cur;
        for (int k = 0; k < n; k += 2) {
          const vec2f q = {c[k], c[k + 1]};
          len += std::hypot(q.x - p.x, q.y - p.y);
          p = q;
        }
        cur = p;
        edges += segmentsFor(len * scale, quality);
        break;
      }
      default:
        break;
    }
    c += n;
    left -= n;
  }
  return stroke ? edges * 8 : edges;
}

// Fill: the points through the transform into loops
struct FillSink {
  EdgeSink &edges;
  const affine2f &m;
  void begin(const vec2f &p) {
    const vec2f d = m.apply(p);
    edges.moveTo(d.x, d.y);
  }
  void vertex(const vec2f &p) {
    const vec2f d = m.apply(p);
    edges.lineTo(d.x, d.y);
  }
  void end(bool) { edges.closeLoop(); }
};

// ---------------------------------------------------------------------------
// Stroking: each segment a quadrilateral, each corner a join, each end a
// cap, all loops turned the same way and filled with the nonzero rule, so
// that their union is the stroke

// Unit circle, 32 points, clockwise on screen (y down)
const vec2f CIRCLE32[32] = {
    {1.000000f, 0.000000f},   {0.980785f, 0.195090f},   {0.923880f, 0.382683f},
    {0.831470f, 0.555570f},   {0.707107f, 0.707107f},   {0.555570f, 0.831470f},
    {0.382683f, 0.923880f},   {0.195090f, 0.980785f},   {0.000000f, 1.000000f},
    {-0.195090f, 0.980785f},  {-0.382683f, 0.923880f},  {-0.555570f, 0.831470f},
    {-0.707107f, 0.707107f},  {-0.831470f, 0.555570f},  {-0.923880f, 0.382683f},
    {-0.980785f, 0.195090f},  {-1.000000f, 0.000000f},  {-0.980785f, -0.195090f},
    {-0.923880f, -0.382683f}, {-0.831470f, -0.555570f}, {-0.707107f, -0.707107f},
    {-0.555570f, -0.831470f}, {-0.382683f, -0.923880f}, {-0.195090f, -0.980785f},
    {0.000000f, -1.000000f},  {0.195090f, -0.980785f},  {0.382683f, -0.923880f},
    {0.555570f, -0.831470f},  {0.707107f, -0.707107f},  {0.831470f, -0.555570f},
    {0.923880f, -0.382683f},  {0.980785f, -0.195090f},
};

struct Stroker {
  EdgeSink &edges;
  const affine2f &m;
  const StrokeStyle &st;
  float hw;        // half width
  int circleStep;  // of CIRCLE32 for round joins and caps
  // The subpath
  int n = 0;
  vec2f first = {0, 0}, firstB = {0, 0}, firstDir = {0, 0};
  vec2f prev = {0, 0}, prevDir = {0, 0};
  vec2f pendA = {0, 0}, pendB = {0, 0}, pendDir = {0, 0};
  bool pending = false, firstPending = false;

  Stroker(EdgeSink &e, const affine2f &m, const StrokeStyle &st, float scale)
      : edges(e), m(m), st(st) {
    hw = std::fabs(st.width) * 0.5f;
    const float rpx = hw * scale;
    circleStep = rpx > 12.0f ? 1 : (rpx > 3.0f ? 2 : 4);
  }

  void begin(const vec2f &p) {
    n = 1;
    first = prev = p;
    pending = firstPending = false;
  }
  void vertex(const vec2f &p) {
    if (n == 0) {
      begin(p);
      return;
    }
    const float dx = p.x - prev.x, dy = p.y - prev.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!(len > 1e-6f)) return;
    const vec2f dir = {dx / len, dy / len};
    if (n == 1) {
      // The first segment waits: its start is a cap or a join
      firstDir = dir;
      firstB = p;
      firstPending = true;
    } else {
      join(prev, prevDir, dir);
      if (pending) segment(pendA, pendB, pendDir, false, false);
      pendA = prev;
      pendB = p;
      pendDir = dir;
      pending = true;
    }
    prev = p;
    prevDir = dir;
    n++;
  }
  void end(bool closed) {
    if (n == 1) {
      // A lone point: a dot for round caps, a square for square ones
      if (st.cap == LineCap::ROUND) circle(first);
      else if (st.cap == LineCap::SQUARE) square(first);
    } else if (closed && n >= 3) {
      // Close with a segment back to the start and joins at both ends
      const float dx = first.x - prev.x, dy = first.y - prev.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1e-6f) {
        const vec2f dir = {dx / len, dy / len};
        join(prev, prevDir, dir);
        if (pending) segment(pendA, pendB, pendDir, false, false);
        segment(prev, first, dir, false, false);
        join(first, dir, firstDir);
      } else {
        join(prev, prevDir, firstDir);
        if (pending) segment(pendA, pendB, pendDir, false, false);
      }
      segment(first, firstB, firstDir, false, false);
    } else if (n == 2) {
      segment(first, firstB, firstDir, true, true);
    } else {
      segment(first, firstB, firstDir, true, false);
      if (pending) segment(pendA, pendB, pendDir, false, true);
    }
    n = 0;
    pending = firstPending = false;
  }

 private:
  void loop(const vec2f *pts, int k) {
    // Turn every loop the same way (positive area) for the nonzero union
    float area = 0.0f;
    for (int i = 0, j = k - 1; i < k; j = i++)
      area += pts[j].x * pts[i].y - pts[i].x * pts[j].y;
    edges.sign = area < 0.0f ? -1 : 1;
    for (int i = 0; i < k; i++) {
      const vec2f d = m.apply(pts[i]);
      if (i == 0) edges.moveTo(d.x, d.y);
      else edges.lineTo(d.x, d.y);
    }
    edges.closeLoop();
    edges.sign = 1;
  }
  void segment(vec2f a, vec2f b, const vec2f &dir, bool capA, bool capB) {
    const vec2f nrm = {-dir.y * hw, dir.x * hw};
    if (st.cap == LineCap::SQUARE) {
      if (capA) a = a - dir * hw;
      if (capB) b = b + dir * hw;
    } else if (st.cap == LineCap::ROUND) {
      if (capA) circle(a);
      if (capB) circle(b);
    }
    const vec2f q[4] = {a + nrm, b + nrm, b - nrm, a - nrm};
    loop(q, 4);
  }
  void join(const vec2f &v, const vec2f &d0, const vec2f &d1) {
    const float cross = d0.x * d1.y - d0.y * d1.x;
    const float dot = d0.x * d1.x + d0.y * d1.y;
    if (std::fabs(cross) < 1e-6f && dot > 0.0f) return;  // straight on
    if (st.join == LineJoin::ROUND) {
      circle(v);
      return;
    }
    // The outer corners of the two segments
    const vec2f n0 = {-d0.y * hw, d0.x * hw}, n1 = {-d1.y * hw, d1.x * hw};
    const vec2f o0 = cross > 0.0f ? v - n0 : v + n0;
    const vec2f o1 = cross > 0.0f ? v - n1 : v + n1;
    if (st.join == LineJoin::MITER) {
      const vec2f mid = {(o0.x + o1.x) * 0.5f - v.x, (o0.y + o1.y) * 0.5f - v.y};
      const float m2 = mid.x * mid.x + mid.y * mid.y;
      // The tip is hw^2 / |mid| along mid; the miter ratio is hw / |mid|
      if (m2 > 1e-12f && hw * hw <= st.miterLimit * st.miterLimit * m2) {
        const float k = hw * hw / m2;
        const vec2f tip = {v.x + mid.x * k, v.y + mid.y * k};
        const vec2f q[4] = {v, o0, tip, o1};
        loop(q, 4);
        return;
      }
    }
    const vec2f t[3] = {v, o0, o1};
    loop(t, 3);
  }
  void circle(const vec2f &c) {
    vec2f pts[32];
    int k = 0;
    for (int i = 0; i < 32; i += circleStep)
      pts[k++] = {c.x + CIRCLE32[i].x * hw, c.y + CIRCLE32[i].y * hw};
    loop(pts, k);
  }
  void square(const vec2f &c) {
    const vec2f q[4] = {{c.x - hw, c.y - hw}, {c.x + hw, c.y - hw},
                        {c.x + hw, c.y + hw}, {c.x - hw, c.y + hw}};
    loop(q, 4);
  }
};

// ---------------------------------------------------------------------------
// The calls

// Target pixel bounds of a rectangle under m, rounded outward
static Rect deviceBounds(const RectF &r, const affine2f &m) {
  const vec2f c[4] = {m.apply(r.x, r.y), m.apply(r.right(), r.y),
                      m.apply(r.x, r.bottom()), m.apply(r.right(), r.bottom())};
  float x0 = c[0].x, x1 = c[0].x, y0 = c[0].y, y1 = c[0].y;
  for (int i = 1; i < 4; i++) {
    x0 = std::min(x0, c[i].x);
    x1 = std::max(x1, c[i].x);
    y0 = std::min(y0, c[i].y);
    y1 = std::max(y1, c[i].y);
  }
  if (!(x0 <= x1 && y0 <= y1)) return {0, 0, 0, 0};
  const int ix0 = snap(x0), iy0 = snap(y0);
  return {ix0, iy0, snap(x1) + 1 - ix0, snap(y1) + 1 - iy0};
}

// The buffers a scan may use when the arena has no room: on the caller's
// stack
struct ScanBuffers {
  int16_t cov[STACK_COV + 2];
  Edge edges[STACK_EDGES];
  int16_t active[STACK_EDGES];
};

// Sets up a scan over the target pixels `area` (within the clip): the
// coverage buffer when the sink antialiases (from the scratch memory, or the
// stack up to STACK_COV pixels wide; without either, no antialiasing), the
// origin and the rows, and the edge buffer (as much scratch as there is,
// else the stack). The scratch memory is released by the caller's
// ScratchMark.
static void prepareScan(Graphics2D &g, const Sink &sink, const Rect &area,
                        ScanBuffers &bufs, Scan &scan, Edge *&edges,
                        int16_t *&active, int &capacity) {
  const Rect &clip = sink.ras.clip;
  const int oy = clip.y + clip.height / 2;
  scan.ox = clip.x + clip.width / 2;
  scan.oy = oy;
  scan.rows = 1;
  scan.cov = nullptr;
  if (sink.aa) {
    const int n = area.width;
    int16_t *cov = (int16_t *)G2Impl::scratchAlloc(g, sizeof(int16_t) * (size_t)(n + 2));
    if (!cov && n <= STACK_COV) cov = bufs.cov;
    if (cov) {
      std::memset(cov, 0, sizeof(int16_t) * (size_t)(n + 2));
      scan.cov = cov;
      scan.covX0 = area.x;
      scan.covX1 = area.right();
      scan.rows = AA_ROWS;
    }
  }
  // Without the coverage buffer one sub-row per pixel row, sampled at the
  // pixel center like the polygons
  scan.rowLo = (area.y - oy) * scan.rows;
  scan.rowHi = (area.bottom() - oy) * scan.rows - 1;
  edges = bufs.edges;
  active = bufs.active;
  capacity = STACK_EDGES;
  // As many edges as the scratch memory holds (16 + 2 bytes each)
  const size_t avail = G2Impl::scratchAvail(g);
  const size_t per = sizeof(Edge) + sizeof(int16_t);
  if (avail >= per * (STACK_EDGES + 1) + 8) {
    const size_t n = std::min((size_t)4096, (avail - 8) / per);
    Edge *e = (Edge *)G2Impl::scratchAlloc(g, sizeof(Edge) * n);
    int16_t *a = (int16_t *)G2Impl::scratchAlloc(g, sizeof(int16_t) * n);
    if (e && a) {
      edges = e;
      active = a;
      capacity = (int)n;
    }
  }
}

// Fills (stroke == nullptr) or strokes a path under m with the brush
static void drawPathImpl(Graphics2D &g, const Path &path, const affine2f &m,
                         const Brush &brush, const StrokeStyle *stroke,
                         bool antialias) {
  if (!g.hasTarget() || path.opCount == 0 || !path.ops || !path.coords) return;
  if (stroke && !(std::fabs(stroke->width) > 0.0f)) return;
  Sink sink;
  if (!sink.init(g, brush, m, antialias)) return;
  const Rect &clip = sink.ras.clip;
  if (clip.isEmpty()) return;
  // The pixels the path can touch, for the coverage buffer and an early out
  RectF b = path.bounds;
  if (b.isEmpty()) b = pathBounds(path);
  if (stroke) {
    const float e = std::fabs(stroke->width) *
                    (stroke->join == LineJoin::MITER
                         ? std::max(stroke->miterLimit, 1.0f) * 0.5f
                         : 0.71f);
    b = {b.x - e, b.y - e, b.width + 2 * e, b.height + 2 * e};
  }
  const Rect area = deviceBounds(b, m).intersect(clip);
  if (area.isEmpty()) return;

  G2Impl::ScratchMark mark(g);
  ScanBuffers bufs;
  Scan scan = {sink, stroke ? FillRule::NONZERO : path.rule, 0, 0, 1, 0, 0, nullptr, 0, 0};
  Edge *edges;
  int16_t *active;
  int capacity;
  prepareScan(g, sink, area, bufs, scan, edges, active, capacity);
  EdgeSink es = {scan, edges, active, capacity, 0};
  const float scale = scaleOf(m);
  // Curves coarser when their edges would not fit (a segment count goes
  // with the square root of the length)
  // Only for fills: a stroke's loops are convex, so cutting them when the
  // buffer is full (EdgeSink::edge) loses nothing but time
  float quality = 1.0f;
  const int est = stroke ? 0 : estimateEdges(path, scale, 1.0f, false);
  if (est > es.capacity) {
    const float r = (float)es.capacity / (float)est;
    quality = std::max(r * r * 0.9f, 1e-4f);
  }
  if (stroke) {
    Stroker s(es, m, *stroke, scale);
    walkPath(path, scale, quality, s);
  } else {
    FillSink s = {es, m};
    walkPath(path, scale, quality, s);
  }
  es.finish();
}

}  // namespace

// --- The area fills, antialiased ---------------------------------------------

bool G2Impl::wantsAntialias(const Graphics2D &g) {
  return ANTIALIAS && g.state_.antialias &&
         g.state_.blendMode != BlendMode::NONE;
}

void G2Impl::fillPathColor(Graphics2D &g, const vg::Path &path, Color c) {
  drawPathImpl(g, path, g.state_.transform, vg::Brush{c, nullptr}, nullptr,
               true);
}

void G2Impl::fillRectAA(Graphics2D &g, const RectF &r, Color c) {
  if (kind(g) <= TransformKind::SCALE) {
    // Axis-aligned: the coverage is the product of the overlaps along x
    // and y, so the rows and the two edge columns are painted directly,
    // without the rasterizer
    Sink sink;
    if (!g.hasTarget() || !sink.init(g, vg::Brush{c, nullptr}, g.state_.transform, true))
      return;
    const affine2f &m = g.state_.transform;
    float x0 = m.a * r.x + m.tx, x1 = m.a * r.right() + m.tx;
    float y0 = m.d * r.y + m.ty, y1 = m.d * r.bottom() + m.ty;
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const Rect &clip = sink.ras.clip;
    const float lim = 1e6f;
    x0 = std::clamp(x0, -lim, lim), x1 = std::clamp(x1, -lim, lim);
    y0 = std::clamp(y0, -lim, lim), y1 = std::clamp(y1, -lim, lim);
    if (!(x1 > x0 && y1 > y0)) return;
    const int ix0 = (int)std::floor(x0), ix1 = (int)std::ceil(x1) - 1;
    const int iy0 = (int)std::floor(y0), iy1 = (int)std::ceil(y1) - 1;
    // The coverage of the edge columns (the inside is 1)
    const float cx0 = std::min(x1, (float)ix0 + 1.0f) - x0;
    const float cx1 = ix1 > ix0 ? x1 - (float)ix1 : cx0;
    for (int y = std::max(iy0, clip.y); y <= std::min(iy1, clip.bottom() - 1); y++) {
      const float cy = std::min(y1, (float)y + 1.0f) - std::max(y0, (float)y);
      auto paintRun = [&](int xa, int xb, float cov) {
        xa = std::max(xa, clip.x);
        xb = std::min(xb, clip.right());
        if (xb <= xa) return;
        Paint p;
        if (sink.paintAt((uint32_t)(cov * 64.0f + 0.5f), p))
          sink.ras.spanRaw(y, xa, xb, p);
      };
      if (ix1 == ix0) {
        paintRun(ix0, ix0 + 1, cx0 * cy);
        continue;
      }
      paintRun(ix0, ix0 + 1, cx0 * cy);
      paintRun(ix0 + 1, ix1, cy);
      paintRun(ix1, ix1 + 1, cx1 * cy);
    }
    return;
  }
  uint8_t ops[5];
  float co[8];
  vg::PathBuilder pb(ops, 5, co, 8);
  pb.rect(r.x, r.y, r.width, r.height);
  fillPathColor(g, pb.path(), c);
}

void G2Impl::fillEllipseAA(Graphics2D &g, const RectF &r, Color c) {
  uint8_t ops[6];
  float co[26];
  vg::PathBuilder pb(ops, 6, co, 26);
  const RectF n = r.normalized();
  pb.ellipse(n.x + n.width * 0.5f, n.y + n.height * 0.5f, n.width * 0.5f,
             n.height * 0.5f);
  fillPathColor(g, pb.path(), c);
}

void G2Impl::fillRoundRectAA(Graphics2D &g, const RectF &r, float radius,
                             Color c) {
  uint8_t ops[10];
  float co[40];
  vg::PathBuilder pb(ops, 10, co, 40);
  const RectF n = r.normalized();
  pb.roundRect(n.x, n.y, n.width, n.height, radius, radius);
  fillPathColor(g, pb.path(), c);
}

void G2Impl::fillPolygonAA(Graphics2D &g, const vec2i *pi, const vec2f *pf,
                           int n, Color c) {
  // The vertices are pixels: (x, y) stands for the point (x + 0.5, y + 0.5).
  // A path of lines over them, in pieces of 64 that share no edge (so the
  // winding of a polygon over more vertices is not kept: documented)
  constexpr int N = 64;
  uint8_t ops[N + 1];
  float co[N * 2];
  if (n <= N) {
    vg::PathBuilder pb(ops, N + 1, co, N * 2);
    for (int i = 0; i < n; i++) {
      const float x = (pi ? (float)pi[i].x : pf[i].x) + 0.5f;
      const float y = (pi ? (float)pi[i].y : pf[i].y) + 0.5f;
      if (i == 0) pb.moveTo(x, y);
      else pb.lineTo(x, y);
    }
    pb.close();
    fillPathColor(g, pb.path(vg::FillRule::EVEN_ODD), c);
    return;
  }
  // Too many for the stack: the polygon API's own path, without antialiasing
  Paint p;
  if (G2Impl::makePaint(g, c, p)) G2Impl::fillPolygon(g, pi, pf, n, p, true);
}

// --- Lines and outlines, antialiased -------------------------------------------
//
// Lines and outlines stay a pixel wide whatever the transform: the geometry
// is taken to the target first and stroked there, a pixel wide, under the
// identity. Lines run between pixel centers with square caps, so that an
// axis-aligned one covers the same pixels as drawLine(); outlines run half
// a pixel inside the shape, so that they stay within the fill.

namespace {

// The scale of the transform along its axes (for insets of half a pixel)
void axisScales(const affine2f &m, float &sx, float &sy) {
  sx = std::sqrt(m.a * m.a + m.b * m.b);
  sy = std::sqrt(m.c * m.c + m.d * m.d);
}

void toDevice(float *co, int n, const affine2f &m) {
  for (int i = 0; i + 1 < n; i += 2) {
    const vec2f p = m.apply(co[i], co[i + 1]);
    co[i] = p.x;
    co[i + 1] = p.y;
  }
}

// A line a pixel wide in target pixels, antialiased the way of Wu: along
// its major axis every pixel column (row) gets the two pixels the line
// passes between, weighted by where it passes. Consecutive segments skip
// their first column, so that a vertex is not painted twice (a translucent
// polyline would show its joints otherwise). Far cheaper than stroking the
// line as a polygon: a few instructions per pixel, nothing per row.
struct ThinLine {
  const Raster ras;
  const Sink &sink;
  // The pixels of the column at the end of the last segment (where the
  // next one starts): a continuing segment paints a pixel of its first
  // column only by what its weight exceeds the one already there, so that
  // a vertex is neither painted twice (a translucent polyline would show
  // its joints) nor left dim (where the two segments split its coverage).
  // The first column of a polygon is kept the same way for its last segment.
  struct Pair {
    int x[2], y[2];
    uint32_t w[2];
    int n = 0;
  };
  Pair last, firstCol;
  bool haveFirst = false;

  // `joinStart`: the segment continues the last one; `joinEnd`: it ends at
  // the first column of the polygon
  void segment(float x0, float y0, float x1, float y1, bool joinStart,
               bool joinEnd) {
    const float dx = x1 - x0, dy = y1 - y0;
    if (std::fabs(dx) >= std::fabs(dy)) {
      walk(x0, y0, x1, y1, joinStart, joinEnd, false);
    } else {
      walk(y0, x0, y1, x1, joinStart, joinEnd, true);
    }
  }

 private:
  // Along the major axis a (the other is b); `swapped`: a is y
  void walk(float a0, float b0, float a1, float b1, bool joinStart,
            bool joinEnd, bool swapped) {
    const bool reversed = a0 > a1;
    if (reversed) {
      std::swap(a0, a1);
      std::swap(b0, b1);
    }
    const float lim = 1e6f;
    if (!(a0 > -lim && a1 < lim)) return;
    const float grad = a1 > a0 ? (b1 - b0) / (a1 - a0) : 0.0f;
    // The pixel centers along a within [a0, a1]; the segment's start is the
    // first column, or the last when reversed
    const int ia0 = (int)std::ceil(a0 - 0.5f), iaEnd = (int)std::floor(a1 - 0.5f);
    const int startCol = reversed ? iaEnd : ia0, endCol = reversed ? ia0 : iaEnd;
    for (int ia = ia0; ia <= iaEnd; ia++) {
      const float ac = (float)ia + 0.5f;
      const float b = b0 + (ac - a0) * grad - 0.5f;  // in pixel index space
      const float fb = std::floor(b);
      const int ib = (int)fb;
      const uint32_t w1 = (uint32_t)((b - fb) * 64.0f + 0.5f), w0 = 64 - w1;
      Pair col;
      col.n = 0;
      col.x[col.n] = swapped ? ib : ia, col.y[col.n] = swapped ? ia : ib, col.w[col.n++] = w0;
      if (w1) col.x[col.n] = swapped ? ib + 1 : ia, col.y[col.n] = swapped ? ia : ib + 1, col.w[col.n++] = w1;
      const Pair *against = nullptr;
      if (ia == startCol && joinStart) against = &last;
      else if (ia == endCol && joinEnd && haveFirst) against = &firstCol;
      for (int k = 0; k < col.n; k++) {
        uint32_t w = col.w[k];
        if (against) {
          for (int j = 0; j < against->n; j++)
            if (against->x[j] == col.x[k] && against->y[j] == col.y[k])
              w = w > against->w[j] ? w - against->w[j] : 0;
        }
        if (w) plot(col.x[k], col.y[k], w);
      }
      if (ia == endCol) last = col;
      if (ia == startCol && !haveFirst) {
        firstCol = col;
        haveFirst = true;
      }
    }
    if (ia0 > iaEnd) last.n = 0;  // nothing painted: no joint to subtract
  }
  void plot(int x, int y, uint32_t cov) {
    if (!ras.clip.contains(x, y)) return;
    Paint p;
    if (sink.paintAt(cov, p)) plotRaw(ras.target, x, y, p);
  }
};

// Strokes a path already in target pixels a pixel wide, as thin lines
void strokeDevice(Graphics2D &g, const vg::Path &path, Color c, LineCap cap) {
  (void)cap;  // the lines reach the pixel centers at the ends either way
  Sink sink;
  if (!sink.init(g, vg::Brush{c, nullptr}, affine2f::identity(), true)) return;
  if (sink.ras.clip.isEmpty()) return;
  struct Lines {
    ThinLine tl;
    vec2f first = {0, 0}, prev = {0, 0};
    int n = 0;
    void begin(const vec2f &p) {
      first = prev = p;
      n = 1;
      tl.haveFirst = false;
      tl.last.n = 0;
    }
    void vertex(const vec2f &p) {
      tl.segment(prev.x, prev.y, p.x, p.y, n > 1, false);
      prev = p;
      n++;
    }
    void end(bool closed) {
      if (closed && n > 2) tl.segment(prev.x, prev.y, first.x, first.y, true, true);
      n = 0;
    }
  } lines = {{G2Impl::raster(g), sink}};
  walkPath(path, 1.0f, 1.0f, lines);
}

// An ellipse (or its arc) half a pixel inside the one inscribed in r,
// built in the drawing coordinates; false if there is no room for it
bool insetEllipse(const Graphics2D &g, const RectF &r, float &cx, float &cy,
                  float &rx, float &ry) {
  float sx, sy;
  axisScales(G2Impl::matrix(g), sx, sy);
  if (!(sx > 0.0f) || !(sy > 0.0f)) return false;
  const RectF n = r.normalized();
  cx = n.x + n.width * 0.5f;
  cy = n.y + n.height * 0.5f;
  rx = n.width * 0.5f - 0.5f / sx;
  ry = n.height * 0.5f - 0.5f / sy;
  return rx > 0.0f && ry > 0.0f;
}

}  // namespace

void G2Impl::strokePointsAA(Graphics2D &g, const vec2i *pi, const vec2f *pf,
                            int n, bool closed, Color c) {
  if (n < 2) return;
  // The points, as pixel centers on the target, in pieces of 64
  constexpr int N = 64;
  uint8_t ops[N + 1];
  float co[N * 2];
  const affine2f &m = g.state_.transform;
  const bool whole = n <= N;
  int i = 0;
  for (;;) {
    const int k = std::min(N, n - i);
    vg::PathBuilder pb(ops, N + 1, co, N * 2);
    for (int j = 0; j < k; j++) {
      const float x = (pi ? (float)pi[i + j].x : pf[i + j].x) + 0.5f;
      const float y = (pi ? (float)pi[i + j].y : pf[i + j].y) + 0.5f;
      const vec2f d = m.apply(x, y);
      if (j == 0) pb.moveTo(d.x, d.y);
      else pb.lineTo(d.x, d.y);
    }
    if (closed && whole) pb.close();
    strokeDevice(g, pb.path(), c,
                 closed && whole ? LineCap::BUTT : LineCap::SQUARE);
    if (i + k >= n) break;
    i += k - 1;
  }
  if (closed && !whole) {
    // The closing segment of a long polygon, on its own
    const vec2f a = m.apply((pi ? (float)pi[n - 1].x : pf[n - 1].x) + 0.5f,
                            (pi ? (float)pi[n - 1].y : pf[n - 1].y) + 0.5f);
    const vec2f b = m.apply((pi ? (float)pi[0].x : pf[0].x) + 0.5f,
                            (pi ? (float)pi[0].y : pf[0].y) + 0.5f);
    vg::PathBuilder pb(ops, N + 1, co, N * 2);
    pb.moveTo(a.x, a.y).lineTo(b.x, b.y);
    strokeDevice(g, pb.path(), c, LineCap::SQUARE);
  }
}

void G2Impl::drawEllipseAA(Graphics2D &g, const RectF &r, Color c) {
  float cx, cy, rx, ry;
  if (!insetEllipse(g, r, cx, cy, rx, ry)) {
    fillEllipseAA(g, r, c);  // too thin for an outline: all edge
    return;
  }
  uint8_t ops[6];
  float co[26];
  vg::PathBuilder pb(ops, 6, co, 26);
  pb.ellipse(cx, cy, rx, ry);
  toDevice(co, pb.coordCount(), g.state_.transform);
  strokeDevice(g, pb.path(), c, LineCap::BUTT);
}

void G2Impl::drawArcAA(Graphics2D &g, const RectF &r, float a0, float a1,
                       Color c) {
  float cx, cy, rx, ry;
  if (!insetEllipse(g, r, cx, cy, rx, ry)) {
    fillSectorAA(g, r, a0, a1, c);
    return;
  }
  uint8_t ops[8];
  float co[32];
  vg::PathBuilder pb(ops, 8, co, 32);
  pb.arc(cx, cy, rx, ry, a0, a1);
  if (a1 - a0 >= 6.2831853f) pb.close();
  toDevice(co, pb.coordCount(), g.state_.transform);
  strokeDevice(g, pb.path(), c, LineCap::BUTT);
}

void G2Impl::fillSectorAA(Graphics2D &g, const RectF &r, float a0, float a1,
                          Color c) {
  const RectF n = r.normalized();
  const float cx = n.x + n.width * 0.5f, cy = n.y + n.height * 0.5f;
  uint8_t ops[8];
  float co[32];
  vg::PathBuilder pb(ops, 8, co, 32);
  if (a1 - a0 >= 6.2831853f) {
    pb.ellipse(cx, cy, n.width * 0.5f, n.height * 0.5f);
  } else {
    pb.moveTo(cx, cy);
    pb.arc(cx, cy, n.width * 0.5f, n.height * 0.5f, a0, a1);
    pb.close();
  }
  fillPathColor(g, pb.path(), c);
}

void G2Impl::drawRoundRectAA(Graphics2D &g, const RectF &r, float radius,
                             Color c) {
  float sx, sy;
  axisScales(g.state_.transform, sx, sy);
  if (!(sx > 0.0f) || !(sy > 0.0f)) return;
  const RectF n = r.normalized();
  const float ix = 0.5f / sx, iy = 0.5f / sy;
  if (n.width <= 2 * ix || n.height <= 2 * iy) {
    fillRoundRectAA(g, r, radius, c);
    return;
  }
  radius = std::min(std::fabs(radius), std::min(n.width, n.height) * 0.5f);
  uint8_t ops[10];
  float co[40];
  vg::PathBuilder pb(ops, 10, co, 40);
  pb.roundRect(n.x + ix, n.y + iy, n.width - 2 * ix, n.height - 2 * iy,
               std::max(radius - ix, 0.0f), std::max(radius - iy, 0.0f));
  toDevice(co, pb.coordCount(), g.state_.transform);
  strokeDevice(g, pb.path(), c, LineCap::BUTT);
}

void G2Impl::drawRectAA(Graphics2D &g, const RectF &r, float thickness,
                        Color c) {
  const RectF n = r.normalized();
  uint8_t ops[10];
  float co[16];
  vg::PathBuilder pb(ops, 10, co, 16);
  pb.rect(n.x, n.y, n.width, n.height);
  const float iw = n.width - 2 * thickness, ih = n.height - 2 * thickness;
  if (iw > 0.0f && ih > 0.0f) pb.rect(n.x + thickness, n.y + thickness, iw, ih);
  fillPathColor(g, pb.path(vg::FillRule::EVEN_ODD), c);
}

void Graphics2D::fillPath(const vg::Path &path) {
  drawPathImpl(*this, path, state_.transform, state_.fillBrush, nullptr,
               state_.antialias);
}

void Graphics2D::strokePath(const vg::Path &path) {
  drawPathImpl(*this, path, state_.transform, state_.strokeBrush,
               &state_.strokeStyle, state_.antialias);
}

void Graphics2D::strokePolyline(const vec2f *points, int count, bool closed) {
  if (count <= 0 || !points) return;
  // A path of lines over the points, without copying them: ops on the stack
  // for up to 64 points, else in pieces that share their joins poorly
  constexpr int N = 64;
  uint8_t ops[N + 1];
  float coords[N * 2];
  int i = 0;
  for (;;) {
    const int n = std::min(N, count - i);
    vg::PathBuilder pb(ops, N + 1, coords, N * 2);
    pb.polyline(points + i, n, closed && n == count);
    strokePath(pb.path());
    if (i + n >= count) break;
    i += n - 1;  // continue from the last point
  }
}

void Graphics2D::drawPicture(const vg::Picture &pic, Color currentColor,
                             float currentStrokeWidth) {
  if (!hasTarget() || !pic.shapes || (pic.features & ~vg::SUPPORTED_FEATURES))
    return;
  const affine2f base = state_.transform;
  const Rect clip = clipRect();
  for (int i = 0; i < pic.shapeCount; i++) {
    const vg::Shape &sh = pic.shapes[i];
    if (!sh.data) continue;
    if (sh.clip) {
      // The clip rectangle in target pixels: exact without a rotation,
      // else its bounding box
      Rect r = kind_ <= TransformKind::SCALE
                   ? G2Impl::mapRectSigned(*this, *sh.clip).normalized()
                   : deviceBounds(*sh.clip, base);
      setClipRect(r.intersect(clip));
      if (clipRect().isEmpty()) {
        setClipRect(clip);
        continue;
      }
    }
    const affine2f m = base * sh.transform;
    vg::Brush fill = sh.fill, stroke = sh.stroke;
    if (sh.flags & vg::SHAPE_FILL_CURRENT_COLOR)
      fill.color = colorWithAlpha(currentColor, colorA(fill.color));
    if (sh.flags & vg::SHAPE_STROKE_CURRENT_COLOR)
      stroke.color = colorWithAlpha(currentColor, colorA(stroke.color));
    switch (sh.kind) {
      case vg::ShapeKind::PATH: {
        const vg::Path &p = *(const vg::Path *)sh.data;
        drawPathImpl(*this, p, m, fill, nullptr, state_.antialias);
        if (sh.flags & vg::SHAPE_STROKE_CURRENT_WIDTH) {
          vg::StrokeStyle st = sh.strokeStyle;
          st.width = currentStrokeWidth;
          drawPathImpl(*this, p, m, stroke, &st, state_.antialias);
        } else {
          drawPathImpl(*this, p, m, stroke, &sh.strokeStyle, state_.antialias);
        }
        break;
      }
      case vg::ShapeKind::IMAGE: {
        const Texture &t = *(const Texture *)sh.data;
        const int op = state_.opacity;
        const int a = colorA(fill.color);
        if (a == 0) break;
        setTransform(m);
        if (a != 255) setOpacity((op * a + 127) / 255);
        drawImage(t, 0, 0);
        if (a != 255) setOpacity(op);
        break;
      }
      case vg::ShapeKind::TEXT: {
        const vg::Text &t = *(const vg::Text *)sh.data;
        if (!t.text || colorA(fill.color) == 0) break;
        const TextState saved = state_.text;
        if (t.font) setFont(t.font);
        if (!state_.text.font) {
          state_.text = saved;
          break;
        }
        setTransform(m);
        setTextColor(fill.color);
        // (x, y) is on the baseline; the cursor is the top of the line box
        drawString(roundToInt(t.x), roundToInt(t.y) - state_.text.ascent, t.text);
        state_.text = saved;
        break;
      }
      default:
        break;
    }
    if (sh.clip) setClipRect(clip);
  }
  setTransform(base);
}

}  // namespace shapoco::gfx2d
