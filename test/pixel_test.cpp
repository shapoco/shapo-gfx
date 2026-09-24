// Pixel format helpers: conversions, cursors and blending

#include <cstdint>
#include <cstring>

#include "check.hpp"
#include "shapoco/gfx2d/gfx2d.hpp"

using namespace shapoco::gfx2d;

static int absDiff(int a, int b) { return a > b ? a - b : b - a; }

// Color -> native -> Color must stay within the quantization step of the format
static void testRoundTrips() {
  const Color samples[] = {0xFF000000, 0xFFFFFFFF, 0xFF102030,
                           0xFF80FF40, 0xFFF0E0D0, 0x80123456};
  for (Color c : samples) {
    Color r565 = rgb565ToColor(colorToRgb565(c));
    CHECK(absDiff(colorR(r565), colorR(c)) <= 8);
    CHECK(absDiff(colorG(r565), colorG(c)) <= 4);
    CHECK(absDiff(colorB(r565), colorB(c)) <= 8);
    CHECK_EQ(colorA(r565), 255);

    Color r444 = rgb444ToColor(colorToRgb444(c));
    CHECK(absDiff(colorR(r444), colorR(c)) <= 16);
    CHECK(absDiff(colorB(r444), colorB(c)) <= 16);

    Color r4444 = argb4444ToColor(colorToArgb4444(c));
    CHECK(absDiff(colorA(r4444), colorA(c)) <= 16);
    CHECK(absDiff(colorG(r4444), colorG(c)) <= 16);
  }
  // Extremes are exact
  CHECK_EQ(colorToRgb565(Colors::WHITE), 0xFFFF);
  CHECK_EQ(colorToRgb444(Colors::WHITE), 0x0FFF);
  CHECK_EQ(colorToArgb4444(Colors::WHITE), 0xFFFF);
  CHECK_EQ(colorToArgb4444(Colors::TRANSPARENT), 0x0000);
  CHECK_EQ(rgb565ToColor(0xFFFF), Colors::WHITE);
  CHECK_EQ(rgb444ToColor(0x0FFF), Colors::WHITE);
  CHECK_EQ(colorToGray1(Colors::WHITE), 1u);
  CHECK_EQ(colorToGray1(Colors::BLACK), 0u);
  CHECK_EQ(rgb444ToRgb565(0x0FFF), 0xFFFF);
  CHECK_EQ(rgb565ToRgb444(0xFFFF), 0x0FFF);
  CHECK_EQ(bswap16(0x1234), 0x3412);
  CHECK_EQ(packRgb565(1.0f, 1.0f, 1.0f), 0xFFFF);
  CHECK_EQ(packRgb565Swapped(1.0f, 0.0f, 0.0f), bswap16(0xF800));
}

// Writing a row through a cursor and reading it back reproduces the values, for
// every start position; fill() and write() agree.
template <PixelFormat F>
static void testCursor(int width) {
  using T = FormatTraits<F>;
  uint8_t a[128], b[128];
  std::memset(a, 0xA5, sizeof(a));
  std::memset(b, 0xA5, sizeof(b));
  const uint32_t maxVal = (F == PixelFormat::GRAY1)
                              ? 1u
                              : ((F == PixelFormat::RGB444) ? 0xFFFu : 0xFFFFu);
  for (int start = 0; start < 5; start++) {
    std::memset(a, 0xA5, sizeof(a));
    std::memset(b, 0xA5, sizeof(b));
    typename T::Cursor w;
    w.init(a, start);
    for (int i = 0; i < width; i++) {
      w.write((uint32_t)(i * 2654435761u) & maxVal);
      w.next();
    }
    typename T::Cursor r;
    r.init(a, start);
    for (int i = 0; i < width; i++) {
      CHECK_EQ(r.read(), (uint32_t)(i * 2654435761u) & maxVal);
      r.next();
    }
    // fill vs per-pixel write
    uint32_t v = 0x5A5Au & maxVal;
    typename T::Cursor f;
    f.init(a, start);
    f.fill(width, v);
    typename T::Cursor g;
    g.init(b, start);
    for (int i = 0; i < width; i++) {
      g.write(v);
      g.next();
    }
    CHECK(std::memcmp(a, b, sizeof(a)) == 0);
    // Pixels outside [start, start + width) are untouched
    uint8_t pristine[128];
    std::memset(pristine, 0xA5, sizeof(pristine));
    typename T::Cursor before, ref;
    before.init(a, 0);
    ref.init(pristine, 0);
    for (int i = 0; i < start; i++) {
      CHECK_EQ(before.read(), ref.read());
      before.next();
      ref.next();
    }
    // skip(n) lands where n next() calls do
    for (int n = 0; n < 9; n++) {
      uint8_t c1[128], c2[128];
      std::memcpy(c1, a, sizeof(c1));
      std::memcpy(c2, a, sizeof(c2));
      typename T::Cursor s, t;
      s.init(c1, start);
      t.init(c2, start);
      s.skip(n);
      for (int i = 0; i < n; i++) t.next();
      for (int i = 0; i < 3; i++) {
        CHECK_EQ(s.read(), t.read());
        s.write(maxVal ^ s.read());
        t.write(maxVal ^ t.read());
        s.next();
        t.next();
      }
      CHECK(std::memcmp(c1, c2, sizeof(c1)) == 0);
    }
  }
}

static void testBlend() {
  // alpha 0 keeps dst, alpha 64 gives src
  CHECK_EQ(blendAlphaRgb565(0x1234, 0xABCD, 0), 0x1234);
  CHECK_EQ(blendAlphaRgb565(0x1234, 0xABCD, 64), 0xABCD);
  CHECK_EQ(blendAlphaRgb444(0x123, 0xABC, 0), 0x123);
  CHECK_EQ(blendAlphaRgb444(0x123, 0xABC, 64), 0xABC);
  CHECK_EQ(blendAlphaArgb4444(0x0000, 0x0FFF, 64),
           0xFFFF);  // opaque over transparent
  CHECK_EQ(blendAlphaArgb4444(0xF123, 0x0ABC, 0), 0xF123);
  // 50 % of white over black is mid gray in every channel
  uint16_t half = blendAlphaRgb565(0x0000, 0xFFFF, 32);
  CHECK_EQ(half >> 11, 15);
  CHECK_EQ((half >> 5) & 63, 31);
  CHECK_EQ(half & 31, 15);
  // Saturating add
  CHECK_EQ(addSaturateRgb565(0xF800, 1, 0, 0), 0xF800);
  CHECK_EQ(addSaturateRgb565(0x0000, 1, 2, 3), makeRgb565(1, 2, 3));
  CHECK_EQ(addSaturateRgb444(0xF00, 1, 15, 15), 0xFFF);
  // Byte-order independent helpers
  CHECK_EQ(minStride(PixelFormat::GRAY1, 13), 2u);
  CHECK_EQ(minStride(PixelFormat::RGB444, 3), 5u);
  CHECK_EQ(minStride(PixelFormat::RGB565_SWAPPED, 7), 14u);
  CHECK_EQ(makeColorHsv(0, 255, 255), Colors::RED);
  CHECK_EQ(makeColorHsv(120, 255, 255), Colors::GREEN);
  CHECK_EQ(makeColorHsv(240, 255, 255), Colors::BLUE);
  CHECK_EQ(makeColor(300, -5, 128), 0xFFFF0080u);
}

void testPixel() {
  testRoundTrips();
#if SHAPOGFX_FORMAT_GRAY1
  testCursor<PixelFormat::GRAY1>(37);
#endif
#if SHAPOGFX_FORMAT_RGB444
  testCursor<PixelFormat::RGB444>(37);
#endif
#if SHAPOGFX_FORMAT_ARGB4444
  testCursor<PixelFormat::ARGB4444>(37);
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
  testCursor<PixelFormat::RGB565_SWAPPED>(37);
#endif
#if SHAPOGFX_FORMAT_RGB565
  testCursor<PixelFormat::RGB565>(37);
#endif
  testBlend();
}
