# ShapoGFX

2D/3D graphics libraries for embedded systems.

ShapoGFX is a small, dependency-free C++17 software renderer for
microcontrollers driving small displays: a 2D drawing API with bitmap fonts
and a scanline 3D renderer that needs neither a frame buffer nor a Z buffer.
The library never allocates memory; it draws into buffers you provide and
takes its working memory from an arena you hand it.

- **Documentation (Japanese):** https://shapoco.github.io/shapo-gfx/ref/
- **Live demos:** [demo2d](https://shapoco.github.io/shapo-gfx/example/demo2d/),
  [demo3d](https://shapoco.github.io/shapo-gfx/example/demo3d/)
- **Design specification (English):** [SPEC.md](SPEC.md)

## Highlights

- Pixel formats GRAY1, RGB444, ARGB4444, RGB565_SWAPPED (byte-swapped, ready for
  DMA to display controllers) and, opt-in, RGB565 (native byte order, for 16-bit
  display interfaces); unused formats can be compiled out
- `Graphics2D`: shapes, lines, polygons, blits with alpha/additive blending,
  two-color bitmaps, GFXfont text with four bundled fonts
- `Graphics3D`: scanline rasterizer rendering any screen region into a band
  buffer; Gouraud shading, textures in any format, environment mapping,
  alpha/additive blending, perspective-correct texturing, built-in shapes,
  static scene graphs with a visitor hook for animation
- Tools: `img2cpp` (images) and `gltf2cpp` (glTF 2.0 models) generate `const`
  data headers
- Self-checking tests meant to run under ASan/UBSan

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

To use the library from another CMake project (including Pico SDK projects):

```cmake
add_subdirectory(path/to/shapo-gfx)
target_link_libraries(your_target PRIVATE shapoco::gfx)
```

### PlatformIO

`library.json` makes the repository usable as a PlatformIO library:

```ini
[env:my_board]
platform = espressif32
board = seeed_xiao_esp32s3
framework = arduino
lib_deps = https://github.com/shapoco/shapo-gfx.git
; the headers are C++17; many cores still default to gnu++11
build_unflags = -std=gnu++11
build_flags = -std=gnu++17
```

### Without a build system

Add `include/` to the include path and compile `src/gfx2d/*.cpp` and
`src/gfx3d/*.cpp` with C++17.

Python tooling and documentation dependencies:

```sh
python3 -m pip install -r requirements.txt
```

## Repository layout

```
include/shapoco/gfx2d/   2D API and shared types (pixel formats, Surface, Graphics2D, fonts)
include/shapoco/gfx3d/   3D renderer (Graphics3D, shapes, static scenes, math)
src/                     implementation
bin/                     img2cpp, gltf2cpp and their requirements
library.json             PlatformIO manifest
example/wasm/            demo2d, demo3d (WASM and native entry points)
docs/                    published site: demo pages and their WASM builds
docsrc/                  Sphinx sources of the manual, deployed to /ref/ by CI
                         (`make -C docsrc preview` to read it locally)
test/                    self-checking tests
```

## License

MIT. See [LICENSE](LICENSE), which also contains the notice for the bundled
Adafruit `gfxfont.h` (BSD).
