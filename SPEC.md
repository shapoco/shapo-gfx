# ShapoGFX Specification

## Overview

ShapoGFX is a set of 2D/3D graphics libraries for embedded systems, written in
portable C++17 with no platform dependencies.

- Namespaces: `shapoco::gfx2d` (2D API and shared types), `shapoco::gfx3d` (3D renderer)
- Pixel formats: GRAY1, RGB444, ARGB4444, RGB565_SWAPPED, RGB565 (see below)
- Low memory: no frame buffer, no Z buffer; the 3D renderer works scanline by scanline
- No dynamic allocation inside the library; the 3D renderer's working memory comes from
  a user-supplied arena, and so do the 2D API's state stack and scratch memory (the 2D API
  draws without one too)
- Image data (vertex arrays, textures, fonts) is referenced, not copied, so it may live in flash

## Source layout

```
include/shapoco/gfx2d/   2D API and shared types
include/shapoco/gfx3d/   3D renderer
src/gfx2d/, src/gfx3d/   implementation
src/common/              internal helpers of both renderers (integer math, target detection)
src/gfx2d/internal.hpp   internal declarations of the 2D renderer (options, spans, access)
src/gfx2d/graphics2d.cpp 2D row operations, state, rectangles, lines, text
src/gfx2d/shapes.cpp     2D ellipses, arcs, rounded rectangles, polygons
src/gfx2d/images.cpp     2D images and 1-bit masks (bitmaps, glyphs)
src/gfx2d/vg.cpp         2D vector graphics (paths, brushes, strokes, pictures)
src/gfx2d/rig.cpp        2D skeletal animation (rig::Instance, drawBind)
src/gfx2d/arch.hpp       architecture hooks of the 2D renderer (internal; RP2)
src/gfx3d/arch/          architecture hooks of the 3D renderer (internal; generic + RP2)
example/wasm/            sample programs (WASM and native)
docs/example/            browser pages for the samples
test/                    self-checking tests
```

Users include `shapoco/gfx2d/gfx2d.hpp` and/or `shapoco/gfx3d/gfx3d.hpp` (and the
optional `shapoco/gfx2d/fonts.hpp`, `surface_alloc.hpp` and `rig.hpp`) and compile
`src/gfx2d/*.cpp` and `src/gfx3d/*.cpp`. Header guards and compile-time options use
the prefixes `SHAPOGFX_` (shared), `SHAPOGFX2D_` and `SHAPOGFX3D_`.

## Version (`version.hpp`)

gfx2d and gfx3d are released together and share one version number, defined in
`include/shapoco/gfx2d/version.hpp` and included by `config.hpp`, so it is visible
through either umbrella header. `SHAPOGFX_VERSION_MAJOR`, `_MINOR` and `_PATCH` are
the components, `SHAPOGFX_VERSION_STRING` the `"major.minor.patch"` string and
`SHAPOGFX_VERSION` the single integer `0x00MMmmpp` for `#if` comparisons
(`SHAPOGFX_MAKE_VERSION(major, minor, patch)` builds one). The same values are
available as `constexpr` constants `shapoco::gfx::VERSION_MAJOR`, `VERSION_MINOR`,
`VERSION_PATCH`, `VERSION` and `VERSION_STRING`. The number must match `"version"`
in `library.json` and in `idf_component.yml`; each release is tagged `v<version>`
in git.

## Compile-time configuration (`config.hpp`)

| Macro | Default | Effect |
|---|---|---|
| `SHAPOGFX_FORMAT_GRAY1` | 1 | Enable the GRAY1 format |
| `SHAPOGFX_FORMAT_RGB444` | 1 | Enable the RGB444 format |
| `SHAPOGFX_FORMAT_ARGB4444` | 1 | Enable the ARGB4444 format |
| `SHAPOGFX_FORMAT_RGB565_SWAPPED` | 1 | Enable the RGB565_SWAPPED format |
| `SHAPOGFX_FORMAT_RGB565` | 0 | Enable the RGB565 (native byte order) format |
| `SHAPOGFX_COORD_BITS` | 11 | Bits of a screen coordinate and of a surface's width and height (1..15). Wider or taller surfaces are rejected (see below) |
| `SHAPOGFX3D_CORRECT_PERSPECTIVE` | 1 | Perspective correction level of the 3D renderer (0/1/2) |
| `SHAPOGFX3D_PERSPECTIVE_STEP` | 16 | Level 2: pixels between two exact evaluations of the texture coordinates (power of two) |
| `SHAPOGFX3D_GOURAUD_STEP` | 1 (4 on the RP2040 / RP2350) | Pixels between two updates of the vertex color that modulates the texels of a textured, smoothly shaded span (power of two up to 16). 4 saves a few instructions per textured pixel; the color is then constant over groups of 4 pixels (in demo3d 1.3% of the pixels change, mostly by a shade) |
| `SHAPOGFX3D_RP2_INTERP` | 1 on RP2, else 0 | RP2040/RP2350 (Pico SDK): fetch 16-bit texels through the SIO interpolator `interp0` and step Gouraud colors through `interp1`. On by default when the target is detected as RP2 (`PICO_RP2040` / `PICO_RP2350`) and `hardware/interp.h` is on the include path; 0 turns it off |
| `SHAPOGFX2D_RP2_INTERP` | 1 on RP2, else 0 | RP2040/RP2350 (Pico SDK): a rotated or sheared `drawImage()` of a 16-bit image whose stride is a power of two fetches the pixels through the SIO interpolator `interp0`. Detected like `SHAPOGFX3D_RP2_INTERP`; 0 turns it off |
| `SHAPOGFX2D_FPU_SQRT` | 1 with an FPU, else 0 | The integer square root of the axis-aligned ellipse extents is seeded by the FPU's `sqrtf()` and corrected to the exact floor (the same value as the pure integer root, which a core without an FPU keeps). Detected from the compiler (`__ARM_FP`, `__riscv_flen`, the ESP32-S3/P4, x86, AArch64, WebAssembly); 0 or 1 overrides |
| `SHAPOGFX2D_TRANSFORM` | 1 | 2D transforms; 0 removes them (the transform stays the identity, `setTransform()` and friends do nothing) |
| `SHAPOGFX2D_BLEND` | 1 | 2D blend modes other than `ALPHA` and the opacity; 0 removes them (`setBlend()` does nothing) |
| `SHAPOGFX2D_COLOR_KEY` | 1 | Color key of `drawImage()`; 0 removes it (`setColorKey()` does nothing) |
| `SHAPOGFX2D_STACK_DEPTH` | 16 | Levels of the 2D state stack (`pushState()`) |
| `SHAPOGFX2D_RIG` | 1 | Skeletal animation (`rig.hpp`); 0 removes it (`rig::Instance::init()` returns false, the rest does nothing) |
| `SHAPOGFX2D_ANTIALIAS` | 1 | Antialiasing of the vector calls (`fillPath()`, `strokePath()`, `drawPicture()`) and of the area fills under `setAntialias(true)`; 0 removes the coverage code and draws their edges like polygons whatever `setAntialias()` says |
| `SHAPOGFX3D_HOT_ATTR` | (empty) | Attribute put on the rasterization side (`render()` and the per-span functions, ~14 KB on Cortex-M0+), e.g. `__attribute__((section(".time_critical.gfx3d")))` to run it from RAM on the Pico SDK |
| `SHAPOGFX3D_HOT_INSTANTIATE` | 0 | 1 also instantiates the per-span function templates explicitly with `SHAPOGFX3D_HOT_ATTR` (GCC ignores a section attribute on a template otherwise); see "Placing the rasterization side" |
| `SHAPOGFX_ARCH_SPLIT_MUL64` | 1 on Cortex-M0/M0+ and ESP8266, else 0 | 1 forms 32x32 -> 64-bit products from four 16x16-bit ones inline instead of calling a library routine, for a core whose multiplier yields only the low 32 bits (fixed-point vertex stage, setup, perspective division); same results either way |
| `SHAPOGFX3D_FIXED_POINT` | 0 | Vertex stage and primitive setup in fixed point, for cores without an FPU (see "Fixed-point vertex stage") |
| `SHAPOGFX3D_DEPTH_BITS` | 32 | Depth resolution of the records: 32 (8.24) or 16 (1.15, 4 bytes less per record with depth; see "Memory management") |
| `SHAPOGFX3D_TEXTURE` | 1 | Texture and environment mapping |
| `SHAPOGFX3D_GOURAUD` | 1 | Gouraud shading; 0 selects flat shading |
| `SHAPOGFX3D_BLEND` | 1 | Translucency |
| `SHAPOGFX3D_LINES` | 1 | `LINES` / `LINE_STRIP` / `LINE_LOOP` primitives |
| `SHAPOGFX3D_POINTS` | 1 | `POINTS` primitives |
| `SHAPOGFX3D_STACK_DEPTH` | 16 | Levels of the matrix stack (`pushState()`) |
| `SHAPOGFX3D_VCACHE_SIZE` | 64 | Entries of the vertex cache (power of two) |
| `SHAPOGFX3D_LAYER_MAX` | 8 | Layers a scene can hold (1..128) |

Every macro below `SHAPOGFX3D_` is read by `src/gfx3d/*.cpp` only, and every
`SHAPOGFX2D_` macro by `src/gfx2d/*.cpp` only; they change no public type, so
translation units cannot disagree about them. A 2D feature turned off keeps its
functions and state members, which are then ignored, so application code compiles
unchanged. `SHAPOGFX_COORD_BITS`
lives in `config.hpp` like the format macros and must have the same value in every
translation unit (the CMake option passes it on as a public definition).

Under ESP-IDF the repository is a component: its `CMakeLists.txt` hands over to
`cmake/esp_idf.cmake` when `ESP_PLATFORM` is set, and `idf_component.yml` is its
manifest. The component is named after its directory, `shapo-gfx`. The macros of
the table above other than `SHAPOGFX3D_RP2_INTERP`, `SHAPOGFX2D_RP2_INTERP`,
`SHAPOGFX3D_HOT_ATTR`, `SHAPOGFX3D_HOT_INSTANTIATE` and
`SHAPOGFX_ARCH_SPLIT_MUL64` are Kconfig symbols of the same name
(`CONFIG_SHAPOGFX_FORMAT_GRAY1`, ...) with the same defaults; the format macros
and `SHAPOGFX_COORD_BITS` are passed on to the components that require
`shapo-gfx`. `SHAPOGFX3D_DEPTH_BITS` is the choice
`CONFIG_SHAPOGFX3D_DEPTH_BITS_32` / `_16` and `SHAPOGFX2D_FPU_SQRT` the choice
`CONFIG_SHAPOGFX2D_FPU_SQRT_AUTO` / `_ON` / `_OFF`, where `AUTO` leaves the
detection to the library.

`SHAPOGFX_COORD_BITS` bounds what the renderers have to handle: a surface or a 3D
screen wider or taller than `SHAPOGFX_COORD_MAX` (`2^bits - 1`) pixels is rejected
-- `Graphics2D::setTarget()` leaves the context without a target, like a surface in
a disabled format, and `Graphics3D::init()` leaves the renderer uninitialized.
Within the limit every screen coordinate fits 16 bits and every product of two
coordinate differences 32 bits, which is what lets the 2D line and polygon walkers
and the 3D span stage do without 64-bit arithmetic. The public types (`int`,
`Rect`, `vec2i`, `Surface::width`) do not change.

`src/common/arch_detect.hpp` detects the target for both renderers
(`SHAPOGFX_ARCH_RP2`, `SHAPOGFX_ARCH_ESP32S3`, `SHAPOGFX_ARCH_ESP32P4`, else
`SHAPOGFX_ARCH_GENERIC`; any of them may be defined by hand). The 3D renderer's
architecture-specific code lives in `src/gfx3d/arch/`, behind a few hooks with a
portable implementation that is always present (`generic.hpp`); `arch.hpp` selects
the implementation. The ESP8266 is `SHAPOGFX_ARCH_GENERIC`:
the ESP8266_RTOS_SDK defines `ESP_PLATFORM` too, but its `CONFIG_IDF_TARGET_ESP8266`
selects nothing. `SHAPOGFX_ARCH_SPLIT_MUL64` (on by default for `__ARM_ARCH_6M__` and
the ESP8266 -- `CONFIG_IDF_TARGET_ESP8266`, `ESP8266` or `ARDUINO_ARCH_ESP8266` --
and settable by hand for any other core without a 32x32 -> 64 multiply) selects
`split_mul.hpp`. The hooks: `mul64` (a 32x32 -> 64-bit product, used by the
fixed-point vertex stage and setup) and `mulShiftU16` (a 32x16-bit product shifted
right, used by the perspective division), the texture walker (`InterpTex` on RP2),
`GouraudRG` (red and green of a smooth span, interp1 on RP2) and `RenderState`
(hardware state that `render()` saves and restores). With `SHAPOGFX3D_RP2_INTERP` the target
must link `hardware_interp`; `render()` saves and restores `interp0` and `interp1` of
the calling core, so interrupt handlers running during `render()` must not use them.

The 2D renderer's hooks are in `src/gfx2d/arch.hpp`. With `SHAPOGFX2D_FPU_SQRT`
(on where the compiler reports a hardware FPU) the integer square root of the
ellipse extents starts from `sqrtf()` (one `vsqrt` on the Cortex-M33) and is settled
to the exact floor by at most one step each way, instead of the 16-iteration integer
root, which a core without an FPU keeps. With `SHAPOGFX2D_RP2_INTERP`
a rotated or sheared `drawImage()` walks the source through `interp0` -- lane 0 turns
the 16.16 u into the byte offset of the texel, lane 1 the 16.16 v into the byte
offset of the row (hence the power-of-two stride), `POP_FULL` returns the address
and steps both -- which takes the per-pixel fetch of a copy from 11 instructions
to 5 on the Cortex-M33. The call saves and restores `interp0` of the calling core;
the same caveat about interrupt handlers applies, and the target must link
`hardware_interp`. Every other image goes through the portable walk, which gives
the same pixels.

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
| header and flat color | 48 | — |
| + depth plane | 60 (56) | a layer without `LayerFlags::NO_DEPTH` |
| + interpolated color | 76 (72) | three differing vertex colors (`SHAPOGFX3D_GOURAUD`) |
| + texture coordinates | 104 (100) | a textured material (`SHAPOGFX3D_TEXTURE`) |
| all of them | 120 (116) | |

In parentheses: with `SHAPOGFX3D_DEPTH_BITS=16`. The header (40 bytes) holds the
edges or end points, a 16-bit sort key, the row range, the reference column of the
planes and the rasterizer index; the texture pointer lives in the texture part
together with the logarithms of the texture's width, height and stride (computed
once per primitive; the texel walkers need them per span), so an untextured
record carries no material. The depth plane is 8.24 in 12 bytes, or
with 16 bits of depth a 2.14 value and two 16-bit gradients whose scale is chosen
per record in 8 bytes (the gradient is quantized to 2^-15 of its own magnitude, the
value to 2^-14 NDC: surfaces that cross far from the camera may swap slightly
earlier or later where they meet). Interpolated colors are 10.6 values and 8.8
gradients (20 bytes); gradients beyond 127 levels per pixel (a sliver seen edge on)
do not fit, and such a primitive is stored flat in the color of its first vertex.
Over a span of 2000 pixels the 8.8 gradient drifts by at most 4 of 255 levels, less
than a step of RGB565.

Turning a feature off removes the corresponding layouts and their code. A span
is 16 bytes in every configuration -- its pixel range, its layer and pointers to
its record and to the next span -- because its attributes are evaluated from the
record when it is drawn; the pool holds `Config::spanCapacity` of them.

Disabling a format removes its code from both renderers: the pixel cursors, the 2D
per-format row operations, the 3D texture samplers and (for output formats) the 3D
rasterizer table. Surfaces or textures in a disabled format are ignored at run time.
An output format of the 3D renderer is the largest item: its rasterizer table
instantiates the pixel loop for every texture format, blend mode and shading, about
14 KB of code on a Cortex-M33 and 20 KB on a Cortex-M0+ (the part that
`SHAPOGFX3D_HOT_ATTR` places in RAM). This is why RGB565 is off by default: an
application that draws into it enables it with `SHAPOGFX_FORMAT_RGB565=1` and
should then disable RGB565_SWAPPED (`SHAPOGFX_FORMAT_RGB565_SWAPPED=0`) unless it uses both.
The format macros must have the same values in every translation unit.

## `shapoco::gfx2d`

### Pixel formats (`pixel.hpp`)

```c++
enum class PixelFormat : uint8_t { GRAY1, RGB444, ARGB4444, RGB565_SWAPPED, RGB565 };
```

| Format | Bits/pixel | Memory layout | Native pixel (in registers) |
|---|---|---|---|
| `GRAY1` | 1 | MSB first within a byte; 1 = white | 0 or 1 |
| `RGB444` | 12 | 2 pixels in 3 bytes: `R1G1`, `B1R2`, `G2B2` (display order) | `0x0RGB` |
| `ARGB4444` | 16 | native `uint16_t` | `0xARGB`; A = 15 opaque |
| `RGB565_SWAPPED` | 16 | `uint16_t` stored with its bytes swapped relative to the CPU's order; on a little-endian CPU byte 0 = `RRRRRGGG`, byte 1 = `GGGBBBBB` | `RRRRRGGGGGGBBBBB` (5/6/5) |
| `RGB565` | 16 | native `uint16_t` (opt-in, `SHAPOGFX_FORMAT_RGB565=1`) | `RRRRRGGGGGGBBBBB` (5/6/5) |

Every row of an image starts on a byte boundary; rows are `stride` bytes apart
(`minStride(format, width)` gives the smallest legal stride).

The two 16-bit color formats are named by how they relate to the CPU, not by a
fixed byte order: RGB565 keeps the RGB565 value in the CPU's order, RGB565_SWAPPED
swaps its two bytes on every access. On a little-endian CPU -- every target so far
(ARM, Xtensa, RISC-V, x86, WebAssembly) -- RGB565_SWAPPED therefore has the high byte
first in memory. That, and RGB444, are the byte streams expected by common display
controllers over an 8-bit bus, so a Surface in either format can be transferred
without conversion. RGB565 holds the
same pixels in the CPU's own byte order, which saves the byte swap of every pixel
read and write -- one instruction on a Cortex-M3 and up, three or four on Xtensa
(ESP32-S3) or a RISC-V core without Zbb (ESP32-P4), twice that for a blended pixel.
It suits a display interface that sends 16-bit words most significant byte first:
an RP2040/RP2350 SPI in 16-bit mode or a PIO program, the i80 bus of ESP-IDF's
`esp_lcd` (with `swap_color_bytes`), and any DMA that moves 16-bit units. Only a
byte-wise SPI transfer needs RGB565_SWAPPED. ARGB4444 is a composition format (sprites with
alpha), GRAY1 a mask/monochrome format.

### Colors

`Color` is `uint32_t` ARGB8888. It is the only color type of the 2D API and is
converted to the target format once per drawing call. Helpers: `makeColor(r, g, b,
a = 255)`, `makeColorF(...)`, `makeColorHsv(h, s, v, a)`, `colorWithAlpha`,
`lerpColor`, component accessors, the `Colors::` constants, and `colorToNative` /
`nativeToColor` for any format.

Per-format helpers (all `inline`): pack/unpack (`makeRgb565`, `packRgb565`,
`packRgb565Swapped`, `colorToRgb565`, `rgb565ToColor`, the same for RGB444 and ARGB4444,
`colorToGray1`), alpha blending with a 0..64 opacity (`blendAlphaRgb565`,
`blendAlphaRgb444`, `blendAlphaArgb4444`), saturating addition (`addSaturate...`),
`fill16`, `bswap16`, `log2Floor`.

### Pixel cursors

`CursorGray1`, `CursorRgb444`, `CursorArgb4444`, `CursorRgb565Swapped`, `CursorRgb565` give sequential
access to one row: `init(line, x)`, `read()`, `write(native)`, `next()`,
`skip(n)`, `fill(n, native)`. `FormatTraits<F>` maps a format to its cursor and its Color
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
outlive the calls). Coordinates go through the transform of the state, then all
drawing is clipped to the clip rectangle (in target pixels). Colors are `Color`; how
they are put is set by the blend mode and opacity of the state (by default alpha 0
draws nothing, 255 overwrites, anything else blends).

```c++
struct Config {};                     // settings of init() (none yet)
enum class TransformKind : uint8_t { IDENTITY, TRANSLATE, SCALE, AFFINE };
struct TextMetrics { int width, height, ascent, lineAdvance; };   // integer only
struct TextMetricsF { float width, height, ascent, lineAdvance; };  // on the target
struct TextState {
  const GFXfont *font; Color color, background;
  int cursorX, cursorY, lineStartX; int16_t ascent, lineHeight;
};
struct GraphicsState2D {              // what pushState() saves (100 bytes on a 32-bit target)
  affine2f transform; TextState text; Color colorKey;
  vg::Brush fillBrush, strokeBrush; vg::StrokeStyle strokeStyle;   // of the vector calls
  ucoord_t clipX, clipY, clipWidth, clipHeight;   // SHAPOGFX_COORD_BITS: 8 or 16 bits
  BlendMode blendMode; uint8_t opacity; bool colorKeyEnabled;
  bool antialias;                     // of the vector calls and the area fills (false)
};

class Graphics2D {
 public:
  Graphics2D();  explicit Graphics2D(const Surface &target);

  // memory
  bool init(const Config &, void *arena, size_t arenaSize); bool init(void *arena, size_t arenaSize);
  void deinit(); bool isInitialized() const;
  static size_t arenaBytes(size_t scratchBytes = 2048);

  // target
  void setTarget(const Surface &); const Surface &target() const; bool hasTarget() const;
  PixelFormat format() const; Rect bounds() const;

  // state
  bool pushState(); void popState(); int stateDepth() const;
  const GraphicsState2D &state() const; void setState(const GraphicsState2D &);
  void setClipRect(const Rect &); void setClipRect(int x, int y, int w, int h);
  void resetClipRect(); Rect clipRect() const;
  void setTransform(const affine2f &); void resetTransform(); const affine2f &transform() const;
  TransformKind transformKind() const; void applyTransform(const affine2f &);   // transform * m
  void translate(float x, float y); void scale(float sx, float sy); void scale(float s);
  void rotate(float angle); void rotate(float angle, float cx, float cy);
  void setBlend(BlendMode, int opacity = 255); void setBlendMode(BlendMode); void setOpacity(int);
  BlendMode blendMode() const; int opacity() const;
  void setColorKey(Color); void clearColorKey(); bool hasColorKey() const; Color colorKey() const;

  // pixels and rectangles (RectF: float, continuous)
  void clear(Color);                          // overwrites the clip rectangle
  void setPixel(int x, int y, Color, bool transformed = true);
  Color getPixel(int x, int y, bool transformed = true) const;
  void fillRect(const Rect &, Color); void fillRect(int x, int y, int w, int h, Color); void fillRect(const RectF &, Color);
  void drawRect(const Rect &, Color, int thickness = 1); void drawRect(const RectF &, Color, float thickness = 1);
  void fillRoundRect(const Rect &, int radius, Color); void fillRoundRect(const RectF &, float radius, Color);
  void drawRoundRect(const Rect &, int radius, Color); void drawRoundRect(const RectF &, float radius, Color);
  void drawHLine(int x, int y, int w, Color); void drawVLine(int x, int y, int h, Color);

  // ellipses (inscribed in the rectangle)
  void fillEllipse(const Rect &, Color); void fillEllipse(const RectF &, Color);
  void drawEllipse(const Rect &, Color); void drawEllipse(const RectF &, Color);
  void fillCircle(int cx, int cy, int r, Color); void fillCircle(const vec2f &c, float r, Color);
  void drawCircle(int cx, int cy, int r, Color); void drawCircle(const vec2f &c, float r, Color);

  // arcs and sectors of the same ellipses (angles in radians, see below)
  void drawArc(const Rect &, float start, float end, Color);    void drawArc(const RectF &, ...);
  void fillSector(const Rect &, float start, float end, Color); void fillSector(const RectF &, ...);
  void drawCircleArc(int cx, int cy, int r, float start, float end, Color);   // and vec2f, float
  void fillCircleSector(int cx, int cy, int r, float start, float end, Color);

  // lines and polygons (each also with vec2f)
  void drawLine(int x0, int y0, int x1, int y1, Color); void drawLine(const vec2f &, const vec2f &, Color);
  void drawPolyline(const vec2i *, int n, Color); void drawPolygon(const vec2i *, int n, Color);
  void fillPolygon(const vec2i *, int n, Color);   // even-odd rule, <= 32 crossings per row
  void fillTriangle(...); void drawTriangle(...);

  // images (blend mode, opacity and color key of the state)
  void drawImage(const Texture &, int dx, int dy);
  void drawImage(const Texture &, int dx, int dy, const Rect &src);
  void drawImage(const Texture &, int dx, int dy, const Rect &src,
                 const int16_t *polygon, int count);                  // the part of src in a convex polygon
  static constexpr int IMAGE_POLYGON_MAX = 16;
  void drawImage(const Texture &, const Rect &dst, const Rect &src);  // scaled
  void drawImage(const Texture &, const Rect &dst);
  void drawImage(const Texture &, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);
  void drawBitmap(const Texture &gray1, int dx, int dy, Color fg, Color bg = TRANSPARENT);
  void drawBitmap(const Texture &gray1, int dx, int dy, const Rect &src, Color fg, Color bg = TRANSPARENT);

  // text
  void setFont(const GFXfont *); const GFXfont *font() const;
  void setTextColor(Color fg, Color bg = TRANSPARENT);
  void setCursor(int x, int y); vec2i cursor() const;
  int drawChar(int x, int y, int code);        // returns the x advance
  void drawString(const char *); void drawString(int x, int y, const char *);
  TextMetrics charMetrics(int code) const; TextMetrics textMetrics(const char *) const;
  TextMetricsF deviceCharMetrics(int code) const; TextMetricsF deviceTextMetrics(const char *) const;

  // vector graphics (vg.hpp; brushes, stroke style and antialiasing of the state)
  void setAntialias(bool); bool antialias() const;
  void setFillBrush(const vg::Brush &); void setFillColor(Color); const vg::Brush &fillBrush() const;
  void setStrokeBrush(const vg::Brush &); void setStrokeColor(Color); const vg::Brush &strokeBrush() const;
  void setStrokeStyle(const vg::StrokeStyle &); void setStrokeWidth(float); const vg::StrokeStyle &strokeStyle() const;
  void fillPath(const vg::Path &); void strokePath(const vg::Path &); void drawPath(const vg::Path &);  // fill, then stroke
  void strokePolyline(const vec2f *, int n, bool closed = false);
  void drawPicture(const vg::Picture &, Color currentColor = BLACK, float currentStrokeWidth = 1);
};
```

Semantics:

- **Memory**: `init()` takes an arena for the state stack (`SHAPOGFX2D_STACK_DEPTH`
  levels of `GraphicsState2D`, 1.1 KB by default) and uses the rest as scratch memory,
  taken and released within a drawing call: polygons with more than 12 edges keep
  their edges there (24 bytes each), and a rounded rectangle under rotation its corner
  vertices when a corner takes more than 4 chords; the vector calls keep the edges of
  a path there (16 bytes each, plus 2 of bookkeeping) and the coverage of a row (2
  bytes per pixel of the path's width). Without an arena (or with too
  little) everything still draws: `pushState()` returns false, polygons evaluate every
  edge from its vertices on every row (slower, same pixels), turned corners use 4
  chords, and a path is drawn in parts from a buffer of 24 edges on the stack, with
  antialiasing only up to 128 pixels wide (see "Vector graphics").
- **State**: `pushState()` copies the whole state; `popState()` restores it except for
  the text cursor (`cursorX`, `cursorY`, `lineStartX`), and clips the clip rectangle to
  the current target. `setState()` does the same checks and classifies the transform.
- **Transform**: an affine map from the coordinates of the drawing calls to target
  pixels (no perspective). The member functions multiply on the right like a canvas
  context. The transform is classified whenever it changes: `IDENTITY`, `TRANSLATE`
  (translation only), `SCALE` (no rotation or shear; `b` and `c` below 1e-6, so a
  turn by a multiple of pi counts) or `AFFINE`. Under `IDENTITY` and `TRANSLATE` the
  drawing calls run the code they run without a transform, with the translation
  snapped to whole pixels and added to integer coordinates; only float coordinates
  keep its fraction. The clip rectangle is not transformed.
- **Coordinates**: continuous, pixel `(x, y)` covering `[x, x + 1) x [y, y + 1)`.
  *Areas* (rectangles, ellipses, images, glyph boxes) cover the pixels whose
  centers they contain after the transform; an edge through a center gives it to the
  pixel right of or below it (a coordinate `v` snaps to the pixel `ceil(v - 0.5)`).
  *Points* (line end points, polygon vertices, `setPixel()`, circle centers) name a pixel; its center
  goes through the transform and lands in a pixel. *Lines and outlines* stay one
  pixel wide (`drawLine()`, `drawHLine()` / `drawVLine()`, ellipse, arc and rounded
  rectangle outlines); the `thickness` of `drawRect()` is an area and scales. So under
  a scale an integer rectangle becomes the rectangle between its snapped corners, and
  a quarter turn maps integer shapes pixel for pixel.
- **Blend**: shapes and text put their color's alpha x opacity: `ALPHA` blends,
  `ADD` adds the color weighted by it with saturation, `NONE` overwrites with the
  color (its alpha goes into an ARGB4444 target). Images use their pixels' alpha
  (ARGB4444 only) x opacity the same way (the blend helpers take their weight with
  any number of fraction bits, `blendNative<F, SHIFT>`; the ARGB4444 sprite paths
  turn the 4-bit alpha field into a 10-bit weight by one multiply with a factor
  computed once per call from the opacity, `ImageBlit::alphaMul`, and blend with
  it unrounded, while the Color path keeps the 6-bit weight of alpha x opacity),
  and `NONE` copies (ARGB4444 alpha into an
  ARGB4444 target). Every shape reaches the pixels through one span function that
  switches on the blend (the opaque fill first); a color and blend become a native
  value, a weight and an operation once per call. `clear()` overwrites whatever the
  blend and transform.
- **Color key**: `drawImage()` skips the image pixels equal to the key converted to
  the image's format (alpha included for ARGB4444).
- **Rectangles** are half-open (`[x, x + w)`); negative sizes are normalized.
  `drawRect` draws inside the rectangle. Under `SCALE` a rectangle is the rectangle
  between its snapped corners, under `AFFINE` a polygon (a frame is its outer and
  inner outlines joined by an edge walked there and back, which the even-odd rule
  cancels).
- **Antialiasing** (`setAntialias()`, off by default): with it on, the area fills
  go through the vector rasterizer of `vg.cpp` as the path of the same shape
  (`fillRect()` of a `RectF`, or of a `Rect` under `SCALE` / `AFFINE`, since whole
  pixels under a translation have nothing to antialias; `fillEllipse()` and the
  circles as the four-cubic ellipse of the rectangle; `fillRoundRect()`;
  `fillPolygon()` and the triangles with the vertices as pixel centers, up to 64
  vertices, with the even-odd rule) and are antialiased exactly as `fillPath()`
  (see "Vector graphics"); not with the `NONE` blend mode, and not with
  `SHAPOGFX2D_ANTIALIAS=0`. Lines and outlines become thin antialiased lines
  on the target: the points are taken through the transform first (so they stay
  a pixel wide whatever the scale) and each segment is drawn the way of Wu,
  along its major axis every column (row) painting the two pixels the line
  passes between, weighted by where it passes, a few instructions per pixel and
  nothing per row; at a vertex the next segment paints a pixel of its first
  column only by what its weight exceeds the one the last segment left there
  (and the last segment of a polygon likewise against the first), so that a
  vertex is neither painted twice (a translucent polyline would show its
  joints) nor left dim where the two segments split its coverage. `drawLine()`,
  `drawPolyline()` and `drawPolygon()` run between the pixel centers (an
  axis-aligned line covers the same pixels as the plain one); `drawEllipse()`,
  `drawCircle()`, `drawArc()` and `drawRoundRect()` draw the shape inset by half
  a pixel (half a pixel of the target along each axis of the transform),
  flattened like a path, so that the outline stays within the fill; `fillSector()` is
  the path of the pie (the arc as cubics of at most a quarter turn each,
  `vg::PathBuilder::arc()`) and `drawRect()` the even-odd path of its outer and
  inner rectangles (an integer frame under a translation stays plain).
  `drawHLine()` / `drawVLine()` under a translation are whole pixels and stay
  plain. Text and bitmaps under a transform (`SCALE` or `AFFINE`) sample the
  mask at four points per target pixel (a 2 x 2 grid half a pixel apart, through
  the inverse transform) and blend the foreground with a quarter of its alpha
  per set bit and the background with a quarter per clear one (5 levels; at a
  whole scale the samples fall into one texel and nothing is gray);
  untransformed text is pixel-exact already and unchanged. Images under a
  transform (`SCALE` / `AFFINE`, the `dst` rectangle included) or clipped to a
  polygon are drawn pixel by pixel (`G2Impl::drawImageAA()`, `ImageAA` in
  `images.cpp`): every pixel whose center lies inside the outline (the rectangle
  or the polygon) or within 0.71 pixel outside it samples the four texels around
  the source point under its center bilinearly, premultiplied (a transparent
  ARGB4444 texel or a keyed one has weight 0 and lends no color), the four
  clamped to `src`; the four are fetched again only when the point leaves their
  cell (a magnified image keeps them over several pixels) and mixed without the
  premultiplication when all are opaque, the alpha undone through a reciprocal
  table; the source point, the edge distances and their steps are 16.16 integers
  and the span bounds of the rows 24.8, so that no floating point runs per pixel
  or per row (a few conversions per chunk of 32 pixels). The pixels along the outline take its coverage as a factor of their
  alpha, approximated from the outline's edges (per edge the overlap of the pixel
  with its inner side, the signed distance of the center plus a half clamped to
  0..1, multiplied over the edges: exact along an edge, a product at the corners;
  stepped along the row, no rasterizer or buffer); the pixels 0.71 or more inside
  every edge skip it. The color key and the opacity apply as before. A plain copy
  (no transform, or a translation) has nothing to smooth and stays the plain
  copy. The bilinear sampling costs about 10 ns per pixel on the host, 5 to 7
  times the plain blit: with antialiasing on, a frame of demorig costs 7 times
  the plain one at zoom 1 and zoom 8 alike, nearly all of it the character's
  parts.
- **Ellipses and rounded rectangles** are described by the horizontal extent of each
  row. An axis-aligned one (also under `SCALE`, after snapping its rectangle) is
  computed in 32-bit integers with one integer square root per row (the radicand
  scaled into [2^30, 2^32) and the root refined by its remainder, which gives the
  exactly rounded extent; no floating point, except that with `SHAPOGFX2D_FPU_SQRT`
  the root is seeded by the FPU and corrected to the same integer); the corners of a
  scaled rounded rectangle are elliptical. The integer `fillRoundRect()` /
  `drawRoundRect()` without a scaling transform stay in integers throughout (the
  radius is clamped to half the shorter side in integers). Under `AFFINE` an
  ellipse is a general one: the rectangle's conjugate semi-axes through the
  transform, each shortened by half a pixel (which
  puts the ends of an unturned one on the pixel centers there, like the integer
  extent), solved per row in float with one square root, and its row ends rounded to
  half pixels and then down like the integer extent, so a quarter turn gives the same
  pixels. A turned rounded rectangle is a convex polygon with the corners made of
  chords within a quarter pixel of the arc (at most 16 per corner). An outline row
  runs, on each side, from that row's own end inwards to just short of the nearer of
  the two neighboring rows' ends on that side, which yields a closed one-pixel outline
  consistent with the fill. Reaching to the *nearer* neighbor is what closes it where
  the edge is nearly flat -- the top and bottom of a circle, where consecutive rows'
  ends are many columns apart and the end pixels alone would leave a dotted line.
  Where the edge is steep the neighbors are one column away and the row is its end
  pixels. The outline of a turned rounded rectangle comes from the rows of its polygon
  the same way.
- **Arcs and sectors** are the pixels of `drawEllipse()` / `fillEllipse()` whose
  direction from the center lies within the angle range, so they share the
  ellipses' extents. Angles are radians, clockwise on screen from the +x axis,
  and parametric: an angle `t` is the direction of `(rx cos t, ry sin t)`, so 45
  degrees points at the corner of the rectangle and equal angles cut equal areas
  (a pie chart on an ellipse stays in proportion). Under a transform the angles are
  those of the untransformed ellipse (the direction goes through the transform, and
  a mirroring transform reverses the sense). The range runs from `start` to `end`
  taken modulo 2 pi after it; `end - start >= 2 pi` is the whole ellipse,
  `end == start` nothing. Each edge is rounded once to an integer direction (length
  8192, less for ellipses over 8192 pixels) and becomes a half-plane
  `a px + b py > 0` in coordinates relative to the center in 1/16 pixels; per row it
  limits the extent to one column range, found with two integer divisions. A range
  up to pi is the intersection of the half-plane after `start` and the one before
  `end`, a larger one the complement of the range from `end` to `start`. Ties are
  broken as if every pixel were moved by `(e, e^2)` for an infinitesimal `e`, so no
  pixel lies on an edge: sectors that share an angle neither overlap nor leave a gap,
  and the center pixel belongs to exactly one of them. Arcs and sectors whose center
  or size exceeds 2^24 pixels draw nothing.
- **Lines** walk the major axis with a 16.16 fixed-point minor coordinate and are
  clipped along the major axis before stepping; runs of pixels on the same row are
  filled as spans, steep lines pixel by pixel through the format's cursor, stepping
  a row pointer (the target format is switched once per line, not per pixel or
  run). Both end points are drawn. The walk is
  32-bit only: it works relative to the center of the clip rectangle, within +-16383
  pixels of it, and a line reaching further is halved (split points rounded to whole
  pixels) until its parts either miss the clip rectangle or fit.
- **Polygons** are filled per scanline with the even-odd rule, a pixel being inside
  where its center is. The vertices of the polygon API (`fillPolygon()`,
  `fillTriangle()`, integer or float) are pixels like the end points of lines: each
  lands on the pixel its center goes to through the transform, exactly as in
  `drawLine()`, and the edges run through the centers of those pixels. The fill
  therefore stays within the outline `drawPolygon()` draws through the same vertices
  (a center exactly on an edge counts as inside on the left and top edges only, where
  the outline's rounding puts its pixel on or right of it), and a polygon along the
  edges of a rectangle `(x, y)`-`(x + w, y + h)` fills the pixels of
  `Rect{x, y, w, h}`. The polygons of areas (turned rectangles, frames and rounded
  rectangles) keep continuous vertices instead, so that they cover the pixels a turned
  image of the same rectangle covers. Vertices are kept in 1/16 pixels, taken relative
  to the center of the clip rectangle and clamped to +-16383 pixels of it (only a
  vertex that far off screen moves). An edge covers the rows whose centers lie in
  `[y0, y1)`; on its first visible row it finds its column with one division (64
  bits only when it starts above the clip rectangle) and then steps exactly, with an
  integer and a remainder, in 32 bits. Edges shared by two polygons therefore give
  both the same columns: they neither overlap nor leave a gap. Up to 32 crossings per
  row are kept.
- **drawImage** converts between formats and picks a path by what it has to do:
  translation only without a color key goes to the per-pair row loops (same format
  16-bit copies `memcpy`; the other copies and alpha blends of ARGB4444 and of a
  format onto itself convert through RGB565, which is lossless for every color depth,
  GRAY1 targets keeping the `Color` luminance threshold; additive blending and the
  other pairs go through `Color` in chunks of 64 pixels on the stack); a scale (a
  destination rectangle, or a `SCALE` transform with the image's corners snapped like
  a rectangle's, so that it covers the pixels `fillRect()` covers) or a color key goes
  to the scaled path; a rotation or shear to the transformed path. Parts of the source
  rectangle outside the image are not drawn and leave their place empty. The overload
  with a convex polygon (`count` vertices as x, y pairs in image pixels relative to
  the top-left corner of `src`, texel corners, either winding, 3 to
  `IMAGE_POLYGON_MAX`) draws only the pixels whose source point lies in `src` and in
  the polygon (edges included), always through the transformed path whatever the
  transform is (with `SHAPOGFX2D_TRANSFORM=0` too, so the offset is not snapped the
  way `TRANSLATE` snaps it); fewer than 3 vertices mean no polygon, more than the
  maximum or a degenerate polygon draw nothing. It is what `rig` draws sprites with:
  the pixels of a sprite's transparent margin cost the walk and the alpha test
  whether or not they show, and a polygon around the opaque ones takes that off.
- **Scaled drawImage** stretches the source rectangle over the destination
  rectangle with nearest-neighbor sampling at pixel centers: destination pixel
  `t` of `dw` shows source pixel `floor((2t + 1) sw / 2dw)`. A negative
  destination width or height mirrors the image (the rectangle is normalized
  and the source counted from the far end); a negative source size is
  normalized. Sizes up to 32767 are accepted (larger ones draw nothing), which
  keeps every product of the mapping within 32 bits; the mapping is exact, without
  floating point. Per axis the call finds the visible range and the walker state
  with a few divisions; rows then step a DDA (source row = integer step plus a
  remainder that carries). Horizontally a reduction steps the same way per
  destination pixel; an enlargement walks the source pixels instead, each covering
  a run of `q` or `q + 1` destination pixels (Bresenham's run-slice), so that copies
  (with or without a color key) and ARGB4444 sprites write runs with `fill()` (or
  skip keyed runs) and convert every source pixel once. A plain copy into a 16-bit
  target `memcpy`s the previous row wherever consecutive rows show the same source
  row. The per-pixel ops are: a copy within a format, a keyed copy within a format,
  ARGB4444 sprites, a format onto itself with an opacity, and for everything else the
  `Color` conversion in chunks of 64 pixels (a keyed pixel becomes the Color 0, which
  a keyed copy skips and a blend draws with alpha 0).
- **Transformed drawImage** maps the source rectangle's coordinates (its top-left
  corner is the origin) through the transform and samples the pixel under each
  destination pixel center. The inverse transform is computed once in float;
  per row a float estimate picks a reference column inside the image's
  footprint, and from there the row is clipped exactly in 16.16 fixed point
  (the columns whose `u` and `v` lie in the part of the source rectangle inside
  the image), so the per-pixel walk (`u += du`, `v += dv`) needs no bounds
  check and never reads outside that part. A polygon narrows the row further:
  each edge is a half-plane in source coordinates, which along a row is a bound
  on the column that moves linearly with the row, so the setup turns the edges
  into (column at the first row, change per row) pairs -- lower bounds and upper
  bounds by the sign of the edge normal's component along the row, the edges
  perpendicular to it narrowing the row range once -- and a row costs one
  multiply-add per edge and two roundings, in float (the exact rectangle keeps
  the walk safe whatever this rounds to). Images up to 16384 pixels and
  transforms that shrink by at most 4096 are drawn (the limits keep the fixed
  point within 32 bits). On a core without an FPU the per-row setup is a few
  software float operations; the pixels are integer only.
- **drawBitmap and text** draw a 1-bit mask (a GRAY1 image, or a glyph of a GFXfont,
  addressed in bits) with the walkers of the images: runs of equal bits become spans
  in the mask's colors, so the blend and the transform apply. Without a transform, a
  mask with only a foreground (text) is written pixel by pixel per format. A
  transparent bitmap background leaves clear bits untouched.
- **Text** uses Adafruit `GFXfont` data. `setFont` computes the ascent (largest height
  above the baseline) and the line box height over all glyphs; the cursor is the
  top-left corner of the line box and glyphs are placed relative to the baseline
  `ascent` pixels below it. `background` (if not transparent) fills the box
  `xAdvance x lineHeight` of each glyph before drawing it. `'\n'` returns to the x of
  the last `setCursor()` and advances by `yAdvance`. Text is enlarged or turned by the
  transform. `charMetrics()` and `textMetrics()` give sizes in the coordinates of the
  drawing calls (so they lay out text under the same transform), as integers: font
  metrics are whole pixels, and no floating point is involved (which matters on cores
  without an FPU, such as the Cortex-M0+ or the ESP8266). `deviceCharMetrics()` and
  `deviceTextMetrics()` measure the same on the target, in float: `width` scaled by the
  length of the transform's x axis, `height`, `ascent` and `lineAdvance` by that of its
  y axis, so along the text's own axes and unchanged by a rotation (two square
  roots).

Every drawing function switches on the target format once per call (or per row),
never per pixel; the per-pixel loops are instantiated per format from the cursor
templates. The shapes walked by rows (ellipses, arcs, rounded rectangles, polygons,
frames) select the span function of the format once per call and call it per span.

Sizes of `src/gfx2d` (`-O2`, all formats but RGB565): 52.7 KB on a Cortex-M33 and 57.4 KB
on a Cortex-M0+; 39.3 KB on the M33 with `SHAPOGFX2D_TRANSFORM=0` and 35.3 KB with the
three 2D features off.

### Fonts (`fonts.hpp`, `gfxfont.h`, `font/*.h`)

`gfxfont.h` is the Adafruit GFXfont structure (BSD license, see LICENSE), moved into
`shapoco::gfx2d` (`gfx2d::GFXglyph`, `gfx2d::GFXfont`) with an include guard of its own,
so that it never collides with the `GFXfont` of the other libraries (Adafruit GFX,
LovyanGFX / M5GFX). The layout is that of Adafruit. The bundled fonts (generated with
ShapoFont) are `const GFXfont` objects in `shapoco::gfx2d`; each `font/*.h` can be
included alone:
`ShapoSansMono_s08c07`, `ShapoSansP_s05`, `ShapoSansP_s07c05a01`, `ShapoSansP_s08c07`,
`ShapoSansP_s12c09a01w02`, `ShapoSansP_s21c16a01w03`, `ShapoSansP_s27c22a01w04` and
`MameSeg7_s40c38w06` (7-segment; `.`, `0`-`9` and `A`-`F` only). Any GFXfont from the
Adafruit ecosystem can be used, by including its header inside the namespace so that it
is built from `gfx2d::GFXfont` (`namespace shapoco { namespace gfx2d {` /
`#include "FreeSans9pt7b.h"` / `} }`, after `gfxfont.h` and, on a target without
`PROGMEM`, `#define PROGMEM`). The `lgfx::GFXfont` of LovyanGFX / M5GFX is another
structure and cannot be used.

### Geometry (`math2d.hpp`)

`vec2f`, `colorf` (float RGBA used by the 3D API), `vec2i`, `Rect` (with `right()`,
`bottom()`, `contains`, `normalized`, `intersect`, `offset`), `RectF` (its float
counterpart, constructible from a `Rect`: `right()`, `bottom()`, `isEmpty`, `normalized`,
`offset`), `clamp01`, `clampInt`, `lerp`, and `affine2f`:

```c++
struct affine2f {            // x' = a x + c y + tx, y' = b x + d y + ty
  float a, b, c, d, tx, ty;
  static affine2f identity(), translation(x, y), scaling(sx, sy), scaling(s),
                  rotation(angle), rotation(angle, cx, cy), shearing(kx, ky);
  // the point (pivotX, pivotY) to (x, y), scaled by (sx, sy), then rotated about it
  static affine2f placement(x, y, angle, sx = 1, sy = 1, pivotX = 0, pivotY = 0);
  affine2f &translate(x, y), &scale(sx, sy), &scale(s), &rotate(angle), &shear(kx, ky),
           &multiply(const affine2f &);   // multiply on the right: applies first
  vec2f apply(x, y) const; vec2f apply(vec2f) const; vec2f applyLinear(vec2f) const;
  float determinant() const; bool invert(affine2f &out) const;
};
affine2f operator*(const affine2f &m, const affine2f &n);  // n first, then m
vec2f operator*(const affine2f &, const vec2f &);
```

Coordinates are continuous (pixel `(x, y)` covers `[x, x + 1) x [y, y + 1)`), angles
are radians and turn clockwise on screen. The member functions work like a canvas
context: `translation(x, y).rotate(a).scale(s).translate(-w / 2, -h / 2)` centers an
image on `(x, y)`, which is what `placement(x, y, a, s, s, w / 2, h / 2)` builds
directly.

### Vector graphics (`vg.hpp`)

`shapoco::gfx2d::vg` holds vector data: paths of lines and bezier curves, brushes
(a color or a gradient), stroke styles, and pictures (shapes with their brushes,
transforms and clips) that `Graphics2D` draws with `fillPath()`, `strokePath()`
and `drawPicture()`. The data is plain `const` structs that live in flash,
generated from SVG by `bin/svg2cpp` or written by hand (`PathBuilder`). The names
are those of vector formats in general, not of SVG. `gfx2d.hpp` includes the
header (through `graphics2d.hpp`).

```c++
constexpr uint16_t FORMAT_VERSION = 1;      // members so far (as rig: generated headers assert on it)
constexpr uint16_t SUPPORTED_FEATURES = 0;  // bits of Picture::features this build honors (none yet)
enum class PathOp : uint8_t { MOVE, LINE, QUAD, CUBIC, CLOSE };   // 2, 2, 4, 6, 0 coordinates
constexpr int pathOpCoords(PathOp);
enum class FillRule : uint8_t { NONZERO, EVEN_ODD };
struct Path { const uint8_t *ops; const float *coords; uint16_t opCount, coordCount;
              FillRule rule; uint8_t pad[3]; RectF bounds; };               // 32 B
RectF pathBounds(const Path &);              // of the points, control points included
class PathBuilder {                          // into arrays the caller provides
  PathBuilder(uint8_t *ops, int opCapacity, float *coords, int coordCapacity);
  PathBuilder &moveTo(x, y), &lineTo(x, y), &quadTo(cx, cy, x, y), &cubicTo(c1x, c1y, c2x, c2y, x, y), &close();
  PathBuilder &rect(x, y, w, h), &roundRect(x, y, w, h, rx, ry), &ellipse(cx, cy, rx, ry), &circle(cx, cy, r),
              &polyline(const vec2f *, int n, bool closed);
  bool overflowed() const; int opCount() const; int coordCount() const;
  Path path(FillRule = NONZERO) const;       // bounds included
};
enum class GradientKind : uint8_t { LINEAR, RADIAL };
enum class Spread : uint8_t { PAD, REFLECT, REPEAT };
struct GradientStop { float offset; Color color; };          // offsets 0..1 in order
struct Gradient { GradientKind kind; Spread spread; uint8_t stopCount, pad;
                  const GradientStop *stops; affine2f toGradient; };        // 32 B
Gradient linearGradient(const vec2f &p0, const vec2f &p1, const GradientStop *, int n, Spread = PAD);
Gradient radialGradient(const vec2f &center, float radius, const GradientStop *, int n, Spread = PAD);
struct Brush { Color color; const Gradient *gradient; };    // gradient == nullptr: solid
constexpr Brush solidBrush(Color), gradientBrush(const Gradient *, int opacity = 255), NO_BRUSH;
enum class LineCap : uint8_t { BUTT, ROUND, SQUARE };
enum class LineJoin : uint8_t { MITER, ROUND, BEVEL };
struct StrokeStyle { float width; LineCap cap; LineJoin join; uint8_t pad[2]; float miterLimit; };
constexpr StrokeStyle strokeStyle(float width, LineCap = BUTT, LineJoin = MITER, float miterLimit = 4);
enum class ShapeKind : uint8_t { PATH, IMAGE, TEXT };
enum ShapeFlags : uint8_t { SHAPE_FILL_CURRENT_COLOR = 1, SHAPE_STROKE_CURRENT_COLOR = 2 };
struct Text { const char *text; const GFXfont *font; float x, y; };  // (x, y) on the baseline
struct Shape { ShapeKind kind; uint8_t flags, pad[2]; const void *data;  // Path, Texture or Text
               Brush fill, stroke; StrokeStyle strokeStyle; affine2f transform; const RectF *clip; };  // 64 B
struct Picture { const Shape *shapes; uint16_t shapeCount, features; RectF bounds; };
```

- **Paths**: every op but the first continues from the current point; a LINE, QUAD or
  CUBIC right after CLOSE (or first) starts at the last MOVE. Filling closes every
  subpath; stroking closes only those ending in CLOSE. Coordinates are continuous like
  those of `affine2f` (a path along the edges of the rectangle `(x, y)-(x + w, y + h)`
  fills the pixels of `Rect{x, y, w, h}`). `bounds` is used to size the coverage
  buffer and to skip paths outside the clip; left empty, it is computed when drawn.
- **Brushes**: a `Brush` is a color, or a gradient whose colors the alpha of `color`
  scales (its RGB is ignored). A gradient lives in its own space: LINEAR runs along x
  from 0 to 1, RADIAL from the origin to the unit circle; `toGradient` maps the
  coordinates of the drawing calls to that space, so a gradient between two points, an
  ellipse or a tilted one are all the same code (`linearGradient()` maps p0 to 0 and p1
  to 1 along x, `radialGradient()` the center to the origin and the radius to 1).
  Beyond its ends a gradient shows its end colors (PAD), itself mirrored (REFLECT) or
  itself again (REPEAT). Colors are interpolated between the stops (not premultiplied)
  into a table of 64 entries once per call; a pixel's position is stepped in Q16 from
  an exact value at the start of every run of 32 pixels (no drift), the radial
  distance by `isqrt32` of the Q12 components (clamped to 8 radii); no floating point
  per pixel. The brushes of the state default to opaque white.
- **Strokes**: `width` is centered on the path, in the coordinates of the drawing
  calls, so the transform scales it (non-uniformly too). Caps BUTT / ROUND / SQUARE,
  joins MITER / ROUND / BEVEL with `miterLimit` as in SVG (a miter beyond
  `miterLimit x width / 2` from the corner is beveled). A zero-length subpath
  (`M L` to the same point, or `M Z`) draws a dot with ROUND caps and a square with
  SQUARE ones; a lone MOVE draws nothing (as in SVG). Each segment becomes a
  quadrilateral, each corner a join polygon, each end a cap, every loop turned the same
  way and all filled at once with the nonzero rule, so a translucent stroke is painted
  once where its pieces overlap. Round joins and caps are polygons of 8, 16 or 32
  vertices by their radius on screen (no trigonometry at run time). Dashes are not
  supported (svg2cpp splits static dashes into subpaths).
- **Drawing**: curves are flattened when drawn, into `ceil(sqrt(1.5 L))` segments
  for a control polygon of `L` pixels on the target (at most 64; the coefficient in
  `segmentsFor()` of `vg.cpp` sets the density), so a picture scaled up keeps its
  round corners (the error is about a tenth of a pixel). The edges go to the
  scratch memory of the arena (16 + 2 bytes each, a stroke about 7 per segment),
  sorted by their top row, and are scanned as the polygons of `shapes.cpp` are
  (columns in 1/16 pixel, a pixel inside where its center is, edges stepped by a DDA
  in 1/16384 of a 1/16 pixel) with the crossings of each row sorted (at most 64; the
  rest are dropped) and joined by the fill rule (`FillRule` of the path for fills,
  always nonzero for strokes). When the curves would make more edges than the memory
  holds, a fill's curves are flattened coarser (the segment count goes with the square
  root of the length, so by the square of the shortfall; strokes are not coarsened,
  since their loops are convex and may be cut); a loop that still does not fit is cut
  and both parts closed with the chord to its first point, which is exact for a convex
  loop (every loop of a stroke) and may show the chord on a concave fill; the parts of
  a path are drawn one after the other, which can show where translucent parts meet.
  Without an arena a buffer of 24 edges on the stack is used the same way. The fill
  brush, blend mode and opacity of the state apply; `NONE` copies.
- **Antialiasing** (`setAntialias()`, off by default; `SHAPOGFX2D_ANTIALIAS`): the
  path is scanned on 4 sub-rows per pixel row, each sampled at its center, and the
  1/16 columns inside on each sub-row are summed into the coverage of the pixel,
  0..64, which is directly the alpha64 of the blend (so a pixel half covered gets half
  the color's alpha x opacity). The coverage of a row is accumulated as deltas (2
  bytes per pixel of the path's width, in the scratch memory or, up to 128 pixels, on
  the stack; without room the path is drawn without antialiasing) and painted in runs
  of equal coverage for a solid brush, per pixel for a gradient. Not with the `NONE`
  blend mode, which copies. The other calls (rectangles, polygons, images, text) are
  not antialiased.
- **Pictures**: `drawPicture()` draws the shapes in order, each under
  `transform of the state x Shape::transform`, with its own brushes and stroke style
  (a shape whose `fill` has no alpha and no gradient is not filled, one whose `stroke`
  has none or whose width is 0 is not stroked). `SHAPE_FILL_CURRENT_COLOR` /
  `SHAPE_STROKE_CURRENT_COLOR` replace the RGB of the brush's color by `currentColor`
  (SVG's `currentColor`; the slot's color in rig), keeping its alpha and gradient.
  `clip` (in the picture's space, through the transform of the state) becomes the clip
  rectangle of the context for the shape: exact without a rotation, else its bounding
  box; a shape clipped away entirely is skipped. `SHAPE_STROKE_CURRENT_WIDTH`
  replaces the width of the stroke style by `currentStrokeWidth` (the stroke width
  of the slot in rig, which an animation changes). IMAGE shapes draw the texture with
  its pixels at `(0, 0)-(width, height)` of the shape's space (`drawImage()` under the
  transform, nearest neighbor) with the alpha of `fill.color` as the opacity; TEXT
  shapes draw `text` with the bitmap font `font` (or the context's font) in
  `fill.color`, the baseline at `(x, y)` (the cursor `ascent` above). The state is
  restored afterwards (no state stack is used). A picture with a bit of `features`
  outside `SUPPORTED_FEATURES` draws nothing.
- **Room for later features**: as for rig, generated headers initialize the
  structures by position, members are only appended and zero means "not used", so
  `FORMAT_VERSION` and `Picture::features` leave room for dashes, masks, patterns and
  the like.

Cost: `src/gfx2d/vg.cpp` is 26.0 KB on a Cortex-M33 and 31.4 KB on a Cortex-M0+
(`-O2`; about 3 KB of it the antialiased lines, outlines, sectors and frames).
The fills, lines and outlines reference it (for their antialiased path), so a
program using `Graphics2D` links it; the sampled masks and the bilinear images add
11.4 KB to `images.cpp` (a sampler per source format), and the routing about
1.5 KB to `graphics2d.cpp` and `shapes.cpp`. With antialiasing on, the fills
and the Wu lines of demorig's stars, ring and buttons cost about twice the plain
ones on the host; the bilinear images dominate the frame (see "Antialiasing"
under `Graphics2D`). Off, only a flag is tested per call. A stroked segment
costs its quadrilateral and a join polygon (3 or 4 vertices; 8 to 32 for round
ones); the pixel work is that of the polygons of `shapes.cpp` plus, with
antialiasing, four sub-rows per row and the per-pixel blend along the edges.

### Skeletal animation (`rig.hpp`, optional)

`shapoco::gfx2d::rig` poses armatures (trees of bones carrying images or vector
pictures) from keyframed animations and draws them with `Graphics2D`. The data is
static (`static const`, in flash) and is generated from DragonBones by
`bin/dbones2cpp` and from animated SVG by `bin/svg2cpp`; `rig::Instance` keeps
the pose of one armature in memory the user provides, and `drawBind()` draws the
bind pose without one. `gfx2d.hpp` does not include the header. Conventions are
those of DragonBones: y down, angles clockwise, Flash matrices (`x' = a x + c y +
tx`, as `affine2f`).

```c++
using angle16_t = int16_t;  // 1/65536 turn: differences wrap to the shortest way
using scale16_t = int16_t;  // Q12 (SCALE_ONE = 4096)
constexpr uint16_t FORMAT_VERSION = 3;      // members added so far (see "Room for later features")
constexpr uint16_t FEATURE_SLOT_COLOR = 1;    // Slot::colorR / G / B and COLOR timelines are meaningful
constexpr uint16_t FEATURE_STROKE_WIDTH = 2;  // Slot::strokeWidth and STROKE_WIDTH timelines are meaningful
constexpr uint16_t SUPPORTED_FEATURES = FEATURE_SLOT_COLOR | FEATURE_STROKE_WIDTH;
enum class AttachmentKind : uint8_t { IMAGE, MESH, ARMATURE, BOUNDING_BOX, VECTOR };  // IMAGE and VECTOR are drawn
struct Bone { const char *name; float x, y; angle16_t rotX, rotY;
              scale16_t scaleX, scaleY; uint8_t parent, flags; };   // 24 B (32-bit)
struct Attachment { const Texture *texture; Rect src; affine2f local;
                    const int16_t *hull; uint8_t hullCount; AttachmentKind kind;
                    uint8_t pad[2]; const void *ext; };              // 56 B; VECTOR: ext = const vg::Picture *
struct Slot { const char *name; const Attachment *attachments; uint8_t attachmentCount;
              int8_t defaultAttachment; uint8_t bone, alpha; BlendMode blend;
              uint8_t colorR, colorG, colorB;                        // the slot's color (FEATURE_SLOT_COLOR)
              const RectF *clip; uint8_t clipBone, pad[3];           // version 2
              float strokeWidth; };                                  // 28 B; version 3 (FEATURE_STROKE_WIDTH)
struct Armature { const char *name; const Bone *bones; const Slot *slots;
                  uint8_t boneCount, slotCount; bool colorKeyEnabled; Color colorKey;
                  RectF bounds; uint32_t signature; uint16_t features; };
struct Curve { int16_t y[17]; };  // easing at x = i / 16, Q14
struct TranslateKey { uint16_t frame; uint8_t curve, pad; float x, y; };
struct RotateKey { uint16_t frame; uint8_t curve; int8_t turns; angle16_t rotX, rotY; };
struct ScaleKey { uint16_t frame; uint8_t curve, pad; scale16_t scaleX, scaleY; };
struct AttachmentKey { uint16_t frame; int8_t attachment; uint8_t pad; };
struct AlphaKey { uint16_t frame; uint8_t curve, alpha; };
struct ColorKey { uint16_t frame; uint8_t curve, r, g, b, pad[2]; };   // version 2
struct StrokeWidthKey { uint16_t frame; uint8_t curve, pad; float width; };   // version 3
enum class Channel : uint8_t { TRANSLATE, ROTATE, SCALE, ATTACHMENT, ALPHA, COLOR, STROKE_WIDTH };
struct BoneTimeline { const void *keys; uint16_t keyCount; uint8_t bone; Channel channel; };
struct SlotTimeline { const void *keys; uint16_t keyCount; uint8_t slot; Channel channel; };
struct DrawOrderKey { uint16_t frame; const uint8_t *order; };
struct Animation { const char *name; uint16_t duration; uint8_t frameRate,
                   boneTimelineCount, slotTimelineCount, curveCount;
                   uint16_t drawOrderKeyCount; const BoneTimeline *boneTimelines;
                   const SlotTimeline *slotTimelines; const DrawOrderKey *drawOrderKeys;
                   const Curve *curves; uint32_t signature; uint16_t features; };
float frameAt(const Animation &, float seconds, bool loop = true);
void drawBind(Graphics2D &, const Armature &);   // the bind pose, without an Instance
```

- **Bones** are ordered parents first (`parent` is a smaller index or `NO_PARENT`;
  indices are `uint8_t`, at most 255 bones and slots). A bone's local transform is
  `a = cos(rotY) sx, b = sin(rotY) sx, c = -sin(rotX) sy, d = cos(rotX) sy,
  (tx, ty) = (x, y)` (DragonBones' skX / skY: equal for a rotation, different for a
  skew); its world transform is the parent's world transform times the local one
  (a plain affine product, so a child of a non-uniformly scaled bone is sheared).
- **Slots** are in the base draw order. A slot shows one of its attachments (-1:
  none). An IMAGE attachment maps the top-left corner of `src` (a part of a texture
  atlas) to the bone's space; `texture == nullptr` marks a display that is not drawn
  (an unsupported DragonBones display kept so that the indices match). `hull` is a
  convex polygon around the opaque pixels of `src` (x, y pairs relative to its
  top-left corner, at most `Graphics2D::IMAGE_POLYGON_MAX` vertices; `nullptr` / 0
  for the whole rectangle), the polygon `draw()` clips the image to. A VECTOR
  attachment is a `vg::Picture` behind `ext`, `local` mapping the picture's space to
  the bone's, drawn with `drawPicture()` in the slot's color. `alpha` is the
  slot's opacity, `blend` ALPHA or ADD. `colorR / G / B` is the slot's color when
  `FEATURE_SLOT_COLOR` is set in `Armature::features` (white otherwise): the
  `currentColor` of its vector pictures (SVG's `currentColor`, and the fill or
  stroke an animation changes); for images it is reserved as a tint, not applied
  today. `strokeWidth` (with `FEATURE_STROKE_WIDTH`; 1 otherwise) is the width of
  the strokes of its pictures flagged `SHAPE_STROKE_CURRENT_WIDTH`, in the
  picture's coordinates. `clip` is a rectangle in the space of bone `clipBone` the slot is clipped
  to (`nullptr`: none), drawn as the clip rectangle of the `Graphics2D`: exact
  while that bone is not turned on screen, else its bounding box.
- **Animations** hold one timeline per channel of a bone (TRANSLATE, ROTATE, SCALE)
  or a slot (ATTACHMENT, ALPHA, COLOR, STROKE_WIDTH), sorted by bone / slot and
  channel, with keys in frame order starting at frame 0. Bone key values are offsets
  added to the bind pose (angles wrap in int16, positions add in float), scale keys
  multiply it (Q12); slot key values replace the slot's (COLOR keys the RGB of its
  color, interpolated per component like alphas; STROKE_WIDTH keys its stroke width,
  in float). `curve` is the easing from a key to the next:
  `CURVE_LINEAR`, `CURVE_STEP` (hold) or an index into `curves`. Draw order keys
  hold complete orders (draw position -> slot; `nullptr` returns to the base order).
  `signature` (FNV-1a of the bone and slot names) ties an animation to its armature.
- **Room for later features** (mesh deformation, skinning, IK, nested armatures,
  tints, ...): the structures are meant to grow. Generated headers initialize them
  by position, so members are only appended, a header generated before a member
  leaves it zero, and zero (or the enumerator 0) means "not used" for every
  appended member. `FORMAT_VERSION` counts the members added this way; a generated
  header `static_assert`s on the version it needs, so old library code refuses new
  data at compile time instead of misreading it (a header generated before a member
  compiles with a `-Wmissing-field-initializers` warning under `-Wextra`). The
  members added so far: version 1 reserved `Bone::flags` (inheritance of rotation /
  scale / reflection; fills the padding), `Attachment::kind` / `ext`,
  `Slot::colorR / G / B` (then named the tint; dbones2cpp writes 255, 255, 255, and
  since an older header leaves 0, 0, 0 they are only read once `FEATURE_SLOT_COLOR`
  says the data has them; fill the padding), `RotateKey::turns` (extra whole turns,
  `clockwise` / `tweenRotate`; was padding) and `Armature::features` /
  `Animation::features`, bits the data uses; version 2 added `AttachmentKind::VECTOR`
  (its data behind `ext`), `Slot::clip` / `clipBone`, `Channel::COLOR` with
  `ColorKey`, and defined `FEATURE_SLOT_COLOR`; version 3 added `Slot::strokeWidth`
  and `Channel::STROKE_WIDTH` with `StrokeWidthKey` under `FEATURE_STROKE_WIDTH`.
  Still unused: `Bone::flags`,
  `RotateKey::turns`, the kinds MESH / ARMATURE / BOUNDING_BOX and the tint of
  images. `init()`, `pose()` and `drawBind()` refuse data with a bit outside
  `SUPPORTED_FEATURES`, since drawing it without the feature would show something
  else than what was made.
- `frameAt()` converts seconds to a frame (`seconds * frameRate`), wrapped to
  `[0, duration)` or clamped to `[0, duration]`.

`rig::Instance` (a small handle; copies share the memory):

- `static size_t bytes(const Armature &)`: 24 bytes per bone (world transforms), 20
  per slot (bounding box, attachment, alpha, color, stroke width) and 1 per slot (draw order),
  rounded up to 4, plus 3 bytes of alignment slack. `init(armature, memory, size)` fails if the
  memory is too small or `features` has a bit outside `SUPPORTED_FEATURES`, and starts
  in the bind pose; nothing is allocated.
- `pose(anim, frame, visitor = nullptr)` clamps `frame` to `[0, duration]` (NaN to 0)
  and refuses (false, nothing changes) an animation whose signature differs or whose
  `features` has a bit outside `SUPPORTED_FEATURES`. Per
  bone it starts from the bind values, adds each of its timelines (the key at or
  before the frame, `k0`, and the next, `k1`: `t = (frame - k0.frame) / (k1.frame -
  k0.frame)` in float; the Q14 easing `e` is `0` for STEP, `(int)(t * 16384)` for
  LINEAR, else the table interpolated linearly at `u = 16 t`; values are `a + (b -
  a) e / 16384` in float for positions, `a + (((int16)(b - a) e) >> 14)` for angles
  and `a + (((b - a) e) >> 14)` for scales and alphas), calls
  `BoneVisitor::onBone(bone, BonePose &)` if given, and multiplies the local matrix
  onto the parent's. Its sines and cosines come from a 257-entry quarter-wave Q15
  table interpolated linearly (error below 5e-5; one pair when rotX == rotY), not from
  libm, whose `sinf()` / `cosf()` cost thousands of cycles in software floating point
  and 3.5-4 KB of code. Then each slot takes its
  attachment, alpha, color and stroke width, and its bounding box in the armature's space (the four
  corners of `src`, or of the picture's `bounds`, through `world * local`, floor /
  ceil, int16; an attachment of a reserved kind, an `IMAGE` without a texture or a
  `VECTOR` without a picture gets an empty box and is not drawn). The draw order is
  that of the last key at or before the frame, copied only when it changes (an order
  with an index out of range falls back to the base order). `poseBind()` does the
  same without an animation. `bin/shapogfx_dbones.py` evaluates poses with the same
  arithmetic (float32 progress, integer easing), the reference of the tests.
- `draw(g)` / `draw(g, first, end)` draw the draw positions `[first, end)` (clamped)
  with `g`'s transform as the placement of the armature: per visible slot with a
  non-zero alpha, `setTransform(placement * world[bone] * local)` and
  `drawImage(texture, 0, 0, src, hull, hullCount)` (the transformed path; the hull
  cuts only pixels that would not have shown, so the picture is that of the whole
  rectangles) or `drawPicture(picture, color, strokeWidth)`, with the clip rectangle of `g`
  narrowed to the slot's `clip` (through `placement * world[clipBone]`) while it is
  drawn. The slot's alpha scales
  `g`'s opacity, ADD slots draw additively unless `g`'s blend mode is NONE, and a
  keyed armature sets its key color for its keyed textures and clears it for its
  ARGB4444 ones (a conversion with `--out-format auto` mixes them; comparing pixels
  that have alpha with a key would only cost). Slots whose bounding box, mapped
  by the placement, lies outside `g`'s clip rectangle (a pixel wider) are skipped,
  so drawing in bands costs little. The transform, opacity, blend mode, color key
  and clip rectangle of `g` are restored (no state stack is used); the vector
  pictures use `g`'s antialiasing flag. Without `SHAPOGFX2D_TRANSFORM`
  nothing is drawn. Drawing something between two slots is `draw(g, 0, k)`, the
  drawing, `draw(g, k, n)` with `k = drawIndexOf(slot)`.
- `drawBind(g, armature)` draws the bind pose (default attachments, alphas and
  colors, the base order) the same way without an `Instance`, computing each bone's
  world transform up its chain of parents on the fly: no memory, more arithmetic per
  frame for a deep tree. It is the way to draw a converted SVG that is not animated
  as an armature (svg2cpp emits a plain `vg::Picture` for those unless asked for the
  rig).
- Accessors: `drawIndexOf(slot)`, `slotAt(drawIndex)`, `boneIndex(name)`,
  `slotIndex(name)` (linear `strcmp`, -1 if none), `boneTransform(bone)` (armature
  space), `attachmentOf` / `setAttachment`, `alphaOf` / `setAlpha`, `colorOf` /
  `setColor` (the color's alpha is ignored) and `strokeWidthOf` / `setStrokeWidth`
  (overrides until the next pose),
  `bounds()` (union of the visible slots' boxes) and `bounds(placement)` (the box of
  its corners after `placement`).

Drawing on several cores: `draw()` reads the `Instance` and the armature and
changes nothing but the state of the `Graphics2D` it is given, and `gfx2d` has no
writable global state, so several cores may draw different bands of one frame from
one `Instance` at the same time. The conditions:

- Every core draws with a `Graphics2D` of its own (with an arena of its own, if it
  has one). One context must not be used by two cores at once.
- The `Instance` does not change while anyone draws: `pose()`, `poseBind()`,
  `setAttachment()`, `setAlpha()`, `init()` and `deinit()` wait until every core is
  done. A copy of an `Instance` shares its memory and is no way around this.
- The cores write different pixels: targets that do not overlap (a `Surface` per
  band, with the placement translated by the band's position), or one target with
  clip rectangles that do not overlap.

The SIO interpolator that the transformed `drawImage()` uses on RP2040 / RP2350
belongs to the calling core and is saved and restored. The bands put the same
pixels as one call; the M5Stack builds of demorig draw every strip this way.

Cost, measured on the rgb_chan character of demorig converted at scale 0.5 (29 bones, 41 slots): `pose()`
retires about 17,000 instructions on x86-64 (13,700 of them for the bones without
timelines); `draw()` adds about 3,000 to the `drawImage()` calls it makes, which do
the pixel work (a transformed ARGB4444 blit over the parts' footprints, about 51,000
source pixels here). Drawing in 8 bands of 40 rows costs 3% more than at once
(8% more without the clip skip), and a band the character misses costs 3,000
instructions (27,000 without the skip). `src/gfx2d/rig.cpp` is 8.5 KB on a
Cortex-M33 and 11.0 KB on a Cortex-M0+ (`-O2`, code and the sine table; 5.1 / 6.4
KB before the vector attachments, the clips and `drawBind()`); it is not linked in
when unused, and the vector drawing (`vg.cpp`) only when a picture is drawn.

What the pixel work costs is decided by the images: on x86-64 a transformed
ARGB4444 pixel onto RGB565 retires about 25 instructions when it is transparent, 50
when opaque and 70 when translucent, and the rectangle of a limb drawn diagonally
is mostly transparent. On the microcontrollers, whether the images fit the cache
in front of the flash weighs as much as the instruction count: every part is read
once per frame, so when their bytes exceed the cache they come from the flash
every frame (M5Stack Tab5, 256 KB L2: 30 fps with the 321 KB the atlas rows
touched, 42 fps once the per-image textures brought that to 216 KB; the CoreS3's
64 KB holds neither and did not move). `--scale`, `--fit-rotate` and
`--atlas-width 0` all shrink that footprint. The hulls dbones2cpp emits by default (`--hull 8`) take the
demorig frame of 320 x 240 from 4.11 to 3.88 million instructions (-5.5%), the
640 x 360 one from 8.34 to 7.63 million (-8.6%) and the frame zoomed in twice by
10%, with the same pixels on screen; `--fit-rotate` on top takes 13% off the atlas
(338 to 296 KB) and about 1% more off the zoomed frames, at the price of one
resampling of the turned images.

Not supported (dbones2cpp warns and drops them): mesh deformation (FFD, weighted
meshes), IK, nested armatures, events, the RGB tint of image slots, extra turns
(`clockwise` / `tweenRotate`), bones not inheriting rotation or scale, several skins
at run time, blending of animations. The structures hold room for them (see "Room
for later features"): each would be data behind `Attachment::ext`, `Bone::flags`,
the tint of `Slot` or `RotateKey::turns`, constraint and timeline arrays appended to
`Armature` / `Animation` under a bit of `features`, and more memory behind
`Instance::bytes()`.

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
using gfx2d::Surface;   // render target: RGB565_SWAPPED, RGB565 or RGB444

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
  uint8_t alphaCutoff, wrap, shininess;  // reserved (were padding); 0
  uint32_t flags;          // MaterialFlags
};

enum class PrimitiveType : uint8_t {
  TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN,   // lit, textured, culled
  POINTS, LINES, LINE_STRIP, LINE_LOOP       // unlit, 1 px (points: pointSize), not culled
};

struct Primitive {
  PrimitiveType type;
  uint8_t flags;                         // reserved (was padding); 0
  uint16_t indexCount;
  const VertexBuffer *vertexBuffer;      // either vertex form
  const uint16_t *indices;
  const Material *material;  // nullptr: use the material set by setMaterial()
};

struct Config {                  // from defaultConfig(), adjusted as needed
  int16_t screenWidth = 0, screenHeight = 0;
  void *arena = nullptr; size_t arenaSize = 0;
  int spanCapacity = 0;         // per render context; 0: a quarter of the arena left, clamped to 32..512
  int renderContexts = 1;       // render() calls that may run at the same time (1..4)
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
  int spanCapacity, spanPeak, spanDropped;  // per context; busiest context; all contexts
  int badIndices;    // triangles dropped because an index was >= vertexCount
  int nodesDropped;  // nodes skipped because the state stack was full
};

// Static scene description (see "Static scenes")
constexpr uint16_t MODEL_FORMAT_VERSION = 1;  // members added so far (see "Room for later features")
struct Mesh  { const Primitive *primitives; uint16_t primitiveCount; uint16_t flags; };
struct NodeTRS { vec3f translation; float rotation[4]; vec3f scale; };  // T * R * S = transform
struct Node  { const char *name; mat4f transform; const Mesh *mesh;
               const Node *const *children; uint16_t childCount;
               uint16_t flags; const NodeTRS *trs; };
struct Scene { const Node *const *roots; uint16_t rootCount;
               uint16_t nodeCount; const Node *const *nodes; };  // all nodes, glTF order
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
  // The same with render context ctx; calls with different contexts may run at
  // the same time (e.g. one per core, each on its own band)
  void render(int ctx, int16_t x, int16_t y, int16_t w, int16_t h,
              const Surface &dst, int16_t dstX = 0, int16_t dstY = 0);

  Stats getStats() const;
  // Triangle-buffer bytes one primitive takes (record + entry), by what it holds;
  // depends on the build and on Config::renderContexts
  size_t primitiveBytes(bool depth, bool smooth, bool textured) const;
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
  depth plane (12 bytes less each, 8 with `SHAPOGFX3D_DEPTH_BITS=16`) and it is not sorted. Use it for geometry that
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

**Room for later features** (skinning, keyframe animation, morph targets, more
material parameters): as in `rig`, generated headers initialize the model structures
by position, so from `MODEL_FORMAT_VERSION` 1 on members are only appended, a header
generated before a member leaves it zero, and zero means "not used" for every
appended member; a generated header `static_assert`s on the version it needs. The
renderer reads none of the reserved members. Where padding was, it holds them for
free: `Material::alphaCutoff` / `wrap` / `shininess` (alpha test threshold, texture
wrap modes, specular exponent), `Primitive::flags` (per-primitive options; `type` and
`indexCount` moved next to it, so a `Primitive` is 16 bytes), `Mesh::flags`,
`Node::flags` and `Scene::nodeCount`. Appended: `Node::trs`, the translation /
rotation / scale the transform was composed from (what an animation changes one
component of; gltf2cpp emits it for nodes given as TRS, nullptr for a matrix), and
`Scene::nodes`, every node in the order of the source file, which is what glTF
animation channels and skins refer to. Skins, morph targets and animations
themselves would be appended arrays and a run-time state object of their own.
`VertexBuffer` and `FixedVertex` keep their layout: their padding lies after
`vertexCount` / before `color`, where a member would break the hand-written
`{count, vertices}` idiom, and neither has a plausible per-buffer or per-vertex use.

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
ARGB4444 are expanded, RGB565_SWAPPED is byte-swapped, RGB565 is taken as it is. An
ARGB4444 texture supplies a
per-texel alpha `a4` (0..15); the triangle becomes translucent, and a material with
`BlendMode::NONE` is rasterized as `ALPHA` with `opacity = a4 / 15`, while `ALPHA`
and `ADD` multiply their opacity by `a4 / 15`. Texel alpha 0 skips the pixel.

## Memory management

`init()` aligns the arena to 8 bytes and carves it as follows:

1. **Fixed part**: the render contexts (`Config::renderContexts`, each with line
   buckets of 2 x screen height x `uint16_t`), layer table (`SHAPOGFX3D_LAYER_MAX`
   entries of 8 bytes), matrix stack (16 entries), vertex cache (64 entries).
2. **Span pools**: one per render context, `Config::spanCapacity` spans of 16 bytes
   each, or when it is 0 a quarter of the remaining space shared among the
   contexts, clamped to 32..512 spans per context (so 512 spans, 8 KB, per context
   unless the arena is small). Spans beyond the capacity are dropped, which leaves
   holes in the picture, so a tuned value is one that keeps `Stats::spanPeak` below
   it with margin for the worst frame; whatever it saves goes to the triangle
   buffer.
3. **Triangle buffer**: everything that remains.

The triangle buffer holds records of different sizes (see the table above), so it
is a byte budget rather than a triangle count: the records are packed downwards
from the end of the region while the entries (a 2-byte record offset, plus a
2-byte scanline link per render context: 4 bytes with one context) grow upwards
from its start, and the buffer is full when the two meet. The entries are
contiguous; `beginRender()` places the links of every context in the space
between them and the records, which the entry size reserves.
`Stats::triBytes` and `Stats::triBytesTotal` report both ends of it. Records are
addressed by a 4-byte-unit offset, which caps the region at 256 KB; a larger arena
leaves the excess unused. `primitiveBytes(depth, smooth, textured)` returns what one
primitive takes (its record and its entry) in the build and configuration at hand,
for applications that size their arena from a scene budget.

`SHAPOGFX3D_STACK_DEPTH` and `SHAPOGFX3D_VCACHE_SIZE` size the fixed part (about
1.1 KB and 2.8 KB at their defaults). A smaller vertex cache costs re-transformed
vertices, not correctness.

If the arena is too small for the fixed part, `init()` leaves the renderer
uninitialized. Overflowing buffers drop the excess for the current frame.

### Render contexts (multi-core rendering)

Everything `render()` changes -- the span pool and span lists, the per-scanline
triangle lists and the links of the active list -- belongs to a render context;
the scene (records, entries, sort order) is only read between `beginRender()` and
`endRender()`. With `Config::renderContexts = n` a renderer holds n contexts, and
`render(ctx, ...)` calls with different contexts may run at the same time: on a
dual-core RP2040/RP2350 core 0 renders the upper half with context 0 while core 1
renders the lower half with context 1, which roughly halves the time of the span
stage and the pixel loops together. `beginScene()` ... `beginRender()` run on one
core, and `endRender()` after both halves are done. Each extra context costs a span
pool, 4 bytes per screen row and 2 bytes per primitive. `render()` without a context
uses context 0. The SIO interpolators are per core, so the RP2 paths work on both
cores. The tests render two halves on two threads (checked with ThreadSanitizer)
and compare with a single call byte for byte.

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
- A vertex is converted when it is fetched (three float-to-int conversions; a
  `PackedVertex` is decoded in integers instead, its buffer's scale as a
  normalized fixed-point value and its bias in 16.16 converted once per
  primitive, so a glTF mesh costs no float operation per vertex),
  transformed with 32x32 -> 64 multiplies into 16.16 view space, and projected
  with one division: 1/w comes from a normalized reciprocal (one 32-bit
  hardware division gives 16 bits of it, and a quotient is the top 16 bits of
  the dividend times it, a 32-bit product, for about 15 significant bits),
  which also serves the plane setup, where the determinant's reciprocal is
  shared by every gradient of the triangle. Screen coordinates are 16.16 px,
  depth 8.24, colors 8.8.
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

### Placing the rasterization side

With `SHAPOGFX3D_HOT_ATTR` (and `SHAPOGFX3D_HOT_INSTANTIATE=1` for the templates) the
following get the attribute, for example to run them from RAM
(`.time_critical.*` on the Pico SDK, `.iram1.*` on the ESP8266):

- `Graphics3D::render()`, both overloads;
- the span-list functions of `gfx3d.cpp`: `allocSpan`, `cutSpan`, `appendTranslucent`,
  `insertOpaque`, `insertTranslucent`, `clipTranslucent`, `mergeLists` (static
  functions taking the render context; they were members before the render
  contexts);
- the rasterizers `rasterSpanT<blend, texture format, flat, output format>`, one per
  non-null entry of the rasterizer table of each enabled output format (blend modes
  NONE/ALPHA/ADD, the flat variant always and the smooth one with
  `SHAPOGFX3D_GOURAUD`, the blending ones only with `SHAPOGFX3D_BLEND`, texture
  format NONE plus each enabled texture format with `SHAPOGFX3D_TEXTURE`), and
  `fillLineT<output format>`; these are the explicit instantiations.

- the attribute evaluation of a span, `spanAttrs<smooth, textured>` (the compiler
  keeps the textured ones out of line; they are explicit instantiations too).

Everything else `render()` runs -- the span builders, `fragNearer`/`depthAt`,
`PerspDiv` and the pixel loop (`rasterLoop`) -- is inlined into them. The only calls
from this code to code without the attribute are `memset` (GCC turns the reset of
the scanline buckets, once per `render()` call, into it), the integer division
helpers of the target (textured spans, and the clamp of a smooth span whose color
leaves 0..255) and, on a core without `clz`, `__clzsi2`. To list them for a build,
disassemble the section with relocations
(`arm-none-eabi-objdump -dr -j .time_critical.gfx3d gfx3d.o`) and look for the call
relocations (`R_ARM_THM_CALL`). With `SHAPOGFX3D_TEXTURE=0` the attributed code is
about 6.2 KB on a Cortex-M0+ in the fixed-point build (12.3 KB before the span stage
was reworked); with every texture format it is about 24 KB on a Cortex-M33 (a
rasterizer evaluates a span's attributes in one path whatever its layer, reaching
the parts of the record through the fixed layout offsets rather than through one
record type per layer, which had doubled that code).

### Stack

`render()` needs about 170 bytes of stack plus a rasterizer's frame (48 to 96 bytes
on ARM); `beginRender()` about 560 bytes (the 256 counters of its radix sort). The
deepest path of scene building goes through `putPrimitive()` into the
triangle setup: on ARM with `SHAPOGFX3D_TEXTURE=0` about 570 bytes in the float build
(810 when a triangle has to be clipped to the guard band; that variant of the setup
is a separate function, so its buffers are on the stack only then) and 890 bytes in
the fixed-point build; lines take about 660 / 860. Measure the target's own compiler
with `-fstack-usage`; Xtensa frames are larger than ARM's.

### Platform notes

The library is portable; these are the settings that suit the targets it was tuned
for (checked by cross-compiling and on the host, not on hardware).

- **RP2350** (Cortex-M33 with FPU): the default float build. `SHAPOGFX3D_RP2_INTERP`
  and `SHAPOGFX2D_RP2_INTERP` are on by default with the Pico SDK's `hardware_interp`;
  give sprites that are rotated or scaled with an `affine2f` a power-of-two stride
  (e.g. 16, 32 or 64 pixels wide) to let the 2D one take them. Put the rasterization side
  in RAM with `SHAPOGFX3D_HOT_ATTR='__attribute__((section(".time_critical.gfx3d")))'`
  and `SHAPOGFX3D_HOT_INSTANTIATE=1`, and render on both cores with
  `Config::renderContexts = 2`. In RISC-V (Hazard3) mode there is no FPU: use
  `SHAPOGFX3D_FIXED_POINT=1`.
- **RP2040** (Cortex-M0+, no FPU): `SHAPOGFX3D_FIXED_POINT=1`, the rasterization side
  in RAM as above, both cores. Textures read from XIP flash compete with the code for
  the 16 KB cache, so copy small, often used textures to RAM. Disable the 16-bit
  output format that is not used (about 20 KB of RAM code each), and consider
  `SHAPOGFX3D_DEPTH_BITS=16` and `SHAPOGFX3D_GOURAUD_STEP=4` where memory or cycles are
  short.
- **ESP32-S3** (Xtensa LX7 with FPU): the float build; its divide and square root are
  slow, but the renderer needs one division per vertex and two per textured span.
  Place the arena in internal SRAM, not PSRAM (the span lists are walked per pixel
  row); a frame buffer in PSRAM works for plain writes but makes blending slow. Draw
  into `RGB565` and let `esp_lcd` swap the bytes, and render on both cores with two
  render contexts (one task pinned to each core).
- **ESP32-P4** (RISC-V with FPU, two cores): as the S3; the arena in internal memory
  (L2MEM). The PPA and the 2D-DMA can take large 2D blits and fills off the CPU, which
  an application does next to the library; the 3D renderer cannot use them.

## Rendering pipeline

### `beginRender()`

Sorts the entries of each layer (never the records) farthest first by the average
view-space z of the primitive's vertices, as a 16-bit key with the ordering of z (the
float build keeps the sign, the exponent and 7 bits of mantissa, the fixed-point
build a 5-bit exponent and 10 bits of mantissa of the 16.16 value, so depths within
1/128 or 1/1024 of each other tie); equal keys keep the order the primitives were
added in. The sort is a radix sort of the key in two passes of 8 bits (the second
skipped when every key of the layer shares its high byte), with the links of render
context 0, unused until `render()`, as its scratch space: two to four sequential
passes over the entries instead of the n log n record lookups of a comparison sort.
Layers with `LayerFlags::NO_DEPTH` are left in the order they were added. Depth
order between opaque spans of one layer is resolved by depth comparison in
`render()`, so this sort primarily determines the compositing order of translucent
primitives.

`beginRender()` also places the scanline links of every render context, so
`render()` requires it: called without it for the scene at hand (or after more
primitives were added), `render()` draws nothing.

### `render()`

The target format selects a rasterizer table and a fill function once per call;
RGB565_SWAPPED, RGB565 and RGB444 are supported (others return without drawing). The region is
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
  reciprocal of `1/w` plus two 32x16-bit multiplications (`arch::mulShiftU16`;
  two 16x16 -> 32-bit products each on a Cortex-M0+, which has no 64-bit
  multiply) for `u` and `v`. Per drawn textured span: two such divisions and two 32-bit divisions
  by the span length; costs 12 bytes per textured record. Along a scanline, a
  horizontal surface seen by a camera without roll has constant depth, so this level
  renders such surfaces without distortion; surfaces whose depth varies along the
  scanline keep affine distortion inside each span, while span end pixels (and
  therefore edges shared between triangles) are exact.
- **2**: full correction. `(u/w, v/w, 1/w)` are interpolated across the span and
  divided every `SHAPOGFX3D_PERSPECTIVE_STEP` pixels (default 16) the same way;
  `(u, v)` are interpolated linearly in fixed point in between. Costs one division
  per 16 textured pixels and 12 bytes per textured record.

With `SHAPOGFX3D_RP2_INTERP` (RP2040/RP2350), RGB565_SWAPPED, RGB565 and ARGB4444 texels of
textures with a power-of-two stride are addressed by the SIO interpolator `interp0`:
lane 0 maps `u` to the byte offset in the row, lane 1 maps `v` to the row offset,
and one `POP_FULL` per pixel yields the texel address and steps both coordinates.
Other textures use the software walker. Opaque, untextured, smoothly shaded spans
into RGB565_SWAPPED or RGB565 take their red and green from `interp1` (`arch::GouraudRG`): lanes 0
and 1 step r and g (8.16) with `ADD_RAW`, and their shifted and masked values
(`(r >> 8) & 0xF800`, `(g >> 13) & 0x07E0`) sum to the red and green of the pixel in
the `FULL` result; blue is stepped in software. Both paths give the same pixels as
the portable ones, which was checked on the host against an emulation of the
interpolator (`tmp.work`), not on silicon.

### `endRender()`

Currently does nothing; reserved for future use.

## Tools (`bin/`)

Python 3 scripts (dependencies in `bin/requirements.txt`; also runnable with `uv run`
thanks to inline metadata). `shapogfx_imgconv.py` is the shared image conversion
module.

- **img2cpp** `[-f FORMAT] [-d DITHER] [-k COLOR] [--name N] [--namespace NS] [--resize WxH] [--pot] input output.hpp`
  emits an aligned `static const` pixel array and a `static const gfx2d::Texture`.
  Formats rgb565_swapped (default), rgb565, argb4444, rgb444, gray1; dithering none / diffusion /
  pattern; `-k` makes a key color transparent; `--pot` resizes to a power of two.
  Pixels are quantized with rounding; the memory layout matches `pixel.hpp`
  (RGB444 is emitted as bytes; RGB565, ARGB4444 and RGB565_SWAPPED as `uint16_t` values,
  the last with their bytes already swapped, which the compiler lays out in the
  target's byte order so that each format holds its definition on any CPU).
- **gltf2cpp** `[--namespace NS] [--vertex-format float|packed] [--texformat auto|...] [--dither D] [--key-color C] [--max-texture-size N] [--no-resize-pot] input.gltf|glb output.hpp`
  (all glTF primitive modes are supported)
  emits, inside a namespace named after the file, `tex<i>` textures, `mat<i>` (and
  `mat<i>Vc` for primitives with vertex colors) materials, `mesh<i>Prim<j>Vertices` /
  `...Indices` / `mesh<i>`, `node_<name>` (or `node<i>`) nodes in child-first order,
  `scene<i>` and a `scene` alias for the default scene, plus `node_<name>Trs`
  (`NodeTRS`) for nodes given as TRS, the table `nodes` of every node in glTF
  order that the scenes point to, a `static_assert` on `MODEL_FORMAT_VERSION >= 1` and
  the reserved members written as 0. Vertex attributes are
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

- **dbones2cpp** `[--namespace NS] [--armature A] [--skin S] [--scale S] [--anim-scale auto|S] [--in-key COLOR] [--out-format argb4444|rgb565_swapped|rgb565|auto] [--auto-alpha PERCENT] [--out-key COLOR] [--alpha-threshold N] [--dither D] [--atlas-width auto|N|0] [--texture-dir DIR] [--fit-rotate] [--fit-min-gain PERCENT] [--hull N] [--preview FRAMES] [--dump-pose FRAMES] input_ske.json [extra.dbani ...] output.hpp`
  converts a DragonBones 5.x armature for `rig` (`shapogfx_dbones.py` is its core,
  shared with the tests). The header `static_assert`s `rig::FORMAT_VERSION >= 1`
  (the members it initializes) and writes the members reserved for later features
  as unused (`flags` 0, `AttachmentKind::IMAGE` with `ext` nullptr, tint 255, 255,
  255, `turns` 0, `features` 0). It reads the 5.0 (one `frame` timeline per bone with every
  channel) and 5.5 (`translateFrame` / `rotateFrame` / `scaleFrame`, `displayFrame`,
  `colorFrame`) animation formats, `zOrder` timelines, and the images from the
  `<name>_texture/` folder or a `<name>_tex.json` atlas (trimmed and rotated
  sub-textures). Bones are sorted parents first, slots by `z`. Channels whose keys
  all equal the bind pose are dropped; curves (`curve` bezier arrays, single or
  piecewise, and `tweenEasing`) become deduplicated 17-sample Q14 tables. `--scale`
  resizes the images (premultiplied Lanczos) and every position; `.dbani` files add
  animations of the same armature (same bone and slot names), their positions scaled
  by `--anim-scale` (auto: the median ratio of the bone lengths). Transparency is
  split into input (the images' alpha, or `--in-key`) and output: ARGB4444, or
  RGB565 with pixels below `--alpha-threshold` in `--out-key` (opaque pixels that
  quantize to the key get their blue LSB flipped) and `Armature::colorKeyEnabled`,
  or `auto`, which decides per image: the translucent pixels (alpha 1..14 after
  4-bit quantization) next to a transparent one are an antialiased edge, which a
  key threshold merely hardens, while those away from any are meant to show
  through, so an image with more than `--auto-alpha` (5%) of the latter among its
  visible pixels stays ARGB4444 and the rest become RGB565_SWAPPED with the key
  (rgb_chan: the ties and bracelets against 33 keyed parts; demorig does not use
  it, for the look of the edges). Keyed images are
  drawn by copies instead of blends -- a rotated pixel costs about 20 instructions
  whether transparent or opaque, against 25 / 50 / 70 for ARGB4444 -- which took
  the rgb_chan frame from 3.90 to 2.73 million instructions at 320 x 240 (-30%)
  and from 7.60 to 4.96 million at 640 x 360, at the price of hard edges on those
  parts. Mixed formats go into two atlases (`atlas` ARGB4444, `atlasKeyed`) or
  into per-image textures of their own format; `Instance::draw()` sets the key
  for the keyed textures only, and `--preview` draws keyed images with their
  edges hardened as the device will.
  Each image is fitted before packing: its transparent margin (pixels below the
  opaque threshold: alpha 9, the least that ARGB4444 keeps, or `--alpha-threshold`
  for the keyed formats) is trimmed, the attachment's `local` taking up the offset;
  with `--fit-rotate` an image is also turned so that the smallest-area bounding
  rectangle of its opaque pixels' convex hull (rotating calipers) is upright, when
  that saves at least `--fit-min-gain` (3%) of the area -- one bicubic resampling
  from the original image with the scale folded in, premultiplied, stray pixels
  farther than one pixel from a quarter-opaque one stripped, and the turn folded
  into `local` -- so that a limb drawn diagonally no longer carries its empty
  corners; and the convex hull of what remains is simplified to at most `--hull`
  vertices (8; 0 for none) by replacing edges with the meeting point of their
  neighbors, least added area first, rounded outward to integers and verified to
  hold every opaque pixel, then emitted as `hull_<image>` and referenced by the
  attachments (dropped where it would enclose over 98% of the rectangle). The
  images are shelf-packed into one atlas whose width (a power of two, 64..2048)
  gives the smallest area, so the stride is a power of two (RP2 interpolator path);
  `--atlas-width 0` emits one texture per image, which has no row padding: when the
  images live in flash behind a cache, a frame touches a third fewer cache lines
  (rgb_chan: 3464 lines of 64 bytes against 5133), which is what took the Tab5
  from 30 to 42 fps (its 256 KB L2 holds the dense textures, not the atlas) and
  changed nothing on the CoreS3 (64 KB: neither fits); the atlas keeps the RP2
  interpolator path. The measured costs of the options on the rgb_chan frame
  (320 x 240, x86-64): the hulls -5.5%, `--fit-rotate` mostly flash (-13%),
  `--out-format auto` -30% (the parts that are only translucent along their edges
  are copied with a key instead of blended, and lose that antialiasing; all keyed
  would be -33% and lose the translucent parts), `--scale` flash and cache
  footprint only (the drawn pixels are the screen's). Output: `atlasData` / `atlas`,
  `hull_<image>`, `attachments_<slot>`, `bones`, `slots`, `armature`, per animation
  `anim_<name>_curves`, key arrays, timelines, draw orders and `anim_<name>`, then
  `animations[]` and `ANIMATION_COUNT`. `--preview` renders poses of the first
  animation from the converted data to PNG, `--dump-pose` writes the poses of every
  animation as JSON. Unsupported features are listed in the header's comment.


- **svg2cpp** `[--namespace NS] [--scale S] [--picture | --rig] [--keep IDS] [--keep-all] [--fps N] [--duration SECONDS] [--anim-name NAME] [--font FAMILY=PATH ...] [--font-dir DIR ...] [--text-font EXPR] [--image-format F] [--dither D] [--dump JSON] [--verbose] input.svg output.hpp`
  converts an SVG file for `vg` and `rig` (`shapogfx_svg.py` is its core; it
  imports the curve tables, angle / Q12 conversion, signature and emitting
  helpers of `shapogfx_dbones.py`, and `shapogfx_imgconv.py` for images). A
  static SVG (or `--picture`) becomes one `vg::Picture` named `picture`: the tree
  is flattened, every drawable leaf a `vg::Shape` whose `transform` is its CTM
  relative to the root, the path in the leaf's own user space (so stroke widths
  are right under non-uniform scales), with `path<i>Ops` / `path<i>Coords` /
  `path<i>`, `stops<i>` / `gradient<i>` (`gradient<i>s` for strokes; stop arrays
  deduplicated), `clip<i>` (`RectF`), `text<i>` (`vg::Text`), `img<i>Data` /
  `img<i>` (textures, deduplicated by content), `shapes[]` and `picture` (bounds
  = the SVG's pixel size x `--scale`). The header `static_assert`s
  `vg::FORMAT_VERSION >= 1`, defines `OP_MOVE` .. `OP_CLOSE` inside the
  namespace for the op arrays, and lists the options and every warning in its
  comment. Supported: `svg` (width / height / viewBox / preserveAspectRatio),
  `g`, `a`, `path` (every command, absolute and relative; arcs become cubics),
  `rect` (`rx` / `ry`), `circle`, `ellipse`, `line`, `polyline`, `polygon`,
  `image` (`data:` URIs or files next to the SVG, `preserveAspectRatio`;
  `--image-format` / `--dither` as img2cpp), `text` / `tspan` (outlined with
  fontTools when `--font` / `--font-dir` finds the family: `x` / `y` lists, `dx` /
  `dy`, `text-anchor`, `letter-spacing`, `kern` table; else a `vg::Text` drawn
  with the bitmap font of `--text-font`, with a warning), `defs`, `use` (expanded;
  `symbol` with a viewBox when the use has a size), `switch` (the first child whose
  `systemLanguage` fits), `linearGradient` / `radialGradient` (`href` inheritance,
  `objectBoundingBox` and `userSpaceOnUse`, `gradientTransform`, `spreadMethod`,
  stop opacity; `fx` / `fy` warned and ignored), `clipPath` holding one `rect`
  (`clipPathUnits` too; a turned clip becomes its bounding box with a warning),
  presentation attributes, `style` attributes and `<style>` sheets (type, class,
  id, `*`, comma and descendant selectors; specificity, later rules win; other
  selectors are dropped with a warning), inheritance, `currentColor` (the
  `SHAPE_*_CURRENT_COLOR` flags), CSS colors in every notation, units (px, pt, mm,
  cm, in, pc, %), `fill-rule`, `stroke-linecap` / `linejoin` / `miterlimit`,
  `stroke-dasharray` / `dashoffset` (static: the path is cut along its length into
  dash subpaths, closed subpaths as open ones), `visibility` / `display`. The
  `opacity` of a group is multiplied into the brushes of its descendants (an
  approximation where they overlap, noted in the header). Warned and skipped:
  `mask`, `pattern` (the fill becomes none), `filter`, `marker`, `foreignObject`,
  `textPath`, SVG images, unknown elements.

  An SVG with SMIL animation (`animate`, `set`, `animateTransform`,
  `animateMotion`; or `--rig`) becomes a `rig::Armature` (`armature`, `bones[]`,
  `slots[]`, `attachments_<slot>[]`, a `picture_<slot>` per VECTOR attachment with
  its `path_<slot>_<i>` .. objects, `clip_<slot>`) and one `rig::Animation`
  (`anim_<name>`, `animations[]`, `ANIMATION_COUNT`) in the layout of dbones2cpp,
  asserting `rig::FORMAT_VERSION >= 3` with `features = FEATURE_SLOT_COLOR`, plus
  `FEATURE_STROKE_WIDTH` when a stroke width is animated.
  Elements with animation (and those on the way to them, `--keep` ids and, with
  `--keep-all`, every element with an id) become bones named after their ids: one
  bone for the element's static transform (`<id>_base` when animated bones
  follow; any affine matrix is decomposed exactly into the rig's rotation / skew /
  scale parameters, scales clamped to the Q12 range with a warning), then one
  bone per `animateTransform` with an identity bind pose and the full values as
  keys (`additive="sum"` chains are therefore automatic; `replace` keeps a static
  item of the same type as the base value and drops anything else with a
  warning): `_translate`, `_scale`, `_pivot` + `_rotate` (+ `_unpivot`) for a
  rotation about a center (a constant center folds the `-cx, -cy` into the
  children), `_skewX` / `_skewY` (ROTATE + SCALE keys, an approximation), and a
  `_motion` bone per `animateMotion` (sampled per frame along the path with
  `keyPoints` / `keyTimes` / `calcMode`, straight runs merged, `rotate`
  auto / auto-reverse / angle → ROTATE keys); the last bone of the chain takes
  the element's name. Animated `x` / `y` / `cx` / `cy` go to a position bone
  (`<id>_pos`, or `<id>` when there is no other) as TRANSLATE keys; `r` / `rx` /
  `ry` / `width` / `height` as SCALE keys relative to a reference size (the base,
  or the largest value when the base is 0 or the growth exceeds 8 x; the stroke
  scales too, warned). `opacity` (and `fill-opacity` / `stroke-opacity` reaching a
  shape) → ALPHA keys, products of nested animated opacities sampled at the
  union of their keys; `fill` / `stroke` / `color` → the shape flagged
  `SHAPE_*_CURRENT_COLOR`, the slot's color set to the base and a COLOR timeline
  (one animated color per slot; others warned); `stroke-width` → the shape flagged
  `SHAPE_STROKE_CURRENT_WIDTH`, `Slot::strokeWidth` the base and a STROKE_WIDTH
  timeline (one per slot), its keys divided by the scale of a size animation of the
  same element (sampled per frame while either moves, straight runs merged) so that
  the drawn width stays what SVG shows; `visibility` / `display` → ATTACHMENT keys
  (0 / -1). Static subtrees collapse into one slot with one
  VECTOR attachment holding all their shapes (one slot per run of consecutive
  static siblings; the first run under a kept container is named after it, e.g.
  `root`), so bones and slots stay few (255 each; errors beyond). Images directly
  under a kept container or animated become IMAGE attachments with a texture of
  their own. Slot clips come from the innermost `clip-path` rectangle, in the
  space of the clipped element's bone. Timing: `begin` (offsets only; event
  begins drop the animation with a warning), `dur`, `repeatCount` / `repeatDur`,
  `fill` (`remove` returns to the base value with a STEP key on the end frame,
  which the key search of `pose()` honors), `calcMode` (discrete → STEP keys,
  linear, paced, spline with `keySplines` as 17-sample curve tables), `keyTimes`,
  `values` / `from` / `to` / `by`, `additive` (sum: the values added at the union
  of key times; replace: the later active one wins); `end`, `min` / `max`,
  `accumulate` and animations of `d`, `points`, `transform` via `animate` and line
  end points are warned and dropped. Keys are placed at
  `--fps` frames (rounding within half a frame), every timeline starts at frame 0
  with the base value, repeats are unrolled over the document duration
  (`--duration`, or the common period of the repeating animations extended to the
  end of the others, 1 frame when nothing animates, at most 60 s with a warning),
  and rotations are split into keys at most a quarter turn apart so that the
  shortest-way interpolation of the rig stays right. `--dump` writes a JSON
  summary (shapes with their kinds, flags, brushes and first ops, or the bones,
  slots and key counts per timeline) for checking a conversion without compiling.
  Limits: 255 bones, slots and timelines, 253 curves, 65535 frames and ops per
  path, 255 stops, `use` nesting 8 deep.

## Sample programs

The samples are 480x320 and render into an RGB565_SWAPPED buffer. Each has a WASM entry
point (`<name>_init`, `<name>_frame`, `<name>_get_fb`, `<name>_get_width`,
`<name>_get_height`) driven by `docs/example/viewer.js`, and a native `main()` that
writes one frame as a PPM file. The WASM binaries are committed so that `docs/` can be
served as a static site.

- `example/wasm/demo2d/`: exercises the `Graphics2D` API only (no reference to
  `gfx3d`): a scrolling ellipse pattern, filled and outlined polygon stars, ARGB4444
  sprites with alpha and additive blending, GRAY1 bitmaps with and without a
  background color, an RGB444 off-screen surface drawn with a second `Graphics2D` and
  blitted (whole and partial), rounded rectangles, circles, ellipses, triangles, lines,
  pixels, clipping and text in several fonts including measurement and text enlarged
  by a transform, a pie chart (`fillSector`) and a progress ring (`drawCircleArc`),
  a partial copy of the panel with its background keyed out (color key), and the
  panel scaled, mirrored and rotated (with a frame and a caption turning with it,
  under `pushState()`) next to a squashed, spinning sprite. It runs with an arena of
  4 KB.
- `example/wasm/demo3d/`: a textured floor, an environment-mapped torus (`putTorus`),
  opaque, alpha-blended and additive cubes, and a vertex-colored windmill generated
  from `model/windmill.glb` (`model/make_windmill.py`) with `gltf2cpp` whose "Blades"
  node is rotated by a `NodeVisitor`. The frame is composed in two passes: a 2D
  backdrop (gradient, stars, caption) drawn with `Graphics2D`, then the 3D scene
  rendered in four bands with the clear disabled. Mouse and keyboard control the
  camera in the browser.
- `example/wasm/demorig/`: a DragonBones character, pop stars from an animated SVG
  (`assets/2d/pop_star.svg` converted by svg2cpp: two `rig::Instance`s restarted in
  turn every second at a random place, size and angle behind the character), and
  an (AA) button at the bottom left that turns antialiasing on for everything
  `Graphics2D` antialiases (the pictures and the area fills)
  (`example/common/demorig/model/rgb_chan.hpp`, generated with
  `dbones2cpp --scale 0.4 --fit-rotate` from `assets/2d/rgb_chan/`, whose images
  are drawn at twice the size they show at so that the turned parts are
  resampled from something finer, by `make model`: 29 bones, 41 slots, the
  trimmed and turned parts with their hulls in a 256 x 568 ARGB4444 atlas, about
  297 KB; `model/rgb_chan_sep.hpp` is the same with `--atlas-width 0`, one texture
  per image, 228 KB, which `scene.cpp` includes when a build defines
  `DEMORIG_MODEL_HEADER` to it. `--out-format auto` was tried and not kept: it
  made the CoreS3 draw at 27 fps instead of 24 and the Tab5 at 56 instead of 42,
  but the hard edges showed) posed by a `rig::Instance`
  from its 24 fps animation (`frameAt()` every frame) and bobbing up and down in a
  ring of additive rectangles that turns around it. The ring's back half is drawn
  first, then the character up to its left arm (`draw(g, 0, k)` with
  `k = drawIndexOf(slotIndex("l_arm"))`), the ring's front half, and the rest of
  the character, so the arm reaches out in front of the ring. Behind it, colorful
  stars (outlined and filled polygons, like those of demo2d) turn and fall
  diagonally over a scrolling checkerboard.

  The scene, the view and the overlay are `example/common/demorig/` (ShapoGFX
  only), shared with the M5Stack builds. The scene is laid out in world pixels of
  the screen size and scaled with the height (320 being the reference: the ring,
  the character, the stars' size and speed, the checkerboard's squares). A view
  zooms it about the screen center (1/4 to 16 times, in powers of two, animated in
  log2) and scrolls it by dragging, the view center kept within the scene; the
  background color fills the whole screen and the rest is clipped to the scene's
  rectangle. (+) / (-) buttons at the bottom of the right edge and the frame rate
  and zoom in the top left corner are drawn over it. `Demo::draw(g, bandY)` draws
  any band of rows and is const, so two cores can draw two bands of one frame at
  once; what a band does not show (stars, the ring's rectangles, rows of the
  checkerboard) is skipped by boxes computed once per frame, which keeps the cost
  of drawing a 320 x 240 frame in 8 bands 6% above drawing it at once. The browser
  page takes the screen size from `?screen=WxH` (default 480 x 320) and the mouse or
  touch through `viewer.js`; the native build takes the time, the size, the zoom,
  the view center and a band count on the command line.
- `example/m5cores3/demorig/`, `example/m5tab5/demorig/`: demorig on M5Stack
  CoreS3 (320 x 240, touch) and Tab5 (a 640 x 360 frame scaled twice by the PPA,
  touch), ESP-IDF 5.5 projects sharing the components of
  `example/m5common/` (the scene and the M5Unified front end) and taking the
  repository itself as the ShapoGFX component. A frame
  is drawn in strips of 60 rows, each split between the two cores,
  into two buffers in internal RAM: one strip is drawn while the previous one goes
  out by SPI DMA (the CoreS3) or the PPA (the Tab5), and the last one is
  left in flight across the frame boundary. The serial console gets the frame rate
  and the time per frame every 2 seconds. They draw from the per-image textures
  (`rgb_chan_sep.hpp`): the parts come from flash through the cache, and the
  padding of the atlas rows would cost a third more cache lines per frame
  (rgb_chan: 5133 lines of 64 bytes against 3464).

## Tests

`test/` builds `shapogfx_tests` (registered with CTest) without any external
framework. It checks color conversions and cursors for every enabled format, blending
identities, `Graphics2D` clipping, fills, polygons, lines, ellipses, image blits,
bitmaps, text and its metrics, the blend state
(opacity against the color's alpha, additive shapes and text, `NONE` writing the
alpha of an ARGB4444 target), the state stack (what is saved and restored, the
cursor kept, the depth limit, a clip rectangle restored onto a smaller target),
scaled and transformed images (every format pair and blend against a per-pixel
reference built from 1 x 1 blits, the mapping of the scaled path exactly, the
transformed one wherever a pixel center is not within 1/500 texel of a texel edge,
and the polygon overload likewise -- source points within 1/500 texel of an edge of
the polygon left out -- under rotations, shears, plain translations and scales,
with a triangle, a diamond of the other winding, a hexagon and an octagon, plus a
polygon around the whole rectangle drawing what the plain call draws, one beside
it, a degenerate one and too many vertices drawing nothing, two vertices meaning
no polygon, and the polygon applying without a transform;
nothing outside the source rectangle is read even where centers fall on its edges;
transforms without rotation equal the scaled path with snapped corners), the color
key (plain, scaled and transformed, every format pair and blend, against 1 x 1 blits
of the pixels not keyed out), transformed shapes (integer and fractional translations
draw what drawing at the offset draws; scales equal the scaled rectangles; a quarter
turn maps rectangles, frames, images, pixels, lines, text and ellipses exactly; a
turned rectangle and a turned image cover the same pixels; sectors under a mirroring
shear still partition the ellipse; turned rounded rectangles and frames), polygons
(integer and float vertices agree, triangles sharing an edge partition their
quadrilateral, the scratch-memory and the per-row paths agree, clipping above the
top), the float API against the integer one, `affine2f`, arcs and sectors (a full
turn equals the ellipse, sectors between consecutive cuts cover it exactly once,
parametric angles, wrap-around, mirrored quarters), cursor
`skip()`, consistency between RGB565_SWAPPED and RGB444 targets (and RGB565
holding exactly the byte-swapped RGB565_SWAPPED pixels, drawn or blitted), and for
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
visitor skipping and animation, deep-tree cut-off). For `rig`, `test/tools/make_test_rig.py`
generates a small armature (four bones with rotation, skew and non-uniform scale;
five slots defined out of draw order, two attachments, alpha, an additive slot, a
diagonal bar with a transparent margin; an
animation with a bezier curve, linear and held keys, offsets across the int16 wrap,
attachment and alpha timelines and a draw order key; a `.dbani` at twice the size),
converts it five ways (atlas, one texture per image, RGB565 with a key color,
with `--fit-rotate`, and with `--out-format auto`, where a sixth slot's image,
translucent inside an opaque border, keeps its alpha while the others get the key),
checks that its 5.5-format / atlas variant converts to the same header, and writes
the tool's poses as the expected values. The tests check the poses against them
(world transforms within 1e-3, attachments, alphas, draw order, bounds), clamping,
`frameAt()`, the signature check, the visitor, the accessors, `draw()` against the
same `drawImage()` calls made by hand on two target formats, the hulls (every
attachment has one; drawn with them or with the whole rectangles the picture is
the same, on the ARGB4444 atlas and the keyed one), the turned bar of the
`--fit-rotate` conversion (a quarter of the texels, landing where the original
does: centroid within a quarter texel, same angle and about the same area when
drawn four times enlarged, the other parts pixel-identical), the mixed formats
(the ARGB4444 part draws what the ARGB4444 conversion draws and the keyed parts
what the keyed one draws, `draw()` toggling the key and restoring the caller's),
the separate textures and the key color, drawing in ranges and in bands against drawing at once, the halves of a frame
drawn at the same time on two threads by two contexts from one `Instance` (as clip
rectangles and as targets of their own; checked with ThreadSanitizer), the
restored `Graphics2D` state, drawn pixels within `bounds(placement)`, and the atlas
pixels against the source images (the trimmed bar included) with every hull
holding the opaque pixels and leaving out transparent ones. For `vg` (`test/vg_test.cpp`): the
path builder (ops, coordinates, bounds, overflow), a rectangle path filling what
`fillRect()` fills under translations and scales and, turned, exactly the pixels
whose centers are inside, a circle path against `fillEllipse()`, the fill rules (a
star's center, holes with either winding), antialiasing (a quarter and half covered
corners and edges, a thin slanted stroke never covering a pixel fully, none with the
flag off or the NONE blend mode, every format), gradients (linear along x, PAD /
REPEAT / REFLECT, radial symmetry, three stops under a turned transform, the brush's
alpha, no stops, ADD and NONE, antialiased edges), strokes (butt / square / round
caps pixel-exact, miter / bevel / round joins, the miter limit, a closed polyline,
a translucent stroke painted once, width 0, zero-length subpaths, the width under
a non-uniform scale, a gradient stroke, a mirrored transform), flattening (a unit
circle scaled 60 x within a pixel of the true circle, a quadratic curve), pictures
(brushes, the current color, clips moving with the transform, a feature bit,
IMAGE shapes against `drawImage()`, TEXT shapes against `drawString()`, the state
restored), drawing without an arena and with a too-small one (the same pixels as
with one for opaque colors; a star antialiased identically from the stack buffer),
the state stack and opacity / clipping, and the vector attachments of rig (a
hand-made armature with a VECTOR attachment in the slot's color, a clipped slot,
`drawBind()` against `Instance::draw()`, a COLOR timeline, the feature bit off).
For svg2cpp (`test/tools/make_test_vg.py` writes `test_vg.svg`, a static SVG
exercising every element and most properties, and `test_vg_anim.svg` with every
animation element, and converts them to `test_vg.hpp` (picture), `test_vg_text.hpp`
(the same with a DejaVu font, when installed) and `test_vg_anim.hpp` (rig, `--keep
kept`, 30 fps, 2 s), plus their `--dump` JSONs): the shape kinds, flags, clips and
gradients the conversion decided on, the drawn pixels (the class rule, gradients,
a dash gap, even-odd, the clip, `use`, `switch`, hidden elements, the images,
the texts outlined or as bitmap text, nothing right of an end-anchored text), and
the armature (13 bones, 8 slots, the names of the kept element and the
collapsed backdrop, the quarter turn, the summed translation, the position and
radius keys, the hidden interval, the color timeline, the stroke width timeline
divided by the radius scale, the pixels at three frames, `drawBind()` against the
instance). The antialiased area fills are checked against the paths of the same
shapes (rectangles float and integer under a rotation, the plain fill under a
translation, ellipses, circles, rounded rectangles, polygons with pixel-center
vertices, nothing with the flag off or the NONE blend mode); the antialiased
lines and outlines (axis-aligned lines and a rectangle's outline unchanged, a
diagonal line partial along the plain one, frames against the even-odd path, an
ellipse and a rounded rectangle outline within their fills, a quarter arc in its
quadrant, a sector and the full-turn sector equal to the ellipse, a pixel wide
under a scale) and the sampled text and bitmaps (unchanged untransformed, gray at a
fractional scale along the plain shape, a turned bitmap with both colors), and the
antialiased images (a solid image turned and clipped to a polygon equals the
antialiased quadrilateral and triangle, a 2 x 2 checker scaled 8 x is a monotonic
gradient between the texel centers, the scaled overload samples the same way and an
unscaled copy stays plain, a keyed texel lends no color, an ARGB4444 image's alpha
multiplies the coverage, the plain copy with the flag off). The tests are meant to be run with AddressSanitizer and
UndefinedBehaviorSanitizer on the native build, and with ThreadSanitizer for the
ones that draw on two threads (where it stops with "unexpected memory mapping",
run the tests with `setarch -R`). The CMake options of the renderer
are passed to the tests as well, so a configuration with a feature compiled out
skips the tests that need it and the rest must still pass.
