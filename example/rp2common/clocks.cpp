#include "clocks.hpp"

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

namespace rp2common {

namespace {

ClockReport g_report;

// Runs from RAM with the interrupts off, so that nothing is fetched from the
// flash while its timing changes
void __no_inline_not_in_flash_func(setFlashTiming)(uint32_t clkdiv,
                                                   uint32_t rxdelay) {
  const uint32_t irq = save_and_disable_interrupts();
  uint32_t t = qmi_hw->m[0].timing;
  t &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
  t |= (clkdiv << QMI_M0_TIMING_CLKDIV_LSB) & QMI_M0_TIMING_CLKDIV_BITS;
  t |= (rxdelay << QMI_M0_TIMING_RXDELAY_LSB) & QMI_M0_TIMING_RXDELAY_BITS;
  qmi_hw->m[0].timing = t;
  __dsb();
  __isb();
  restore_interrupts(irq);
}

}  // namespace

ClockConfig defaultClockConfig() {
  return {RP2COMMON_SYS_KHZ, RP2COMMON_VREG, RP2COMMON_FLASH_CLKDIV,
          RP2COMMON_FLASH_RXDELAY, RP2COMMON_PERI_DIV};
}

bool initClocks(const ClockConfig &cfg) {
  g_report.qmiTimingBefore = qmi_hw->m[0].timing;
  // Voltage first, then a slower flash, and only then the faster clock
  vreg_set_voltage((enum vreg_voltage)cfg.vreg);
  sleep_ms(10);
  setFlashTiming((uint32_t)cfg.flashClkdiv, (uint32_t)cfg.flashRxdelay);
  g_report.qmiTimingAfter = qmi_hw->m[0].timing;
  const bool ok = set_sys_clock_khz(cfg.sysKhz, false);
  const uint32_t sysHz = clock_get_hz(clk_sys);
  clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, sysHz,
                  sysHz / (uint32_t)cfg.periDiv);
  g_report.sysHz = sysHz;
  g_report.periHz = clock_get_hz(clk_peri);
  return ok;
}

const ClockReport &clockReport() { return g_report; }

}  // namespace rp2common
