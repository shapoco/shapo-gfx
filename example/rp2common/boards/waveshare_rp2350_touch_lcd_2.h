// Pico SDK board header of the Waveshare RP2350-Touch-LCD-2 (and -C, the
// same board with a camera): RP2350A, 16 MB W25Q128JV flash, 2-inch
// 240x320 ST7789T3 IPS panel on SPI0, CST816D touch and QMI8658 IMU on I2C0.
// Pins from the board's schematic and Waveshare's demo code
// (https://www.waveshare.com/wiki/RP2350-Touch-LCD-2).
//
// Select it with
//   set(PICO_BOARD waveshare_rp2350_touch_lcd_2)
//   set(PICO_BOARD_HEADER_DIRS <this directory>)
// (example/rp2common/pico_project.cmake does). Besides the SDK's defaults it
// names the panel and touch pins as RP2COMMON_* for example/rp2common/.

#ifndef _BOARDS_WAVESHARE_RP2350_TOUCH_LCD_2_H
#define _BOARDS_WAVESHARE_RP2350_TOUCH_LCD_2_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define WAVESHARE_RP2350_TOUCH_LCD_2

#define PICO_RP2350A 1

// --- UART (pins of the expansion header) ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- I2C: touch and IMU ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 12
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 13
#endif

// --- SPI: the panel (write only) ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 18
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 19
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 17
#endif

// --- Panel: ST7789T3, 240x320 (portrait) ---
#define RP2COMMON_LCD_SPI 0
#define RP2COMMON_LCD_SCK_PIN 18
#define RP2COMMON_LCD_MOSI_PIN 19
#define RP2COMMON_LCD_CS_PIN 17
#define RP2COMMON_LCD_DC_PIN 16
#define RP2COMMON_LCD_RST_PIN 20  // shared with the touch controller
#define RP2COMMON_LCD_BL_PIN 15   // backlight, PWM
#define RP2COMMON_LCD_WIDTH 240
#define RP2COMMON_LCD_HEIGHT 320

// --- Touch: CST816D ---
#define RP2COMMON_TOUCH_I2C 0
#define RP2COMMON_TOUCH_SDA_PIN 12
#define RP2COMMON_TOUCH_SCL_PIN 13
#define RP2COMMON_TOUCH_INT_PIN 29
#define RP2COMMON_TOUCH_RST_PIN 20  // the panel's reset
#define RP2COMMON_TOUCH_ADDR 0x15

// --- Flash ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
