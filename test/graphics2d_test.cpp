// Graphics2D: clipping, fills, images, bitmaps, text, the state stack,
// transforms, blending and the color key

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "check.hpp"
#include "shapoco/gfx2d/fonts.hpp"
#include "shapoco/gfx2d/gfx2d.hpp"
#include "shapoco/gfx2d/surface_alloc.hpp"

using namespace shapoco::gfx2d;

static constexpr float PI = 3.14159265f;

// Pixels of the target equal to c (untransformed)
static int countColor(const Graphics2D &g, Color c) {
  int n = 0;
  for (int y = 0; y < g.bounds().height; y++) {
    for (int x = 0; x < g.bounds().width; x++) {
      if (g.getPixel(x, y, false) == c) n++;
    }
  }
  return n;
}

static bool sameSurface(const OwnedSurface &a, const OwnedSurface &b) {
  return std::memcmp(a.pixels(), b.pixels(), a.bytes()) == 0;
}

static uint32_t nextRand(uint32_t &s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}
static int randInt(uint32_t &s, int lo, int hi) {
  return lo + (int)(nextRand(s) % (uint32_t)(hi - lo + 1));
}

static void fillRandom(OwnedSurface &s, uint32_t seed) {
  uint8_t *p = (uint8_t *)s.pixels();
  for (size_t i = 0; i < s.bytes(); i++) p[i] = (uint8_t)nextRand(seed);
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

struct BlendCase {
  BlendMode mode;
  int opacity;
};
static const BlendCase kBlends[] = {
    {BlendMode::NONE, 255},
    {BlendMode::ALPHA, 255},
#if SHAPOGFX2D_BLEND
    {BlendMode::ADD, 255},
    {BlendMode::ALPHA, 160},
    {BlendMode::NONE, 255},
    {BlendMode::ADD, 40},
#endif
};
static constexpr int kBlendCount = sizeof(kBlends) / sizeof(kBlends[0]);

static void setBlendCase(Graphics2D &g, int i) {
  g.setBlend(kBlends[i % kBlendCount].mode, kBlends[i % kBlendCount].opacity);
}

// ---------------------------------------------------------------------------
// Basics

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
  CHECK_EQ(g.getPixel(5, 5), Colors::WHITE);
  CHECK_EQ(g.getPixel(19, 14), Colors::WHITE);
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
  // drawRect: the outline inside the rectangle, thickness included
  g.clear(Colors::BLACK);
  g.drawRect(2, 2, 10, 8, Colors::WHITE, 2);
  CHECK_EQ(countColor(g, Colors::WHITE), 10 * 8 - 6 * 4);
  // A rounded rectangle without a radius is the rectangle
  g.clear(Colors::BLACK);
  g.fillRoundRect(2, 2, 10, 8, 0, Colors::WHITE);
  CHECK_EQ(countColor(g, Colors::WHITE), 80);
  g.clear(Colors::BLACK);
  g.drawRoundRect(2, 2, 10, 8, 0, Colors::WHITE);
  CHECK_EQ(countColor(g, Colors::WHITE), 80 - 8 * 6);
}

static void testBlending() {
  OwnedSurface s = createSurface(PixelFormat::RGB565_SWAPPED, 8, 8);
  Graphics2D g(s);
  g.clear(Colors::BLACK);
  g.fillRect(0, 0, 8, 8, makeColor(255, 255, 255, 128));
  Color c = g.getPixel(3, 3);
  CHECK(colorR(c) >= 120 && colorR(c) <= 136);
  CHECK(colorG(c) >= 120 && colorG(c) <= 136);
  // Transparent texels leave the target untouched
  uint16_t px2[4] = {0x0FFF, 0xFFFF, 0x0FFF, 0xFFFF};  // alpha 0 / 15
  Texture t2 = makeTexture(PixelFormat::ARGB4444, 2, 2, px2);
  g.clear(Colors::BLUE);
  g.drawImage(t2, 0, 0);
  CHECK_EQ(g.getPixel(0, 0), Colors::BLUE);
  CHECK_EQ(g.getPixel(1, 0), Colors::WHITE);
  // clear() overwrites whatever the blend
  g.setBlend(BlendMode::ALPHA, 10);
  g.clear(Colors::RED);
  CHECK_EQ(g.getPixel(5, 5), Colors::RED);
  g.setBlend(BlendMode::ALPHA);

#if SHAPOGFX2D_BLEND
  // Copy ignores alpha
  g.setBlend(BlendMode::NONE);
  g.drawImage(t2, 4, 4);
  CHECK_EQ(g.getPixel(4, 4), Colors::WHITE);
  // Additive image: white sprite added twice saturates
  uint16_t px[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};  // opaque white
  Texture t = makeTexture(PixelFormat::ARGB4444, 2, 2, px);
  g.clear(makeColor(128, 128, 128));
  g.setBlend(BlendMode::ADD);
  g.drawImage(t, 0, 0);
  CHECK_EQ(g.getPixel(0, 0), Colors::WHITE);
  CHECK(g.getPixel(2, 2) != Colors::WHITE);
  // Additive shapes and text
  g.clear(Colors::BLACK);
  g.fillRect(0, 0, 4, 4, makeColor(100, 100, 100));
  g.fillRect(0, 0, 2, 2, makeColor(100, 100, 100));
  CHECK(colorR(g.getPixel(1, 1)) > colorR(g.getPixel(3, 3)) + 80);
  g.setFont(&ShapoSansMono_s08c07);
  g.setTextColor(makeColor(0, 0, 200));
  g.clear(makeColor(0, 0, 100));
  g.drawString(0, 0, "#");
  CHECK(countColor(g, makeColor(0, 0, 255)) > 0);  // 100 + 200 saturates

  // The opacity multiplies the color's alpha
  OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 8, 8);
  OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 8, 8);
  Graphics2D ga(a), gb(b);
  ga.setBlend(BlendMode::ALPHA, 128);
  ga.fillEllipse(0, 0, 8, 8, Colors::WHITE);
  gb.fillEllipse(0, 0, 8, 8, makeColor(255, 255, 255, 128));
  CHECK(sameSurface(a, b));
  ga.setBlend(BlendMode::ADD, 0);  // opacity 0 draws nothing
  ga.fillRect(0, 0, 8, 8, Colors::WHITE);
  CHECK(sameSurface(a, b));

  // NONE writes the color's alpha into an ARGB4444 target
#if SHAPOGFX_FORMAT_ARGB4444
  OwnedSurface argb = createSurface(PixelFormat::ARGB4444, 4, 4);
  Graphics2D gr(argb);
  gr.setBlend(BlendMode::NONE);
  gr.fillRect(0, 0, 2, 2, makeColor(255, 0, 0, 0x88));
  CHECK_EQ(((uint16_t *)argb.pixels())[0], 0x8F00);
  gr.clear(Colors::TRANSPARENT);
  CHECK_EQ(((uint16_t *)argb.pixels())[0], 0);
#endif
#endif
}

// Drawing the same content into RGB444 and RGB565_SWAPPED targets gives
// matching pixels within the 4-bit quantization
static void testFormatConsistency() {
  OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 33, 17);
  OwnedSurface b = createSurface(PixelFormat::RGB444, 33, 17);
  Graphics2D ga(a), gb(b);
  auto draw = [](Graphics2D &g) {
    g.clear(makeColor(20, 40, 60));
    g.fillEllipse(1, 1, 30, 14, makeColor(200, 100, 50));
    g.fillRoundRect(3, 2, 20, 10, 4, makeColor(50, 200, 100, 128));
    g.drawRoundRect(3, 2, 20, 10, 4, Colors::WHITE);
    g.setFont(&ShapoSansP_s08c07);
    g.setTextColor(Colors::YELLOW);
    g.drawString(5, 4, "Ab");
    g.drawLine(0, 16, 32, 0, Colors::CYAN);
  };
  draw(ga);
  draw(gb);
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
  draw(gn);
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
  Graphics2D(n2).drawImage(a, 0, 0);
  CHECK_EQ(sameAsBE(n2), 0);
  OwnedSurface b2 = createSurface(PixelFormat::RGB565_SWAPPED, 33, 17);
  Graphics2D(b2).drawImage(n, 0, 0);
  CHECK(std::memcmp(b2.pixels(), a.pixels(), a.bytes()) == 0);
#endif

  // Blit RGB444 -> RGB565_SWAPPED and compare with drawing directly
  OwnedSurface c = createSurface(PixelFormat::RGB565_SWAPPED, 33, 17);
  Graphics2D gc(c);
  gc.drawImage(b, 0, 0);
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
  // A translucent foreground goes through spans
  g.clear(Colors::BLACK);
  g.drawBitmap(bmp, 0, 0, makeColor(255, 255, 255, 128));
  CHECK(colorR(g.getPixel(0, 0)) > 100 && colorR(g.getPixel(0, 0)) < 150);
  CHECK_EQ(g.getPixel(1, 0), Colors::BLACK);

  // Text: measurement matches the advance sum, glyphs land inside the line
  // box
  g.setFont(&ShapoSansMono_s08c07);
  const TextMetrics h = g.charMetrics('H');
  const TextMetrics m = g.textMetrics("Hello");
  CHECK(h.width > 0 && h.height > 0 && h.lineAdvance > 0);
  CHECK_EQ(m.width, 5 * h.width);
  CHECK_EQ(m.height, h.height);
  g.clear(Colors::BLACK);
  g.setTextColor(Colors::WHITE);
  g.drawString(2, 2, "H");
  const int lit = countColor(g, Colors::WHITE);
  CHECK(lit > 0);
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 64; x++) {
      if (g.getPixel(x, y) == Colors::WHITE) {
        CHECK(x >= 2 && x < 2 + (int)h.width && y >= 2 &&
              y < 2 + (int)h.height);
      }
    }
  }
  // Newline advances the cursor
  g.setFont(&ShapoSansP_s08c07);
  g.setCursor(0, 0);
  g.drawString("a\nb");
  CHECK_EQ(g.cursor().y, (int)g.textMetrics("").lineAdvance);
  CHECK_EQ(g.cursor().x, (int)g.charMetrics('b').width);
  // A background box of xAdvance x lineHeight per glyph
  g.clear(Colors::BLACK);
  g.setTextColor(Colors::WHITE, Colors::BLUE);
  g.drawString(1, 1, "ab");
  const TextMetrics ab = g.textMetrics("ab");
  CHECK_EQ(countColor(g, Colors::BLUE) + countColor(g, Colors::WHITE),
           (int)(ab.width * ab.height));
}

static void testMetrics() {
  OwnedSurface s = createSurface(PixelFormat::RGB565_SWAPPED, 8, 8);
  Graphics2D g(s);
  CHECK_EQ(g.textMetrics("abc").width, 0);
  CHECK_EQ(g.charMetrics('a').height, 0);
  const GFXfont &f = ShapoSansP_s12c09a01w02;
  g.setFont(&f);
  const GFXglyph &ga = f.glyph['a' - f.first];
  const TextMetrics a = g.charMetrics('a');
  CHECK_EQ(a.width, ga.xAdvance);
  CHECK_EQ(a.lineAdvance, f.yAdvance);
  CHECK_EQ(a.ascent, g.textState().ascent);
  CHECK_EQ(a.height, g.textState().lineHeight);
  CHECK_EQ(g.charMetrics(1).width, 0);  // not in the font
  const TextMetrics t = g.textMetrics("ab\nabc\n");
  CHECK_EQ(t.width, g.textMetrics("abc").width);
  CHECK_EQ(t.height, a.height + 2 * a.lineAdvance);
  CHECK_EQ(t.deviceWidth, t.width);
  CHECK_EQ(g.textMetrics(nullptr).height, a.height);
#if SHAPOGFX2D_TRANSFORM
  // In drawing coordinates; the device size follows the transform
  g.rotate(0.7f);
  g.scale(3, 2);
  const TextMetrics ts = g.textMetrics("abc");
  CHECK_EQ(ts.width, g.textMetrics("abc").width);
  CHECK(std::fabs(ts.deviceWidth - ts.width * 3) < 1e-3f);
  CHECK(std::fabs(ts.deviceHeight - ts.height * 2) < 1e-3f);
#endif
  // The deprecated integer versions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  CHECK_EQ(g.measureText("ab\nabc"), (int)g.textMetrics("abc").width);
  CHECK_EQ(g.charAdvance('a'), ga.xAdvance);
  CHECK_EQ(g.textHeight(), g.textState().lineHeight);
  CHECK_EQ(g.lineAdvance(), f.yAdvance);
#pragma GCC diagnostic pop
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

  // A triangle with one vertex a million pixels below: its edges are all
  // but vertical on screen, through the centers of pixels 0 and 39 of row
  // 0, so the rows below cover columns 1 to 38
  g.clear(Colors::BLACK);
  const vec2i tri[3] = {{0, 0}, {39, 0}, {20, 1000000}};
  g.fillPolygon(tri, 3, Colors::WHITE);
  for (int y = 0; y < 30; y++) {
    CHECK_EQ(g.getPixel(0, y), y == 0 ? Colors::WHITE : Colors::BLACK);
    CHECK_EQ(g.getPixel(1, y), Colors::WHITE);
    CHECK_EQ(g.getPixel(38, y), Colors::WHITE);
    CHECK_EQ(g.getPixel(39, y), Colors::BLACK);
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
// State stack

static void testStateStack() {
  OwnedSurface s = createSurface(PixelFormat::RGB565_SWAPPED, 40, 30);
  Graphics2D g(s);
  CHECK(!g.isInitialized());
  CHECK(!g.pushState());  // no arena
  static uint8_t arena[16384];
  CHECK(Graphics2D::arenaBytes(0) >=
        SHAPOGFX2D_STACK_DEPTH * sizeof(GraphicsState2D));
  CHECK(Graphics2D::arenaBytes(0) <= sizeof(arena));
  CHECK(!g.init(arena, 8));  // too small
  CHECK(g.init(arena + 1, sizeof(arena) - 1));  // aligned internally
  CHECK(g.isInitialized());

  g.setClipRect(1, 2, 3, 4);
  g.setBlend(BlendMode::ADD, 100);
  g.setColorKey(Colors::RED);
  g.setFont(&ShapoSansP_s08c07);
  g.setTextColor(Colors::YELLOW);
  g.translate(5, 6);
  CHECK(g.pushState());
  CHECK_EQ(g.stateDepth(), 1);
  g.resetClipRect();
  g.setBlend(BlendMode::ALPHA);
  g.clearColorKey();
  g.setFont(nullptr);
  g.setTextColor(Colors::WHITE);
  g.resetTransform();
  g.setCursor(7, 8);
  g.popState();
  CHECK_EQ(g.stateDepth(), 0);
  const Rect clip = g.clipRect();
  CHECK(clip.x == 1 && clip.y == 2 && clip.width == 3 && clip.height == 4);
  CHECK(g.font() == &ShapoSansP_s08c07);
  CHECK_EQ(g.textState().color, Colors::YELLOW);
  CHECK(g.textState().lineHeight > 0);
  // The cursor is not part of what is restored
  CHECK(g.cursor() == (vec2i{7, 8}));
#if SHAPOGFX2D_BLEND
  CHECK(g.blendMode() == BlendMode::ADD);
  CHECK_EQ(g.opacity(), 100);
#else
  CHECK(g.blendMode() == BlendMode::ALPHA);
  CHECK_EQ(g.opacity(), 255);
#endif
#if SHAPOGFX2D_COLOR_KEY
  CHECK(g.hasColorKey());
  CHECK_EQ(g.colorKey(), Colors::RED);
#else
  CHECK(!g.hasColorKey());
#endif
#if SHAPOGFX2D_TRANSFORM
  CHECK(g.transformKind() == TransformKind::TRANSLATE);
  CHECK_EQ(g.transform().tx, 5);
#else
  CHECK(g.transformKind() == TransformKind::IDENTITY);
#endif

  // The stack has SHAPOGFX2D_STACK_DEPTH levels; popping an empty one does
  // nothing
  for (int i = 0; i < SHAPOGFX2D_STACK_DEPTH; i++) CHECK(g.pushState());
  CHECK(!g.pushState());
  for (int i = 0; i < SHAPOGFX2D_STACK_DEPTH + 2; i++) g.popState();
  CHECK_EQ(g.stateDepth(), 0);

  // A clip rectangle restored onto a smaller target is clipped to it
  g.resetClipRect();
  CHECK(g.pushState());
  OwnedSurface small = createSurface(PixelFormat::RGB565_SWAPPED, 10, 10);
  g.setTarget(small);
  g.popState();
  CHECK(g.clipRect().width == 10 && g.clipRect().height == 10);

  g.deinit();
  CHECK(!g.pushState());
}

// ---------------------------------------------------------------------------
// Scaled and transformed images

// The scaled drawImage() against a per-pixel reference made of 1 x 1 blits:
// target pixel t of dw takes source pixel floor((2t + 1) sw / 2dw), counted
// from the far end when mirrored. Every format pair and blend must match to
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
        setBlendCase(ga, n);
        setBlendCase(gb, n);
        ga.drawImage(img, dst, src);

        const Rect d = dst.normalized();
        for (int j = 0; j < d.height; j++) {
          const int k = (2 * j + 1) * src.height / (2 * d.height);
          const int sy = dst.height < 0 ? src.bottom() - 1 - k : src.y + k;
          for (int i = 0; i < d.width; i++) {
            const int q = (2 * i + 1) * src.width / (2 * d.width);
            const int sx = dst.width < 0 ? src.right() - 1 - q : src.x + q;
            gb.drawImage(img, d.x + i, d.y + j, Rect{sx, sy, 1, 1});
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
  g.drawImage(img, Rect{0, 0, 12, 6});
  Graphics2D gi(img);
  for (int y = 0; y < 6; y++) {
    for (int x = 0; x < 12; x++)
      CHECK_EQ(g.getPixel(x, y), gi.getPixel(x / 4, y / 3));
  }
  // Mirrored: the same pixels backwards
  g.drawImage(img, Rect{12, 6, -12, -6});
  for (int y = 0; y < 6; y++) {
    for (int x = 0; x < 12; x++)
      CHECK_EQ(g.getPixel(x, y), gi.getPixel(2 - x / 4, 1 - y / 3));
  }
  // A source rectangle reaching out of the image keeps its place
  OwnedSurface u = createSurface(PixelFormat::RGB565_SWAPPED, 8, 8);
  OwnedSurface v = createSurface(PixelFormat::RGB565_SWAPPED, 8, 8);
  Graphics2D(u).drawImage(img, 1, 1, Rect{-2, -1, 5, 3});
  Graphics2D(v).drawImage(img, Rect{1, 1, 5, 3}, Rect{-2, -1, 5, 3});
  CHECK(sameSurface(u, v));
  CHECK_EQ(Graphics2D(u).getPixel(3, 2), gi.getPixel(0, 0));
}

#if SHAPOGFX2D_TRANSFORM
// The transformed drawImage() against the texel under each pixel center,
// computed in double; pixels whose center falls within 1/500 texel of a
// texel edge may go either way and are not compared. A color key, if set on
// `ga`'s side, is applied to the reference by hand.
static void checkAffine(const Texture &img, const affine2f &m, const Rect &src,
                        PixelFormat df, int blend, uint32_t fill, int &failures,
                        const char *what, const Color *key = nullptr) {
  OwnedSurface a = createSurface(df, 40, 32), b = createSurface(df, 40, 32);
  fillRandom(a, fill);
  std::memcpy(b.pixels(), a.pixels(), a.bytes());
  Graphics2D ga(a), gb(b);
  const Graphics2D gi(Surface{img.format, img.width, img.height, img.stride,
                              const_cast<void *>(img.pixels)});
  ga.setTransform(m);
  setBlendCase(ga, blend);
  setBlendCase(gb, blend);
  if (key) ga.setColorKey(*key);
  ga.drawImage(img, 0, 0, src);
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
      if (!sure[y][x] || !in.contains(tu, tv)) continue;
      if (key && colorToNative(img.format, gi.getPixel(tu, tv)) ==
                     colorToNative(img.format, *key))
        continue;
      gb.drawImage(img, x, y, Rect{tu, tv, 1, 1});
    }
  }
  int bad = 0;
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 40; x++)
      bad += sure[y][x] && ga.getPixel(x, y, false) != gb.getPixel(x, y);
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
  for (PixelFormat sf : kFormats) {
    OwnedSurface sq = createSurface(sf, 16, 16);  // power-of-two stride
    OwnedSurface odd = createSurface(sf, 13, 9);
    fillRandom(sq, nextRand(seed));
    fillRandom(odd, nextRand(seed));
    for (PixelFormat df : kFormats) {
      for (int n = 0; n < 16; n++) {
        const OwnedSurface &img = n & 1 ? odd : sq;
        // Clear of the quarter turns, which take other paths
        const float angle = ((float)randInt(seed, 0, 359) + 0.5f) * PI / 180;
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
        checkAffine(img.surface(), m, src, df, n, n, failures, "random");
      }
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
          gt.setTransform(m);
          gt.drawImage(framed, 0, 0, inner);
          red += countColor(gt, Colors::RED);
          white += countColor(gt, Colors::WHITE);
        }
      }
    }
    CHECK_EQ(red, 0);
    CHECK(white > 1000);
  }

  // Transforms without rotation go to the plain and the scaled paths, with
  // the corners snapped to whole pixels
  OwnedSurface img = createSurface(PixelFormat::ARGB4444, 7, 5);
  fillRandom(img, 99);
  const Rect src = {1, 1, 5, 3};
  struct {
    affine2f m;
    Rect dst;
  } exact[] = {
      {affine2f::translation(4, 3), {4, 3, 5, 3}},
      {affine2f::translation(4.4f, 2.6f), {4, 3, 5, 3}},
      {affine2f::translation(4, 3).scale(2, 3), {4, 3, 10, 9}},
      {affine2f::translation(20, 3).scale(-3, 1), {20, 3, -15, 3}},
      {affine2f::translation(3.25f, 1.5f).scale(2.5f), {3, 1, 13, 8}},
      {affine2f::rotation(PI, 10, 5), {20, 10, -5, -3}},
  };
  for (auto &e : exact) {
    OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 32, 16);
    OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 32, 16);
    Graphics2D ga(a);
    ga.setTransform(e.m);
    ga.drawImage(img, 0, 0, src);
    Graphics2D(b).drawImage(img, e.dst, src);
    CHECK(sameSurface(a, b));
  }
  // A singular transform draws nothing
  OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 32, 16);
  Graphics2D ga(a);
  ga.setTransform(affine2f::scaling(0.0f, 2.0f));
  ga.drawImage(img, 0, 0);
  ga.setTransform(affine2f{1, 2, 2, 4, 0, 0});
  ga.drawImage(img, 0, 0);
  CHECK_EQ(countColor(ga, Colors::BLACK), 32 * 16);
}
#endif  // SHAPOGFX2D_TRANSFORM

static void testAffineHelpers() {
  auto near = [](const vec2f &p, float x, float y) {
    return std::fabs(p.x - x) < 1e-4f && std::fabs(p.y - y) < 1e-4f;
  };
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

// ---------------------------------------------------------------------------
// Color key

#if SHAPOGFX2D_COLOR_KEY
// Plain, scaled and transformed images with a color key against 1 x 1 blits
// of the pixels that are not keyed out, for every format pair and blend
static void testColorKey() {
  uint32_t seed = 4242;
  int failures = 0;
  for (PixelFormat sf : kFormats) {
    OwnedSurface img = createSurface(sf, 11, 7);
    fillRandom(img, nextRand(seed));
    Graphics2D gi(img);
    const Color key = gi.getPixel(0, 0);
    gi.setBlend(BlendMode::NONE);
    for (int i = 0; i < 25; i++)
      gi.setPixel(randInt(seed, 0, 10), randInt(seed, 0, 6), key);
    auto keyed = [&](int x, int y) {
      return colorToNative(sf, gi.getPixel(x, y)) == colorToNative(sf, key);
    };
    for (PixelFormat df : kFormats) {
      for (int n = 0; n < 12; n++) {
        OwnedSurface a = createSurface(df, 36, 24),
                     b = createSurface(df, 36, 24);
        fillRandom(a, n);
        std::memcpy(b.pixels(), a.pixels(), a.bytes());
        Graphics2D ga(a), gb(b);
        setBlendCase(ga, n);
        setBlendCase(gb, n);
        ga.setColorKey(key);
        const Rect src = {1, 0, 9, 7};
        Rect dst = {3, 2, 9, 7};
        if (n % 3 == 1) dst = {2, 1, 25, 16};   // enlarged (runs)
        if (n % 3 == 2) dst = {30, 20, -7, -5};  // reduced, mirrored
        ga.drawImage(img, dst, src);
        const Rect d = dst.normalized();
        for (int j = 0; j < d.height; j++) {
          const int k = (2 * j + 1) * src.height / (2 * d.height);
          const int sy = dst.height < 0 ? src.bottom() - 1 - k : src.y + k;
          for (int i = 0; i < d.width; i++) {
            const int q = (2 * i + 1) * src.width / (2 * d.width);
            const int sx = dst.width < 0 ? src.right() - 1 - q : src.x + q;
            if (!keyed(sx, sy))
              gb.drawImage(img, d.x + i, d.y + j, Rect{sx, sy, 1, 1});
          }
        }
        if (!sameSurface(a, b) && failures++ < 5) {
          std::printf("  color key: src fmt %d dst fmt %d case %d\n", (int)sf,
                      (int)df, n);
          CHECK(false);
        }
#if SHAPOGFX2D_TRANSFORM
        checkAffine(img.surface(),
                    affine2f::placement(18, 12, 0.4f + n, 1.7f, 1.3f, 5, 3),
                    src, df, n, n, failures, "color key", &key);
#endif
      }
    }
  }
  // Without the key everything is drawn again
  OwnedSurface img = createSurface(PixelFormat::RGB565_SWAPPED, 4, 4);
  Graphics2D gi(img);
  gi.clear(Colors::RED);
  OwnedSurface t = createSurface(PixelFormat::RGB565_SWAPPED, 4, 4);
  Graphics2D g(t);
  g.setColorKey(Colors::RED);
  g.drawImage(img, 0, 0);
  CHECK_EQ(countColor(g, Colors::RED), 0);
  g.clearColorKey();
  g.drawImage(img, 0, 0);
  CHECK_EQ(countColor(g, Colors::RED), 16);
}
#endif

// ---------------------------------------------------------------------------
// Transforms of shapes

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

#if SHAPOGFX2D_TRANSFORM
// A bit of everything, at (ox, oy)
static void drawScene(Graphics2D &g, int ox, int oy, const Texture &img) {
  static const uint8_t bits[2] = {0xA5, 0x3C};
  const Texture bmp = makeTexture(PixelFormat::GRAY1, 8, 2, bits);
  g.fillRect(ox + 1, oy + 1, 9, 5, makeColor(200, 40, 40));
  g.drawRect(ox + 12, oy + 1, 9, 7, makeColor(40, 200, 40, 180), 2);
  g.fillRoundRect(ox + 23, oy + 1, 14, 9, 3, makeColor(40, 40, 200));
  g.drawRoundRect(ox + 23, oy + 12, 14, 9, 4, Colors::WHITE);
  g.fillEllipse(ox + 1, oy + 12, 11, 8, makeColor(200, 200, 40, 200));
  g.drawEllipse(ox + 12, oy + 12, 9, 12, Colors::CYAN);
  g.fillSector(ox + 1, oy + 22, 13, 13, 0.3f, 2.5f, Colors::MAGENTA);
  g.drawArc(ox + 15, oy + 22, 12, 9, -1.0f, 2.0f, Colors::WHITE);
  g.drawLine(ox + 1, oy + 36, ox + 30, oy + 40, Colors::YELLOW);
  const vec2i star[5] = {{ox + 40, oy + 2},  {ox + 46, oy + 20},
                         {ox + 30, oy + 8},  {ox + 50, oy + 8},
                         {ox + 34, oy + 20}};
  g.fillPolygon(star, 5, makeColor(255, 128, 0, 160));
  g.drawPolygon(star, 5, Colors::WHITE);
  g.drawHLine(ox + 40, oy + 25, 12, Colors::GREEN);
  g.drawVLine(ox + 55, oy + 2, 20, Colors::GREEN);
  g.setPixel(ox + 52, oy + 30, Colors::RED);
  g.drawImage(img, ox + 30, oy + 28);
  g.drawImage(img, Rect{ox + 45, oy + 30, 12, -9}, Rect{1, 1, 5, 4});
  g.drawBitmap(bmp, ox + 2, oy + 43, Colors::WHITE, Colors::BLUE);
  g.setFont(&ShapoSansP_s08c07);
  g.setTextColor(Colors::WHITE, makeColor(0, 0, 100));
  g.drawString(ox + 14, oy + 43, "Ag");
}

static void testTransformedShapes() {
  OwnedSurface img = createSurface(PixelFormat::ARGB4444, 7, 6);
  fillRandom(img, 5);
  // An integer translation, and a fractional one snapped to whole pixels,
  // draw what drawing at the offset draws
  const struct {
    float tx, ty;
    int ox, oy;
  } offs[] = {{7, 5, 7, 5}, {-3, 2, -3, 2}, {7.4f, 4.6f, 7, 5}, {6.5f, 4.5f, 6, 4}};
  for (auto &o : offs) {
    OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 72, 60);
    OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 72, 60);
    Graphics2D ga(a), gb(b);
    ga.translate(o.tx, o.ty);
    CHECK(ga.transformKind() == TransformKind::TRANSLATE);
    drawScene(ga, 0, 0, img);
    drawScene(gb, o.ox, o.oy, img);
    CHECK(sameSurface(a, b));
  }

  // Scaled by whole numbers: rectangles, ellipses and images are those of the
  // scaled rectangles
  {
    OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 64, 48);
    OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 64, 48);
    Graphics2D ga(a), gb(b);
    ga.scale(2, 3);
    CHECK(ga.transformKind() == TransformKind::SCALE);
    ga.fillRect(1, 1, 5, 3, Colors::RED);
    gb.fillRect(2, 3, 10, 9, Colors::RED);
    ga.fillEllipse(8, 1, 10, 4, Colors::GREEN);
    gb.fillEllipse(16, 3, 20, 12, Colors::GREEN);
    ga.drawImage(img, 1, 5);
    gb.drawImage(img, Rect{2, 15, 14, 18});
    ga.drawRect(20, 6, 8, 8, Colors::BLUE, 1);
    gb.fillRect(40, 18, 16, 3, Colors::BLUE);
    gb.fillRect(40, 39, 16, 3, Colors::BLUE);
    gb.fillRect(40, 21, 2, 18, Colors::BLUE);
    gb.fillRect(54, 21, 2, 18, Colors::BLUE);
    CHECK(sameSurface(a, b));
    // Lines stay one pixel wide
    ga.drawLine(0, 0, 10, 0, Colors::WHITE);
    CHECK_EQ(countColor(ga, Colors::WHITE), 21);
  }

  // Mirrored: the target's right edge from the left
  {
    OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 40, 20);
    Graphics2D ga(a);
    ga.translate(40, 0);
    ga.scale(-1, 1);
    ga.fillRect(0, 0, 5, 5, Colors::WHITE);
    CHECK_EQ(countColor(ga, Colors::WHITE), 25);
    CHECK_EQ(ga.getPixel(35, 0, false), Colors::WHITE);
    CHECK_EQ(ga.getPixel(0, 0), Colors::WHITE);  // through the transform
  }

  // A quarter turn: (x, y) -> (20 - y, 10 + x)
  {
    OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 40, 40);
    OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 40, 40);
    Graphics2D ga(a), gb(b);
    ga.translate(20, 10);
    ga.rotate(PI / 2);
    CHECK(ga.transformKind() == TransformKind::AFFINE);
    ga.fillRect(0, 0, 8, 3, Colors::WHITE);
    gb.fillRect(17, 10, 3, 8, Colors::WHITE);
    ga.drawRect(0, 10, 12, 6, Colors::RED, 2);
    gb.drawRect(4, 10, 6, 12, Colors::RED, 2);
    CHECK(sameSurface(a, b));
    // Images: target pixel (X, Y) shows source pixel (Y - 10, 19 - X)
    OwnedSurface c = createSurface(PixelFormat::RGB565_SWAPPED, 7, 6);
    fillRandom(c, 3);
    ga.clear(Colors::BLACK);
    ga.drawImage(c, 0, 0);
    const Graphics2D gc(c);
    int bad = 0;
    for (int y = 10; y < 17; y++) {
      for (int x = 14; x < 20; x++)
        bad += ga.getPixel(x, y, false) != gc.getPixel(y - 10, 19 - x);
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(countColor(ga, Colors::BLACK), 40 * 40 - 42);
    // Pixels and one-pixel lines map pixel to pixel
    ga.clear(Colors::BLACK);
    ga.setPixel(3, 4, Colors::WHITE);
    CHECK_EQ(ga.getPixel(15, 13, false), Colors::WHITE);
    CHECK_EQ(ga.getPixel(3, 4), Colors::WHITE);
    ga.drawHLine(2, 1, 5, Colors::WHITE);
    for (int y = 12; y < 17; y++) CHECK_EQ(ga.getPixel(18, y, false), Colors::WHITE);
    CHECK_EQ(countColor(ga, Colors::WHITE), 6);
    // Text turns with the same pixels
    gb.clear(Colors::BLACK);
    gb.setFont(&ShapoSansP_s08c07);
    gb.drawString(2, 2, "Rg");
    ga.clear(Colors::BLACK);
    ga.setFont(&ShapoSansP_s08c07);
    ga.drawString(2, 2, "Rg");
    CHECK_EQ(countColor(ga, Colors::WHITE), countColor(gb, Colors::WHITE));
    // Ellipses: the general ellipse matches the axis-aligned one, give or
    // take the rounding of a few rows
    static bool ma[40 * 40], mb[40 * 40];
    ga.clear(Colors::BLACK);
    gb.clear(Colors::BLACK);
    ga.fillEllipse(0, 0, 14, 9, Colors::WHITE);
    gb.fillEllipse(11, 10, 9, 14, Colors::WHITE);
    litMask(a, ma);
    litMask(b, mb);
    int differ = 0;
    for (int i = 0; i < 40 * 40; i++) differ += ma[i] != mb[i];
    if (differ > 2) std::printf("  turned ellipse: %d pixels differ\n", differ);
    CHECK(differ <= 2);
  }

  // A turned rectangle and a turned image of its size cover the same pixels,
  // but for centers within rounding of the edges
  {
    OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 48, 48);
    OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 48, 48);
    OwnedSurface c = createSurface(PixelFormat::RGB565_SWAPPED, 20, 13);
    Graphics2D(c).clear(Colors::WHITE);
    static bool ma[48 * 48], mb[48 * 48];
    int differ = 0, total = 0;
    for (int i = 0; i < 12; i++) {
      Graphics2D ga(a), gb(b);
      ga.clear(Colors::BLACK);
      gb.clear(Colors::BLACK);
      const affine2f m =
          affine2f::placement(24.3f, 23.8f, 0.37f * i, 1.3f, 0.9f, 10, 6.5f);
      ga.setTransform(m);
      gb.setTransform(m);
      ga.fillRect(0, 0, 20, 13, Colors::WHITE);
      gb.drawImage(c, 0, 0);
      total += litMask(a, ma);
      litMask(b, mb);
      for (int k = 0; k < 48 * 48; k++) differ += ma[k] != mb[k];
    }
    CHECK(total > 12 * 250);
    CHECK(differ * 200 < total);
  }

  // Sectors under a mirroring, shearing turn still cover the ellipse exactly
  // once
  {
    const int W = 48, H = 40;
    static bool whole[W * H], part[W * H], cover[W * H];
    const affine2f m = affine2f::placement(24, 20, 0.6f, -1.4f, 0.8f, 12, 10)
                           .shear(0.3f, 0.1f);
    for (int filled = 0; filled < 2; filled++) {
      auto drawn = [&](float a0, float a1, bool all, bool *mask) {
        OwnedSurface s = createSurface(PixelFormat::RGB565_SWAPPED, W, H);
        Graphics2D g(s);
        g.clear(Colors::BLACK);
        g.setTransform(m);
        const Rect r = {0, 0, 24, 20};
        if (all)
          filled ? g.fillEllipse(r, Colors::WHITE) : g.drawEllipse(r, Colors::WHITE);
        else if (filled)
          g.fillSector(r, a0, a1, Colors::WHITE);
        else
          g.drawArc(r, a0, a1, Colors::WHITE);
        return litMask(s, mask);
      };
      const int total = drawn(0, 0, true, whole);
      CHECK(total > 40);
      const float cuts[4] = {0.2f, 1.9f, 3.0f, 4.4f};
      std::memset(cover, 0, sizeof(cover));
      int sum = 0, overlap = 0;
      for (int i = 0; i < 4; i++) {
        sum += drawn(cuts[i], i == 3 ? cuts[0] : cuts[i + 1], false, part);
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

  // Turned rounded rectangles and frames: the outline lies within the fill,
  // the frame leaves the middle empty
  {
    static uint8_t arena[4096];
    OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 60, 60);
    OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 60, 60);
    static bool fill[60 * 60], line[60 * 60];
    for (int withArena = 0; withArena < 2; withArena++) {
      Graphics2D ga(a), gb(b);
      if (withArena) ga.init(arena, sizeof(arena));
      if (withArena) gb.init(arena, sizeof(arena));
      ga.clear(Colors::BLACK);
      gb.clear(Colors::BLACK);
      const affine2f m = affine2f::placement(30, 30, 0.5f, 2, 2, 12, 8);
      ga.setTransform(m);
      gb.setTransform(m);
      ga.fillRoundRect(0, 0, 24, 16, 6, Colors::WHITE);
      gb.drawRoundRect(0, 0, 24, 16, 6, Colors::WHITE);
      const int nf = litMask(a, fill), nl = litMask(b, line);
      // 4 * (24 x 16 - (4 - pi) 6^2)
      CHECK(std::abs(nf - 1413) < 30);
      CHECK(nl > 100 && nl < 250);
      for (int k = 0; k < 60 * 60; k++) CHECK(!line[k] || fill[k]);
      ga.clear(Colors::BLACK);
      ga.drawRect(0, 0, 24, 16, Colors::WHITE, 3);
      CHECK_EQ(ga.getPixel(30, 30, false), Colors::BLACK);
      const int nr = countColor(ga, Colors::WHITE);
      CHECK(std::abs(nr - 4 * (24 * 16 - 18 * 10)) < 30);  // 816
    }
  }
}
#endif  // SHAPOGFX2D_TRANSFORM

// ---------------------------------------------------------------------------
// Polygons and the float API

static void testPolygons() {
  static uint8_t arena[4096];
  const int W = 64, H = 48;
  OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, W, H);
  OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, W, H);
  Graphics2D ga(a), gb(b);
  // vec2i and vec2f vertices on whole pixels fill the same pixels
  vec2i pi[10];
  vec2f pf[10];
  for (int i = 0; i < 10; i++) {
    const float r = (i & 1) ? 22.0f : 9.0f, t = i * PI / 5;
    pi[i] = {32 + (int)std::lround(r * std::cos(t)),
             24 + (int)std::lround(r * std::sin(t))};
    pf[i] = {(float)pi[i].x, (float)pi[i].y};
  }
  ga.fillPolygon(pi, 10, Colors::WHITE);
  gb.fillPolygon(pf, 10, Colors::WHITE);
  CHECK(sameSurface(a, b));
#if SHAPOGFX2D_TRANSFORM
  ga.clear(Colors::BLACK);
  gb.clear(Colors::BLACK);
  ga.setTransform(affine2f::rotation(0.3f, 32, 24));
  gb.setTransform(affine2f::rotation(0.3f, 32, 24));
  ga.fillPolygon(pi, 10, Colors::WHITE);
  gb.fillPolygon(pf, 10, Colors::WHITE);
  CHECK(sameSurface(a, b));
  ga.resetTransform();
  gb.resetTransform();
#endif

  // The vertices are pixels like those of lines: a triangle's fill stays
  // within the outline drawn through the same vertices (in every row, no
  // fill left of the leftmost or right of the rightmost outline pixel)
  {
    uint32_t seed = 31;
    int outside = 0;
    for (int n = 0; n < 60; n++) {
      Graphics2D g(a);
      g.clear(Colors::BLACK);
#if SHAPOGFX2D_TRANSFORM
      if (n & 1) g.setTransform(affine2f::rotation(0.1f * n, 32, 24));
#endif
      const vec2i t[3] = {{randInt(seed, 2, 61), randInt(seed, 2, 45)},
                          {randInt(seed, 2, 61), randInt(seed, 2, 45)},
                          {randInt(seed, 2, 61), randInt(seed, 2, 45)}};
      g.fillPolygon(t, 3, Colors::BLUE);
      g.drawPolygon(t, 3, Colors::WHITE);
      for (int y = 0; y < H; y++) {
        int l = W, r = -1;
        for (int x = 0; x < W; x++) {
          if (g.getPixel(x, y, false) == Colors::WHITE) l = std::min(l, x), r = x;
        }
        for (int x = 0; x < W; x++) {
          if (g.getPixel(x, y, false) == Colors::BLUE && (x < l || x > r))
            outside++;
        }
      }
    }
    CHECK_EQ(outside, 0);
  }

  // Two triangles sharing an edge cover their quadrilateral exactly once
  {
    const vec2f q[4] = {{3.3f, 2.7f}, {50.2f, 8.9f}, {58.6f, 40.1f},
                        {9.1f, 33.4f}};
    static bool whole[W * H], t0[W * H], t1[W * H];
    ga.clear(Colors::BLACK);
    ga.fillPolygon(q, 4, Colors::WHITE);
    const int n = litMask(a, whole);
    ga.clear(Colors::BLACK);
    ga.fillTriangle(q[0], q[1], q[2], Colors::WHITE);
    const int n0 = litMask(a, t0);
    ga.clear(Colors::BLACK);
    ga.fillTriangle(q[2], q[0], q[3], Colors::WHITE);
    const int n1 = litMask(a, t1);
    CHECK_EQ(n0 + n1, n);
    int overlap = 0;
    for (int k = 0; k < W * H; k++) overlap += t0[k] && t1[k];
    CHECK_EQ(overlap, 0);
  }

  // Many edges: in the scratch memory or, without an arena, evaluated per
  // row, with the same pixels; also when the polygon starts above the clip
  // rectangle
  {
    vec2f many[40];
    for (int i = 0; i < 40; i++) {
      const float r = (i & 1) ? 30.0f : 14.0f + (i % 7), t = i * PI / 20;
      many[i] = {32 + r * std::cos(t), 20 + r * std::sin(t)};
    }
    Graphics2D gc(a), gd(b);
    CHECK(gd.init(arena, sizeof(arena)));
    for (int clip = 0; clip < 2; clip++) {
      gc.clear(Colors::BLACK);
      gd.clear(Colors::BLACK);
      if (clip) {
        gc.setClipRect(0, 11, W, 30);
        gd.setClipRect(0, 11, W, 30);
      }
      gc.fillPolygon(many, 40, Colors::WHITE);
      gd.fillPolygon(many, 40, Colors::WHITE);
      CHECK(sameSurface(a, b));
      CHECK(countColor(gc, Colors::WHITE) > 500);
    }
    // ... and the clipped part is the unclipped one's
    static bool full[W * H], part[W * H];
    gc.resetClipRect();
    gc.clear(Colors::BLACK);
    gc.fillPolygon(many, 40, Colors::WHITE);
    litMask(a, full);
    gc.clear(Colors::BLACK);
    gc.setClipRect(0, 11, W, 30);
    gc.fillPolygon(many, 40, Colors::WHITE);
    litMask(a, part);
    for (int y = 11; y < 41; y++) {
      for (int x = 0; x < W; x++) CHECK_EQ(full[y * W + x], part[y * W + x]);
    }
  }
}

static void testFloatApi() {
  OwnedSurface a = createSurface(PixelFormat::RGB565_SWAPPED, 40, 32);
  OwnedSurface b = createSurface(PixelFormat::RGB565_SWAPPED, 40, 32);
  Graphics2D ga(a), gb(b);
  // A pixel is inside where its center is
  ga.fillRect(RectF{1.5f, 2.25f, 3.0f, 4.0f}, Colors::WHITE);
  gb.fillRect(1, 2, 3, 4, Colors::WHITE);
  CHECK(sameSurface(a, b));
  // Whole numbers draw what the integer versions draw
  ga.drawRect(RectF(Rect{6, 3, 12, 9}), Colors::RED, 2.0f);
  gb.drawRect(Rect{6, 3, 12, 9}, Colors::RED, 2);
  ga.fillEllipse(RectF(Rect{20, 2, 13, 9}), Colors::GREEN);
  gb.fillEllipse(Rect{20, 2, 13, 9}, Colors::GREEN);
  ga.drawEllipse(RectF(Rect{2, 14, 17, 11}), Colors::CYAN);
  gb.drawEllipse(Rect{2, 14, 17, 11}, Colors::CYAN);
  ga.fillCircle(vec2f{28, 22}, 6, Colors::YELLOW);
  gb.fillCircle(28, 22, 6, Colors::YELLOW);
  ga.drawCircleArc(vec2f{10, 20}, 5, 0.5f, 2.0f, Colors::WHITE);
  gb.drawCircleArc(10, 20, 5, 0.5f, 2.0f, Colors::WHITE);
  ga.fillRoundRect(RectF(Rect{21, 12, 10, 7}), 3.0f, Colors::BLUE);
  gb.fillRoundRect(Rect{21, 12, 10, 7}, 3, Colors::BLUE);
  ga.drawLine(vec2f{2, 30}, vec2f{37, 26}, Colors::MAGENTA);
  gb.drawLine(2, 30, 37, 26, Colors::MAGENTA);
  const vec2f tri[3] = {{30, 1}, {39, 9}, {33, 11}};
  const vec2i trii[3] = {{30, 1}, {39, 9}, {33, 11}};
  ga.drawPolygon(tri, 3, Colors::WHITE);
  gb.drawPolygon(trii, 3, Colors::WHITE);
  CHECK(sameSurface(a, b));
}

// ---------------------------------------------------------------------------
// Arcs and sectors

static void testArcs() {
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
#if SHAPOGFX2D_TRANSFORM
  // Mirrored, the same quarter turns the other way: (0, pi/2) is the lower
  // left quarter
  drawn(
      [](Graphics2D &g) {
        g.translate(40, 0);
        g.scale(-1, 1);
        g.fillCircleSector(20, 20, 12, 0, PI / 2, Colors::WHITE);
      },
      part);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (x < 19 && y > 20) CHECK_EQ(part[y * W + x], whole[y * W + x + 1]);
      if (x > 19 || y < 20) CHECK(!part[y * W + x]);
    }
  }
#endif
}

void testGraphics2D() {
  for (PixelFormat f : kFormats) testFillAndClip(f);
#if SHAPOGFX_FORMAT_RGB565_SWAPPED
  testBlending();
  testFormatConsistency();
  testBitmapAndText();
  testMetrics();
  testFarGeometry();
  testCoordLimit();
  testStateStack();
  testScaledImage();
#if SHAPOGFX2D_TRANSFORM
  testAffineImage();
  testTransformedShapes();
#endif
#if SHAPOGFX2D_COLOR_KEY
  testColorKey();
#endif
  testPolygons();
  testFloatApi();
  testArcs();
#endif
  testOwnedSurface();
  testAffineHelpers();
}
