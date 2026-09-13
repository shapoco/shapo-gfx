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
  vec2f uv;  // unused when environment mapping is enabled
};

struct VertexBuffer {
  uint16_t vertexCount;
  const Vertex *vertices;
};

namespace MaterialFlags {
constexpr uint32_t TEXTURE = 1u << 0;  // enable texture mapping
constexpr uint32_t ENV_MAP = 1u << 1;  // use the texture as an environment map
constexpr uint32_t DOUBLE_SIDED =
    1u << 2;  // draw both sides (disable back-face culling)
}  // namespace MaterialFlags

// Textures may be in any enabled pixel format; width and height must be powers
// of two. A texture in ARGB4444 makes the material translucent: its alpha is
// multiplied into the material opacity per pixel.
struct Material {
  colorf diffuse;          // diffuse color (a is used as opacity)
  colorf ambient;          // ambient color
  const Texture *texture;  // texture (may be nullptr when unused)
  BlendMode blendMode;
  uint32_t flags;  // combination of MaterialFlags
};

enum class PrimitiveType : uint8_t {
  TRIANGLES,
  TRIANGLE_STRIP,
  TRIANGLE_FAN,
};

struct Primitive {
  PrimitiveType type;
  const VertexBuffer *vertexBuffer;
  uint16_t indexCount;
  const uint16_t *indices;
  const Material
      *material;  // nullptr: use the material set by Renderer::setMaterial()
};

// ---------------------------------------------------------------------------
// Statistics

struct Stats {
  size_t arenaSize;  // size of the arena passed to init()
  size_t arenaUsed;  // bytes actually used in the last frame (fixed part +
                     // triangles + span peak)
  int triCapacity;   // capacity of the triangle buffer
  int triCount;      // triangles in the current scene (after culling)
  int triDropped;  // triangles dropped because the buffer overflowed (reset by
                   // beginScene())
  int spanCapacity;  // capacity of the span pool
  int spanPeak;  // maximum number of spans used on a single scanline (reset by
                 // beginRender())
  int spanDropped;  // spans dropped because the pool overflowed (reset by
                    // beginRender())
};

// Internal structures (defined in gfx3d.cpp)
namespace detail {
struct Triangle;
struct Span;
struct StackEntry;
struct CachedVertex;
}  // namespace detail

// ---------------------------------------------------------------------------
// Renderer
//
// All working memory is carved from the arena passed to init(); the library
// never calls malloc/new. Multiple Renderer instances may coexist, each with
// its own arena. All angles are in radians.

class Renderer {
 public:
  Renderer() = default;
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;
  Renderer(Renderer &&) = default;
  Renderer &operator=(Renderer &&) = default;

  // Initialize (w, h: screen size; arena, arenaSize: working memory).
  void init(int16_t w, int16_t h, void *arena, size_t arenaSize);
  void
  deinit();  // release the arena (the renderer becomes unusable until init())

  void beginScene();  // start building a scene
  void endScene();    // finish building a scene

  void loadIdentity();             // reset the current matrix to identity
  void translate(const vec3f &v);  // apply a translation to the current matrix
  void translate(float x, float y, float z);  // same
  void rotate(float angle,
              const vec3f &axis);  // apply a rotation to the current matrix
  void rotate(float angle, float x, float y, float z);  // same
  void scale(const vec3f &v);  // apply a scaling to the current matrix
  void scale(float x, float y, float z);  // same

  void pushState();  // push the current matrix and material onto the stack
  void popState();   // restore the matrix and material from the stack

  void setMaterial(const Material &mat);     // set the current material
  void putPrimitive(const Primitive &prim);  // add a primitive to the scene
  // Add a box given its center and size.
  // divs: subdivisions per edge of each face (each face becomes divs x divs
  // quads; UVs of the intermediate points are interpolated).
  void putCube(const vec3f &center, const vec3f &size, int divs = 1);

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
  // in RGB565BE or RGB444; other formats are ignored. The region is clipped to
  // the screen and to dst.
  void render(int16_t x, int16_t y, int16_t w, int16_t h, const Surface &dst,
              int16_t dstX = 0, int16_t dstY = 0);

  // Get statistics (call after endRender() to get the values of that frame).
  Stats getStats() const;

  int16_t screenWidth() const { return screenW_; }
  int16_t screenHeight() const { return screenH_; }
  bool isInitialized() const { return tris_ != nullptr; }

 private:
  int16_t screenW_ = 0, screenH_ = 0;

  detail::Triangle *tris_ = nullptr;
  int triCapacity_ = 0;
  int triCount_ = 0;
  uint16_t *order_ =
      nullptr;  // triangle indices sorted by depth (farthest first)
  uint16_t *link_ = nullptr;  // per-position links (line buckets / active list)
  uint16_t *bucketHead_ =
      nullptr;  // per-scanline triangle lists (used inside render())
  uint16_t *bucketTail_ = nullptr;

  detail::Span *spanPool_ = nullptr;
  int spanCapacity_ = 0;
  int spanCount_ = 0;
  detail::Span *opaqueHead_ = nullptr;  // opaque: ascending x, non-overlapping
  detail::Span *transHead_ = nullptr,
               *transTail_ =
                   nullptr;  // translucent: insertion order (farthest first)

  detail::StackEntry *stack_ = nullptr;
  int stackTop_ = 0;

  detail::CachedVertex *vcache_ = nullptr;

  mat4f cur_ = mat4f::identity();
  const Material *curMat_ = nullptr;

  mat4f proj_ = mat4f::identity();
  float zNear_ = 0.1f;

  bool lightEnabled_ = false;
  vec3f lightDir_ = {0, 0, -1};  // view space, normalized
  colorf lightCol_ = {1, 1, 1, 1};

  bool envEnabled_ = false;
  colorf envCol_ = {0, 0, 0, 1};

  bool clearEnabled_ = true;
  colorf clearColor_ = {0, 0, 0, 1};

  size_t arenaSize_ = 0;
  size_t arenaFixed_ =
      0;  // bytes always in use (line buckets, stack, vertex cache)
  int triDropped_ = 0;
  int spanPeak_ = 0;
  int spanDropped_ = 0;

  void shadeVertex(const Vertex &in, const Material *mat, const Texture *tex,
                   detail::CachedVertex &out) const;
  void emitTriangle(const detail::CachedVertex &a,
                    const detail::CachedVertex &b,
                    const detail::CachedVertex &c, const Material *mat,
                    const Texture *tex);

  detail::Span *allocSpan();
  detail::Span **cutSpan(detail::Span **pp, int ox0, int ox1);
  void appendTranslucent(const detail::Span &sp, int x1);
  void insertOpaque(detail::Span &frag);
  void insertTranslucent(detail::Span &frag);
  void clipTranslucent(const detail::Span &frag);
  uint16_t mergeLists(uint16_t a, uint16_t b);
};

}  // namespace shapoco::gfx3d

#endif
