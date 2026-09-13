# ShapoGFX Specification

## Overview

ShapoGFX is a set of 2D/3D graphics libraries for embedded systems, written in
portable C++17 with no platform dependencies.

- Namespaces: `shapoco::gfx2d` (2D and shared code), `shapoco::gfx3d` (3D renderer)
- Output format: RGB565
- Low memory: no frame buffer, no Z buffer; the 3D renderer works scanline by scanline
- No dynamic allocation inside the library; working memory comes from a user-supplied arena
- Model data (vertex arrays, textures) is referenced, not copied, so it may live in flash

## Source layout

```
include/shapoco/gfx2d/   public headers shared by 2D and 3D
include/shapoco/gfx3d/   public headers of the 3D renderer
src/gfx3d/               3D renderer implementation
example/demo3d/          sample program
docs/example/demo3d/     browser viewer for the sample
```

Users include `shapoco/gfx3d/gfx3d.hpp` (which pulls in the gfx2d headers) and
compile `src/gfx3d/gfx3d.cpp`.

Header guards use the prefixes `SHAPOGFX2D_` and `SHAPOGFX3D_`. Compile-time
options use the same prefixes.

## `shapoco::gfx2d`

Header-only for now. Contains what the 3D renderer shares with a future 2D API.

### `math2d.hpp`

- `vec2f`: 2D vector (x, y) with `+`, `-`, `*` (scalar), `dot`, `lerp`
- `colorf`: color (r, g, b, a) in nominal 0..1 with `+`, `*` (color and scalar), `lerp`
- `clamp01(float)`

### `pixel.hpp` (RGB565 helpers, all `inline`)

```c++
enum class BlendMode : uint8_t {
    NONE,  // no blending (overwrite)
    ALPHA, // alpha blending
    ADD,   // additive blending
};

uint16_t makeRgb565(uint32_t r5, uint32_t g6, uint32_t b5);
uint16_t packRgb565(float r, float g, float b);   // clamped, rounded
uint16_t packRgb565(const colorf &c);
void     fillRgb565(uint16_t *dst, int n, uint16_t color);  // 32-bit writes where possible
uint16_t blendAlphaRgb565(uint16_t dst, uint16_t src, uint32_t alpha64); // alpha in 0..64
uint16_t addSaturateRgb565(uint16_t dst, uint32_t r5, uint32_t g6, uint32_t b5);
uint16_t addSaturateRgb565(uint16_t dst, uint16_t src);
int      log2Floor(int v);
```

### `texture.hpp`

```c++
struct Texture {
    int16_t width;
    int16_t height;
    const uint16_t *pixels; // RGB565, row-major
};
```

The 3D renderer additionally requires `width` and `height` to be powers of two.

## `shapoco::gfx3d`

### Coordinate system

OpenGL-compatible right-handed coordinate system. The camera looks down -Z in
view space. Screen space has its origin at the top-left with y pointing down.
Front faces are counter-clockwise in screen space.

All angles are in radians.

### `math3d.hpp`

- Re-exports `vec2f`, `colorf`, `clamp01` and `lerp` from `gfx2d`
- `vec3f`: 3D vector with `+`, `-`, unary `-`, `*` (scalar), `dot`, `cross`, `length`, `normalize`, `lerp`
- `mat4f`: 4x4 column-major matrix (`m[col * 4 + row]`) with `identity`, `translation`,
  `rotation(angle, axis)`, `scaling`, `perspective(fovY, aspect, zNear, zFar)`,
  `orthographic(l, r, b, t, zNear, zFar)`, `operator*`, `transformPoint`,
  `transformPoint4` (also returns w), `transformDir`

### Data structures

```c++
struct Vertex {
    vec3f position;
    vec3f normal;
    vec2f uv;              // unused when environment mapping is enabled
};

struct VertexBuffer {
    uint16_t vertexCount;
    const Vertex *vertices;
};

namespace MaterialFlags {
constexpr uint32_t TEXTURE = 1u << 0;      // enable texture mapping
constexpr uint32_t ENV_MAP = 1u << 1;      // use the texture as an environment map
constexpr uint32_t DOUBLE_SIDED = 1u << 2; // disable back-face culling
}

struct Material {
    colorf diffuse;         // diffuse color; a is the opacity
    colorf ambient;         // ambient color
    const Texture *texture; // may be nullptr when unused
    BlendMode blendMode;
    uint32_t flags;         // MaterialFlags
};

enum class PrimitiveType : uint8_t { TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN };

struct Primitive {
    PrimitiveType type;
    const VertexBuffer *vertexBuffer;
    uint16_t indexCount;      // number of indices; the triangle count follows from the type
    const uint16_t *indices;
    const Material *material; // nullptr: use the material set by setMaterial()
};

struct Stats {
    size_t arenaSize;  // size of the arena passed to init()
    size_t arenaUsed;  // bytes used in the last frame (fixed part + triangles + span peak)
    int triCapacity;   // triangle buffer capacity
    int triCount;      // triangles in the current scene (after culling)
    int triDropped;    // dropped due to overflow (reset by beginScene())
    int spanCapacity;  // span pool capacity
    int spanPeak;      // maximum spans used on one scanline (reset by beginRender())
    int spanDropped;   // dropped due to overflow (reset by beginRender())
};
```

Materials whose blend mode is not `NONE` are treated as translucent: their spans
do not remove spans behind them and are composited in list order.

### `Renderer`

All state lives in a `Renderer` object. Several instances may coexist, each with
its own arena. A `Renderer` is movable but not copyable. Calling any drawing
method before `init()` (or after `deinit()`) is a no-op.

```c++
class Renderer {
public:
    // Initialize with the screen size and the working memory.
    void init(int16_t w, int16_t h, void *arena, size_t arenaSize);
    void deinit();

    void beginScene();
    void endScene();

    void loadIdentity();
    void translate(const vec3f &v);
    void translate(float x, float y, float z);
    void rotate(float angle, const vec3f &axis);
    void rotate(float angle, float x, float y, float z);
    void scale(const vec3f &v);
    void scale(float x, float y, float z);

    void pushState(); // push the current matrix and material (depth 16)
    void popState();

    void setMaterial(const Material &mat);
    void putPrimitive(const Primitive &prim);
    // Box given by center and size; each face is split into divs x divs quads
    // and the UVs of intermediate points are interpolated.
    void putCube(const vec3f &center, const vec3f &size, int divs = 1);

    // dir is transformed by the current matrix at call time.
    void enableParallelLight(const vec3f &dir, const colorf &col);
    void disableParallelLight();
    void enableEnvironmentLight(const colorf &col);
    void disableEnvironmentLight();

    void setClearColor(const colorf &col); // pixels not covered by any span

    void setPerspectiveProjection(float fovY, float aspect, float zNear, float zFar);
    void setOrthographicProjection(float left, float right, float bottom, float top,
                                   float zNear, float zFar);

    void beginRender();
    void endRender();
    // Render the region (x, y, w, h). dst points to the region's top-left pixel,
    // stride is the row pitch of dst in pixels.
    void render(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t *dst, uint32_t stride);

    Stats getStats() const;
    int16_t screenWidth() const;
    int16_t screenHeight() const;
    bool isInitialized() const;
};
```

`setMaterial()` stores a pointer; the `Material` (and any `Texture`, vertex and
index arrays) must stay valid until `endRender()`.

## Scene construction

1. Call `beginScene()`. This resets the triangle buffer, the matrix stack and the current matrix.
2. Set up the camera and lights with the matrix functions, then add primitives.
3. Call `endScene()`.

`putPrimitive()` decomposes the primitive into triangles and performs per-vertex
lighting (Gouraud shading), transformation and projection immediately. The
results are stored in the triangle buffer and rasterized later by `render()`,
which may be called several times for different regions of the screen.

Vertices shared by several triangles of one primitive (strips, fans, indexed
meshes) are transformed once thanks to a small direct-mapped vertex cache (64
entries), which is invalidated at the start of each primitive.

Triangles are discarded at this stage when:

- they cross or lie in front of the near plane (no clipping is performed);
- back-face culling is enabled for the material and they face away from the camera;
- they do not cover any scanline;
- the triangle buffer is full (counted in `Stats::triDropped`).

### Lighting

Per vertex:

```
color = ambient * environmentLight                 (if the environment light is enabled)
      + diffuse * parallelLight * max(0, n . -L)   (if the parallel light is enabled)
```

If neither light is enabled the vertex color is `diffuse`. For `BlendMode::ADD`
the color is pre-multiplied by the opacity (`diffuse.a`). Vertex colors are
interpolated linearly across each span and, when a texture is present, modulate
the texel.

### Environment mapping

With `ENV_MAP`, the texture coordinate is derived from the view-space normal `n`:
`u = 0.5 + 0.5 n.x`, `v = 0.5 - 0.5 n.y`. The top half of the texture therefore
appears on surfaces facing up.

## Memory management

`init()` aligns the arena to 8 bytes and carves it as follows:

1. **Fixed part**: line buckets (2 x screen height x `uint16_t`), matrix stack
   (16 entries), vertex cache (64 entries).
2. **Span pool**: a quarter of the remaining space, clamped to 32..512 spans.
3. **Triangle buffer**: everything that remains, including 4 bytes per triangle
   for the sort order and link arrays.

If the arena is too small for the fixed part, `init()` leaves the renderer
uninitialized. Overflowing buffers drop the excess for the current frame.

## Rendering pipeline

### `beginRender()`

Sorts the triangle indices (not the triangles) farthest first by their average
view-space z. Ordering between opaque spans is resolved by depth comparison in
`render()`, so this sort primarily determines the compositing order of
translucent triangles.

### `render()`

At the start of the call, for every scanline of the region, a list of the
triangles that start intersecting on that line is built (in depth order). For
each scanline this list is merged into the active list (triangles crossing the
current line, in depth order), triangles that have been passed are removed, and
then:

1. The span lists are cleared.
2. For each active triangle, farthest first:
    1. The two intersections of the triangle with the scanline give a span,
       represented as "attribute values at the leftmost pixel + per-pixel
       increments" (depth as `float`; color and texture coordinates as 16.16
       fixed point).
    2. The span is inserted. Opaque spans are kept in a list sorted by x that
       never overlaps; translucent spans are kept in a separate list in insertion
       order (farthest first). On overlap, the depth (NDC depth, linear in screen
       space) is compared at the center of the overlapping interval. If the new
       span is nearer and opaque, the overlapping part of the farther span is
       removed whether it is opaque or translucent. If a translucent span is
       nearer, both are kept. Because this does not rely on the per-triangle sort
       order alone, large and small polygons are ordered correctly.
3. The opaque spans are rasterized in x order with the gaps filled in the clear
   color; then the translucent spans are composited in list order according to
   their material's `BlendMode`.

Per-pixel processing is integer only. The inner loop is specialized for each
combination of blend mode x textured x flat (all three vertex colors equal); a
flat, opaque, untextured span degenerates to a plain fill. Texture coordinates
wrap with a bit mask, hence the power-of-two requirement. Additive blending
uses colors pre-multiplied by the opacity at the vertex stage.

Vertex colors are always interpolated affinely. The interpolation of texture
coordinates is selected at compile time of `gfx3d.cpp` with
`SHAPOGFX3D_CORRECT_PERSPECTIVE` (default 1):

- **0**: affine everywhere. Texture coordinates are interpolated linearly along
  the edges and across the span.
- **1** (default): vertical correction. `(u/w, v/w, 1/w)`, which are linear in
  screen space, are interpolated along the edges; at the two end points of each
  span they are divided to obtain exact `(u, v)`, and the span interior is
  interpolated affinely in fixed point. Costs two `float` divides per span and 12
  bytes per triangle. Along a scanline, a horizontal surface seen by a camera
  without roll has constant depth, so this level renders such surfaces without
  distortion; surfaces whose depth varies along the scanline keep affine
  distortion inside each span, while span end points (and therefore edges shared
  between triangles) are exact.
- **2**: full correction. `(u/w, v/w, 1/w)` are interpolated across the span in
  `float` and divided per pixel. Costs one divide per textured pixel, 12 bytes per
  triangle and 8 bytes per span.

### `endRender()`

Currently does nothing; reserved for future use.

## Sample program

`example/demo3d/` is a browser and native sample at a fixed resolution of 480x320.

- `scene.hpp` / `scene.cpp`: builds a scene using the library (a textured floor,
  an environment-mapped torus built as a `TRIANGLE_STRIP`, and opaque,
  alpha-blended and additive cubes).
- `main.cpp`: holds the frame buffer and the arena. Compiled with Emscripten it
  exports `demo3d_init`, `demo3d_frame`, `demo3d_get_fb`, `demo3d_get_width` and
  `demo3d_get_height`; compiled natively it writes one frame to a PPM file.
- `Makefile`: Emscripten build into `docs/example/demo3d/demo3d.wasm`. The
  sample is built with the library's default `SHAPOGFX3D_CORRECT_PERSPECTIVE`;
  the floor is deliberately left unsubdivided to show the effect.
- `docs/example/demo3d/index.html`, `main.js`: loads the WASM module, converts the
  RGB565 frame buffer to RGBA and draws it on a canvas. Mouse and keyboard control
  the camera.

The WASM binary is committed so that `docs/` can be served as a static site.
