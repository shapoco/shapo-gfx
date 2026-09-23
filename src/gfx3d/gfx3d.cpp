#include "shapoco/gfx3d/gfx3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>

// Perspective correction of texture coordinates:
//   0: none (affine interpolation everywhere)
//   1: vertical only (default). (u/w, v/w, 1/w) are interpolated along the
//      triangle plane and divided at the two end points of each span, so the
//      end points are exact and the span interior is affine. Costs one float
//      divide per span. Exact for horizontal surfaces seen by a camera without
//      roll.
//   2: full. (u/w, v/w, 1/w) are interpolated across the span and divided
//      every SHAPOGFX3D_PERSPECTIVE_STEP pixels; the texture coordinates are
//      interpolated linearly in between.
#ifndef SHAPOGFX3D_CORRECT_PERSPECTIVE
#define SHAPOGFX3D_CORRECT_PERSPECTIVE 1
#endif

// Level 2: pixels between two exact evaluations of (u, v). Power of two.
#ifndef SHAPOGFX3D_PERSPECTIVE_STEP
#define SHAPOGFX3D_PERSPECTIVE_STEP 16
#endif

// Pixels between two updates of the vertex color that modulates the texels of
// a textured, smoothly shaded span (a power of two, 1..16). 1 updates it on
// every pixel; 4 saves a few instructions per pixel and keeps the color of a
// group of 4 pixels constant, which is invisible unless the color changes by
// a whole shade within 4 pixels.
#ifndef SHAPOGFX3D_GOURAUD_STEP
#define SHAPOGFX3D_GOURAUD_STEP 1
#endif

// Optional features. Turning one off removes its code from the renderer and
// shrinks the per-triangle and per-span working memory, so the same arena
// holds more geometry. Like the perspective options above these are read by
// this file only and change no public type, so translation units cannot
// disagree about them.
//
//   SHAPOGFX3D_TEXTURE  0: no texture and no environment mapping.
//                          Material::texture and the TEXTURE / ENV_MAP flags
//                          are then ignored at run time, like a texture in a
//                          disabled pixel format.
//   SHAPOGFX3D_GOURAUD  0: flat shading. A triangle takes the color of its
//                          first vertex instead of interpolating the three,
//                          so smoothly shaded surfaces become faceted.
//   SHAPOGFX3D_BLEND    0: no translucency. Everything is drawn opaque; the
//                          material's blend mode and the alpha of an ARGB4444
//                          texture are ignored.
//   SHAPOGFX3D_LINES    0: LINES / LINE_STRIP / LINE_LOOP are ignored, so
//                          putLine() and putWireCube() draw nothing.
//   SHAPOGFX3D_POINTS   0: POINTS primitives are ignored.
#ifndef SHAPOGFX3D_TEXTURE
#define SHAPOGFX3D_TEXTURE 1
#endif
#ifndef SHAPOGFX3D_GOURAUD
#define SHAPOGFX3D_GOURAUD 1
#endif
#ifndef SHAPOGFX3D_BLEND
#define SHAPOGFX3D_BLEND 1
#endif
#ifndef SHAPOGFX3D_LINES
#define SHAPOGFX3D_LINES 1
#endif
#ifndef SHAPOGFX3D_POINTS
#define SHAPOGFX3D_POINTS 1
#endif

// Fixed part of the arena: the matrix stack of pushState(), the direct-mapped
// cache of transformed vertices (a power of two; a smaller cache costs
// re-transformed vertices, never correctness) and the layer table.
#ifndef SHAPOGFX3D_STACK_DEPTH
#define SHAPOGFX3D_STACK_DEPTH 16
#endif
#ifndef SHAPOGFX3D_VCACHE_SIZE
#define SHAPOGFX3D_VCACHE_SIZE 64
#endif
#ifndef SHAPOGFX3D_LAYER_MAX
#define SHAPOGFX3D_LAYER_MAX 8
#endif

// Perspective correction only exists where there are texture coordinates
#if SHAPOGFX3D_TEXTURE
#define SHAPOGFX3D_PERSPECTIVE SHAPOGFX3D_CORRECT_PERSPECTIVE
#else
#define SHAPOGFX3D_PERSPECTIVE 0
#endif

// Points and lines share their vertex stage and their span builder
#if SHAPOGFX3D_LINES || SHAPOGFX3D_POINTS
#define SHAPOGFX3D_UNLIT 1
#else
#define SHAPOGFX3D_UNLIT 0
#endif

// Fixed-point vertex stage and primitive setup, for cores without an FPU.
// The public API is unchanged -- positions, matrices and materials stay
// float -- but the current matrix and the projection are converted to fixed
// point once when they change, a vertex once when it is fetched, and
// everything from there to the primitive records is integer arithmetic:
// 32x32 -> 64 multiplies and a normalized-reciprocal division (one 32-bit
// hardware division for a 16-bit reciprocal, see Rcp). The records and
// everything after them (render()) are the same in both builds and integer
// only.
//
// Limits that the float path does not have: view-space coordinates within
// +-32767 model units, rotation and scale entries of the current matrix
// within +-2048, screen coordinates clamped to the guard band (+-8191 pixels;
// a triangle with a vertex beyond it bends where it crosses the screen, where
// the float path clips it exactly), texture coordinates within +-30000
// texels. Pictures are not pixel-identical to the float path: expect a few
// pixels per thousand to differ along edges.
#ifndef SHAPOGFX3D_FIXED_POINT
#define SHAPOGFX3D_FIXED_POINT 0
#endif

// Depth resolution of the records: 32 bits (8.24 fixed point, the default) or
// 16 bits (1.15, 6 bytes instead of 12 per record with depth). With 16 bits
// the depth gradient is quantized to 2^-15 NDC per pixel, which can shift a
// depth by up to 0.06 NDC across a span of 2000 pixels: surfaces that cross
// far from the camera may then swap where they meet.
#ifndef SHAPOGFX3D_DEPTH_BITS
#define SHAPOGFX3D_DEPTH_BITS 32
#endif
static_assert(SHAPOGFX3D_DEPTH_BITS == 32 || SHAPOGFX3D_DEPTH_BITS == 16,
              "SHAPOGFX3D_DEPTH_BITS must be 32 or 16");

// Attribute put on the rasterization side -- render() and everything it
// calls per span -- so that a platform can place just that code somewhere
// fast. About 14 KB on a Cortex-M0+. The Pico SDK keeps .time_critical.*
// sections in RAM, so there
//   -DSHAPOGFX3D_HOT_ATTR='__attribute__((section(".time_critical.gfx3d")))'
// takes the span loops out of the XIP flash cache. The per-span functions
// are templates, which need SHAPOGFX3D_HOT_INSTANTIATE=1 as well (see the
// explicit instantiations next to the rasterizer tables).
#ifndef SHAPOGFX3D_HOT_ATTR
#define SHAPOGFX3D_HOT_ATTR
#endif
// Architecture hooks (SHAPOGFX3D_RP2_INTERP is decided there)
#include "../common/intmath.hpp"
#include "arch/arch.hpp"

namespace shapoco::gfx3d {

// ---------------------------------------------------------------------------
// Internal data structures
//
// Vertex processing (transform, lighting, projection) and primitive setup
// are done in float, or in fixed point with SHAPOGFX3D_FIXED_POINT. Either
// way a primitive ends up as a record of integers: its edges, and each
// attribute as a plane (a linear function of the pixel position). From there
// on -- building the spans of a scanline, resolving their overlaps and
// rasterizing them -- one implementation serves both builds, and it uses
// 32-bit integer arithmetic only.

namespace detail {

static constexpr int FIX_SHIFT = 16;
static constexpr float FIX_ONE = 65536.0f;

static constexpr int PERSPECTIVE_STEP = SHAPOGFX3D_PERSPECTIVE_STEP;
static_assert(PERSPECTIVE_STEP >= 2 &&
                  (PERSPECTIVE_STEP & (PERSPECTIVE_STEP - 1)) == 0,
              "SHAPOGFX3D_PERSPECTIVE_STEP must be a power of two");
static constexpr int GOURAUD_STEP = SHAPOGFX3D_GOURAUD_STEP;
static_assert(GOURAUD_STEP >= 1 && GOURAUD_STEP <= 16 &&
                  (GOURAUD_STEP & (GOURAUD_STEP - 1)) == 0,
              "SHAPOGFX3D_GOURAUD_STEP must be a power of two up to 16");

// Screen coordinates as stored (SHAPOGFX_COORD_BITS): a pixel position on
// the screen, and one that may be negative (clipped values, differences)
static constexpr int COORD_BITS = SHAPOGFX_COORD_BITS;
using ucoord_t = std::conditional_t<(COORD_BITS <= 8), uint8_t, uint16_t>;
using coord_t = std::conditional_t<(COORD_BITS <= 7), int8_t, int16_t>;

// Formats of the records, shared by both builds: screen coordinates 16.16 px,
// depth 8.24 (NDC; smaller = nearer), colors 8.16 (0..255), texture
// coordinates 16.16 texels. With perspective correction, 1/w is scaled per
// primitive by a power of two that puts its largest vertex value into
// [2^IW_NORM, 2^(IW_NORM + 1)) -- the scale cancels in u = (u/w) / (1/w) --
// and u/w is stored as u (16.16) x that scaled 1/w / 2^UW_SHIFT.
static constexpr int FP_SHIFT = 16;
static constexpr int32_t FP_HALF = 1 << 15;
// Guard band of the screen coordinates: at least twice the largest screen,
// and no more than 16.16 can hold
static constexpr int32_t SCREEN_MAX = (COORD_BITS <= 12 ? 8191 : 32767)
                                      << FP_SHIFT;
static constexpr int32_t SLOPE_MAX = INT32_MAX;  // |edge slope|, 16.16 px/row
static constexpr int Z_SHIFT = 24;
static constexpr int32_t Z_MAX_FP = 120 << Z_SHIFT;
static constexpr int32_t Z_DELTA_MAX_FP = 64 << Z_SHIFT;
static constexpr int32_t COLOR_MAX_FP = 255 << FIX_SHIFT;
static constexpr int32_t COLOR_GRAD_MAX = 16000 << FIX_SHIFT;
static constexpr int32_t TEX_MAX_FP = 30000 << FIX_SHIFT;
static constexpr int32_t TEX_GRAD_MAX = 16000 << FIX_SHIFT;
static constexpr int IW_NORM = 28;
static constexpr int UW_SHIFT = 30;
static constexpr int32_t IW_MAX_FP = INT32_MAX;
static constexpr int32_t UW_MAX_FP = INT32_MAX;

#if SHAPOGFX3D_FIXED_POINT
// Formats of the fixed-point vertex stage: screen and view-space coordinates
// 16.16 (pixels, model units), vertex colors 8.8 (0..255), 1/w Q26, depth
// 8.24, matrix entries Q18, normals Q15, the light direction in model space
// Q24.
static constexpr int COLOR_SHIFT = 8;
static constexpr int32_t COLOR_MAX = 255 << COLOR_SHIFT;
static constexpr int IW_SHIFT = 26;
static constexpr int MAT_SHIFT = 18;
static constexpr int NORMAL_SHIFT = 15;
static constexpr int LIGHT_SHIFT = 24;

// A vertex after lighting and projection
struct ShadedVertex {
  int32_t sx, sy;  // screen, 16.16 px
  int32_t z;       // NDC depth, 8.24 (smaller = nearer)
#if SHAPOGFX3D_TEXTURE
  int32_t u, v;  // texels, 16.16
#endif
  int32_t r, g, b;  // 0..255 in 8.8 (pre-multiplied by opacity for additive
                    // blending)
};
#else
static constexpr float Z_ONE = 16777216.0f;  // 8.24 fixed point depth

// A vertex after lighting and projection
struct ShadedVertex {
  float sx, sy;  // screen coordinates
  float zNdc;    // NDC depth (linear in screen space; smaller = nearer)
#if SHAPOGFX3D_TEXTURE
  float u, v;  // texture coordinates in texels
#endif
  float r, g, b;  // vertex color 0..255 (pre-multiplied by opacity for additive
                  // blending)
};
#endif

// Linear function of the pixel position, held as its value at the center of
// the record's reference pixel (TriHead::xa, yMin) and its per-pixel
// gradients, all in the attribute's own format. at() takes the offset of a
// pixel from the reference pixel and evaluates in wrapping 32-bit
// arithmetic: the true value at any pixel a primitive covers lies within the
// range of its vertex values, so the sum is right even where a product
// wraps. (A plane whose gradients would not fit their format is stored
// constant instead, see PlaneSet.)
struct Plane {
  int32_t a0, dx, dy;
  int32_t at(int ox, int oy) const {
    return (int32_t)((uint32_t)a0 + (uint32_t)dx * (uint32_t)ox +
                     (uint32_t)dy * (uint32_t)oy);
  }
};

// Texture format of a triangle; selects the rasterizer together with the blend
// mode and the flat flag: rasterFn = tex * 6 + blend * 2 + flat
enum class TexFmt : uint8_t {
  NONE = 0,
#if SHAPOGFX3D_TEXTURE
  GRAY1,
  RGB444,
  ARGB4444,
  RGB565BE,
  RGB565,
#endif
  COUNT
};
static constexpr int RASTER_PER_TEX = 6;  // blend modes (3) x flat (2)

namespace TriFlags {
constexpr uint8_t FLAT =
    1u << 0;  // all three vertex colors are equal (no color interpolation)
constexpr uint8_t TEX = 1u << 1;     // samples a texture
constexpr uint8_t OPAQUE = 1u << 2;  // opaque (BlendMode::NONE)
constexpr uint8_t LINE = 1u << 3;  // line segment [0] -> [1] (see makeLineSpan)
constexpr uint8_t POINT =
    1u << 4;  // point: sx/sy[0] = top-left pixel, slope[0] = size
constexpr uint8_t LEFT_LONG =
    1u << 5;  // triangles: the long edge (top->bottom) is on the left
constexpr uint8_t STEEP = 1u << 6;  // lines: |dy| >= |dx|
}  // namespace TriFlags

// Layer of a primitive, as stored in its record and in its spans: the index of
// the layer, or'ed with NO_DEPTH when the layer carries no depth plane.
// Spans are built in layer order, so a span whose layer byte differs from
// another one's belongs to a later layer and is therefore the nearer one.
namespace LayerId {
constexpr uint8_t INDEX = 0x7F;
constexpr uint8_t NO_DEPTH = 0x80;
}  // namespace LayerId

// Geometry of a primitive, in 16.16 px.
//
// Triangle: the x of each edge at the center of the first row it is used on,
// and its step per row. [0] top -> middle, from row yMin; [1] middle ->
// bottom, from row yMid; [2] top -> bottom (the long edge), from row yMin.
// The x of an edge on a row it covers is within the guard band, so it is
// evaluated in wrapping 32-bit arithmetic like a plane.
struct TriGeo {
  int32_t x[3];
  int32_t slope[3];
};
// Line segment a -> b, ay <= by. dxdy is the x step per row; xRef is the x at
// the center of row floor(ay) for a steep line and at y = floor(ay) + 1 for a
// shallow one (see makeLineSpan()).
struct LineGeo {
  int32_t ax, ay, bx, by;
  int32_t dxdy, xRef;
};
// Point: the leftmost column and the size of the square in pixels
struct PointGeo {
  int32_t x0, size;
};

// A triangle, line or point after setup.
//
// Only the attributes a primitive actually has are stored, so a record is 44
// to 112 bytes instead of always 112 (32-bit target, perspective level 1).
// Every record starts with the same header, which is all the sort, the
// scanline buckets and the span builders need; the optional parts follow in
// a fixed order and are reached through the record type, which the
// rasterizer and fragNearer() recover from the header.
struct TriHead {
  union {
    TriGeo tri;
    LineGeo line;
    PointGeo point;
  } geo;
  int16_t sortKey;     // ascending = farther first (see sortKeyOf())
  coord_t yMin, yMax;  // range of scanlines crossed (inclusive), on screen
  coord_t yMid;        // triangles: first row of edge [1]
  int16_t xa;          // column of the planes' reference pixel (row: yMin)
  uint8_t flags;       // TriFlags
  uint8_t alpha64;     // opacity (0..64)
  uint8_t rasterFn;    // index of the rasterizer (tex * 6 + blend * 2 + flat)
  uint8_t layer;       // LayerId
};

// Optional parts of a record, in this order. The two "absent" types differ so
// that a record can leave out both.
struct PartNoZ {};
struct PartNoTex {};
struct PartZ {
#if SHAPOGFX3D_DEPTH_BITS == 16
  // NDC depth: the value at the reference pixel in 2.14 (the reference pixel
  // may lie just outside the primitive, beyond -1..1) and the gradients in
  // 1.(15 + zsh), the shift chosen per record so that the larger gradient
  // uses the 16 bits; evaluated in 1.15 (see depthAt())
  int16_t z0, zdx, zdy;
  uint8_t zsh;  // 0..14
#else
  Plane z;  // NDC depth, 8.24
#endif
};
// Vertex colors: the values at the reference pixel in 10.6 (0..255 plus the
// margin of a reference pixel next to the primitive) and the gradients in
// 8.8, evaluated in 8.8 (see colorAt()). Gradients beyond +-127 per pixel do
// not fit; such a primitive (a sliver) is stored flat instead.
struct PartSmooth {
  int16_t c0[3];  // r, g, b
  int16_t dx[3], dy[3];
};
struct PartFlat {
  uint8_t r, g, b;  // one color for the whole primitive (0..255)
};
struct PartTex {
  const Texture *tex;
#if SHAPOGFX3D_PERSPECTIVE >= 1
  Plane uw, vw, iw;  // (u/w, v/w, 1/w), see IW_NORM
#else
  Plane u, v;  // texels, 16.16
#endif
};

// D: the layer has a depth plane, G: interpolated (Gouraud) color, T: textured
template <bool D, bool G, bool T>
struct TriRec : TriHead,
                std::conditional_t<D, PartZ, PartNoZ>,
                std::conditional_t<G, PartSmooth, PartFlat>,
                std::conditional_t<T, PartTex, PartNoTex> {
  static constexpr bool HAS_DEPTH = D;
  static constexpr bool SMOOTH = G;
  static constexpr bool TEXTURED = T;
};

// Records are addressed by a 4-byte-unit offset from the start of the region,
// which is what limits the triangle buffer to 256 KB.
static constexpr size_t REC_ALIGN = alignof(TriHead) > 4 ? alignof(TriHead) : 4;
static constexpr size_t REC_UNIT = 4;
static constexpr size_t REC_REGION_MAX = 0xFFFFu * REC_UNIT;

// Size of each record layout, indexed by recIndex()
static constexpr size_t TRI_REC_SIZE[8] = {
    sizeof(TriRec<false, false, false>), sizeof(TriRec<false, false, true>),
    sizeof(TriRec<false, true, false>),  sizeof(TriRec<false, true, true>),
    sizeof(TriRec<true, false, false>),  sizeof(TriRec<true, false, true>),
    sizeof(TriRec<true, true, false>),   sizeof(TriRec<true, true, true>),
};
static inline int recIndex(bool depth, bool smooth, bool tex) {
  return (depth ? 4 : 0) | (smooth ? 2 : 0) | (tex ? 1 : 0);
}

// One entry per primitive: the offset of its record from the start of the
// region, in 4 bytes. beginRender() sorts the entries of each layer by depth.
// Each render context keeps a scanline-list link per entry as well (see
// RenderContext::link), which is where the rest of an entry's bytes go.
using TriEntry = uint16_t;

struct LayerDesc {
  int32_t first;  // index of the first entry of the layer
  uint8_t id;     // LayerId byte stored in the records and spans
};

// A span on a scanline: a pixel range and the primitive covering it. Its
// attributes are evaluated from the record's planes when it is drawn (and
// its depth where two spans overlap), so cutting a span only moves its ends.
struct Span {
  coord_t x0, x1;  // pixel range [x0, x1)
  uint8_t lay;     // LayerId of the primitive (see fragNearer())
  const TriHead *tri;
  Span *next;
};

// Attributes of a primitive that are linear in screen space, as its setup
// computes them (in the record formats), and the planes it hands to
// storePrimitive(). The flat color replaces r, g, b where they are equal.
// Without texturing the texture attributes take no room (A_COUNT stops
// before them).
enum : int {
  A_Z,
  A_R,
  A_G,
  A_B,
  A_T0,
  A_T1,
  A_T2,
  A_COUNT = SHAPOGFX3D_TEXTURE ? 7 : 4
};
struct PlaneSet {
  Plane p[A_COUNT];  // A_T*: (u/w, v/w, 1/w), or (u, v) at level 0
  uint8_t fr, fg, fb;
  const Texture *tex;  // textured primitives
};

// The state of one render() call: its span pool and span lists, the
// per-scanline triangle lists and the links of the active triangle list. A
// renderer holds Config::renderContexts of them, so that as many render()
// calls may run at the same time -- one per core, each on its own band.
struct RenderContext {
  Span *pool;
  int count;     // spans used on the current scanline
  int peak;      // most spans used on a scanline (reset by beginRender())
  int dropped;   // spans dropped because the pool overflowed
  Span *opaque;  // ascending x, non-overlapping
  Span *transHead, *transTail;        // insertion order (farthest first)
  uint16_t *bucketHead, *bucketTail;  // per scanline: entries starting there
  uint16_t *link;  // per entry: the next of its list (placed by beginRender())
};

struct StackEntry {
  mat4f matrix;
  const Material *material;
};

// Direct-mapped cache of transformed vertices, reused within putPrimitive()
struct CachedVertex {
  ShadedVertex sv;
#if SHAPOGFX3D_FIXED_POINT
  int32_t invW;   // 1/w, Q26
  int32_t viewZ;  // 16.16
#else
  float invW;
  float viewZ;
#endif
  uint16_t tag;  // vertex index (NONE = empty)
  bool ok;  // false: in front of the near plane (the whole triangle is dropped)
};

// Vertex of a point or line: view-space position and unlit color (0..255)
struct UnlitVertex {
#if SHAPOGFX3D_FIXED_POINT
  int32_t vx, vy, vz;  // 16.16
  int32_t r, g, b;     // 8.8
#else
  vec3f view;
  float r, g, b;
#endif
};

#if SHAPOGFX3D_FIXED_POINT
// A vertex as the fixed-point stage reads it, from any of the three input
// forms (vertexAtQ())
struct VertexQ {
  int32_t p[3];  // model units, 16.16
  int32_t n[3];  // Q15
  int32_t u, v;  // texture-space units, 16.16 (texels after x texture size)
  gfx2d::Color color;
};
#endif

// Per-primitive constants of the vertex stage
struct PrimSetup {
  const Material *mat;
  const Texture *tex;  // nullptr when untextured
  float texW, texH;
  bool envMap;
  bool lit;          // some light is enabled
  bool viewNormal;   // the view-space normal is needed (environment map, or a
                     // light with a non-uniformly scaled matrix)
  vec3f lightModel;  // light direction (towards the light) in model space,
                     // scaled so that dot(normal, lightModel) is the diffuse
                     // factor; valid when lit && !viewNormal
#if SHAPOGFX3D_FIXED_POINT
  int32_t lightModelQ[3];  // lightModel, Q24
  int32_t amb[3], dif[3];  // material colors, 8.8
  int32_t alpha256;        // opacity, 0..256
  int32_t texWq, texHq;    // texture size in texels (integers)
  bool vertexColor, add;
#endif
};

static constexpr int STACK_DEPTH = SHAPOGFX3D_STACK_DEPTH;
static constexpr int LAYER_MAX = SHAPOGFX3D_LAYER_MAX;
static constexpr int SPAN_CAPACITY_MIN = 32;
static constexpr int SPAN_CAPACITY_MAX = 512;
static constexpr int RENDER_CONTEXTS_MAX = 4;
static constexpr int VCACHE_SIZE = SHAPOGFX3D_VCACHE_SIZE;
static constexpr uint16_t NONE = 0xFFFF;

static_assert(STACK_DEPTH >= 1, "SHAPOGFX3D_STACK_DEPTH must be at least 1");
static_assert(LAYER_MAX >= 1 && LAYER_MAX <= (int)LayerId::INDEX + 1,
              "SHAPOGFX3D_LAYER_MAX must be between 1 and 128");
static_assert(VCACHE_SIZE >= 1 && (VCACHE_SIZE & (VCACHE_SIZE - 1)) == 0,
              "SHAPOGFX3D_VCACHE_SIZE must be a power of two");

}  // namespace detail

using namespace detail;

// ---------------------------------------------------------------------------
// Utilities

static inline uintptr_t alignUp8(uintptr_t p) {
  return (p + 7u) & ~(uintptr_t)7u;
}

// NaN maps to lo, so a degenerate value never reaches a float-to-int cast
static inline float clampf(float v, float lo, float hi) {
  return !(v > lo) ? lo : (v > hi ? hi : v);
}

// Float to int for screen coordinates (the range is limited first)
static constexpr float COORD_MAX = 1e8f;
static inline int floorInt(float v) {
  return (int)std::floor(clampf(v, -COORD_MAX, COORD_MAX));
}
static inline int ceilInt(float v) {
  return (int)std::ceil(clampf(v, -COORD_MAX, COORD_MAX));
}

// v x scale rounded to an integer, saturating at +-maxAbs; NaN maps to 0.
// Converts the float setup's results to the record formats.
static inline int32_t fixF(float v, float scale, int32_t maxAbs) {
  const float s = v * scale;
  const float lim = (float)maxAbs;
  if (s != s) return 0;
  if (s <= -lim) return -maxAbs;
  if (s >= lim) return maxAbs;
  return (int32_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

static inline int32_t clampFix(int64_t v, int32_t maxAbs) {
  return v < -maxAbs ? -maxAbs : (v > maxAbs ? maxAbs : (int32_t)v);
}

// a * b for 64-bit operands that nearly always fit 32 bits (the setup's
// coordinate differences and gradients): with SHAPOGFX_ARCH_SPLIT_MUL64 the
// product is formed inline when they do, instead of by a library call
static inline int64_t mulFit(int64_t a, int64_t b) {
#if SHAPOGFX_ARCH_SPLIT_MUL64
  if (a == (int32_t)a && b == (int32_t)b)
    return arch::mul64((int32_t)a, (int32_t)b);
#endif
  return a * b;
}

// floor(log2(v)) of a positive float, from its bits (no library call)
static inline int floatExponent(float v) {
  uint32_t i;
  std::memcpy(&i, &v, sizeof(i));
  return (int)((i >> 23) & 0xFFu) - 127;
}
// 2^k for -126 <= k <= 127, from its bits
static inline float pow2f(int k) {
  k = k < -126 ? -126 : (k > 127 ? 127 : k);
  const uint32_t i = (uint32_t)(k + 127) << 23;
  float v;
  std::memcpy(&v, &i, sizeof(v));
  return v;
}

// Sort key of a primitive (TriHead::sortKey) from the average view-space z
// of its vertices: 16 bits with the ordering of z. The float build keeps the
// top half of an integer with the ordering of the float (sign, exponent and
// 7 bits of mantissa); the fixed-point build keeps a 5-bit exponent and 10
// bits of mantissa of the 16.16 value. Primitives whose keys are equal keep
// the order they were added in.
static inline int16_t sortKeyOf(float z) {
  int32_t i;
  std::memcpy(&i, &z, sizeof(i));
  return (int16_t)((i ^ (int32_t)(((uint32_t)(i >> 31)) >> 1)) >> 16);
}
static inline int16_t sortKeyOf(int32_t z) {
  const uint32_t m = z < 0 ? (uint32_t)0 - (uint32_t)z : (uint32_t)z;
  int code = (int)m;
  if (m >= 1024) {
    const int e = 22 - __builtin_clz(m);  // 1..22
    code = (e << 10) | (int)((m >> (e - 1)) & 0x3FFu);
  }
  return (int16_t)(z < 0 ? -code : code);
}

// --- Fixed-point helpers ----------------------------------------------------
// (the fixed-point vertex stage and, in both builds, the setup of lines)

// float -> fixed with `shift` fraction bits, saturating; NaN maps to 0
static inline int32_t fToFix(float v, int shift, int32_t maxAbs) {
  const float s = v * (float)((int64_t)1 << shift);
  const float lim = (float)maxAbs;
  if (s != s) return 0;
  if (s <= -lim) return -maxAbs;
  if (s >= lim) return maxAbs;
  return (int32_t)s;
}
// Leading zeros of a nonzero 64-bit value, from 32-bit counts (one
// instruction each from the Cortex-M3 on)
static inline int clz64(uint64_t v) {
  const uint32_t hi = (uint32_t)(v >> 32);
  return hi ? __builtin_clz(hi) : 32 + __builtin_clz((uint32_t)v);
}

// A divisor as a normalized 16-bit reciprocal, so that several dividends can
// share the division: 1 / |den| ~= rcp * 2^(ds - 79). It takes one 32-bit
// division, and a quotient (mulRcp()) one 16 x 16 -> 32-bit product, for
// about 15 significant bits -- plenty for what the setup divides (1/w, edge
// slopes, plane gradients), and no 64-bit multiplication on a core without
// one.
struct Rcp {
  uint32_t rcp;  // (2^15, 2^16)
  int ds;
  bool neg, zero;
};
static inline Rcp makeRcp(int64_t den) {
  Rcp r;
  r.zero = (den == 0);
  r.neg = den < 0;
  if (r.zero) {
    r.rcp = 0;
    r.ds = 0;
    return r;
  }
  const uint64_t u = r.neg ? (uint64_t)0 - (uint64_t)den : (uint64_t)den;
  r.ds = clz64(u);
  r.rcp = 0xFFFFFFFFu / (uint32_t)((u << r.ds) >> 47);  // / [2^16, 2^17)
  return r;
}
// num / den * 2^shift, saturating at +-maxAbs
static inline int32_t mulRcp(int64_t num, const Rcp &d, int shift,
                             int32_t maxAbs) {
  if (num == 0 || d.zero) return 0;
  const bool neg = (num < 0) != d.neg;
  const uint64_t u = num < 0 ? (uint64_t)0 - (uint64_t)num : (uint64_t)num;
  const int ns = clz64(u);
  // The top 16 bits of the dividend times the reciprocal
  const uint32_t p = (uint32_t)((u << ns) >> 48) * d.rcp;
  const int e = d.ds - ns - 31 + shift;  // num / den * 2^shift = p * 2^e
  uint64_t r;
  if (e >= 32 || (e > 0 && (p >> (32 - e)) != 0)) {
    r = (uint64_t)maxAbs;
  } else if (e >= 0) {
    r = (uint64_t)p << e;
  } else {
    r = (-e >= 32) ? 0 : (p >> -e);
  }
  if (r > (uint64_t)maxAbs) r = (uint64_t)maxAbs;
  return neg ? -(int32_t)r : (int32_t)r;
}
__attribute__((noinline)) static int32_t divQ(int64_t num, int64_t den,
                                              int shift, int32_t maxAbs) {
  return mulRcp(num, makeRcp(den), shift, maxAbs);
}

#if SHAPOGFX3D_FIXED_POINT
static inline int32_t clampColorFP(int32_t v) {
  return v < 0 ? 0 : (v > COLOR_MAX ? COLOR_MAX : v);
}

using gfx::intmath::isqrt32;
// A vector of any length (components up to 2^30) to a Q15 unit vector
static inline void normalizeQ15(int64_t x, int64_t y, int64_t z,
                                int32_t out[3]) {
  const uint64_t len2 = (uint64_t)(x * x + y * y + z * z);
  if (len2 == 0) {
    out[0] = out[1] = 0;
    out[2] = 1 << NORMAL_SHIFT;
    return;
  }
  int sh = 0;
  while ((len2 >> sh) >= ((uint64_t)1 << 32)) sh += 2;
  const int64_t len = (int64_t)isqrt32((uint32_t)(len2 >> sh)) << (sh / 2);
  const Rcp r = makeRcp(len);
  out[0] = mulRcp(x, r, NORMAL_SHIFT, 1 << NORMAL_SHIFT);
  out[1] = mulRcp(y, r, NORMAL_SHIFT, 1 << NORMAL_SHIFT);
  out[2] = mulRcp(z, r, NORMAL_SHIFT, 1 << NORMAL_SHIFT);
}
#endif  // SHAPOGFX3D_FIXED_POINT

// Texture format usable by the rasterizer; NONE for disabled formats
static inline TexFmt texFmtOf(const Texture *tex) {
#if !SHAPOGFX3D_TEXTURE
  (void)tex;
  return TexFmt::NONE;
#else
  if (!tex || !tex->pixels) return TexFmt::NONE;
  switch (tex->format) {
#if SHAPOGFX_FORMAT_GRAY1
    case PixelFormat::GRAY1: return TexFmt::GRAY1;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444: return TexFmt::RGB444;
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    case PixelFormat::ARGB4444: return TexFmt::ARGB4444;
#endif
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE: return TexFmt::RGB565BE;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565: return TexFmt::RGB565;
#endif
    default: return TexFmt::NONE;
  }
#endif
}

// True for a texture format with per-texel alpha (none, without texturing)
static constexpr bool texFmtHasAlpha(TexFmt f) {
#if SHAPOGFX3D_TEXTURE
  return f == TexFmt::ARGB4444;
#else
  (void)f;
  return false;
#endif
}

// The texture actually used by a material (nullptr if unused or unsupported)
static inline const Texture *materialTexture(const Material *mat) {
#if !SHAPOGFX3D_TEXTURE
  (void)mat;
  return nullptr;
#else
  const Texture *tex =
      (mat->flags & (MaterialFlags::TEXTURE | MaterialFlags::ENV_MAP))
          ? mat->texture
          : nullptr;
  return texFmtOf(tex) != TexFmt::NONE ? tex : nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Initialization

void Graphics3D::init(const Config &cfg) {
  *this = Graphics3D();
  const int16_t w = cfg.screenWidth, h = cfg.screenHeight;
  screenW_ = w;
  screenH_ = h;
  arenaSize_ = cfg.arenaSize;
  if (w <= 0 || h <= 0 || w > SHAPOGFX_COORD_MAX || h > SHAPOGFX_COORD_MAX ||
      !cfg.arena) {
    *this = Graphics3D();
    return;
  }

  uintptr_t p = (uintptr_t)cfg.arena;
  const uintptr_t end = p + cfg.arenaSize;
  p = alignUp8(p);
  auto avail = [&]() -> size_t { return (end > p) ? (size_t)(end - p) : 0; };

  // Render contexts, each with its line buckets (screenH x 2)
  const int nctx =
      std::max(1, std::min(RENDER_CONTEXTS_MAX, (int)cfg.renderContexts));
  const size_t ctxBytes = alignUp8((size_t)nctx * sizeof(RenderContext));
  const size_t bucketBytes = alignUp8((size_t)h * 2 * sizeof(uint16_t));
  // Fixed allocations: layer table, matrix stack, vertex cache
  const size_t layerBytes = alignUp8((size_t)LAYER_MAX * sizeof(LayerDesc));
  const size_t stackBytes = alignUp8((size_t)STACK_DEPTH * sizeof(StackEntry));
  const size_t vcacheBytes =
      alignUp8((size_t)VCACHE_SIZE * sizeof(CachedVertex));
  if (avail() <
      ctxBytes + nctx * bucketBytes + layerBytes + stackBytes + vcacheBytes) {
    *this = Graphics3D();
    return;
  }
  contexts_ = (RenderContext *)p;
  contextCount_ = nctx;
  p += ctxBytes;
  for (int c = 0; c < nctx; c++) {
    RenderContext &rc = contexts_[c];
    rc = RenderContext();
    rc.bucketHead = (uint16_t *)p;
    rc.bucketTail = rc.bucketHead + h;
    p += bucketBytes;
  }
  layers_ = (LayerDesc *)p;
  p += layerBytes;
  stack_ = (StackEntry *)p;
  p += stackBytes;
  vcache_ = (CachedVertex *)p;
  p += vcacheBytes;
  arenaFixed_ = (size_t)(p - (uintptr_t)cfg.arena);

  // Span pools, one per context: as requested, or a quarter of the
  // remaining space shared among the contexts
  int spanCap = cfg.spanCapacity;
  if (spanCap <= 0) {
    spanCap = (int)((avail() / 4) / (sizeof(Span) * nctx));
    spanCap = std::max(SPAN_CAPACITY_MIN, std::min(SPAN_CAPACITY_MAX, spanCap));
  }
  spanCap = std::min(spanCap, (int)(avail() / (sizeof(Span) * nctx)));
  spanCapacity_ = spanCap;
  for (int c = 0; c < nctx; c++) {
    contexts_[c].pool = (Span *)p;
    p += (size_t)spanCap * sizeof(Span);
  }
  p = alignUp8(p);

  // Triangle buffer: everything that remains, entries growing up from its
  // start and records down from its end
  size_t region = avail();
  if (region > REC_REGION_MAX) region = REC_REGION_MAX;
  region &= ~(size_t)(REC_ALIGN - 1);
  recBase_ = (uint8_t *)p;
  entries_ = (TriEntry *)p;
  entryBytes_ = (int)sizeof(TriEntry) + nctx * (int)sizeof(uint16_t);
  recEnd_ = recBase_ + region;
  recTop_ = recEnd_;
  if (spanCap <= 0 || region < (size_t)entryBytes_ + TRI_REC_SIZE[7]) {
    *this = Graphics3D();
    return;
  }
}

void Graphics3D::deinit() { *this = Graphics3D(); }

// ---------------------------------------------------------------------------
// Scene construction

void Graphics3D::beginScene() {
  triCount_ = 0;
  triDropped_ = 0;
  badIndices_ = 0;
  nodesDropped_ = 0;
  stackTop_ = 0;
  recTop_ = recEnd_;
  layerCount_ = 0;
  layersDropped_ = 0;
  layerFlags_ = 0;
  layerOpen_ = false;
  cur_ = mat4f::identity();
  curQDirty_ = true;
}

void Graphics3D::endScene() { layerOpen_ = false; }

void Graphics3D::beginLayer(uint32_t flags) {
  if (layerCount_ >= LAYER_MAX) {
    layersDropped_++;  // no free layer: what follows stays in the current one
    return;
  }
  layerOpen_ = false;  // opened by the next primitive
  layerFlags_ = flags;
}

void Graphics3D::endLayer() {
  layerOpen_ = false;
  layerFlags_ = 0;
}

// Layer byte of the primitive being emitted; opens a layer when none is
uint8_t Graphics3D::layerByte() {
  if (!layerOpen_) {
    if (layerCount_ < LAYER_MAX) {
      LayerDesc &l = layers_[layerCount_];
      l.first = triCount_;
      l.id = (uint8_t)(layerCount_ |
                       ((layerFlags_ & LayerFlags::NO_DEPTH) ? LayerId::NO_DEPTH
                                                             : 0));
      layerCount_++;
    }
    layerOpen_ = true;
  }
  return layers_[layerCount_ - 1].id;
}

// Reserve a record and register its entry. Records are packed downwards from
// the end of the region and the entries upwards from its start, so the buffer
// is full when the two meet. The space between them keeps room for the links
// of every render context, which beginRender() places there.
uint8_t *Graphics3D::allocRecord(size_t size) {
  if (!recBase_ || triCount_ >= (int)NONE) return nullptr;
  uint8_t *rec = recTop_ - size;
  if (rec < recBase_ + (size_t)(triCount_ + 1) * (size_t)entryBytes_) {
    return nullptr;
  }
  recTop_ = rec;
  entries_[triCount_] = (TriEntry)((size_t)(rec - recBase_) / REC_UNIT);
  return rec;
}

void Graphics3D::loadIdentity() {
  cur_ = mat4f::identity();
  curQDirty_ = true;
}

void Graphics3D::translate(const vec3f &v) {
  cur_ = cur_ * mat4f::translation(v.x, v.y, v.z);
  curQDirty_ = true;
}
void Graphics3D::translate(float x, float y, float z) {
  cur_ = cur_ * mat4f::translation(x, y, z);
  curQDirty_ = true;
}
void Graphics3D::rotate(float angle, const vec3f &axis) {
  cur_ = cur_ * mat4f::rotation(angle, axis);
  curQDirty_ = true;
}
void Graphics3D::rotate(float angle, float x, float y, float z) {
  cur_ = cur_ * mat4f::rotation(angle, {x, y, z});
  curQDirty_ = true;
}
void Graphics3D::scale(const vec3f &v) {
  cur_ = cur_ * mat4f::scaling(v.x, v.y, v.z);
  curQDirty_ = true;
}
void Graphics3D::scale(float x, float y, float z) {
  cur_ = cur_ * mat4f::scaling(x, y, z);
  curQDirty_ = true;
}
void Graphics3D::transform(const mat4f &m) {
  cur_ = cur_ * m;
  curQDirty_ = true;
}
void Graphics3D::lookAt(const vec3f &eye, const vec3f &target,
                        const vec3f &up) {
  cur_ = cur_ * mat4f::lookAt(eye, target, up);
  curQDirty_ = true;
}

bool Graphics3D::pushState() {
  if (!stack_ || stackTop_ >= STACK_DEPTH) return false;
  stack_[stackTop_].matrix = cur_;
  stack_[stackTop_].material = curMat_;
  stackTop_++;
  return true;
}

void Graphics3D::popState() {
  if (!stack_ || stackTop_ <= 0) return;
  stackTop_--;
  cur_ = stack_[stackTop_].matrix;
  curQDirty_ = true;
  curMat_ = stack_[stackTop_].material;
}

void Graphics3D::setMaterial(const Material &mat) { curMat_ = &mat; }

void Graphics3D::enableParallelLight(const vec3f &dir, const colorf &col) {
  lightEnabled_ = true;
  lightDir_ = normalize(cur_.transformDir(dir));
  lightCol_ = col;
#if SHAPOGFX3D_FIXED_POINT
  lightQ_.dir[0] = fToFix(lightDir_.x, NORMAL_SHIFT, 1 << NORMAL_SHIFT);
  lightQ_.dir[1] = fToFix(lightDir_.y, NORMAL_SHIFT, 1 << NORMAL_SHIFT);
  lightQ_.dir[2] = fToFix(lightDir_.z, NORMAL_SHIFT, 1 << NORMAL_SHIFT);
  lightQ_.col[0] = fToFix(col.r, COLOR_SHIFT, 1 << 15);
  lightQ_.col[1] = fToFix(col.g, COLOR_SHIFT, 1 << 15);
  lightQ_.col[2] = fToFix(col.b, COLOR_SHIFT, 1 << 15);
#endif
}

void Graphics3D::disableParallelLight() { lightEnabled_ = false; }

void Graphics3D::enableEnvironmentLight(const colorf &col) {
  envEnabled_ = true;
  envCol_ = col;
#if SHAPOGFX3D_FIXED_POINT
  lightQ_.env[0] = fToFix(col.r, COLOR_SHIFT, 1 << 15);
  lightQ_.env[1] = fToFix(col.g, COLOR_SHIFT, 1 << 15);
  lightQ_.env[2] = fToFix(col.b, COLOR_SHIFT, 1 << 15);
#endif
}

void Graphics3D::disableEnvironmentLight() { envEnabled_ = false; }

void Graphics3D::setClearColor(const colorf &col) {
  clearColor_ = col;
  clearColor8_ = gfx2d::makeColorF(col);
  clearEnabled_ = true;
}

void Graphics3D::disableClear() { clearEnabled_ = false; }

void Graphics3D::setPerspectiveProjection(float fovY, float aspect, float zNear,
                                          float zFar) {
  proj_ = mat4f::perspective(fovY, aspect, zNear, zFar);
  projKind_ = ProjKind::PERSPECTIVE;
  zNear_ = zNear;
  projQDirty_ = true;
}

void Graphics3D::setOrthographicProjection(float left, float right,
                                           float bottom, float top, float zNear,
                                           float zFar) {
  proj_ = mat4f::orthographic(left, right, bottom, top, zNear, zFar);
  projKind_ = ProjKind::ORTHOGRAPHIC;
  zNear_ = zNear;
  projQDirty_ = true;
}

// ---------------------------------------------------------------------------
// Vertex processing (transform + lighting + projection)

#if SHAPOGFX3D_FIXED_POINT

// Bring the fixed-point copies of the current matrix and the projection up
// to date. The matrix changes with every transform call, the projection with
// the setters; both are converted once, not per vertex.
void Graphics3D::refreshFixed() {
  if (curQDirty_) {
    for (int c = 0; c < 3; c++) {
      for (int r = 0; r < 3; r++) {
        curQ_.r[c * 3 + r] = fToFix(cur_.m[c * 4 + r], MAT_SHIFT, INT32_MAX);
      }
    }
    for (int r = 0; r < 3; r++) {
      curQ_.t[r] = fToFix(cur_.m[12 + r], FP_SHIFT, INT32_MAX);
    }
    curQDirty_ = false;
  }
  if (projQDirty_) {
    ProjQ &p = projQ_;
    const float hw = screenW_ * 0.5f, hh = screenH_ * 0.5f;
    p.kind = (uint8_t)projKind_;
    p.zNear = fToFix(zNear_, FP_SHIFT, INT32_MAX);
    p.cx = (int32_t)screenW_ << (FP_SHIFT - 1);
    p.cy = (int32_t)screenH_ << (FP_SHIFT - 1);
    // Perspective: sx = cx + (x / w) * m[0] * (W / 2); z = m[10] + m[14] / w
    p.fx = fToFix(proj_.m[0] * hw, 8, INT32_MAX);
    p.fy = fToFix(proj_.m[5] * hh, 8, INT32_MAX);
    p.zA = fToFix(proj_.m[10], 24, INT32_MAX);
    p.zB = fToFix(proj_.m[14], FP_SHIFT, INT32_MAX);
    // Orthographic: sx = x * m[0] * (W / 2) + (m[12] / 2 + 1 / 2) * W
    p.sxScale = fToFix(proj_.m[0] * hw, 16, INT32_MAX);
    p.syScale = fToFix(-proj_.m[5] * hh, 16, INT32_MAX);
    p.sxOff =
        fToFix((proj_.m[12] * 0.5f + 0.5f) * screenW_, FP_SHIFT, INT32_MAX);
    p.syOff =
        fToFix((0.5f - proj_.m[13] * 0.5f) * screenH_, FP_SHIFT, INT32_MAX);
    p.zScale = fToFix(proj_.m[10], 24, INT32_MAX);
    p.zOff = fToFix(proj_.m[14], 24, INT32_MAX);
    for (int i = 0; i < 16; i++) {
      p.m[i] = fToFix(proj_.m[i], MAT_SHIFT, INT32_MAX);
    }
    projQDirty_ = false;
  }
}

// Project a view-space point (16.16) to the screen (16.16 px), the NDC depth
// (8.24) and 1/w (Q26). Returns false when w <= 0.
bool Graphics3D::projectQ(int32_t vx, int32_t vy, int32_t vz, ShadedVertex &sv,
                          int32_t &invW) const {
  const ProjQ &p = projQ_;
  switch ((ProjKind)p.kind) {
    case ProjKind::PERSPECTIVE: {
      const int32_t w = -vz;
      if (w <= 0) return false;
      invW = divQ(1, w, IW_SHIFT + FP_SHIFT, INT32_MAX);  // 2^26 / (w units)
      const int64_t xr = clampFix((arch::mul64(vx, invW)) >> IW_SHIFT, 1 << 27);
      const int64_t yr = clampFix((arch::mul64(vy, invW)) >> IW_SHIFT, 1 << 27);
      sv.sx =
          clampFix(p.cx + (arch::mul64((int32_t)xr, p.fx) >> 8), SCREEN_MAX);
      sv.sy =
          clampFix(p.cy - (arch::mul64((int32_t)yr, p.fy) >> 8), SCREEN_MAX);
      sv.z = clampFix((int64_t)p.zA + ((arch::mul64(p.zB, invW)) >>
                                       (FP_SHIFT + IW_SHIFT - 24)),
                      Z_MAX_FP);
      return true;
    }
    case ProjKind::ORTHOGRAPHIC: {
      sv.sx =
          clampFix(((arch::mul64(vx, p.sxScale)) >> 16) + p.sxOff, SCREEN_MAX);
      sv.sy =
          clampFix(((arch::mul64(vy, p.syScale)) >> 16) + p.syOff, SCREEN_MAX);
      sv.z = clampFix(((arch::mul64(vz, p.zScale)) >> FP_SHIFT) + p.zOff,
                      Z_MAX_FP);
      invW = 1 << IW_SHIFT;
      return true;
    }
    default: {
      // Any matrix: Q18 entries on a 16.16 point
      auto row = [&](int r) -> int64_t {
        return ((arch::mul64(p.m[r], vx) + arch::mul64(p.m[4 + r], vy) +
                 arch::mul64(p.m[8 + r], vz)) >>
                MAT_SHIFT) +
               ((int64_t)p.m[12 + r] >> (MAT_SHIFT - FP_SHIFT));
      };
      const int64_t cx = row(0), cy = row(1), cz = row(2), w = row(3);
      if (w <= 0) return false;
      const int32_t rx = divQ(cx, w, FP_SHIFT, 1 << 27);
      const int32_t ry = divQ(cy, w, FP_SHIFT, 1 << 27);
      sv.sx = clampFix(p.cx + ((arch::mul64(rx, screenW_)) >> 1), SCREEN_MAX);
      sv.sy = clampFix(p.cy - ((arch::mul64(ry, screenH_)) >> 1), SCREEN_MAX);
      sv.z = divQ(cz, w, 24, Z_MAX_FP);
      invW = divQ(1, w, IW_SHIFT + FP_SHIFT, INT32_MAX);
      return true;
    }
  }
}

// The current matrix (Q18 / 16.16) on a 16.16 position -> view space 16.16
static inline void transformQ(const MatQ &m, const int32_t p[3], int32_t &vx,
                              int32_t &vy, int32_t &vz) {
  const int32_t px = p[0], py = p[1], pz = p[2];
  vx = (int32_t)(((arch::mul64(m.r[0], px) + arch::mul64(m.r[3], py) +
                   arch::mul64(m.r[6], pz)) >>
                  MAT_SHIFT) +
                 m.t[0]);
  vy = (int32_t)(((arch::mul64(m.r[1], px) + arch::mul64(m.r[4], py) +
                   arch::mul64(m.r[7], pz)) >>
                  MAT_SHIFT) +
                 m.t[1]);
  vz = (int32_t)(((arch::mul64(m.r[2], px) + arch::mul64(m.r[5], py) +
                   arch::mul64(m.r[8], pz)) >>
                  MAT_SHIFT) +
                 m.t[2]);
}

// A material color (0..1, Q8) times the vertex color, as 8.8 of 0..255
static inline int32_t vertexColorQ(int32_t unitQ8, uint32_t c8, bool useVertex,
                                   bool add, int32_t alpha256) {
  int32_t r = unitQ8 * 255;  // Q8 of 0..1 -> 8.8 of 0..255
  if (useVertex)
    r = (int32_t)(arch::mul64(r, (int32_t)(c8 * 257)) >> 16);  // x c / 255
  if (add) r = (int32_t)((arch::mul64(r, alpha256)) >> 8);
  return clampColorFP(r);
}

#else
void Graphics3D::refreshFixed() {}
bool Graphics3D::projectQ(int32_t, int32_t, int32_t, ShadedVertex &,
                          int32_t &) const {
  return false;
}
#endif  // SHAPOGFX3D_FIXED_POINT

// Project a view-space point to the screen. Returns false when w <= 0.
// The projection matrices built by the two setters are sparse, so only their
// non-zero elements are used.
bool Graphics3D::projectPoint(const vec3f &p, float &sx, float &sy, float &zNdc,
                              float &invW) const {
  const float *m = proj_.m;
  float cx, cy, cz, w;
  switch (projKind_) {
    case ProjKind::PERSPECTIVE:
      cx = m[0] * p.x;
      cy = m[5] * p.y;
      cz = m[10] * p.z + m[14];
      w = -p.z;
      break;
    case ProjKind::ORTHOGRAPHIC:
      cx = m[0] * p.x + m[12];
      cy = m[5] * p.y + m[13];
      cz = m[10] * p.z + m[14];
      w = 1.0f;
      break;
    default: {
      vec3f c = proj_.transformPoint4(p, w);
      cx = c.x;
      cy = c.y;
      cz = c.z;
      break;
    }
  }
  if (w <= 0.0f) return false;
  invW = 1.0f / w;
  sx = (cx * invW * 0.5f + 0.5f) * screenW_;
  sy = (0.5f - cy * invW * 0.5f) * screenH_;
  zNdc = cz * invW;
  return true;
}

// Light direction in model space. Possible when the upper 3x3 of the model
// matrix is a rotation times a uniform scale (columns orthogonal and of equal
// length); then dot(n, out) equals the diffuse factor of the normalized
// view-space normal for a unit model-space normal n. Returns false otherwise.
static bool lightToModelSpace(const mat4f &m, const vec3f &lightDirView,
                              vec3f &out) {
  const vec3f c0 = {m.m[0], m.m[1], m.m[2]};
  const vec3f c1 = {m.m[4], m.m[5], m.m[6]};
  const vec3f c2 = {m.m[8], m.m[9], m.m[10]};
  const float l0 = dot(c0, c0), l1 = dot(c1, c1), l2 = dot(c2, c2);
  if (l0 <= 0.0f) return false;
  const float tol = l0 * 1e-3f;
  if (std::fabs(l1 - l0) > tol || std::fabs(l2 - l0) > tol) return false;
  if (std::fabs(dot(c0, c1)) > tol || std::fabs(dot(c1, c2)) > tol ||
      std::fabs(dot(c0, c2)) > tol)
    return false;
  const float inv = 1.0f / std::sqrt(l0);
  const vec3f L = -lightDirView;
  out = {dot(c0, L) * inv, dot(c1, L) * inv, dot(c2, L) * inv};
  return true;
}

#if SHAPOGFX3D_FIXED_POINT
void Graphics3D::shadeVertexQ(const VertexQ &in, const PrimSetup &ps,
                              CachedVertex &out) const {
  out.ok = false;
  int32_t vx, vy, vz;
  transformQ(curQ_, in.p, vx, vy, vz);
  // Triangles crossing or in front of the near plane are dropped
  if (vz > -projQ_.zNear) return;
  ShadedVertex &sv = out.sv;
  if (!projectQ(vx, vy, vz, sv, out.invW)) return;

  const int32_t *nq = in.n;
  int32_t d = 0;  // diffuse factor, Q15
  int32_t n[3] = {0, 0, 0};
  if (ps.viewNormal) {
    const MatQ &m = curQ_;
    normalizeQ15((arch::mul64(m.r[0], nq[0]) + arch::mul64(m.r[3], nq[1]) +
                  arch::mul64(m.r[6], nq[2])) >>
                     MAT_SHIFT,
                 (arch::mul64(m.r[1], nq[0]) + arch::mul64(m.r[4], nq[1]) +
                  arch::mul64(m.r[7], nq[2])) >>
                     MAT_SHIFT,
                 (arch::mul64(m.r[2], nq[0]) + arch::mul64(m.r[5], nq[1]) +
                  arch::mul64(m.r[8], nq[2])) >>
                     MAT_SHIFT,
                 n);
    if (lightEnabled_) {
      d = -(int32_t)((arch::mul64(n[0], lightQ_.dir[0]) +
                      arch::mul64(n[1], lightQ_.dir[1]) +
                      arch::mul64(n[2], lightQ_.dir[2])) >>
                     NORMAL_SHIFT);
    }
  } else if (lightEnabled_) {
    d = (int32_t)((arch::mul64(nq[0], ps.lightModelQ[0]) +
                   arch::mul64(nq[1], ps.lightModelQ[1]) +
                   arch::mul64(nq[2], ps.lightModelQ[2])) >>
                  LIGHT_SHIFT);
  }

#if SHAPOGFX3D_TEXTURE
  if (ps.tex) {
    if (ps.envMap) {
      // Environment map UV from the view-space normal: (n + 1) / 2 in Q16
      // times the texture size is 16.16 texels
      sv.u = clampFix(arch::mul64(n[0] + (1 << NORMAL_SHIFT), ps.texWq),
                      TEX_MAX_FP);
      sv.v = clampFix(arch::mul64((1 << NORMAL_SHIFT) - n[1], ps.texHq),
                      TEX_MAX_FP);
    } else {
      sv.u = clampFix(arch::mul64(in.u, ps.texWq), TEX_MAX_FP);
      sv.v = clampFix(arch::mul64(in.v, ps.texHq), TEX_MAX_FP);
    }
  } else {
    sv.u = sv.v = 0;
  }
#endif

  // Gouraud shading: lighting is evaluated per vertex. Colors are Q8 of
  // 0..1 here and become 8.8 of 0..255 in vertexColorQ().
  int32_t r, g, b;
  if (ps.lit) {
    r = g = b = 0;
    if (envEnabled_) {
      r += (ps.amb[0] * lightQ_.env[0]) >> COLOR_SHIFT;
      g += (ps.amb[1] * lightQ_.env[1]) >> COLOR_SHIFT;
      b += (ps.amb[2] * lightQ_.env[2]) >> COLOR_SHIFT;
    }
    if (lightEnabled_ && d > 0) {
      r += (int32_t)(arch::mul64(
                         (int32_t)(arch::mul64(ps.dif[0], lightQ_.col[0]) >>
                                   COLOR_SHIFT),
                         d) >>
                     NORMAL_SHIFT);
      g += (int32_t)(arch::mul64(
                         (int32_t)(arch::mul64(ps.dif[1], lightQ_.col[1]) >>
                                   COLOR_SHIFT),
                         d) >>
                     NORMAL_SHIFT);
      b += (int32_t)(arch::mul64(
                         (int32_t)(arch::mul64(ps.dif[2], lightQ_.col[2]) >>
                                   COLOR_SHIFT),
                         d) >>
                     NORMAL_SHIFT);
    }
  } else {
    r = ps.dif[0];
    g = ps.dif[1];
    b = ps.dif[2];
  }
  sv.r = vertexColorQ(r, gfx2d::colorR(in.color), ps.vertexColor, ps.add,
                      ps.alpha256);
  sv.g = vertexColorQ(g, gfx2d::colorG(in.color), ps.vertexColor, ps.add,
                      ps.alpha256);
  sv.b = vertexColorQ(b, gfx2d::colorB(in.color), ps.vertexColor, ps.add,
                      ps.alpha256);

  out.viewZ = vz;
  out.ok = true;
}
#else
void Graphics3D::shadeVertex(const Vertex &in, const PrimSetup &ps,
                             CachedVertex &out) const {
  out.ok = false;
  vec3f viewPos = cur_.transformPoint(in.position);
  // Triangles crossing or in front of the near plane are dropped
  if (viewPos.z > -zNear_) return;
  ShadedVertex &sv = out.sv;
  if (!projectPoint(viewPos, sv.sx, sv.sy, sv.zNdc, out.invW)) return;

  const Material *mat = ps.mat;
  float d = 0.0f;  // diffuse factor
  vec3f n = {0, 0, 0};
  if (ps.viewNormal) {
    n = normalize(cur_.transformDir(in.normal));
    if (lightEnabled_) d = dot(n, -lightDir_);
  } else if (lightEnabled_) {
    d = dot(in.normal, ps.lightModel);
  }

#if SHAPOGFX3D_TEXTURE
  if (ps.tex) {
    vec2f uv;
    if (ps.envMap) {
      // Environment map UV from the view-space normal
      uv = {n.x * 0.5f + 0.5f, 0.5f - n.y * 0.5f};
    } else {
      uv = in.uv;
    }
    sv.u = uv.x * ps.texW;
    sv.v = uv.y * ps.texH;
  } else {
    sv.u = sv.v = 0.0f;
  }
#endif

  // Gouraud shading: lighting is evaluated per vertex
  float r, g, b;
  if (ps.lit) {
    r = g = b = 0.0f;
    if (envEnabled_) {
      r += mat->ambient.r * envCol_.r;
      g += mat->ambient.g * envCol_.g;
      b += mat->ambient.b * envCol_.b;
    }
    if (lightEnabled_ && d > 0.0f) {
      r += mat->diffuse.r * lightCol_.r * d;
      g += mat->diffuse.g * lightCol_.g * d;
      b += mat->diffuse.b * lightCol_.b * d;
    }
  } else {
    r = mat->diffuse.r;
    g = mat->diffuse.g;
    b = mat->diffuse.b;
  }
  if (mat->flags & MaterialFlags::VERTEX_COLOR) {
    r *= gfx2d::colorR(in.color) * (1.0f / 255.0f);
    g *= gfx2d::colorG(in.color) * (1.0f / 255.0f);
    b *= gfx2d::colorB(in.color) * (1.0f / 255.0f);
  }
  if (mat->blendMode == BlendMode::ADD) {
    // Additive blending just adds (color x opacity), so pre-multiply here
    float a = clamp01(mat->diffuse.a);
    r *= a;
    g *= a;
    b *= a;
  }
  sv.r = clamp01(r) * 255.0f;
  sv.g = clamp01(g) * 255.0f;
  sv.b = clamp01(b) * 255.0f;

  out.viewZ = viewPos.z;
  out.ok = true;
}
#endif

// ---------------------------------------------------------------------------
// Primitive setup
//
// Triangles, lines and points become records in the same integer format in
// both builds; only the arithmetic that gets them there differs.

static inline uint8_t materialAlpha64(const Material *mat) {
#if !SHAPOGFX3D_BLEND
  (void)mat;
  return 64;  // translucency compiled out: everything is opaque
#else
  return (uint8_t)((mat->blendMode == BlendMode::NONE)
                       ? 64
                       : (int)(clamp01(mat->diffuse.a) * 64.0f + 0.5f));
#endif
}

// Construct the record layout selected by `depth`, `smooth` and `tex` in the
// reserved memory, copy the header into it and let `fill` write the optional
// attributes. `fill` is a generic lambda: the record type reaches it as the
// type of its argument, so it can pick the attributes this layout has with
// `if constexpr`. Layouts the configuration never selects are not compiled.
template <bool D, bool G, bool T, typename F>
static inline void makeRec(uint8_t *rec, const TriHead &h, const F &fill) {
  TriRec<D, G, T> &t = *::new (rec) TriRec<D, G, T>;
  static_cast<TriHead &>(t) = h;
  fill(t);
}
template <bool D, bool G, typename F>
static inline void makeRecT(uint8_t *rec, const TriHead &h, bool tex,
                            const F &fill) {
#if SHAPOGFX3D_TEXTURE
  if (tex) return makeRec<D, G, true>(rec, h, fill);
#else
  (void)tex;
#endif
  makeRec<D, G, false>(rec, h, fill);
}
template <bool D, typename F>
static inline void makeRecG(uint8_t *rec, const TriHead &h, bool smooth,
                            bool tex, const F &fill) {
#if SHAPOGFX3D_GOURAUD
  if (smooth) return makeRecT<D, true>(rec, h, tex, fill);
#else
  (void)smooth;
#endif
  makeRecT<D, false>(rec, h, tex, fill);
}
template <typename F>
static inline void makeRecord(uint8_t *rec, const TriHead &h, bool depth,
                              bool smooth, bool tex, const F &fill) {
  if (depth) return makeRecG<true>(rec, h, smooth, tex, fill);
  makeRecG<false>(rec, h, smooth, tex, fill);
}

// |gradient| of a plane, any format. A steeper one (a sliver seen edge on)
// makes the plane constant, which keeps every evaluation within 32 bits.
static constexpr int32_t GRAD_LIMIT = 1 << 30;

// Which planes a primitive needs (masks of the attribute indices)
static constexpr uint32_t ATTR_Z = 1u << A_Z;
static constexpr uint32_t ATTR_RGB = (1u << A_R) | (1u << A_G) | (1u << A_B);
#if SHAPOGFX3D_PERSPECTIVE >= 1
static constexpr uint32_t ATTR_TEX = (1u << A_T0) | (1u << A_T1) | (1u << A_T2);
#else
static constexpr uint32_t ATTR_TEX = (1u << A_T0) | (1u << A_T1);
#endif
static inline uint32_t attrMask(bool depth, bool smooth, bool textured) {
  return (depth ? ATTR_Z : 0) | (smooth ? ATTR_RGB : 0) |
         (textured ? ATTR_TEX : 0);
}

// Colors of a smooth primitive in the record format (see PartSmooth); false
// when a gradient does not fit
static inline bool packSmooth(const PlaneSet &ps, PartSmooth &out) {
  auto fits = [](int32_t v) { return v >= INT16_MIN && v <= INT16_MAX; };
  for (int k = 0; k < 3; k++) {
    const Plane &p = ps.p[A_R + k];
    const int32_t c0 = (p.a0 + (1 << 9)) >> 10;  // 8.16 -> 10.6
    const int32_t dx = (p.dx + (1 << 7)) >> 8;   // 8.16 -> 8.8
    const int32_t dy = (p.dy + (1 << 7)) >> 8;
    if (!fits(c0) || !fits(dx) || !fits(dy)) return false;
    out.c0[k] = (int16_t)c0;
    out.dx[k] = (int16_t)dx;
    out.dy[k] = (int16_t)dy;
  }
  return true;
}

// The depth plane (8.24) in the record format
static inline void packDepth(const Plane &p, PartZ &out) {
#if SHAPOGFX3D_DEPTH_BITS == 16
  auto clamp16 = [](int32_t v) {
    return (int16_t)(v < -32767 ? -32767 : (v > 32767 ? 32767 : v));
  };
  out.z0 = clamp16((p.a0 + (1 << 9)) >> 10);  // 8.24 -> 2.14
  // Gradients 8.24 -> 1.(15 + sh), sh as large as the 16 bits allow. At
  // most 14, so that a depth difference within the primitive (below 2^16 in
  // 1.15) times 2^sh stays within 32 bits in depthAt().
  auto scaled = [](int32_t v, int sh) -> int64_t {
    return sh < 9 ? ((int64_t)v + ((int64_t)1 << (8 - sh))) >> (9 - sh)
                  : (int64_t)v * ((int64_t)1 << (sh - 9));
  };
  int sh = 14;
  while (sh > 0 && (std::abs(scaled(p.dx, sh)) > 32767 ||
                    std::abs(scaled(p.dy, sh)) > 32767))
    sh--;
  const int64_t dx = scaled(p.dx, sh), dy = scaled(p.dy, sh);
  if (dx < -32767 || dx > 32767 || dy < -32767 || dy > 32767) {
    out.zdx = out.zdy = 0;  // steeper than 1 NDC per pixel: constant
    out.zsh = 0;
  } else {
    out.zdx = (int16_t)dx;
    out.zdy = (int16_t)dy;
    out.zsh = (uint8_t)sh;
  }
#else
  out.z = p;
#endif
}

// Write the record of a primitive whose header and planes are complete. A
// smooth primitive whose color gradients do not fit the record is stored
// flat, in the color of its first vertex.
void Graphics3D::storePrimitive(const TriHead &h, const PlaneSet &ps,
                                bool smooth, bool textured) {
  PartSmooth sm = {};
  const bool flattened = smooth && !packSmooth(ps, sm);
  if (flattened) smooth = false;
  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  uint8_t *rec = allocRecord(TRI_REC_SIZE[recIndex(depth, smooth, textured)]);
  if (!rec) {  // buffer overflow: drop for this frame
    triDropped_++;
    return;
  }
  makeRecord(rec, h, depth, smooth, textured, [&](auto &t) {
    using R = std::remove_reference_t<decltype(t)>;
    if (flattened) {
      t.flags |= TriFlags::FLAT;
      t.rasterFn |= 1;
    }
    if constexpr (R::HAS_DEPTH) packDepth(ps.p[A_Z], t);
    if constexpr (R::SMOOTH) {
      static_cast<PartSmooth &>(t) = sm;
    } else {
      t.r = ps.fr;
      t.g = ps.fg;
      t.b = ps.fb;
    }
    if constexpr (R::TEXTURED) {
      t.tex = ps.tex;
#if SHAPOGFX3D_PERSPECTIVE >= 1
      t.uw = ps.p[A_T0];
      t.vw = ps.p[A_T1];
      t.iw = ps.p[A_T2];
#else
      t.u = ps.p[A_T0];
      t.v = ps.p[A_T1];
#endif
    }
  });
  triCount_++;
}

// Header fields of a triangle other than its geometry. `smooth`: the vertex
// colors differ; `tf`: the texture format actually used.
static inline void triangleHeader(TriHead &h, const Material *mat, TexFmt tf,
                                  bool smooth) {
  uint8_t flags = 0;
  if (!smooth) flags |= TriFlags::FLAT;
  if (tf != TexFmt::NONE) flags |= TriFlags::TEX;
#if SHAPOGFX3D_BLEND
  // A texture with alpha makes the triangle translucent even in BlendMode::NONE
  const bool texAlpha = texFmtHasAlpha(tf);
  int blend = (int)mat->blendMode;
  if (texAlpha && mat->blendMode == BlendMode::NONE)
    blend = (int)BlendMode::ALPHA;
  if (mat->blendMode == BlendMode::NONE && !texAlpha) flags |= TriFlags::OPAQUE;
#else
  const int blend = (int)BlendMode::NONE;
  flags |= TriFlags::OPAQUE;  // translucency compiled out
#endif
  h.flags = flags;
  h.alpha64 = materialAlpha64(mat);
  h.rasterFn =
      (uint8_t)((int)tf * RASTER_PER_TEX + blend * 2 + (smooth ? 0 : 1));
}

#if SHAPOGFX3D_UNLIT
static inline uint8_t unlitRasterFn(const Material *mat, bool flat) {
#if SHAPOGFX3D_BLEND
  const int blend = (int)mat->blendMode;
#else
  (void)mat;
  const int blend = (int)BlendMode::NONE;
#endif
  return (uint8_t)((int)TexFmt::NONE * RASTER_PER_TEX + blend * 2 +
                   (flat ? 1 : 0));
}

// OPAQUE unless the material really blends
static inline uint8_t opaqueFlag(const Material *mat) {
#if SHAPOGFX3D_BLEND
  return (mat->blendMode == BlendMode::NONE) ? TriFlags::OPAQUE : 0;
#else
  (void)mat;
  return TriFlags::OPAQUE;
#endif
}
#endif

// Reference column of the planes, from an x (16.16) on the primitive in row
// yMin
static inline int16_t referenceColumn(int32_t x) {
  return (int16_t)(x >> FP_SHIFT);
}

// x (16.16) at the center of `row` of the edge with upper end (xs, ys) and
// slope s. For whole rows k, edgeX(row) + s * k == edgeX(row + k) exactly, so
// two triangles sharing an edge compute the same x on every row whatever row
// each starts it from, and no pixel center falls between them.
static inline int32_t edgeX(int32_t xs, int32_t ys, int32_t s, int row) {
  const int64_t oy = (((int64_t)row << FP_SHIFT) + FP_HALF) - ys;
  return clampFix(xs + (mulFit(s, oy) >> FP_SHIFT), SCREEN_MAX);
}

// Rows, edges and reference column of a triangle whose vertices (16.16 px,
// within the guard band) are sorted by y, given the slopes of its edges
// top->middle, middle->bottom and top->bottom (16.16 px per row, 0 where
// horizontal). False when it covers no pixel row of the screen.
__attribute__((noinline)) static bool triangleGeometry(const int32_t *x,
                                                       const int32_t *y,
                                                       const int32_t *slopes,
                                                       int screenH,
                                                       TriHead &h) {
  // Rows whose centers the triangle covers: ceil(y - 0.5) .. floor(y - 0.5)
  auto ceilRow = [](int32_t v) -> int {
    return (v - FP_HALF + 0xFFFF) >> FP_SHIFT;
  };
  const int yMin = std::max(ceilRow(y[0]), 0);
  const int yMax = std::min((int)((y[2] - FP_HALF) >> FP_SHIFT), screenH - 1);
  if (yMin > yMax) return false;
  const int yMid = std::max(yMin, std::min(yMax + 1, ceilRow(y[1])));
  h.yMin = (coord_t)yMin;
  h.yMax = (coord_t)yMax;
  h.yMid = (coord_t)yMid;

  TriGeo &g = h.geo.tri;
  for (int k = 0; k < 3; k++) g.slope[k] = slopes[k];
  g.x[0] = edgeX(x[0], y[0], g.slope[0], yMin);
  g.x[1] = edgeX(x[1], y[1], g.slope[1], yMid);
  g.x[2] = edgeX(x[0], y[0], g.slope[2], yMin);
  // The long edge is on the left when the middle vertex lies to its right
  if (x[0] + (mulFit(g.slope[2], (int64_t)y[1] - y[0]) >> FP_SHIFT) < x[1]) {
    h.flags |= TriFlags::LEFT_LONG;
  }
  h.xa = referenceColumn(g.x[2]);
  return true;
}

#if !SHAPOGFX3D_FIXED_POINT

namespace detail {
// A vertex of the float setup: screen position in pixels and the attributes
// that are linear in screen space, in the record formats (A_*)
struct SetupVertex {
  float x, y;
  float a[A_COUNT];
};
}  // namespace detail

// Planes through the attributes of three setup vertices, evaluated at the
// reference pixel
struct PlaneSolver {
  float x10, y10, x20, y20, invDet;
  float rx, ry;  // center of the reference pixel minus vertex 0
  Plane operator()(float a0, float a1, float a2) const {
    const float d1 = a1 - a0, d2 = a2 - a0;
    const float dx = (d1 * y20 - d2 * y10) * invDet;
    const float dy = (d2 * x10 - d1 * x20) * invDet;
    constexpr float LIM = (float)GRAD_LIMIT;
    if (!(std::fabs(dx) < LIM) || !(std::fabs(dy) < LIM)) {
      return {fixF((a0 + a1 + a2) * (1.0f / 3.0f), 1.0f, INT32_MAX), 0, 0};
    }
    return {fixF(a0 + dx * rx + dy * ry, 1.0f, INT32_MAX),
            fixF(dx, 1.0f, GRAD_LIMIT), fixF(dy, 1.0f, GRAD_LIMIT)};
  }
};

// Geometry and planes of the triangle p0 p1 p2 (any order; within the guard
// band). False when it covers no pixel row of the screen.
__attribute__((noinline)) static bool setupTriangle(const SetupVertex *p0,
                                                    const SetupVertex *p1,
                                                    const SetupVertex *p2,
                                                    int screenH, uint32_t attrs,
                                                    TriHead &h, PlaneSet &ps) {
  if (p1->y < p0->y) std::swap(p0, p1);
  if (p2->y < p1->y) std::swap(p1, p2);
  if (p1->y < p0->y) std::swap(p0, p1);
  const float x10 = p1->x - p0->x, y10 = p1->y - p0->y;
  const float x20 = p2->x - p0->x, y20 = p2->y - p0->y;
  const float det = x10 * y20 - x20 * y10;
  if (!(det != 0.0f)) return false;  // a sliver left by clipping
  const int32_t x[3] = {fixF(p0->x, FIX_ONE, SCREEN_MAX),
                        fixF(p1->x, FIX_ONE, SCREEN_MAX),
                        fixF(p2->x, FIX_ONE, SCREEN_MAX)};
  const int32_t y[3] = {fixF(p0->y, FIX_ONE, SCREEN_MAX),
                        fixF(p1->y, FIX_ONE, SCREEN_MAX),
                        fixF(p2->y, FIX_ONE, SCREEN_MAX)};
  auto slope = [](const SetupVertex *a, const SetupVertex *b) {
    return b->y > a->y ? fixF((b->x - a->x) / (b->y - a->y), FIX_ONE, SLOPE_MAX)
                       : 0;
  };
  const int32_t slopes[3] = {slope(p0, p1), slope(p1, p2), slope(p0, p2)};
  if (!triangleGeometry(x, y, slopes, screenH, h)) return false;

  const PlaneSolver solve = {x10,
                             y10,
                             x20,
                             y20,
                             1.0f / det,
                             (float)h.xa + 0.5f - p0->x,
                             (float)h.yMin + 0.5f - p0->y};
  for (int k = 0; k < A_COUNT; k++) {
    if (attrs & (1u << k)) ps.p[k] = solve(p0->a[k], p1->a[k], p2->a[k]);
  }
  return true;
}

// A vertex of a triangle clipped to the guard band: its position and its
// weights of the triangle's vertices 1 and 2 (the attributes follow from
// them, being linear in screen space)
struct ClipVertex {
  float x, y, w1, w2;
};

// Clip the polygon `in` (n vertices) against the half plane where
// sign * (coordinate `axis`) <= lim; returns the vertex count of `out`
static int clipPolygon(const ClipVertex *in, int n, ClipVertex *out, int axis,
                       float sign, float lim) {
  auto coord = [&](const ClipVertex &v) {
    return sign * (axis ? v.y : v.x) - lim;
  };
  int m = 0;
  for (int i = 0; i < n; i++) {
    const ClipVertex &a = in[i], &b = in[(i + 1) % n];
    const float da = coord(a), db = coord(b);
    if (da <= 0.0f) out[m++] = a;
    if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
      const float t = da / (da - db);
      out[m++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.w1 + (b.w1 - a.w1) * t, a.w2 + (b.w2 - a.w2) * t};
    }
  }
  return m;
}

// Guard band of the float setup in pixels, a little inside the record limit
static constexpr float GUARD = (float)(SCREEN_MAX >> FP_SHIFT) - 2.0f;

void Graphics3D::emitTriangle(const CachedVertex &a, const CachedVertex &b,
                              const CachedVertex &c, const Material *mat,
                              const Texture *tex) {
  if (!a.ok || !b.ok || !c.ok) return;

  // Back-face culling (screen y points down, so front-facing = negative area)
  const float area2 = (b.sv.sx - a.sv.sx) * (c.sv.sy - a.sv.sy) -
                      (c.sv.sx - a.sv.sx) * (b.sv.sy - a.sv.sy);
  if (area2 == 0.0f) return;
  if (area2 > 0.0f && !(mat->flags & MaterialFlags::DOUBLE_SIDED)) return;

  // A vertex beyond the guard band takes the clipping variant
  auto inside = [](const CachedVertex &v) {
    return std::fabs(v.sv.sx) <= GUARD && std::fabs(v.sv.sy) <= GUARD;
  };
  if (inside(a) && inside(b) && inside(c)) {
    emitTriangleSetup<false>(a, b, c, mat, tex);
  } else {
    emitTriangleSetup<true>(a, b, c, mat, tex);
  }
}

// The setup of a triangle that passed culling. Two out-of-line variants, so
// that the clip buffers of the rare CLIP one take stack only when used and
// never on top of the other's frame.
template <bool CLIP>
__attribute__((noinline)) void Graphics3D::emitTriangleSetup(
    const CachedVertex &a, const CachedVertex &b, const CachedVertex &c,
    const Material *mat, const Texture *tex) {
  // Interpolated color only where the three vertex colors differ; the record
  // of a flat triangle holds one color instead of three planes
#if SHAPOGFX3D_GOURAUD
  const bool smooth =
      !(a.sv.r == b.sv.r && a.sv.r == c.sv.r && a.sv.g == b.sv.g &&
        a.sv.g == c.sv.g && a.sv.b == b.sv.b && a.sv.b == c.sv.b);
#else
  const bool smooth = false;  // flat shading: the first vertex as passed in
#endif
  const TexFmt tf = texFmtOf(tex);
  const bool textured = (tf != TexFmt::NONE);

  TriHead h;
  triangleHeader(h, mat, tf, smooth);
  h.sortKey = sortKeyOf((a.viewZ + b.viewZ + c.viewZ) * (1.0f / 3.0f));
  h.layer = layerByte();
  const bool depth = !(h.layer & LayerId::NO_DEPTH);

  PlaneSet ps;
  ps.tex = tex;
  // Flat: the color of the first vertex as passed in
  ps.fr = (uint8_t)(int)(a.sv.r + 0.5f);
  ps.fg = (uint8_t)(int)(a.sv.g + 0.5f);
  ps.fb = (uint8_t)(int)(a.sv.b + 0.5f);

  const CachedVertex *cv[3] = {&a, &b, &c};
  SetupVertex sv[3];
  for (int i = 0; i < 3; i++) {
    const ShadedVertex &v = cv[i]->sv;
    sv[i].x = v.sx;
    sv[i].y = v.sy;
    sv[i].a[A_Z] = clampf(v.zNdc + depthBias_, -120.0f, 120.0f) * Z_ONE;
    sv[i].a[A_R] = v.r * FIX_ONE;
    sv[i].a[A_G] = v.g * FIX_ONE;
    sv[i].a[A_B] = v.b * FIX_ONE;
    for (int k = A_T0; k < A_COUNT; k++) sv[i].a[k] = 0.0f;
  }
#if SHAPOGFX3D_TEXTURE
  if (textured) {
    // Wrap texture coordinates per triangle to keep them small (subtract the
    // texture period below the minimum from all three vertices; relative
    // values are unchanged). Sizes are powers of two.
    float u[3], v[3];
    for (int i = 0; i < 3; i++) {
      u[i] = clampf(cv[i]->sv.u, -30000.0f, 30000.0f);
      v[i] = clampf(cv[i]->sv.v, -30000.0f, 30000.0f);
    }
    const int wPot = 1 << gfx2d::log2Floor(tex->width);
    const int hPot = 1 << gfx2d::log2Floor(tex->height);
    const float uOff =
        (float)((int)std::floor(std::min({u[0], u[1], u[2]})) & ~(wPot - 1));
    const float vOff =
        (float)((int)std::floor(std::min({v[0], v[1], v[2]})) & ~(hPot - 1));
    for (int i = 0; i < 3; i++) {
      u[i] = clampf(u[i] - uOff, 0.0f, 30000.0f);
      v[i] = clampf(v[i] - vOff, 0.0f, 30000.0f);
    }
#if SHAPOGFX3D_PERSPECTIVE >= 1
    // (u/w, v/w, 1/w) are linear in screen space. 1/w is scaled so that its
    // largest vertex value lies in [2^IW_NORM, 2^(IW_NORM + 1)).
    const int k = IW_NORM - floatExponent(std::max({a.invW, b.invW, c.invW}));
    const float scale = pow2f(k / 2) * pow2f(k - k / 2);
    for (int i = 0; i < 3; i++) {
      const float iw = cv[i]->invW * scale;
      sv[i].a[A_T0] = u[i] * iw * (FIX_ONE / (float)(1u << UW_SHIFT));
      sv[i].a[A_T1] = v[i] * iw * (FIX_ONE / (float)(1u << UW_SHIFT));
      sv[i].a[A_T2] = iw;
    }
#else
    for (int i = 0; i < 3; i++) {
      sv[i].a[A_T0] = u[i] * FIX_ONE;
      sv[i].a[A_T1] = v[i] * FIX_ONE;
    }
#endif
  }
#endif  // SHAPOGFX3D_TEXTURE

  const uint32_t attrs = attrMask(depth, smooth, textured);
  if constexpr (!CLIP) {
    if (setupTriangle(&sv[0], &sv[1], &sv[2], screenH_, attrs, h, ps)) {
      storePrimitive(h, ps, smooth, textured);
    }
  } else {
    // Clip to the guard band in screen space, where every stored attribute
    // is linear, and store the pieces
    // 3 vertices + one per clip plane; the fan's setup vertices reuse the
    // second buffer once the clipping is done
    ClipVertex bufA[7];
    union {
      ClipVertex bufB[7];
      SetupVertex p[3];
    } u;
    static_assert(sizeof(u.p) <= sizeof(u.bufB), "fan vertices must fit");
    ClipVertex *const bufB = u.bufB;
    bufA[0] = {sv[0].x, sv[0].y, 0.0f, 0.0f};
    bufA[1] = {sv[1].x, sv[1].y, 1.0f, 0.0f};
    bufA[2] = {sv[2].x, sv[2].y, 0.0f, 1.0f};
    int n = 3;
    n = clipPolygon(bufA, n, bufB, 0, 1.0f, GUARD);
    n = clipPolygon(bufB, n, bufA, 0, -1.0f, GUARD);
    n = clipPolygon(bufA, n, bufB, 1, 1.0f, GUARD);
    n = clipPolygon(bufB, n, bufA, 1, -1.0f, GUARD);
    // Fan triangles, their attributes from the weights
    auto vertexAt = [&](const ClipVertex &cv, SetupVertex &out) {
      out.x = cv.x;
      out.y = cv.y;
      for (int k = 0; k < A_COUNT; k++) {
        out.a[k] = sv[0].a[k] + (sv[1].a[k] - sv[0].a[k]) * cv.w1 +
                   (sv[2].a[k] - sv[0].a[k]) * cv.w2;
      }
    };
    SetupVertex *const p = u.p;
    vertexAt(bufA[0], p[0]);
    const uint8_t flags = h.flags;  // setupTriangle() adds geometry flags
    for (int i = 1; i + 1 < n; i++) {
      vertexAt(bufA[i], p[1]);
      vertexAt(bufA[i + 1], p[2]);
      h.flags = flags;
      if (setupTriangle(&p[0], &p[1], &p[2], screenH_, attrs, h, ps)) {
        storePrimitive(h, ps, smooth, textured);
      }
    }
  }
}

#else  // SHAPOGFX3D_FIXED_POINT

namespace detail {
// A vertex of the fixed-point setup: screen position (16.16 px) and the
// attributes in the record formats (A_*)
struct SetupVertex {
  int32_t x, y;
  int32_t a[A_COUNT];
};
}  // namespace detail

// Cramer's rule works on 14.14 px, so that the products of the guard band's
// coordinate differences and the attribute differences fit 64 bits
static constexpr int CR_DROP = 2;

// Planes through the attributes of three setup vertices, evaluated at the
// reference pixel
struct PlaneSolver {
  int32_t x10, y10, x20, y20;  // 14.14 px (fit 32 bits for any COORD_BITS)
  Rcp rd;                      // 1 / det
  int64_t rx, ry;  // center of the reference pixel minus vertex 0, 16.16 px
  __attribute__((noinline)) Plane operator()(int32_t a0, int32_t a1,
                                             int32_t a2) const {
    const int64_t d1 = (int64_t)a1 - a0, d2 = (int64_t)a2 - a0;
    const int32_t dx = mulRcp(mulFit(d1, y20) - mulFit(d2, y10), rd,
                              FP_SHIFT - CR_DROP, GRAD_LIMIT);
    const int32_t dy = mulRcp(mulFit(d2, x10) - mulFit(d1, x20), rd,
                              FP_SHIFT - CR_DROP, GRAD_LIMIT);
    if (dx == GRAD_LIMIT || dx == -GRAD_LIMIT || dy == GRAD_LIMIT ||
        dy == -GRAD_LIMIT) {
      return {a0 / 3 + a1 / 3 + a2 / 3, 0, 0};
    }
    return {clampFix(a0 + ((mulFit(dx, rx) + mulFit(dy, ry)) >> FP_SHIFT),
                     INT32_MAX),
            dx, dy};
  }
};

// The coordinate differences and the reciprocal of the determinant of a
// triangle sorted by y; false when it is degenerate. The steps of the
// fixed-point setup are separate functions, which keeps their 64-bit
// temporaries out of each other's stack frames (on a Cortex-M0+ an inlined
// chain of them costs hundreds of bytes of stack).
__attribute__((noinline)) static bool makeSolver(const int32_t *x,
                                                 const int32_t *y,
                                                 PlaneSolver &s) {
  s.x10 = (int32_t)(((int64_t)x[1] - x[0]) >> CR_DROP);
  s.y10 = (int32_t)(((int64_t)y[1] - y[0]) >> CR_DROP);
  s.x20 = (int32_t)(((int64_t)x[2] - x[0]) >> CR_DROP);
  s.y20 = (int32_t)(((int64_t)y[2] - y[0]) >> CR_DROP);
  const int64_t det = arch::mul64(s.x10, s.y20) - arch::mul64(s.x20, s.y10);
  if (det == 0) return false;
  s.rd = makeRcp(det);
  return true;
}

// Slopes of the edges top->middle, middle->bottom and top->bottom
__attribute__((noinline)) static void edgeSlopes(const int32_t *x,
                                                 const int32_t *y,
                                                 int32_t *slopes) {
  static const uint8_t EDGE[3][2] = {{0, 1}, {1, 2}, {0, 2}};
  for (int k = 0; k < 3; k++) {
    const int i = EDGE[k][0], j = EDGE[k][1];
    slopes[k] = y[j] > y[i] ? divQ((int64_t)x[j] - x[i], (int64_t)y[j] - y[i],
                                   FP_SHIFT, SLOPE_MAX)
                            : 0;
  }
}

// Geometry and planes of the triangle p0 p1 p2 (any order). False when it
// covers no pixel row of the screen.
__attribute__((noinline)) static bool setupTriangle(const SetupVertex *p0,
                                                    const SetupVertex *p1,
                                                    const SetupVertex *p2,
                                                    int screenH, uint32_t attrs,
                                                    TriHead &h, PlaneSet &ps) {
  if (p1->y < p0->y) std::swap(p0, p1);
  if (p2->y < p1->y) std::swap(p1, p2);
  if (p1->y < p0->y) std::swap(p0, p1);
  const int32_t x[3] = {p0->x, p1->x, p2->x}, y[3] = {p0->y, p1->y, p2->y};
  PlaneSolver solve;
  if (!makeSolver(x, y, solve)) return false;
  int32_t slopes[3];
  edgeSlopes(x, y, slopes);
  if (!triangleGeometry(x, y, slopes, screenH, h)) return false;
  solve.rx = ((int64_t)h.xa * (1 << FP_SHIFT) + FP_HALF) - x[0];
  solve.ry = (((int64_t)h.yMin << FP_SHIFT) + FP_HALF) - y[0];
  for (int k = 0; k < A_COUNT; k++) {
    if (attrs & (1u << k)) ps.p[k] = solve(p0->a[k], p1->a[k], p2->a[k]);
  }
  return true;
}

void Graphics3D::emitTriangle(const CachedVertex &a, const CachedVertex &b,
                              const CachedVertex &c, const Material *mat,
                              const Texture *tex) {
  if (!a.ok || !b.ok || !c.ok) return;

  // Back-face culling (screen y points down, so front-facing = negative area)
  // (in the 14.14 px of the setup, whose differences fit 32 bits for any
  // SHAPOGFX_COORD_BITS)
  auto d = [](int32_t p, int32_t q) { return ((int64_t)p - q) >> CR_DROP; };
  const int64_t area2 = mulFit(d(b.sv.sx, a.sv.sx), d(c.sv.sy, a.sv.sy)) -
                        mulFit(d(c.sv.sx, a.sv.sx), d(b.sv.sy, a.sv.sy));
  if (area2 == 0) return;
  if (area2 > 0 && !(mat->flags & MaterialFlags::DOUBLE_SIDED)) return;

    // Interpolated color only where the three vertex colors differ; the record
    // of a flat triangle holds one color instead of three planes
#if SHAPOGFX3D_GOURAUD
  const bool smooth =
      !(a.sv.r == b.sv.r && a.sv.r == c.sv.r && a.sv.g == b.sv.g &&
        a.sv.g == c.sv.g && a.sv.b == b.sv.b && a.sv.b == c.sv.b);
#else
  const bool smooth = false;  // flat shading: the first vertex as passed in
#endif
  const TexFmt tf = texFmtOf(tex);
  const bool textured = (tf != TexFmt::NONE);

  TriHead h;
  triangleHeader(h, mat, tf, smooth);
  // The average in 32 bits (a 64-bit division is a slow library call on a
  // core without an FPU): a quarter of the sum, divided by 3, times 4
  h.sortKey =
      sortKeyOf(((a.viewZ >> 2) + (b.viewZ >> 2) + (c.viewZ >> 2)) / 3 * 4);
  h.layer = layerByte();
  const bool depth = !(h.layer & LayerId::NO_DEPTH);

  PlaneSet ps;
  ps.tex = tex;
  // Flat: the color of the first vertex as passed in
  ps.fr = (uint8_t)((a.sv.r + 128) >> 8);
  ps.fg = (uint8_t)((a.sv.g + 128) >> 8);
  ps.fb = (uint8_t)((a.sv.b + 128) >> 8);

  const CachedVertex *cv[3] = {&a, &b, &c};
  const int32_t biasQ = fToFix(depthBias_, Z_SHIFT, 1 << 30);
  SetupVertex sv[3];
  for (int i = 0; i < 3; i++) {
    const ShadedVertex &v = cv[i]->sv;
    sv[i].x = v.sx;
    sv[i].y = v.sy;
    sv[i].a[A_Z] = clampFix((int64_t)v.z + biasQ, Z_MAX_FP);
    sv[i].a[A_R] = v.r << 8;  // 8.8 -> 8.16
    sv[i].a[A_G] = v.g << 8;
    sv[i].a[A_B] = v.b << 8;
    for (int k = A_T0; k < A_COUNT; k++) sv[i].a[k] = 0;
  }
#if SHAPOGFX3D_TEXTURE
  if (textured) {
    int32_t u[3], v[3];
    for (int i = 0; i < 3; i++) {
      u[i] = cv[i]->sv.u;
      v[i] = cv[i]->sv.v;
    }
    // Wrap texture coordinates per triangle to keep them small (subtract the
    // texture period below the minimum from all three vertices; relative
    // values are unchanged). Sizes are powers of two.
    const int wPot = 1 << gfx2d::log2Floor(tex->width);
    const int hPot = 1 << gfx2d::log2Floor(tex->height);
    const int32_t uOff =
        (std::min({u[0], u[1], u[2]}) >> FP_SHIFT) & ~(wPot - 1);
    const int32_t vOff =
        (std::min({v[0], v[1], v[2]}) >> FP_SHIFT) & ~(hPot - 1);
    for (int i = 0; i < 3; i++) {
      u[i] =
          clampFix((int64_t)u[i] - (int64_t)uOff * (1 << FP_SHIFT), TEX_MAX_FP);
      v[i] =
          clampFix((int64_t)v[i] - (int64_t)vOff * (1 << FP_SHIFT), TEX_MAX_FP);
    }
#if SHAPOGFX3D_PERSPECTIVE >= 1
    // (u/w, v/w, 1/w) are linear in screen space. 1/w (Q26, positive) is
    // scaled so that its largest vertex value lies in [2^IW_NORM,
    // 2^(IW_NORM + 1)).
    const int32_t iwMax = std::max({a.invW, b.invW, c.invW, (int32_t)1});
    const int sh = __builtin_clz((uint32_t)iwMax) - (31 - IW_NORM);
    for (int i = 0; i < 3; i++) {
      const int32_t iw = sh >= 0 ? cv[i]->invW << sh : cv[i]->invW >> -sh;
      sv[i].a[A_T0] = (int32_t)(arch::mul64(u[i], iw) >> UW_SHIFT);
      sv[i].a[A_T1] = (int32_t)(arch::mul64(v[i], iw) >> UW_SHIFT);
      sv[i].a[A_T2] = iw;
    }
#else
    for (int i = 0; i < 3; i++) {
      sv[i].a[A_T0] = u[i];
      sv[i].a[A_T1] = v[i];
    }
#endif
  }
#endif  // SHAPOGFX3D_TEXTURE

  // The vertex stage has clamped the screen coordinates to the guard band
  if (setupTriangle(&sv[0], &sv[1], &sv[2], screenH_,
                    attrMask(depth, smooth, textured), h, ps)) {
    storePrimitive(h, ps, smooth, textured);
  }
}

#endif  // SHAPOGFX3D_FIXED_POINT

// ---------------------------------------------------------------------------
// Points and lines
//
// They are unlit (diffuse x vertex color), never culled and clipped against
// the near plane. Both are stored in the triangle buffer and become spans in
// makeSpan(), so they are depth-resolved against everything else.

#if SHAPOGFX3D_UNLIT

#if SHAPOGFX3D_FIXED_POINT
void Graphics3D::unlitVertexQ(const VertexQ &in, const Material *mat,
                              UnlitVertex &out) const {
  transformQ(curQ_, in.p, out.vx, out.vy, out.vz);
  const bool useVertex = (mat->flags & MaterialFlags::VERTEX_COLOR) != 0;
  const bool add = mat->blendMode == BlendMode::ADD;
  const int32_t alpha256 = fToFix(clamp01(mat->diffuse.a), 8, 256);
  out.r = vertexColorQ(fToFix(mat->diffuse.r, COLOR_SHIFT, 1 << 15),
                       gfx2d::colorR(in.color), useVertex, add, alpha256);
  out.g = vertexColorQ(fToFix(mat->diffuse.g, COLOR_SHIFT, 1 << 15),
                       gfx2d::colorG(in.color), useVertex, add, alpha256);
  out.b = vertexColorQ(fToFix(mat->diffuse.b, COLOR_SHIFT, 1 << 15),
                       gfx2d::colorB(in.color), useVertex, add, alpha256);
}
#else
void Graphics3D::unlitVertex(const Vertex &in, const Material *mat,
                             UnlitVertex &out) const {
  out.view = cur_.transformPoint(in.position);
  float r = mat->diffuse.r, g = mat->diffuse.g, b = mat->diffuse.b;
  if (mat->flags & MaterialFlags::VERTEX_COLOR) {
    r *= gfx2d::colorR(in.color) * (1.0f / 255.0f);
    g *= gfx2d::colorG(in.color) * (1.0f / 255.0f);
    b *= gfx2d::colorB(in.color) * (1.0f / 255.0f);
  }
  if (mat->blendMode == BlendMode::ADD) {
    float a = clamp01(mat->diffuse.a);
    r *= a, g *= a, b *= a;
  }
  out.r = clamp01(r) * 255.0f;
  out.g = clamp01(g) * 255.0f;
  out.b = clamp01(b) * 255.0f;
}
#endif

// Header of a line or point other than its geometry
static inline void unlitHeader(TriHead &h, const Material *mat, uint8_t kind,
                               bool smooth) {
  h.flags = (uint8_t)(kind | opaqueFlag(mat) | (smooth ? 0 : TriFlags::FLAT));
  h.alpha64 = materialAlpha64(mat);
  h.rasterFn = unlitRasterFn(mat, !smooth);
}

#if SHAPOGFX3D_LINES
// The line setup drops two fraction bits where a coordinate difference
// multiplies another, so that the product fits 64 bits
static constexpr int LINE_DROP = 2;

// End point of a line in the setup: 16.16 px and the attributes in the
// record formats (depth, color)
struct LineEnd {
  int32_t x, y;
  int32_t a[A_B + 1];
};

// Geometry and planes of the segment p -> q (end points within the guard
// band). False when it misses the screen.
__attribute__((noinline)) static bool setupLine(const LineEnd &e0,
                                                const LineEnd &e1, int screenW,
                                                int screenH, uint32_t attrs,
                                                TriHead &h, PlaneSet &ps) {
  const bool swap = e1.y < e0.y;
  const LineEnd &p = swap ? e1 : e0, &q = swap ? e0 : e1;
  // Rows containing the end points; nothing to do when fully off screen
  const int yMin = std::max((int)(p.y >> FP_SHIFT), 0);
  const int yMax = std::min((int)(q.y >> FP_SHIFT), screenH - 1);
  if (yMin > yMax) return false;
  if (std::max(p.x, q.x) < 0 || std::min(p.x, q.x) >= (screenW << FP_SHIFT))
    return false;
  h.yMin = (coord_t)yMin;
  h.yMax = (coord_t)yMax;
  h.yMid = h.yMin;

  const int64_t dx = (int64_t)q.x - p.x, dy = (int64_t)q.y - p.y;  // dy >= 0
  const bool steep = dy >= (dx < 0 ? -dx : dx);
  if (steep) h.flags |= TriFlags::STEEP;
  LineGeo &g = h.geo.line;
  g.ax = p.x;
  g.ay = p.y;
  g.bx = q.x;
  g.by = q.y;
  g.dxdy = dy > 0 ? divQ(dx, dy, FP_SHIFT, SLOPE_MAX) : 0;
  // x of the line at y (16.16), exactly from the end points
  auto xAt = [&](int64_t y) -> int32_t {
    if (dy <= 0) return p.x;
    return clampFix(p.x + (int64_t)divQ(dx * ((y - p.y) >> LINE_DROP),
                                        dy >> LINE_DROP, 0, INT32_MAX),
                    SCREEN_MAX);
  };
  const int64_t r0 = p.y >> FP_SHIFT;  // floor(ay)
  g.xRef = steep ? xAt(r0 * (1 << FP_SHIFT) + FP_HALF)
                 : xAt((r0 + 1) * (1 << FP_SHIFT));

  // Reference pixel: on the segment, in row yMin
  const int64_t yRef = ((int64_t)yMin << FP_SHIFT) + FP_HALF;
  h.xa = referenceColumn(xAt(yRef < p.y ? p.y : (yRef > q.y ? q.y : yRef)));
  const int64_t ox = ((int64_t)h.xa * (1 << FP_SHIFT) + FP_HALF) - p.x;
  const int64_t oy = yRef - p.y;

  // Attributes vary along the major axis: per row for steep lines, per
  // column for shallow ones (see makeLineSpan)
  for (int k = 0; k <= A_B; k++) {
    if (!(attrs & (1u << k))) continue;
    const int64_t d = (int64_t)q.a[k] - p.a[k];
    const int64_t len = steep ? dy : dx;
    const int32_t grad =
        len != 0 ? divQ(d, len, FP_SHIFT, GRAD_LIMIT) : 0;  // per pixel
    if (grad == GRAD_LIMIT || grad == -GRAD_LIMIT) {
      ps.p[k] = {(int32_t)(((int64_t)p.a[k] + q.a[k]) / 2), 0, 0};
      continue;
    }
    const int32_t a0 = clampFix(
        p.a[k] + (mulFit(grad, steep ? oy : ox) >> FP_SHIFT), INT32_MAX);
    ps.p[k] = steep ? Plane{a0, 0, grad} : Plane{a0, grad, 0};
  }
  return true;
}

#if SHAPOGFX3D_FIXED_POINT
void Graphics3D::emitLine(UnlitVertex a, UnlitVertex b, const Material *mat) {
  // Clip against the near plane (visible: z <= -zNear)
  const int32_t zn = -projQ_.zNear;
  const bool aIn = a.vz <= zn, bIn = b.vz <= zn;
  if (!aIn && !bIn) return;
  if (aIn != bIn) {
    const int32_t tt = divQ((int64_t)zn - a.vz, (int64_t)b.vz - a.vz, FP_SHIFT,
                            1 << FP_SHIFT);  // Q16, 0..1
    auto lerpQ = [&](int32_t p, int32_t q) {
      return (int32_t)(p + ((((int64_t)q - p) * tt) >> FP_SHIFT));
    };
    UnlitVertex c;
    c.vx = lerpQ(a.vx, b.vx);
    c.vy = lerpQ(a.vy, b.vy);
    c.vz = lerpQ(a.vz, b.vz);
    c.r = lerpQ(a.r, b.r);
    c.g = lerpQ(a.g, b.g);
    c.b = lerpQ(a.b, b.b);
    (aIn ? b : a) = c;
  }
  ShadedVertex sa, sb;
  int32_t invW;
  if (!projectQ(a.vx, a.vy, a.vz, sa, invW)) return;
  if (!projectQ(b.vx, b.vy, b.vz, sb, invW)) return;
  const int32_t biasQ = fToFix(depthBias_, Z_SHIFT, 1 << 30);
#if SHAPOGFX3D_GOURAUD
  const bool smooth = !(a.r == b.r && a.g == b.g && a.b == b.b);
#else
  const bool smooth = false;  // flat shading: the first end point
#endif

  TriHead h;
  unlitHeader(h, mat, TriFlags::LINE, smooth);
  h.sortKey = sortKeyOf((a.vz >> 1) + (b.vz >> 1));
  h.layer = layerByte();
  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  PlaneSet ps;
  ps.fr = (uint8_t)((a.r + 128) >> 8);
  ps.fg = (uint8_t)((a.g + 128) >> 8);
  ps.fb = (uint8_t)((a.b + 128) >> 8);
  const LineEnd p = {sa.sx,
                     sa.sy,
                     {clampFix((int64_t)sa.z + biasQ, Z_MAX_FP), a.r << 8,
                      a.g << 8, a.b << 8}};
  const LineEnd q = {sb.sx,
                     sb.sy,
                     {clampFix((int64_t)sb.z + biasQ, Z_MAX_FP), b.r << 8,
                      b.g << 8, b.b << 8}};
  if (setupLine(p, q, screenW_, screenH_, attrMask(depth, smooth, false), h,
                ps)) {
    storePrimitive(h, ps, smooth, false);
  }
}
#else
void Graphics3D::emitLine(UnlitVertex a, UnlitVertex b, const Material *mat) {
  // Clip against the near plane (visible: z <= -zNear)
  const float zn = -zNear_;
  const bool aIn = a.view.z <= zn, bIn = b.view.z <= zn;
  if (!aIn && !bIn) return;
  if (aIn != bIn) {
    float tt = (zn - a.view.z) / (b.view.z - a.view.z);
    UnlitVertex c;
    c.view = lerp(a.view, b.view, tt);
    c.r = a.r + (b.r - a.r) * tt;
    c.g = a.g + (b.g - a.g) * tt;
    c.b = a.b + (b.b - a.b) * tt;
    (aIn ? b : a) = c;
  }
  float ax, ay, az, bx, by, bz, invW;
  if (!projectPoint(a.view, ax, ay, az, invW)) return;
  if (!projectPoint(b.view, bx, by, bz, invW)) return;
#if SHAPOGFX3D_GOURAUD
  const bool smooth = !(a.r == b.r && a.g == b.g && a.b == b.b);
#else
  const bool smooth = false;  // flat shading: the first end point
#endif

  // Clip the segment to the guard band in screen space, where its depth and
  // color are linear (Liang-Barsky)
  const float pa[6] = {ax,  ay,  clampf(az + depthBias_, -120.0f, 120.0f),
                       a.r, a.g, a.b};
  const float pb[6] = {bx,  by,  clampf(bz + depthBias_, -120.0f, 120.0f),
                       b.r, b.g, b.b};
  float t0 = 0.0f, t1 = 1.0f;
  const float d[2] = {bx - ax, by - ay};
  for (int axis = 0; axis < 2; axis++) {
    for (float sign : {1.0f, -1.0f}) {
      // sign * (p + t * d) <= GUARD
      const float num = GUARD - sign * pa[axis], den = sign * d[axis];
      if (den == 0.0f) {
        if (num < 0.0f) return;
      } else if (den > 0.0f) {
        t1 = std::min(t1, num / den);
      } else {
        t0 = std::max(t0, num / den);
      }
    }
  }
  if (!(t0 <= t1)) return;
  float qa[6], qb[6];
  for (int k = 0; k < 6; k++) {
    qa[k] = pa[k] + (pb[k] - pa[k]) * t0;
    qb[k] = pa[k] + (pb[k] - pa[k]) * t1;
  }

  TriHead h;
  unlitHeader(h, mat, TriFlags::LINE, smooth);
  h.sortKey = sortKeyOf((a.view.z + b.view.z) * 0.5f);
  h.layer = layerByte();
  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  PlaneSet ps;
  ps.fr = (uint8_t)(int)(a.r + 0.5f);
  ps.fg = (uint8_t)(int)(a.g + 0.5f);
  ps.fb = (uint8_t)(int)(a.b + 0.5f);
  auto end = [](const float *e) -> LineEnd {
    return {
        fixF(e[0], FIX_ONE, SCREEN_MAX),
        fixF(e[1], FIX_ONE, SCREEN_MAX),
        {fixF(e[2], Z_ONE, Z_MAX_FP), fixF(e[3], FIX_ONE, COLOR_MAX_FP),
         fixF(e[4], FIX_ONE, COLOR_MAX_FP), fixF(e[5], FIX_ONE, COLOR_MAX_FP)}};
  };
  if (setupLine(end(qa), end(qb), screenW_, screenH_,
                attrMask(depth, smooth, false), h, ps)) {
    storePrimitive(h, ps, smooth, false);
  }
}
#endif
#endif  // SHAPOGFX3D_LINES

#if SHAPOGFX3D_FIXED_POINT
void Graphics3D::emitPoint(const UnlitVertex &a, const Material *mat) {
  if (a.vz > -projQ_.zNear) return;
  ShadedVertex sv;
  int32_t invW;
  if (!projectQ(a.vx, a.vy, a.vz, sv, invW)) return;
  const int32_t z =
      clampFix((int64_t)sv.z + fToFix(depthBias_, Z_SHIFT, 1 << 30), Z_MAX_FP);
  const int size = pointSize_;
  // Square of `size` pixels centered on the point: floor(s - size / 2 + 0.5)
  const int32_t half = (int32_t)size << (FP_SHIFT - 1);
  const int x0 = (sv.sx - half + FP_HALF) >> FP_SHIFT;
  const int y0 = (sv.sy - half + FP_HALF) >> FP_SHIFT;
  const uint8_t r = (uint8_t)((a.r + 128) >> 8),
                g = (uint8_t)((a.g + 128) >> 8),
                b = (uint8_t)((a.b + 128) >> 8);
  const int16_t sortKey = sortKeyOf(a.vz);
#else
void Graphics3D::emitPoint(const UnlitVertex &a, const Material *mat) {
  if (a.view.z > -zNear_) return;
  float sx, sy, zf, invW;
  if (!projectPoint(a.view, sx, sy, zf, invW)) return;
  const int32_t z = fixF(zf + depthBias_, Z_ONE, Z_MAX_FP);
  const int size = pointSize_;
  // Square of `size` pixels centered on the point
  const int x0 = floorInt(sx - size * 0.5f + 0.5f);
  const int y0 = floorInt(sy - size * 0.5f + 0.5f);
  const uint8_t r = (uint8_t)(int)(a.r + 0.5f), g = (uint8_t)(int)(a.g + 0.5f),
                b = (uint8_t)(int)(a.b + 0.5f);
  const int16_t sortKey = sortKeyOf(a.view.z);
#endif
  if (x0 + size <= 0 || x0 >= (int)screenW_) return;
  const int yMin = std::max(y0, 0);
  const int yMax = std::min(y0 + size - 1, (int)screenH_ - 1);
  if (yMin > yMax) return;

  TriHead h;
  unlitHeader(h, mat, TriFlags::POINT, false);
  h.geo.point.x0 = x0;
  h.geo.point.size = size;
  h.sortKey = sortKey;
  h.yMin = (coord_t)yMin;
  h.yMax = (coord_t)yMax;
  h.yMid = h.yMin;
  h.xa = (int16_t)x0;
  h.layer = layerByte();
  PlaneSet ps;
  ps.p[A_Z] = {z, 0, 0};
  ps.fr = r;
  ps.fg = g;
  ps.fb = b;
  storePrimitive(h, ps, false, false);
}

#endif  // SHAPOGFX3D_UNLIT

// One vertex of a buffer. A buffer of plain vertices is used in place; a
// packed one is decoded into `tmp`, which the caller owns. The vertex cache
// means this happens once per vertex and primitive.
static inline const Vertex &vertexAt(const VertexBuffer &vb, uint16_t i,
                                     Vertex &tmp) {
  if (vb.vertices) return vb.vertices[i];
  if (vb.fixed) {
    const FixedVertex &f = vb.fixed[i];
    constexpr float POS = 1.0f / 65536.0f, NRM = 1.0f / 32768.0f;
    tmp.position = {f.position[0] * POS, f.position[1] * POS,
                    f.position[2] * POS};
    tmp.normal = {f.normal[0] * NRM, f.normal[1] * NRM, f.normal[2] * NRM};
    tmp.uv = {f.uv[0] * PACKED_UV_SCALE, f.uv[1] * PACKED_UV_SCALE};
    tmp.color = f.color;
    return tmp;
  }
  const PackedVertex &p = vb.packed[i];
  tmp.position = {p.position[0] * vb.scale.x + vb.bias.x,
                  p.position[1] * vb.scale.y + vb.bias.y,
                  p.position[2] * vb.scale.z + vb.bias.z};
  tmp.normal = {p.normal[0] * PACKED_NORMAL_SCALE,
                p.normal[1] * PACKED_NORMAL_SCALE,
                p.normal[2] * PACKED_NORMAL_SCALE};
  tmp.uv = {p.uv[0] * PACKED_UV_SCALE, p.uv[1] * PACKED_UV_SCALE};
  tmp.color = gfx2d::makeColor(p.color[0], p.color[1], p.color[2]);
  return tmp;
}

#if SHAPOGFX3D_FIXED_POINT
// The fixed-point stage's input, from any of the three forms. A FixedVertex
// is copied; the float forms are converted here, once per vertex and
// primitive.
static inline const VertexQ &vertexAtQ(const VertexBuffer &vb, uint16_t i,
                                       VertexQ &tmp) {
  if (vb.fixed) {
    const FixedVertex &f = vb.fixed[i];
    for (int k = 0; k < 3; k++) {
      tmp.p[k] = f.position[k];
      tmp.n[k] = f.normal[k];
    }
    tmp.u = (int32_t)f.uv[0] << 6;  // 1/1024 -> 16.16
    tmp.v = (int32_t)f.uv[1] << 6;
    tmp.color = f.color;
    return tmp;
  }
  Vertex ftmp;
  const Vertex &v = vertexAt(vb, i, ftmp);
  tmp.p[0] = fToFix(v.position.x, FP_SHIFT, INT32_MAX);
  tmp.p[1] = fToFix(v.position.y, FP_SHIFT, INT32_MAX);
  tmp.p[2] = fToFix(v.position.z, FP_SHIFT, INT32_MAX);
  tmp.n[0] = fToFix(v.normal.x, NORMAL_SHIFT, 1 << 18);
  tmp.n[1] = fToFix(v.normal.y, NORMAL_SHIFT, 1 << 18);
  tmp.n[2] = fToFix(v.normal.z, NORMAL_SHIFT, 1 << 18);
  tmp.u = fToFix(v.uv.x, FP_SHIFT, 1 << 30);
  tmp.v = fToFix(v.uv.y, FP_SHIFT, 1 << 30);
  tmp.color = v.color;
  return tmp;
}
#endif

const CachedVertex &Graphics3D::fetchVertex(const VertexBuffer &vb, uint16_t vi,
                                            const PrimSetup &ps) {
  static const CachedVertex INVALID = {};  // ok == false: drops the triangle
  if (vi >= vb.vertexCount) {  // out-of-range index: the triangle is dropped
    badIndices_++;
    return INVALID;
  }
  CachedVertex &cv = vcache_[vi & (VCACHE_SIZE - 1)];
  if (cv.tag != vi) {
#if SHAPOGFX3D_FIXED_POINT
    VertexQ tmp;
    shadeVertexQ(vertexAtQ(vb, vi, tmp), ps, cv);
#else
    Vertex tmp;
    shadeVertex(vertexAt(vb, vi, tmp), ps, cv);
#endif
    cv.tag = vi;
  }
  return cv;
}

#if SHAPOGFX3D_UNLIT
bool Graphics3D::fetchUnlitVertex(const VertexBuffer &vb, uint16_t vi,
                                  const Material *mat, UnlitVertex &out) {
  if (vi >= vb.vertexCount) {
    badIndices_++;
    return false;
  }
#if SHAPOGFX3D_FIXED_POINT
  VertexQ tmp;
  unlitVertexQ(vertexAtQ(vb, vi, tmp), mat, out);
#else
  Vertex tmp;
  unlitVertex(vertexAt(vb, vi, tmp), mat, out);
#endif
  return true;
}
#endif

void Graphics3D::putPrimitive(const Primitive &prim) {
  if (!recBase_) return;
  const Material *mat = prim.material ? prim.material : curMat_;
  if (!mat) return;
  if (!prim.vertexBuffer || !prim.indices) return;
  const VertexBuffer &vb = *prim.vertexBuffer;
  if (!vb.vertices && !vb.packed && !vb.fixed) return;
  const uint16_t *idx = prim.indices;
  int n = prim.indexCount;

#if SHAPOGFX3D_FIXED_POINT
  refreshFixed();
#endif
  // Per-primitive constants of the vertex stage
  PrimSetup ps;
  ps.mat = mat;
  ps.tex = materialTexture(mat);
  ps.texW = ps.tex ? (float)ps.tex->width : 0.0f;
  ps.texH = ps.tex ? (float)ps.tex->height : 0.0f;
  ps.envMap = ps.tex && (mat->flags & MaterialFlags::ENV_MAP);
  ps.lit = envEnabled_ || lightEnabled_;
  ps.viewNormal = ps.envMap;
  ps.lightModel = {0, 0, 0};
  if (lightEnabled_ && !lightToModelSpace(cur_, lightDir_, ps.lightModel)) {
    ps.viewNormal = true;
  }
#if SHAPOGFX3D_FIXED_POINT
  ps.lightModelQ[0] = fToFix(ps.lightModel.x, LIGHT_SHIFT, INT32_MAX);
  ps.lightModelQ[1] = fToFix(ps.lightModel.y, LIGHT_SHIFT, INT32_MAX);
  ps.lightModelQ[2] = fToFix(ps.lightModel.z, LIGHT_SHIFT, INT32_MAX);
  ps.amb[0] = fToFix(mat->ambient.r, COLOR_SHIFT, 1 << 15);
  ps.amb[1] = fToFix(mat->ambient.g, COLOR_SHIFT, 1 << 15);
  ps.amb[2] = fToFix(mat->ambient.b, COLOR_SHIFT, 1 << 15);
  ps.dif[0] = fToFix(mat->diffuse.r, COLOR_SHIFT, 1 << 15);
  ps.dif[1] = fToFix(mat->diffuse.g, COLOR_SHIFT, 1 << 15);
  ps.dif[2] = fToFix(mat->diffuse.b, COLOR_SHIFT, 1 << 15);
  ps.alpha256 = fToFix(clamp01(mat->diffuse.a), 8, 256);
  ps.texWq = ps.tex ? (int32_t)ps.tex->width : 0;
  ps.texHq = ps.tex ? (int32_t)ps.tex->height : 0;
  ps.vertexColor = (mat->flags & MaterialFlags::VERTEX_COLOR) != 0;
  ps.add = mat->blendMode == BlendMode::ADD;
#endif

  // Vertex cache: avoid re-transforming vertices shared by several triangles
  // (strips, fans, indexed meshes). Invalidated per primitive.
  for (int i = 0; i < VCACHE_SIZE; i++) vcache_[i].tag = NONE;
  auto fetch = [&](uint16_t vi) -> const CachedVertex & {
    return fetchVertex(vb, vi, ps);
  };

  switch (prim.type) {
    case PrimitiveType::TRIANGLES:
      for (int i = 0; i + 2 < n; i += 3) {
        emitTriangle(fetch(idx[i]), fetch(idx[i + 1]), fetch(idx[i + 2]), mat,
                     ps.tex);
      }
      break;
    case PrimitiveType::TRIANGLE_STRIP:
      for (int i = 2; i < n; i++) {
        if (i & 1) {
          emitTriangle(fetch(idx[i - 1]), fetch(idx[i - 2]), fetch(idx[i]), mat,
                       ps.tex);
        } else {
          emitTriangle(fetch(idx[i - 2]), fetch(idx[i - 1]), fetch(idx[i]), mat,
                       ps.tex);
        }
      }
      break;
    case PrimitiveType::TRIANGLE_FAN:
      for (int i = 2; i < n; i++) {
        emitTriangle(fetch(idx[0]), fetch(idx[i - 1]), fetch(idx[i]), mat,
                     ps.tex);
      }
      break;
    case PrimitiveType::POINTS:
    case PrimitiveType::LINES:
    case PrimitiveType::LINE_STRIP:
    case PrimitiveType::LINE_LOOP: {
#if SHAPOGFX3D_UNLIT
      // Unlit vertices are cheap, so they are computed on the fly (no cache)
      auto fetchUnlit = [&](uint16_t vi, UnlitVertex &out) -> bool {
        return fetchUnlitVertex(vb, vi, mat, out);
      };
      UnlitVertex a, b;
      (void)a, (void)b;
      if (prim.type == PrimitiveType::POINTS) {
#if SHAPOGFX3D_POINTS
        for (int i = 0; i < n; i++) {
          if (fetchUnlit(idx[i], a)) emitPoint(a, mat);
        }
#endif
      } else {
#if SHAPOGFX3D_LINES
        if (prim.type == PrimitiveType::LINES) {
          for (int i = 0; i + 1 < n; i += 2) {
            if (fetchUnlit(idx[i], a) && fetchUnlit(idx[i + 1], b))
              emitLine(a, b, mat);
          }
        } else {
          for (int i = 1; i < n; i++) {
            if (fetchUnlit(idx[i - 1], a) && fetchUnlit(idx[i], b))
              emitLine(a, b, mat);
          }
          if (prim.type == PrimitiveType::LINE_LOOP && n > 2) {
            if (fetchUnlit(idx[n - 1], a) && fetchUnlit(idx[0], b))
              emitLine(a, b, mat);
          }
        }
#endif
      }
#endif  // SHAPOGFX3D_UNLIT
      break;
    }
  }
}

void Graphics3D::putCube(const vec3f &center, const vec3f &size, int divs) {
  if (divs < 1) divs = 1;

  static const int8_t FACE_NORMALS[6][3] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
  };
  // Four corners of each face, counter-clockwise when seen from outside (signs
  // of a unit cube)
  static const int8_t FACE_CORNERS[6][4][3] = {
      {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},      // +X
      {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},  // -X
      {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},      // +Y
      {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},  // -Y
      {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},      // +Z
      {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},  // -Z
  };
  static const vec2f FACE_UVS[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  static const uint16_t QUAD_INDICES[6] = {0, 1, 2, 0, 2, 3};

  vec3f half = size * 0.5f;
  const float invDivs = 1.0f / (float)divs;

  for (int f = 0; f < 6; f++) {
    vec3f corner[4];
    for (int i = 0; i < 4; i++) {
      corner[i] = {
          center.x + half.x * FACE_CORNERS[f][i][0],
          center.y + half.y * FACE_CORNERS[f][i][1],
          center.z + half.z * FACE_CORNERS[f][i][2],
      };
    }
    vec3f normal = {(float)FACE_NORMALS[f][0], (float)FACE_NORMALS[f][1],
                    (float)FACE_NORMALS[f][2]};
    // corner[0] is the origin, corner[0]->corner[1] the u axis,
    // corner[0]->corner[3] the v axis
    vec3f du = corner[1] - corner[0];
    vec3f dv = corner[3] - corner[0];
    vec2f duvU = FACE_UVS[1] - FACE_UVS[0];
    vec2f duvV = FACE_UVS[3] - FACE_UVS[0];

    // Split the face into divs x divs quads (UVs of intermediate points are
    // interpolated)
    for (int j = 0; j < divs; j++) {
      for (int i = 0; i < divs; i++) {
        float u0 = (float)i * invDivs, u1 = (float)(i + 1) * invDivs;
        float v0 = (float)j * invDivs, v1 = (float)(j + 1) * invDivs;
        const float us[4] = {u0, u1, u1, u0};
        const float vs[4] = {v0, v0, v1, v1};

        Vertex quad[4];
        for (int k = 0; k < 4; k++) {
          quad[k].position = corner[0] + du * us[k] + dv * vs[k];
          quad[k].normal = normal;
          quad[k].uv = FACE_UVS[0] + duvU * us[k] + duvV * vs[k];
          quad[k].color = VERTEX_WHITE;
        }
        VertexBuffer vb = {4, quad};
        Primitive prim = {PrimitiveType::TRIANGLES, &vb, 6, QUAD_INDICES,
                          nullptr};
        putPrimitive(prim);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Rendering

// Header of the record an entry points to
static inline const TriHead *recOf(const uint8_t *base, TriEntry e) {
  return (const TriHead *)(base + (size_t)e * REC_UNIT);
}

void Graphics3D::beginRender() {
  // Sort each layer farthest first (ascending view-space z: more negative
  // comes first). Depth order between opaque spans of the same layer is
  // resolved by the depth test at span insertion, so this sort mainly
  // determines the compositing order of translucent primitives; between
  // layers the order the layers were opened in decides. Only the entries are
  // permuted, never the records.
  if (!recBase_) return;
  // The links of each context go between the entries and the records
  uint16_t *link =
      (uint16_t *)(recBase_ + (size_t)triCount_ * sizeof(TriEntry));
  for (int c = 0; c < contextCount_; c++) {
    RenderContext &rc = contexts_[c];
    rc.peak = 0;
    rc.dropped = 0;
    rc.link = link + (size_t)c * (size_t)triCount_;
  }
  const uint8_t *base = recBase_;
  for (int i = 0; i < layerCount_; i++) {
    if (layers_[i].id & LayerId::NO_DEPTH) continue;  // kept in the order added
    const int first = layers_[i].first;
    const int last = (i + 1 < layerCount_) ? layers_[i + 1].first : triCount_;
    std::sort(entries_ + first, entries_ + last,
              [base](TriEntry a, TriEntry b) {
                const int ka = recOf(base, a)->sortKey;
                const int kb = recOf(base, b)->sortKey;
                // Equal depth keeps the order the primitives were added in:
                // records grow downwards, so the earlier one sits higher.
                if (ka != kb) return ka < kb;
                return a > b;
              });
  }
}

void Graphics3D::endRender() {}

static SHAPOGFX3D_HOT_ATTR Span *allocSpan(RenderContext &rc, int capacity) {
  if (rc.count >= capacity) {
    rc.dropped++;
    return nullptr;
  }
  return &rc.pool[rc.count++];
}

// Advance the left end of a span by n pixels
static inline void spanAdvance(Span &sp, int n) {
  sp.x0 = (coord_t)(sp.x0 + n);
}

// Depth (8.24, or 1.15 with SHAPOGFX3D_DEPTH_BITS 16) of a record whose layer
// has depth at pixel (x, yi). PartZ directly follows the header in every such
// layout (TriRec<true, G, T>).
static inline int32_t depthAt(const TriHead &h, int x, int yi) {
  const PartZ &z = static_cast<const TriRec<true, false, false> &>(h);
  const int ox = x - h.xa, oy = yi - h.yMin;
#if SHAPOGFX3D_DEPTH_BITS == 16
  const int32_t d = (int32_t)((uint32_t)(int32_t)z.zdx * (uint32_t)ox +
                              (uint32_t)(int32_t)z.zdy * (uint32_t)oy);
  return (int32_t)z.z0 * 2 + (d >> z.zsh);
#else
  return z.z.at(ox, oy);
#endif
}

// Is frag nearer than e at the pixel at the center of the overlap [ox0, ox1)
// on row yi?
//
// Spans reach the list in layer order, so a span whose layer differs from
// another one's belongs to a later layer and is by definition the nearer one;
// inside a layer without depth the later span wins for the same reason. Only
// within a layer that has depth are the two depths compared, each evaluated
// from its record's depth plane.
static inline bool fragNearer(const Span &frag, const Span &e, int ox0, int ox1,
                              int yi) {
  if (frag.lay != e.lay) return true;
  if (frag.lay & LayerId::NO_DEPTH) return true;
  const int xc = (ox0 + ox1 - 1) >> 1;
  return depthAt(*frag.tri, xc, yi) < depthAt(*e.tri, xc, yi);
}

// Remove the range [ox0, ox1) from list element e = *pp.
// Returns the position at which to continue scanning (after the remaining part,
// or the rest of e).
static SHAPOGFX3D_HOT_ATTR Span **cutSpan(RenderContext &rc, int capacity,
                                          Span **pp, int ox0, int ox1) {
  Span *e = *pp;
  bool leftRemains = e->x0 < ox0;
  bool rightRemains = e->x1 > ox1;
  if (leftRemains && rightRemains) {
    // Hole in the middle: split the right part into a new span (dropped if the
    // pool is full)
    Span *r = allocSpan(rc, capacity);
    if (r) {
      *r = *e;
      spanAdvance(*r, ox1 - r->x0);
      r->next = e->next;
      e->next = r;
    }
    e->x1 = (coord_t)ox0;
    return &e->next;
  } else if (leftRemains) {
    e->x1 = (coord_t)ox0;
    return &e->next;
  } else if (rightRemains) {
    spanAdvance(*e, ox1 - e->x0);
    return pp;
  } else {
    *pp = e->next;  // fully covered: remove
    return pp;
  }
}

#if SHAPOGFX3D_BLEND

// Append (part of) a span [sp.x0, x1) to the translucent list
static SHAPOGFX3D_HOT_ATTR void appendTranslucent(RenderContext &rc,
                                                  int capacity, const Span &sp,
                                                  int x1) {
  Span *n = allocSpan(rc, capacity);
  if (!n) return;  // pool overflow: drop this span
  *n = sp;
  n->x1 = (coord_t)x1;
  n->next = nullptr;
  if (rc.transTail) {
    rc.transTail->next = n;
  } else {
    rc.transHead = n;
  }
  rc.transTail = n;
}

#endif  // SHAPOGFX3D_BLEND

// Insert an opaque span into the list sorted by x.
// Overlaps with existing spans are resolved by comparing depth at the center of
// the overlap and removing the farther part. Because this does not rely on the
// per-triangle sort order, large and small polygons are ordered correctly too.
static SHAPOGFX3D_HOT_ATTR void insertOpaque(RenderContext &rc, int capacity,
                                             Span &frag, int yi) {
  Span **pp = &rc.opaque;
  while (*pp && (*pp)->x1 <= frag.x0) pp = &(*pp)->next;
  while (*pp && (*pp)->x0 < frag.x1) {
    Span *e = *pp;
    int ox0 = std::max(e->x0, frag.x0);
    int ox1 = std::min(e->x1, frag.x1);
    if (fragNearer(frag, *e, ox0, ox1, yi)) {
      pp = cutSpan(rc, capacity, pp, ox0, ox1);
    } else {
      // The part sticking out to the left of e is final (elements before it are
      // done)
      if (frag.x0 < ox0) {
        Span *n = allocSpan(rc, capacity);
        if (n) {
          *n = frag;
          n->x1 = (coord_t)ox0;
          n->next = e;
          *pp = n;
          pp = &n->next;
        }
      }
      if (frag.x1 > ox1) {
        spanAdvance(frag, ox1 - frag.x0);
        pp = &e->next;
      } else {
        return;  // the rest is completely hidden
      }
    }
  }
  if (frag.x0 < frag.x1) {
    Span *n = allocSpan(rc, capacity);
    if (n) {
      *n = frag;
      n->next = *pp;
      *pp = n;
    }
  }
}

#if SHAPOGFX3D_BLEND

// Add a translucent span to the translucent list, excluding the parts hidden by
// nearer opaque spans
static SHAPOGFX3D_HOT_ATTR void insertTranslucent(RenderContext &rc,
                                                  int capacity, Span &frag,
                                                  int yi) {
  Span **pp = &rc.opaque;
  while (*pp && (*pp)->x1 <= frag.x0) pp = &(*pp)->next;
  while (*pp && (*pp)->x0 < frag.x1) {
    Span *e = *pp;
    int ox0 = std::max(e->x0, frag.x0);
    int ox1 = std::min(e->x1, frag.x1);
    if (fragNearer(frag, *e, ox0, ox1, yi)) {
      pp = &e->next;  // translucent is nearer: keep both
      continue;
    }
    if (frag.x0 < ox0) appendTranslucent(rc, capacity, frag, ox0);
    if (frag.x1 > ox1) {
      spanAdvance(frag, ox1 - frag.x0);
      pp = &e->next;
    } else {
      return;
    }
  }
  if (frag.x0 < frag.x1) appendTranslucent(rc, capacity, frag, frag.x1);
}

// Remove the parts of translucent spans that lie behind the new opaque span
// frag
static SHAPOGFX3D_HOT_ATTR void clipTranslucent(RenderContext &rc, int capacity,
                                                const Span &frag, int yi) {
  Span **pp = &rc.transHead;
  bool modified = false;
  while (*pp) {
    Span *e = *pp;
    if (e->x1 <= frag.x0 || e->x0 >= frag.x1) {
      pp = &e->next;
      continue;
    }
    int ox0 = std::max(e->x0, frag.x0);
    int ox1 = std::min(e->x1, frag.x1);
    if (fragNearer(frag, *e, ox0, ox1, yi)) {
      pp = cutSpan(rc, capacity, pp, ox0, ox1);
      modified = true;
    } else {
      pp = &e->next;
    }
  }
  if (modified) {
    rc.transTail = nullptr;
    for (Span *e = rc.transHead; e; e = e->next) rc.transTail = e;
  }
}

#endif  // SHAPOGFX3D_BLEND

// Span builders: the pixel range a primitive covers on row yi, limited to
// [rx0, rx1). They read the header only and use 32-bit arithmetic only.

// Triangle: the pixels whose centers lie between the two edges crossing the
// row center (the top->middle edge covers the rows before yMid, the
// middle->bottom edge the rows from yMid on)
static inline bool makeTriSpan(const TriHead &t, int yi, int rx0, int rx1,
                               Span &out) {
  const TriGeo &g = t.geo.tri;
  auto edge = [&](int k, int row0) {
    return (int32_t)((uint32_t)g.x[k] +
                     (uint32_t)g.slope[k] * (uint32_t)(yi - row0));
  };
  const int32_t xLong = edge(2, t.yMin);
  const int32_t xShort = (yi < t.yMid) ? edge(0, t.yMin) : edge(1, t.yMid);
  const bool leftLong = (t.flags & TriFlags::LEFT_LONG) != 0;
  const int32_t xl = leftLong ? xLong : xShort;
  const int32_t xr = leftLong ? xShort : xLong;
  // Fill the pixels whose centers (xi + 0.5) fall inside [xl, xr)
  int xi0 = (xl - FP_HALF + 0xFFFF) >> FP_SHIFT;
  int xi1 = (xr - FP_HALF + 0xFFFF) >> FP_SHIFT;
  xi0 = std::max(xi0, rx0);
  xi1 = std::min(xi1, rx1);
  if (xi0 >= xi1) return false;
  out.x0 = (coord_t)xi0;
  out.x1 = (coord_t)xi1;
  return true;
}

#if SHAPOGFX3D_LINES
// Line segment a -> b on pixel row yi: one pixel per row for steep lines, one
// pixel per column (a horizontal run) for shallow lines, so the coverage
// matches a Bresenham line. Both end points are drawn.
static bool makeLineSpan(const TriHead &t, int yi, int rx0, int rx1,
                         Span &out) {
  const LineGeo &g = t.geo.line;
  const int r0 = g.ay >> FP_SHIFT;  // row of a
  auto xAtRow = [&](int k) {        // x at row (r0 + k) of xRef's kind
    return (int32_t)((uint32_t)g.xRef + (uint32_t)g.dxdy * (uint32_t)k);
  };
  int c0, c1;
  if (t.flags & TriFlags::STEEP) {
    // Steep: the pixel at the row center, the parameter kept within the
    // segment
    const int32_t yc = ((int32_t)yi << FP_SHIFT) + FP_HALF;
    const int32_t x = yc <= g.ay ? g.ax : (yc >= g.by ? g.bx : xAtRow(yi - r0));
    c0 = x >> FP_SHIFT;
    c1 = c0 + 1;
    if (c0 < rx0 || c0 >= rx1) return false;
  } else {
    // Shallow: columns whose center's y falls into [yi, yi + 1)
    const int ca = g.ax >> FP_SHIFT, cb = g.bx >> FP_SHIFT;
    const int cMin = std::min(ca, cb), cMax = std::max(ca, cb);
    if (g.ay == g.by) {
      c0 = cMin;
      c1 = cMax + 1;
    } else {
      // x at the top and the bottom of the row, kept within the segment
      int32_t xA =
          ((int32_t)yi << FP_SHIFT) <= g.ay ? g.ax : xAtRow(yi - r0 - 1);
      int32_t xB =
          ((int32_t)(yi + 1) << FP_SHIFT) >= g.by ? g.bx : xAtRow(yi - r0);
      if (xA > xB) std::swap(xA, xB);
      c0 = (xA - FP_HALF + 0xFFFF) >> FP_SHIFT;  // ceil(x - 0.5)
      c1 = (xB - FP_HALF + 0xFFFF) >> FP_SHIFT;
      // The rows of the end points always include the end point pixels
      if (yi == r0) {
        c0 = std::min(c0, ca);
        c1 = std::max(c1, ca + 1);
      }
      if (yi == (g.by >> FP_SHIFT)) {
        c0 = std::min(c0, cb);
        c1 = std::max(c1, cb + 1);
      }
      c0 = std::max(c0, cMin);
      c1 = std::min(c1, cMax + 1);
    }
    c0 = std::max(c0, rx0);
    c1 = std::min(c1, rx1);
    if (c0 >= c1) return false;
  }
  out.x0 = (coord_t)c0;
  out.x1 = (coord_t)c1;
  return true;
}
#endif  // SHAPOGFX3D_LINES

#if SHAPOGFX3D_POINTS
// Point: a square with its leftmost column at x0
static inline bool makePointSpan(const TriHead &t, int rx0, int rx1,
                                 Span &out) {
  const int c0 = std::max((int)t.geo.point.x0, rx0);
  const int c1 = std::min((int)t.geo.point.x0 + (int)t.geo.point.size, rx1);
  if (c0 >= c1) return false;
  out.x0 = (coord_t)c0;
  out.x1 = (coord_t)c1;
  return true;
}
#endif  // SHAPOGFX3D_POINTS

// Span of primitive t on row yi, limited to [rx0, rx1). Returns false when
// it covers no pixel there.
static inline bool makeSpan(const TriHead &t, int yi, int rx0, int rx1,
                            Span &out) {
  bool ok;
#if SHAPOGFX3D_LINES
  if (t.flags & TriFlags::LINE) {
    ok = makeLineSpan(t, yi, rx0, rx1, out);
  } else
#endif
#if SHAPOGFX3D_POINTS
      if (t.flags & TriFlags::POINT) {
    ok = makePointSpan(t, rx0, rx1, out);
  } else
#endif
  {
    ok = makeTriSpan(t, yi, rx0, rx1, out);
  }
  out.lay = t.layer;
  out.tri = &t;
  out.next = nullptr;
  return ok;
}

// ---------------------------------------------------------------------------
// Pixel processing (fixed point)
//
// The loop is specialized for every combination of blend mode x texture format
// x flat (constant color) x output format, so that the inner loop contains no
// branches and no unnecessary interpolation.

// Texel fetch: returns the texel as native RGB565 and its 4-bit alpha (15 when
// the format has no alpha). `row` points to the start of the texel row.
template <TexFmt T>
struct TexSampler;

template <>
struct TexSampler<TexFmt::NONE> {  // never called (untextured spans skip the
                                   // fetch)
  static inline uint32_t fetch(const uint8_t *, uint32_t, uint32_t &a4) {
    a4 = 15;
    return 0;
  }
};

#if SHAPOGFX3D_TEXTURE

#if SHAPOGFX_FORMAT_GRAY1
template <>
struct TexSampler<TexFmt::GRAY1> {
  static inline uint32_t fetch(const uint8_t *row, uint32_t u, uint32_t &a4) {
    a4 = 15;
    return ((row[u >> 3] >> (7u - (u & 7u))) & 1u) ? 0xFFFFu : 0u;
  }
};
#endif
#if SHAPOGFX_FORMAT_RGB444
template <>
struct TexSampler<TexFmt::RGB444> {
  static inline uint32_t fetch(const uint8_t *row, uint32_t u, uint32_t &a4) {
    a4 = 15;
    const uint8_t *p = row + (u >> 1) * 3 + (u & 1u);
    uint32_t v = (u & 1u) ? (((uint32_t)(p[0] & 0x0Fu) << 8) | p[1])
                          : (((uint32_t)p[0] << 4) | (p[1] >> 4));
    return gfx2d::rgb444ToRgb565((uint16_t)v);
  }
};
#endif
#if SHAPOGFX_FORMAT_ARGB4444
template <>
struct TexSampler<TexFmt::ARGB4444> {
  static inline uint32_t fetch(const uint8_t *row, uint32_t u, uint32_t &a4) {
    uint32_t p = ((const uint16_t *)row)[u];
    a4 = p >> 12;
    return gfx2d::rgb444ToRgb565((uint16_t)(p & 0x0FFFu));
  }
};
#endif
#if SHAPOGFX_FORMAT_RGB565BE
template <>
struct TexSampler<TexFmt::RGB565BE> {
  static inline uint32_t fetch(const uint8_t *row, uint32_t u, uint32_t &a4) {
    a4 = 15;
    return gfx2d::bswap16(((const uint16_t *)row)[u]);
  }
};
#endif
#if SHAPOGFX_FORMAT_RGB565
template <>
struct TexSampler<TexFmt::RGB565> {
  static inline uint32_t fetch(const uint8_t *row, uint32_t u, uint32_t &a4) {
    a4 = 15;
    return ((const uint16_t *)row)[u];
  }
};
#endif

#endif  // SHAPOGFX3D_TEXTURE

// Texture coordinate walker: (u, v) in 16.16 texels, wrapped with bit masks
// (width and height must be powers of two). fetchNext() returns the current
// texel and advances to the next pixel.
template <TexFmt T>
struct SoftTex {
  const uint8_t *tp;
  uint32_t uMask, vMask, tstride;
  int32_t u, v, du, dv;

  void init(const Texture &tex, int32_t u0, int32_t v0, int32_t du0,
            int32_t dv0) {
    uMask = (1u << gfx2d::log2Floor(tex.width)) - 1;
    vMask = (1u << gfx2d::log2Floor(tex.height)) - 1;
    tp = (const uint8_t *)tex.pixels;
    tstride = tex.stride;
    u = u0;
    v = v0;
    du = du0;
    dv = dv0;
  }
  void setStep(int32_t du0, int32_t dv0) {
    du = du0;
    dv = dv0;
  }
  inline uint32_t fetchNext(uint32_t &a4) {
    const uint8_t *row =
        tp + (size_t)((uint32_t)(v >> FIX_SHIFT) & vMask) * tstride;
    const uint32_t texel =
        TexSampler<T>::fetch(row, (uint32_t)(u >> FIX_SHIFT) & uMask, a4);
    u += du;
    v += dv;
    return texel;
  }
};

// Output format: pixel cursor, packing from 5/6/5 components, blending
template <PixelFormat OUT>
struct OutTraits;

#if SHAPOGFX_FORMAT_RGB565BE
template <>
struct OutTraits<PixelFormat::RGB565BE> {
  using Cursor = gfx2d::CursorRgb565BE;
  static inline uint32_t pack(uint32_t r5, uint32_t g6, uint32_t b5) {
    return gfx2d::makeRgb565(r5, g6, b5);
  }
  static inline uint32_t blend(uint32_t d, uint32_t s, uint32_t a64) {
    return gfx2d::blendAlphaRgb565((uint16_t)d, (uint16_t)s, a64);
  }
  static inline uint32_t add(uint32_t d, uint32_t r5, uint32_t g6,
                             uint32_t b5) {
    return gfx2d::addSaturateRgb565((uint16_t)d, r5, g6, b5);
  }
};
#endif
#if SHAPOGFX_FORMAT_RGB565
template <>
struct OutTraits<PixelFormat::RGB565> {
  using Cursor = gfx2d::CursorRgb565;
  static inline uint32_t pack(uint32_t r5, uint32_t g6, uint32_t b5) {
    return gfx2d::makeRgb565(r5, g6, b5);
  }
  static inline uint32_t blend(uint32_t d, uint32_t s, uint32_t a64) {
    return gfx2d::blendAlphaRgb565((uint16_t)d, (uint16_t)s, a64);
  }
  static inline uint32_t add(uint32_t d, uint32_t r5, uint32_t g6,
                             uint32_t b5) {
    return gfx2d::addSaturateRgb565((uint16_t)d, r5, g6, b5);
  }
};
#endif
#if SHAPOGFX_FORMAT_RGB444
template <>
struct OutTraits<PixelFormat::RGB444> {
  using Cursor = gfx2d::CursorRgb444;
  static inline uint32_t pack(uint32_t r5, uint32_t g6, uint32_t b5) {
    return gfx2d::makeRgb444(r5 >> 1, g6 >> 2, b5 >> 1);
  }
  static inline uint32_t blend(uint32_t d, uint32_t s, uint32_t a64) {
    return gfx2d::blendAlphaRgb444((uint16_t)d, (uint16_t)s, a64);
  }
  static inline uint32_t add(uint32_t d, uint32_t r5, uint32_t g6,
                             uint32_t b5) {
    return gfx2d::addSaturateRgb444((uint16_t)d, r5 >> 1, g6 >> 2, b5 >> 1);
  }
};
#endif

// Vertex color of a span: interpolated per pixel (Gouraud shading) or
// constant over the span (flat shading)
struct SpanColor {
#if SHAPOGFX3D_GOURAUD
  int32_t r, g, b;     // 8.16 fixed point
  int32_t dr, dg, db;  // per-pixel step
  void setFlat(uint32_t r8, uint32_t g8, uint32_t b8) {
    r = (int32_t)r8 << FIX_SHIFT;
    g = (int32_t)g8 << FIX_SHIFT;
    b = (int32_t)b8 << FIX_SHIFT;
    dr = dg = db = 0;
  }
  void advance() { r += dr, g += dg, b += db; }
  void advance(int k) { r += dr * k, g += dg * k, b += db * k; }
  uint32_t r8() const { return (uint32_t)(r >> FIX_SHIFT) & 0xFFu; }
  uint32_t g8() const { return (uint32_t)(g >> FIX_SHIFT) & 0xFFu; }
  uint32_t b8() const { return (uint32_t)(b >> FIX_SHIFT) & 0xFFu; }
  uint32_t r5() const { return (uint32_t)(r >> 19) & 31u; }
  uint32_t g6() const { return (uint32_t)(g >> 18) & 63u; }
  uint32_t b5() const { return (uint32_t)(b >> 19) & 31u; }
#else
  uint32_t r, g, b;  // 0..255, constant
  void setFlat(uint32_t r8, uint32_t g8, uint32_t b8) {
    r = r8, g = g8, b = b8;
  }
  void advance() {}
  void advance(int) {}
  uint32_t r8() const { return r; }
  uint32_t g8() const { return g; }
  uint32_t b8() const { return b; }
  uint32_t r5() const { return r >> 3; }
  uint32_t g6() const { return g >> 2; }
  uint32_t b5() const { return b >> 3; }
#endif
};

// Texture coordinates of a span at its first pixel, as the walker takes them
struct SpanTex {
  const Texture *tex;
#if SHAPOGFX3D_PERSPECTIVE == 2
  int32_t uw, vw, iw;     // plane values (see IW_NORM)
  int32_t duw, dvw, diw;  // per-pixel steps
#else
  int32_t u, v, du, dv;  // 16.16 texels
#endif
};

static inline int32_t addWrap(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a + (uint32_t)b);
}
static inline int32_t mulWrap(int32_t a, int b) {
  return (int32_t)((uint32_t)a * (uint32_t)b);
}

#if SHAPOGFX3D_PERSPECTIVE >= 1
// u (16.16 texels) = uw x 2^UW_SHIFT / iw in the formats of the texture
// planes (see IW_NORM). One normalized 32-bit division gives a 16-bit
// reciprocal of iw that the multiplications by uw and vw share
// (arch::mulShiftU16: a 32 x 16-bit product, two 16 x 16-bit ones on a
// Cortex-M0+).
struct PerspDiv {
  uint32_t r;
  int sh;
  explicit PerspDiv(int32_t iw) {
    // iw at a pixel lies within its primitive's vertex values, the largest
    // of which is in [2^IW_NORM, 2^(IW_NORM + 1)). Smaller ones are limited
    // to IW_MIN, where a primitive spans a depth ratio beyond 2^14.
    constexpr int32_t IW_MIN = 1 << (IW_NORM - 14);
    const uint32_t d = (uint32_t)(iw < IW_MIN ? IW_MIN : iw);
    const int n = __builtin_clz(d);      // 2 .. 17
    r = 0xFFFFFFFFu / ((d << n) >> 15);  // 2^(47 - n) / d, in (2^15, 2^16)
    sh = 47 - UW_SHIFT - n;              // 0 .. 15
  }
  int32_t operator()(int32_t uw) const { return arch::mulShiftU16(uw, r, sh); }
};
#endif

// Color and texture coordinates of a span of n pixels whose first pixel is
// (ox, oy) from the reference pixel of its record t
template <typename REC>
static inline void spanAttrs(const REC &t, int ox, int oy, int n,
                             SpanColor &col, SpanTex &st) {
  (void)n;
  (void)st;
  if constexpr (REC::SMOOTH) {
#if SHAPOGFX3D_GOURAUD
    // The plane values at pixel centers inside the primitive lie within
    // 0..255 up to rounding; clamp the start value to guard the rounding.
    // In 8.8, then 8.16 for the pixel loop
    int32_t c[3];
    for (int k = 0; k < 3; k++) {
      const int32_t v = (int32_t)((uint32_t)((int32_t)t.c0[k] * 4) +
                                  (uint32_t)(int32_t)t.dx[k] * (uint32_t)ox +
                                  (uint32_t)(int32_t)t.dy[k] * (uint32_t)oy);
      c[k] = (v < 0 ? 0 : (v > (255 << 8) ? (255 << 8) : v)) << 8;
    }
    col.r = c[0];
    col.g = c[1];
    col.b = c[2];
    col.dr = (int32_t)t.dx[0] * 256;
    col.dg = (int32_t)t.dx[1] * 256;
    col.db = (int32_t)t.dx[2] * 256;
#endif
  } else {
    col.setFlat(t.r, t.g, t.b);
  }
  if constexpr (REC::TEXTURED) {
    st.tex = t.tex;
#if SHAPOGFX3D_PERSPECTIVE == 2
    st.uw = t.uw.at(ox, oy);
    st.vw = t.vw.at(ox, oy);
    st.iw = t.iw.at(ox, oy);
    st.duw = t.uw.dx;
    st.dvw = t.vw.dx;
    st.diw = t.iw.dx;
#elif SHAPOGFX3D_PERSPECTIVE == 1
    // Vertical-only correction: (u, v) are exact at the first and the last
    // pixel and interpolated affinely in between
    const int32_t uw = t.uw.at(ox, oy), vw = t.vw.at(ox, oy);
    const int32_t iw = t.iw.at(ox, oy);
    const PerspDiv d0(iw);
    st.u = d0(uw);
    st.v = d0(vw);
    st.du = st.dv = 0;
    if (n > 1) {
      const int k = n - 1;
      const PerspDiv d1(addWrap(iw, mulWrap(t.iw.dx, k)));
      st.du = (d1(addWrap(uw, mulWrap(t.uw.dx, k))) - st.u) / k;
      st.dv = (d1(addWrap(vw, mulWrap(t.vw.dx, k))) - st.v) / k;
    }
#else
    st.u = t.u.at(ox, oy);
    st.v = t.v.at(ox, oy);
    st.du = t.u.dx;
    st.dv = t.v.dx;
#endif
  }
}

// The pixel loop: n pixels through cursor cur, colored by col, texels from
// tx (which st started)
template <BlendMode B, TexFmt T, bool FLAT, PixelFormat OUT, typename Tex>
static inline void rasterLoop(typename OutTraits<OUT>::Cursor &cur, int n,
                              uint32_t a64, SpanColor col, const SpanTex &st,
                              Tex &tx) {
  using O = OutTraits<OUT>;
  constexpr bool TEX = (T != TexFmt::NONE);
  constexpr bool TEXA = texFmtHasAlpha(T);
  (void)st;

#if SHAPOGFX3D_GOURAUD
  // An opaque, untextured, smoothly shaded RGB565 span: red and green come
  // packed from arch::GouraudRG (the SIO interpolator on RP2), blue is
  // stepped here
  if constexpr (B == BlendMode::NONE && !TEX && !FLAT &&
                (OUT == PixelFormat::RGB565BE || OUT == PixelFormat::RGB565)) {
    arch::GouraudRG rg;
    rg.init(col.r, col.g, col.dr, col.dg);
    int32_t b = col.b;
    const int32_t db = col.db;
    for (int i = 0; i < n; i++) {
      cur.write(rg.next() | (((uint32_t)b >> 19) & 31u));
      cur.next();
      b += db;
    }
    return;
  }
#endif

  uint32_t sr = col.r5();
  uint32_t sg = col.g6();
  uint32_t sb = col.b5();

  // Texels are modulated by the vertex color (0..255): (c + 1) * t >> 8
  // preserves the maximum value. A smooth span updates the color every
  // GSTEP pixels (SHAPOGFX3D_GOURAUD_STEP).
  constexpr int GSTEP = (TEX && !FLAT) ? GOURAUD_STEP : 1;
  uint32_t cr = col.r8() + 1, cg = col.g8() + 1, cb = col.b8() + 1;
  int gLeft = GSTEP;
  (void)gLeft;

#if SHAPOGFX3D_PERSPECTIVE == 2
  // Sub-spans of PERSPECTIVE_STEP pixels: (u, v) are exact at the sub-span
  // ends and interpolated linearly inside. uAcc/vAcc mirror the walker's
  // accumulators at the sub-span start.
  int32_t uw = st.uw, vw = st.vw, iw = st.iw;
  int32_t uAcc = 0, vAcc = 0, du = 0, dv = 0;
  int left = 0, prevM = 0;
  if constexpr (TEX) {
    const PerspDiv d(iw);
    uAcc = d(uw);
    vAcc = d(vw);
  }
#endif

  for (int i = 0; i < n; i++) {
    uint32_t a4 = 15;
    if constexpr (TEX) {
#if SHAPOGFX3D_PERSPECTIVE == 2
      if (left == 0) {
        uAcc += du * prevM;
        vAcc += dv * prevM;
        const int m = std::min(n - i, PERSPECTIVE_STEP);
        uw = addWrap(uw, mulWrap(st.duw, m));
        vw = addWrap(vw, mulWrap(st.dvw, m));
        iw = addWrap(iw, mulWrap(st.diw, m));
        const PerspDiv d(iw);
        const int32_t u1 = d(uw), v1 = d(vw);
        if (m == PERSPECTIVE_STEP) {
          du = (u1 - uAcc) / PERSPECTIVE_STEP;
          dv = (v1 - vAcc) / PERSPECTIVE_STEP;
        } else {
          du = (u1 - uAcc) / m;
          dv = (v1 - vAcc) / m;
        }
        tx.setStep(du, dv);
        left = prevM = m;
      }
      left--;
#endif
      const uint32_t texel = tx.fetchNext(a4);
      sr = (cr * (texel >> 11)) >> 8;
      sg = (cg * ((texel >> 5) & 63u)) >> 8;
      sb = (cb * (texel & 31u)) >> 8;
    } else if constexpr (!FLAT) {
      sr = col.r5();
      sg = col.g6();
      sb = col.b5();
    }

    // a4 * 17 + (a4 >> 3) maps 0..15 to 0..256
    const uint32_t a256 = TEXA ? (a4 * 17u + (a4 >> 3)) : 256u;
    if (!TEXA || a256 != 0) {
      if constexpr (B == BlendMode::NONE) {
        cur.write(O::pack(sr, sg, sb));
      } else if constexpr (B == BlendMode::ALPHA) {
        uint32_t a = TEXA ? ((a64 * a256) >> 8) : a64;
        cur.write(O::blend(cur.read(), O::pack(sr, sg, sb), a));
      } else {
        // Additive (color is pre-multiplied by opacity): saturating add per
        // channel
        if (TEXA) {
          sr = (sr * a256) >> 8;
          sg = (sg * a256) >> 8;
          sb = (sb * a256) >> 8;
        }
        cur.write(O::add(cur.read(), sr, sg, sb));
      }
    }
    cur.next();

    if constexpr (!FLAT) {
      if constexpr (GSTEP == 1) {
        col.advance();
        if constexpr (TEX) {
          cr = col.r8() + 1;
          cg = col.g8() + 1;
          cb = col.b8() + 1;
        }
      } else if (--gLeft == 0) {
        col.advance(GSTEP);
        cr = col.r8() + 1;
        cg = col.g8() + 1;
        cb = col.b8() + 1;
        gLeft = GSTEP;
      }
    }
  }
}

// Rasterize n pixels of span sp, on screen row yi, starting at pixel x of
// `line`. The span's attributes are evaluated here, from its record, at its
// first pixel.
template <BlendMode B, TexFmt T, bool FLAT, PixelFormat OUT>
static SHAPOGFX3D_HOT_ATTR void rasterSpanT(uint8_t *line, int x, int n,
                                            const Span &sp, int yi) {
  using O = OutTraits<OUT>;
  constexpr bool TEX = (T != TexFmt::NONE);

  // Recover the record layout: the rasterizer's own flat/texture variant
  // plus the layer's depth flag
  const TriHead &h = *sp.tri;
  const int ox = sp.x0 - h.xa, oy = yi - h.yMin;
  SpanColor col;
  SpanTex st;
  if (h.layer & LayerId::NO_DEPTH) {
    spanAttrs(static_cast<const TriRec<false, !FLAT, TEX> &>(h), ox, oy, n, col,
              st);
  } else {
    spanAttrs(static_cast<const TriRec<true, !FLAT, TEX> &>(h), ox, oy, n, col,
              st);
  }

  typename O::Cursor cur;
  cur.init(line, x);

  // With equal vertex colors and no texture, the color is constant over the
  // span
  if constexpr (B == BlendMode::NONE && FLAT && !TEX) {
    cur.fill(n, O::pack(col.r5(), col.g6(), col.b5()));
    return;
  }

  const uint32_t a64 = h.alpha64;
  if constexpr (TEX) {
#if SHAPOGFX3D_TEXTURE
    const Texture &tex = *st.tex;
#if SHAPOGFX3D_PERSPECTIVE == 2
    const int32_t u0 = 0, v0 = 0, du0 = 0, dv0 = 0;  // set per sub-span
#else
    const int32_t u0 = st.u, v0 = st.v, du0 = st.du, dv0 = st.dv;
#endif
#if SHAPOGFX3D_RP2_INTERP
    if constexpr (T == TexFmt::RGB565BE || T == TexFmt::RGB565 ||
                  T == TexFmt::ARGB4444) {
      using InterpTex =
          arch::rp2::InterpTex<T == TexFmt::ARGB4444, T == TexFmt::RGB565BE>;
      if (InterpTex::usable(tex)) {
        InterpTex tx;
        tx.init(tex, u0, v0, du0, dv0);
        rasterLoop<B, T, FLAT, OUT>(cur, n, a64, col, st, tx);
        return;
      }
    }
#endif
    SoftTex<T> tx;
    tx.init(tex, u0, v0, du0, dv0);
    rasterLoop<B, T, FLAT, OUT>(cur, n, a64, col, st, tx);
#endif
  } else {
    SoftTex<TexFmt::NONE> tx;
    rasterLoop<B, T, FLAT, OUT>(cur, n, a64, col, st, tx);
  }
}

using RasterFn = void (*)(uint8_t *, int, int, const Span &, int);
using FillFn = void (*)(uint8_t *, int, int, uint32_t);

template <PixelFormat OUT>
static SHAPOGFX3D_HOT_ATTR void fillLineT(uint8_t *line, int x, int n,
                                          uint32_t native) {
  typename OutTraits<OUT>::Cursor cur;
  cur.init(line, x);
  cur.fill(n, native);
}

// GCC ignores a section attribute on a function template but honours it on
// an explicit instantiation, so with SHAPOGFX3D_HOT_INSTANTIATE the span
// functions the tables below select are instantiated here, explicitly, with
// SHAPOGFX3D_HOT_ATTR. The rows mirror the tables' own.
#ifndef SHAPOGFX3D_HOT_INSTANTIATE
#define SHAPOGFX3D_HOT_INSTANTIATE 0
#endif
#if SHAPOGFX3D_HOT_INSTANTIATE
#define SHAPOGFX3D_HOT_INST(B, T, FLAT, OUT)                      \
  template SHAPOGFX3D_HOT_ATTR void rasterSpanT<B, T, FLAT, OUT>( \
      uint8_t *, int, int, const Span &, int);
#if SHAPOGFX3D_GOURAUD
#define SHAPOGFX3D_HOT_SMOOTH(B, T, OUT) SHAPOGFX3D_HOT_INST(B, T, false, OUT)
#else
#define SHAPOGFX3D_HOT_SMOOTH(B, T, OUT)
#endif
#if SHAPOGFX3D_BLEND
#define SHAPOGFX3D_HOT_PAIR(B, T, OUT) \
  SHAPOGFX3D_HOT_SMOOTH(B, T, OUT) SHAPOGFX3D_HOT_INST(B, T, true, OUT)
#else
#define SHAPOGFX3D_HOT_PAIR(B, T, OUT)
#endif
#define SHAPOGFX3D_HOT_ROW(OUT, T)                   \
  SHAPOGFX3D_HOT_SMOOTH(BlendMode::NONE, T, OUT)     \
  SHAPOGFX3D_HOT_INST(BlendMode::NONE, T, true, OUT) \
  SHAPOGFX3D_HOT_PAIR(BlendMode::ALPHA, T, OUT)      \
  SHAPOGFX3D_HOT_PAIR(BlendMode::ADD, T, OUT)
#if SHAPOGFX3D_TEXTURE && SHAPOGFX_FORMAT_GRAY1
#define SHAPOGFX3D_HOT_ROW_GRAY1(OUT) SHAPOGFX3D_HOT_ROW(OUT, TexFmt::GRAY1)
#else
#define SHAPOGFX3D_HOT_ROW_GRAY1(OUT)
#endif
#if SHAPOGFX3D_TEXTURE && SHAPOGFX_FORMAT_RGB444
#define SHAPOGFX3D_HOT_ROW_RGB444(OUT) SHAPOGFX3D_HOT_ROW(OUT, TexFmt::RGB444)
#else
#define SHAPOGFX3D_HOT_ROW_RGB444(OUT)
#endif
#if SHAPOGFX3D_TEXTURE && SHAPOGFX_FORMAT_ARGB4444
#define SHAPOGFX3D_HOT_ROW_ARGB4444(OUT) \
  SHAPOGFX3D_HOT_ROW(OUT, TexFmt::ARGB4444)
#else
#define SHAPOGFX3D_HOT_ROW_ARGB4444(OUT)
#endif
#if SHAPOGFX3D_TEXTURE && SHAPOGFX_FORMAT_RGB565BE
#define SHAPOGFX3D_HOT_ROW_RGB565BE(OUT) \
  SHAPOGFX3D_HOT_ROW(OUT, TexFmt::RGB565BE)
#else
#define SHAPOGFX3D_HOT_ROW_RGB565BE(OUT)
#endif
#if SHAPOGFX3D_TEXTURE && SHAPOGFX_FORMAT_RGB565
#define SHAPOGFX3D_HOT_ROW_RGB565(OUT) SHAPOGFX3D_HOT_ROW(OUT, TexFmt::RGB565)
#else
#define SHAPOGFX3D_HOT_ROW_RGB565(OUT)
#endif
#define SHAPOGFX3D_HOT_TABLE(OUT)                                       \
  SHAPOGFX3D_HOT_ROW(OUT, TexFmt::NONE)                                 \
  SHAPOGFX3D_HOT_ROW_GRAY1(OUT)                                         \
  SHAPOGFX3D_HOT_ROW_RGB444(OUT)                                        \
  SHAPOGFX3D_HOT_ROW_ARGB4444(OUT)                                      \
  SHAPOGFX3D_HOT_ROW_RGB565BE(OUT)                                      \
  SHAPOGFX3D_HOT_ROW_RGB565(OUT)                                        \
  template SHAPOGFX3D_HOT_ATTR void fillLineT<OUT>(uint8_t *, int, int, \
                                                   uint32_t);
#if SHAPOGFX_FORMAT_RGB565BE
SHAPOGFX3D_HOT_TABLE(PixelFormat::RGB565BE)
#endif
#if SHAPOGFX_FORMAT_RGB565
SHAPOGFX3D_HOT_TABLE(PixelFormat::RGB565)
#endif
#if SHAPOGFX_FORMAT_RGB444
SHAPOGFX3D_HOT_TABLE(PixelFormat::RGB444)
#endif
#endif  // SHAPOGFX3D_HOT_INSTANTIATE

// Rasterizer table for one output format, indexed by Triangle::rasterFn
// (texture format x blend mode x flat). Entries this configuration never
// selects are null and their function is not instantiated: rows of disabled
// texture formats (see texFmtOf()), the interpolated variants without
// SHAPOGFX3D_GOURAUD and the blending ones without SHAPOGFX3D_BLEND.
#if SHAPOGFX3D_GOURAUD
#define SHAPOGFX3D_RASTER_SMOOTH(B, T, OUT) rasterSpanT<B, T, false, OUT>
#else
#define SHAPOGFX3D_RASTER_SMOOTH(B, T, OUT) nullptr
#endif
#if SHAPOGFX3D_BLEND
#define SHAPOGFX3D_RASTER_PAIR(B, T, OUT) \
  SHAPOGFX3D_RASTER_SMOOTH(B, T, OUT), rasterSpanT<B, T, true, OUT>
#else
#define SHAPOGFX3D_RASTER_PAIR(B, T, OUT) nullptr, nullptr
#endif
#define SHAPOGFX3D_RASTER_ROW(OUT, T)                   \
  SHAPOGFX3D_RASTER_SMOOTH(BlendMode::NONE, T, OUT),    \
      rasterSpanT<BlendMode::NONE, T, true, OUT>,       \
      SHAPOGFX3D_RASTER_PAIR(BlendMode::ALPHA, T, OUT), \
      SHAPOGFX3D_RASTER_PAIR(BlendMode::ADD, T, OUT)
#define SHAPOGFX3D_RASTER_NULL_ROW \
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
#if SHAPOGFX_FORMAT_GRAY1
#define SHAPOGFX3D_RASTER_ROW_GRAY1(OUT) \
  SHAPOGFX3D_RASTER_ROW(OUT, TexFmt::GRAY1)
#else
#define SHAPOGFX3D_RASTER_ROW_GRAY1(OUT) SHAPOGFX3D_RASTER_NULL_ROW
#endif
#if SHAPOGFX_FORMAT_RGB444
#define SHAPOGFX3D_RASTER_ROW_RGB444(OUT) \
  SHAPOGFX3D_RASTER_ROW(OUT, TexFmt::RGB444)
#else
#define SHAPOGFX3D_RASTER_ROW_RGB444(OUT) SHAPOGFX3D_RASTER_NULL_ROW
#endif
#if SHAPOGFX_FORMAT_ARGB4444
#define SHAPOGFX3D_RASTER_ROW_ARGB4444(OUT) \
  SHAPOGFX3D_RASTER_ROW(OUT, TexFmt::ARGB4444)
#else
#define SHAPOGFX3D_RASTER_ROW_ARGB4444(OUT) SHAPOGFX3D_RASTER_NULL_ROW
#endif
#if SHAPOGFX_FORMAT_RGB565BE
#define SHAPOGFX3D_RASTER_ROW_RGB565BE(OUT) \
  SHAPOGFX3D_RASTER_ROW(OUT, TexFmt::RGB565BE)
#else
#define SHAPOGFX3D_RASTER_ROW_RGB565BE(OUT) SHAPOGFX3D_RASTER_NULL_ROW
#endif
#if SHAPOGFX_FORMAT_RGB565
#define SHAPOGFX3D_RASTER_ROW_RGB565(OUT) \
  SHAPOGFX3D_RASTER_ROW(OUT, TexFmt::RGB565)
#else
#define SHAPOGFX3D_RASTER_ROW_RGB565(OUT) SHAPOGFX3D_RASTER_NULL_ROW
#endif
#if SHAPOGFX3D_TEXTURE
#define SHAPOGFX3D_RASTER_TABLE(OUT)                                         \
  {                                                                          \
    SHAPOGFX3D_RASTER_ROW(OUT, TexFmt::NONE),                                \
        SHAPOGFX3D_RASTER_ROW_GRAY1(OUT), SHAPOGFX3D_RASTER_ROW_RGB444(OUT), \
        SHAPOGFX3D_RASTER_ROW_ARGB4444(OUT),                                 \
        SHAPOGFX3D_RASTER_ROW_RGB565BE(OUT),                                 \
        SHAPOGFX3D_RASTER_ROW_RGB565(OUT),                                   \
  }
#else
#define SHAPOGFX3D_RASTER_TABLE(OUT) \
  { SHAPOGFX3D_RASTER_ROW(OUT, TexFmt::NONE) }
#endif

static constexpr int RASTER_TABLE_SIZE = (int)TexFmt::COUNT * RASTER_PER_TEX;

#if SHAPOGFX_FORMAT_RGB565BE
static const RasterFn RASTER_FNS_RGB565BE[RASTER_TABLE_SIZE] =
    SHAPOGFX3D_RASTER_TABLE(PixelFormat::RGB565BE);
#endif
#if SHAPOGFX_FORMAT_RGB565
static const RasterFn RASTER_FNS_RGB565[RASTER_TABLE_SIZE] =
    SHAPOGFX3D_RASTER_TABLE(PixelFormat::RGB565);
#endif
#if SHAPOGFX_FORMAT_RGB444
static const RasterFn RASTER_FNS_RGB444[RASTER_TABLE_SIZE] =
    SHAPOGFX3D_RASTER_TABLE(PixelFormat::RGB444);
#endif

// Merge two ascending lists (links are stored in the entries)
static SHAPOGFX3D_HOT_ATTR uint16_t mergeLists(uint16_t *link, uint16_t a,
                                               uint16_t b) {
  uint16_t head = NONE;
  uint16_t *pp = &head;
  while (a != NONE && b != NONE) {
    if (a < b) {
      *pp = a;
      pp = &link[a];
      a = link[a];
    } else {
      *pp = b;
      pp = &link[b];
      b = link[b];
    }
  }
  *pp = (a != NONE) ? a : b;
  return head;
}

SHAPOGFX3D_HOT_ATTR void Graphics3D::render(int16_t x, int16_t y, int16_t w,
                                            int16_t h, const Surface &dst,
                                            int16_t dstX, int16_t dstY) {
  render(0, x, y, w, h, dst, dstX, dstY);
}

// Everything render() changes is in its context, so calls with different
// contexts may run at the same time (the rest of the renderer is only read).
SHAPOGFX3D_HOT_ATTR void Graphics3D::render(int ctx, int16_t x, int16_t y,
                                            int16_t w, int16_t h,
                                            const Surface &dst, int16_t dstX,
                                            int16_t dstY) {
  if (!recBase_ || !dst.pixels || ctx < 0 || ctx >= contextCount_) return;
  RenderContext &rc = contexts_[ctx];
  const int cap = spanCapacity_;

  // Output format
  const RasterFn *table = nullptr;
  FillFn fillFn = nullptr;
  switch (dst.format) {
#if SHAPOGFX_FORMAT_RGB565BE
    case PixelFormat::RGB565BE:
      table = RASTER_FNS_RGB565BE;
      fillFn = fillLineT<PixelFormat::RGB565BE>;
      break;
#endif
#if SHAPOGFX_FORMAT_RGB565
    case PixelFormat::RGB565:
      table = RASTER_FNS_RGB565;
      fillFn = fillLineT<PixelFormat::RGB565>;
      break;
#endif
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      table = RASTER_FNS_RGB444;
      fillFn = fillLineT<PixelFormat::RGB444>;
      break;
#endif
    default: return;
  }
  const uint32_t clearNative = gfx2d::colorToNative(dst.format, clearColor8_);

  // Clip the region to the destination
  int rx = x, ry = y, rw = w, rh = h, dx = dstX, dy = dstY;
  if (dx < 0) {
    rx -= dx;
    rw += dx;
    dx = 0;
  }
  if (dy < 0) {
    ry -= dy;
    rh += dy;
    dy = 0;
  }
  if (dx + rw > dst.width) rw = dst.width - dx;
  if (dy + rh > dst.height) rh = dst.height - dy;
  if (rw <= 0 || rh <= 0) return;

  const int rx0 = rx, rx1 = rx + rw;
  const int y0 = std::max(ry, 0);
  const int y1 = std::min(ry + rh, (int)screenH_);
  if (y0 >= y1 || rx0 >= rx1) return;
  const TriEntry *const ent = entries_;
  uint16_t *const link = rc.link;
  uint16_t *const bucketHead = rc.bucketHead, *const bucketTail = rc.bucketTail;
  const uint8_t *const base = recBase_;

  arch::RenderState archState;  // e.g. the RP2 interpolator the caller uses
  archState.begin();

  // For each scanline of the region, build the list of triangles that start
  // intersecting at that line (linked by entry position, i.e. by depth)
  for (int i = y0; i < y1; i++) bucketHead[i] = bucketTail[i] = NONE;
  for (int p = 0; p < triCount_; p++) {
    const TriHead &t = *recOf(base, ent[p]);
    if (t.yMax < y0 || t.yMin >= y1) continue;
    int line = std::max((int)t.yMin, y0);
    link[p] = NONE;
    if (bucketTail[line] != NONE) {
      link[bucketTail[line]] = (uint16_t)p;
    } else {
      bucketHead[line] = (uint16_t)p;
    }
    bucketTail[line] = (uint16_t)p;
  }

  uint16_t active = NONE;  // triangles intersecting the current line (by depth)
  for (int yi = y0; yi < y1; yi++) {
    active = mergeLists(link, active, bucketHead[yi]);

    // Clear the span lists (per scanline)
    rc.count = 0;
    rc.opaque = nullptr;
    rc.transHead = rc.transTail = nullptr;

    uint16_t *pp = &active;
    while (*pp != NONE) {
      const uint16_t p = *pp;
      const TriHead &t = *recOf(base, ent[p]);
      if (yi > t.yMax) {
        *pp = link[p];  // passed: remove from the active list
        continue;
      }
      Span sp;
      if (makeSpan(t, yi, rx0, rx1, sp)) {
#if SHAPOGFX3D_BLEND
        if (t.flags & TriFlags::OPAQUE) {
          clipTranslucent(rc, cap, sp, yi);
          insertOpaque(rc, cap, sp, yi);
        } else {
          insertTranslucent(rc, cap, sp, yi);
        }
#else
        insertOpaque(rc, cap, sp, yi);  // every primitive is opaque
#endif
      }
      pp = &link[p];
    }
    if (rc.count > rc.peak) rc.peak = rc.count;

    // Draw the opaque spans (ascending x) and the background in the gaps,
    // then composite the translucent spans on top
    uint8_t *line = dst.linePtr(dy + (yi - ry));
    const int xBase = dx - rx0;  // screen x -> dst x
    int cursor = rx0;
    for (const Span *e = rc.opaque; e; e = e->next) {
      if (clearEnabled_ && e->x0 > cursor) {
        fillFn(line, xBase + cursor, e->x0 - cursor, clearNative);
      }
      table[e->tri->rasterFn](line, xBase + e->x0, e->x1 - e->x0, *e, yi);
      cursor = e->x1;
    }
    if (clearEnabled_ && cursor < rx1) {
      fillFn(line, xBase + cursor, rx1 - cursor, clearNative);
    }
#if SHAPOGFX3D_BLEND
    for (const Span *e = rc.transHead; e; e = e->next) {
      table[e->tri->rasterFn](line, xBase + e->x0, e->x1 - e->x0, *e, yi);
    }
#endif
  }

  archState.end();
}

// ---------------------------------------------------------------------------
// Statistics

size_t Graphics3D::primitiveBytes(bool depth, bool smooth,
                                  bool textured) const {
#if !SHAPOGFX3D_GOURAUD
  smooth = false;
#endif
#if !SHAPOGFX3D_TEXTURE
  textured = false;
#endif
  return TRI_REC_SIZE[recIndex(depth, smooth, textured)] + (size_t)entryBytes_;
}

Stats Graphics3D::getStats() const {
  Stats st;
  st.arenaSize = arenaSize_;
  st.triBytes =
      (size_t)(recEnd_ - recTop_) + (size_t)triCount_ * (size_t)entryBytes_;
  st.triBytesTotal = (size_t)(recEnd_ - recBase_);
  // Spans: the peak of the busiest context, the drops of all of them
  int peak = 0, dropped = 0;
  size_t spanBytes = 0;
  for (int c = 0; c < contextCount_; c++) {
    peak = std::max(peak, contexts_[c].peak);
    dropped += contexts_[c].dropped;
    spanBytes += (size_t)contexts_[c].peak * sizeof(Span);
  }
  st.arenaUsed = arenaFixed_ + st.triBytes + spanBytes;
  st.triCount = triCount_;
  st.triDropped = triDropped_;
  st.layerCount = layerCount_;
  st.layersDropped = layersDropped_;
  st.spanCapacity = spanCapacity_;
  st.spanPeak = peak;
  st.spanDropped = dropped;
  st.badIndices = badIndices_;
  st.nodesDropped = nodesDropped_;
  return st;
}

}  // namespace shapoco::gfx3d
