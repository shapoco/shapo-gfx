#ifndef RP2COMMON_DIAG_HPP
#define RP2COMMON_DIAG_HPP

// Post-mortem diagnostics of the frame loop. A HardFault (on either core)
// records the faulting PC, LR, SP and fault status registers in RAM that a
// reset leaves alone and reboots through the watchdog; a hang (no frame for
// RP2COMMON_WATCHDOG_MS) lets the watchdog reboot with the last progress
// marks of both cores. diagReport() prints what the previous run left, once.
//
// Look a PC up with: arm-none-eabi-addr2line -Cfpe build/<demo>.elf 0x...

#include <cstdint>

#ifndef RP2COMMON_WATCHDOG_MS
#define RP2COMMON_WATCHDOG_MS 3000
#endif

namespace rp2common {

// What a core is doing (diagMark)
enum class Phase : uint32_t {
  IDLE,          // core1 waiting for a job
  UPDATE,        // core0: touch and App::update()
  DRAW,          // drawing rows (App::drawRows())
  WAIT_CORE1,    // core0 waiting for core1
  WAIT_PANEL,    // core0 waiting for the DMA / SPI
  END_FRAME,     // core0: App::endFrame()
};

// Note what core `core` is doing; cheap (two stores)
void diagMark(int core, Phase phase, int y);

// Print the record of the previous run, if any, and clear it. Call once
// stdio is up.
void diagReport();

// Arm the watchdog (call before the loop), and feed it (once per frame)
void diagStartWatchdog();
void diagFeedWatchdog();

}  // namespace rp2common

#endif
