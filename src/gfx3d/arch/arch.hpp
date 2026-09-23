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
// Anything else is SHAPOGFX_ARCH_GENERIC.

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

// RP2040 / RP2350: fetch 16-bit texels through the SIO interpolator (interp0
// of the core that calls render()). On by default where the Pico SDK's
// hardware_interp is available; define it as 0 to use the portable walker.
#ifndef SHAPOGFX3D_RP2_INTERP
#if defined(SHAPOGFX_ARCH_RP2) && __has_include("hardware/interp.h")
#define SHAPOGFX3D_RP2_INTERP 1
#else
#define SHAPOGFX3D_RP2_INTERP 0
#endif
#endif

#include "generic.hpp"
#if defined(SHAPOGFX_ARCH_RP2)
#include "rp2.hpp"
#endif

// The hooks gfx3d.cpp calls
namespace shapoco::gfx3d::arch {
using generic::mulShift;
#if defined(SHAPOGFX_ARCH_RP2) && SHAPOGFX3D_RP2_INTERP
using rp2::RenderState;
#else
using generic::RenderState;
#endif
}  // namespace shapoco::gfx3d::arch

#endif
