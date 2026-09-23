#ifndef SHAPOGFX3D_ARCH_ARCH_HPP
#define SHAPOGFX3D_ARCH_ARCH_HPP

// Architecture layer of the 3D renderer (internal; included by gfx3d.cpp
// only). The renderer itself is portable C++; this header detects the target
// and selects the implementation of a few hooks. The generic implementation
// (generic.hpp) always exists and is the reference: an architecture-specific
// one computes the same result faster.
//
// Detection, or define one of these to 1 by hand:
//   SHAPOGFX_ARCH_RP2      RP2040 / RP2350 (Pico SDK)
//   SHAPOGFX_ARCH_ESP32S3  ESP32-S3 (ESP-IDF)
//   SHAPOGFX_ARCH_ESP32P4  ESP32-P4 (ESP-IDF)
// Anything else is SHAPOGFX_ARCH_GENERIC -- including the ESP8266, which the
// ESP8266_RTOS_SDK builds with ESP_PLATFORM defined as well (its sdkconfig
// names CONFIG_IDF_TARGET_ESP8266, which selects nothing here).
//
// SHAPOGFX_ARCH_SPLIT_MUL64: 1 forms 32 x 32 -> 64-bit products from four
// 16 x 16-bit ones inline (split_mul.hpp), for a core whose multiplier
// yields only the low 32 bits of a product and would call a library routine
// instead. Defaults to 1 on ARMv6-M (Cortex-M0/M0+, RP2040) and on the
// ESP8266 (Xtensa lx106), else 0; define it by hand for another such core.
// The results are the same either way.

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

#ifndef SHAPOGFX_ARCH_SPLIT_MUL64
#if defined(__ARM_ARCH_6M__) || defined(CONFIG_IDF_TARGET_ESP8266) || \
    defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
#define SHAPOGFX_ARCH_SPLIT_MUL64 1
#else
#define SHAPOGFX_ARCH_SPLIT_MUL64 0
#endif
#endif

// RP2040 / RP2350: fetch 16-bit texels through the SIO interpolator interp0
// and step Gouraud colors through interp1 (of the core that calls render()).
// On by default where the Pico SDK's hardware_interp is available; define it
// as 0 to use the portable code.
#ifndef SHAPOGFX3D_RP2_INTERP
#if defined(SHAPOGFX_ARCH_RP2) && __has_include("hardware/interp.h")
#define SHAPOGFX3D_RP2_INTERP 1
#else
#define SHAPOGFX3D_RP2_INTERP 0
#endif
#endif

#include "generic.hpp"
#if SHAPOGFX_ARCH_SPLIT_MUL64
#include "split_mul.hpp"
#endif
#if defined(SHAPOGFX_ARCH_RP2)
#include "rp2.hpp"
#endif

// The hooks gfx3d.cpp calls
namespace shapoco::gfx3d::arch {
#if SHAPOGFX_ARCH_SPLIT_MUL64
using split::mul64;
using split::mulShiftU16;
#else
using generic::mul64;
using generic::mulShiftU16;
#endif
#if defined(SHAPOGFX_ARCH_RP2) && SHAPOGFX3D_RP2_INTERP
using rp2::GouraudRG;
using rp2::RenderState;
#else
using generic::GouraudRG;
using generic::RenderState;
#endif
}  // namespace shapoco::gfx3d::arch

#endif
