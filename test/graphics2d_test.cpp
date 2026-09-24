// Graphics2D: clipping, fills, images, bitmaps, text

#include <cmath>
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
  OwnedSurface s = createSurface(PixelFormat::RGB565_SWAPPED, 8, 8);
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

// Drawing the same content into RGB444 and RGB565_SWAPPED targets gives
// matching pixels within the 4-bit quantization
static void testFormatConsistency() {
  OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 33, 17);
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
  // Native RGB565 holds the same pixels as RGB565_SWAPPED, byte-swapped,
  // whether drawn directly or blitted in either direction
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
  OwnedSurface b2 = createSurface(PixelFormat::RGB565_SWAPPED, 33, 17);
  Graphics2D(b2).drawImage(n, 0, 0, BlendMode::NONE);
  CHECK(std::memcmp(b2.pixels(), a.pixels(), a.bytes()) == 0);
#endif

  // Blit RGB444 -> RGB565_SWAPPED and compare with drawing directly
  OwnedSurface c = createSurface(PixelFormat::RGB565_SWAPPED, 33, 17);
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
  OwnedSurface s = createSurface(PixelFormat::RGB565_SWAPPED, 64, 32);
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
  OwnedSurface s = createSurface(PixelFormat::RGB565_SWAPPED, 40, 30);
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
  const Surface big = {PixelFormat::RGB565_SWAPPED,
                       (int16_t)(SHAPOGFX_COORD_MAX + 1), 1, 2, px};
  CHECK(!Graphics2D(big).hasTarget());
#endif
  const Surface ok = {PixelFormat::RGB565_SWAPPED, (int16_t)SHAPOGFX_COORD_MAX,
                      1, 2, px};
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

// ---------------------------------------------------------------------------
// Scaled and transformed images, arcs and sectors

static uint32_t nextRand(uint32_t &s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}
static int randInt(uint32_t &s, int lo, int hi) {
  return lo + (int)(nextRand(s) % (uint32_t)(hi - lo + 1));
}

static const PixelFormat kFormats[] = {
#if SHAPOGFX_FORMAT_GRAY1
    PixelFormat::GRAY1,
#endif
#if SHAPOGFX_FORMAT_RGB444
    PixelFormat::RGB444,
#endif
#if SHAPOGFX_FORMAT_ARGB4444
    PixelFormat::ARGB4444,
#endif
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
    PixelFormat::RGB565_SWAPPED,
#endif
#if SHAPOGFX_FORMAT_RGB565
    PixelFormat::RGB565,
#endif
};

static void fillRandom(OwnedSurface &s, uint32_t seed) {
  uint8_t *p = (uint8_t *)s.pixels();
  for (size_t i = 0; i < s.bytes(); i++) p[i] = (uint8_t)nextRand(seed);
}

static bool sameSurface(const OwnedSurface &a, const OwnedSurface &b) {
  return std::memcmp(a.pixels(), b.pixels(), a.bytes()) == 0;
}

static const BlendMode kModes[] = {BlendMode::NONE, BlendMode::ALPHA,
                                   BlendMode::ADD};
static const int kOpacities[] = {255, 255, 160, 40};

// The scaled drawImage() against a per-pixel reference made of 1 x 1 blits:
// target pixel t of dw takes source pixel floor((2t + 1) sw / 2dw), counted
// from the far end when mirrored. Every format pair and mode must match to
// the byte.
static void testScaledImage() {
  uint32_t seed = 12345;
  int failures = 0;
  for (PixelFormat sf : kFormats) {
    for (PixelFormat df : kFormats) {
      for (int n = 0; n < 40; n++) {
        OwnedSurface img = createSurface(sf, n & 1 ? 13 : 16, n & 2 ? 9 : 8);
        fillRandom(img, nextRand(seed));
        OwnedSurface a = createSurface(df, 33, 23),
                     b = createSurface(df, 33, 23);
        fillRandom(a, n);
        std::memcpy(b.pixels(), a.pixels(), a.bytes());
        Graphics2D ga(a), gb(b);
        if (n % 3 == 0) {
          const Rect clip = {randInt(seed, 0, 10), randInt(seed, 0, 8),
                             randInt(seed, 5, 25), randInt(seed, 5, 15)};
          ga.setClipRect(clip);
          gb.setClipRect(clip);
        }
        const Rect src = {randInt(seed, -4, 12), randInt(seed, -3, 8),
                          randInt(seed, 1, 18), randInt(seed, 1, 12)};
        Rect dst = {randInt(seed, -12, 30), randInt(seed, -10, 20),
                    randInt(seed, 1, 44), randInt(seed, 1, 30)};
        if (n % 4 == 1) dst.width = -dst.width;
        if (n % 5 == 2) dst.height = -dst.height;
        if (n % 7 == 3) dst.width = src.width;  // one axis unscaled
        const BlendMode mode = kModes[n % 3];
        const int op = kOpacities[(n / 3) % 4];
        ga.drawImage(img, dst, src, mode, op);

        const Rect d = dst.normalized();
        for (int j = 0; j < d.height; j++) {
          const int k = (2 * j + 1) * src.height / (2 * d.height);
          const int sy = dst.height < 0 ? src.bottom() - 1 - k : src.y + k;
          for (int i = 0; i < d.width; i++) {
            const int q = (2 * i + 1) * src.width / (2 * d.width);
            const int sx = dst.width < 0 ? src.right() - 1 - q : src.x + q;
            gb.drawImage(img, d.x + i, d.y + j, Rect{sx, sy, 1, 1}, mode, op);
          }
        }
        if (!sameSurface(a, b) && failures++ < 5) {
          std::printf("  scaled: src fmt %d dst fmt %d case %d\n", (int)sf,
                      (int)df, n);
          CHECK(false);
        }
      }
    }
  }
  // An exact enlargement repeats every pixel
  OwnedSurface img = createSurface(PixelFormat::RGB565_SWAPPED, 3, 2);
  fillRandom(img, 7);
  OwnedSurface t = createSurface(PixelFormat::RGB565_SWAPPED, 12, 6);
  Graphics2D g(t);
  g.drawImage(img, Rect{0, 0, 12, 6}, BlendMode::NONE);
  Graphics2D gi(img);
  for (int y = 0; y < 6; y++) {
    for (int x = 0; x < 12; x++)
      CHECK_EQ(g.getPixel(x, y), gi.getPixel(x / 4, y / 3));
  }
  // Mirrored: the same pixels backwards
  g.drawImage(img, Rect{12, 6, -12, -6}, BlendMode::NONE);
  for (int y = 0; y < 6; y++) {
    for (int x = 0; x < 12; x++)
      CHECK_EQ(g.getPixel(x, y), gi.getPixel(2 - x / 4, 1 - y / 3));
  }
}

// The transformed drawImage() against the texel under each pixel center,
// computed in double; pixels whose center falls within 1/500 texel of a
// texel edge may go either way and are not compared.
static void checkAffine(const Texture &img, const affine2f &m, const Rect &src,
                        PixelFormat df, BlendMode mode, int op, uint32_t fill,
                        int &failures, const char *what) {
  OwnedSurface a = createSurface(df, 40, 32), b = createSurface(df, 40, 32);
  fillRandom(a, fill);
  std::memcpy(b.pixels(), a.pixels(), a.bytes());
  Graphics2D ga(a), gb(b);
  ga.drawImage(img, m, src, mode, op);
  const double det = (double)m.a * m.d - (double)m.b * m.c;
  const double ia = m.d / det, ib = -m.b / det, ic = -m.c / det, id = m.a / det;
  const Rect in = src.normalized().intersect({0, 0, img.width, img.height});
  bool sure[32][40];
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 40; x++) {
      const double X = x + 0.5 - m.tx, Y = y + 0.5 - m.ty;
      const double u = ia * X + ic * Y + src.x, v = ib * X + id * Y + src.y;
      const double fu = u - std::floor(u), fv = v - std::floor(v);
      sure[y][x] = fu > 0.002 && fu < 0.998 && fv > 0.002 && fv < 0.998;
      const int tu = (int)std::floor(u), tv = (int)std::floor(v);
      if (sure[y][x] && in.contains(tu, tv))
        gb.drawImage(img, x, y, Rect{tu, tv, 1, 1}, mode, op);
    }
  }
  int bad = 0;
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 40; x++)
      bad += sure[y][x] && ga.getPixel(x, y) != gb.getPixel(x, y);
  }
  if (bad && failures++ < 5) {
    std::printf("  affine (%s): %d pixels differ, src fmt %d dst fmt %d\n",
                what, bad, (int)img.format, (int)df);
    CHECK(false);
  }
}

static void testAffineImage() {
  uint32_t seed = 777;
  int failures = 0;
  constexpr float PI = 3.14159265f;
  for (PixelFormat sf : kFormats) {
    OwnedSurface sq = createSurface(sf, 16, 16);  // power-of-two stride
    OwnedSurface odd = createSurface(sf, 13, 9);
    fillRandom(sq, nextRand(seed));
    fillRandom(odd, nextRand(seed));
    for (PixelFormat df : kFormats) {
      for (int n = 0; n < 16; n++) {
        const OwnedSurface &img = n & 1 ? odd : sq;
        const float angle = (float)randInt(seed, 0, 359) * PI / 180.0f;
        const float sx = (float)randInt(seed, 30, 300) / 100.0f *
                         (n % 5 == 0 ? -1.0f : 1.0f);
        const float sy = (float)randInt(seed, 30, 300) / 100.0f;
        affine2f m = affine2f::placement(
            (float)randInt(seed, 0, 40), (float)randInt(seed, 0, 32), angle, sx,
            sy, img.width() * 0.5f, img.height() * 0.5f);
        if (n % 3 == 1) m.shear(0.3f, -0.2f);
        Rect src = {0, 0, img.width(), img.height()};
        if (n % 4 == 2)
          src = {randInt(seed, -3, 6), randInt(seed, -3, 5),
                 randInt(seed, 2, 14), randInt(seed, 2, 12)};
        checkAffine(img.surface(), m, src, df, kModes[n % 3],
                    kOpacities[(n / 3) % 4], n, failures, "random");
      }
      // Not whole pixels, so not the scaled path: enlarged and reduced
      checkAffine(sq.surface(), affine2f::translation(3.25f, 1.5f).scale(2.5f),
                  {0, 0, 16, 16}, df, BlendMode::ALPHA, 255, 1, failures,
                  "scale 2.5");
      checkAffine(sq.surface(), affine2f::translation(1.5f, 2.5f).scale(0.4f),
                  {0, 0, 16, 16}, df, BlendMode::NONE, 255, 2, failures,
                  "scale 0.4");
    }
  }

  // Nothing outside the source rectangle is ever read, even where pixel
  // centers fall exactly on its edges (simple scales and quarter turns with
  // half-pixel offsets): the region is framed with a color that must not
  // show up
  {
    OwnedSurface framed = createSurface(PixelFormat::RGB565_SWAPPED, 12, 10);
    Graphics2D gf(framed);
    gf.clear(Colors::RED);
    gf.fillRect(2, 2, 8, 6, Colors::WHITE);
    const Rect inner = {2, 2, 8, 6};
    const float offs[] = {0.0f, 0.5f, 0.25f, 1.0f / 3};
    const float scales[] = {0.5f, 2.0f, 0.25f, 1.5f, 1.0f / 3, 3.0f};
    int red = 0, white = 0;
    for (float o : offs) {
      for (float sc : scales) {
        for (int q = 0; q < 4; q++) {
          affine2f m = affine2f::translation(16.0f + o, 12.0f + o);
          m.rotate(q * PI / 2).scale(sc, sc * 1.5f).translate(-4, -3);
          OwnedSurface t = createSurface(PixelFormat::RGB565_SWAPPED, 40, 32);
          Graphics2D gt(t);
          gt.drawImage(framed, m, inner, BlendMode::NONE);
          red += countColor(gt, Colors::RED);
          white += countColor(gt, Colors::WHITE);
        }
      }
    }
    CHECK_EQ(red, 0);
    CHECK(white > 1000);
  }

  // Whole-pixel transforms without rotation go to the plain and the scaled
  // drawImage()
  OwnedSurface img = createSurface(PixelFormat::ARGB4444, 7, 5);
  fillRandom(img, 99);
  const Rect src = {1, 1, 5, 3};
  struct {
    affine2f m;
    Rect dst;
  } exact[] = {
      {affine2f::translation(4, 3), {4, 3, 5, 3}},
      {affine2f::translation(4, 3).scale(2, 3), {4, 3, 10, 9}},
      {affine2f::translation(20, 3).scale(-3, 1), {20, 3, -15, 3}},
  };
  for (auto &e : exact) {
    OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 32, 16);
    OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 32, 16);
    Graphics2D(a).drawImage(img, e.m, src);
    Graphics2D(b).drawImage(img, e.dst, src);
    CHECK(sameSurface(a, b));
  }
  // A singular transform draws nothing
  OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 32, 16);
  Graphics2D(a).drawImage(img, affine2f::scaling(0.0f, 2.0f), BlendMode::NONE);
  Graphics2D ga(a);
  CHECK_EQ(countColor(ga, Colors::BLACK), 32 * 16);
}

static void testAffineHelpers() {
  auto near = [](const vec2f &p, float x, float y) {
    return std::fabs(p.x - x) < 1e-4f && std::fabs(p.y - y) < 1e-4f;
  };
  constexpr float PI = 3.14159265f;
  // A positive angle turns +x towards +y (clockwise on screen)
  CHECK(near(affine2f::rotation(PI / 2).apply(1, 0), 0, 1));
  CHECK(near(affine2f::rotation(PI / 2, 10, 10).apply(11, 10), 10, 11));
  // Chained calls apply the last one first
  affine2f m = affine2f::translation(5, 6);
  m.rotate(0.7f).scale(2, 3).translate(-4, -2);
  const affine2f p = affine2f::placement(5, 6, 0.7f, 2, 3, 4, 2);
  const vec2f q = m.apply(3, 9), r = p.apply(3, 9);
  CHECK(near(q, r.x, r.y));
  CHECK(near(p.apply(4, 2), 5, 6));  // the pivot stays put
  affine2f inv;
  CHECK(m.invert(inv));
  CHECK(near((inv * m).apply(7, -3), 7, -3));
  CHECK(near(m * (inv * vec2f{7, -3}), 7, -3));
  CHECK(!affine2f::scaling(0, 1).invert(inv));
  CHECK(near(affine2f::shearing(0.5f, 0).apply(0, 2), 1, 2));
}

// Pixels of a surface that are not black
static int litMask(const OwnedSurface &s, bool *mask) {
  Graphics2D g(s.surface());
  int n = 0;
  for (int y = 0; y < s.height(); y++) {
    for (int x = 0; x < s.width(); x++) {
      const bool lit = g.getPixel(x, y) != Colors::BLACK;
      mask[y * s.width() + x] = lit;
      n += lit;
    }
  }
  return n;
}

static void testArcs() {
  constexpr float PI = 3.14159265f;
  const int W = 48, H = 40;
  static bool whole[W * H], part[W * H], cover[W * H];
  auto drawn = [&](auto fn, bool *mask) {
    OwnedSurface s = createSurface(PixelFormat::RGB565_SWAPPED, W, H);
    Graphics2D g(s);
    g.clear(Colors::BLACK);
    fn(g);
    return litMask(s, mask);
  };
  const Rect rects[] = {{3, 2, 41, 35}, {4, 4, 40, 34},    {10, 12, 30, 9},
                        {20, 3, 1, 30}, {-10, -8, 50, 44}, {5, 5, 3, 3}};
  const float cuts[][4] = {{0.0f, 1.0f, 2.5f, 4.0f},
                           {-0.3f, 0.9f, 3.3f, 3.4f},
                           {PI / 4, PI / 2, PI, 3 * PI / 2},
                           {0.1f, 0.2f, 0.3f, 6.0f}};
  for (const Rect &r : rects) {
    for (int filled = 0; filled < 2; filled++) {
      auto shape = [&](Graphics2D &g, float a0, float a1) {
        if (filled)
          g.fillSector(r, a0, a1, Colors::WHITE);
        else
          g.drawArc(r, a0, a1, Colors::WHITE);
      };
      const int total = drawn(
          [&](Graphics2D &g) {
            if (filled)
              g.fillEllipse(r, Colors::WHITE);
            else
              g.drawEllipse(r, Colors::WHITE);
          },
          whole);
      // A full turn is the whole ellipse
      CHECK_EQ(
          drawn([&](Graphics2D &g) { shape(g, 1.0f, 1.0f + 2 * PI); }, part),
          total);
      CHECK(std::memcmp(part, whole, sizeof(whole)) == 0);
      CHECK_EQ(drawn([&](Graphics2D &g) { shape(g, 1.0f, 1.0f); }, part), 0);
      // Sectors between consecutive cuts cover the ellipse exactly once
      for (const auto &c : cuts) {
        std::memset(cover, 0, sizeof(cover));
        int sum = 0, overlap = 0;
        for (int i = 0; i < 4; i++) {
          const float a0 = c[i], a1 = i == 3 ? c[0] : c[i + 1];
          sum += drawn([&](Graphics2D &g) { shape(g, a0, a1); }, part);
          for (int k = 0; k < W * H; k++) {
            overlap += part[k] && cover[k];
            cover[k] = cover[k] || part[k];
            CHECK(!part[k] || whole[k]);
          }
        }
        CHECK_EQ(overlap, 0);
        CHECK_EQ(sum, total);
      }
    }
  }

  // A quarter of a circle: the pixels right of and below the center (those
  // on the axes go to one of the neighboring quarters)
  const auto circle = [](Graphics2D &g) {
    g.fillCircle(20, 20, 12, Colors::WHITE);
  };
  const int full = drawn(circle, whole);
  drawn(
      [](Graphics2D &g) {
        g.fillCircleSector(20, 20, 12, 0, PI / 2, Colors::WHITE);
      },
      part);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (x > 20 && y > 20) CHECK_EQ(part[y * W + x], whole[y * W + x]);
      if (x < 20 || y < 20) CHECK(!part[y * W + x]);
    }
  }
  // The end angle wraps: (0, -pi/2) is the other three quarters
  const int three = drawn(
      [](Graphics2D &g) {
        g.fillCircleSector(20, 20, 12, 0, -PI / 2, Colors::WHITE);
      },
      part);
  const int one = drawn(
      [](Graphics2D &g) {
        g.fillCircleSector(20, 20, 12, -PI / 2, 0, Colors::WHITE);
      },
      cover);
  CHECK_EQ(three + one, full);
  // Parametric angles: on a flat ellipse, 30 degrees on the circle lies
  // inside [0, 45) and 60 degrees outside, although the geometric angle of
  // that point is below 45 degrees too
  drawn(
      [](Graphics2D &g) {
        g.fillSector(0, 10, 41, 11, 0, PI / 4, Colors::WHITE);
      },
      part);
  auto at = [&](float t) {
    const int x = 20 + (int)std::lrint(16 * std::cos(t));
    const int y = 15 + (int)std::lrint(4 * std::sin(t));
    return part[y * W + x];
  };
  CHECK(at(PI / 6));
  CHECK(!at(PI / 3));
}

void testGraphics2D() {
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
  testFillAndClip(PixelFormat::RGB565_SWAPPED);
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
  testScaledImage();
  testAffineImage();
  testAffineHelpers();
  testArcs();
}
