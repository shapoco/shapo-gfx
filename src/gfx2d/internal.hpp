#ifndef SHAPOGFX2D_INTERNAL_HPP
#define SHAPOGFX2D_INTERNAL_HPP

// Internal declarations of the 2D renderer, shared by src/gfx2d/*.cpp.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <type_traits>

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

namespace shapoco::gfx2d::detail {

static_assert(SHAPOGFX2D_STACK_DEPTH >= 1 && SHAPOGFX2D_STACK_DEPTH <= 1024,
              "SHAPOGFX2D_STACK_DEPTH must be between 1 and 1024");
constexpr int STACK_DEPTH = SHAPOGFX2D_STACK_DEPTH;
constexpr bool TRANSFORM = SHAPOGFX2D_TRANSFORM != 0;
constexpr bool BLEND = SHAPOGFX2D_BLEND != 0;
constexpr bool COLOR_KEY = SHAPOGFX2D_COLOR_KEY != 0;

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
};

}  // namespace shapoco::gfx2d::detail

#endif
