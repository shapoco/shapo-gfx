#ifndef SHAPOGFX2D_CONFIG_HPP
#define SHAPOGFX2D_CONFIG_HPP

// Compile-time configuration shared by gfx2d and gfx3d.
//
// ShapoGFX is written in C++17. Diagnose a too-old standard here rather than
// letting the headers fail with dozens of unrelated errors (PlatformIO projects
// often default to gnu++11: add
//   build_unflags = -std=gnu++11
//   build_flags = -std=gnu++17
// to platformio.ini).
#if defined(_MSVC_LANG)
#define SHAPOGFX_CPLUSPLUS _MSVC_LANG
#else
#define SHAPOGFX_CPLUSPLUS __cplusplus
#endif
#if SHAPOGFX_CPLUSPLUS < 201703L
#error \
    "ShapoGFX requires C++17 or later (compile with -std=gnu++17 / -std=c++17)."
#endif
//
// Each pixel format can be disabled (define the macro as 0 before including the
// headers, or on the compiler command line) to remove its code paths from both
// the 2D and the 3D renderer. RGB565 (native byte order) is off by default: as
// a 3D output format it costs as much code as RGB565BE; enable it with 1.

#ifndef SHAPOGFX_FORMAT_GRAY1
#define SHAPOGFX_FORMAT_GRAY1 1
#endif
#ifndef SHAPOGFX_FORMAT_RGB444
#define SHAPOGFX_FORMAT_RGB444 1
#endif
#ifndef SHAPOGFX_FORMAT_ARGB4444
#define SHAPOGFX_FORMAT_ARGB4444 1
#endif
#ifndef SHAPOGFX_FORMAT_RGB565BE
#define SHAPOGFX_FORMAT_RGB565BE 1
#endif
#ifndef SHAPOGFX_FORMAT_RGB565
#define SHAPOGFX_FORMAT_RGB565 0
#endif

// Bits of a screen coordinate and of a surface's width and height (1..15).
// Surfaces wider or taller than SHAPOGFX_COORD_MAX pixels are rejected
// (Graphics2D::setTarget() leaves the context without a target,
// Graphics3D::init() fails), which lets the renderers keep coordinates in 16
// bits and every product of two of them in 32. Like the format macros it must
// have the same value in every translation unit.
#ifndef SHAPOGFX_COORD_BITS
#define SHAPOGFX_COORD_BITS 11
#endif
static_assert(SHAPOGFX_COORD_BITS >= 1 && SHAPOGFX_COORD_BITS <= 15,
              "SHAPOGFX_COORD_BITS must be between 1 and 15");
#define SHAPOGFX_COORD_MAX ((1 << SHAPOGFX_COORD_BITS) - 1)

#endif
