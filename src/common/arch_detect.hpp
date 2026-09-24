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
// names CONFIG_IDF_TARGET_ESP8266, which selects nothing here).

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

#endif
