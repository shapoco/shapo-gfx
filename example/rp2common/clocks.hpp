#ifndef RP2COMMON_CLOCKS_HPP
#define RP2COMMON_CLOCKS_HPP

// Overclocking of the RP2350: the core voltage, the flash interface (QMI)
// divider, then the system clock, and clk_peri (the SPI's clock) from it:
// the SDK moves clk_peri to the 48 MHz USB PLL when the system clock changes
// after startup (PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK is 0).

#include <cstdint>

namespace rp2common {

struct ClockConfig {
  uint32_t sysKhz;    // system clock, e.g. 250000
  int vreg;           // core voltage, a VREG_VOLTAGE_* value
  int flashClkdiv;    // QMI divider of the flash clock (clk_sys / n)
  int flashRxdelay;   // QMI sampling delay in half clk_sys cycles
  int periDiv;        // clk_peri = clk_sys / n (1..3)
};

// 250 MHz at 1.20 V, the flash at 250 / 3 = 83 MHz (the W25Q128JV takes up
// to 133 MHz; the bootrom leaves the same divider and delay, 0x...7203, and
// the SDK's own default is 150 / 2 = 75 MHz with a delay of 2), clk_peri at
// 250 / 2 = 125 MHz, which the SPI divides by 2 for the panel's 62.5 MHz.
// Override the macros to try other settings.
#ifndef RP2COMMON_SYS_KHZ
#define RP2COMMON_SYS_KHZ 250000
#endif
#ifndef RP2COMMON_VREG
#define RP2COMMON_VREG VREG_VOLTAGE_1_20
#endif
#ifndef RP2COMMON_FLASH_CLKDIV
#define RP2COMMON_FLASH_CLKDIV 3
#endif
#ifndef RP2COMMON_FLASH_RXDELAY
#define RP2COMMON_FLASH_RXDELAY 2
#endif
#ifndef RP2COMMON_PERI_DIV
#define RP2COMMON_PERI_DIV 2
#endif

ClockConfig defaultClockConfig();

// Call first thing in main(), before stdio and before core1 runs. False if
// the system clock could not be set (it then stays as it was).
bool initClocks(const ClockConfig &cfg);

// What initClocks() found and set, for the log
struct ClockReport {
  uint32_t qmiTimingBefore, qmiTimingAfter;
  uint32_t sysHz, periHz;
};
const ClockReport &clockReport();

}  // namespace rp2common

#endif
