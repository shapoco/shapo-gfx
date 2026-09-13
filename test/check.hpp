#ifndef SHAPOGFX_TEST_CHECK_HPP
#define SHAPOGFX_TEST_CHECK_HPP

// Minimal self-checking test support (no external framework).

#include <cstdio>

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
