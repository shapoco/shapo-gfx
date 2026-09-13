#ifndef SHAPOGFX2D_CONFIG_HPP
#define SHAPOGFX2D_CONFIG_HPP

// Compile-time configuration shared by gfx2d and gfx3d.
//
// Each pixel format can be disabled (define the macro as 0 before including the
// headers, or on the compiler command line) to remove its code paths from both
// the 2D and the 3D renderer.

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

#endif
