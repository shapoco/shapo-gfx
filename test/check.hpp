#ifndef SHAPOGFX_TEST_CHECK_HPP
#define SHAPOGFX_TEST_CHECK_HPP

// Minimal self-checking test support (no external framework).

#include <cstdio>

// Optional features of the 3D renderer (see src/gfx3d/gfx3d.cpp). The build
// system passes the same values to the library and to the tests, so a test can
// skip what the current configuration cannot draw.
#ifndef SHAPOGFX3D_TEXTURE
#define SHAPOGFX3D_TEXTURE 1
#endif
#ifndef SHAPOGFX3D_GOURAUD
#define SHAPOGFX3D_GOURAUD 1
#endif
#ifndef SHAPOGFX3D_BLEND
#define SHAPOGFX3D_BLEND 1
#endif
#ifndef SHAPOGFX3D_LINES
#define SHAPOGFX3D_LINES 1
#endif
#ifndef SHAPOGFX3D_POINTS
#define SHAPOGFX3D_POINTS 1
#endif

extern int g_checkFailures;

#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_checkFailures++;                                            \
    }                                                               \
  } while (0)

#define CHECK_EQ(a, b)                                                 \
  do {                                                                 \
    long long va_ = (long long)(a), vb_ = (long long)(b);              \
    if (va_ != vb_) {                                                  \
      std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, \
                  __LINE__, #a, #b, va_, vb_);                         \
      g_checkFailures++;                                               \
    }                                                                  \
  } while (0)

void testPixel();
void testGraphics2D();
void testGfx3D();
void testTools();

#endif
