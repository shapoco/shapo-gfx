# ShapoGFX

English | [日本語](README.ja.md)

2D/3D graphics libraries for embedded systems.

ShapoGFX is a small, dependency-free C++17 software renderer for
microcontrollers driving small displays: a 2D drawing API with bitmap fonts
and a scanline 3D renderer that needs neither a frame buffer nor a Z buffer.
The library never allocates memory; it draws into buffers you provide and
takes its working memory from an arena you hand it.

- **Documentation (Japanese):** https://shapoco.github.io/shapo-gfx/ref/
- **Live demos:** [demo2d](https://shapoco.github.io/shapo-gfx/example/demo2d/),
  [demo3d](https://shapoco.github.io/shapo-gfx/example/demo3d/),
  [demorig](https://shapoco.github.io/shapo-gfx/example/demorig/)
- **Design specification (English):** [SPEC.md](SPEC.md)

## Highlights

- Pixel formats GRAY1, RGB444, ARGB4444, RGB565_SWAPPED (byte-swapped, ready for
  DMA to display controllers) and, opt-in, RGB565 (native byte order, for 16-bit
  display interfaces); unused formats can be compiled out
- `Graphics2D`: shapes, lines, polygons, blits with alpha/additive blending,
  two-color bitmaps, GFXfont text with four bundled fonts
- `vg`: vector graphics: paths of lines and bezier curves, filled (nonzero or
  even-odd) with colors or linear / radial gradients, stroked with a width, caps
  and joins, antialiased, grouped into pictures converted from SVG
- `rig`: 2D skeletal animation of DragonBones characters (bones, slots, keyframes
  interpolated at any frame rate, draw order) and of animated SVG (the shapes as
  vector attachments), drawn with `Graphics2D`
- `Graphics3D`: scanline rasterizer rendering any screen region into a band
  buffer; Gouraud shading, textures in any format, environment mapping,
  alpha/additive blending, perspective-correct texturing, built-in shapes,
  static scene graphs with a visitor hook for animation
- Tools: `img2cpp` (images), `gltf2cpp` (glTF 2.0 models), `dbones2cpp`
  (DragonBones armatures) and `svg2cpp` (SVG pictures and SMIL animations)
  generate `const` data headers
- Self-checking tests meant to run under ASan/UBSan

## Applications

- [Devour Sphere](https://github.com/shapoco/devour-sphere): a 3D shooter
  running on RP2350 / RP2040 / ESP32-S3 / ESP32-P4 boards and in the browser,
  written as a showcase of ShapoGFX

## Installation

The standard way to use ShapoGFX is to clone the repository once and point the
environment variable `SHAPOGFX_PATH` at the clone. Build files then refer to
the library through that variable instead of a hard-coded path.

1. Create `${HOME}/sgfx/` and move into it:

   ```sh
   mkdir -p ${HOME}/sgfx
   cd ${HOME}/sgfx
   ```

2. Clone the repository:

   ```sh
   git clone https://github.com/shapoco/shapo-gfx.git
   ```

3. Set `SHAPOGFX_PATH` to `${HOME}/sgfx/shapo-gfx`. Add the same line to
   `~/.bashrc` (or your shell's equivalent) so it survives a new shell:

   ```sh
   export SHAPOGFX_PATH=${HOME}/sgfx/shapo-gfx
   echo 'export SHAPOGFX_PATH=${HOME}/sgfx/shapo-gfx' >> ~/.bashrc
   ```

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

To use the library from another CMake project (including Pico SDK projects):

```cmake
add_subdirectory($ENV{SHAPOGFX_PATH} shapo-gfx)
target_link_libraries(your_target PRIVATE shapoco::gfx)
```

### ESP-IDF

The repository is an ESP-IDF component named `shapo-gfx`. Add it to the
component directories in the `CMakeLists.txt` of the project:

```cmake
set(EXTRA_COMPONENT_DIRS $ENV{SHAPOGFX_PATH})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(your_project)
```

or, without a local clone, to the dependencies in `main/idf_component.yml`:

```yaml
dependencies:
  shapo-gfx:
    git: https://github.com/shapoco/shapo-gfx.git
    version: v1.6.0
```

(`path: ${SHAPOGFX_PATH}` in place of `git:` and `version:` refers to the local
clone). The key has to be `shapo-gfx`, the name of the directory. Then require
it from the components that use it:

```cmake
idf_component_register(SRCS "app_main.cpp" REQUIRES shapo-gfx)
```

The compile-time options are under "ShapoGFX" in `idf.py menuconfig`
(`CONFIG_SHAPOGFX_FORMAT_GRAY1=n` and so on in `sdkconfig.defaults`).

### PlatformIO

`library.json` makes the repository usable as a PlatformIO library:

```ini
[env:my_board]
platform = espressif32
board = seeed_xiao_esp32s3
framework = arduino
lib_deps = symlink://${sysenv.SHAPOGFX_PATH}
; or straight from GitHub, without a local clone:
; lib_deps = https://github.com/shapoco/shapo-gfx.git
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
include/shapoco/gfx2d/   2D API and shared types (pixel formats, Surface, Graphics2D, fonts, vg, rig)
include/shapoco/gfx3d/   3D renderer (Graphics3D, shapes, static scenes, math)
src/                     implementation
bin/                     img2cpp, gltf2cpp, dbones2cpp, svg2cpp and their requirements
library.json             PlatformIO manifest
example/wasm/            demo2d, demo3d, demorig (WASM and native entry points)
example/common/demorig/  demorig's scene, view and overlay (shared by the WASM and M5Stack builds)
example/m5*/demorig/     demorig on M5Stack CoreS3 / Tab5 (ESP-IDF projects)
example/m5common/        their shared ESP-IDF components (see its README)
docs/                    published site: demo pages and their WASM builds
docsrc/                  Sphinx sources of the manual, deployed to /ref/ by CI
                         (`make -C docsrc preview` to read it locally)
test/                    self-checking tests
```

## License

MIT. See [LICENSE](LICENSE), which also contains the notice for the bundled
Adafruit `gfxfont.h` (BSD).
