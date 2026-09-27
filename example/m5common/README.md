# demorig on M5Stack devices

ESP-IDF 5.5 projects that run demorig (the scene of `example/common/demorig/`, also
built for the browser by `example/wasm/demorig/`) on two M5Stack devices:

| Project | Device | Frame | Output | Input |
|---|---|---|---|---|
| `example/m5cores3/demorig/` | M5Stack CoreS3 (ESP32-S3) | 320x240 | SPI DMA, 40 MHz | zoom buttons, swipe to scroll |
| `example/m5tab5/demorig/` | M5Stack Tab5 (ESP32-P4) | 640x360 | PPA, scaled x2 into the 1280x720 DSI framebuffer | zoom buttons, swipe to scroll |

```sh
cd example/m5cores3/demorig
./build.sh             # ESP-IDF from $IDF_ROOT (default ~/esp/5.5)
./run.sh [PORT]        # build and flash
./monitor.sh [PORT]    # serial log
```

M5Unified and M5GFX are fetched by the IDF component manager on the first build
(each project's `main/idf_component.yml`; the Tab5 pins M5GFX 0.2.25 because
`panel_ppa.cpp` reaches into its `Panel_DSI`).

## Components (`components/`)

- `shapogfx`: gfx2d of this repository (GRAY1 and RGB444 compiled out)
- `demorig`: `example/common/demorig/` (the scene, the view, the buttons and the
  frame rate display; ShapoGFX only), built with `DEMORIG_MODEL_HEADER` set to
  `model/rgb_chan_sep.hpp`: one texture per image instead of the atlas, since the
  parts come from flash through the cache and the atlas rows' padding would cost a
  third more cache lines per frame
- `m5demorig`: the front end: `runner.cpp` (M5Unified bring-up, touch, the frame
  loop), `panel_spi.cpp` (ESP32-S3) or `panel_ppa.cpp` (ESP32-P4)

## Frame loop

A frame is drawn in strips of 60 rows. The upper half of a strip is drawn by
core0 and the lower half by a task on core1 at the same time, into one of two
strip buffers in internal RAM; while the next strip is drawn into the other
buffer, the previous one goes out to the panel. The last strip of a
frame stays in flight while the next frame is updated.

The serial log shows, every 2 seconds, the frame rate, the zoom and the time per
frame spent updating the scene, drawing on each core, waiting for core1 and
waiting for the panel. `idf.py -DM5DEMORIG_DUAL_CORE=0 build` draws on core0
alone for comparison (`-DM5DEMORIG_DUAL_CORE=1` switches back).

Sending a whole frame takes about 31 ms on the CoreS3 (SPI at 40 MHz), which caps
it at about 32 fps. Measured at zoom 1 (2026-09-27): CoreS3 24 fps, Tab5 42 fps
with the per-image textures (Tab5 was 30 fps with the atlas, whose footprint does
not fit its 256 KB L2 cache while the dense textures do; the CoreS3's 64 KB cache
holds neither, so it did not change). `dbones2cpp --out-format rgb565_swapped`
(a key color instead of alpha: copies instead of blends, no soft edges) would take
another third off the drawing time; see docsrc/tools/dbones2cpp.rst.
