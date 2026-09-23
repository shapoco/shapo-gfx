#ifndef SHAPOGFX3D_GFX3D_HPP
#define SHAPOGFX3D_GFX3D_HPP

#include <cstddef>
#include <cstdint>

#include "shapoco/gfx2d/gfx2d.hpp"
#include "shapoco/gfx3d/math3d.hpp"

namespace shapoco::gfx3d {

// Shared with shapoco::gfx2d.
using gfx2d::BlendMode;
using gfx2d::PixelFormat;
using gfx2d::Surface;
using gfx2d::Texture;

// ---------------------------------------------------------------------------
// Data structures

struct Vertex {
  vec3f position;
  vec3f normal;
  vec2f uv;            // unused when environment mapping is enabled
  gfx2d::Color color;  // ARGB8888; used when MaterialFlags::VERTEX_COLOR is set
                       // (alpha ignored)
};

constexpr gfx2d::Color VERTEX_WHITE = 0xFFFFFFFFu;

// Compact vertex (16 bytes instead of the 36 of Vertex) for models kept in
// flash. A VertexBuffer holds either form; a packed vertex is decoded once per
// vertex, which the vertex cache makes negligible.
//
//   position  integers scaled by VertexBuffer::scale and offset by
//             VertexBuffer::bias, so the resolution is 1/65534 of the model's
//             extent along each axis
//   uv        1/1024 texel-space units (range -32..32)
//   normal    1/127 units; the decoded vector is unit length to about 1%,
//             which is well below the resolution of the output formats
//   color     R, G, B (the alpha of Vertex::color is ignored anyway)
struct PackedVertex {
  int16_t position[3];
  int16_t uv[2];
  int8_t normal[3];
  uint8_t color[3];
};
static_assert(sizeof(PackedVertex) == 16, "PackedVertex must stay 16 bytes");

constexpr float PACKED_UV_SCALE = 1.0f / 1024.0f;
constexpr float PACKED_NORMAL_SCALE = 1.0f / 127.0f;

// Vertices of a primitive, in either form. `scale` and `bias` apply to packed
// positions only; the remaining members may be left at their defaults for a
// buffer of plain `Vertex` (`VertexBuffer vb = {count, vertices};`).
// A vertex in fixed point: what the fixed-point vertex stage
// (SHAPOGFX3D_FIXED_POINT) takes as it is, with no conversion at all. The
// float stage converts it, so a scene may use this form on every target.
// For an application that already computes its geometry in integers.
struct FixedVertex {
  int32_t position[3];  // model units, 16.16
  int16_t normal[3];    // Q15 (unit length)
  int16_t uv[2];        // 1/1024 texel-space units (range -32..32)
  gfx2d::Color color;   // ARGB8888, used with MaterialFlags::VERTEX_COLOR
};

struct VertexBuffer {
  uint16_t vertexCount;
  const Vertex *vertices;                // nullptr: the buffer is packed
  const PackedVertex *packed = nullptr;  // used when `vertices` is nullptr
  const FixedVertex *fixed = nullptr;    // used when both above are nullptr
  vec3f scale = {1, 1, 1};               // packed position scale
  vec3f bias = {0, 0, 0};                // packed position offset
};

namespace MaterialFlags {
constexpr uint32_t TEXTURE = 1u << 0;  // enable texture mapping
constexpr uint32_t ENV_MAP = 1u << 1;  // use the texture as an environment map
constexpr uint32_t DOUBLE_SIDED =
    1u << 2;  // draw both sides (disable back-face culling)
constexpr uint32_t VERTEX_COLOR =
    1u << 3;  // multiply the lit color by Vertex::color
}  // namespace MaterialFlags

// Textures may be in any enabled pixel format; width and height must be powers
// of two. A texture in ARGB4444 makes the material translucent: its alpha is
// multiplied into the material opacity per pixel.
//
// Texturing and translucency can be compiled out of the renderer
// (SHAPOGFX3D_TEXTURE, SHAPOGFX3D_BLEND); the corresponding members are then
// ignored at run time, exactly like a surface in a disabled pixel format.
struct Material {
  colorf diffuse;          // diffuse color (a is used as opacity)
  colorf ambient;          // ambient color
  const Texture *texture;  // texture (may be nullptr when unused)
  BlendMode blendMode;
  uint32_t flags;  // combination of MaterialFlags
};

// Triangles are lit, textured and back-face culled. Points and lines are
// unlit (diffuse x vertex color), 1 pixel wide (points: pointSize()), never
// culled, and clipped against the near plane. They take part in the depth
// resolution like any other span, so lines hidden by nearer surfaces disappear.
enum class PrimitiveType : uint8_t {
  TRIANGLES,
  TRIANGLE_STRIP,
  TRIANGLE_FAN,
  POINTS,
  LINES,       // pairs of indices
  LINE_STRIP,  // consecutive indices
  LINE_LOOP,   // like LINE_STRIP, closed back to the first vertex
};

struct Primitive {
  PrimitiveType type;
  const VertexBuffer *vertexBuffer;
  uint16_t indexCount;
  const uint16_t *indices;
  const Material
      *material;  // nullptr: use the material set by Graphics3D::setMaterial()
};

// ---------------------------------------------------------------------------
// Statistics

struct Stats {
  size_t arenaSize;  // size of the arena passed to init()
  size_t arenaUsed;  // bytes actually used in the last frame (fixed part +
                     // triangles + span peak)
  size_t triBytes;   // bytes of the triangle buffer holding the current scene
                     // (records plus 4 bytes of sort order and link each)
  size_t triBytesTotal;  // bytes available for the triangle buffer. Records
                         // vary in size, so this is a byte budget rather than
                         // a triangle count.
  int triCount;          // triangles in the current scene (after culling)
  int triDropped;  // triangles dropped because the buffer overflowed (reset by
                   // beginScene())
  int layerCount;  // layers used by the current scene (reset by beginScene())
  int layersDropped;  // beginLayer() calls ignored because there was no free
                      // layer (reset by beginScene())
  int spanCapacity;   // capacity of the span pool (of each render context)
  int spanPeak;     // maximum number of spans used on a single scanline, in the
                    // busiest render context (reset by beginRender())
  int spanDropped;  // spans dropped because a pool overflowed, all contexts
                    // together (reset by beginRender())
  int badIndices;  // triangles dropped because an index was out of range (reset
                   // by beginScene())
  int nodesDropped;  // nodes skipped because the state stack was full (reset by
                     // beginScene())
};

// ---------------------------------------------------------------------------
// Static scene description (typically generated by bin/gltf2cpp as const data)

struct Mesh {
  const Primitive *primitives;  // each primitive carries its own material
  uint16_t primitiveCount;
};

struct Node {
  const char *name;             // may be nullptr
  mat4f transform;              // local transform relative to the parent
  const Mesh *mesh;             // may be nullptr
  const Node *const *children;  // may be nullptr when childCount == 0
  uint16_t childCount;
};

struct Scene {
  const Node *const *roots;
  uint16_t rootCount;
};

// Optional per-node hook for putNode()/putScene(): may modify the local
// transform (e.g. to animate a part) or return false to skip the node and its
// children. No allocation; the visitor is only called during traversal.
class NodeVisitor {
 public:
  virtual ~NodeVisitor() = default;
  virtual bool onNode(const Node &node, mat4f &local) {
    (void)node, (void)local;
    return true;
  }
};

// ---------------------------------------------------------------------------
// Layers
//
// A layer is a range of the scene that is depth-sorted on its own. Layers are
// opened with beginLayer() and every layer is drawn in front of the layers
// opened before it, which the application guarantees by building the scene
// back to front; within a layer the usual depth resolution applies. The scene
// of an application that never calls beginLayer() is a single layer and
// behaves exactly as before.
namespace LayerFlags {
// The layer carries no depth: its primitives are drawn in the order they were
// added (the later one wins), and their records hold no depth plane, which
// makes them 12 bytes smaller (8 with SHAPOGFX3D_DEPTH_BITS=16). Use it for
// geometry that is already ordered
// back to front, such as a background.
constexpr uint32_t NO_DEPTH = 1u << 0;
}  // namespace LayerFlags

// ---------------------------------------------------------------------------
// Initialization parameters
//
// Obtain the defaults from defaultConfig(), adjust what needs adjusting and
// pass the result to Graphics3D::init(). New members may be added later with
// a default that preserves the current behavior.
struct Config {
  int16_t screenWidth = 0;
  int16_t screenHeight = 0;
  void *arena = nullptr;   // working memory (8-byte aligned internally)
  size_t arenaSize = 0;    // size of the arena in bytes
  int spanCapacity = 0;    // spans held per scanline; 0 selects the default
                           // (a quarter of the arena left after the fixed
                           // part, clamped to 32..512). Spans beyond the
                           // capacity are dropped, which leaves holes in the
                           // picture; Stats::spanPeak tells how many a scene
                           // really needs, so a tuned value gives the rest of
                           // the arena to the triangle buffer. Each render
                           // context has a pool of this many.
  int renderContexts = 1;  // render() calls that may run at the same time
                           // (1..4, see render(ctx, ...)). Each costs a span
                           // pool, 4 bytes per screen row and 2 bytes per
                           // primitive.
};

inline Config defaultConfig(int16_t w, int16_t h, void *arena,
                            size_t arenaSize) {
  Config cfg;
  cfg.screenWidth = w;
  cfg.screenHeight = h;
  cfg.arena = arena;
  cfg.arenaSize = arenaSize;
  return cfg;
}

// Internal structures (defined in gfx3d.cpp)
namespace detail {
// Fixed-point copies of the current matrix (rotation / scale Q18,
// translation 16.16), the projection and the lights, kept by the
// fixed-point vertex stage (SHAPOGFX3D_FIXED_POINT) and refreshed when the
// float originals change. Present, but unused, in the float build.
struct MatQ {
  int32_t r[9];  // column-major, r[col * 3 + row]
  int32_t t[3];
};
struct ProjQ {
  uint8_t kind;              // ProjKind
  int32_t zNear;             // 16.16
  int32_t fx, fy;            // perspective: focal length, 8.8 px
  int32_t cx, cy;            // screen center, 16.16 px
  int32_t zA, zB;            // perspective: z = zA (8.24) + zB (16.16) / w
  int32_t sxScale, syScale;  // orthographic: px per unit, Q16
  int32_t sxOff, syOff;      // orthographic: 16.16 px
  int32_t zScale, zOff;      // orthographic: Q24 per unit, 8.24
  int32_t m[16];             // generic: the projection matrix, Q18
};
struct LightQ {
  int32_t dir[3];  // view space, towards the scene, Q15
  int32_t col[3];  // 8.8
  int32_t env[3];  // 8.8
};
struct ShadedVertex;
struct VertexQ;
struct TriHead;
struct PlaneSet;
struct RenderContext;
struct LayerDesc;
struct Span;
struct StackEntry;
struct CachedVertex;
struct UnlitVertex;
struct PrimSetup;
}  // namespace detail

// ---------------------------------------------------------------------------
// Graphics3D
//
// All working memory is carved from the arena passed to init(); the library
// never calls malloc/new. Multiple Graphics3D instances may coexist, each with
// its own arena. All angles are in radians.

class Graphics3D {
 public:
  Graphics3D() = default;
  Graphics3D(const Graphics3D &) = delete;
  Graphics3D &operator=(const Graphics3D &) = delete;
  Graphics3D(Graphics3D &&) = default;
  Graphics3D &operator=(Graphics3D &&) = default;

  // Initialize from a Config (see defaultConfig()).
  void init(const Config &cfg);
  // Initialize with the default parameters (w, h: screen size; arena,
  // arenaSize: working memory).
  void init(int16_t w, int16_t h, void *arena, size_t arenaSize) {
    init(defaultConfig(w, h, arena, arenaSize));
  }
  void
  deinit();  // release the arena (the renderer becomes unusable until init())

  void beginScene();  // start building a scene
  void endScene();    // finish building a scene

  // Open a new layer (see LayerFlags). Everything added afterwards is drawn in
  // front of everything added before, so layers must be opened back to front.
  // The layer is closed by the next beginLayer(), by endLayer() or by
  // endScene(); beginScene() opens the first layer implicitly. Ignored (and
  // counted in Stats::layersDropped) when all layers are in use.
  void beginLayer(uint32_t flags = 0);
  // Close the current layer. What follows goes into a new layer with the
  // default flags, still in front of everything before it.
  void endLayer();

  void loadIdentity();             // reset the current matrix to identity
  void translate(const vec3f &v);  // apply a translation to the current matrix
  void translate(float x, float y, float z);  // same
  void rotate(float angle,
              const vec3f &axis);  // apply a rotation to the current matrix
  void rotate(float angle, float x, float y, float z);  // same
  void scale(const vec3f &v);  // apply a scaling to the current matrix
  void scale(float x, float y, float z);  // same
  void transform(const mat4f &m);         // multiply the current matrix by m
  // Multiply the current matrix by a view matrix looking from eye towards
  // target
  void lookAt(const vec3f &eye, const vec3f &target,
              const vec3f &up = {0, 1, 0});

  // Push the current matrix and material onto the stack (SHAPOGFX3D_STACK_DEPTH
  // levels, 16 by default).
  // Returns false (and pushes nothing) when the stack is full.
  bool pushState();
  void popState();  // restore the matrix and material from the stack

  void setMaterial(const Material &mat);     // set the current material
  void putPrimitive(const Primitive &prim);  // add a primitive to the scene

  // Size of POINTS in pixels (a square, default 1, at most 64)
  void setPointSize(int pixels) {
    pointSize_ = pixels < 1 ? 1 : (pixels > 64 ? 64 : pixels);
  }
  int pointSize() const { return pointSize_; }
  // Depth bias added to the NDC depth of every primitive emitted afterwards
  // (NDC range -1..1; negative brings it nearer). Use a small negative value to
  // draw lines on top of coplanar polygons without z-fighting.
  void setDepthBias(float bias) { depthBias_ = bias; }
  float depthBias() const { return depthBias_; }
  // --- Basic shapes
  // ----------------------------------------------------------- All shapes are
  // centered at `center`, have their axis along +Y, outward normals and
  // counter-clockwise front faces. Segment counts are clamped to [3, 64]
  // (subdivision counts to [1, 64]). Vertex colors are white.
  //
  // Box; divs: subdivisions per edge of each face
  void putCube(const vec3f &center, const vec3f &size, int divs = 1);
  // Rectangle in the XZ plane facing +Y (single-sided). u runs along +X, v
  // along +Z.
  void putPlane(const vec3f &center, float sizeX, float sizeZ, int divsX = 1,
                int divsZ = 1);
  // Disk in the XZ plane facing +Y (single-sided); u/v map its bounding square
  void putDisk(const vec3f &center, float radius, int segments = 16);
  // UV sphere: u = longitude (around +Y), v = 0 at the north pole (+Y), 1 at
  // the south pole
  void putSphereUV(const vec3f &center, float radius, int segmentsU = 16,
                   int segmentsV = 8);
  // Geodesic sphere from a subdivided icosahedron; level 0..4 (20 x 4^level
  // triangles). UVs are equirectangular like putSphereUV, with the seam and
  // poles fixed per face.
  void putIcosphere(const vec3f &center, float radius, int level = 2);
  // Cylinder along +Y; heightDivs: rings along the height; caps: closed ends.
  // Side u = around the axis, v = 0 at the top, 1 at the bottom.
  void putCylinder(const vec3f &center, float radius, float height,
                   int segments = 16, int heightDivs = 1, bool caps = true);
  // Truncated cone (radiusTop = 0 for a cone); same conventions as putCylinder
  void putCone(const vec3f &center, float radiusBottom, float radiusTop,
               float height, int segments = 16, int heightDivs = 1,
               bool caps = true);
  // Torus around +Y; u = around the ring, v = around the tube
  void putTorus(const vec3f &center, float majorRadius, float minorRadius,
                int majorSegments = 24, int minorSegments = 12);
  // Line segment (unlit, 1 pixel wide) and box outline (12 edges)
  void putLine(const vec3f &a, const vec3f &b);
  void putWireCube(const vec3f &center, const vec3f &size);

  // --- Static scenes
  // ------------------------------------------------------------
  void putMesh(const Mesh &mesh);
  // Push, apply node.transform (after the visitor may adjust it), draw the
  // mesh, recurse into the children, pop. Skipped (and counted) when the stack
  // is full.
  void putNode(const Node &node, NodeVisitor *visitor = nullptr);
  void putScene(const Scene &scene, NodeVisitor *visitor = nullptr);

  // Set a directional light (dir is transformed by the current matrix at call
  // time).
  void enableParallelLight(const vec3f &dir, const colorf &col);
  void disableParallelLight();

  void enableEnvironmentLight(const colorf &col);  // set the ambient light
  void disableEnvironmentLight();

  // Background: pixels not covered by any span are filled with the clear color.
  // With the clear disabled they keep the previous content of the target, so a
  // scene can be rendered on top of a 2D background.
  void setClearColor(
      const colorf &col);  // set the background color (and enable clearing)
  void disableClear();
  bool isClearEnabled() const { return clearEnabled_; }

  void setPerspectiveProjection(float fovY, float aspect, float zNear,
                                float zFar);
  void setOrthographicProjection(float left, float right, float bottom,
                                 float top, float zNear, float zFar);

  void beginRender();  // start rendering (sorts the triangles)
  void endRender();    // finish rendering

  // Render the screen region (x, y, w, h) into dst at (dstX, dstY). dst must be
  // in RGB565_SWAPPED, RGB565 or RGB444; other formats are ignored. The region
  // is clipped to the screen and to dst.
  void render(int16_t x, int16_t y, int16_t w, int16_t h, const Surface &dst,
              int16_t dstX = 0, int16_t dstY = 0);
  // The same with render context ctx (0 .. Config::renderContexts - 1).
  // Calls with different contexts may run at the same time, for example one
  // per core, each rendering its own region: between beginRender() and
  // endRender() render() only reads the scene. Calls with the same context
  // must not overlap. An invalid context draws nothing.
  void render(int ctx, int16_t x, int16_t y, int16_t w, int16_t h,
              const Surface &dst, int16_t dstX = 0, int16_t dstY = 0);

  // Get statistics (call after endRender() to get the values of that frame).
  Stats getStats() const;

  // Bytes of the triangle buffer one primitive takes -- its record and its
  // entry -- by what it holds: a depth plane (a layer without
  // LayerFlags::NO_DEPTH), interpolated colors (its vertex colors differ) and
  // texture coordinates. The values depend on the build (compiled-out
  // features, SHAPOGFX3D_DEPTH_BITS, the pointer size) and, through the
  // entry, on Config::renderContexts, so budget the arena with this rather
  // than with fixed numbers. Before init() one render context is assumed.
  size_t primitiveBytes(bool depth, bool smooth, bool textured) const;

  int16_t screenWidth() const { return screenW_; }
  int16_t screenHeight() const { return screenH_; }
  bool isInitialized() const { return recBase_ != nullptr; }

 private:
  int16_t screenW_ = 0, screenH_ = 0;

  // Triangle buffer. Primitive records vary in size (see gfx3d.cpp), so they
  // are packed downwards from the end of the region while the entry array
  // (record offset + scanline link, 4 bytes each) grows upwards from its
  // start; the scene is complete as long as the two have not met.
  uint8_t *recBase_ = nullptr;   // start of the region (entry array)
  uint8_t *recTop_ = nullptr;    // lowest record stored so far
  uint8_t *recEnd_ = nullptr;    // end of the region
  uint16_t *entries_ = nullptr;  // == recBase_, sorted by beginRender()
  int entryBytes_ = 4;  // per primitive: the entry and a link per context
  int triCount_ = 0;

  detail::LayerDesc *layers_ = nullptr;
  int layerCount_ = 0;
  int layersDropped_ = 0;
  uint32_t layerFlags_ = 0;  // flags of the layer opened by the next primitive
  bool layerOpen_ = false;   // a layer is receiving primitives

  // What render() changes, per context (span pools, span lists, scanline
  // lists), so that several render() calls can run at the same time
  detail::RenderContext *contexts_ = nullptr;
  int contextCount_ = 0;
  int spanCapacity_ = 0;  // spans per context

  detail::StackEntry *stack_ = nullptr;
  int stackTop_ = 0;

  detail::CachedVertex *vcache_ = nullptr;

  mat4f cur_ = mat4f::identity();
  const Material *curMat_ = nullptr;

  // Projection. The kind lets the vertex stage skip the zero elements of the
  // matrices built by the two setters.
  enum class ProjKind : uint8_t { GENERIC, PERSPECTIVE, ORTHOGRAPHIC };
  mat4f proj_ = mat4f::identity();
  ProjKind projKind_ = ProjKind::GENERIC;
  float zNear_ = 0.1f;

  bool lightEnabled_ = false;
  vec3f lightDir_ = {0, 0, -1};  // view space, normalized
  colorf lightCol_ = {1, 1, 1, 1};

  bool envEnabled_ = false;
  colorf envCol_ = {0, 0, 0, 1};

  bool clearEnabled_ = true;
  colorf clearColor_ = {0, 0, 0, 1};
  gfx2d::Color clearColor8_ = 0xFF000000u;  // clearColor_ as render() uses it

  size_t arenaSize_ = 0;
  size_t arenaFixed_ = 0;  // bytes always in use (line buckets, layer table,
                           // matrix stack, vertex cache)
  int triDropped_ = 0;
  int badIndices_ = 0;
  int nodesDropped_ = 0;

  int pointSize_ = 1;
  float depthBias_ = 0.0f;
  // Fixed-point stage (SHAPOGFX3D_FIXED_POINT): see detail::MatQ
  detail::MatQ curQ_ = {};
  detail::ProjQ projQ_ = {};
  detail::LightQ lightQ_ = {};
  bool curQDirty_ = true;
  bool projQDirty_ = true;
  void refreshFixed();  // bring curQ_ / projQ_ up to date with cur_ / proj_
  bool projectQ(int32_t vx, int32_t vy, int32_t vz, detail::ShadedVertex &sv,
                int32_t &invW) const;

  bool projectPoint(const vec3f &view, float &sx, float &sy, float &zNdc,
                    float &invW) const;
  // Transformed vertex `vi` of `vb`, from the vertex cache; decodes a packed
  // vertex on a miss. Out of line: putPrimitive() calls it from every branch.
  const detail::CachedVertex &fetchVertex(const VertexBuffer &vb, uint16_t vi,
                                          const detail::PrimSetup &ps);
  bool fetchUnlitVertex(const VertexBuffer &vb, uint16_t vi,
                        const Material *mat, detail::UnlitVertex &out);
  void shadeVertex(const Vertex &in, const detail::PrimSetup &ps,
                   detail::CachedVertex &out) const;
  // The fixed-point stage's forms of shadeVertex() and unlitVertex()
  void shadeVertexQ(const detail::VertexQ &in, const detail::PrimSetup &ps,
                    detail::CachedVertex &out) const;
  void unlitVertexQ(const detail::VertexQ &in, const Material *mat,
                    detail::UnlitVertex &out) const;
  void emitTriangle(const detail::CachedVertex &a,
                    const detail::CachedVertex &b,
                    const detail::CachedVertex &c, const Material *mat,
                    const Texture *tex);
  // Reserve `size` bytes for a primitive record and register its entry;
  // nullptr when the triangle buffer is full. openLayer() is called first, so
  // the record belongs to the current layer.
  uint8_t *allocRecord(size_t size);
  // Store a primitive whose header and planes are complete (see PlaneSet)
  void storePrimitive(const detail::TriHead &h, const detail::PlaneSet &ps,
                      bool smooth, bool textured);
  // Float setup of a culled-in triangle; CLIP: it reaches beyond the guard
  // band and is clipped to it
  template <bool CLIP>
  void emitTriangleSetup(const detail::CachedVertex &a,
                         const detail::CachedVertex &b,
                         const detail::CachedVertex &c, const Material *mat,
                         const Texture *tex);
  uint8_t layerByte();  // id of the current layer, opening one if needed
  void unlitVertex(const Vertex &in, const Material *mat,
                   detail::UnlitVertex &out) const;
  void emitLine(detail::UnlitVertex a, detail::UnlitVertex b,
                const Material *mat);
  void emitPoint(const detail::UnlitVertex &a, const Material *mat);
};

}  // namespace shapoco::gfx3d

#endif
