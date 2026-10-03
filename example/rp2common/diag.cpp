#include "diag.hpp"

#include <cstdio>

#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"
#include "pico/stdlib.h"

namespace rp2common {

namespace {

constexpr uint32_t MAGIC_FAULT = 0xFA17C0DEu;
constexpr uint32_t MAGIC_LIVE = 0x11FE11FEu;  // progress marks are valid

struct Progress {
  uint32_t phase, y, count;
};

struct Record {
  uint32_t magic;
  uint32_t core;
  uint32_t pc, lr, sp, xpsr;
  uint32_t cfsr, hfsr, mmfar, bfar;
};

// Left alone by the reset and by crt0 (.uninitialized_data)
Record __uninitialized_ram(g_fault);
uint32_t __uninitialized_ram(g_liveMagic);
Progress __uninitialized_ram(g_progress)[2];

// System Control Block fault registers (Armv8-M)
volatile uint32_t &reg(uintptr_t addr) {
  return *reinterpret_cast<volatile uint32_t *>(addr);
}
constexpr uintptr_t SCB_CFSR = 0xE000ED28, SCB_HFSR = 0xE000ED2C,
                    SCB_MMFAR = 0xE000ED34, SCB_BFAR = 0xE000ED38;

const char *phaseName(uint32_t p) {
  switch ((Phase)p) {
    case Phase::IDLE: return "idle";
    case Phase::UPDATE: return "update";
    case Phase::DRAW: return "draw";
    case Phase::WAIT_CORE1: return "wait core1";
    case Phase::WAIT_PANEL: return "wait panel";
    case Phase::END_FRAME: return "end frame";
  }
  return "?";
}

}  // namespace

void diagMark(int core, Phase phase, int y) {
  g_progress[core].phase = (uint32_t)phase;
  g_progress[core].y = (uint32_t)y;
  g_progress[core].count++;
}

void diagStartWatchdog() {
  for (auto &p : g_progress) p = {};
  g_liveMagic = MAGIC_LIVE;
  watchdog_enable(RP2COMMON_WATCHDOG_MS, true);
}

void diagFeedWatchdog() { watchdog_update(); }

void diagReport() {
  // Only a timeout of the watchdog armed by diagStartWatchdog() (not the
  // reboots of picotool or of the fault handler)
  const bool byWatchdog = watchdog_enable_caused_reboot();
  if (g_fault.magic == MAGIC_FAULT) {
    const Record &r = g_fault;
    std::printf(
        "*** previous run: HardFault on core%lu: pc 0x%08lx lr 0x%08lx sp "
        "0x%08lx xpsr 0x%08lx\n"
        "    cfsr 0x%08lx hfsr 0x%08lx mmfar 0x%08lx bfar 0x%08lx\n",
        (unsigned long)r.core, (unsigned long)r.pc, (unsigned long)r.lr,
        (unsigned long)r.sp, (unsigned long)r.xpsr, (unsigned long)r.cfsr,
        (unsigned long)r.hfsr, (unsigned long)r.mmfar, (unsigned long)r.bfar);
  } else if (byWatchdog && g_liveMagic == MAGIC_LIVE) {
    std::printf("*** previous run: hung (watchdog, no frame for %d ms)\n",
                RP2COMMON_WATCHDOG_MS);
  }
  if ((g_fault.magic == MAGIC_FAULT || byWatchdog) &&
      g_liveMagic == MAGIC_LIVE) {
    // Where the cores were (core1's mark is that of its last job)
    for (int c = 0; c < 2; c++) {
      std::printf("    core%d last mark: %s, y %lu (mark #%lu)\n", c,
                  phaseName(g_progress[c].phase),
                  (unsigned long)g_progress[c].y,
                  (unsigned long)g_progress[c].count);
    }
  }
  g_fault.magic = 0;
  g_liveMagic = 0;
}

}  // namespace rp2common

// Called by isr_hardfault with the exception frame (r0 r1 r2 r3 r12 lr pc
// xpsr) of the stack the fault was taken on
extern "C" void __used rp2commonFault(const uint32_t *frame) {
  using namespace rp2common;
  g_fault.core = get_core_num();
  g_fault.pc = frame[6];
  g_fault.lr = frame[5];
  g_fault.sp = (uint32_t)(uintptr_t)frame;
  g_fault.xpsr = frame[7];
  g_fault.cfsr = reg(SCB_CFSR);
  g_fault.hfsr = reg(SCB_HFSR);
  g_fault.mmfar = reg(SCB_MMFAR);
  g_fault.bfar = reg(SCB_BFAR);
  g_fault.magic = MAGIC_FAULT;
  watchdog_reboot(0, 0, 0);
  for (;;) __wfi();
}

// Replaces the SDK's (weak) handler: pick the stack the fault was taken on
extern "C" __attribute__((naked)) void isr_hardfault() {
  __asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "b rp2commonFault\n");
}
