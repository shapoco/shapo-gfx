# ShapoGFX demos on the Waveshare RP2350-Touch-LCD-2

Pico SDK projects that run demo2d, demo3d and demorig (the scenes of
`example/common/`, also built for the browser by `example/wasm/`) on the
[Waveshare RP2350-Touch-LCD-2](https://www.waveshare.com/wiki/RP2350-Touch-LCD-2)
(or -C, the same board with a camera): RP2350A, 2-inch 240x320 ST7789T3 panel
on SPI, CST816D touch on I2C, 16 MB flash.

| Project | Frame | Input |
|---|---|---|
| `demo2d/` | 320x240, the compact layout of the scene, frame rate at the bottom left | none |
| `demo3d/` | 320x240, two render contexts (one per core), the rasterizer in RAM | drag to turn the camera, (+) / (-) to move it closer / farther |
| `demorig/` | 320x240, the atlas model (`rgb_chan.hpp`) | zoom and (AA) buttons, swipe to scroll |

The screen is used in landscape. `-DRP2COMMON_LCD_FLIP=ON` turns it by 180
degrees.

```sh
cd example/rp2350-touch-lcd-2
./build.sh                  # all three: <demo>/build/<demo>.uf2
./build.sh demorig -- -DDEMORIG_ATLAS=OFF  # one, with cmake options
./flash.sh demorig          # picotool load -f -x (or copy the .uf2 to the RPI-RP2 drive)
```

The Pico SDK comes from `$PICO_SDK_PATH` (default `~/pico/pico-sdk`), picotool
from `$picotool_DIR` or the newest one under `~/.pico-sdk/picotool/`. The
programs log over USB (stdio): the clocks at startup, then every 2 seconds the
frame rate and the time per frame spent reading the touch, updating, drawing on
each core, waiting for core1 and waiting for the panel.

## Common part (`example/rp2common/`)

- `boards/waveshare_rp2350_touch_lcd_2.h`: the Pico SDK board header (16 MB
  flash, default pins) with the panel and touch pins as `RP2COMMON_*`
- `pico_project.cmake`: SDK import and board selection, included before
  `project()`
- `clocks.cpp`: 250 MHz at 1.20 V. The flash interface is set to
  clk_sys / 3 (83 MHz, sampling delay 2; the bootrom leaves the same) before
  the clock goes up. clk_peri is put back on clk_sys, divided by 2 (125 MHz):
  the SDK moves it to the 48 MHz USB PLL when the system clock changes, which
  would leave the panel's SPI at 24 MHz. The SPI divides it by 2: 62.5 MHz
- `diag.cpp`: post-mortem diagnostics. A HardFault on either core records the
  PC, LR, SP and the fault status registers in RAM that survives a reset and
  reboots; a hang (no frame for 3 s) lets the watchdog reboot. The next run
  prints what happened and where each core was (`*** previous run: ...`);
  look the PC up with `arm-none-eabi-addr2line -Cfpe <demo>/build/<demo>.elf 0x...`
- `panel_st7789.cpp`: the panel (Waveshare's initialization sequence, MADCTL
  0x28 for landscape), written by DMA in strips; one window per frame, so a
  strip costs no commands
- `touch_cst816.cpp`: the touch controller, polled once per frame while the
  last strip is still going out (it shares its reset pin with the panel, so it
  is reset once, by the panel)
- `runner.cpp`: the frame loop, as in the M5Stack builds of demorig
  (`example/m5common/`): a frame is drawn in strips of 60 rows, the upper half
  of a strip on core0 and the lower half on core1 at the same time, into one of
  two strip buffers; while the next strip is drawn, the previous one goes out
  by DMA, and the last one of a frame stays in flight while the next frame is
  updated. core1 runs on an 8 KB stack of its own.

Options (`./build.sh <demo> -- -D...`):

| Option | Default | |
|---|---|---|
| `RP2COMMON_INTERP` | ON | ShapoGFX's SIO interpolator paths (`SHAPOGFX2D_RP2_INTERP`, `SHAPOGFX3D_RP2_INTERP`); OFF for the portable code |
| `RP2COMMON_DUAL_CORE` | ON | OFF draws every strip on core0 alone, for comparison |
| `RP2COMMON_LCD_FLIP` | OFF | turn the screen by 180 degrees |
| `RP2COMMON_STDIO_UART` | OFF | log to UART0 (GP0 / GP1) instead of USB |
| `DEMO3D_HOT` (demo3d) | ON | the span rasterizer in RAM (`SHAPOGFX3D_HOT_ATTR`) |
| `DEMORIG_ATLAS` (demorig) | ON | the atlas model (`rgb_chan.hpp`): a power-of-two stride, so the turned parts take the interpolator path, though more bytes are read from flash per frame; OFF for one texture per image (`rgb_chan_sep.hpp`, as on the M5Stack). The atlas was a little faster: 28.6 against 28.0 fps at zoom 1 |

The clocks are set by cache variables of the same names as the macros of
`clocks.hpp` and `runner.cpp` (empty: the defaults): `RP2COMMON_SYS_KHZ`
(250000), `RP2COMMON_VREG` (`VREG_VOLTAGE_1_20`), `RP2COMMON_FLASH_CLKDIV` (3),
`RP2COMMON_FLASH_RXDELAY` (2), `RP2COMMON_PERI_DIV` (2) and
`RP2COMMON_LCD_BAUD` (62500000), e.g.
`./build.sh demorig -- -DRP2COMMON_FLASH_CLKDIV=4` if the flash does not keep
up. (Do not pass them in `CMAKE_CXX_FLAGS`: that replaces the SDK's CPU flags.)

Sending a whole frame takes 19.7 ms at 62.5 MHz, which caps the frame rate at
about 50 fps. The panel's tearing effect output is not wired to the RP2350, so
the frames are not synchronized with its refresh.
