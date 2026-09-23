// Graphics2D: clipping, fills, images, bitmaps, text

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "check.hpp"
#include "shapoco/gfx2d/fonts.hpp"
#include "shapoco/gfx2d/gfx2d.hpp"
#include "shapoco/gfx2d/surface_alloc.hpp"

using namespace shapoco::gfx2d;

static int countColor(Graphics2D &g, Color c) {
  int n = 0;
  for (int y = 0; y < g.bounds().height; y++) {
    for (int x = 0; x < g.bounds().width; x++) {
      if (g.getPixel(x, y) == c) n++;
    }
  }
  return n;
}

static void testFillAndClip(PixelFormat fmt) {
  OwnedSurface s = createSurface(fmt, 40, 30);
  Graphics2D g(s);
  g.clear(Colors::BLACK);
  CHECK_EQ(countColor(g, Colors::BLACK), 40 * 30);

  // Partially outside rectangle is clipped
  g.fillRect(-5, -5, 10, 10, Colors::WHITE);
  CHECK_EQ(countColor(g, Colors::WHITE), 25);
  // Normalized negative size
  g.fillRect(39, 29, -3, -3, Colors::WHITE);
  CHECK_EQ(countColor(g, Colors::WHITE), 25 + 9);
  // Clip rect restricts drawing
  g.setClipRect(10, 10, 5, 5);
  g.fillRect(0, 0, 40, 30, Colors::WHITE);
  g.resetClipRect();
  CHECK_EQ(countColor(g, Colors::WHITE), 25 + 9 + 25);
  // Alpha 0 draws nothing, alpha 255 overwrites
  g.fillRect(0, 0, 40, 30, Colors::TRANSPARENT);
  CHECK_EQ(countColor(g, Colors::WHITE), 25 + 9 + 25);
  // Polygon equal to a rectangle fills the same pixels
  g.clear(Colors::BLACK);
  vec2i quad[4] = {{5, 5}, {20, 5}, {20, 15}, {5, 15}};
  g.fillPolygon(quad, 4, Colors::WHITE);
  CHECK_EQ(countColor(g, Colors::WHITE), 15 * 10);
  // Line end points are drawn
  g.clear(Colors::BLACK);
  g.drawLine(2, 3, 30, 20, Colors::WHITE);
  CHECK_EQ(g.getPixel(2, 3), Colors::WHITE);
  CHECK_EQ(g.getPixel(30, 20), Colors::WHITE);
  g.drawLine(10, 25, 10, 2, Colors::WHITE);  // vertical, reversed
  CHECK_EQ(g.getPixel(10, 25), Colors::WHITE);
  CHECK_EQ(g.getPixel(10, 2), Colors::WHITE);
  // Ellipse fill stays inside its rectangle and covers the center
  g.clear(Colors::BLACK);
  g.fillEllipse(4, 4, 20, 12, Colors::WHITE);
  CHECK_EQ(g.getPixel(14, 10), Colors::WHITE);
  CHECK_EQ(g.getPixel(4, 4), Colors::BLACK);
  CHECK_EQ(g.getPixel(3, 10), Colors::BLACK);
  CHECK_EQ(g.getPixel(24, 10), Colors::BLACK);
  CHECK_EQ(g.getPixel(4, 10), Colors::WHITE);
}

static void testBlending() {
  OwnedSurface s = createSurface(PixelFormat::RGB565BE, 8, 8);
  Graphics2D g(s);
  g.clear(Colors::BLACK);
  g.fillRect(0, 0, 8, 8, makeColor(255, 255, 255, 128));
  Color c = g.getPixel(3, 3);
  CHECK(colorR(c) >= 120 && colorR(c) <= 136);
  CHECK(colorG(c) >= 120 && colorG(c) <= 136);
  // Additive image: white sprite added twice saturates
  uint16_t px[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};  // ARGB4444 opaque white
  Texture t = makeTexture(PixelFormat::ARGB4444, 2, 2, px);
  g.clear(makeColor(128, 128, 128));
  g.drawImage(t, 0, 0, BlendMode::ADD);
  CHECK_EQ(g.getPixel(0, 0), Colors::WHITE);
  CHECK(g.getPixel(2, 2) != Colors::WHITE);
  // Transparent texels leave the target untouched
  uint16_t px2[4] = {0x0FFF, 0xFFFF, 0x0FFF, 0xFFFF};  // alpha 0 / 15
  Texture t2 = makeTexture(PixelFormat::ARGB4444, 2, 2, px2);
  g.clear(Colors::BLUE);
  g.drawImage(t2, 0, 0, BlendMode::ALPHA);
  CHECK_EQ(g.getPixel(0, 0), Colors::BLUE);
  CHECK_EQ(g.getPixel(1, 0), Colors::WHITE);
  // Copy ignores alpha
  g.drawImage(t2, 4, 4, BlendMode::NONE);
  CHECK_EQ(g.getPixel(4, 4), Colors::WHITE);
}

// Drawing the same content into RGB444 and RGB565BE targets gives matching
// pixels within the 4-bit quantization
static void testFormatConsistency() {
  OwnedSurface a = createSurface(PixelFormat::RGB565BE, 33, 17);
  OwnedSurface b = createSurface(PixelFormat::RGB444, 33, 17);
  Graphics2D ga(a), gb(b);
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(makeColor(20, 40, 60));
    g->fillEllipse(1, 1, 30, 14, makeColor(200, 100, 50));
    g->fillRoundRect(3, 2, 20, 10, 4, makeColor(50, 200, 100, 128));
    g->drawRoundRect(3, 2, 20, 10, 4, Colors::WHITE);
    g->setFont(&ShapoSansP_s08c07);
    g->setTextColor(Colors::YELLOW);
    g->drawString(5, 4, "Ab");
    g->drawLine(0, 16, 32, 0, Colors::CYAN);
  }
  for (int y = 0; y < 17; y++) {
    for (int x = 0; x < 33; x++) {
      Color ca = ga.getPixel(x, y), cb = gb.getPixel(x, y);
      CHECK(std::abs(colorR(ca) - colorR(cb)) <= 17);
      CHECK(std::abs(colorG(ca) - colorG(cb)) <= 17);
      CHECK(std::abs(colorB(ca) - colorB(cb)) <= 17);
    }
  }
#if SHAPOGFX_FORMAT_RGB565
  // Native RGB565 holds the same pixels as RGB565BE, byte-swapped, whether
  // drawn directly or blitted in either direction
  OwnedSurface n = createSurface(PixelFormat::RGB565, 33, 17);
  Graphics2D gn(n);
  gn.clear(makeColor(20, 40, 60));
  gn.fillEllipse(1, 1, 30, 14, makeColor(200, 100, 50));
  gn.fillRoundRect(3, 2, 20, 10, 4, makeColor(50, 200, 100, 128));
  gn.drawRoundRect(3, 2, 20, 10, 4, Colors::WHITE);
  gn.setFont(&ShapoSansP_s08c07);
  gn.setTextColor(Colors::YELLOW);
  gn.drawString(5, 4, "Ab");
  gn.drawLine(0, 16, 32, 0, Colors::CYAN);
  auto sameAsBE = [&](const OwnedSurface &nat) {
    int bad = 0;
    for (int y = 0; y < 17; y++) {
      const uint16_t *pn = (const uint16_t *)nat.surface().linePtr(y);
      const uint16_t *pb = (const uint16_t *)a.surface().linePtr(y);
      for (int x = 0; x < 33; x++) bad += (pn[x] != bswap16(pb[x]));
    }
    return bad;
  };
  CHECK_EQ(sameAsBE(n), 0);
  OwnedSurface n2 = createSurface(PixelFormat::RGB565, 33, 17);
  Graphics2D(n2).drawImage(a, 0, 0, BlendMode::NONE);
  CHECK_EQ(sameAsBE(n2), 0);
  OwnedSurface b2 = createSurface(PixelFormat::RGB565BE, 33, 17);
  Graphics2D(b2).drawImage(n, 0, 0, BlendMode::NONE);
  CHECK(std::memcmp(b2.pixels(), a.pixels(), a.bytes()) == 0);
#endif

  // Blit RGB444 -> RGB565BE and compare with drawing directly
  OwnedSurface c = createSurface(PixelFormat::RGB565BE, 33, 17);
  Graphics2D gc(c);
  gc.drawImage(b, 0, 0, BlendMode::NONE);
  for (int y = 0; y < 17; y++) {
    for (int x = 0; x < 33; x++) {
      Color cb = gb.getPixel(x, y), cc = gc.getPixel(x, y);
      CHECK(std::abs(colorR(cb) - colorR(cc)) <= 8);
    }
  }
}

static void testBitmapAndText() {
  OwnedSurface s = createSurface(PixelFormat::RGB565BE, 64, 32);
  Graphics2D g(s);
  g.clear(Colors::BLACK);
  // 8x2 bitmap: 10101010 / 11110000
  uint8_t bits[2] = {0xAA, 0xF0};
  Texture bmp = makeTexture(PixelFormat::GRAY1, 8, 2, bits);
  g.drawBitmap(bmp, 0, 0, Colors::WHITE);
  CHECK_EQ(g.getPixel(0, 0), Colors::WHITE);
  CHECK_EQ(g.getPixel(1, 0), Colors::BLACK);
  CHECK_EQ(g.getPixel(3, 1), Colors::WHITE);
  CHECK_EQ(g.getPixel(4, 1), Colors::BLACK);
  g.drawBitmap(bmp, 10, 0, Colors::WHITE, Colors::RED);
  CHECK_EQ(g.getPixel(11, 0), Colors::RED);

  // Text: measurement matches the advance sum, glyphs land inside the line box
  g.setFont(&ShapoSansMono_s08c07);
  int w = g.measureText("Hello");
  CHECK_EQ(w, 5 * g.charAdvance('H'));
  CHECK(g.textHeight() > 0 && g.lineAdvance() > 0);
  g.clear(Colors::BLACK);
  g.setTextColor(Colors::WHITE);
  g.drawString(2, 2, "H");
  int lit = countColor(g, Colors::WHITE);
  CHECK(lit > 0);
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 64; x++) {
      if (g.getPixel(x, y) == Colors::WHITE) {
        CHECK(x >= 2 && x < 2 + g.charAdvance('H') && y >= 2 &&
              y < 2 + g.textHeight());
      }
    }
  }
  // Scaled text covers 4x the pixels
  g.clear(Colors::BLACK);
  g.setFont(&ShapoSansMono_s08c07, 2);
  g.drawString(2, 2, "H");
  CHECK_EQ(countColor(g, Colors::WHITE), lit * 4);
  // Newline advances the cursor
  g.setFont(&ShapoSansP_s08c07);
  g.setCursor(0, 0);
  g.drawString("a\nb");
  CHECK_EQ(g.cursor().y, g.lineAdvance());
  CHECK_EQ(g.cursor().x, g.charAdvance('b'));
}

// Lines and polygons with vertices far outside the target: the walkers work
// in 32 bits within a range around the clip rectangle, so such a line is
// split and such a polygon vertex clamped, but what is on screen stays put.
static void testFarGeometry() {
  OwnedSurface s = createSurface(PixelFormat::RGB565BE, 40, 30);
  Graphics2D g(s);
  g.clear(Colors::BLACK);
  g.drawLine(-1000000, 10, 1000000, 10, Colors::WHITE);
  CHECK_EQ(countColor(g, Colors::WHITE), 40);
  for (int x = 0; x < 40; x++) CHECK_EQ(g.getPixel(x, 10), Colors::WHITE);

  // A diagonal through (20, 15) whose ends are 100000 pixels away covers
  // the same pixels as the part of it on screen, give or take the rounding
  // of the split points
  g.clear(Colors::BLACK);
  g.drawLine(20 - 100001, 15 - 100001, 20 + 99999, 15 + 99999, Colors::WHITE);
  int lit = 0;
  for (int x = 0; x < 40; x++) {
    for (int y = 0; y < 30; y++) {
      if (g.getPixel(x, y) != Colors::WHITE) continue;
      lit++;
      CHECK(std::abs(y - (x - 5)) <= 1);
    }
  }
  CHECK(lit >= 29 && lit <= 31);
  CHECK_EQ(g.getPixel(20, 15), Colors::WHITE);

  // A triangle with one vertex a million pixels below: every row is crossed
  g.clear(Colors::BLACK);
  const vec2i tri[3] = {{0, 0}, {39, 0}, {20, 1000000}};
  g.fillPolygon(tri, 3, Colors::WHITE);
  for (int y = 0; y < 30; y++) {
    CHECK_EQ(g.getPixel(19, y), Colors::WHITE);
    CHECK_EQ(g.getPixel(0, y), y == 0 ? Colors::WHITE : Colors::BLACK);
  }
}

// A surface beyond SHAPOGFX_COORD_MAX is no target
static void testCoordLimit() {
  static uint16_t px[4];
#if SHAPOGFX_COORD_MAX < 32767
  const Surface big = {PixelFormat::RGB565BE, (int16_t)(SHAPOGFX_COORD_MAX + 1),
                       1, 2, px};
  CHECK(!Graphics2D(big).hasTarget());
#endif
  const Surface ok = {PixelFormat::RGB565BE, (int16_t)SHAPOGFX_COORD_MAX, 1, 2,
                      px};
  CHECK(Graphics2D(ok).hasTarget());
}

static void testOwnedSurface() {
  OwnedSurface a = createSurface(PixelFormat::RGB444, 5, 3);
  CHECK(a.valid());
  CHECK_EQ(a.stride(), 8u);
  CHECK_EQ(a.bytes(), 24u);
  OwnedSurface b = std::move(a);
  CHECK(!a.valid());
  CHECK(b.valid());
  Graphics2D g(b);
  g.clear(Colors::WHITE);
  CHECK_EQ(g.getPixel(4, 2), Colors::WHITE);
}

void testGraphics2D() {
#if SHAPOGFX_FORMAT_RGB565BE
  testFillAndClip(PixelFormat::RGB565BE);
#endif
#if SHAPOGFX_FORMAT_RGB565
  testFillAndClip(PixelFormat::RGB565);
#endif
#if SHAPOGFX_FORMAT_RGB444
  testFillAndClip(PixelFormat::RGB444);
#endif
#if SHAPOGFX_FORMAT_ARGB4444
  testFillAndClip(PixelFormat::ARGB4444);
#endif
#if SHAPOGFX_FORMAT_GRAY1
  testFillAndClip(PixelFormat::GRAY1);
#endif
  testBlending();
  testFormatConsistency();
  testBitmapAndText();
  testFarGeometry();
  testCoordLimit();
  testOwnedSurface();
}
