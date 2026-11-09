#ifndef SHAPOGFX_COMMON_ARCH_DETECT_HPP
#define SHAPOGFX_COMMON_ARCH_DETECT_HPP

// Target detection shared by the 2D and the 3D renderer (internal; included
// by src/*.cpp only). Detects the target, or define one of these to 1 by
// hand:
//   SHAPOGFX_ARCH_RP2      RP2040 / RP2350 (Pico SDK)
//   SHAPOGFX_ARCH_ESP32S3  ESP32-S3 (ESP-IDF)
//   SHAPOGFX_ARCH_ESP32P4  ESP32-P4 (ESP-IDF)
// Anything else is SHAPOGFX_ARCH_GENERIC -- including the ESP8266, which the
// ESP8266_RTOS_SDK builds with ESP_PLATFORM defined as well (its sdkconfig
// names CONFIG_IDF_TARGET_ESP8266, which selects none of the targets above;
// see SHAPOGFX_ARCH_WORD_LOADS below).

#if !defined(SHAPOGFX_ARCH_RP2) && !defined(SHAPOGFX_ARCH_ESP32S3) && \
    !defined(SHAPOGFX_ARCH_ESP32P4) && !defined(SHAPOGFX_ARCH_GENERIC)
#if defined(PICO_RP2040) || defined(PICO_RP2350)
#define SHAPOGFX_ARCH_RP2 1
#elif defined(ESP_PLATFORM)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define SHAPOGFX_ARCH_ESP32S3 1
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
#define SHAPOGFX_ARCH_ESP32P4 1
#endif
#endif
#endif
#if !defined(SHAPOGFX_ARCH_RP2) && !defined(SHAPOGFX_ARCH_ESP32S3) && \
    !defined(SHAPOGFX_ARCH_ESP32P4)
#define SHAPOGFX_ARCH_GENERIC 1
#endif

// SHAPOGFX_ARCH_FPU: 1 where float arithmetic is done by hardware, 0 where
// every operation is a library call (define it by hand to override).
// (__ARM_FP is defined only with a hardware FPU in use, unlike __VFP_FP__,
// which names the float format and is defined on a Cortex-M0+ as well)
#ifndef SHAPOGFX_ARCH_FPU
#if defined(__ARM_FP) || defined(__riscv_flen) ||                          \
    defined(SHAPOGFX_ARCH_ESP32S3) || defined(SHAPOGFX_ARCH_ESP32P4) ||    \
    defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) ||    \
    defined(__wasm__)
#define SHAPOGFX_ARCH_FPU 1
#else
#define SHAPOGFX_ARCH_FPU 0
#endif
#endif

// SHAPOGFX_ARCH_WORD_LOADS: 1 where an 8- or 16-bit load from constant data
// costs far more than a 32-bit one. The ESP8266 maps its flash into the
// instruction address space, which only takes 32-bit loads: a narrower one
// raises an exception that the SDK emulates (0.9 us a load at 160 MHz,
// against 0.09 us for a word). The 2D renderer then reads the source pixels
// of a scaled or transformed image (GRAY1 and the 16-bit formats) and the
// bits of a mask as the aligned word around them (src/gfx2d/arch.hpp). Same
// pixels either way. On by default for the ESP8266; define it by hand to
// override.
#ifndef SHAPOGFX_ARCH_WORD_LOADS
#if defined(CONFIG_IDF_TARGET_ESP8266) || defined(ESP8266) || \
    defined(ARDUINO_ARCH_ESP8266)
#define SHAPOGFX_ARCH_WORD_LOADS 1
#else
#define SHAPOGFX_ARCH_WORD_LOADS 0
#endif
#endif

#endif
