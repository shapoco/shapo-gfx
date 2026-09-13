# ShapoGFX

2D/3D graphics libraries for embedded systems.

ShapoGFX is a small, dependency-free C++17 software renderer aimed at
microcontrollers driving RGB565 displays. The 3D renderer draws scanline by
scanline without a frame buffer or a Z buffer, and all of its working memory
comes from an arena that you provide.

- **Live demo:** https://shapoco.github.io/shapo-gfx/example/demo3d/
- **Specification:** [SPEC.md](SPEC.md)

## Features

### `shapoco::gfx3d`

- Scanline rasterization with per-line span lists: no frame buffer, no Z buffer
- Renders any rectangular region of the screen, so the output can be streamed to
  a display in bands from a small transfer buffer
- No dynamic allocation; everything is carved from a user-supplied arena
- Directional light, ambient light, Gouraud shading
- Texture mapping (RGB565), environment mapping
- Alpha blending and additive blending with correct ordering against opaque geometry
- Triangle lists, strips and fans; a built-in subdivided box
- OpenGL-style right-handed coordinate system and matrix stack
- Perspective-correct texture mapping in three levels: off, vertical only
  (default, two divides per span) or full (per-pixel divide)
- Per-pixel work is integer only (16-bit fixed point); vertex work is `float`
- Pure C++17, no platform dependencies; multiple independent `Renderer` instances

### `shapoco::gfx2d`

Currently header-only and shared with the 3D renderer: 2D vectors, float colors,
the `Texture` type, blend modes and RGB565 pixel helpers (packing, fills, alpha
and additive blending). A 2D drawing API is planned.

## Directory layout

```
include/shapoco/gfx2d/   public headers shared by 2D and 3D (math2d, pixel, texture)
include/shapoco/gfx3d/   public headers of the 3D renderer (gfx3d.hpp, math3d.hpp)
src/gfx3d/               3D renderer implementation
example/demo3d/          sample program (shared scene, WASM/native entry point)
docs/example/demo3d/     browser viewer for the sample (index.html, main.js, demo3d.wasm)
```

## Quick start

```c++
#include "shapoco/gfx3d/gfx3d.hpp"

namespace g3 = shapoco::gfx3d;

static uint8_t arena[64 * 1024];
static uint16_t band[320 * 40];   // 40-line transfer buffer
static g3::Renderer renderer;

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
        renderer.render(0, y, 320, 40, band, 320);
        // ... send `band` to the display at (0, y) ...
    }
    renderer.endRender();
}
```

Model data (vertex arrays, index arrays, textures) is only referenced, never
copied, so it can live in flash as `const` data.

## Building

### With CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/example/demo3d/demo3d out.ppm   # renders one frame of the sample to a PPM file
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
| `SHAPOGFX_BUILD_EXAMPLES` | `ON` when top-level | Build `example/demo3d` |

### Without CMake

Add `include/` to the include path and compile `src/gfx3d/gfx3d.cpp` with C++17.
Define `SHAPOGFX3D_CORRECT_PERSPECTIVE=<level>` when compiling `gfx3d.cpp` to
change the perspective correction level (see below).

### Perspective correction

`SHAPOGFX3D_CORRECT_PERSPECTIVE` selects how texture coordinates are
interpolated. It only affects the compilation of `gfx3d.cpp`.

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

## Sample: demo3d

`example/demo3d/` renders a 480x320 scene with a textured floor, a chrome
(environment-mapped) torus and three cubes (opaque, alpha-blended, additive).
The same scene is used by both the browser and the native build. The sample
uses the library's default perspective correction level; the floor is a single
large quad per face, which shows the effect of the correction clearly.

Browser (requires [Emscripten](https://emscripten.org/)):

```sh
cd example/demo3d
make                      # builds docs/example/demo3d/demo3d.wasm
cd ../..
./launch_web_server.sh    # serves docs/ on http://localhost:52880/
```

Then open http://localhost:52880/example/demo3d/ (the page uses `fetch()`, so it
does not work from `file://`). `make serve` in `example/demo3d/` does the same on
port 8000. Drag or use the arrow keys to rotate the camera,
and the wheel or PageUp/PageDown to zoom.

## Memory usage

All working memory is taken from the arena passed to `Renderer::init()`. On a
32-bit target the layout is roughly:

| Region | Size |
|---|---|
| Line buckets | screen height x 4 bytes |
| Matrix stack (16 entries) | about 1.1 KB |
| Vertex cache (64 entries) | about 2.8 KB |
| Span pool | 1/4 of the remainder (32 to 512 spans, 64 bytes each; 72 at correction level 2) |
| Triangle buffer | the rest (136 bytes per triangle plus 4 bytes of indices; 124 + 4 at correction level 0) |

For example, a 128 KB arena at 480x320 holds about 670 triangles and 490 spans
with the default settings.
When a buffer overflows, the excess triangles or spans are dropped for that
frame; `Renderer::getStats()` reports capacities, peak usage and drop counts so
you can size the arena.

## Constraints

- Texture width and height must be powers of two (coordinates are wrapped with a
  bit mask; other sizes tile incorrectly).
- Triangles that cross or lie in front of the near plane are dropped rather than
  clipped.
- Pixels are quantized to RGB565 by truncation in fixed point (within about 1 LSB
  of a float implementation).
- The renderer is not thread-safe; use one `Renderer` per thread.

## Performance reference

One 480x320 frame of the sample scene (torus + 4 cubes, with translucency,
texture and environment mapping):

- WebAssembly on a desktop PC (Node.js, single thread): well under 1 ms
- RP2350 @ 312 MHz, single core: about 55 ms for `render()` (15 to 18 fps),
  measured before per-pixel processing was converted to fixed point and with
  correction level 0

## License

MIT. See [LICENSE](LICENSE).
