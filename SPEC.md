# ShapoGFX Specification

## Overview

ShapoGFX is a set of 2D/3D graphics libraries for embedded systems, written in
portable C++17 with no platform dependencies.

- Namespaces: `shapoco::gfx2d` (2D API and shared types), `shapoco::gfx3d` (3D renderer)
- Pixel formats: GRAY1, RGB444, ARGB4444, RGB565BE (see below)
- Low memory: no frame buffer, no Z buffer; the 3D renderer works scanline by scanline
- No dynamic allocation inside the library; the 3D renderer's working memory comes from
  a user-supplied arena, the 2D API needs none
- Image data (vertex arrays, textures, fonts) is referenced, not copied, so it may live in flash

## Source layout

```
include/shapoco/gfx2d/   2D API and shared types
include/shapoco/gfx3d/   3D renderer
src/gfx2d/, src/gfx3d/   implementation
src/gfx3d/arch/          architecture hooks of the 3D renderer (internal; generic + RP2)
example/wasm/            sample programs (WASM and native)
docs/example/            browser pages for the samples
test/                    self-checking tests
```

Users include `shapoco/gfx2d/gfx2d.hpp` and/or `shapoco/gfx3d/gfx3d.hpp` and compile
`src/gfx2d/*.cpp` and `src/gfx3d/*.cpp`. Header guards and compile-time options use
the prefixes `SHAPOGFX_` (shared), `SHAPOGFX2D_` and `SHAPOGFX3D_`.

## Compile-time configuration (`config.hpp`)

| Macro | Default | Effect |
|---|---|---|
| `SHAPOGFX_FORMAT_GRAY1` | 1 | Enable the GRAY1 format |
| `SHAPOGFX_FORMAT_RGB444` | 1 | Enable the RGB444 format |
| `SHAPOGFX_FORMAT_ARGB4444` | 1 | Enable the ARGB4444 format |
| `SHAPOGFX_FORMAT_RGB565BE` | 1 | Enable the RGB565BE format |
| `SHAPOGFX_COORD_BITS` | 11 | Bits of a screen coordinate and of a surface's width and height (1..15). Wider or taller surfaces are rejected (see below) |
| `SHAPOGFX3D_CORRECT_PERSPECTIVE` | 1 | Perspective correction level of the 3D renderer (0/1/2) |
| `SHAPOGFX3D_PERSPECTIVE_STEP` | 16 | Level 2: pixels between two exact evaluations of the texture coordinates (power of two) |
| `SHAPOGFX3D_RP2_INTERP` | 1 on RP2, else 0 | RP2040/RP2350 (Pico SDK): fetch 16-bit texels through the SIO interpolator `interp0`. On by default when the target is detected as RP2 (`PICO_RP2040` / `PICO_RP2350`) and `hardware/interp.h` is on the include path; 0 turns it off |
| `SHAPOGFX3D_HOT_ATTR` | (empty) | Attribute put on the rasterization side (`render()` and the per-span functions, ~14 KB on Cortex-M0+), e.g. `__attribute__((section(".time_critical.gfx3d")))` to run it from RAM on the Pico SDK |
| `SHAPOGFX3D_HOT_INSTANTIATE` | 0 | 1 also instantiates the per-span function templates explicitly with `SHAPOGFX3D_HOT_ATTR` (GCC ignores a section attribute on a template otherwise) |
| `SHAPOGFX3D_FIXED_POINT` | 0 | Vertex stage, primitive setup and span builders in fixed point, for cores without an FPU (see "Fixed-point vertex stage") |
| `SHAPOGFX3D_TEXTURE` | 1 | Texture and environment mapping |
| `SHAPOGFX3D_GOURAUD` | 1 | Gouraud shading; 0 selects flat shading |
| `SHAPOGFX3D_BLEND` | 1 | Translucency |
| `SHAPOGFX3D_LINES` | 1 | `LINES` / `LINE_STRIP` / `LINE_LOOP` primitives |
| `SHAPOGFX3D_POINTS` | 1 | `POINTS` primitives |
| `SHAPOGFX3D_STACK_DEPTH` | 16 | Levels of the matrix stack (`pushState()`) |
| `SHAPOGFX3D_VCACHE_SIZE` | 64 | Entries of the vertex cache (power of two) |
| `SHAPOGFX3D_LAYER_MAX` | 8 | Layers a scene can hold (1..128) |

Every macro below `SHAPOGFX3D_` is read by `src/gfx3d/*.cpp` only and changes no
public type, so translation units cannot disagree about them. `SHAPOGFX_COORD_BITS`
lives in `config.hpp` like the format macros and must have the same value in every
translation unit (the CMake option passes it on as a public definition).

`SHAPOGFX_COORD_BITS` bounds what the renderers have to handle: a surface or a 3D
screen wider or taller than `SHAPOGFX_COORD_MAX` (`2^bits - 1`) pixels is rejected
-- `Graphics2D::setTarget()` leaves the context without a target, like a surface in
a disabled format, and `Graphics3D::init()` leaves the renderer uninitialized.
Within the limit every screen coordinate fits 16 bits and every product of two
coordinate differences 32 bits, which is what lets the 2D line and polygon walkers
and the 3D span stage do without 64-bit arithmetic. The public types (`int`,
`Rect`, `vec2i`, `Surface::width`) do not change.

The 3D renderer's architecture-specific code lives in `src/gfx3d/arch/`, behind a
few hooks with a portable implementation that is always present (`generic.hpp`);
`arch.hpp` detects the target (`SHAPOGFX_ARCH_RP2`, `SHAPOGFX_ARCH_ESP32S3`,
`SHAPOGFX_ARCH_ESP32P4`, else `SHAPOGFX_ARCH_GENERIC`; any of them may be defined
by hand) and selects the implementation. With `SHAPOGFX3D_RP2_INTERP` the target
must link `hardware_interp`; `render()` saves and restores `interp0` of the calling
core, so interrupt handlers running during `render()` must not use it.

### Optional features of the 3D renderer

Turning a feature off removes its code and shrinks the per-triangle and per-span
working memory, so the same arena holds more geometry. The members it covers are
then ignored at run time, exactly like a surface in a disabled pixel format, so
scene data written for the full renderer still compiles and draws.

| Off | Effect on the picture |
|---|---|
| `SHAPOGFX3D_TEXTURE` | `Material::texture` and the `TEXTURE` / `ENV_MAP` flags are ignored; materials draw in their plain lit color |
| `SHAPOGFX3D_GOURAUD` | A primitive takes the color of its first vertex instead of interpolating, so smoothly shaded surfaces become faceted |
| `SHAPOGFX3D_BLEND` | Everything is drawn opaque; the blend mode and the alpha of an ARGB4444 texture are ignored |
| `SHAPOGFX3D_LINES` | Line primitives draw nothing, and so do `putLine()` and `putWireCube()` |
| `SHAPOGFX3D_POINTS` | Point primitives draw nothing |

A primitive is stored in the record layout it needs, so turning a feature off is
not the only way to shrink it: a flat or untextured primitive of a full build
costs no more than one of a build without the feature. Bytes per record on a
32-bit target at the default perspective level, including the 4 bytes of entry
(record offset and scanline link):

| Record | Bytes | Selected by |
|---|---|---|
| header only | 48 | — |
| + depth plane | 60 | a layer without `LayerFlags::NO_DEPTH` |
| + interpolated color | 96 | three differing vertex colors (`SHAPOGFX3D_GOURAUD`) |
| + texture coordinates | 100 | a textured material (`SHAPOGFX3D_TEXTURE`) |
| all of them | 132 | |

Turning a feature off removes the corresponding layouts and their code. A span
is 16 bytes in every configuration -- its pixel range, its layer and pointers to
its record and to the next span -- because its attributes are evaluated from the
record when it is drawn; the pool holds `Config::spanCapacity` of them.

Disabling a format removes its code from both renderers: the pixel cursors, the 2D
per-format row operations, the 3D texture samplers and (for output formats) the 3D
rasterizer table. Surfaces or textures in a disabled format are ignored at run time.
The format macros must have the same values in every translation unit.

## `shapoco::gfx2d`

### Pixel formats (`pixel.hpp`)

```c++
enum class PixelFormat : uint8_t { GRAY1, RGB444, ARGB4444, RGB565BE };
```

| Format | Bits/pixel | Memory layout | Native pixel (in registers) |
|---|---|---|---|
| `GRAY1` | 1 | MSB first within a byte; 1 = white | 0 or 1 |
| `RGB444` | 12 | 2 pixels in 3 bytes: `R1G1`, `B1R2`, `G2B2` (display order) | `0x0RGB` |
| `ARGB4444` | 16 | native `uint16_t` | `0xARGB`; A = 15 opaque |
| `RGB565BE` | 16 | `uint16_t` stored byte-swapped: byte 0 = `RRRRRGGG`, byte 1 = `GGGBBBBB` | `RRRRRGGGGGGBBBBB` (5/6/5) |

Every row of an image starts on a byte boundary; rows are `stride` bytes apart
(`minStride(format, width)` gives the smallest legal stride).

RGB565BE and RGB444 are the byte streams expected by common display controllers, so
a Surface in either format can be transferred without conversion. ARGB4444 is a
composition format (sprites with alpha), GRAY1 a mask/monochrome format.

### Colors

`Color` is `uint32_t` ARGB8888. It is the only color type of the 2D API and is
converted to the target format once per drawing call. Helpers: `makeColor(r, g, b,
a = 255)`, `makeColorF(...)`, `makeColorHsv(h, s, v, a)`, `colorWithAlpha`,
`lerpColor`, component accessors, the `Colors::` constants, and `colorToNative` /
`nativeToColor` for any format.

Per-format helpers (all `inline`): pack/unpack (`makeRgb565`, `packRgb565`,
`packRgb565BE`, `colorToRgb565`, `rgb565ToColor`, the same for RGB444 and ARGB4444,
`colorToGray1`), alpha blending with a 0..64 opacity (`blendAlphaRgb565`,
`blendAlphaRgb444`, `blendAlphaArgb4444`), saturating addition (`addSaturate...`),
`fill16`, `bswap16`, `log2Floor`.

### Pixel cursors

`CursorGray1`, `CursorRgb444`, `CursorArgb4444`, `CursorRgb565BE` give sequential
access to one row: `init(line, x)`, `read()`, `write(native)`, `next()`,
`fill(n, native)`. `FormatTraits<F>` maps a format to its cursor and its Color
conversions; `blendNative<F>` and `addNative<F>` blend native pixels. These are the
building blocks of both renderers and are available to applications.

### Surfaces (`surface.hpp`)

```c++
struct Texture {            // read-only image
  PixelFormat format;
  int16_t width, height;
  uint32_t stride;          // bytes per row
  const void *pixels;
};
struct Surface {            // writable image; converts implicitly to Texture
  PixelFormat format;
  int16_t width, height;
  uint32_t stride;
  void *pixels;
};
Texture makeTexture(PixelFormat, int w, int h, const void *pixels, uint32_t stride = 0);
Surface makeSurface(PixelFormat, int w, int h, void *pixels, uint32_t stride = 0);
size_t surfaceBytes(PixelFormat, int w, int h);
```

Both are aggregates and can be `constexpr`/`const` data in flash.

### `OwnedSurface` (`surface_alloc.hpp`, optional)

A movable, non-copyable object holding a zero-initialized heap buffer in a
`std::unique_ptr<uint8_t[]>` together with the matching `Surface`.
`createSurface(format, w, h)` constructs one. This is the only place in the library
that allocates; the core headers do not include `<memory>`.

### `Graphics2D` (`graphics2d.hpp`)

A drawing context bound to a `Surface` (a copy of the struct; the pixel buffer must
outlive the calls). All drawing is clipped to the clip rectangle. Colors are
`Color`; alpha 0 draws nothing, 255 overwrites, anything else blends.

```c++
class Graphics2D {
 public:
  Graphics2D();  explicit Graphics2D(const Surface &target);
  void setTarget(const Surface &); const Surface &target() const; bool hasTarget() const;
  PixelFormat format() const; Rect bounds() const;

  // state
  void setClipRect(const Rect &); void setClipRect(int x, int y, int w, int h);
  void resetClipRect(); const Rect &clipRect() const;
  const GraphicsState2D &state() const; void setState(const GraphicsState2D &);

  // pixels and rectangles
  void clear(Color);                          // fills the clip rectangle
  void setPixel(int x, int y, Color); Color getPixel(int x, int y) const;
  void fillRect(const Rect &, Color);         void fillRect(int x, int y, int w, int h, Color);
  void fillRect(const Rect &, Color, BlendMode, int opacity = 255);  // NONE / ALPHA / ADD (additive, saturating)
  void drawRect(const Rect &, Color, int thickness = 1);
  void fillRoundRect(const Rect &, int radius, Color); void drawRoundRect(const Rect &, int radius, Color);
  void drawHLine(int x, int y, int w, Color); void drawVLine(int x, int y, int h, Color);

  // ellipses (inscribed in the rectangle)
  void fillEllipse(const Rect &, Color); void drawEllipse(const Rect &, Color);
  void fillCircle(int cx, int cy, int r, Color); void drawCircle(int cx, int cy, int r, Color);

  // lines and polygons
  void drawLine(int x0, int y0, int x1, int y1, Color);
  void drawPolyline(const vec2i *, int n, Color); void drawPolygon(const vec2i *, int n, Color);
  void fillPolygon(const vec2i *, int n, Color);   // even-odd rule, <= 16 crossings per row
  void fillTriangle(...); void drawTriangle(...);

  // images
  void drawImage(const Texture &, int dx, int dy, BlendMode = ALPHA, int opacity = 255);
  void drawImage(const Texture &, int dx, int dy, const Rect &src, BlendMode = ALPHA, int opacity = 255);
  void drawBitmap(const Texture &gray1, int dx, int dy, Color fg, Color bg = TRANSPARENT);
  void drawBitmap(const Texture &gray1, int dx, int dy, const Rect &src, Color fg, Color bg = TRANSPARENT);

  // text
  void setFont(const GFXfont *, int scale = 1); void setTextScale(int);
  void setTextColor(Color fg, Color bg = TRANSPARENT);
  void setCursor(int x, int y); vec2i cursor() const;
  int drawChar(int x, int y, int code);        // returns the scaled x advance
  void drawString(const char *); void drawString(int x, int y, const char *);
  int measureText(const char *) const; int charAdvance(int code) const;
  int textHeight() const; int lineAdvance() const;
};
```

Semantics:

- **Rectangles** are half-open (`[x, x + w)`); negative sizes are normalized.
  `drawRect` draws inside the rectangle.
- **Ellipses and rounded rectangles** are described by the horizontal extent of each
  row (computed with one square root per row). An outline row runs, on each side,
  from that row's own end inwards to just short of the nearer of the two neighboring
  rows' ends on that side, which yields a closed one-pixel outline consistent with
  the fill. Reaching to the *nearer* neighbor is what closes it where the edge is
  nearly flat -- the top and bottom of a circle, where consecutive rows' ends are
  many columns apart and the end pixels alone would leave a dotted line. Where the
  edge is steep the neighbors are one column away and the row is its end pixels, as
  before.
- **Lines** walk the major axis with a 16.16 fixed-point minor coordinate and are
  clipped along the major axis before stepping; runs of pixels on the same row are
  filled as spans. Both end points are drawn. The walk is 32-bit only: it works
  relative to the center of the clip rectangle, within +-16383 pixels of it, and a
  line reaching further is halved (split points rounded to whole pixels) until its
  parts either miss the clip rectangle or fit.
- **Polygons** are filled per scanline with the even-odd rule using the same
  half-open convention as Xiamocon-style rasterizers (an edge covers `y` when
  `y0 <= y < y1`). Vertices are taken relative to the center of the clip rectangle
  and clamped to +-16383 pixels of it, so that the crossings are computed in 32
  bits; only a vertex that far off screen moves.
- **drawImage** converts between formats. `BlendMode::NONE` copies (ARGB4444 alpha is
  copied into an ARGB4444 target and ignored otherwise); `ALPHA` blends with the
  source alpha (only ARGB4444 has one; other formats are copied unless `opacity` is
  below 255); `ADD` adds the color scaled by alpha x opacity with saturation. Same
  format 16-bit copies use `memcpy`; the other `NONE` and `ALPHA` combinations use
  per-pair row templates converting through RGB565 (lossless for every color depth;
  GRAY1 targets keep the `Color` luminance threshold); `ADD` goes through `Color` in
  chunks of 64 pixels on the stack.
- **drawBitmap** renders a GRAY1 image as a two-color mask; runs of equal bits become
  spans. A transparent background leaves clear bits untouched.
- **Text** uses Adafruit `GFXfont` data. `setFont` computes the ascent (largest height
  above the baseline) and the line box height over all glyphs; the cursor is the
  top-left corner of the line box and glyphs are placed relative to the baseline
  `ascent x scale` pixels below it. `background` (if not transparent) fills the box
  `xAdvance x lineHeight` of each glyph before drawing it. `'\n'` returns to the x of
  the last `setCursor()` and advances by `yAdvance x scale`. Glyphs are rendered by a
  per-format template: at scale 1 the set bits are written through the row cursor, at
  larger scales runs of set bits become `scale x scale` blocks; all formats and alpha
  work.

Every drawing function switches on the target format once per call (or per row),
never per pixel; the per-pixel loops are instantiated per format from the cursor
templates.

### Fonts (`fonts.hpp`, `gfxfont.h`, `font/*.h`)

`gfxfont.h` is the Adafruit GFXfont structure (BSD license, see LICENSE). The bundled
ShapoSans fonts (generated with ShapoFont) are `const GFXfont` objects in the global
namespace: `ShapoSansMono_s08c07`, `ShapoSansP_s08c07`, `ShapoSansP_s12c09a01w02`,
`ShapoSansP_s21c16a01w03`. Any GFXfont from the Adafruit ecosystem can be used.

### Geometry (`math2d.hpp`)

`vec2f`, `colorf` (float RGBA used by the 3D API), `vec2i`, `Rect` (with `right()`,
`bottom()`, `contains`, `normalized`, `intersect`, `offset`), `clamp01`, `clampInt`,
`lerp`.

## `shapoco::gfx3d`

### Coordinate system

OpenGL-compatible right-handed coordinate system. The camera looks down -Z in view
space. Screen space has its origin at the top-left with y pointing down. Front faces
are counter-clockwise in screen space. All angles are in radians.

### `math3d.hpp`

- Re-exports `vec2f`, `colorf`, `clamp01` and `lerp` from `gfx2d`
- `vec3f` with `+`, `-`, unary `-`, `*` (scalar), `dot`, `cross`, `length`, `normalize`, `lerp`
- `mat4f`: 4x4 column-major matrix with `identity`, `translation`, `rotation(angle, axis)`,
  `scaling`, `perspective(fovY, aspect, zNear, zFar)`, `orthographic(l, r, b, t, zNear, zFar)`,
  `operator*`, `transformPoint`, `transformPoint4` (also returns w), `transformDir`

### Data structures

```c++
using gfx2d::Texture;   // any enabled format; width and height must be powers of two
using gfx2d::Surface;   // render target: RGB565BE or RGB444

struct Vertex {
  vec3f position; vec3f normal; vec2f uv;
  gfx2d::Color color;   // ARGB8888, used with MaterialFlags::VERTEX_COLOR (alpha ignored)
};
struct PackedVertex {      // 16-byte vertex for models in flash
  int16_t position[3];    // x VertexBuffer::scale + VertexBuffer::bias
  int16_t uv[2];          // 1/1024 texel-space units (range -32..32)
  int8_t normal[3];       // 1/127 units
  uint8_t color[3];       // R, G, B
};
struct VertexBuffer {
  uint16_t vertexCount;
  const Vertex *vertices;                // nullptr: the buffer is packed
  const PackedVertex *packed = nullptr;  // used when `vertices` is nullptr
  vec3f scale = {1, 1, 1};               // packed position scale
  vec3f bias = {0, 0, 0};                // packed position offset
};

namespace MaterialFlags {
constexpr uint32_t TEXTURE = 1u << 0;       // enable texture mapping
constexpr uint32_t ENV_MAP = 1u << 1;       // use the texture as an environment map
constexpr uint32_t DOUBLE_SIDED = 1u << 2;  // disable back-face culling
constexpr uint32_t VERTEX_COLOR = 1u << 3;  // multiply the lit color by Vertex::color
}

struct Material {
  colorf diffuse;          // diffuse color; a is the opacity
  colorf ambient;          // ambient color
  const Texture *texture;  // may be nullptr when unused
  BlendMode blendMode;     // NONE, ALPHA, ADD
  uint32_t flags;          // MaterialFlags
};

enum class PrimitiveType : uint8_t {
  TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN,   // lit, textured, culled
  POINTS, LINES, LINE_STRIP, LINE_LOOP       // unlit, 1 px (points: pointSize), not culled
};

struct Primitive {
  PrimitiveType type;
  const VertexBuffer *vertexBuffer;      // either vertex form
  uint16_t indexCount;
  const uint16_t *indices;
  const Material *material;  // nullptr: use the material set by setMaterial()
};

struct Config {                  // from defaultConfig(), adjusted as needed
  int16_t screenWidth = 0, screenHeight = 0;
  void *arena = nullptr; size_t arenaSize = 0;
  int spanCapacity = 0;         // 0: a quarter of the arena left, clamped to 32..512
};
Config defaultConfig(int16_t w, int16_t h, void *arena, size_t arenaSize);

namespace LayerFlags {
constexpr uint32_t NO_DEPTH = 1u << 0;  // ordered by insertion, no depth plane stored
}

struct Stats {
  size_t arenaSize, arenaUsed;
  size_t triBytes, triBytesTotal;  // triangle buffer in use / available
  int triCount, triDropped;
  int layerCount, layersDropped;
  int spanCapacity, spanPeak, spanDropped;
  int badIndices;    // triangles dropped because an index was >= vertexCount
  int nodesDropped;  // nodes skipped because the state stack was full
};

// Static scene description (see "Static scenes")
struct Mesh  { const Primitive *primitives; uint16_t primitiveCount; };
struct Node  { const char *name; mat4f transform; const Mesh *mesh;
               const Node *const *children; uint16_t childCount; };
struct Scene { const Node *const *roots; uint16_t rootCount; };
class NodeVisitor { public: virtual bool onNode(const Node &, mat4f &local); };
```

Vertex forms: a `VertexBuffer` holds 36-byte `Vertex`, 16-byte `PackedVertex`
or 24-byte `FixedVertex` data (position 16.16, normal Q15, uv in 1/1024 units,
color). The fixed form is what the fixed-point vertex stage reads with no
conversion at all, for an application that already computes its geometry in
integers; the float stage converts it, so a scene may use it on every target. A packed position is an integer scaled by the buffer's
`scale` and offset by its `bias`, so its resolution is 1/65534 of the model's
extent along each axis; uv is in 1/1024 texel-space units and the normal in
1/127 units, which makes the decoded vector unit length to about 1%. Both errors
are well below the resolution of the output formats. A packed vertex is decoded
once per vertex and primitive (the vertex cache absorbs the cost), so the saving
is in flash: `bin/gltf2cpp --vertex-format packed` emits this form.

`VertexBuffer` itself carries the packed pointer and the scale and bias whichever
form it points at, which makes it 36 bytes on a 32-bit target instead of the 8 a
pointer and a count would need. A buffer of plain `Vertex` therefore pays 28
bytes it did not before, while packing saves 20 bytes per vertex; packing wins
from the second vertex of a primitive on, but a model split into many primitives
of very few vertices each gains little.

Translucency: a triangle is translucent when its material's blend mode is not
`NONE` or when its texture is ARGB4444. Translucent spans do not remove spans behind
them and are composited in list order.

### `Graphics3D`

All state lives in a `Graphics3D` object; several may coexist, each with its own arena.
A `Graphics3D` is movable but not copyable. Drawing methods called before `init()` (or
after `deinit()`) are no-ops.

```c++
class Graphics3D {
 public:
  void init(const Config &);
  void init(int16_t w, int16_t h, void *arena, size_t arenaSize);  // default Config
  void deinit();

  void beginScene(); void endScene();
  void beginLayer(uint32_t flags = 0); void endLayer();   // see Layers
  void loadIdentity();
  void translate(const vec3f &); void translate(float x, float y, float z);
  void rotate(float angle, const vec3f &axis); void rotate(float angle, float x, float y, float z);
  void scale(const vec3f &); void scale(float x, float y, float z);
  void transform(const mat4f &);                       // multiply by an arbitrary matrix
  void lookAt(const vec3f &eye, const vec3f &target, const vec3f &up = {0, 1, 0});
  bool pushState(); void popState();                  // matrix + material, depth 16; false when full

  void setMaterial(const Material &);
  void putPrimitive(const Primitive &);
  void setPointSize(int pixels);        // size of POINTS (square), 1..64, default 1
  void setDepthBias(float bias);        // added to the NDC depth of later primitives (default 0)

  // shapes (see below)
  void putCube(const vec3f &center, const vec3f &size, int divs = 1);
  void putPlane(const vec3f &center, float sizeX, float sizeZ, int divsX = 1, int divsZ = 1);
  void putDisk(const vec3f &center, float radius, int segments = 16);
  void putSphereUV(const vec3f &center, float radius, int segmentsU = 16, int segmentsV = 8);
  void putIcosphere(const vec3f &center, float radius, int level = 2);
  void putCylinder(const vec3f &center, float radius, float height, int segments = 16,
                   int heightDivs = 1, bool caps = true);
  void putCone(const vec3f &center, float radiusBottom, float radiusTop, float height,
               int segments = 16, int heightDivs = 1, bool caps = true);
  void putTorus(const vec3f &center, float majorRadius, float minorRadius,
                int majorSegments = 24, int minorSegments = 12);
  void putLine(const vec3f &a, const vec3f &b);           // unlit segment
  void putWireCube(const vec3f &center, const vec3f &size); // 12 edges

  // static scenes (see below)
  void putMesh(const Mesh &);
  void putNode(const Node &, NodeVisitor *visitor = nullptr);
  void putScene(const Scene &, NodeVisitor *visitor = nullptr);

  void enableParallelLight(const vec3f &dir, const colorf &col);  // dir transformed by the current matrix
  void disableParallelLight();
  void enableEnvironmentLight(const colorf &col); void disableEnvironmentLight();

  void setClearColor(const colorf &);   // background for uncovered pixels (enables clearing)
  void disableClear();                  // uncovered pixels keep the target content
  bool isClearEnabled() const;

  void setPerspectiveProjection(float fovY, float aspect, float zNear, float zFar);
  void setOrthographicProjection(float l, float r, float b, float t, float zNear, float zFar);

  void beginRender(); void endRender();
  // Render the screen region (x, y, w, h) into dst at (dstX, dstY); clipped to both
  void render(int16_t x, int16_t y, int16_t w, int16_t h, const Surface &dst,
              int16_t dstX = 0, int16_t dstY = 0);

  Stats getStats() const;
  int16_t screenWidth() const; int16_t screenHeight() const; bool isInitialized() const;
};
```

`setMaterial()` stores a pointer; materials, textures, vertex and index arrays must
stay valid until `endRender()`.

## Scene construction

1. `beginScene()` resets the triangle buffer, the layers, the matrix stack and the
   current matrix.
2. Set up the camera and lights with the matrix functions, then add primitives.
3. `endScene()`.

### Layers

A scene is a sequence of layers, and **every layer is drawn in front of the layers
opened before it**; the application guarantees this by adding its geometry back to
front. Inside a layer the usual depth resolution applies. A scene that never calls
`beginLayer()` is a single layer and behaves exactly as one that predates them.

- `beginLayer(flags)` closes the current layer and opens a new one. The layer is
  created when its first primitive arrives, so an empty one costs nothing. When
  `SHAPOGFX3D_LAYER_MAX` layers are already in use the call is ignored and counted
  in `Stats::layersDropped`; the primitives stay in the current layer.
- `endLayer()` closes the current layer. What follows goes into a new layer with
  the default flags, still in front of everything before it.
- `LayerFlags::NO_DEPTH`: the layer carries no depth at all. Its primitives are
  drawn in the order they were added (the later one wins), its records hold no
  depth plane (12 bytes less each) and it is not sorted. Use it for geometry that
  is already ordered back to front; `setDepthBias()` has no effect in it.

Layer order is resolved where spans meet (see `render()`), so it also settles cases
the depth comparison cannot, such as a polygon that spans a large depth range
crossing another one.

`putPrimitive()` decomposes the primitive into triangles and performs per-vertex
lighting (Gouraud shading), transformation and projection immediately. The results
are stored in the triangle buffer and rasterized later by `render()`, which may be
called several times for different regions and targets.

Vertices shared by several triangles of one primitive are transformed once thanks to
a direct-mapped vertex cache (64 entries) invalidated at the start of each primitive.

Triangles are discarded at this stage when they cross or lie in front of the near
plane (no clipping), when back-face culling applies, when they cover no scanline, when
the triangle buffer has no room left for their record (`Stats::triDropped`), or when
one of their indices is outside the vertex buffer (`Stats::badIndices`). The index check makes it safe to draw
data of unverified origin.

### Shapes

The shape functions generate geometry on the fly and feed it to `putPrimitive()`:
parametric surfaces (plane, sphere, cylinder, cone, torus) are emitted as
`TRIANGLE_STRIP`s one band at a time in chunks of 16 segments from a stack buffer of
34 vertices; disks and caps are `TRIANGLE_FAN`s; the icosphere emits one strip per row
of each subdivided icosahedron face. Sines and cosines are tabulated once per call, so
regenerating a shape every frame costs little more than drawing a stored mesh (the
vertices shared between chunks are shaded twice). Segment counts are clamped to
3..64 (subdivisions to 1..64, icosphere level to 0..4).

Conventions: shapes are centered at `center` with their axis along +Y, normals point
outward and front faces are counter-clockwise, so single-sided materials show the
outside. UVs: sphere u = longitude around +Y starting at +X towards +Z, v = 0 at the
north pole; the icosphere uses the same equirectangular mapping with the seam and the
poles handled per face; cylinder/cone side u = around the axis, v = 0 at the top; plane
u along +X, v along +Z; disk u/v = bounding square; torus u = around the ring, v =
around the tube. All shape vertices are white (`VERTEX_WHITE`).

### Points and lines

`POINTS`, `LINES`, `LINE_STRIP` and `LINE_LOOP` are processed without lighting,
texturing or culling: the vertex color is `diffuse` (times `Vertex::color` with
`VERTEX_COLOR`, pre-multiplied by the opacity for `ADD`). Lines are clipped against the
near plane in view space (a segment with one end behind the plane is shortened; the
color is interpolated at the cut); points behind it are dropped. Both are stored in the
triangle buffer (one record each, counted in `Stats::triCount`; never textured, so
they use one of the smaller layouts) and turned into spans by `render()`:

- A line covers, on each pixel row it crosses, either the single pixel at the row center
  (steep lines) or the run of columns whose centers map into that row (shallow lines), so
  the coverage is that of a Bresenham line with both end points drawn. Depth and color are
  interpolated along the run.
- A point is an axis-aligned square of `pointSize()` pixels centered on the projected
  position.

Their spans go through the same opaque / translucent lists as triangle spans, so lines are
hidden by nearer surfaces (hidden-line removal) and blended with `ALPHA` / `ADD` like any
other primitive. Line width is fixed at one pixel.

### Depth bias

`setDepthBias(bias)` adds `bias` to the NDC depth (range -1..1) of every primitive emitted
afterwards; negative values bring them nearer. A layer with `LayerFlags::NO_DEPTH`
stores no depth, so the bias does nothing there. Its purpose is drawing wireframes or
markers on top of coplanar polygons without z-fighting (e.g. `-0.002`). It applies to
triangles as well.

### Vertex colors

With `MaterialFlags::VERTEX_COLOR` the lit color (ambient + diffuse terms, or the plain
diffuse color without lights) is multiplied by `Vertex::color` (RGB, 0..255). The alpha
of the vertex color is ignored; translucency is per material.

### Static scenes

`Mesh`, `Node` and `Scene` describe a model as plain aggregates so that a whole model
(vertices, indices, textures, materials, nodes) can be `static const` data in flash,
typically generated by `bin/gltf2cpp`. `putMesh()` draws every primitive with its own
material. `putNode()` pushes the state, multiplies the current matrix by the node's
transform, draws its mesh, recurses into the children and pops; `putScene()` does this
for every root. The optional `NodeVisitor` is called before each node with a copy of
its local transform: it may modify the transform (animation) or return `false` to skip
the node and its subtree. When the state stack (16 levels) is full the subtree is
skipped and counted in `Stats::nodesDropped`, so deep or malformed trees cannot corrupt
the matrix stack.

Memory safety of scene data rests on three points: all references are to static
storage (nothing is owned or freed), every array is paired with its count and the
traversal never reads past it, and the index check in `putPrimitive()` bounds every
vertex access.

### Lighting

```
color = ambient * environmentLight                 (if the environment light is enabled)
      + diffuse * parallelLight * max(0, n . -L)   (if the parallel light is enabled)
```

Without lights the vertex color is `diffuse`. For `BlendMode::ADD` the color is
pre-multiplied by the opacity. Vertex colors are interpolated linearly across each
span and modulate the texel when a texture is present.

Without `SHAPOGFX3D_GOURAUD` the lit color of the first vertex of each primitive
is used for all of it, so the interpolation above does not happen.

Vertex normals must be unit length. When the upper 3x3 of the current matrix is a
rotation times a uniform scale (the usual case), the light direction is transformed
into model space once per primitive and `n . -L` is a single dot product per vertex;
otherwise (non-uniform scale, shear, or environment mapping) the normal is transformed
to view space and normalized per vertex.

### Environment mapping

With `ENV_MAP`, `u = 0.5 + 0.5 n.x`, `v = 0.5 - 0.5 n.y` from the view-space normal.

### Textures

Any enabled format. Texels are fetched through a format-specific sampler and
converted to 5/6/5 before modulation: GRAY1 becomes white or black, RGB444 and
ARGB4444 are expanded, RGB565BE is byte-swapped. An ARGB4444 texture supplies a
per-texel alpha `a4` (0..15); the triangle becomes translucent, and a material with
`BlendMode::NONE` is rasterized as `ALPHA` with `opacity = a4 / 15`, while `ALPHA`
and `ADD` multiply their opacity by `a4 / 15`. Texel alpha 0 skips the pixel.

## Memory management

`init()` aligns the arena to 8 bytes and carves it as follows:

1. **Fixed part**: line buckets (2 x screen height x `uint16_t`), layer table
   (`SHAPOGFX3D_LAYER_MAX` entries of 8 bytes), matrix stack (16 entries), vertex
   cache (64 entries).
2. **Span pool**: `Config::spanCapacity` spans of 16 bytes, or a quarter of the
   remaining space clamped to 32..512 spans when it is 0 (so 512 spans, 8 KB,
   unless the arena is small). Spans beyond the capacity are dropped,
   which leaves holes in the picture, so a tuned value is one that keeps
   `Stats::spanPeak` below it with margin for the worst frame; whatever it saves
   goes to the triangle buffer.
3. **Triangle buffer**: everything that remains.

The triangle buffer holds records of different sizes (see the table above), so it
is a byte budget rather than a triangle count: the records are packed downwards
from the end of the region while the 4-byte entries (record offset and scanline
link) grow upwards from its start, and the buffer is full when the two meet.
`Stats::triBytes` and `Stats::triBytesTotal` report both ends of it. Records are
addressed by a 4-byte-unit offset, which caps the region at 256 KB; a larger arena
leaves the excess unused.

`SHAPOGFX3D_STACK_DEPTH` and `SHAPOGFX3D_VCACHE_SIZE` size the fixed part (about
1.1 KB and 2.8 KB at their defaults). A smaller vertex cache costs re-transformed
vertices, not correctness.

If the arena is too small for the fixed part, `init()` leaves the renderer
uninitialized. Overflowing buffers drop the excess for the current frame.

### Fixed-point vertex stage (`SHAPOGFX3D_FIXED_POINT`)

The pipeline from a vertex to a primitive record is float by default: the
current matrix and the projection, lighting, the perspective divide, and the
plane setup of every triangle, line and point (Cramer's rule). On a Cortex-M0+
or another core without an FPU every one of those operations is a library call
of 50 to 100 cycles, and a kite of two triangles was measured at 60 us.

With `SHAPOGFX3D_FIXED_POINT=1` all of it is integer. The public API does not
change: positions, matrices and materials stay float, and existing scenes
compile and draw as they are. What changes is where the conversion happens --
once, at the boundary:

- The current matrix is converted to fixed point (rotation and scale Q18,
  translation 16.16) when it changes, not per vertex (`refreshFixed()`; every
  matrix call marks it dirty). The projection likewise, as the few numbers the
  perspective or orthographic mapping needs (a focal length in 8.8 px, the
  screen center, `m[10]` and `m[14]`); a matrix set any other way goes through
  a Q18 4x4 path. Lights are converted when enabled, material colors once per
  primitive.
- A vertex is converted when it is fetched (three float-to-int conversions),
  transformed with 32x32 -> 64 multiplies into 16.16 view space, and projected
  with one division: 1/w comes from a normalized reciprocal (a 32-bit hardware
  division for 16 bits, then Newton steps), which also serves the plane setup,
  where the determinant's reciprocal is shared by every gradient of the
  triangle. Screen coordinates are 16.16 px, depth 8.24, colors 8.8.
- The primitive setup computes the same integer records as the float build
  (see "Rendering pipeline"), with 64-bit products and the normalized
  reciprocal where the float build uses float; `render()` is the same code in
  both builds.

Limits the float path does not have: view-space coordinates within +-32767
model units; rotation and scale entries within +-2048; screen coordinates are
clamped to the guard band, +-8191 px (a triangle with a vertex beyond it bends
where it crosses the screen, noticeably so when the vertex is thousands of
pixels further out; the float path clips such a triangle exactly); texture
coordinates within +-30000 texels. The picture is not pixel-identical to the float path.
Measured on the same scenes, one 240x240 frame of a game differs in 90 pixels
on average (worst 811) and a lit, textured, translucent test scene in 7% of its
pixels, almost all by one shading step; both look the same. The test suite
passes in either build.

## Rendering pipeline

### `beginRender()`

Sorts the entries of each layer (never the records) farthest first by the sum of the
view-space z of the primitive's vertices, as an integer key with the ordering of the
float; equal keys keep the order the primitives were added in. Layers with
`LayerFlags::NO_DEPTH` are left in the order they were added. Depth order between
opaque spans of one layer is resolved by depth comparison in `render()`, so this sort
primarily determines the compositing order of translucent primitives.

### `render()`

The target format selects a rasterizer table and a fill function once per call;
RGB565BE and RGB444 are supported (others return without drawing). The region is
clipped to the screen and to the destination surface.

For every scanline of the region, a list of the triangles that start intersecting on
that line is built (in depth order, layer by layer). For each scanline this list is
merged into the active list, triangles that have been passed are removed, and then:

1. The span lists are cleared.
2. For each active triangle, farthest first:
    1. The record layout is recovered from the header (layer, flat and textured
       flags) and selects the span builder, so a primitive costs only the
       attributes it has.
    2. The two edges crossing the scanline give the span's pixel range: a span is
       only that range, the layer and a pointer to the record. Each edge is
       stored as its x at the center of the first row it is used on plus its
       step per row, both 16.16 px, so the x on a row is one 32-bit
       multiply-add. The stored x is derived from the edge's upper vertex
       (`x + floor(slope * (row center - y))`), which makes the x of a shared
       edge bit-identical in both triangles on every row: no pixel center falls
       between them.
    3. The span is inserted. Opaque spans are kept in a list sorted by x that never
       overlaps; translucent spans in a separate list in insertion order. Which of
       two overlapping spans is nearer is decided in this order: spans reach the
       list in layer order, so one from another layer is the one added later and
       therefore in front; inside a layer without depth the later span wins for the
       same reason; otherwise the NDC depths of the two records' depth planes are
       compared at the pixel at the center of the overlapping interval. Cutting
       a span only moves its ends. If the new span is nearer and opaque, the overlapping
       part of the farther span is removed whether it is opaque or translucent; if a
       translucent span is nearer, both are kept. Within a layer this does not rely
       on the per-primitive sort order alone, so large and small polygons are ordered
       correctly.
3. The opaque spans are rasterized in x order. The gaps are filled with the clear
   color when clearing is enabled and left untouched otherwise. Then the translucent
   spans are composited in list order according to their blend mode. Only now,
   and only for the spans that are drawn, are the color and the texture
   coordinates evaluated at the span's first pixel from the record's planes,
   with the plane's `dx` as the per-pixel increment. A flat primitive stores one
   color instead of three planes, so its span takes it as a constant.

Everything in `render()` is 32-bit integer arithmetic, in the float build as in
the fixed-point one. Each attribute of a record is a plane: its value at the
center of a reference pixel (column `xa` on the first row, next to the
primitive) and its two per-pixel gradients, in the attribute's own format --
depth 8.24 (NDC), color 8.16 (0..255), texture coordinates 16.16 texels. It is
evaluated at a pixel as `a0 + dx * (x - xa) + dy * (y - yMin)` in wrapping
32-bit arithmetic: at any pixel the primitive covers, the true value lies
within the range of its vertex values, so the sum is right even where a
product wraps. A plane whose gradients would exceed 2^30 in its format (a
sliver seen edge on) is stored constant instead. The setup computes the records
in float (or in fixed point, see above) once per primitive; the float build
first clips a triangle or a line that reaches beyond the guard band of the
16.16 coordinates (+-8191 px, +-32767 px from 13 coordinate bits on) in screen
space, where every stored attribute is linear, so far-off vertices cost a few
extra records but no accuracy.

Per-pixel processing is integer only. The inner loop is specialized for each
combination of blend mode (3) x texture format (none + 4) x flat (all three vertex
colors equal) x output format (2), selected through a table indexed by the
triangle's precomputed rasterizer index; a flat, opaque, untextured span degenerates
to a plain fill. Texture coordinates wrap with a bit mask, hence the power-of-two
requirement. Output pixels are written through the format's cursor, so RGB444
targets may start at odd x positions.

Vertex colors are always interpolated affinely. The interpolation of texture
coordinates is selected with `SHAPOGFX3D_CORRECT_PERSPECTIVE` (default 1):

- **0**: affine everywhere.
- **1** (default): vertical correction. `(u/w, v/w, 1/w)`, which are linear in screen
  space, are planes of the triangle; at the first and the last pixel of each drawn
  span they are divided to obtain exact `(u, v)`, and the span interior is
  interpolated affinely in fixed point. `1/w` is scaled per primitive by a power of
  two that puts its largest vertex value into [2^28, 2^29) (the scale cancels in the
  division), and a division is one normalized 32-bit division giving a 16-bit
  reciprocal of `1/w` plus two 32x32 -> 64 multiplications (`arch::mulShift`) for
  `u` and `v`. Per drawn textured span: two such divisions and two 32-bit divisions
  by the span length; costs 12 bytes per textured record. Along a scanline, a
  horizontal surface seen by a camera without roll has constant depth, so this level
  renders such surfaces without distortion; surfaces whose depth varies along the
  scanline keep affine distortion inside each span, while span end pixels (and
  therefore edges shared between triangles) are exact.
- **2**: full correction. `(u/w, v/w, 1/w)` are interpolated across the span and
  divided every `SHAPOGFX3D_PERSPECTIVE_STEP` pixels (default 16) the same way;
  `(u, v)` are interpolated linearly in fixed point in between. Costs one division
  per 16 textured pixels and 12 bytes per textured record.

With `SHAPOGFX3D_RP2_INTERP` (RP2040/RP2350), RGB565BE and ARGB4444 texels of
textures with a power-of-two stride are addressed by the SIO interpolator: lane 0
maps `u` to the byte offset in the row, lane 1 maps `v` to the row offset, and one
`POP_FULL` per pixel yields the texel address and steps both coordinates. Other
textures use the software walker.

### `endRender()`

Currently does nothing; reserved for future use.

## Tools (`bin/`)

Python 3 scripts (dependencies in `bin/requirements.txt`; also runnable with `uv run`
thanks to inline metadata). `shapogfx_imgconv.py` is the shared image conversion
module.

- **img2cpp** `[-f FORMAT] [-d DITHER] [-k COLOR] [--name N] [--namespace NS] [--resize WxH] [--pot] input output.hpp`
  emits an aligned `static const` pixel array and a `static const gfx2d::Texture`.
  Formats rgb565be (default), argb4444, rgb444, gray1; dithering none / diffusion /
  pattern; `-k` makes a key color transparent; `--pot` resizes to a power of two.
  Pixels are quantized with rounding; the memory layout matches `pixel.hpp`
  (RGB565BE and RGB444 are emitted as bytes, ARGB4444 as `uint16_t`).
- **gltf2cpp** `[--namespace NS] [--vertex-format float|packed] [--texformat auto|...] [--dither D] [--key-color C] [--max-texture-size N] [--no-resize-pot] input.gltf|glb output.hpp`
  (all glTF primitive modes are supported)
  emits, inside a namespace named after the file, `tex<i>` textures, `mat<i>` (and
  `mat<i>Vc` for primitives with vertex colors) materials, `mesh<i>Prim<j>Vertices` /
  `...Indices` / `mesh<i>`, `node_<name>` (or `node<i>`) nodes in child-first order,
  `scene<i>` and a `scene` alias for the default scene. Vertex attributes are
  interleaved into `Vertex`, or into the 16-byte `PackedVertex` with
  `--vertex-format packed` (positions quantized over the primitive's bounding box,
  texture coordinates outside -32..32 are clamped with a warning); missing normals
  are generated by accumulating face normals; COLOR_0 becomes `Vertex::color` and sets `VERTEX_COLOR` on a material copy;
  node TRS is composed into the column-major matrix on the tool side. Textures are
  resized to a power of two (with a warning) unless `--no-resize-pot`; `auto` picks
  ARGB4444 when the image or the material's alpha mode needs alpha.
    glTF point and line modes map to `POINTS` / `LINES` / `LINE_LOOP` / `LINE_STRIP` (no
  normals are generated for them). Oversized index ranges and out-of-range indices are
  reported and skipped, so the generated data always satisfies the renderer's invariants.

## Sample programs

Both samples are 480x320 and render into an RGB565BE buffer. Each has a WASM entry
point (`<name>_init`, `<name>_frame`, `<name>_get_fb`, `<name>_get_width`,
`<name>_get_height`) driven by `docs/example/viewer.js`, and a native `main()` that
writes one frame as a PPM file. The WASM binaries are committed so that `docs/` can be
served as a static site.

- `example/wasm/demo2d/`: exercises the `Graphics2D` API only (no reference to
  `gfx3d`): a scrolling ellipse pattern, filled and outlined polygon stars, ARGB4444
  sprites with alpha and additive blending, GRAY1 bitmaps with and without a
  background color, an RGB444 off-screen surface drawn with a second `Graphics2D` and
  blitted (whole and partial), rounded rectangles, circles, ellipses, triangles, lines,
  pixels, clipping and text in all four fonts including scaling and measurement.
- `example/wasm/demo3d/`: a textured floor, an environment-mapped torus (`putTorus`),
  opaque, alpha-blended and additive cubes, and a vertex-colored windmill generated
  from `model/windmill.glb` (`model/make_windmill.py`) with `gltf2cpp` whose "Blades"
  node is rotated by a `NodeVisitor`. The frame is composed in two passes: a 2D
  backdrop (gradient, stars, caption) drawn with `Graphics2D`, then the 3D scene
  rendered in four bands with the clear disabled. Mouse and keyboard control the
  camera in the browser.

## Tests

`test/` builds `shapogfx_tests` (registered with CTest) without any external
framework. It checks color conversions and cursors for every enabled format, blending
identities, `Graphics2D` clipping, fills, polygons, lines, ellipses, image blits,
bitmaps and text metrics, consistency between RGB565BE and RGB444 targets, and for
the 3D renderer: banded versus whole-frame rendering (byte identical), offset
rendering, transparent clear, all texture formats on both output formats, texel
alpha, the winding of every shape (culled and double-sided renders must match),
vertex colors, the index range check, points/lines (Bresenham coverage, end points,
LINE_LOOP, hidden-line removal, point size, near-plane clipping, depth bias),
watertight shared edges (no background pixel inside the silhouette of a subdivided
plane or an icosphere) and exact coverage of a triangle with a vertex far beyond the
guard band (float build), plus the `SHAPOGFX_COORD_BITS` limits and 2D lines and
polygons with far-off vertices. `test/data` holds a procedural image and a
small glTF model with the headers generated from them in both vertex forms
(`test/tools` regenerates them); the tests verify the generated textures against
the source pixels, that the packed model renders like the float one, and the
generated scene graph (names, hierarchy, transforms, generated normals, traversal,
visitor skipping and animation, deep-tree cut-off). The tests are meant to be run with AddressSanitizer and
UndefinedBehaviorSanitizer on the native build. The CMake options of the renderer
are passed to the tests as well, so a configuration with a feature compiled out
skips the tests that need it and the rest must still pass.
