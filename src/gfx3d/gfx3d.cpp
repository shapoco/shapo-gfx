#include "shapoco/gfx3d/gfx3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

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

// Fixed part of the arena: the matrix stack of pushState() and the
// direct-mapped cache of transformed vertices (a power of two; a smaller
// cache costs re-transformed vertices, never correctness).
#ifndef SHAPOGFX3D_STACK_DEPTH
#define SHAPOGFX3D_STACK_DEPTH 16
#endif
#ifndef SHAPOGFX3D_VCACHE_SIZE
#define SHAPOGFX3D_VCACHE_SIZE 64
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

// RP2040 / RP2350 (Pico SDK): fetch 16-bit texels through the SIO
// interpolator (interp0 of the core that calls render()). Opt-in.
#ifndef SHAPOGFX3D_RP2_INTERP
#define SHAPOGFX3D_RP2_INTERP 0
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
}  // namespace TriFlags

// A triangle, line or point after setup.
struct Triangle {
  // Triangles: the vertices sorted by sy; [0] = top, [1] = middle (the bottom
  // vertex is only needed through the edge slopes). Lines: [0] = a, [1] = b.
  // Points: [0] = top-left pixel.
  float sx[2], sy[2];
  // Triangles: dx/dy of the edges top->middle, middle->bottom, top->bottom
  // (0 for horizontal edges). Lines: [0] = 1/dx, [1] = 1/dy (0 when the
  // difference is 0). Points: [0] = size in pixels.
  float slope[3];
  Plane z;  // NDC depth
#if SHAPOGFX3D_GOURAUD
  Plane r, g, b;  // vertex color 0..255
#else
  uint8_t r, g, b;  // flat shading: one color for the whole primitive (0..255)
#endif
#if SHAPOGFX3D_TEXTURE
#if SHAPOGFX3D_PERSPECTIVE >= 1
  Plane uw, vw, iw;  // (u/w, v/w, 1/w)
#else
  Plane u, v;  // texels
#endif
#endif
  const Material *mat;
  int32_t sortKey;     // ascending = farther first
  int16_t yMin, yMax;  // range of scanlines crossed (inclusive)
  uint8_t flags;       // TriFlags
  uint8_t alpha64;     // opacity (0..64)
  uint8_t rasterFn;    // index of the rasterizer (tex * 6 + blend * 2 + flat)
  bool leftLong;       // triangles: the long edge (top->bottom) is on the left
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
#if SHAPOGFX3D_TEXTURE
#if SHAPOGFX3D_PERSPECTIVE == 2
  float uw, vw, iw;  // (u/w, v/w, 1/w)
  float duw, dvw, diw;
#else
  int32_t u, v;  // texels, 16.16 fixed point
  int32_t du, dv;
#endif
#endif
  const Triangle *tri;
  Span *next;
};

struct StackEntry {
  mat4f matrix;
  const Material *material;
};

// Direct-mapped cache of transformed vertices, reused within putPrimitive()
struct CachedVertex {
  ShadedVertex sv;
  float invW;
  float viewZ;
  uint16_t tag;  // vertex index (NONE = empty)
  bool ok;  // false: in front of the near plane (the whole triangle is dropped)
};

// Vertex of a point or line: view-space position and unlit color (0..255)
struct UnlitVertex {
  vec3f view;
  float r, g, b;
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
};

static constexpr int STACK_DEPTH = SHAPOGFX3D_STACK_DEPTH;
static constexpr int SPAN_CAPACITY_MIN = 32;
static constexpr int SPAN_CAPACITY_MAX = 512;
static constexpr int VCACHE_SIZE = SHAPOGFX3D_VCACHE_SIZE;
static constexpr uint16_t NONE = 0xFFFF;

static_assert(STACK_DEPTH >= 1, "SHAPOGFX3D_STACK_DEPTH must be at least 1");
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

void Graphics3D::init(int16_t w, int16_t h, void *arena, size_t arenaSize) {
  *this = Graphics3D();
  screenW_ = w;
  screenH_ = h;
  arenaSize_ = arenaSize;

  uintptr_t p = (uintptr_t)arena;
  const uintptr_t end = p + arenaSize;
  p = alignUp8(p);
  auto avail = [&]() -> size_t { return (end > p) ? (size_t)(end - p) : 0; };

  // Fixed allocations: line buckets (screenH x 2), matrix stack, vertex cache
  const size_t bucketBytes = alignUp8((size_t)h * 2 * sizeof(uint16_t));
  const size_t stackBytes = alignUp8((size_t)STACK_DEPTH * sizeof(StackEntry));
  const size_t vcacheBytes =
      alignUp8((size_t)VCACHE_SIZE * sizeof(CachedVertex));
  if (avail() < bucketBytes + stackBytes + vcacheBytes) {
    *this = Graphics3D();
    return;
  }
  bucketHead_ = (uint16_t *)p;
  bucketTail_ = bucketHead_ + h;
  p += bucketBytes;
  stack_ = (StackEntry *)p;
  p += stackBytes;
  vcache_ = (CachedVertex *)p;
  p += vcacheBytes;
  arenaFixed_ = (size_t)(p - (uintptr_t)arena);

  // Span pool: a quarter of the remaining space
  int spanCap = (int)((avail() / 4) / sizeof(Span));
  spanCap = std::max(SPAN_CAPACITY_MIN, std::min(SPAN_CAPACITY_MAX, spanCap));
  spanPool_ = (Span *)p;
  spanCapacity_ = spanCap;
  p += (size_t)spanCap * sizeof(Span);
  p = alignUp8(p);

  // Triangle buffer + order/link (4 bytes per triangle): everything that
  // remains
  int triCap = (int)(avail() / (sizeof(Triangle) + 2 * sizeof(uint16_t)));
  triCap = std::min(triCap, (int)NONE - 1);
  order_ = (uint16_t *)p;
  link_ = order_ + triCap;
  p += (size_t)triCap * 2 * sizeof(uint16_t);
  p = alignUp8(p);
  triCapacity_ = std::min(triCap, (int)(avail() / sizeof(Triangle)));
  tris_ = (Triangle *)p;
  if (triCapacity_ <= 0 || spanCap <= 0) {
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
  cur_ = mat4f::identity();
}

void Graphics3D::endScene() {}

void Graphics3D::loadIdentity() { cur_ = mat4f::identity(); }

void Graphics3D::translate(const vec3f &v) {
  cur_ = cur_ * mat4f::translation(v.x, v.y, v.z);
}
void Graphics3D::translate(float x, float y, float z) {
  cur_ = cur_ * mat4f::translation(x, y, z);
}
void Graphics3D::rotate(float angle, const vec3f &axis) {
  cur_ = cur_ * mat4f::rotation(angle, axis);
}
void Graphics3D::rotate(float angle, float x, float y, float z) {
  cur_ = cur_ * mat4f::rotation(angle, {x, y, z});
}
void Graphics3D::scale(const vec3f &v) {
  cur_ = cur_ * mat4f::scaling(v.x, v.y, v.z);
}
void Graphics3D::scale(float x, float y, float z) {
  cur_ = cur_ * mat4f::scaling(x, y, z);
}
void Graphics3D::transform(const mat4f &m) { cur_ = cur_ * m; }
void Graphics3D::lookAt(const vec3f &eye, const vec3f &target,
                        const vec3f &up) {
  cur_ = cur_ * mat4f::lookAt(eye, target, up);
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
  curMat_ = stack_[stackTop_].material;
}

void Graphics3D::setMaterial(const Material &mat) { curMat_ = &mat; }

void Graphics3D::enableParallelLight(const vec3f &dir, const colorf &col) {
  lightEnabled_ = true;
  lightDir_ = normalize(cur_.transformDir(dir));
  lightCol_ = col;
}

void Graphics3D::disableParallelLight() { lightEnabled_ = false; }

void Graphics3D::enableEnvironmentLight(const colorf &col) {
  envEnabled_ = true;
  envCol_ = col;
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
}

void Graphics3D::setOrthographicProjection(float left, float right,
                                           float bottom, float top, float zNear,
                                           float zFar) {
  proj_ = mat4f::orthographic(left, right, bottom, top, zNear, zFar);
  projKind_ = ProjKind::ORTHOGRAPHIC;
  zNear_ = zNear;
}

// ---------------------------------------------------------------------------
// Vertex processing (transform + lighting + projection)

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

  if (triCount_ >= triCapacity_) {  // buffer overflow: drop for this frame
    triDropped_++;
    return;
  }
  Triangle &t = tris_[triCount_];

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

  t.sx[0] = x0;
  t.sy[0] = y0;
  t.sx[1] = v1.sx;
  t.sy[1] = v1.sy;
  const float y21 = v2.sy - v1.sy;
  t.slope[0] = (y10 != 0.0f) ? x10 / y10 : 0.0f;
  t.slope[1] = (y21 != 0.0f) ? (v2.sx - v1.sx) / y21 : 0.0f;
  t.slope[2] = x20 / y20;  // y20 > 0, otherwise area2 == 0
  // The long edge is on the left when the middle vertex lies to its right
  t.leftLong = (x0 + t.slope[2] * y10) < v1.sx;

  t.z = plane(v0.zNdc + depthBias_, v1.zNdc + depthBias_, v2.zNdc + depthBias_);

  uint8_t flags = 0;
#if SHAPOGFX3D_GOURAUD
  t.r = plane(v0.r, v1.r, v2.r);
  t.g = plane(v0.g, v1.g, v2.g);
  t.b = plane(v0.b, v1.b, v2.b);
  if (v0.r == v1.r && v0.r == v2.r && v0.g == v1.g && v0.g == v2.g &&
      v0.b == v1.b && v0.b == v2.b) {
    flags |= TriFlags::FLAT;
  }
#else
  // Flat shading: the first vertex as passed in (before the y sort) provides
  // the color of the whole triangle
  t.r = (uint8_t)(int)(a.sv.r + 0.5f);
  t.g = (uint8_t)(int)(a.sv.g + 0.5f);
  t.b = (uint8_t)(int)(a.sv.b + 0.5f);
  flags |= TriFlags::FLAT;
#endif

  const TexFmt tf = texFmtOf(tex);
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

#if SHAPOGFX3D_TEXTURE
  if (tex) {
    flags |= TriFlags::TEX;
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
  }
#endif  // SHAPOGFX3D_TEXTURE

  t.mat = mat;
  t.sortKey = floatSortKey(a.viewZ + b.viewZ + c.viewZ);
  t.yMin = (int16_t)yMin;
  t.yMax = (int16_t)yMax;
  t.flags = flags;
  t.alpha64 = materialAlpha64(mat);
  t.rasterFn = (uint8_t)((int)tf * RASTER_PER_TEX + blend * 2 +
                         ((flags & TriFlags::FLAT) ? 1 : 0));
  triCount_++;
}

// ---------------------------------------------------------------------------
// Points and lines
//
// They are unlit (diffuse x vertex color), never culled and clipped against
// the near plane. Both are stored in the triangle buffer and become spans in
// makeSpan(), so they are depth-resolved against everything else.

#if SHAPOGFX3D_UNLIT

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

static inline Plane constPlane(float v) { return {v, 0.0f, 0.0f}; }

// Texture planes of untextured primitives
static inline void clearTexPlanes(Triangle &t) {
#if !SHAPOGFX3D_TEXTURE
  (void)t;
#elif SHAPOGFX3D_PERSPECTIVE >= 1
  t.uw = t.vw = constPlane(0.0f);
  t.iw = constPlane(1.0f);
#else
  t.u = t.v = constPlane(0.0f);
#endif
}

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

  if (triCount_ >= triCapacity_) {
    triDropped_++;
    return;
  }
  Triangle &t = tris_[triCount_];
  t.sx[0] = ax;
  t.sy[0] = ay;
  t.sx[1] = bx;
  t.sy[1] = by;
  const float dx = bx - ax, dy = by - ay;
  t.slope[0] = (dx != 0.0f) ? 1.0f / dx : 0.0f;  // 1/dx
  t.slope[1] = (dy != 0.0f) ? 1.0f / dy : 0.0f;  // 1/dy
  t.slope[2] = 0.0f;
  // Attributes vary along the major axis: per row for steep lines, per
  // column for shallow ones (see makeLineSpan)
  const bool steep = std::fabs(dy) >= std::fabs(dx);
  auto linePlane = [&](float p, float q) -> Plane {
    if (steep) {
      const float k = (q - p) * t.slope[1];
      return {p - k * ay, 0.0f, k};
    }
    const float k = (q - p) * t.slope[0];
    return {p - k * ax, k, 0.0f};
  };
  t.z = linePlane(az, bz);
#if SHAPOGFX3D_GOURAUD
  t.r = linePlane(a.r, b.r);
  t.g = linePlane(a.g, b.g);
  t.b = linePlane(a.b, b.b);
  const bool flat = (a.r == b.r && a.g == b.g && a.b == b.b);
#else
  t.r = (uint8_t)(int)(a.r + 0.5f);  // flat shading: the first end point
  t.g = (uint8_t)(int)(a.g + 0.5f);
  t.b = (uint8_t)(int)(a.b + 0.5f);
  const bool flat = true;
#endif
  clearTexPlanes(t);

  uint8_t flags = TriFlags::LINE;
  if (flat) flags |= TriFlags::FLAT;
  flags |= opaqueFlag(mat);
  t.mat = mat;
  t.sortKey = floatSortKey(a.view.z + b.view.z);
  t.yMin = (int16_t)yMin;
  t.yMax = (int16_t)yMax;
  t.flags = flags;
  t.alpha64 = materialAlpha64(mat);
  t.rasterFn = unlitRasterFn(mat, flat);
  t.leftLong = false;
  triCount_++;
}

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

  if (triCount_ >= triCapacity_) {
    triDropped_++;
    return;
  }
  Triangle &t = tris_[triCount_];
  t.sx[0] = t.sx[1] = (float)x0;
  t.sy[0] = t.sy[1] = (float)y0;
  t.slope[0] = (float)size;
  t.slope[1] = t.slope[2] = 0.0f;
  t.z = constPlane(z);
#if SHAPOGFX3D_GOURAUD
  t.r = constPlane(a.r);
  t.g = constPlane(a.g);
  t.b = constPlane(a.b);
#else
  t.r = (uint8_t)(int)(a.r + 0.5f);
  t.g = (uint8_t)(int)(a.g + 0.5f);
  t.b = (uint8_t)(int)(a.b + 0.5f);
#endif
  clearTexPlanes(t);
  uint8_t flags = TriFlags::POINT | TriFlags::FLAT;
  flags |= opaqueFlag(mat);
  t.mat = mat;
  t.sortKey = floatSortKey(a.view.z);
  t.yMin = (int16_t)yMin;
  t.yMax = (int16_t)yMax;
  t.flags = flags;
  t.alpha64 = materialAlpha64(mat);
  t.rasterFn = unlitRasterFn(mat, true);
  t.leftLong = false;
  triCount_++;
}

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
  if (!tris_) return;
  const Material *mat = prim.material ? prim.material : curMat_;
  if (!mat) return;
  if (!prim.vertexBuffer || !prim.indices) return;
  const VertexBuffer &vb = *prim.vertexBuffer;
  if (!vb.vertices && !vb.packed) return;
  const uint16_t *idx = prim.indices;
  int n = prim.indexCount;

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

void Graphics3D::beginRender() {
  // Sort farthest first (ascending view-space z: more negative comes first).
  // Depth order between opaque spans is resolved by the depth test at span
  // insertion, so this sort mainly determines the compositing order of
  // translucent triangles. Only the index array is permuted.
  spanPeak_ = 0;
  spanDropped_ = 0;
  if (!tris_) return;
  for (int i = 0; i < triCount_; i++) order_[i] = (uint16_t)i;
  const Triangle *tris = tris_;
  std::sort(order_, order_ + triCount_, [tris](uint16_t a, uint16_t b) {
    return tris[a].sortKey < tris[b].sortKey;
  });
}

void Graphics3D::endRender() {}

Span *Graphics3D::allocSpan() {
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

// Is frag nearer than e at the center of the overlap [ox0, ox1)? Depths are
// compared doubled so that the half-pixel center stays integer.
static inline bool fragNearer(const Span &frag, const Span &e, int ox0,
                              int ox1) {
  const int k = ox0 + ox1 - 1;  // 2 * center
  const int64_t zf =
      2 * (int64_t)frag.z0 + (int64_t)frag.dz * (k - 2 * frag.x0);
  const int64_t ze = 2 * (int64_t)e.z0 + (int64_t)e.dz * (k - 2 * e.x0);
  return zf < ze;
}

// Remove the range [ox0, ox1) from list element e = *pp.
// Returns the position at which to continue scanning (after the remaining part,
// or the rest of e).
Span **Graphics3D::cutSpan(Span **pp, int ox0, int ox1) {
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
void Graphics3D::appendTranslucent(const Span &sp, int x1) {
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
void Graphics3D::insertOpaque(Span &frag) {
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
void Graphics3D::insertTranslucent(Span &frag) {
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
void Graphics3D::clipTranslucent(const Span &frag) {
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

// Fill the depth and color of a span covering [x0, x1) from the planes of t,
// evaluated at the center of the leftmost pixel (xc, yc)
static inline void fillSpanBase(const Triangle &t, float xc, float yc, int x0,
                                int x1, Span &out) {
  out.x0 = x0;
  out.x1 = x1;
  out.z0 = toZ(t.z.at(xc, yc));
  out.dz = toZDelta(t.z.dx);
#if SHAPOGFX3D_GOURAUD
  // Color: the plane values at pixel centers inside the triangle lie within
  // 0..255 up to rounding; clamp the start value to guard the rounding.
  out.r = toFixColor(t.r.at(xc, yc));
  out.g = toFixColor(t.g.at(xc, yc));
  out.b = toFixColor(t.b.at(xc, yc));
  out.dr = toFixDelta(t.r.dx);
  out.dg = toFixDelta(t.g.dx);
  out.db = toFixDelta(t.b.dx);
#else
  out.r = t.r;
  out.g = t.g;
  out.b = t.b;
#endif
  out.tri = &t;
  out.next = nullptr;
}

// Span of an untextured primitive (points and lines)
#if SHAPOGFX3D_UNLIT
static inline void fillUnlitSpan(const Triangle &t, float xc, float yc, int x0,
                                 int x1, Span &out) {
  fillSpanBase(t, xc, yc, x0, x1, out);
#if SHAPOGFX3D_TEXTURE
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

#if SHAPOGFX3D_LINES
// Line segment a -> b on pixel row yi: one pixel per row for steep lines, one
// pixel per column (a horizontal run) for shallow lines, so the coverage
// matches a Bresenham line. Both end points are drawn.
static bool makeLineSpan(const Triangle &t, int yi, int rx0, int rx1,
                         Span &out) {
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
#endif  // SHAPOGFX3D_LINES

#if SHAPOGFX3D_POINTS
// Point: a square with its top-left pixel at (sx[0], sy[0]), size slope[0]
static bool makePointSpan(const Triangle &t, int yi, int rx0, int rx1,
                          Span &out) {
  const int size = (int)t.slope[0];
  int c0 = std::max((int)t.sx[0], rx0);
  int c1 = std::min((int)t.sx[0] + size, rx1);
  if (c0 >= c1) return false;
  fillUnlitSpan(t, (float)c0 + 0.5f, (float)yi + 0.5f, c0, c1, out);
  return true;
}
#endif  // SHAPOGFX3D_POINTS

// Build the span of triangle t on the scanline with center yc, limited to
// the region [rx0, rx1). Returns false when the triangle covers no pixel
// center there.
static bool makeTriSpan(const Triangle &t, float yc, int rx0, int rx1,
                        Span &out) {
  // Edge x at yc: the long edge, and the short edge covering this row (the
  // top->middle edge covers [sy0, sy1), middle->bottom covers [sy1, sy2))
  const float xLong = t.sx[0] + t.slope[2] * (yc - t.sy[0]);
  const float xShort = (yc < t.sy[1]) ? t.sx[0] + t.slope[0] * (yc - t.sy[0])
                                      : t.sx[1] + t.slope[1] * (yc - t.sy[1]);
  const float xl = t.leftLong ? xLong : xShort;
  const float xr = t.leftLong ? xShort : xLong;
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

#if SHAPOGFX3D_TEXTURE
#if SHAPOGFX3D_PERSPECTIVE == 2
  out.uw = t.uw.at(xc, yc);
  out.vw = t.vw.at(xc, yc);
  out.iw = t.iw.at(xc, yc);
  out.duw = t.uw.dx;
  out.dvw = t.vw.dx;
  out.diw = t.iw.dx;
#elif SHAPOGFX3D_PERSPECTIVE == 1
  if (t.flags & TriFlags::TEX) {
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
  } else {
    out.u = out.v = out.du = out.dv = 0;
  }
#else
  out.u = toFixTex(t.u.at(xc, yc));
  out.v = toFixTex(t.v.at(xc, yc));
  out.du = toFixDelta(t.u.dx);
  out.dv = toFixDelta(t.v.dx);
#endif
#endif  // SHAPOGFX3D_TEXTURE
  return true;
}

static inline bool makeSpan(const Triangle &t, int yi, int rx0, int rx1,
                            Span &out) {
#if SHAPOGFX3D_LINES
  if (t.flags & TriFlags::LINE) return makeLineSpan(t, yi, rx0, rx1, out);
#endif
#if SHAPOGFX3D_POINTS
  if (t.flags & TriFlags::POINT) return makePointSpan(t, yi, rx0, rx1, out);
#endif
  return makeTriSpan(t, (float)yi + 0.5f, rx0, rx1, out);
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
  float uw = sp.uw, vw = sp.vw, iw = sp.iw;
  int32_t uAcc = 0, vAcc = 0, du = 0, dv = 0;
  int left = 0, prevM = 0;
  if constexpr (TEX) {
    const float inv = 1.0f / iw;
    uAcc = toFixTex(uw * inv);
    vAcc = toFixTex(vw * inv);
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
        uw += sp.duw * (float)m;
        vw += sp.dvw * (float)m;
        iw += sp.diw * (float)m;
        const float inv = 1.0f / iw;
        const int32_t u1 = toFixTex(uw * inv), v1 = toFixTex(vw * inv);
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
static void rasterSpanT(uint8_t *line, int x, int n, const Span &sp) {
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
static void fillLineT(uint8_t *line, int x, int n, uint32_t native) {
  typename OutTraits<OUT>::Cursor cur;
  cur.init(line, x);
  cur.fill(n, native);
}

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

// Merge two ascending lists (links are stored in link_)
uint16_t Graphics3D::mergeLists(uint16_t a, uint16_t b) {
  uint16_t *link = link_;
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

void Graphics3D::render(int16_t x, int16_t y, int16_t w, int16_t h,
                        const Surface &dst, int16_t dstX, int16_t dstY) {
  if (!tris_ || !spanPool_ || !dst.pixels) return;

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
  uint16_t *const link = link_;

#if SHAPOGFX3D_RP2_INTERP
  interp_hw_save_t interpSave;
  interp_save(interp0, &interpSave);
#endif

  // For each scanline of the region, build the list of triangles that start
  // intersecting at that line (linked by position in order_, i.e. by depth)
  for (int i = y0; i < y1; i++) bucketHead_[i] = bucketTail_[i] = NONE;
  for (int p = 0; p < triCount_; p++) {
    const Triangle &t = tris_[order_[p]];
    if (t.yMax < y0 || t.yMin >= y1) continue;
    int line = std::max((int)t.yMin, y0);
    link[p] = NONE;
    if (bucketTail_[line] != NONE) {
      link[bucketTail_[line]] = (uint16_t)p;
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
      const Triangle &t = tris_[order_[p]];
      if (yi > t.yMax) {
        *pp = link[p];  // passed: remove from the active list
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
      pp = &link[p];
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
  st.arenaUsed = arenaFixed_ +
                 (size_t)triCount_ * (sizeof(Triangle) + 2 * sizeof(uint16_t)) +
                 (size_t)spanPeak_ * sizeof(Span);
  st.triCapacity = triCapacity_;
  st.triCount = triCount_;
  st.triDropped = triDropped_;
  st.spanCapacity = spanCapacity_;
  st.spanPeak = spanPeak_;
  st.spanDropped = spanDropped_;
  st.badIndices = badIndices_;
  st.nodesDropped = nodesDropped_;
  return st;
}

}  // namespace shapoco::gfx3d
