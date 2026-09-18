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
// everything from there to the span records is integer arithmetic: 32x32
// -> 64 multiplies and a normalized-reciprocal division (one 32-bit
// hardware division plus Newton steps). The per-pixel loops were integer
// already. The records change layout (integer coordinates and planes), so
// the span builders follow.
//
// Limits that the float path does not have: view-space coordinates within
// +-32767 model units, rotation and scale entries of the current matrix
// within +-2048, screen coordinates clamped to +-8191 pixels (a triangle
// with a vertex far off screen bends slightly at the screen edge; the float
// path allows 1e8), texture coordinates within +-30000 texels. Pictures are
// not pixel-identical to the float path: expect a few pixels per thousand
// to differ along edges.
#ifndef SHAPOGFX3D_FIXED_POINT
#define SHAPOGFX3D_FIXED_POINT 0
#endif

// RP2040 / RP2350 (Pico SDK): fetch 16-bit texels through the SIO
// interpolator (interp0 of the core that calls render()). Opt-in.
#ifndef SHAPOGFX3D_RP2_INTERP
#define SHAPOGFX3D_RP2_INTERP 0
#endif

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
#if SHAPOGFX3D_RP2_INTERP
#include "hardware/interp.h"
#endif

namespace shapoco::gfx3d {

// ---------------------------------------------------------------------------
// Internal data structures
//
// Vertex processing (transform, lighting, projection) and triangle setup are
// done in float. Each attribute of a triangle is stored as a plane (a linear
// function of the screen position), so a span is set up by evaluating the
// planes at its leftmost pixel; the per-pixel steps are the plane gradients.
// All per-pixel work uses integer arithmetic (16.16 fixed point for colors
// and texture coordinates, 8.24 for depth).

namespace detail {

static constexpr int FIX_SHIFT = 16;
static constexpr float FIX_ONE = 65536.0f;
static constexpr float FIX_DELTA_MAX = 16000.0f;  // |per-pixel step| (8.16)
static constexpr float FIX_TEX_MAX = 30000.0f;    // |texture coordinate|
static constexpr float Z_ONE = 16777216.0f;       // 8.24 fixed point depth
static constexpr float Z_MAX = 120.0f;
static constexpr float Z_DELTA_MAX = 64.0f;

static constexpr int PERSPECTIVE_STEP = SHAPOGFX3D_PERSPECTIVE_STEP;
static_assert(PERSPECTIVE_STEP >= 2 &&
                  (PERSPECTIVE_STEP & (PERSPECTIVE_STEP - 1)) == 0,
              "SHAPOGFX3D_PERSPECTIVE_STEP must be a power of two");

#if SHAPOGFX3D_FIXED_POINT
// Formats of the fixed-point stage: screen and view-space coordinates 16.16
// (pixels, model units), vertex colors 8.8 (0..255), 1/w Q26, u/w and v/w
// Q12 (texels per unit), depth 8.24 as the spans want it, matrix entries
// Q18, normals Q15, the light direction in model space Q24.
static constexpr int FP_SHIFT = 16;
static constexpr int32_t FP_HALF = 1 << 15;
static constexpr int32_t SCREEN_MAX = 8191 << FP_SHIFT;
static constexpr int COLOR_SHIFT = 8;
static constexpr int32_t COLOR_MAX = 255 << COLOR_SHIFT;
static constexpr int IW_SHIFT = 26;
static constexpr int UW_SHIFT = 12;
static constexpr int MAT_SHIFT = 18;
static constexpr int NORMAL_SHIFT = 15;
static constexpr int LIGHT_SHIFT = 24;
static constexpr int32_t GRAD_MAX = 1 << 30;   // |plane gradient|, any format
static constexpr int32_t SLOPE_MAX = 1 << 30;  // |edge slope|, 16.16 px per px
static constexpr int32_t TEX_MAX_FP = 30000 << FP_SHIFT;
static constexpr int32_t Z_MAX_FP = 120 << 24;
static constexpr int32_t Z_DELTA_MAX_FP = 64 << 24;
static constexpr int32_t COLOR_GRAD_MAX = 16000 << FP_SHIFT;

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
// Linear function of the screen position, held as its value at the record's
// reference point (TriHead::sx[0], sy[0]) and its per-pixel gradients, all in
// the attribute's own format. Offsets from the reference point are 16.16 px.
struct Plane {
  int32_t a0, dx, dy;
  int32_t at(int32_t ox, int32_t oy) const {
    return a0 + (int32_t)(((int64_t)dx * ox + (int64_t)dy * oy) >> FP_SHIFT);
  }
};
#else
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

// Linear function of the screen position: value = c + dx * x + dy * y
struct Plane {
  float c, dx, dy;
  float at(float x, float y) const { return c + dx * x + dy * y; }
};
#endif

// Texture format of a triangle; selects the rasterizer together with the blend
// mode and the flat flag: rasterFn = tex * 6 + blend * 2 + flat
enum class TexFmt : uint8_t {
  NONE = 0,
#if SHAPOGFX3D_TEXTURE
  GRAY1,
  RGB444,
  ARGB4444,
  RGB565BE,
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
}  // namespace TriFlags

// Layer of a primitive, as stored in its record and in its spans: the index of
// the layer, or'ed with NO_DEPTH when the layer carries no depth plane.
// Spans are built in layer order, so a span whose layer byte differs from
// another one's belongs to a later layer and is therefore the nearer one.
namespace LayerId {
constexpr uint8_t INDEX = 0x7F;
constexpr uint8_t NO_DEPTH = 0x80;
}  // namespace LayerId

// A triangle, line or point after setup.
//
// Only the attributes a primitive actually has are stored, so a record is 48
// to 128 bytes instead of always 128 (32-bit target, perspective level 1).
// Every record starts with the same header, which is all the sort, the
// scanline buckets and the rasterizers need; the optional parts follow in a
// fixed order and are reached through the record type, which makeSpan()
// recovers from the header.
struct TriHead {
  // Triangles: the vertices sorted by sy; [0] = top, [1] = middle (the bottom
  // vertex is only needed through the edge slopes). Lines: [0] = a, [1] = b.
  // Points: [0] = top-left pixel.
#if SHAPOGFX3D_FIXED_POINT
  int32_t sx[2], sy[2];  // 16.16 px (points: whole pixels << 16)
  // Triangles: dx/dy of the edges top->middle, middle->bottom, top->bottom
  // (0 for horizontal edges), 16.16 px per px. Lines: [0] = dx/dy (0 when dy
  // is 0). Points: [0] = size in pixels.
  int32_t slope[3];
#else
  float sx[2], sy[2];
  // Triangles: dx/dy of the edges top->middle, middle->bottom, top->bottom
  // (0 for horizontal edges). Lines: [0] = 1/dx, [1] = 1/dy (0 when the
  // difference is 0). Points: [0] = size in pixels.
  float slope[3];
#endif
  const Material *mat;
  int32_t sortKey;     // ascending = farther first
  int16_t yMin, yMax;  // range of scanlines crossed (inclusive)
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
  Plane z;  // NDC depth
};
struct PartSmooth {
  Plane r, g, b;  // vertex color 0..255
};
struct PartFlat {
  uint8_t r, g, b;  // one color for the whole primitive (0..255)
};
struct PartTex {
#if SHAPOGFX3D_PERSPECTIVE >= 1
  Plane uw, vw, iw;  // (u/w, v/w, 1/w)
#else
  Plane u, v;  // texels
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

// One entry per primitive: where its record is and the scanline list link.
// beginRender() sorts the entries of each layer by depth.
struct TriEntry {
  uint16_t rec;   // record offset from the start of the region, in 4 bytes
  uint16_t link;  // next entry of the scanline list (NONE: end)
};

struct LayerDesc {
  int32_t first;  // index of the first entry of the layer
  uint8_t id;     // LayerId byte stored in the records and spans
};

// A span on a scanline: attribute values at the leftmost pixel and per-pixel
// increments
struct Span {
  int32_t x0, x1;  // pixel range [x0, x1)
  int32_t z0, dz;  // NDC depth, 8.24 fixed point (used to resolve overlaps)
#if SHAPOGFX3D_GOURAUD
  int32_t r, g, b;  // 8.16 fixed point (0..255)
  int32_t dr, dg, db;
#else
  uint8_t r, g, b;  // constant over the span (0..255)
#endif
  uint8_t lay;  // LayerId of the primitive (see fragNearer())
#if SHAPOGFX3D_TEXTURE
#if SHAPOGFX3D_PERSPECTIVE == 2
#if SHAPOGFX3D_FIXED_POINT
  int32_t uw, vw, iw;  // (u/w, v/w) Q12, 1/w Q26
  int32_t duw, dvw, diw;
#else
  float uw, vw, iw;  // (u/w, v/w, 1/w)
  float duw, dvw, diw;
#endif
#else
  int32_t u, v;  // texels, 16.16 fixed point
  int32_t du, dv;
#endif
#endif
  const TriHead *tri;
  Span *next;
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

static inline int32_t toFix(float v) { return (int32_t)(v * FIX_ONE); }
static inline int32_t toFixDelta(float v) {
  return toFix(clampf(v, -FIX_DELTA_MAX, FIX_DELTA_MAX));
}
static inline int32_t toFixColor(float v) {
  return toFix(clampf(v, 0.0f, 255.0f));
}
static inline int32_t toFixTex(float v) {
  return toFix(clampf(v, -FIX_TEX_MAX, FIX_TEX_MAX));
}
static inline int32_t toZ(float v) {
  return (int32_t)(clampf(v, -Z_MAX, Z_MAX) * Z_ONE);
}
static inline int32_t toZDelta(float v) {
  return (int32_t)(clampf(v, -Z_DELTA_MAX, Z_DELTA_MAX) * Z_ONE);
}

// Integer key with the same ordering as the float (no NaN)
static inline int32_t floatSortKey(float f) {
  int32_t i;
  std::memcpy(&i, &f, sizeof(i));
  return i ^ (int32_t)(((uint32_t)(i >> 31)) >> 1);
}

#if SHAPOGFX3D_FIXED_POINT
// --- Fixed-point helpers ----------------------------------------------------

// float -> fixed with `shift` fraction bits, saturating; NaN maps to 0
static inline int32_t fToFix(float v, int shift, int32_t maxAbs) {
  const float s = v * (float)((int64_t)1 << shift);
  const float lim = (float)maxAbs;
  if (s != s) return 0;
  if (s <= -lim) return -maxAbs;
  if (s >= lim) return maxAbs;
  return (int32_t)s;
}
static inline int32_t clampFix(int64_t v, int32_t maxAbs) {
  return v < -maxAbs ? -maxAbs : (v > maxAbs ? maxAbs : (int32_t)v);
}
static inline int32_t clampColorFP(int32_t v) {
  return v < 0 ? 0 : (v > COLOR_MAX ? COLOR_MAX : v);
}
static inline int clz64(uint64_t v) { return __builtin_clzll(v); }

// ~2^62 / x for x in [2^31, 2^32): a 32-bit division gives the first 16 bits
// and two Newton steps the rest (about 30 good bits)
static inline uint32_t rcpNorm(uint32_t x) {
  uint64_t r = (uint64_t)(0xFFFFFFFFu / (x >> 16)) << 14;
  for (int i = 0; i < 2; i++) {
    const int64_t e = (int64_t)(((uint64_t)1 << 62) - (uint64_t)x * r);
    r = (uint64_t)((int64_t)r + (((int64_t)(r >> 8) * (e >> 24)) >> 30));
  }
  return r > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)r;
}

// A divisor as a normalized reciprocal, so that several dividends can share
// the division: 1 / den ~= rcp * 2^(ds - 94)
struct Rcp {
  uint32_t rcp;
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
  r.rcp = rcpNorm((uint32_t)((u << r.ds) >> 32));
  return r;
}
// num / den * 2^shift, saturating at +-maxAbs
static inline int32_t mulRcp(int64_t num, const Rcp &d, int shift,
                             int32_t maxAbs) {
  if (num == 0 || d.zero) return 0;
  const bool neg = (num < 0) != d.neg;
  const uint64_t u = num < 0 ? (uint64_t)0 - (uint64_t)num : (uint64_t)num;
  const int ns = clz64(u);
  const uint64_t p = (uint64_t)(uint32_t)((u << ns) >> 32) * d.rcp;
  const int e = d.ds - ns - 62 + shift;  // num / den * 2^shift = p * 2^e
  uint64_t r;
  if (e >= 64 || (e > 0 && (p >> (64 - e)) != 0)) {
    r = (uint64_t)maxAbs;
  } else if (e >= 0) {
    r = p << e;
  } else {
    r = (-e >= 64) ? 0 : (p >> -e);
  }
  if (r > (uint64_t)maxAbs) r = (uint64_t)maxAbs;
  return neg ? -(int32_t)r : (int32_t)r;
}
static inline int32_t divQ(int64_t num, int64_t den, int shift,
                           int32_t maxAbs) {
  return mulRcp(num, makeRcp(den), shift, maxAbs);
}

// Integer square root (floor), Newton from above
static inline uint32_t isqrt32(uint32_t v) {
  if (v < 2) return v;
  const int bits = 32 - __builtin_clz(v);
  uint32_t r = 1u << ((bits + 1) >> 1);
  for (;;) {
    const uint32_t nr = (r + v / r) >> 1;
    if (nr >= r) return r;
    r = nr;
  }
}
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
  if (w <= 0 || h <= 0 || !cfg.arena) {
    *this = Graphics3D();
    return;
  }

  uintptr_t p = (uintptr_t)cfg.arena;
  const uintptr_t end = p + cfg.arenaSize;
  p = alignUp8(p);
  auto avail = [&]() -> size_t { return (end > p) ? (size_t)(end - p) : 0; };

  // Fixed allocations: line buckets (screenH x 2), layer table, matrix stack,
  // vertex cache
  const size_t bucketBytes = alignUp8((size_t)h * 2 * sizeof(uint16_t));
  const size_t layerBytes = alignUp8((size_t)LAYER_MAX * sizeof(LayerDesc));
  const size_t stackBytes = alignUp8((size_t)STACK_DEPTH * sizeof(StackEntry));
  const size_t vcacheBytes =
      alignUp8((size_t)VCACHE_SIZE * sizeof(CachedVertex));
  if (avail() < bucketBytes + layerBytes + stackBytes + vcacheBytes) {
    *this = Graphics3D();
    return;
  }
  bucketHead_ = (uint16_t *)p;
  bucketTail_ = bucketHead_ + h;
  p += bucketBytes;
  layers_ = (LayerDesc *)p;
  p += layerBytes;
  stack_ = (StackEntry *)p;
  p += stackBytes;
  vcache_ = (CachedVertex *)p;
  p += vcacheBytes;
  arenaFixed_ = (size_t)(p - (uintptr_t)cfg.arena);

  // Span pool: as requested, or a quarter of the remaining space
  int spanCap = cfg.spanCapacity;
  if (spanCap <= 0) {
    spanCap = (int)((avail() / 4) / sizeof(Span));
    spanCap = std::max(SPAN_CAPACITY_MIN, std::min(SPAN_CAPACITY_MAX, spanCap));
  }
  spanCap = std::min(spanCap, (int)(avail() / sizeof(Span)));
  spanPool_ = (Span *)p;
  spanCapacity_ = spanCap;
  p += (size_t)spanCap * sizeof(Span);
  p = alignUp8(p);

  // Triangle buffer: everything that remains, entries growing up from its
  // start and records down from its end
  size_t region = avail();
  if (region > REC_REGION_MAX) region = REC_REGION_MAX;
  region &= ~(size_t)(REC_ALIGN - 1);
  recBase_ = (uint8_t *)p;
  entries_ = (TriEntry *)p;
  recEnd_ = recBase_ + region;
  recTop_ = recEnd_;
  if (spanCap <= 0 || region < sizeof(TriEntry) + TRI_REC_SIZE[7]) {
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
// is full when the two meet.
uint8_t *Graphics3D::allocRecord(size_t size) {
  if (!recBase_ || triCount_ >= (int)NONE) return nullptr;
  uint8_t *rec = recTop_ - size;
  if (rec < recBase_ + (size_t)(triCount_ + 1) * sizeof(TriEntry)) {
    return nullptr;
  }
  recTop_ = rec;
  entries_[triCount_].rec = (uint16_t)((size_t)(rec - recBase_) / REC_UNIT);
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
      const int64_t xr = clampFix(((int64_t)vx * invW) >> IW_SHIFT, 1 << 27);
      const int64_t yr = clampFix(((int64_t)vy * invW) >> IW_SHIFT, 1 << 27);
      sv.sx = clampFix(p.cx + ((xr * p.fx) >> 8), SCREEN_MAX);
      sv.sy = clampFix(p.cy - ((yr * p.fy) >> 8), SCREEN_MAX);
      sv.z = clampFix((int64_t)p.zA + (((int64_t)p.zB * invW) >>
                                       (FP_SHIFT + IW_SHIFT - 24)),
                      Z_MAX_FP);
      return true;
    }
    case ProjKind::ORTHOGRAPHIC: {
      sv.sx = clampFix((((int64_t)vx * p.sxScale) >> 16) + p.sxOff, SCREEN_MAX);
      sv.sy = clampFix((((int64_t)vy * p.syScale) >> 16) + p.syOff, SCREEN_MAX);
      sv.z =
          clampFix((((int64_t)vz * p.zScale) >> FP_SHIFT) + p.zOff, Z_MAX_FP);
      invW = 1 << IW_SHIFT;
      return true;
    }
    default: {
      // Any matrix: Q18 entries on a 16.16 point
      auto row = [&](int r) -> int64_t {
        return (((int64_t)p.m[r] * vx + (int64_t)p.m[4 + r] * vy +
                 (int64_t)p.m[8 + r] * vz) >>
                MAT_SHIFT) +
               ((int64_t)p.m[12 + r] >> (MAT_SHIFT - FP_SHIFT));
      };
      const int64_t cx = row(0), cy = row(1), cz = row(2), w = row(3);
      if (w <= 0) return false;
      const int32_t rx = divQ(cx, w, FP_SHIFT, 1 << 27);
      const int32_t ry = divQ(cy, w, FP_SHIFT, 1 << 27);
      sv.sx = clampFix(p.cx + (((int64_t)rx * screenW_) >> 1), SCREEN_MAX);
      sv.sy = clampFix(p.cy - (((int64_t)ry * screenH_) >> 1), SCREEN_MAX);
      sv.z = divQ(cz, w, 24, Z_MAX_FP);
      invW = divQ(1, w, IW_SHIFT + FP_SHIFT, INT32_MAX);
      return true;
    }
  }
}

// The current matrix (Q18 / 16.16) on a float position -> view space 16.16
static inline void transformQ(const MatQ &m, const vec3f &pos, int32_t &vx,
                              int32_t &vy, int32_t &vz) {
  const int32_t px = fToFix(pos.x, FP_SHIFT, INT32_MAX);
  const int32_t py = fToFix(pos.y, FP_SHIFT, INT32_MAX);
  const int32_t pz = fToFix(pos.z, FP_SHIFT, INT32_MAX);
  vx = (int32_t)((((int64_t)m.r[0] * px + (int64_t)m.r[3] * py +
                   (int64_t)m.r[6] * pz) >>
                  MAT_SHIFT) +
                 m.t[0]);
  vy = (int32_t)((((int64_t)m.r[1] * px + (int64_t)m.r[4] * py +
                   (int64_t)m.r[7] * pz) >>
                  MAT_SHIFT) +
                 m.t[1]);
  vz = (int32_t)((((int64_t)m.r[2] * px + (int64_t)m.r[5] * py +
                   (int64_t)m.r[8] * pz) >>
                  MAT_SHIFT) +
                 m.t[2]);
}

// A material color (0..1, Q8) times the vertex color, as 8.8 of 0..255
static inline int32_t vertexColorQ(int32_t unitQ8, uint32_t c8, bool useVertex,
                                   bool add, int32_t alpha256) {
  int32_t r = unitQ8 * 255;  // Q8 of 0..1 -> 8.8 of 0..255
  if (useVertex) r = (int32_t)(((int64_t)r * c8 * 257) >> 16);  // x c / 255
  if (add) r = (int32_t)(((int64_t)r * alpha256) >> 8);
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
void Graphics3D::shadeVertex(const Vertex &in, const PrimSetup &ps,
                             CachedVertex &out) const {
  out.ok = false;
  int32_t vx, vy, vz;
  transformQ(curQ_, in.position, vx, vy, vz);
  // Triangles crossing or in front of the near plane are dropped
  if (vz > -projQ_.zNear) return;
  ShadedVertex &sv = out.sv;
  if (!projectQ(vx, vy, vz, sv, out.invW)) return;

  const int32_t nq[3] = {fToFix(in.normal.x, NORMAL_SHIFT, 1 << 18),
                         fToFix(in.normal.y, NORMAL_SHIFT, 1 << 18),
                         fToFix(in.normal.z, NORMAL_SHIFT, 1 << 18)};
  int32_t d = 0;  // diffuse factor, Q15
  int32_t n[3] = {0, 0, 0};
  if (ps.viewNormal) {
    const MatQ &m = curQ_;
    normalizeQ15(((int64_t)m.r[0] * nq[0] + (int64_t)m.r[3] * nq[1] +
                  (int64_t)m.r[6] * nq[2]) >>
                     MAT_SHIFT,
                 ((int64_t)m.r[1] * nq[0] + (int64_t)m.r[4] * nq[1] +
                  (int64_t)m.r[7] * nq[2]) >>
                     MAT_SHIFT,
                 ((int64_t)m.r[2] * nq[0] + (int64_t)m.r[5] * nq[1] +
                  (int64_t)m.r[8] * nq[2]) >>
                     MAT_SHIFT,
                 n);
    if (lightEnabled_) {
      d = -(int32_t)(((int64_t)n[0] * lightQ_.dir[0] +
                      (int64_t)n[1] * lightQ_.dir[1] +
                      (int64_t)n[2] * lightQ_.dir[2]) >>
                     NORMAL_SHIFT);
    }
  } else if (lightEnabled_) {
    d = (int32_t)(((int64_t)nq[0] * ps.lightModelQ[0] +
                   (int64_t)nq[1] * ps.lightModelQ[1] +
                   (int64_t)nq[2] * ps.lightModelQ[2]) >>
                  LIGHT_SHIFT);
  }

#if SHAPOGFX3D_TEXTURE
  if (ps.tex) {
    if (ps.envMap) {
      // Environment map UV from the view-space normal: (n + 1) / 2 in Q16
      // times the texture size is 16.16 texels
      sv.u = clampFix((int64_t)(n[0] + (1 << NORMAL_SHIFT)) * ps.texWq,
                      TEX_MAX_FP);
      sv.v = clampFix((int64_t)((1 << NORMAL_SHIFT) - n[1]) * ps.texHq,
                      TEX_MAX_FP);
    } else {
      sv.u = clampFix((int64_t)fToFix(in.uv.x, FP_SHIFT, 1 << 30) * ps.texWq,
                      TEX_MAX_FP);
      sv.v = clampFix((int64_t)fToFix(in.uv.y, FP_SHIFT, 1 << 30) * ps.texHq,
                      TEX_MAX_FP);
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
      r += (int32_t)((((int64_t)ps.dif[0] * lightQ_.col[0]) >> COLOR_SHIFT) *
                         d >>
                     NORMAL_SHIFT);
      g += (int32_t)((((int64_t)ps.dif[1] * lightQ_.col[1]) >> COLOR_SHIFT) *
                         d >>
                     NORMAL_SHIFT);
      b += (int32_t)((((int64_t)ps.dif[2] * lightQ_.col[2]) >> COLOR_SHIFT) *
                         d >>
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
// Triangle setup

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

#if SHAPOGFX3D_FIXED_POINT
void Graphics3D::emitTriangle(const CachedVertex &a, const CachedVertex &b,
                              const CachedVertex &c, const Material *mat,
                              const Texture *tex) {
  if (!a.ok || !b.ok || !c.ok) return;

  // Back-face culling (screen y points down, so front-facing = negative area)
  const int64_t area2 = (int64_t)(b.sv.sx - a.sv.sx) * (c.sv.sy - a.sv.sy) -
                        (int64_t)(c.sv.sx - a.sv.sx) * (b.sv.sy - a.sv.sy);
  if (area2 == 0) return;
  if (area2 > 0 && !(mat->flags & MaterialFlags::DOUBLE_SIDED)) return;

  // Sort the vertices by screen y
  const CachedVertex *cv[3] = {&a, &b, &c};
  if (cv[1]->sv.sy < cv[0]->sv.sy) std::swap(cv[0], cv[1]);
  if (cv[2]->sv.sy < cv[1]->sv.sy) std::swap(cv[1], cv[2]);
  if (cv[1]->sv.sy < cv[0]->sv.sy) std::swap(cv[0], cv[1]);
  const ShadedVertex &v0 = cv[0]->sv, &v1 = cv[1]->sv, &v2 = cv[2]->sv;

  // Rows whose centers the triangle covers: ceil(y0 - 0.5) .. floor(y2 - 0.5)
  int yMin = (v0.sy - FP_HALF + 0xFFFF) >> FP_SHIFT;
  int yMax = (v2.sy - FP_HALF) >> FP_SHIFT;
  yMin = std::max(yMin, 0);
  yMax = std::min(yMax, (int)screenH_ - 1);
  if (yMin > yMax) return;

  // Attribute planes (Cramer's rule on the sorted vertices). The screen
  // deltas are 16.16 px, so a gradient per pixel needs the extra shift.
  const int32_t x0 = v0.sx, y0 = v0.sy;
  const int64_t x10 = (int64_t)v1.sx - x0, y10 = (int64_t)v1.sy - y0;
  const int64_t x20 = (int64_t)v2.sx - x0, y20 = (int64_t)v2.sy - y0;
  const Rcp rd = makeRcp(x10 * y20 - x20 * y10);  // never 0 (area2 != 0)
  auto plane = [&](int32_t a0, int32_t a1, int32_t a2, int32_t gradMax) {
    const int64_t d1 = (int64_t)a1 - a0, d2 = (int64_t)a2 - a0;
    return Plane{a0, mulRcp(d1 * y20 - d2 * y10, rd, FP_SHIFT, gradMax),
                 mulRcp(d2 * x10 - d1 * x20, rd, FP_SHIFT, gradMax)};
  };

  TriHead h;
  h.sx[0] = x0;
  h.sy[0] = y0;
  h.sx[1] = v1.sx;
  h.sy[1] = v1.sy;
  const int64_t y21 = (int64_t)v2.sy - v1.sy;
  h.slope[0] = (y10 != 0) ? divQ(x10, y10, FP_SHIFT, SLOPE_MAX) : 0;
  h.slope[1] =
      (y21 != 0) ? divQ((int64_t)v2.sx - v1.sx, y21, FP_SHIFT, SLOPE_MAX) : 0;
  h.slope[2] = divQ(x20, y20, FP_SHIFT, SLOPE_MAX);  // y20 > 0 (area2 != 0)

  uint8_t flags = 0;
  // The long edge is on the left when the middle vertex lies to its right
  if (x0 + (((int64_t)h.slope[2] * y10) >> FP_SHIFT) < v1.sx) {
    flags |= TriFlags::LEFT_LONG;
  }

  // Interpolated color only where the three vertex colors differ; the record
  // of a flat triangle holds one color instead of three planes
#if SHAPOGFX3D_GOURAUD
  const bool smooth = !(v0.r == v1.r && v0.r == v2.r && v0.g == v1.g &&
                        v0.g == v2.g && v0.b == v1.b && v0.b == v2.b);
#else
  const bool smooth = false;  // flat shading: the first vertex as passed in
#endif
  if (!smooth) flags |= TriFlags::FLAT;

  const TexFmt tf = texFmtOf(tex);
  const bool textured = (tf != TexFmt::NONE);
  if (textured) flags |= TriFlags::TEX;
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

  h.mat = mat;
  h.sortKey = (a.viewZ >> 2) + (b.viewZ >> 2) + (c.viewZ >> 2);
  h.yMin = (int16_t)yMin;
  h.yMax = (int16_t)yMax;
  h.flags = flags;
  h.alpha64 = materialAlpha64(mat);
  h.rasterFn =
      (uint8_t)((int)tf * RASTER_PER_TEX + blend * 2 + (smooth ? 0 : 1));
  h.layer = layerByte();

  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  uint8_t *rec = allocRecord(TRI_REC_SIZE[recIndex(depth, smooth, textured)]);
  if (!rec) {  // buffer overflow: drop for this frame
    triDropped_++;
    return;
  }
  const int32_t biasQ = fToFix(depthBias_, 24, 1 << 30);
  makeRecord(rec, h, depth, smooth, textured, [&](auto &t) {
    using R = std::remove_reference_t<decltype(t)>;
    if constexpr (R::HAS_DEPTH) {
      t.z = plane(clampFix((int64_t)v0.z + biasQ, Z_MAX_FP),
                  clampFix((int64_t)v1.z + biasQ, Z_MAX_FP),
                  clampFix((int64_t)v2.z + biasQ, Z_MAX_FP), Z_DELTA_MAX_FP);
    }
    if constexpr (R::SMOOTH) {  // 8.8 -> 8.16
      t.r = plane(v0.r << 8, v1.r << 8, v2.r << 8, COLOR_GRAD_MAX);
      t.g = plane(v0.g << 8, v1.g << 8, v2.g << 8, COLOR_GRAD_MAX);
      t.b = plane(v0.b << 8, v1.b << 8, v2.b << 8, COLOR_GRAD_MAX);
    } else {
      // Flat: the color of the first vertex as passed in (before the y sort)
      t.r = (uint8_t)((a.sv.r + 128) >> 8);
      t.g = (uint8_t)((a.sv.g + 128) >> 8);
      t.b = (uint8_t)((a.sv.b + 128) >> 8);
    }
    if constexpr (R::TEXTURED) {
#if SHAPOGFX3D_TEXTURE
      int32_t u[3] = {v0.u, v1.u, v2.u}, v[3] = {v0.v, v1.v, v2.v};
      // Wrap texture coordinates per triangle to avoid fixed-point overflow
      // (subtract the texture period below the minimum from all three
      // vertices; relative values are unchanged). Sizes are powers of two.
      const int wPot = 1 << gfx2d::log2Floor(tex->width);
      const int hPot = 1 << gfx2d::log2Floor(tex->height);
      const int32_t uOff =
          (std::min({u[0], u[1], u[2]}) >> FP_SHIFT) & ~(wPot - 1);
      const int32_t vOff =
          (std::min({v[0], v[1], v[2]}) >> FP_SHIFT) & ~(hPot - 1);
      for (int i = 0; i < 3; i++) {
        u[i] -= uOff << FP_SHIFT;
        v[i] -= vOff << FP_SHIFT;
      }
#if SHAPOGFX3D_PERSPECTIVE >= 1
      // (u/w, v/w) are linear in screen space: 16.16 texels x Q26 -> Q12
      int32_t uw[3], vw[3], iw[3];
      for (int i = 0; i < 3; i++) {
        uw[i] = clampFix(
            ((int64_t)u[i] * cv[i]->invW) >> (FP_SHIFT + IW_SHIFT - UW_SHIFT),
            INT32_MAX);
        vw[i] = clampFix(
            ((int64_t)v[i] * cv[i]->invW) >> (FP_SHIFT + IW_SHIFT - UW_SHIFT),
            INT32_MAX);
        iw[i] = cv[i]->invW;
      }
      t.uw = plane(uw[0], uw[1], uw[2], GRAD_MAX);
      t.vw = plane(vw[0], vw[1], vw[2], GRAD_MAX);
      t.iw = plane(iw[0], iw[1], iw[2], GRAD_MAX);
#else
      t.u = plane(u[0], u[1], u[2], COLOR_GRAD_MAX);
      t.v = plane(v[0], v[1], v[2], COLOR_GRAD_MAX);
#endif
#endif  // SHAPOGFX3D_TEXTURE
    }
  });
  triCount_++;
}
#else
void Graphics3D::emitTriangle(const CachedVertex &a, const CachedVertex &b,
                              const CachedVertex &c, const Material *mat,
                              const Texture *tex) {
  if (!a.ok || !b.ok || !c.ok) return;

  // Back-face culling (screen y points down, so front-facing = negative area)
  const float area2 = (b.sv.sx - a.sv.sx) * (c.sv.sy - a.sv.sy) -
                      (c.sv.sx - a.sv.sx) * (b.sv.sy - a.sv.sy);
  if (area2 == 0.0f) return;
  if (area2 > 0.0f && !(mat->flags & MaterialFlags::DOUBLE_SIDED)) return;

  // Sort the vertices by screen y
  const CachedVertex *cv[3] = {&a, &b, &c};
  if (cv[1]->sv.sy < cv[0]->sv.sy) std::swap(cv[0], cv[1]);
  if (cv[2]->sv.sy < cv[1]->sv.sy) std::swap(cv[1], cv[2]);
  if (cv[1]->sv.sy < cv[0]->sv.sy) std::swap(cv[0], cv[1]);
  const ShadedVertex &v0 = cv[0]->sv, &v1 = cv[1]->sv, &v2 = cv[2]->sv;

  int yMin = ceilInt(v0.sy - 0.5f);
  int yMax = floorInt(v2.sy - 0.5f);
  yMin = std::max(yMin, 0);
  yMax = std::min(yMax, (int)screenH_ - 1);
  if (yMin > yMax) return;

  // Attribute planes (Cramer's rule on the sorted vertices)
  const float x0 = v0.sx, y0 = v0.sy;
  const float x10 = v1.sx - x0, y10 = v1.sy - y0;
  const float x20 = v2.sx - x0, y20 = v2.sy - y0;
  const float invDet = 1.0f / (x10 * y20 - x20 * y10);  // never 0 (area2 != 0)
  auto plane = [&](float a0, float a1, float a2) -> Plane {
    const float d1 = a1 - a0, d2 = a2 - a0;
    const float dx = (d1 * y20 - d2 * y10) * invDet;
    const float dy = (d2 * x10 - d1 * x20) * invDet;
    return {a0 - dx * x0 - dy * y0, dx, dy};
  };

  TriHead h;
  h.sx[0] = x0;
  h.sy[0] = y0;
  h.sx[1] = v1.sx;
  h.sy[1] = v1.sy;
  const float y21 = v2.sy - v1.sy;
  h.slope[0] = (y10 != 0.0f) ? x10 / y10 : 0.0f;
  h.slope[1] = (y21 != 0.0f) ? (v2.sx - v1.sx) / y21 : 0.0f;
  h.slope[2] = x20 / y20;  // y20 > 0, otherwise area2 == 0

  uint8_t flags = 0;
  // The long edge is on the left when the middle vertex lies to its right
  if ((x0 + h.slope[2] * y10) < v1.sx) flags |= TriFlags::LEFT_LONG;

    // Interpolated color only where the three vertex colors differ; the record
    // of a flat triangle holds one color instead of three planes
#if SHAPOGFX3D_GOURAUD
  const bool smooth = !(v0.r == v1.r && v0.r == v2.r && v0.g == v1.g &&
                        v0.g == v2.g && v0.b == v1.b && v0.b == v2.b);
#else
  const bool smooth = false;  // flat shading: the first vertex as passed in
#endif
  if (!smooth) flags |= TriFlags::FLAT;

  const TexFmt tf = texFmtOf(tex);
  const bool textured = (tf != TexFmt::NONE);
  if (textured) flags |= TriFlags::TEX;
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

  h.mat = mat;
  h.sortKey = floatSortKey(a.viewZ + b.viewZ + c.viewZ);
  h.yMin = (int16_t)yMin;
  h.yMax = (int16_t)yMax;
  h.flags = flags;
  h.alpha64 = materialAlpha64(mat);
  h.rasterFn =
      (uint8_t)((int)tf * RASTER_PER_TEX + blend * 2 + (smooth ? 0 : 1));
  h.layer = layerByte();

  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  uint8_t *rec = allocRecord(TRI_REC_SIZE[recIndex(depth, smooth, textured)]);
  if (!rec) {  // buffer overflow: drop for this frame
    triDropped_++;
    return;
  }
  makeRecord(rec, h, depth, smooth, textured, [&](auto &t) {
    using R = std::remove_reference_t<decltype(t)>;
    if constexpr (R::HAS_DEPTH) {
      t.z = plane(v0.zNdc + depthBias_, v1.zNdc + depthBias_,
                  v2.zNdc + depthBias_);
    }
    if constexpr (R::SMOOTH) {
      t.r = plane(v0.r, v1.r, v2.r);
      t.g = plane(v0.g, v1.g, v2.g);
      t.b = plane(v0.b, v1.b, v2.b);
    } else {
      // Flat: the color of the first vertex as passed in (before the y sort)
      t.r = (uint8_t)(int)(a.sv.r + 0.5f);
      t.g = (uint8_t)(int)(a.sv.g + 0.5f);
      t.b = (uint8_t)(int)(a.sv.b + 0.5f);
    }
    if constexpr (R::TEXTURED) {
#if SHAPOGFX3D_TEXTURE
      float u[3] = {v0.u, v1.u, v2.u}, v[3] = {v0.v, v1.v, v2.v};
      // Wrap texture coordinates per triangle to avoid fixed-point overflow
      // (subtract the texture period below the minimum from all three
      // vertices; relative values are unchanged). Sizes are powers of two.
      const int wPot = 1 << gfx2d::log2Floor(tex->width);
      const int hPot = 1 << gfx2d::log2Floor(tex->height);
      const float uMin = clampf(std::min({u[0], u[1], u[2]}), -1e6f, 1e6f);
      const float vMin = clampf(std::min({v[0], v[1], v[2]}), -1e6f, 1e6f);
      const float uOff = (float)((int)std::floor(uMin) & ~(wPot - 1));
      const float vOff = (float)((int)std::floor(vMin) & ~(hPot - 1));
      for (int i = 0; i < 3; i++) {
        u[i] -= uOff;
        v[i] -= vOff;
      }
#if SHAPOGFX3D_PERSPECTIVE >= 1
      for (int i = 0; i < 3; i++) {  // (u/w, v/w) are linear in screen space
        u[i] *= cv[i]->invW;
        v[i] *= cv[i]->invW;
      }
      t.uw = plane(u[0], u[1], u[2]);
      t.vw = plane(v[0], v[1], v[2]);
      t.iw = plane(cv[0]->invW, cv[1]->invW, cv[2]->invW);
#else
      t.u = plane(u[0], u[1], u[2]);
      t.v = plane(v[0], v[1], v[2]);
#endif
#endif  // SHAPOGFX3D_TEXTURE
    }
  });
  triCount_++;
}
#endif

// ---------------------------------------------------------------------------
// Points and lines
//
// They are unlit (diffuse x vertex color), never culled and clipped against
// the near plane. Both are stored in the triangle buffer and become spans in
// makeSpan(), so they are depth-resolved against everything else.

#if SHAPOGFX3D_UNLIT

#if SHAPOGFX3D_FIXED_POINT
void Graphics3D::unlitVertex(const Vertex &in, const Material *mat,
                             UnlitVertex &out) const {
  transformQ(curQ_, in.position, out.vx, out.vy, out.vz);
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

#if !SHAPOGFX3D_FIXED_POINT
static inline Plane constPlane(float v) { return {v, 0.0f, 0.0f}; }
#endif

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
  const int32_t biasQ = fToFix(depthBias_, 24, 1 << 30);
  const int32_t az = clampFix((int64_t)sa.z + biasQ, Z_MAX_FP);
  const int32_t bz = clampFix((int64_t)sb.z + biasQ, Z_MAX_FP);
  const int32_t ax = sa.sx, ay = sa.sy, bx = sb.sx, by = sb.sy;

  // Rows containing the end points; nothing to do when fully off screen
  int yMin = std::min(ay, by) >> FP_SHIFT;
  int yMax = std::max(ay, by) >> FP_SHIFT;
  yMin = std::max(yMin, 0);
  yMax = std::min(yMax, (int)screenH_ - 1);
  if (yMin > yMax) return;
  if (std::max(ax, bx) < 0 ||
      std::min(ax, bx) >= ((int32_t)screenW_ << FP_SHIFT))
    return;

  TriHead h;
  h.sx[0] = ax;
  h.sy[0] = ay;
  h.sx[1] = bx;
  h.sy[1] = by;
  const int64_t dx = (int64_t)bx - ax, dy = (int64_t)by - ay;
  h.slope[0] = (dy != 0) ? divQ(dx, dy, FP_SHIFT, SLOPE_MAX) : 0;  // dx / dy
  h.slope[1] = 0;
  h.slope[2] = 0;
  // Attributes vary along the major axis: per row for steep lines, per
  // column for shallow ones (see makeLineSpan)
  const bool steep = (dy < 0 ? -dy : dy) >= (dx < 0 ? -dx : dx);
  auto linePlane = [&](int32_t p, int32_t q, int32_t gradMax) {
    if (steep) {
      return Plane{p, 0,
                   (dy != 0) ? divQ((int64_t)q - p, dy, FP_SHIFT, gradMax) : 0};
    }
    return Plane{p, (dx != 0) ? divQ((int64_t)q - p, dx, FP_SHIFT, gradMax) : 0,
                 0};
  };
#if SHAPOGFX3D_GOURAUD
  const bool smooth = !(a.r == b.r && a.g == b.g && a.b == b.b);
#else
  const bool smooth = false;  // flat shading: the first end point
#endif

  uint8_t flags = TriFlags::LINE;
  if (!smooth) flags |= TriFlags::FLAT;
  flags |= opaqueFlag(mat);
  h.mat = mat;
  h.sortKey = (a.vz >> 1) + (b.vz >> 1);
  h.yMin = (int16_t)yMin;
  h.yMax = (int16_t)yMax;
  h.flags = flags;
  h.alpha64 = materialAlpha64(mat);
  h.rasterFn = unlitRasterFn(mat, !smooth);
  h.layer = layerByte();

  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  uint8_t *rec = allocRecord(TRI_REC_SIZE[recIndex(depth, smooth, false)]);
  if (!rec) {
    triDropped_++;
    return;
  }
  makeRecord(rec, h, depth, smooth, false, [&](auto &t) {
    using R = std::remove_reference_t<decltype(t)>;
    if constexpr (R::HAS_DEPTH) t.z = linePlane(az, bz, Z_DELTA_MAX_FP);
    if constexpr (R::SMOOTH) {
      t.r = linePlane(a.r << 8, b.r << 8, COLOR_GRAD_MAX);
      t.g = linePlane(a.g << 8, b.g << 8, COLOR_GRAD_MAX);
      t.b = linePlane(a.b << 8, b.b << 8, COLOR_GRAD_MAX);
    } else {
      t.r = (uint8_t)((a.r + 128) >> 8);
      t.g = (uint8_t)((a.g + 128) >> 8);
      t.b = (uint8_t)((a.b + 128) >> 8);
    }
  });
  triCount_++;
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
  az += depthBias_;
  bz += depthBias_;

  // Rows containing the end points; nothing to do when fully off screen
  int yMin = floorInt(std::min(ay, by));
  int yMax = floorInt(std::max(ay, by));
  yMin = std::max(yMin, 0);
  yMax = std::min(yMax, (int)screenH_ - 1);
  if (yMin > yMax) return;
  if (std::max(ax, bx) < 0.0f || std::min(ax, bx) >= (float)screenW_) return;

  TriHead h;
  h.sx[0] = ax;
  h.sy[0] = ay;
  h.sx[1] = bx;
  h.sy[1] = by;
  const float dx = bx - ax, dy = by - ay;
  const float invDx = (dx != 0.0f) ? 1.0f / dx : 0.0f;
  const float invDy = (dy != 0.0f) ? 1.0f / dy : 0.0f;
  h.slope[0] = invDx;
  h.slope[1] = invDy;
  h.slope[2] = 0.0f;
  // Attributes vary along the major axis: per row for steep lines, per
  // column for shallow ones (see makeLineSpan)
  const bool steep = std::fabs(dy) >= std::fabs(dx);
  auto linePlane = [&](float p, float q) -> Plane {
    if (steep) {
      const float k = (q - p) * invDy;
      return {p - k * ay, 0.0f, k};
    }
    const float k = (q - p) * invDx;
    return {p - k * ax, k, 0.0f};
  };
#if SHAPOGFX3D_GOURAUD
  const bool smooth = !(a.r == b.r && a.g == b.g && a.b == b.b);
#else
  const bool smooth = false;  // flat shading: the first end point
#endif

  uint8_t flags = TriFlags::LINE;
  if (!smooth) flags |= TriFlags::FLAT;
  flags |= opaqueFlag(mat);
  h.mat = mat;
  h.sortKey = floatSortKey(a.view.z + b.view.z);
  h.yMin = (int16_t)yMin;
  h.yMax = (int16_t)yMax;
  h.flags = flags;
  h.alpha64 = materialAlpha64(mat);
  h.rasterFn = unlitRasterFn(mat, !smooth);
  h.layer = layerByte();

  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  uint8_t *rec = allocRecord(TRI_REC_SIZE[recIndex(depth, smooth, false)]);
  if (!rec) {
    triDropped_++;
    return;
  }
  makeRecord(rec, h, depth, smooth, false, [&](auto &t) {
    using R = std::remove_reference_t<decltype(t)>;
    if constexpr (R::HAS_DEPTH) t.z = linePlane(az, bz);
    if constexpr (R::SMOOTH) {
      t.r = linePlane(a.r, b.r);
      t.g = linePlane(a.g, b.g);
      t.b = linePlane(a.b, b.b);
    } else {
      t.r = (uint8_t)(int)(a.r + 0.5f);
      t.g = (uint8_t)(int)(a.g + 0.5f);
      t.b = (uint8_t)(int)(a.b + 0.5f);
    }
  });
  triCount_++;
}
#endif

#if SHAPOGFX3D_FIXED_POINT
void Graphics3D::emitPoint(const UnlitVertex &a, const Material *mat) {
  if (a.vz > -projQ_.zNear) return;
  ShadedVertex sv;
  int32_t invW;
  if (!projectQ(a.vx, a.vy, a.vz, sv, invW)) return;
  const int32_t z =
      clampFix((int64_t)sv.z + fToFix(depthBias_, 24, 1 << 30), Z_MAX_FP);
  const int size = pointSize_;
  // Square of `size` pixels centered on the point: floor(s - size / 2 + 0.5)
  const int32_t half = (int32_t)size << (FP_SHIFT - 1);
  const int x0 = (sv.sx - half + FP_HALF) >> FP_SHIFT;
  const int y0 = (sv.sy - half + FP_HALF) >> FP_SHIFT;
  if (x0 + size <= 0 || x0 >= (int)screenW_) return;
  int yMin = std::max(y0, 0), yMax = std::min(y0 + size - 1, (int)screenH_ - 1);
  if (yMin > yMax) return;

  TriHead h;
  h.sx[0] = h.sx[1] = (int32_t)x0 << FP_SHIFT;
  h.sy[0] = h.sy[1] = (int32_t)y0 << FP_SHIFT;
  h.slope[0] = size;
  h.slope[1] = h.slope[2] = 0;
  uint8_t flags = TriFlags::POINT | TriFlags::FLAT;
  flags |= opaqueFlag(mat);
  h.mat = mat;
  h.sortKey = a.vz;
  h.yMin = (int16_t)yMin;
  h.yMax = (int16_t)yMax;
  h.flags = flags;
  h.alpha64 = materialAlpha64(mat);
  h.rasterFn = unlitRasterFn(mat, true);
  h.layer = layerByte();

  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  uint8_t *rec = allocRecord(TRI_REC_SIZE[recIndex(depth, false, false)]);
  if (!rec) {
    triDropped_++;
    return;
  }
  makeRecord(rec, h, depth, false, false, [&](auto &t) {
    using R = std::remove_reference_t<decltype(t)>;
    if constexpr (R::HAS_DEPTH) t.z = Plane{z, 0, 0};
    if constexpr (R::SMOOTH) {  // never selected: a point is always flat
      t.r = Plane{a.r << 8, 0, 0};
      t.g = Plane{a.g << 8, 0, 0};
      t.b = Plane{a.b << 8, 0, 0};
    } else {
      t.r = (uint8_t)((a.r + 128) >> 8);
      t.g = (uint8_t)((a.g + 128) >> 8);
      t.b = (uint8_t)((a.b + 128) >> 8);
    }
  });
  triCount_++;
}
#else
void Graphics3D::emitPoint(const UnlitVertex &a, const Material *mat) {
  if (a.view.z > -zNear_) return;
  float sx, sy, z, invW;
  if (!projectPoint(a.view, sx, sy, z, invW)) return;
  z += depthBias_;
  const int size = pointSize_;
  // Square of `size` pixels centered on the point
  const int x0 = floorInt(sx - size * 0.5f + 0.5f);
  const int y0 = floorInt(sy - size * 0.5f + 0.5f);
  if (x0 + size <= 0 || x0 >= (int)screenW_) return;
  int yMin = std::max(y0, 0), yMax = std::min(y0 + size - 1, (int)screenH_ - 1);
  if (yMin > yMax) return;

  TriHead h;
  h.sx[0] = h.sx[1] = (float)x0;
  h.sy[0] = h.sy[1] = (float)y0;
  h.slope[0] = (float)size;
  h.slope[1] = h.slope[2] = 0.0f;
  uint8_t flags = TriFlags::POINT | TriFlags::FLAT;
  flags |= opaqueFlag(mat);
  h.mat = mat;
  h.sortKey = floatSortKey(a.view.z);
  h.yMin = (int16_t)yMin;
  h.yMax = (int16_t)yMax;
  h.flags = flags;
  h.alpha64 = materialAlpha64(mat);
  h.rasterFn = unlitRasterFn(mat, true);
  h.layer = layerByte();

  const bool depth = !(h.layer & LayerId::NO_DEPTH);
  uint8_t *rec = allocRecord(TRI_REC_SIZE[recIndex(depth, false, false)]);
  if (!rec) {
    triDropped_++;
    return;
  }
  makeRecord(rec, h, depth, false, false, [&](auto &t) {
    using R = std::remove_reference_t<decltype(t)>;
    if constexpr (R::HAS_DEPTH) t.z = constPlane(z);
    if constexpr (R::SMOOTH) {  // never selected: a point is always flat
      t.r = constPlane(a.r);
      t.g = constPlane(a.g);
      t.b = constPlane(a.b);
    } else {
      t.r = (uint8_t)(int)(a.r + 0.5f);
      t.g = (uint8_t)(int)(a.g + 0.5f);
      t.b = (uint8_t)(int)(a.b + 0.5f);
    }
  });
  triCount_++;
}
#endif

#endif  // SHAPOGFX3D_UNLIT

// One vertex of a buffer. A buffer of plain vertices is used in place; a
// packed one is decoded into `tmp`, which the caller owns. The vertex cache
// means this happens once per vertex and primitive.
static inline const Vertex &vertexAt(const VertexBuffer &vb, uint16_t i,
                                     Vertex &tmp) {
  if (vb.vertices) return vb.vertices[i];
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

const CachedVertex &Graphics3D::fetchVertex(const VertexBuffer &vb, uint16_t vi,
                                            const PrimSetup &ps) {
  static const CachedVertex INVALID = {};  // ok == false: drops the triangle
  if (vi >= vb.vertexCount) {  // out-of-range index: the triangle is dropped
    badIndices_++;
    return INVALID;
  }
  CachedVertex &cv = vcache_[vi & (VCACHE_SIZE - 1)];
  if (cv.tag != vi) {
    Vertex tmp;
    shadeVertex(vertexAt(vb, vi, tmp), ps, cv);
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
  Vertex tmp;
  unlitVertex(vertexAt(vb, vi, tmp), mat, out);
  return true;
}
#endif

void Graphics3D::putPrimitive(const Primitive &prim) {
  if (!recBase_) return;
  const Material *mat = prim.material ? prim.material : curMat_;
  if (!mat) return;
  if (!prim.vertexBuffer || !prim.indices) return;
  const VertexBuffer &vb = *prim.vertexBuffer;
  if (!vb.vertices && !vb.packed) return;
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
static inline const TriHead *recOf(const uint8_t *base, const TriEntry &e) {
  return (const TriHead *)(base + (size_t)e.rec * REC_UNIT);
}

void Graphics3D::beginRender() {
  // Sort each layer farthest first (ascending view-space z: more negative
  // comes first). Depth order between opaque spans of the same layer is
  // resolved by the depth test at span insertion, so this sort mainly
  // determines the compositing order of translucent primitives; between
  // layers the order the layers were opened in decides. Only the entries are
  // permuted, never the records.
  spanPeak_ = 0;
  spanDropped_ = 0;
  if (!recBase_) return;
  const uint8_t *base = recBase_;
  for (int i = 0; i < layerCount_; i++) {
    if (layers_[i].id & LayerId::NO_DEPTH) continue;  // kept in the order added
    const int first = layers_[i].first;
    const int last = (i + 1 < layerCount_) ? layers_[i + 1].first : triCount_;
    std::sort(entries_ + first, entries_ + last,
              [base](const TriEntry &a, const TriEntry &b) {
                const int32_t ka = recOf(base, a)->sortKey;
                const int32_t kb = recOf(base, b)->sortKey;
                // Equal depth keeps the order the primitives were added in:
                // records grow downwards, so the earlier one sits higher.
                if (ka != kb) return ka < kb;
                return a.rec > b.rec;
              });
  }
}

void Graphics3D::endRender() {}

SHAPOGFX3D_HOT_ATTR Span *Graphics3D::allocSpan() {
  if (spanCount_ >= spanCapacity_) {
    spanDropped_++;
    return nullptr;
  }
  return &spanPool_[spanCount_++];
}

// Advance the left end of a span by n pixels (updating attributes by their
// increments)
static inline void spanAdvance(Span &sp, int n) {
  sp.x0 += n;
  sp.z0 += sp.dz * n;
#if SHAPOGFX3D_GOURAUD
  sp.r += sp.dr * n;
  sp.g += sp.dg * n;
  sp.b += sp.db * n;
#endif
#if SHAPOGFX3D_TEXTURE
#if SHAPOGFX3D_PERSPECTIVE == 2
  sp.uw += sp.duw * n;
  sp.vw += sp.dvw * n;
  sp.iw += sp.diw * n;
#else
  sp.u += sp.du * n;
  sp.v += sp.dv * n;
#endif
#endif
}

// Is frag nearer than e at the center of the overlap [ox0, ox1)?
//
// Spans reach the list in layer order, so a span whose layer differs from
// another one's belongs to a later layer and is by definition the nearer one;
// inside a layer without depth the later span wins for the same reason. Only
// within a layer that has depth are the two depths compared, doubled so that
// the half-pixel center stays integer.
static inline bool fragNearer(const Span &frag, const Span &e, int ox0,
                              int ox1) {
  if (frag.lay != e.lay) return true;
  if (frag.lay & LayerId::NO_DEPTH) return true;
  const int k = ox0 + ox1 - 1;  // 2 * center
  const int64_t zf =
      2 * (int64_t)frag.z0 + (int64_t)frag.dz * (k - 2 * frag.x0);
  const int64_t ze = 2 * (int64_t)e.z0 + (int64_t)e.dz * (k - 2 * e.x0);
  return zf < ze;
}

// Remove the range [ox0, ox1) from list element e = *pp.
// Returns the position at which to continue scanning (after the remaining part,
// or the rest of e).
SHAPOGFX3D_HOT_ATTR Span **Graphics3D::cutSpan(Span **pp, int ox0, int ox1) {
  Span *e = *pp;
  bool leftRemains = e->x0 < ox0;
  bool rightRemains = e->x1 > ox1;
  if (leftRemains && rightRemains) {
    // Hole in the middle: split the right part into a new span (dropped if the
    // pool is full)
    Span *r = allocSpan();
    if (r) {
      *r = *e;
      spanAdvance(*r, ox1 - r->x0);
      r->next = e->next;
      e->next = r;
    }
    e->x1 = ox0;
    return &e->next;
  } else if (leftRemains) {
    e->x1 = ox0;
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
SHAPOGFX3D_HOT_ATTR void Graphics3D::appendTranslucent(const Span &sp, int x1) {
  Span *n = allocSpan();
  if (!n) return;  // pool overflow: drop this span
  *n = sp;
  n->x1 = x1;
  n->next = nullptr;
  if (transTail_) {
    transTail_->next = n;
  } else {
    transHead_ = n;
  }
  transTail_ = n;
}

#endif  // SHAPOGFX3D_BLEND

// Insert an opaque span into the list sorted by x.
// Overlaps with existing spans are resolved by comparing depth at the center of
// the overlap and removing the farther part. Because this does not rely on the
// per-triangle sort order, large and small polygons are ordered correctly too.
SHAPOGFX3D_HOT_ATTR void Graphics3D::insertOpaque(Span &frag) {
  Span **pp = &opaqueHead_;
  while (*pp && (*pp)->x1 <= frag.x0) pp = &(*pp)->next;
  while (*pp && (*pp)->x0 < frag.x1) {
    Span *e = *pp;
    int ox0 = std::max(e->x0, frag.x0);
    int ox1 = std::min(e->x1, frag.x1);
    if (fragNearer(frag, *e, ox0, ox1)) {
      pp = cutSpan(pp, ox0, ox1);
    } else {
      // The part sticking out to the left of e is final (elements before it are
      // done)
      if (frag.x0 < ox0) {
        Span *n = allocSpan();
        if (n) {
          *n = frag;
          n->x1 = ox0;
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
    Span *n = allocSpan();
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
SHAPOGFX3D_HOT_ATTR void Graphics3D::insertTranslucent(Span &frag) {
  Span **pp = &opaqueHead_;
  while (*pp && (*pp)->x1 <= frag.x0) pp = &(*pp)->next;
  while (*pp && (*pp)->x0 < frag.x1) {
    Span *e = *pp;
    int ox0 = std::max(e->x0, frag.x0);
    int ox1 = std::min(e->x1, frag.x1);
    if (fragNearer(frag, *e, ox0, ox1)) {
      pp = &e->next;  // translucent is nearer: keep both
      continue;
    }
    if (frag.x0 < ox0) appendTranslucent(frag, ox0);
    if (frag.x1 > ox1) {
      spanAdvance(frag, ox1 - frag.x0);
      pp = &e->next;
    } else {
      return;
    }
  }
  if (frag.x0 < frag.x1) appendTranslucent(frag, frag.x1);
}

// Remove the parts of translucent spans that lie behind the new opaque span
// frag
SHAPOGFX3D_HOT_ATTR void Graphics3D::clipTranslucent(const Span &frag) {
  Span **pp = &transHead_;
  bool modified = false;
  while (*pp) {
    Span *e = *pp;
    if (e->x1 <= frag.x0 || e->x0 >= frag.x1) {
      pp = &e->next;
      continue;
    }
    int ox0 = std::max(e->x0, frag.x0);
    int ox1 = std::min(e->x1, frag.x1);
    if (fragNearer(frag, *e, ox0, ox1)) {
      pp = cutSpan(pp, ox0, ox1);
      modified = true;
    } else {
      pp = &e->next;
    }
  }
  if (modified) {
    transTail_ = nullptr;
    for (Span *e = transHead_; e; e = e->next) transTail_ = e;
  }
}

#endif  // SHAPOGFX3D_BLEND

// Texture attributes of a span that samples no texture. They are never read
// by its rasterizer, but spanAdvance() steps them like any other span.
#if SHAPOGFX3D_FIXED_POINT
static inline void clearSpanTex(Span &out) {
#if !SHAPOGFX3D_TEXTURE
  (void)out;
#else
#if SHAPOGFX3D_PERSPECTIVE == 2
  out.uw = out.vw = out.duw = out.dvw = out.diw = 0;
  out.iw = 1 << IW_SHIFT;
#else
  out.u = out.v = out.du = out.dv = 0;
#endif
#endif
}
#else
static inline void clearSpanTex(Span &out) {
#if !SHAPOGFX3D_TEXTURE
  (void)out;
#else
#if SHAPOGFX3D_PERSPECTIVE == 2
  out.uw = out.vw = out.duw = out.dvw = 0.0f;
  out.iw = 1.0f;
  out.diw = 0.0f;
#else
  out.u = out.v = out.du = out.dv = 0;
#endif
#endif
}
#endif

// Fill the depth and color of a span covering [x0, x1) from the planes of t,
// evaluated at the center of the leftmost pixel (xc, yc). A record without an
// attribute costs neither the evaluation nor the storage of its plane.
#if SHAPOGFX3D_FIXED_POINT
template <typename REC>
static inline void fillSpanBase(const REC &t, int32_t xc, int32_t yc, int x0,
                                int x1, Span &out) {
  // Offsets of the leftmost pixel center from the record's reference point
  const int32_t ox = xc - t.sx[0], oy = yc - t.sy[0];
  out.x0 = x0;
  out.x1 = x1;
  if constexpr (REC::HAS_DEPTH) {
    out.z0 = t.z.at(ox, oy);
    out.dz = t.z.dx;
  } else {
    out.z0 = 0;  // a layer without depth never compares them (fragNearer())
    out.dz = 0;
  }
#if SHAPOGFX3D_GOURAUD
  if constexpr (REC::SMOOTH) {
    // Color: the plane values at pixel centers inside the triangle lie within
    // 0..255 up to rounding; clamp the start value to guard the rounding.
    constexpr int32_t MAX16 = 255 << FIX_SHIFT;
    auto clamp16 = [](int32_t v) {
      return v < 0 ? 0 : (v > MAX16 ? MAX16 : v);
    };
    out.r = clamp16(t.r.at(ox, oy));
    out.g = clamp16(t.g.at(ox, oy));
    out.b = clamp16(t.b.at(ox, oy));
    out.dr = t.r.dx;
    out.dg = t.g.dx;
    out.db = t.b.dx;
  } else {
    out.r = (int32_t)t.r << FIX_SHIFT;
    out.g = (int32_t)t.g << FIX_SHIFT;
    out.b = (int32_t)t.b << FIX_SHIFT;
    out.dr = out.dg = out.db = 0;
  }
#else
  out.r = t.r;
  out.g = t.g;
  out.b = t.b;
#endif
  out.lay = t.layer;
  out.tri = &t;
  out.next = nullptr;
}
#else
template <typename REC>
static inline void fillSpanBase(const REC &t, float xc, float yc, int x0,
                                int x1, Span &out) {
  out.x0 = x0;
  out.x1 = x1;
  if constexpr (REC::HAS_DEPTH) {
    out.z0 = toZ(t.z.at(xc, yc));
    out.dz = toZDelta(t.z.dx);
  } else {
    out.z0 = 0;  // a layer without depth never compares them (fragNearer())
    out.dz = 0;
  }
#if SHAPOGFX3D_GOURAUD
  if constexpr (REC::SMOOTH) {
    // Color: the plane values at pixel centers inside the triangle lie within
    // 0..255 up to rounding; clamp the start value to guard the rounding.
    out.r = toFixColor(t.r.at(xc, yc));
    out.g = toFixColor(t.g.at(xc, yc));
    out.b = toFixColor(t.b.at(xc, yc));
    out.dr = toFixDelta(t.r.dx);
    out.dg = toFixDelta(t.g.dx);
    out.db = toFixDelta(t.b.dx);
  } else {
    out.r = (int32_t)t.r << FIX_SHIFT;
    out.g = (int32_t)t.g << FIX_SHIFT;
    out.b = (int32_t)t.b << FIX_SHIFT;
    out.dr = out.dg = out.db = 0;
  }
#else
  out.r = t.r;
  out.g = t.g;
  out.b = t.b;
#endif
  out.lay = t.layer;
  out.tri = &t;
  out.next = nullptr;
}
#endif

// Span of an untextured primitive (points and lines)
#if SHAPOGFX3D_UNLIT
template <typename REC>
#if SHAPOGFX3D_FIXED_POINT
static inline void fillUnlitSpan(const REC &t, int32_t xc, int32_t yc, int x0,
                                 int x1, Span &out) {
#else
static inline void fillUnlitSpan(const REC &t, float xc, float yc, int x0,
                                 int x1, Span &out) {
#endif
  fillSpanBase(t, xc, yc, x0, x1, out);
  clearSpanTex(out);
}
#endif

#if SHAPOGFX3D_LINES
// Line segment a -> b on pixel row yi: one pixel per row for steep lines, one
// pixel per column (a horizontal run) for shallow lines, so the coverage
// matches a Bresenham line. Both end points are drawn.
#if SHAPOGFX3D_FIXED_POINT
template <typename REC>
static bool makeLineSpan(const REC &t, int yi, int rx0, int rx1, Span &out) {
  const int32_t ax = t.sx[0], ay = t.sy[0], bx = t.sx[1], by = t.sy[1];
  const int64_t dx = (int64_t)bx - ax, dy = (int64_t)by - ay;
  const int32_t dxdy = t.slope[0];  // 16.16 px per px
  int c0, c1;
  if ((dy < 0 ? -dy : dy) >= (dx < 0 ? -dx : dx)) {
    // Steep: the pixel at the row center, with the parameter kept within
    // the segment
    int64_t o = (int64_t)(((int32_t)yi << FP_SHIFT) + FP_HALF) - ay;
    if (dy > 0) {
      o = o < 0 ? 0 : (o > dy ? dy : o);
    } else {
      o = o > 0 ? 0 : (o < dy ? dy : o);
    }
    c0 = (int)((ax + ((o * dxdy) >> FP_SHIFT)) >> FP_SHIFT);
    c1 = c0 + 1;
    if (c0 < rx0 || c0 >= rx1) return false;
  } else {
    // Shallow: columns whose center's y falls into [yi, yi + 1)
    const int ca = ax >> FP_SHIFT, cb = bx >> FP_SHIFT;
    const int cMin = std::min(ca, cb), cMax = std::max(ca, cb);
    if (dy == 0) {
      c0 = cMin;
      c1 = cMax + 1;
    } else {
      auto xAt = [&](int64_t y) {  // x of the line at y (16.16), bounded
        const int64_t x = ax + (((y - ay) * dxdy) >> FP_SHIFT);
        return x < -((int64_t)1 << 40)
                   ? -((int64_t)1 << 40)
                   : (x > ((int64_t)1 << 40) ? ((int64_t)1 << 40) : x);
      };
      int64_t xA = xAt((int64_t)yi << FP_SHIFT);
      int64_t xB = xAt((int64_t)(yi + 1) << FP_SHIFT);
      if (xA > xB) std::swap(xA, xB);
      c0 = (int)((xA - FP_HALF + 0xFFFF) >> FP_SHIFT);  // ceil(x - 0.5)
      c1 = (int)((xB - FP_HALF + 0xFFFF) >> FP_SHIFT);
      // The rows of the end points always include the end point pixels
      if (yi == (ay >> FP_SHIFT)) {
        c0 = std::min(c0, ca);
        c1 = std::max(c1, ca + 1);
      }
      if (yi == (by >> FP_SHIFT)) {
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
  fillUnlitSpan(t, ((int32_t)c0 << FP_SHIFT) + FP_HALF,
                ((int32_t)yi << FP_SHIFT) + FP_HALF, c0, c1, out);
  return true;
}
#else
template <typename REC>
static bool makeLineSpan(const REC &t, int yi, int rx0, int rx1, Span &out) {
  const float ax = t.sx[0], ay = t.sy[0], bx = t.sx[1], by = t.sy[1];
  const float dx = bx - ax, dy = by - ay;
  const float invDy = t.slope[1];
  int c0, c1;
  if (std::fabs(dy) >= std::fabs(dx)) {
    // Steep: the pixel at the row center
    float tt = (dy != 0.0f) ? clamp01(((float)yi + 0.5f - ay) * invDy) : 0.0f;
    c0 = floorInt(ax + dx * tt);
    c1 = c0 + 1;
    if (c0 < rx0 || c0 >= rx1) return false;
  } else {
    // Shallow: columns whose center's y falls into [yi, yi + 1)
    const int ca = floorInt(ax), cb = floorInt(bx);
    const int cMin = std::min(ca, cb), cMax = std::max(ca, cb);
    if (dy == 0.0f) {
      c0 = cMin;
      c1 = cMax + 1;
    } else {
      float xA = ax + dx * (((float)yi - ay) * invDy);
      float xB = ax + dx * (((float)yi + 1.0f - ay) * invDy);
      if (xA > xB) std::swap(xA, xB);
      c0 = ceilInt(xA - 0.5f);
      c1 = ceilInt(xB - 0.5f);
      // The rows of the end points always include the end point pixels
      if (yi == floorInt(ay)) {
        c0 = std::min(c0, ca);
        c1 = std::max(c1, ca + 1);
      }
      if (yi == floorInt(by)) {
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
  fillUnlitSpan(t, (float)c0 + 0.5f, (float)yi + 0.5f, c0, c1, out);
  return true;
}
#endif
#endif  // SHAPOGFX3D_LINES

#if SHAPOGFX3D_POINTS
// Point: a square with its top-left pixel at (sx[0], sy[0]), size slope[0]
#if SHAPOGFX3D_FIXED_POINT
template <typename REC>
static bool makePointSpan(const REC &t, int yi, int rx0, int rx1, Span &out) {
  const int size = (int)t.slope[0];
  const int x0 = t.sx[0] >> FP_SHIFT;
  int c0 = std::max(x0, rx0);
  int c1 = std::min(x0 + size, rx1);
  if (c0 >= c1) return false;
  fillUnlitSpan(t, ((int32_t)c0 << FP_SHIFT) + FP_HALF,
                ((int32_t)yi << FP_SHIFT) + FP_HALF, c0, c1, out);
  return true;
}
#else
template <typename REC>
static bool makePointSpan(const REC &t, int yi, int rx0, int rx1, Span &out) {
  const int size = (int)t.slope[0];
  int c0 = std::max((int)t.sx[0], rx0);
  int c1 = std::min((int)t.sx[0] + size, rx1);
  if (c0 >= c1) return false;
  fillUnlitSpan(t, (float)c0 + 0.5f, (float)yi + 0.5f, c0, c1, out);
  return true;
}
#endif
#endif  // SHAPOGFX3D_POINTS

// Build the span of triangle t on the scanline with center yc, limited to
// the region [rx0, rx1). Returns false when the triangle covers no pixel
// center there.
#if SHAPOGFX3D_FIXED_POINT
template <typename REC>
static bool makeTriSpan(const REC &t, int yi, int rx0, int rx1, Span &out) {
  // Edge x at the row center: the long edge, and the short edge covering
  // this row (the top->middle edge covers [sy0, sy1), middle->bottom covers
  // [sy1, sy2))
  const int32_t yc = ((int32_t)yi << FP_SHIFT) + FP_HALF;
  const int64_t oy = (int64_t)yc - t.sy[0];
  const int64_t xLong = t.sx[0] + (((int64_t)t.slope[2] * oy) >> FP_SHIFT);
  const int64_t xShort =
      (yc < t.sy[1])
          ? t.sx[0] + (((int64_t)t.slope[0] * oy) >> FP_SHIFT)
          : t.sx[1] +
                (((int64_t)t.slope[1] * ((int64_t)yc - t.sy[1])) >> FP_SHIFT);
  const bool leftLong = (t.flags & TriFlags::LEFT_LONG) != 0;
  const int64_t xl = leftLong ? xLong : xShort;
  const int64_t xr = leftLong ? xShort : xLong;
  const int64_t width = xr - xl;
  if (width <= 0) return false;

  // Fill the pixels whose centers (xi + 0.5) fall inside [xl, xr)
  int xi0 = (int)((xl - FP_HALF + 0xFFFF) >> FP_SHIFT);
  int xi1 = (int)((xr - FP_HALF + 0xFFFF) >> FP_SHIFT);
  xi0 = std::max(xi0, rx0);
  xi1 = std::min(xi1, rx1);
  if (xi0 >= xi1) return false;

  const int32_t xc = ((int32_t)xi0 << FP_SHIFT) + FP_HALF;
  fillSpanBase(t, xc, yc, xi0, xi1, out);

  if constexpr (!REC::TEXTURED) {
    clearSpanTex(out);
  } else {
#if SHAPOGFX3D_TEXTURE
#if SHAPOGFX3D_PERSPECTIVE == 2
    const int32_t ox = xc - t.sx[0];
    out.uw = t.uw.at(ox, (int32_t)oy);
    out.vw = t.vw.at(ox, (int32_t)oy);
    out.iw = t.iw.at(ox, (int32_t)oy);
    out.duw = t.uw.dx;
    out.dvw = t.vw.dx;
    out.diw = t.iw.dx;
#elif SHAPOGFX3D_PERSPECTIVE == 1
    // Vertical-only correction: (u, v) are exact at the span end points and
    // interpolated affinely in between. u = (u/w) / (1/w), in 16.16 texels:
    // Q12 / Q26 needs the shift of 26 - 12 + 16.
    const int32_t oxl = (int32_t)(xl - t.sx[0]), oxr = (int32_t)(xr - t.sx[0]);
    const int32_t iwL = t.iw.at(oxl, (int32_t)oy),
                  iwR = t.iw.at(oxr, (int32_t)oy);
    constexpr int UV_SHIFT = IW_SHIFT - UW_SHIFT + FP_SHIFT;
    const int32_t uL =
        divQ(t.uw.at(oxl, (int32_t)oy), iwL, UV_SHIFT, TEX_MAX_FP);
    const int32_t uR =
        divQ(t.uw.at(oxr, (int32_t)oy), iwR, UV_SHIFT, TEX_MAX_FP);
    const int32_t vL =
        divQ(t.vw.at(oxl, (int32_t)oy), iwL, UV_SHIFT, TEX_MAX_FP);
    const int32_t vR =
        divQ(t.vw.at(oxr, (int32_t)oy), iwR, UV_SHIFT, TEX_MAX_FP);
    const int32_t du = divQ((int64_t)uR - uL, width, FP_SHIFT, COLOR_GRAD_MAX);
    const int32_t dv = divQ((int64_t)vR - vL, width, FP_SHIFT, COLOR_GRAD_MAX);
    const int64_t t0 = xc - xl;
    out.u = uL + (int32_t)(((int64_t)du * t0) >> FP_SHIFT);
    out.v = vL + (int32_t)(((int64_t)dv * t0) >> FP_SHIFT);
    out.du = du;
    out.dv = dv;
#else
    const int32_t ox = xc - t.sx[0];
    out.u = t.u.at(ox, (int32_t)oy);
    out.v = t.v.at(ox, (int32_t)oy);
    out.du = t.u.dx;
    out.dv = t.v.dx;
#endif
#endif  // SHAPOGFX3D_TEXTURE
  }
  return true;
}
#else
template <typename REC>
static bool makeTriSpan(const REC &t, int yi, int rx0, int rx1, Span &out) {
  const float yc = (float)yi + 0.5f;
  // Edge x at yc: the long edge, and the short edge covering this row (the
  // top->middle edge covers [sy0, sy1), middle->bottom covers [sy1, sy2))
  const float xLong = t.sx[0] + t.slope[2] * (yc - t.sy[0]);
  const float xShort = (yc < t.sy[1]) ? t.sx[0] + t.slope[0] * (yc - t.sy[0])
                                      : t.sx[1] + t.slope[1] * (yc - t.sy[1]);
  const bool leftLong = (t.flags & TriFlags::LEFT_LONG) != 0;
  const float xl = leftLong ? xLong : xShort;
  const float xr = leftLong ? xShort : xLong;
  const float width = xr - xl;
  if (width <= 0.0f) return false;

  // Fill the pixels whose centers (xi + 0.5) fall inside [xl, xr)
  int xi0 = ceilInt(xl - 0.5f);
  int xi1 = ceilInt(xr - 0.5f);
  xi0 = std::max(xi0, rx0);
  xi1 = std::min(xi1, rx1);
  if (xi0 >= xi1) return false;

  const float xc = (float)xi0 + 0.5f;
  fillSpanBase(t, xc, yc, xi0, xi1, out);

  if constexpr (!REC::TEXTURED) {
    clearSpanTex(out);
  } else {
#if SHAPOGFX3D_TEXTURE
#if SHAPOGFX3D_PERSPECTIVE == 2
    out.uw = t.uw.at(xc, yc);
    out.vw = t.vw.at(xc, yc);
    out.iw = t.iw.at(xc, yc);
    out.duw = t.uw.dx;
    out.dvw = t.vw.dx;
    out.diw = t.iw.dx;
#elif SHAPOGFX3D_PERSPECTIVE == 1
    // Vertical-only correction: (u, v) are exact at the span end points and
    // interpolated affinely in between. The three divides (1/w at both ends,
    // 1/width) are folded into one.
    const float iwL = t.iw.at(xl, yc), iwR = t.iw.at(xr, yc);
    const float uwL = t.uw.at(xl, yc), uwR = t.uw.at(xr, yc);
    const float vwL = t.vw.at(xl, yc), vwR = t.vw.at(xr, yc);
    const float k = 1.0f / (iwL * iwR * width);
    const float kL = iwR * width * k;  // 1 / iwL
    const float du = (uwR * iwL - uwL * iwR) * k;
    const float dv = (vwR * iwL - vwL * iwR) * k;
    const float t0 = xc - xl;
    out.u = toFixTex(uwL * kL + du * t0);
    out.v = toFixTex(vwL * kL + dv * t0);
    out.du = toFixDelta(du);
    out.dv = toFixDelta(dv);
#else
    out.u = toFixTex(t.u.at(xc, yc));
    out.v = toFixTex(t.v.at(xc, yc));
    out.du = toFixDelta(t.u.dx);
    out.dv = toFixDelta(t.v.dx);
#endif
#endif  // SHAPOGFX3D_TEXTURE
  }
  return true;
}
#endif

// Span of a primitive of a known record layout. Lines and points are never
// textured, so their span builders are only compiled into the untextured
// layouts.
template <bool D, bool G, bool T>
static inline bool makeSpanRec(const TriHead &h, int yi, int rx0, int rx1,
                               Span &out) {
  const TriRec<D, G, T> &t = static_cast<const TriRec<D, G, T> &>(h);
  if constexpr (!T) {
#if SHAPOGFX3D_LINES
    if (h.flags & TriFlags::LINE) return makeLineSpan(t, yi, rx0, rx1, out);
#endif
#if SHAPOGFX3D_POINTS
    if (h.flags & TriFlags::POINT) return makePointSpan(t, yi, rx0, rx1, out);
#endif
  }
  return makeTriSpan(t, yi, rx0, rx1, out);
}

// Recover the record layout from the header and build the span
template <bool D, bool G>
static inline bool makeSpanT(const TriHead &h, int yi, int rx0, int rx1,
                             Span &out) {
#if SHAPOGFX3D_TEXTURE
  if (h.flags & TriFlags::TEX)
    return makeSpanRec<D, G, true>(h, yi, rx0, rx1, out);
#endif
  return makeSpanRec<D, G, false>(h, yi, rx0, rx1, out);
}
template <bool D>
static inline bool makeSpanG(const TriHead &h, int yi, int rx0, int rx1,
                             Span &out) {
#if SHAPOGFX3D_GOURAUD
  if (!(h.flags & TriFlags::FLAT))
    return makeSpanT<D, true>(h, yi, rx0, rx1, out);
#endif
  return makeSpanT<D, false>(h, yi, rx0, rx1, out);
}
static inline bool makeSpan(const TriHead &h, int yi, int rx0, int rx1,
                            Span &out) {
  if (h.layer & LayerId::NO_DEPTH)
    return makeSpanG<false>(h, yi, rx0, rx1, out);
  return makeSpanG<true>(h, yi, rx0, rx1, out);
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

#if SHAPOGFX3D_RP2_INTERP && SHAPOGFX3D_TEXTURE
// Same, through the SIO interpolator: lane 0 turns u into the byte offset of
// the texel in its row, lane 1 turns v into the byte offset of the row, and
// POP_FULL returns the texel address and steps both accumulators. Only for
// 16-bit texels with a power-of-two stride.
template <TexFmt T>
struct InterpTex {
  static bool usable(const Texture &tex) {
    const uint32_t s = tex.stride;
    return tex.width >= 2 && tex.height >= 2 && s >= 2 && (s & (s - 1)) == 0 &&
           gfx2d::log2Floor((int)s) <= FIX_SHIFT;
  }
  void init(const Texture &tex, int32_t u0, int32_t v0, int32_t du0,
            int32_t dv0) {
    const int log2w = gfx2d::log2Floor(tex.width);
    const int log2h = gfx2d::log2Floor(tex.height);
    const int log2s = gfx2d::log2Floor((int)tex.stride);
    interp_config c = interp_default_config();
    interp_config_set_add_raw(&c, true);
    interp_config_set_shift(&c, FIX_SHIFT - 1);  // texel index x 2 bytes
    interp_config_set_mask(&c, 1, log2w);
    interp_set_config(interp0, 0, &c);
    interp_config_set_shift(&c, FIX_SHIFT - log2s);  // row index x stride
    interp_config_set_mask(&c, log2s, log2s + log2h - 1);
    interp_set_config(interp0, 1, &c);
    interp0->base[0] = (uint32_t)du0;
    interp0->base[1] = (uint32_t)dv0;
    interp0->base[2] = (uintptr_t)tex.pixels;
    interp0->accum[0] = (uint32_t)u0;
    interp0->accum[1] = (uint32_t)v0;
  }
  void setStep(int32_t du0, int32_t dv0) {
    interp0->base[0] = (uint32_t)du0;
    interp0->base[1] = (uint32_t)dv0;
  }
  inline uint32_t fetchNext(uint32_t &a4) {
    const uint32_t p = *(const uint16_t *)(uintptr_t)(interp0->pop[2]);
    if constexpr (T == TexFmt::ARGB4444) {
      a4 = p >> 12;
      return gfx2d::rgb444ToRgb565((uint16_t)(p & 0x0FFFu));
    } else {
      a4 = 15;
      return gfx2d::bswap16((uint16_t)p);
    }
  }
};
#endif

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
  void init(const Span &sp) {
    r = sp.r, g = sp.g, b = sp.b;
    dr = sp.dr, dg = sp.dg, db = sp.db;
  }
  void advance() { r += dr, g += dg, b += db; }
  uint32_t r8() const { return (uint32_t)(r >> FIX_SHIFT) & 0xFFu; }
  uint32_t g8() const { return (uint32_t)(g >> FIX_SHIFT) & 0xFFu; }
  uint32_t b8() const { return (uint32_t)(b >> FIX_SHIFT) & 0xFFu; }
  uint32_t r5() const { return (uint32_t)(r >> 19) & 31u; }
  uint32_t g6() const { return (uint32_t)(g >> 18) & 63u; }
  uint32_t b5() const { return (uint32_t)(b >> 19) & 31u; }
#else
  uint32_t r, g, b;  // 0..255, constant
  void init(const Span &sp) { r = sp.r, g = sp.g, b = sp.b; }
  void advance() {}
  uint32_t r8() const { return r; }
  uint32_t g8() const { return g; }
  uint32_t b8() const { return b; }
  uint32_t r5() const { return r >> 3; }
  uint32_t g6() const { return g >> 2; }
  uint32_t b5() const { return b >> 3; }
#endif
};

// The pixel loop: n pixels of span sp through cursor cur, texels from tx
template <BlendMode B, TexFmt T, bool FLAT, PixelFormat OUT, typename Tex>
static inline void rasterLoop(typename OutTraits<OUT>::Cursor &cur,
                              const Span &sp, int n, Tex &tx) {
  using O = OutTraits<OUT>;
  constexpr bool TEX = (T != TexFmt::NONE);
  constexpr bool TEXA = texFmtHasAlpha(T);

  SpanColor col;
  col.init(sp);
  const uint32_t a64 = sp.tri->alpha64;

  uint32_t sr = col.r5();
  uint32_t sg = col.g6();
  uint32_t sb = col.b5();

#if SHAPOGFX3D_PERSPECTIVE == 2
  // Sub-spans of PERSPECTIVE_STEP pixels: (u, v) are exact at the sub-span
  // ends and interpolated linearly inside. uAcc/vAcc mirror the walker's
  // accumulators at the sub-span start.
#if SHAPOGFX3D_FIXED_POINT
  int32_t uw = sp.uw, vw = sp.vw, iw = sp.iw;
  int32_t uAcc = 0, vAcc = 0, du = 0, dv = 0;
  int left = 0, prevM = 0;
  if constexpr (TEX) {
    constexpr int UV_SHIFT = IW_SHIFT - UW_SHIFT + FIX_SHIFT;
    uAcc = divQ(uw, iw, UV_SHIFT, TEX_MAX_FP);
    vAcc = divQ(vw, iw, UV_SHIFT, TEX_MAX_FP);
  }
#else
  float uw = sp.uw, vw = sp.vw, iw = sp.iw;
  int32_t uAcc = 0, vAcc = 0, du = 0, dv = 0;
  int left = 0, prevM = 0;
  if constexpr (TEX) {
    const float inv = 1.0f / iw;
    uAcc = toFixTex(uw * inv);
    vAcc = toFixTex(vw * inv);
  }
#endif
#endif

  for (int i = 0; i < n; i++) {
    uint32_t a4 = 15;
    if constexpr (TEX) {
#if SHAPOGFX3D_PERSPECTIVE == 2
      if (left == 0) {
        uAcc += du * prevM;
        vAcc += dv * prevM;
        const int m = std::min(n - i, PERSPECTIVE_STEP);
#if SHAPOGFX3D_FIXED_POINT
        uw += sp.duw * m;
        vw += sp.dvw * m;
        iw += sp.diw * m;
        constexpr int UV_SHIFT = IW_SHIFT - UW_SHIFT + FIX_SHIFT;
        const int32_t u1 = divQ(uw, iw, UV_SHIFT, TEX_MAX_FP);
        const int32_t v1 = divQ(vw, iw, UV_SHIFT, TEX_MAX_FP);
#else
        uw += sp.duw * (float)m;
        vw += sp.dvw * (float)m;
        iw += sp.diw * (float)m;
        const float inv = 1.0f / iw;
        const int32_t u1 = toFixTex(uw * inv), v1 = toFixTex(vw * inv);
#endif
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
      // Modulate the texel (5/6/5 bits) by the vertex color (0..255).
      // (c + 1) * t >> 8 preserves the maximum value.
      const uint32_t cr = col.r8() + 1;
      const uint32_t cg = col.g8() + 1;
      const uint32_t cb = col.b8() + 1;
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

    if constexpr (!FLAT) col.advance();
  }
}

// Rasterize n pixels of span sp starting at pixel x of row `line`
template <BlendMode B, TexFmt T, bool FLAT, PixelFormat OUT>
static SHAPOGFX3D_HOT_ATTR void rasterSpanT(uint8_t *line, int x, int n,
                                            const Span &sp) {
  using O = OutTraits<OUT>;
  constexpr bool TEX = (T != TexFmt::NONE);

  typename O::Cursor cur;
  cur.init(line, x);

  // With equal vertex colors and no texture, the color is constant over the
  // span
  if constexpr (B == BlendMode::NONE && FLAT && !TEX) {
    SpanColor col;
    col.init(sp);
    cur.fill(n, O::pack(col.r5(), col.g6(), col.b5()));
    return;
  }

  if constexpr (TEX) {
#if SHAPOGFX3D_TEXTURE
    const Texture &tex = *sp.tri->mat->texture;
#if SHAPOGFX3D_PERSPECTIVE == 2
    const int32_t u0 = 0, v0 = 0, du0 = 0, dv0 = 0;  // set per sub-span
#else
    const int32_t u0 = sp.u, v0 = sp.v, du0 = sp.du, dv0 = sp.dv;
#endif
#if SHAPOGFX3D_RP2_INTERP && SHAPOGFX3D_TEXTURE
    if constexpr (T == TexFmt::RGB565BE || T == TexFmt::ARGB4444) {
      if (InterpTex<T>::usable(tex)) {
        InterpTex<T> tx;
        tx.init(tex, u0, v0, du0, dv0);
        rasterLoop<B, T, FLAT, OUT>(cur, sp, n, tx);
        return;
      }
    }
#endif
    SoftTex<T> tx;
    tx.init(tex, u0, v0, du0, dv0);
    rasterLoop<B, T, FLAT, OUT>(cur, sp, n, tx);
#endif
  } else {
    SoftTex<TexFmt::NONE> tx;
    rasterLoop<B, T, FLAT, OUT>(cur, sp, n, tx);
  }
}

using RasterFn = void (*)(uint8_t *, int, int, const Span &);
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
      uint8_t *, int, int, const Span &);
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
#define SHAPOGFX3D_HOT_TABLE(OUT)                                        \
  SHAPOGFX3D_HOT_ROW(OUT, TexFmt::NONE)                                  \
  SHAPOGFX3D_HOT_ROW_GRAY1(OUT)                                          \
  SHAPOGFX3D_HOT_ROW_RGB444(OUT)                                         \
  SHAPOGFX3D_HOT_ROW_ARGB4444(OUT)                                       \
      SHAPOGFX3D_HOT_ROW_RGB565BE(OUT) template SHAPOGFX3D_HOT_ATTR void \
      fillLineT<OUT>(uint8_t *, int, int, uint32_t);
#if SHAPOGFX_FORMAT_RGB565BE
SHAPOGFX3D_HOT_TABLE(PixelFormat::RGB565BE)
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
#if SHAPOGFX3D_TEXTURE
#define SHAPOGFX3D_RASTER_TABLE(OUT)                                         \
  {                                                                          \
    SHAPOGFX3D_RASTER_ROW(OUT, TexFmt::NONE),                                \
        SHAPOGFX3D_RASTER_ROW_GRAY1(OUT), SHAPOGFX3D_RASTER_ROW_RGB444(OUT), \
        SHAPOGFX3D_RASTER_ROW_ARGB4444(OUT),                                 \
        SHAPOGFX3D_RASTER_ROW_RGB565BE(OUT),                                 \
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
#if SHAPOGFX_FORMAT_RGB444
static const RasterFn RASTER_FNS_RGB444[RASTER_TABLE_SIZE] =
    SHAPOGFX3D_RASTER_TABLE(PixelFormat::RGB444);
#endif

// Merge two ascending lists (links are stored in the entries)
uint16_t Graphics3D::mergeLists(uint16_t a, uint16_t b) {
  TriEntry *const ent = entries_;
  uint16_t head = NONE;
  uint16_t *pp = &head;
  while (a != NONE && b != NONE) {
    if (a < b) {
      *pp = a;
      pp = &ent[a].link;
      a = ent[a].link;
    } else {
      *pp = b;
      pp = &ent[b].link;
      b = ent[b].link;
    }
  }
  *pp = (a != NONE) ? a : b;
  return head;
}

SHAPOGFX3D_HOT_ATTR void Graphics3D::render(int16_t x, int16_t y, int16_t w,
                                            int16_t h, const Surface &dst,
                                            int16_t dstX, int16_t dstY) {
  if (!recBase_ || !spanPool_ || !dst.pixels) return;

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
#if SHAPOGFX_FORMAT_RGB444
    case PixelFormat::RGB444:
      table = RASTER_FNS_RGB444;
      fillFn = fillLineT<PixelFormat::RGB444>;
      break;
#endif
    default: return;
  }
  const uint32_t clearNative =
      gfx2d::colorToNative(dst.format, gfx2d::makeColorF(clearColor_));

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
  TriEntry *const ent = entries_;
  const uint8_t *const base = recBase_;

#if SHAPOGFX3D_RP2_INTERP
  interp_hw_save_t interpSave;
  interp_save(interp0, &interpSave);
#endif

  // For each scanline of the region, build the list of triangles that start
  // intersecting at that line (linked by entry position, i.e. by depth)
  for (int i = y0; i < y1; i++) bucketHead_[i] = bucketTail_[i] = NONE;
  for (int p = 0; p < triCount_; p++) {
    const TriHead &t = *recOf(base, ent[p]);
    if (t.yMax < y0 || t.yMin >= y1) continue;
    int line = std::max((int)t.yMin, y0);
    ent[p].link = NONE;
    if (bucketTail_[line] != NONE) {
      ent[bucketTail_[line]].link = (uint16_t)p;
    } else {
      bucketHead_[line] = (uint16_t)p;
    }
    bucketTail_[line] = (uint16_t)p;
  }

  uint16_t active = NONE;  // triangles intersecting the current line (by depth)
  for (int yi = y0; yi < y1; yi++) {
    active = mergeLists(active, bucketHead_[yi]);

    // Clear the span lists (per scanline)
    spanCount_ = 0;
    opaqueHead_ = nullptr;
    transHead_ = transTail_ = nullptr;

    uint16_t *pp = &active;
    while (*pp != NONE) {
      const uint16_t p = *pp;
      const TriHead &t = *recOf(base, ent[p]);
      if (yi > t.yMax) {
        *pp = ent[p].link;  // passed: remove from the active list
        continue;
      }
      Span sp;
      if (makeSpan(t, yi, rx0, rx1, sp)) {
#if SHAPOGFX3D_BLEND
        if (t.flags & TriFlags::OPAQUE) {
          clipTranslucent(sp);
          insertOpaque(sp);
        } else {
          insertTranslucent(sp);
        }
#else
        insertOpaque(sp);  // every primitive is opaque
#endif
      }
      pp = &ent[p].link;
    }
    if (spanCount_ > spanPeak_) spanPeak_ = spanCount_;

    // Draw the opaque spans (ascending x) and the background in the gaps,
    // then composite the translucent spans on top
    uint8_t *line = dst.linePtr(dy + (yi - ry));
    const int xBase = dx - rx0;  // screen x -> dst x
    int cursor = rx0;
    for (const Span *e = opaqueHead_; e; e = e->next) {
      if (clearEnabled_ && e->x0 > cursor) {
        fillFn(line, xBase + cursor, e->x0 - cursor, clearNative);
      }
      table[e->tri->rasterFn](line, xBase + e->x0, e->x1 - e->x0, *e);
      cursor = e->x1;
    }
    if (clearEnabled_ && cursor < rx1) {
      fillFn(line, xBase + cursor, rx1 - cursor, clearNative);
    }
#if SHAPOGFX3D_BLEND
    for (const Span *e = transHead_; e; e = e->next) {
      table[e->tri->rasterFn](line, xBase + e->x0, e->x1 - e->x0, *e);
    }
#endif
  }

#if SHAPOGFX3D_RP2_INTERP
  interp_restore(interp0, &interpSave);
#endif
}

// ---------------------------------------------------------------------------
// Statistics

Stats Graphics3D::getStats() const {
  Stats st;
  st.arenaSize = arenaSize_;
  st.triBytes =
      (size_t)(recEnd_ - recTop_) + (size_t)triCount_ * sizeof(TriEntry);
  st.triBytesTotal = (size_t)(recEnd_ - recBase_);
  st.arenaUsed = arenaFixed_ + st.triBytes + (size_t)spanPeak_ * sizeof(Span);
  st.triCount = triCount_;
  st.triDropped = triDropped_;
  st.layerCount = layerCount_;
  st.layersDropped = layersDropped_;
  st.spanCapacity = spanCapacity_;
  st.spanPeak = spanPeak_;
  st.spanDropped = spanDropped_;
  st.badIndices = badIndices_;
  st.nodesDropped = nodesDropped_;
  return st;
}

}  // namespace shapoco::gfx3d
