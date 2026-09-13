# ShapoGFX

2D/3D graphics libraries for embedded systems.

ShapoGFX is a small, dependency-free C++17 software renderer for
microcontrollers driving small displays. It has a 2D drawing API with bitmap
fonts and a scanline 3D renderer that works without a frame buffer or a Z
buffer. The library never allocates memory: it draws into buffers you provide
and takes its working memory from an arena you hand it.

- **Live demos:** [demo2d](https://shapoco.github.io/shapo-gfx/example/demo2d/),
  [demo3d](https://shapoco.github.io/shapo-gfx/example/demo3d/)
- **Specification:** [SPEC.md](SPEC.md)

## Features

### Pixel formats (shared)

| `PixelFormat` | Bits | Memory layout |
|---|---|---|
| `GRAY1` | 1 | MSB first, 1 = white |
| `RGB444` | 12 | 2 pixels in 3 bytes, display order (`R1G1 B1R2 G2B2`) |
| `ARGB4444` | 16 | native `uint16_t`, `0xARGB` (used for sprites with alpha) |
| `RGB565BE` | 16 | big-endian byte order in memory (display order) |

RGB565 is always stored byte-swapped so that a buffer can be sent to an
SPI/parallel display controller by DMA without a conversion pass. Any format
can be disabled at compile time to remove its code (see below).

### `shapoco::gfx2d`

- `Surface` / `Texture`: writable and read-only images (format, size, stride,
  pixel pointer). Model data and textures can live in flash.
- `Graphics2D`: clip rectangle, pixels, rectangles (plain, rounded, outlined),
  ellipses and circles, lines, polylines and polygons, image blits between any
  two formats with copy / alpha / additive blending and opacity, two-color
  bitmaps, GFXfont text with integer scaling and measurement.
- Colors are ARGB8888 everywhere in the 2D API; an alpha below 255 blends.
- Four bundled fonts (ShapoSans 8 px mono and proportional, 12 px, 21 px) in
  the Adafruit GFXfont format; any GFXfont works.
- Optional `OwnedSurface` helper (`surface_alloc.hpp`) that owns a heap buffer
  through `std::unique_ptr`, for application code that wants automatic cleanup.

### `shapoco::gfx3d`

- Scanline rasterization with per-line span lists: no frame buffer, no Z buffer
- Renders any rectangular region of the screen into a `Surface` (RGB565BE or
  RGB444), so the output can be streamed to a display in bands
- Directional light, ambient light, Gouraud shading
- Texture mapping from any pixel format; ARGB4444 textures are alpha-blended
  per texel; environment mapping
- Alpha blending and additive blending with correct ordering against opaque
  geometry
- Triangle lists, strips and fans; a built-in subdivided box
- OpenGL-style right-handed coordinate system and matrix stack
- Perspective-correct texture mapping in three levels (off, vertical only,
  full); vertical-only is the default and costs two divides per span
- Optional transparent background: uncovered pixels keep the target's content,
  so a 3D scene can be drawn over a 2D backdrop
- Per-pixel work is integer only (16-bit fixed point); vertex work is `float`
- Multiple independent `Graphics3D` instances, each with its own arena

## Directory layout

```
include/shapoco/gfx2d/   2D API and shared types (pixel formats, Surface, Graphics2D, fonts)
include/shapoco/gfx3d/   3D renderer (gfx3d.hpp, math3d.hpp)
src/gfx2d/, src/gfx3d/   implementation
example/wasm/demo2d/     2D sample (WASM and native entry points)
example/wasm/demo3d/     3D sample over a 2D backdrop
docs/example/            browser pages for the samples (index.html, viewer.js, *.wasm)
test/                    self-checking tests (ctest)
```

## Quick start

### 2D

```c++
#include "shapoco/gfx2d/gfx2d.hpp"
#include "shapoco/gfx2d/fonts.hpp"

namespace g2 = shapoco::gfx2d;

static uint16_t fb[320 * 240];  // RGB565BE frame buffer
static const g2::Surface screen = {g2::PixelFormat::RGB565BE, 320, 240, 320 * 2, fb};

void draw() {
    g2::Graphics2D g(screen);
    g.clear(g2::makeColor(20, 24, 40));
    g.fillRoundRect(20, 20, 200, 100, 12, g2::makeColor(255, 255, 255, 40));  // translucent
    g.drawCircle(260, 120, 40, g2::Colors::CYAN);
    g.setFont(&ShapoSansP_s12c09a01w02);
    g.setTextColor(g2::Colors::WHITE);
    g.drawString(32, 32, "Hello, ShapoGFX");
    // ... send fb to the display ...
}
```

### 3D

```c++
#include "shapoco/gfx3d/gfx3d.hpp"

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;

static uint8_t arena[64 * 1024];
static uint16_t band[320 * 40];  // 40-line transfer buffer, RGB565BE
static const g2::Surface bandSurface = {g2::PixelFormat::RGB565BE, 320, 40, 320 * 2, band};
static g3::Graphics3D renderer;

static const g3::Material matRed = {
    {0.9f, 0.15f, 0.1f, 1.0f}, {0.9f, 0.15f, 0.1f, 1.0f}, nullptr, g3::BlendMode::NONE, 0,
};

void setup() {
    renderer.init(320, 240, arena, sizeof(arena));
    renderer.setPerspectiveProjection(60.0f * 3.14159f / 180.0f, 320.0f / 240.0f, 0.3f, 100.0f);
    renderer.setClearColor({0.05f, 0.05f, 0.1f, 1.0f});
}

void drawFrame(float t) {
    renderer.beginScene();
    renderer.translate(0, 0, -5);                       // camera
    renderer.enableParallelLight({-0.5f, -1, -0.6f}, {1, 1, 1, 1});
    renderer.enableEnvironmentLight({0.2f, 0.2f, 0.3f, 1});
    renderer.pushState();
    renderer.rotate(t, 0.3f, 1, 0);
    renderer.setMaterial(matRed);
    renderer.putCube({0, 0, 0}, {1.5f, 1.5f, 1.5f});
    renderer.popState();
    renderer.endScene();

    renderer.beginRender();
    for (int y = 0; y < 240; y += 40) {
        renderer.render(0, y, 320, 40, bandSurface);  // screen region -> band (0, 0)
        // ... send `band` to the display at (0, y) ...
    }
    renderer.endRender();
}
```

Model data (vertex arrays, index arrays, textures) is only referenced, never
copied, so it can live in flash as `const` data. Textures are `g2::Texture`
values: `{format, width, height, stride, pixels}` with power-of-two sizes.

## Building

### With CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/example/wasm/demo2d/demo2d out2d.ppm   # one frame of each sample as PPM
./build/example/wasm/demo3d/demo3d out3d.ppm
ctest --test-dir build --output-on-failure      # self-checking tests
```

To use the library from another CMake project (including Pico SDK projects):

```cmake
add_subdirectory(path/to/shapo-gfx)
target_link_libraries(your_target PRIVATE shapoco::gfx)
```

Options:

| Option | Default | Description |
|---|---|---|
| `SHAPOGFX3D_CORRECT_PERSPECTIVE` | empty (library default, 1) | Perspective correction level: `0` off, `1` vertical only, `2` full. See below. |
| `SHAPOGFX_BUILD_EXAMPLES` | `ON` when top-level | Build the native sample programs |
| `SHAPOGFX_BUILD_TESTS` | `ON` when top-level | Build the tests |

### Without CMake

Add `include/` to the include path and compile `src/gfx2d/*.cpp` and
`src/gfx3d/*.cpp` with C++17. All compile-time options are plain macros.

### Compile-time options

| Macro | Default | Effect |
|---|---|---|
| `SHAPOGFX_FORMAT_GRAY1`, `SHAPOGFX_FORMAT_RGB444`, `SHAPOGFX_FORMAT_ARGB4444`, `SHAPOGFX_FORMAT_RGB565BE` | `1` | Define as `0` to remove a pixel format from both renderers (fewer rasterizer instantiations, smaller code). Surfaces and textures in a disabled format are ignored. |
| `SHAPOGFX3D_CORRECT_PERSPECTIVE` | `1` | Texture perspective correction level (only affects `gfx3d.cpp`) |

The format macros must be defined identically for every translation unit that
includes the headers; the easiest way is a global compiler definition.

### Perspective correction

| Level | Behavior | Cost |
|---|---|---|
| `0` | Affine interpolation everywhere (classic "PS1 look" on large polygons) | none |
| `1` (default) | The two end points of every span are perspective-correct; the span interior is affine | 2 float divides per span, +12 bytes per triangle |
| `2` | Fully perspective-correct: `(u/w, v/w, 1/w)` interpolated and divided per pixel | 1 float divide per textured pixel, +12 bytes per triangle, +8 bytes per span |

Level 1 removes all distortion from horizontal surfaces (floors, ceilings) seen
by a camera without roll, because along a scanline such surfaces have constant
depth and affine interpolation is already exact there. Surfaces whose depth
changes along the scanline (walls receding sideways) keep some distortion inside
each span; subdivide them or use level 2 if that matters.

## Samples

Both samples render 480x320 frames into an RGB565BE buffer and share one
browser viewer (`docs/example/viewer.js`). They also build natively and write a
single frame as a PPM file.

- **demo2d** (`example/wasm/demo2d/`): scrolling background, polygon stars,
  ARGB4444 sprites with alpha and additive blending, GRAY1 icons, an RGB444
  off-screen surface blitted to the screen, rounded rectangles, circles,
  lines, clipping and all four fonts. Uses only `shapoco::gfx2d`.
- **demo3d** (`example/wasm/demo3d/`): a textured floor, an environment-mapped
  torus and three cubes (opaque, alpha-blended, additive) rendered over a 2D
  backdrop drawn with `Graphics2D`, with the 3D clear disabled.

Browser build (requires [Emscripten](https://emscripten.org/)):

```sh
(cd example/wasm/demo2d && make)   # builds docs/example/demo2d/demo2d.wasm
(cd example/wasm/demo3d && make)   # builds docs/example/demo3d/demo3d.wasm
./launch_web_server.sh             # serves docs/ on http://localhost:52880/
```

Then open http://localhost:52880/example/demo2d/ or `.../demo3d/` (the pages
use `fetch()`, so they do not work from `file://`). In demo3d, drag or use the
arrow keys to rotate the camera and the wheel or PageUp/PageDown to zoom. The
WASM binaries are committed so that `docs/` can be published as a static site.

## Memory usage (3D)

All working memory of a `Graphics3D` is taken from the arena passed to `init()`.
On a 32-bit target the layout is roughly:

| Region | Size |
|---|---|
| Line buckets | screen height x 4 bytes |
| Matrix stack (16 entries) | about 1.1 KB |
| Vertex cache (64 entries) | about 2.8 KB |
| Span pool | 1/4 of the remainder (32 to 512 spans, 64 bytes each; 72 at correction level 2) |
| Triangle buffer | the rest (136 bytes per triangle plus 4 bytes of indices; 124 + 4 at correction level 0) |

For example, a 128 KB arena at 480x320 holds about 670 triangles and 490 spans
with the default settings. When a buffer overflows, the excess triangles or
spans are dropped for that frame; `Graphics3D::getStats()` reports capacities,
peak usage and drop counts so you can size the arena.

The 2D API needs no working memory beyond the target buffer.

## Constraints

- 3D texture width and height must be powers of two (coordinates are wrapped
  with a bit mask; other sizes tile incorrectly).
- The 3D renderer outputs RGB565BE and RGB444 only; other target formats are
  ignored. Textures may be in any enabled format.
- Triangles that cross or lie in front of the near plane are dropped rather
  than clipped.
- 3D pixels are quantized by truncation in fixed point (within about 1 LSB of a
  float implementation).
- Polygons filled by `Graphics2D::fillPolygon()` may have at most 16 edge
  crossings per scanline.
- Neither renderer is thread-safe; use one `Graphics3D` / `Graphics2D` per thread.

## Performance reference

One 480x320 frame of the demo3d scene (torus + 4 cubes, with translucency,
texture and environment mapping):

- WebAssembly on a desktop PC (Node.js, single thread): well under 1 ms
- RP2350 @ 312 MHz, single core: about 55 ms for `render()` (15 to 18 fps),
  measured before per-pixel processing was converted to fixed point and with
  correction level 0

## Testing

`test/` contains self-checking tests for the pixel helpers, `Graphics2D` and
the 3D renderer (banded vs. whole-frame rendering, output formats, transparent
clear, texture formats). Run them with sanitizers for the strictest check:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

## License

MIT. See [LICENSE](LICENSE), which also contains the notice for the bundled
Adafruit `gfxfont.h` (BSD).
