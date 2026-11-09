#ifndef SHAPOGFX2D_INTERNAL_HPP
#define SHAPOGFX2D_INTERNAL_HPP

// Internal declarations of the 2D renderer, shared by src/gfx2d/*.cpp.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <type_traits>

#include "../common/arch_detect.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"

// Compile-time options (read by src/gfx2d/*.cpp only; no public type depends
// on them)
#ifndef SHAPOGFX2D_STACK_DEPTH
#define SHAPOGFX2D_STACK_DEPTH 16
#endif
// Transforms (setTransform() and friends); 0 keeps the identity
#ifndef SHAPOGFX2D_TRANSFORM
#define SHAPOGFX2D_TRANSFORM 1
#endif
// Blend modes other than ALPHA and the opacity (setBlend())
#ifndef SHAPOGFX2D_BLEND
#define SHAPOGFX2D_BLEND 1
#endif
// Color key of drawImage() (setColorKey())
#ifndef SHAPOGFX2D_COLOR_KEY
#define SHAPOGFX2D_COLOR_KEY 1
#endif
// Skeletal animation (rig.hpp); 0 leaves rig::Instance doing nothing
#ifndef SHAPOGFX2D_RIG
#define SHAPOGFX2D_RIG 1
#endif
// Antialiasing of the vector calls (fillPath() and friends); 0 draws their
// edges like polygons, whatever the state says
#ifndef SHAPOGFX2D_ANTIALIAS
#define SHAPOGFX2D_ANTIALIAS 1
#endif
// The per-image and per-bone arithmetic of transformed images and rigs
// (inverse, bounding box, matrix products) in fixed point instead of float.
// On by default where float is a library call, which then costs more than
// the pixels; with an FPU the float code is the shorter one. (The rows of a
// transformed image are stepped in fixed point either way.)
#ifndef SHAPOGFX2D_FIXED_POINT
#define SHAPOGFX2D_FIXED_POINT (!SHAPOGFX_ARCH_FPU)
#endif

namespace shapoco::gfx2d::detail {

static_assert(SHAPOGFX2D_STACK_DEPTH >= 1 && SHAPOGFX2D_STACK_DEPTH <= 1024,
              "SHAPOGFX2D_STACK_DEPTH must be between 1 and 1024");
constexpr int STACK_DEPTH = SHAPOGFX2D_STACK_DEPTH;
constexpr bool TRANSFORM = SHAPOGFX2D_TRANSFORM != 0;
constexpr bool BLEND = SHAPOGFX2D_BLEND != 0;
constexpr bool COLOR_KEY = SHAPOGFX2D_COLOR_KEY != 0;
constexpr bool RIG = SHAPOGFX2D_RIG != 0;
constexpr bool ANTIALIAS = SHAPOGFX2D_ANTIALIAS != 0;
constexpr bool FIXED_POINT = SHAPOGFX2D_FIXED_POINT != 0;

// ---------------------------------------------------------------------------
// Integer helpers

// floor(a / b) and ceil(a / b) for any signs
static inline int floorDiv(int a, int b) {
  const int q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
static inline int ceilDiv(int a, int b) {
  const int q = a / b;
  return (a % b != 0 && ((a < 0) == (b < 0))) ? q + 1 : q;
}

// Nearest integer (halves away from zero), without the library call lrint()
// takes even on a core with an FPU
static inline int32_t roundToInt(float v) {
  return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

// Lines and polygons are walked in coordinates relative to the center of the
// clip rectangle, where the clip rectangle (at most SHAPOGFX_COORD_MAX <
// 32768 pixels wide) lies within +-LINE_SAFE. Within that range every 16.16
// value and every product the walkers form fits 32 bits.
constexpr int LINE_SAFE = (1 << 14) - 1;
constexpr int LINE_INPUT_MAX = 1 << 29;  // inputs are clamped to this
static inline int clampInput(int v) {
  return v < -LINE_INPUT_MAX ? -LINE_INPUT_MAX
                             : (v > LINE_INPUT_MAX ? LINE_INPUT_MAX : v);
}

// The pixel for a continuous coordinate: ceil(v - 0.5), the first pixel whose
// center is at or right of an edge at v, and the pixel containing a point at
// v + 0.5. Clamped to +-LINE_INPUT_MAX (NaN goes to the negative end).
static inline int snap(float v) {
  constexpr float LIM = (float)LINE_INPUT_MAX;
  v -= 0.5f;
  if (!(v > -LIM)) return -LINE_INPUT_MAX;
  if (!(v < LIM)) return LINE_INPUT_MAX;
  const int i = (int)v;
  return (float)i < v ? i + 1 : i;
}

// ---------------------------------------------------------------------------
// Fixed point

static inline int clz64(uint64_t v) {
  const uint32_t hi = (uint32_t)(v >> 32);
  return hi ? __builtin_clz(hi) : 32 + __builtin_clz((uint32_t)v);
}

// f * 2^frac to the nearest integer, taken from the bits of the float (no
// float operation: a core without an FPU converts in a dozen instructions).
// False if the result does not fit 31 bits or f is not finite.
static inline bool floatToFixed(float f, int frac, int32_t &out) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  const int e = (int)((bits >> 23) & 0xFF);
  const int sh = e - 150 + frac;  // the value is m * 2^(e - 150)
  if (sh > 7) return false;       // 2^31 or more, infinite or NaN
  uint32_t v = 0;
  if (e != 0 && sh > -25) {  // (zero, the denormals and what rounds to zero)
    const uint32_t m = (bits & 0x7FFFFFu) | 0x800000u;
    v = sh >= 0 ? m << sh : (m + (1u << (-sh - 1))) >> -sh;
  }
  out = (bits >> 31) ? -(int32_t)v : (int32_t)v;
  return true;
}

// f * 2^frac truncated, as 64 bits (|f| * 2^frac below 2^62, f finite): the
// bits are shifted into place (the library's float to 64-bit conversions
// are calls even on a core with an FPU)
static inline int64_t floatToFixed64(float f, int frac) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  const int e = (int)((bits >> 23) & 0xFF);
  if (e == 0) return 0;  // zero and the denormals
  const int64_t m = (int64_t)((bits & 0x7FFFFFu) | 0x800000u);
  const int sh = e - 150 + frac;  // the value is m * 2^(e - 150)
  const int64_t v = sh >= 0 ? m << sh : (sh > -63 ? m >> -sh : 0);
  return (bits >> 31) ? -v : v;
}

// v * 2^-frac as a float, rounded to nearest, built from the bits likewise
// (frac up to 32)
static inline float fixedToFloat(int32_t v, int frac) {
  if (v == 0) return 0.0f;
  uint32_t mag = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
  const int lz = __builtin_clz(mag);
  mag <<= lz;
  // 24 bits of mantissa; a carry out of them goes into the exponent
  const uint32_t man = (mag >> 8) + ((mag >> 7) & 1u);
  uint32_t bits =
      ((uint32_t)(127 + 31 - lz - frac) << 23) + (man - (1u << 23));
  if (v < 0) bits |= 0x80000000u;
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

// The same for 64 bits (frac up to 32), from the top 31 bits: enough for a
// float, and without the library's 64-bit conversion, which goes through
// double
static inline float fixed64ToFloat(int64_t v, int frac) {
  const uint64_t mag = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
  const int sh = std::max(0, 33 - clz64(mag | 1));
  const int32_t top = (int32_t)(mag >> sh);
  return fixedToFloat(v < 0 ? -top : top, frac - sh);
}

// (a * b) >> 16 for a product beyond 64 bits (|a| < 2^47)
static inline int64_t mulShift16(int64_t a, int32_t b) {
  const int64_t hi = (int64_t)(int32_t)(a >> 32) * b;
  const int64_t lo = (int64_t)(uint32_t)a * b;
  return hi * 65536 + (lo >> 16);
}

// floor((hi * 2^32 + lo) / d) for hi < d (so that it fits 32 bits), by two
// 32-bit divisions (Knuth's algorithm D in base 2^16): no 64-bit division
// routine of the library
static inline uint32_t div64by32(uint32_t hi, uint32_t lo, uint32_t d) {
  const int s = __builtin_clz(d);
  d <<= s;
  const uint32_t vn1 = d >> 16, vn0 = d & 0xFFFFu;
  const uint32_t un32 = s ? (hi << s) | (lo >> (32 - s)) : hi;
  const uint32_t un10 = lo << s;
  const uint32_t un1 = un10 >> 16, un0 = un10 & 0xFFFFu;
  uint32_t q1 = un32 / vn1, rhat = un32 - q1 * vn1;
  while (q1 >= 65536u || q1 * vn0 > (rhat << 16) + un1) {
    q1--;
    rhat += vn1;
    if (rhat >= 65536u) break;
  }
  const uint32_t un21 = (un32 << 16) + un1 - q1 * d;  // (modulo 2^32)
  uint32_t q0 = un21 / vn1;
  rhat = un21 - q0 * vn1;
  while (q0 >= 65536u || q0 * vn0 > (rhat << 16) + un0) {
    q0--;
    rhat += vn1;
    if (rhat >= 65536u) break;
  }
  return (q1 << 16) + q0;
}

// The reciprocal of a 64-bit magnitude v > 0 for multiplying instead of
// dividing: r in [2^31, 2^32) and sh with 1 / v = r * 2^-(62 + sh), to 30
// bits
static inline uint32_t reciprocal64(uint64_t v, int &sh) {
  sh = 33 - clz64(v);
  // n in [2^30, 2^31)
  const uint32_t n = sh >= 0 ? (uint32_t)(v >> sh) : (uint32_t)(v << -sh);
  return n == (1u << 30) ? 0xFFFFFFFFu : div64by32(1u << 30, 0, n);
}

// (a * r) >> s, floor, for |a| < 2^62 and s < 96; a result beyond 2^62 is
// returned as +-2^62
constexpr int64_t MUL_SHIFT_SAT = (int64_t)1 << 62;
static inline int64_t mulShiftSat(int64_t a, uint32_t r, int s) {
  const int64_t hi = (int64_t)(int32_t)(a >> 32) * (int64_t)r;
  const uint64_t lo = (uint64_t)(uint32_t)a * r;
  const int64_t t = hi + (int64_t)(lo >> 32);  // (a * r) >> 32
  if (s >= 32) return t >> (s - 32 > 63 ? 63 : s - 32);
  const int l = 32 - s;  // the bits of t to move up
  if (l > 61) return t > 0 ? MUL_SHIFT_SAT : (t < 0 ? -MUL_SHIFT_SAT : 0);
  const int64_t lim = (int64_t)1 << (62 - l);
  if (t >= lim) return MUL_SHIFT_SAT;
  if (t < -lim) return -MUL_SHIFT_SAT;
  // (the low half of the product: its top bits for s >= 0, all of it moved
  // up otherwise)
  const uint32_t low = (uint32_t)lo;
  return t * ((int64_t)1 << l) +
         (s >= 0 ? (int64_t)(low >> s) : (int64_t)low * ((int64_t)1 << -s));
}

// An affine transform in fixed point, for the paths where float would cost a
// core without an FPU more than the pixels do: a .. d are 8.24 (below 128),
// tx and ty 16.16 (below 32768 pixels)
struct AffineQ {
  static constexpr int LIN = 24, POS = 16;
  int32_t a, b, c, d, tx, ty;
  // False if m does not fit (the callers then take their float path)
  static bool from(const affine2f &m, AffineQ &q);
  // out = m * n (n applies first). False if an entry does not fit; it is
  // then left at the end of the range.
  static bool mul(const AffineQ &m, const AffineQ &n, AffineQ &out);
  affine2f toFloat() const;
};

// snap() of a 16.16 coordinate
static inline int snapQ16(int64_t v) {
  constexpr int64_t LIM = (int64_t)LINE_INPUT_MAX << 16;
  return (int)((std::max(-LIM, std::min(LIM, v)) + 0x7FFF) >> 16);
}

// ---------------------------------------------------------------------------
// Per-format row operations

// What a drawing call writes into the pixels of a span
enum class PaintOp : uint8_t {
  FILL,   // native, overwriting
  BLEND,  // native blended with alpha64
  ADD,    // native (already weighted) added with saturation
};

struct Paint {
  uint32_t native;
  uint32_t alpha64;
  PaintOp op;
};

template <PixelFormat F>
using FormatTag = std::integral_constant<PixelFormat, F>;

// Call fn(FormatTag<f>) for an enabled format
template <typename Fn>
__attribute__((always_inline)) inline void withFormat(PixelFormat f, Fn &&fn) {
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
    default: (void)fn; break;
  }
}

// Paint [x, x + n) of `line`. Every shape ends up here, so this is the one
// place that switches on the blend.
void fillSpanFmt(PixelFormat fmt, uint8_t *line, int x, int n, const Paint &p);
// The same for one format, chosen once per drawing call (a no-op for a
// disabled format)
using FillSpanFn = void (*)(uint8_t *line, int x, int n, const Paint &p);
FillSpanFn fillSpanFn(PixelFormat fmt);

// Read n pixels of `line` from x as Colors
void readColorsFmt(PixelFormat fmt, const uint8_t *line, int x, int n,
                   Color *out);

// How writeColorsFmt() puts the Colors
enum class WriteMode : uint8_t {
  COPY,        // convert and overwrite
  COPY_KEYED,  // the same, skipping Colors that are exactly 0 (keyed out)
  ALPHA,       // blend with (alpha x opacity)
  ADD,         // add, weighted by (alpha x opacity)
};
void writeColorsFmt(PixelFormat fmt, uint8_t *line, int x, int n,
                    const Color *src, WriteMode mode, uint32_t opacity64);

// One pixel, not clipped
void plotRaw(const Surface &target, int x, int y, const Paint &p);

// Rectangle r of the target, clipped to `clip`
inline void fillRectRaw(const Surface &target, const Rect &clip, const Rect &r,
                        const Paint &p) {
  const int x0 = std::max(r.x, clip.x), x1 = std::min(r.right(), clip.right());
  const int y0 = std::max(r.y, clip.y), y1 = std::min(r.bottom(), clip.bottom());
  if (x0 >= x1) return;
  if (x1 - x0 == 1) {
    // A pixel, or a column of them: no span loops
    for (int y = y0; y < y1; y++) plotRaw(target, x0, y, p);
    return;
  }
  for (int y = y0; y < y1; y++)
    fillSpanFmt(target.format, target.linePtr(y), x0, x1 - x0, p);
}

// The target and the clip rectangle, which is all the row code needs
struct Raster {
  Surface target;
  Rect clip;
  FillSpanFn fill;  // fillSpanFn(target.format)

  // [x0, x1) of row y, already clipped
  void spanRaw(int y, int x0, int x1, const Paint &p) const {
    if (x1 > x0) fill(target.linePtr(y), x0, x1 - x0, p);
  }
  void span(int y, int x0, int x1, const Paint &p) const {
    if (y < clip.y || y >= clip.bottom()) return;
    spanRaw(y, std::max(x0, clip.x), std::min(x1, clip.right()), p);
  }
  // Rectangle r, clipped
  void rect(const Rect &r, const Paint &p) const {
    fillRectRaw(target, clip, r, p);
  }
  // Center of the clip rectangle, the origin of the line and polygon walkers
  int originX() const { return clip.x + clip.width / 2; }
  int originY() const { return clip.y + clip.height / 2; }
};

// A one-pixel line between two target pixels
void drawLineRaw(const Raster &ras, int x0, int y0, int x1, int y1,
                 const Paint &p);

// A 1-bit mask: bit (x, y) is bit base + y * stride + x of `bits`, MSB first
// (a GRAY1 image, or a glyph of a GFXfont)
struct MaskSource {
  const uint8_t *bits;
  uint32_t base;
  uint32_t stride;
};

// ---------------------------------------------------------------------------
// Access to Graphics2D

struct G2Impl {
  static Raster raster(const Graphics2D &g) {
    return {g.target_, g.clipRect(), fillSpanFn(g.target_.format)};
  }
  static TransformKind kind(const Graphics2D &g) { return g.kind_; }
  // Integer offset of IDENTITY and TRANSLATE
  static int offsetX(const Graphics2D &g) { return g.ox_; }
  static int offsetY(const Graphics2D &g) { return g.oy_; }
  static const affine2f &matrix(const Graphics2D &g) {
    return g.state_.transform;
  }

  // The Paint of a color under the blend of the state; false if it draws
  // nothing
  static bool makePaint(const Graphics2D &g, Color c, Paint &p);
  // The same with the default blend (ALPHA at full opacity) inline, for the
  // calls that often draw a single pixel
  static inline bool makePaintInline(const Graphics2D &g, Color c, Paint &p) {
    if (g.state_.blendMode != BlendMode::ALPHA || g.state_.opacity != 255)
      return makePaint(g, c, p);
    const uint32_t a = colorAlpha64(c);
    if (a == 0) return false;
    p = {colorToNative(g.target_.format, a >= 64 ? c | 0xFF000000u : c), a,
         a >= 64 ? PaintOp::FILL : PaintOp::BLEND};
    return true;
  }

  // Target pixel of the pixel (x, y) (its center through the transform)
  static void mapPixel(const Graphics2D &g, float x, float y, int &px,
                       int &py);
  // Target pixel rectangle of a continuous rectangle under a transform
  // without rotation: the corners, snapped, in the order given (a negative
  // size is a mirrored rectangle)
  static Rect mapRectSigned(const Graphics2D &g, const RectF &r);

  // Scratch memory of the arena, released in reverse order
  static size_t scratchAvail(const Graphics2D &g) {
    const uint32_t top = (g.scratchTop_ + 7u) & ~7u;
    return g.scratch_ && top < g.scratchSize_ ? g.scratchSize_ - top : 0;
  }
  static void *scratchAlloc(Graphics2D &g, size_t bytes) {
    const uint32_t top = (g.scratchTop_ + 7u) & ~7u;
    if (!g.scratch_ || bytes > g.scratchSize_ - std::min(top, g.scratchSize_))
      return nullptr;
    g.scratchTop_ = top + (uint32_t)bytes;
    return g.scratch_ + top;
  }
  struct ScratchMark {
    Graphics2D &g;
    uint32_t top;
    explicit ScratchMark(Graphics2D &g) : g(g), top(g.scratchTop_) {}
    ~ScratchMark() { g.scratchTop_ = top; }
  };

  // Polygons of vertices given either as vec2i or as vec2f (one of pi and pf
  // is null), through the transform. pixels: the vertices are pixels, like
  // the end points of lines (the polygon API), else continuous coordinates
  // (areas)
  static void fillPolygon(Graphics2D &g, const vec2i *pi, const vec2f *pf,
                          int n, const Paint &p, bool pixels = false);
  // The one-pixel outline of a convex polygon (like drawEllipse())
  static void outlineConvex(Graphics2D &g, const vec2f *pts, int n,
                            const Paint &p);

  // Mask at (dx, dy) (drawing coordinates) showing `src` of the mask
  static void drawMask(Graphics2D &g, const MaskSource &m, const Rect &src,
                       int dx, int dy, const Paint *fg, const Paint *bg);

  // The area fills through the vector rasterizer when the state asks for
  // antialiasing (vg.cpp): true when it does
  static bool wantsAntialias(const Graphics2D &g);
  static void fillPathColor(Graphics2D &g, const vg::Path &path, Color c);
  static void fillRectAA(Graphics2D &g, const RectF &r, Color c);
  static void fillEllipseAA(Graphics2D &g, const RectF &r, Color c);
  static void fillRoundRectAA(Graphics2D &g, const RectF &r, float radius,
                              Color c);
  static void fillPolygonAA(Graphics2D &g, const vec2i *pi, const vec2f *pf,
                            int n, Color c);
  // The one-pixel lines and outlines as strokes a pixel wide on the target,
  // and the sectors and frames as paths (vg.cpp)
  static void strokePointsAA(Graphics2D &g, const vec2i *pi, const vec2f *pf,
                             int n, bool closed, Color c);
  static void drawEllipseAA(Graphics2D &g, const RectF &r, Color c);
  static void drawArcAA(Graphics2D &g, const RectF &r, float a0, float a1,
                        Color c);
  static void fillSectorAA(Graphics2D &g, const RectF &r, float a0, float a1,
                           Color c);
  static void drawRoundRectAA(Graphics2D &g, const RectF &r, float radius,
                              Color c);
  static void drawRectAA(Graphics2D &g, const RectF &r, float thickness,
                         Color c);
  // A mask under a transform, sampled 2 x 2 per pixel (images.cpp)
  static void drawMaskAA(Graphics2D &g, const MaskSource &m, const Rect &src,
                         int dx, int dy, Color fg, Color bg);
  // An image under the transform `m` (points relative to the top-left
  // corner of src to target pixels), its outline (the rectangle, or the
  // polygon in image pixels relative to src) antialiased (images.cpp);
  // `silhouette`, if given, is the color of drawSilhouette()
  // drawImage(img, 0, 0, src, polygon, count) under the transform `m`
  // instead of the state's, without antialiasing (the rig's images with
  // SHAPOGFX2D_FIXED_POINT: no float on the way)
  // setTransform() of an AffineQ
  static void setTransformFixed(Graphics2D &g, const AffineQ &m);
  static void drawImageFixed(Graphics2D &g, const Texture &img,
                             const Rect &src, const int16_t *polygon,
                             int count, const AffineQ &m);
  static void drawImageAA(Graphics2D &g, const Texture &img, const Rect &src,
                          const affine2f &m, const int16_t *polygon,
                          int count, const Color *silhouette = nullptr);
};

}  // namespace shapoco::gfx2d::detail

#endif
