#include "shapoco/gfx3d/gfx3d.hpp"

#include <algorithm>
#include <cmath>

// Perspective correction of texture coordinates:
//   0: none (affine interpolation everywhere)
//   1: vertical only (default). (u/w, v/w, 1/w) are interpolated along the
//      triangle edges and divided at the two end points of each span, so the
//      end points are exact and the span interior is affine. Costs two divides
//      per span. Exact for horizontal surfaces seen by a camera without roll.
//   2: full. (u/w, v/w, 1/w) are interpolated across the span and divided per
//      pixel.
#ifndef SHAPOGFX3D_CORRECT_PERSPECTIVE
#define SHAPOGFX3D_CORRECT_PERSPECTIVE 1
#endif

namespace shapoco::gfx3d {

// ---------------------------------------------------------------------------
// Internal data structures
//
// Vertex processing (transform, lighting, projection) and the computation of
// span end points are done in float. All per-pixel work uses integer arithmetic
// (fixed point with 16 fractional bits).

namespace detail {

static constexpr int FIX_SHIFT = 16;
static constexpr float FIX_ONE = 65536.0f;

// A vertex after lighting and projection
struct ShadedVertex {
  float sx, sy;  // screen coordinates
  float zNdc;    // NDC depth (linear in screen space; smaller = nearer)
  float u,
      v;  // texture coordinates in texels (u/w, v/w when perspective-correct)
  float r, g, b;  // vertex color 0..255 (pre-multiplied by opacity for additive
                  // blending)
};

namespace TriFlags {
constexpr uint8_t FLAT =
    1u << 0;  // all three vertex colors are equal (no color interpolation)
constexpr uint8_t TEX = 1u << 1;     // samples a texture
constexpr uint8_t OPAQUE = 1u << 2;  // opaque (BlendMode::NONE)
}  // namespace TriFlags

struct Triangle {
  ShadedVertex v[3];
#if SHAPOGFX3D_CORRECT_PERSPECTIVE >= 1
  float invW[3];  // 1/w
#endif
  float invDy[3];  // 1/(delta sy) of edge i (v[i] -> v[i+1]); 0 for horizontal
                   // edges
  const Material *mat;
  float depth;         // average view-space z (smaller = farther)
  int16_t yMin, yMax;  // range of scanlines crossed (inclusive)
  uint8_t flags;       // TriFlags
  uint8_t alpha64;     // opacity (0..64)
  uint8_t rasterFn;    // index of the rasterizer (blend * 4 + tex * 2 + flat)
};

// A span on a scanline: attribute values at the leftmost pixel and per-pixel
// increments
struct Span {
  int32_t x0, x1;   // pixel range [x0, x1)
  float z0, dz;     // NDC depth (used only to resolve overlaps)
  int32_t r, g, b;  // 8.16 fixed point (0..255)
  int32_t dr, dg, db;
#if SHAPOGFX3D_CORRECT_PERSPECTIVE == 2
  float uw, vw, iw;  // (u/w, v/w, 1/w)
  float duw, dvw, diw;
#else
  int32_t u, v;  // texels, 16.16 fixed point
  int32_t du, dv;
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

static constexpr int STACK_DEPTH = 16;
static constexpr int SPAN_CAPACITY_MIN = 32;
static constexpr int SPAN_CAPACITY_MAX = 512;
static constexpr int VCACHE_SIZE = 64;  // power of two
static constexpr uint16_t NONE = 0xFFFF;

}  // namespace detail

using namespace detail;

// ---------------------------------------------------------------------------
// Utilities

static inline uintptr_t alignUp8(uintptr_t p) {
  return (p + 7u) & ~(uintptr_t)7u;
}

static inline const Texture *materialTexture(const Material *mat) {
  return (mat->flags & (MaterialFlags::TEXTURE | MaterialFlags::ENV_MAP))
             ? mat->texture
             : nullptr;
}

// ---------------------------------------------------------------------------
// Initialization

void Renderer::init(int16_t w, int16_t h, void *arena, size_t arenaSize) {
  *this = Renderer();
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
    *this = Renderer();
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
    *this = Renderer();
    return;
  }
}

void Renderer::deinit() { *this = Renderer(); }

// ---------------------------------------------------------------------------
// Scene construction

void Renderer::beginScene() {
  triCount_ = 0;
  triDropped_ = 0;
  stackTop_ = 0;
  cur_ = mat4f::identity();
}

void Renderer::endScene() {}

void Renderer::loadIdentity() { cur_ = mat4f::identity(); }

void Renderer::translate(const vec3f &v) {
  cur_ = cur_ * mat4f::translation(v.x, v.y, v.z);
}
void Renderer::translate(float x, float y, float z) {
  cur_ = cur_ * mat4f::translation(x, y, z);
}
void Renderer::rotate(float angle, const vec3f &axis) {
  cur_ = cur_ * mat4f::rotation(angle, axis);
}
void Renderer::rotate(float angle, float x, float y, float z) {
  cur_ = cur_ * mat4f::rotation(angle, {x, y, z});
}
void Renderer::scale(const vec3f &v) {
  cur_ = cur_ * mat4f::scaling(v.x, v.y, v.z);
}
void Renderer::scale(float x, float y, float z) {
  cur_ = cur_ * mat4f::scaling(x, y, z);
}

void Renderer::pushState() {
  if (!stack_ || stackTop_ >= STACK_DEPTH) return;
  stack_[stackTop_].matrix = cur_;
  stack_[stackTop_].material = curMat_;
  stackTop_++;
}

void Renderer::popState() {
  if (!stack_ || stackTop_ <= 0) return;
  stackTop_--;
  cur_ = stack_[stackTop_].matrix;
  curMat_ = stack_[stackTop_].material;
}

void Renderer::setMaterial(const Material &mat) { curMat_ = &mat; }

void Renderer::enableParallelLight(const vec3f &dir, const colorf &col) {
  lightEnabled_ = true;
  lightDir_ = normalize(cur_.transformDir(dir));
  lightCol_ = col;
}

void Renderer::disableParallelLight() { lightEnabled_ = false; }

void Renderer::enableEnvironmentLight(const colorf &col) {
  envEnabled_ = true;
  envCol_ = col;
}

void Renderer::disableEnvironmentLight() { envEnabled_ = false; }

void Renderer::setClearColor(const colorf &col) { clearColor_ = col; }

void Renderer::setPerspectiveProjection(float fovY, float aspect, float zNear,
                                        float zFar) {
  proj_ = mat4f::perspective(fovY, aspect, zNear, zFar);
  zNear_ = zNear;
}

void Renderer::setOrthographicProjection(float left, float right, float bottom,
                                         float top, float zNear, float zFar) {
  proj_ = mat4f::orthographic(left, right, bottom, top, zNear, zFar);
  zNear_ = zNear;
}

// ---------------------------------------------------------------------------
// Vertex processing (transform + lighting + projection)

void Renderer::shadeVertex(const Vertex &in, const Material *mat,
                           const Texture *tex, CachedVertex &out) const {
  out.ok = false;
  vec3f viewPos = cur_.transformPoint(in.position);
  // Triangles crossing or in front of the near plane are dropped
  if (viewPos.z > -zNear_) return;
  float w;
  vec3f clip = proj_.transformPoint4(viewPos, w);
  if (w <= 0.0f) return;
  float invW = 1.0f / w;

  ShadedVertex &sv = out.sv;
  sv.sx = (clip.x * invW * 0.5f + 0.5f) * screenW_;
  sv.sy = (0.5f - clip.y * invW * 0.5f) * screenH_;
  sv.zNdc = clip.z * invW;

  vec3f n = normalize(cur_.transformDir(in.normal));

  if (tex) {
    vec2f uv;
    if (mat->flags & MaterialFlags::ENV_MAP) {
      // Environment map UV from the view-space normal
      uv = {n.x * 0.5f + 0.5f, 0.5f - n.y * 0.5f};
    } else {
      uv = in.uv;
    }
    sv.u = uv.x * tex->width;
    sv.v = uv.y * tex->height;
  } else {
    sv.u = sv.v = 0.0f;
  }

  // Gouraud shading: lighting is evaluated per vertex
  float r = 0, g = 0, b = 0;
  bool lit = false;
  if (envEnabled_) {
    r += mat->ambient.r * envCol_.r;
    g += mat->ambient.g * envCol_.g;
    b += mat->ambient.b * envCol_.b;
    lit = true;
  }
  if (lightEnabled_) {
    float d = dot(n, -lightDir_);
    if (d > 0.0f) {
      r += mat->diffuse.r * lightCol_.r * d;
      g += mat->diffuse.g * lightCol_.g * d;
      b += mat->diffuse.b * lightCol_.b * d;
    }
    lit = true;
  }
  if (!lit) {
    r = mat->diffuse.r;
    g = mat->diffuse.g;
    b = mat->diffuse.b;
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

  out.invW = invW;
  out.viewZ = viewPos.z;
  out.ok = true;
}

// ---------------------------------------------------------------------------
// Triangle setup

void Renderer::emitTriangle(const CachedVertex &a, const CachedVertex &b,
                            const CachedVertex &c, const Material *mat,
                            const Texture *tex) {
  if (!a.ok || !b.ok || !c.ok) return;
  if (triCount_ >= triCapacity_) {  // buffer overflow: drop for this frame
    triDropped_++;
    return;
  }

  Triangle &t = tris_[triCount_];
  t.v[0] = a.sv;
  t.v[1] = b.sv;
  t.v[2] = c.sv;

  // Back-face culling (screen y points down, so front-facing = negative area)
  float area2 = (t.v[1].sx - t.v[0].sx) * (t.v[2].sy - t.v[0].sy) -
                (t.v[2].sx - t.v[0].sx) * (t.v[1].sy - t.v[0].sy);
  if (area2 == 0.0f) return;
  if (area2 > 0.0f && !(mat->flags & MaterialFlags::DOUBLE_SIDED)) return;

  float syMin = std::min({t.v[0].sy, t.v[1].sy, t.v[2].sy});
  float syMax = std::max({t.v[0].sy, t.v[1].sy, t.v[2].sy});
  int yMin = (int)std::ceil(syMin - 0.5f);
  int yMax = (int)std::floor(syMax - 0.5f);
  yMin = std::max(yMin, 0);
  yMax = std::min(yMax, (int)screenH_ - 1);
  if (yMin > yMax) return;

  uint8_t flags = 0;
  if (t.v[0].r == t.v[1].r && t.v[0].r == t.v[2].r && t.v[0].g == t.v[1].g &&
      t.v[0].g == t.v[2].g && t.v[0].b == t.v[1].b && t.v[0].b == t.v[2].b) {
    flags |= TriFlags::FLAT;
  }
  if (mat->blendMode == BlendMode::NONE) flags |= TriFlags::OPAQUE;

  if (tex) {
    flags |= TriFlags::TEX;
    // Wrap texture coordinates per triangle to avoid fixed-point overflow
    // (subtract the integer period of the minimum from all three vertices;
    // relative values are unchanged)
    float uMin = std::min({t.v[0].u, t.v[1].u, t.v[2].u});
    float vMin = std::min({t.v[0].v, t.v[1].v, t.v[2].v});
    float uOff = std::floor(uMin / tex->width) * tex->width;
    float vOff = std::floor(vMin / tex->height) * tex->height;
    for (int i = 0; i < 3; i++) {
      t.v[i].u -= uOff;
      t.v[i].v -= vOff;
    }
  }
#if SHAPOGFX3D_CORRECT_PERSPECTIVE >= 1
  const CachedVertex *cv[3] = {&a, &b, &c};
  for (int i = 0; i < 3; i++) {
    t.invW[i] = cv[i]->invW;
    t.v[i].u *= cv[i]->invW;  // multiply by 1/w for perspective correction
    t.v[i].v *= cv[i]->invW;
  }
#endif

  for (int e = 0; e < 3; e++) {
    float dy = t.v[e == 2 ? 0 : e + 1].sy - t.v[e].sy;
    t.invDy[e] = (dy != 0.0f) ? 1.0f / dy : 0.0f;
  }

  int alpha64 = (int)(clamp01(mat->diffuse.a) * 64.0f + 0.5f);
  t.mat = mat;
  t.depth = (a.viewZ + b.viewZ + c.viewZ) * (1.0f / 3.0f);
  t.yMin = (int16_t)yMin;
  t.yMax = (int16_t)yMax;
  t.flags = flags;
  t.alpha64 = (uint8_t)alpha64;
  t.rasterFn =
      (uint8_t)((int)mat->blendMode * 4 + ((flags & TriFlags::TEX) ? 2 : 0) +
                ((flags & TriFlags::FLAT) ? 1 : 0));
  triCount_++;
}

void Renderer::putPrimitive(const Primitive &prim) {
  if (!tris_) return;
  const Material *mat = prim.material ? prim.material : curMat_;
  if (!mat) return;
  const Texture *tex = materialTexture(mat);
  const Vertex *verts = prim.vertexBuffer->vertices;
  const uint16_t *idx = prim.indices;
  int n = prim.indexCount;

  // Vertex cache: avoid re-transforming vertices shared by several triangles
  // (strips, fans, indexed meshes). Invalidated per primitive.
  for (int i = 0; i < VCACHE_SIZE; i++) vcache_[i].tag = NONE;
  auto fetch = [&](uint16_t vi) -> const CachedVertex & {
    CachedVertex &cv = vcache_[vi & (VCACHE_SIZE - 1)];
    if (cv.tag != vi) {
      shadeVertex(verts[vi], mat, tex, cv);
      cv.tag = vi;
    }
    return cv;
  };

  switch (prim.type) {
    case PrimitiveType::TRIANGLES:
      for (int i = 0; i + 2 < n; i += 3) {
        emitTriangle(fetch(idx[i]), fetch(idx[i + 1]), fetch(idx[i + 2]), mat,
                     tex);
      }
      break;
    case PrimitiveType::TRIANGLE_STRIP:
      for (int i = 2; i < n; i++) {
        if (i & 1) {
          emitTriangle(fetch(idx[i - 1]), fetch(idx[i - 2]), fetch(idx[i]), mat,
                       tex);
        } else {
          emitTriangle(fetch(idx[i - 2]), fetch(idx[i - 1]), fetch(idx[i]), mat,
                       tex);
        }
      }
      break;
    case PrimitiveType::TRIANGLE_FAN:
      for (int i = 2; i < n; i++) {
        emitTriangle(fetch(idx[0]), fetch(idx[i - 1]), fetch(idx[i]), mat, tex);
      }
      break;
  }
}

void Renderer::putCube(const vec3f &center, const vec3f &size, int divs) {
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
        float u0 = (float)i / divs, u1 = (float)(i + 1) / divs;
        float v0 = (float)j / divs, v1 = (float)(j + 1) / divs;
        const float us[4] = {u0, u1, u1, u0};
        const float vs[4] = {v0, v0, v1, v1};

        Vertex quad[4];
        for (int k = 0; k < 4; k++) {
          quad[k].position = corner[0] + du * us[k] + dv * vs[k];
          quad[k].normal = normal;
          quad[k].uv = FACE_UVS[0] + duvU * us[k] + duvV * vs[k];
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

void Renderer::beginRender() {
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
    return tris[a].depth < tris[b].depth;
  });
}

void Renderer::endRender() {}

Span *Renderer::allocSpan() {
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
  sp.r += sp.dr * n;
  sp.g += sp.dg * n;
  sp.b += sp.db * n;
#if SHAPOGFX3D_CORRECT_PERSPECTIVE == 2
  sp.uw += sp.duw * n;
  sp.vw += sp.dvw * n;
  sp.iw += sp.diw * n;
#else
  sp.u += sp.du * n;
  sp.v += sp.dv * n;
#endif
}

// NDC depth of the span at pixel position x
static inline float spanDepthAt(const Span &sp, float x) {
  return sp.z0 + sp.dz * (x - (float)sp.x0);
}

// Is frag nearer than e at the center of the overlap [ox0, ox1)?
static inline bool fragNearer(const Span &frag, const Span &e, int ox0,
                              int ox1) {
  float xm = (float)(ox0 + ox1 - 1) * 0.5f;
  return spanDepthAt(frag, xm) < spanDepthAt(e, xm);
}

// Remove the range [ox0, ox1) from list element e = *pp.
// Returns the position at which to continue scanning (after the remaining part,
// or the rest of e).
Span **Renderer::cutSpan(Span **pp, int ox0, int ox1) {
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

// Append (part of) a span [sp.x0, x1) to the translucent list
void Renderer::appendTranslucent(const Span &sp, int x1) {
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

// Insert an opaque span into the list sorted by x.
// Overlaps with existing spans are resolved by comparing depth at the center of
// the overlap and removing the farther part. Because this does not rely on the
// per-triangle sort order, large and small polygons are ordered correctly too.
void Renderer::insertOpaque(Span &frag) {
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

// Add a translucent span to the translucent list, excluding the parts hidden by
// nearer opaque spans
void Renderer::insertTranslucent(Span &frag) {
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
void Renderer::clipTranslucent(const Span &frag) {
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

// Build a span from the intersection of scanline yc with triangle t.
// Returns false if there is no intersection or it lies outside the region [rx0,
// rx1).
static bool makeSpan(const Triangle &t, float yc, int rx0, int rx1, Span &out) {
  struct EndPt {
    float x, z, u, v, r, g, b;
#if SHAPOGFX3D_CORRECT_PERSPECTIVE >= 1
    float iw;
#endif
  };
  EndPt pts[2];
  int n = 0;

  for (int e = 0; e < 3 && n < 2; e++) {
    const ShadedVertex &a = t.v[e];
    const ShadedVertex &b = t.v[e == 2 ? 0 : e + 1];
    // Half-open test so that an intersection exactly at a vertex is not counted
    // twice
    if ((a.sy <= yc && yc < b.sy) || (b.sy <= yc && yc < a.sy)) {
      float tt = (yc - a.sy) * t.invDy[e];
      EndPt &p = pts[n++];
      p.x = a.sx + (b.sx - a.sx) * tt;
      p.z = a.zNdc + (b.zNdc - a.zNdc) * tt;
      p.u = a.u + (b.u - a.u) * tt;
      p.v = a.v + (b.v - a.v) * tt;
      p.r = a.r + (b.r - a.r) * tt;
      p.g = a.g + (b.g - a.g) * tt;
      p.b = a.b + (b.b - a.b) * tt;
#if SHAPOGFX3D_CORRECT_PERSPECTIVE >= 1
      p.iw = t.invW[e] + (t.invW[e == 2 ? 0 : e + 1] - t.invW[e]) * tt;
#endif
    }
  }
  if (n < 2) return false;
#if SHAPOGFX3D_CORRECT_PERSPECTIVE == 1
  // Vertical-only correction: divide at the end points so they are exact;
  // the span interior is then interpolated affinely
  for (int i = 0; i < 2; i++) {
    float inv = 1.0f / pts[i].iw;
    pts[i].u *= inv;
    pts[i].v *= inv;
  }
#endif

  const EndPt *l = &pts[0], *r = &pts[1];
  if (l->x > r->x) std::swap(l, r);
  float width = r->x - l->x;
  if (width <= 0.0f) return false;  // zero width

  // Fill the pixels whose centers (xi + 0.5) fall inside [x0, x1)
  int xi0 = (int)std::ceil(l->x - 0.5f);
  int xi1 = (int)std::ceil(r->x - 0.5f);
  xi0 = std::max(xi0, rx0);
  xi1 = std::min(xi1, rx1);
  if (xi0 >= xi1) return false;

  // Attribute values at the center of the leftmost pixel and per-pixel
  // increments
  float invW = 1.0f / width;
  float t0 = ((float)xi0 + 0.5f - l->x) * invW;
  auto lin = [&](float a0, float a1, float &start, float &delta) {
    float d = (a1 - a0) * invW;
    start = a0 + (a1 - a0) * t0;
    delta = d;
  };
  // Color: if both ends are within 0..255 so are all values in between.
  // Clamp at 0 to guard against slightly negative values from rounding.
  auto linColor = [&](float a0, float a1, int32_t &start, int32_t &delta) {
    float sf, df;
    lin(std::min(a0, 255.0f), std::min(a1, 255.0f), sf, df);
    start = (int32_t)(std::max(sf, 0.0f) * FIX_ONE);
    delta = (int32_t)(df * FIX_ONE);
  };

  out.x0 = xi0;
  out.x1 = xi1;
  lin(l->z, r->z, out.z0, out.dz);
  linColor(l->r, r->r, out.r, out.dr);
  linColor(l->g, r->g, out.g, out.dg);
  linColor(l->b, r->b, out.b, out.db);
#if SHAPOGFX3D_CORRECT_PERSPECTIVE == 2
  lin(l->u, r->u, out.uw, out.duw);
  lin(l->v, r->v, out.vw, out.dvw);
  lin(l->iw, r->iw, out.iw, out.diw);
#else
  float sf, df;
  lin(l->u, r->u, sf, df);
  out.u = (int32_t)(sf * FIX_ONE);
  out.du = (int32_t)(df * FIX_ONE);
  lin(l->v, r->v, sf, df);
  out.v = (int32_t)(sf * FIX_ONE);
  out.dv = (int32_t)(df * FIX_ONE);
#endif
  out.tri = &t;
  out.next = nullptr;
  return true;
}

// ---------------------------------------------------------------------------
// Pixel processing (fixed point)
//
// The loop is specialized for every combination of blend mode x textured x flat
// (constant color), so that the inner loop contains no branches and no
// unnecessary interpolation.

template <BlendMode B, bool TEX, bool FLAT>
static void rasterSpanT(uint16_t *px, int n, const Span &sp) {
  const Triangle &t = *sp.tri;
  int32_t r = sp.r, g = sp.g, b = sp.b;
  const int32_t dr = sp.dr, dg = sp.dg, db = sp.db;
  const uint32_t a = t.alpha64;

  // With equal vertex colors and no texture, the color is constant over the
  // span
  uint32_t sr = (uint32_t)(r >> 19) & 31u;
  uint32_t sg = (uint32_t)(g >> 18) & 63u;
  uint32_t sb = (uint32_t)(b >> 19) & 31u;
  if (B == BlendMode::NONE && FLAT && !TEX) {
    gfx2d::fillRgb565(px, n, gfx2d::makeRgb565(sr, sg, sb));
    return;
  }

  // Texture (width and height must be powers of two)
  const uint16_t *tp = nullptr;
  uint32_t uMask = 0, vMask = 0;
  int wShift = 0;
  if (TEX) {
    const Texture &tex = *t.mat->texture;
    wShift = gfx2d::log2Floor(tex.width);
    uMask = (1u << wShift) - 1;
    vMask = (1u << gfx2d::log2Floor(tex.height)) - 1;
    tp = tex.pixels;
  }
#if SHAPOGFX3D_CORRECT_PERSPECTIVE == 2
  float uw = sp.uw, vw = sp.vw, iw = sp.iw;
  const float duw = sp.duw, dvw = sp.dvw, diw = sp.diw;
#else
  int32_t u = sp.u, v = sp.v;
  const int32_t du = sp.du, dv = sp.dv;
#endif

  for (int i = 0; i < n; i++) {
    if (TEX) {
#if SHAPOGFX3D_CORRECT_PERSPECTIVE == 2
      // Perspective correction: interpolate (u/w, v/w) and 1/w, divide per
      // pixel
      float inv = 1.0f / iw;
      uint32_t ui = (uint32_t)(int32_t)(uw * inv);
      uint32_t vi = (uint32_t)(int32_t)(vw * inv);
#else
      uint32_t ui = (uint32_t)(u >> FIX_SHIFT);
      uint32_t vi = (uint32_t)(v >> FIX_SHIFT);
#endif
      uint32_t texel = tp[((vi & vMask) << wShift) | (ui & uMask)];
      // Modulate the texel (5/6/5 bits) by the vertex color (0..255).
      // (c + 1) * t >> 8 preserves the maximum value.
      uint32_t cr = ((uint32_t)(r >> FIX_SHIFT) & 0xFFu) + 1;
      uint32_t cg = ((uint32_t)(g >> FIX_SHIFT) & 0xFFu) + 1;
      uint32_t cb = ((uint32_t)(b >> FIX_SHIFT) & 0xFFu) + 1;
      sr = (cr * (texel >> 11)) >> 8;
      sg = (cg * ((texel >> 5) & 63u)) >> 8;
      sb = (cb * (texel & 31u)) >> 8;
    } else if (!FLAT) {
      sr = (uint32_t)(r >> 19) & 31u;
      sg = (uint32_t)(g >> 18) & 63u;
      sb = (uint32_t)(b >> 19) & 31u;
    }

    if (B == BlendMode::NONE) {
      px[i] = gfx2d::makeRgb565(sr, sg, sb);
    } else if (B == BlendMode::ALPHA) {
      px[i] = gfx2d::blendAlphaRgb565(px[i], gfx2d::makeRgb565(sr, sg, sb), a);
    } else {
      // Additive (color is pre-multiplied by opacity): saturating add per
      // channel
      px[i] = gfx2d::addSaturateRgb565(px[i], sr, sg, sb);
    }

    if (!FLAT) {
      r += dr;
      g += dg;
      b += db;
    }
    if (TEX) {
#if SHAPOGFX3D_CORRECT_PERSPECTIVE == 2
      uw += duw;
      vw += dvw;
      iw += diw;
#else
      u += du;
      v += dv;
#endif
    }
  }
}

using RasterFn = void (*)(uint16_t *, int, const Span &);

// Triangle::rasterFn = blend * 4 + tex * 2 + flat
static const RasterFn RASTER_FNS[12] = {
    rasterSpanT<BlendMode::NONE, false, false>,
    rasterSpanT<BlendMode::NONE, false, true>,
    rasterSpanT<BlendMode::NONE, true, false>,
    rasterSpanT<BlendMode::NONE, true, true>,
    rasterSpanT<BlendMode::ALPHA, false, false>,
    rasterSpanT<BlendMode::ALPHA, false, true>,
    rasterSpanT<BlendMode::ALPHA, true, false>,
    rasterSpanT<BlendMode::ALPHA, true, true>,
    rasterSpanT<BlendMode::ADD, false, false>,
    rasterSpanT<BlendMode::ADD, false, true>,
    rasterSpanT<BlendMode::ADD, true, false>,
    rasterSpanT<BlendMode::ADD, true, true>,
};

static inline void rasterSpan(uint16_t *line, int rx0, const Span &sp) {
  RASTER_FNS[sp.tri->rasterFn](line + (sp.x0 - rx0), sp.x1 - sp.x0, sp);
}

// Merge two ascending lists (links are stored in link_)
uint16_t Renderer::mergeLists(uint16_t a, uint16_t b) {
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

void Renderer::render(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t *dst,
                      uint32_t stride) {
  if (!tris_ || !spanPool_) return;
  const int rx0 = x, rx1 = x + w;
  const int y0 = std::max((int)y, 0);
  const int y1 = std::min((int)y + h, (int)screenH_);
  if (y0 >= y1 || rx0 >= rx1) return;
  const uint16_t clear565 = gfx2d::packRgb565(clearColor_);
  uint16_t *const link = link_;

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

    const float yc = (float)yi + 0.5f;
    uint16_t *pp = &active;
    while (*pp != NONE) {
      const uint16_t p = *pp;
      const Triangle &t = tris_[order_[p]];
      if (yi > t.yMax) {
        *pp = link[p];  // passed: remove from the active list
        continue;
      }
      Span sp;
      if (makeSpan(t, yc, rx0, rx1, sp)) {
        if (t.flags & TriFlags::OPAQUE) {
          clipTranslucent(sp);
          insertOpaque(sp);
        } else {
          insertTranslucent(sp);
        }
      }
      pp = &link[p];
    }
    if (spanCount_ > spanPeak_) spanPeak_ = spanCount_;

    // Draw the opaque spans (ascending x) and the background in the gaps,
    // then composite the translucent spans on top
    uint16_t *line = dst + (size_t)(yi - y) * stride;
    int cursor = rx0;
    for (const Span *e = opaqueHead_; e; e = e->next) {
      if (e->x0 > cursor)
        gfx2d::fillRgb565(line + (cursor - rx0), e->x0 - cursor, clear565);
      rasterSpan(line, rx0, *e);
      cursor = e->x1;
    }
    if (cursor < rx1)
      gfx2d::fillRgb565(line + (cursor - rx0), rx1 - cursor, clear565);
    for (const Span *e = transHead_; e; e = e->next) rasterSpan(line, rx0, *e);
  }
}

// ---------------------------------------------------------------------------
// Statistics

Stats Renderer::getStats() const {
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
  return st;
}

}  // namespace shapoco::gfx3d
