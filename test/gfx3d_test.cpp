// gfx3d: banded rendering, output formats, transparent clear, texture formats

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "check.hpp"
#include "shapoco/gfx2d/surface_alloc.hpp"
#include "shapoco/gfx3d/gfx3d.hpp"

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;

static constexpr int W = 96, H = 64;
static uint8_t arena[48 * 1024];

static uint16_t tex565[16 * 16];
static uint16_t tex4444[16 * 16];
static uint8_t texG1[16 * 2];
static uint8_t tex444[24 * 16];
static const g3::Texture T565 = {g3::PixelFormat::RGB565BE, 16, 16, 32, tex565};
static const g3::Texture T4444 = {g3::PixelFormat::ARGB4444, 16, 16, 32,
                                  tex4444};
static const g3::Texture TG1 = {g3::PixelFormat::GRAY1, 16, 16, 2, texG1};
static const g3::Texture T444 = {g3::PixelFormat::RGB444, 16, 16, 24, tex444};

static const g3::Material M_RED = {{0.9f, 0.1f, 0.1f, 1},
                                   {0.9f, 0.1f, 0.1f, 1},
                                   nullptr,
                                   g3::BlendMode::NONE,
                                   0};
static const g3::Material M_GLASS = {{0.2f, 0.4f, 1.0f, 0.5f},
                                     {0.2f, 0.4f, 1.0f, 1},
                                     nullptr,
                                     g3::BlendMode::ALPHA,
                                     g3::MaterialFlags::DOUBLE_SIDED};
static const g3::Material M_TEX565 = {{1, 1, 1, 1},
                                      {1, 1, 1, 1},
                                      &T565,
                                      g3::BlendMode::NONE,
                                      g3::MaterialFlags::TEXTURE};
static const g3::Material M_TEX4444 = {{1, 1, 1, 1},
                                       {1, 1, 1, 1},
                                       &T4444,
                                       g3::BlendMode::NONE,
                                       g3::MaterialFlags::TEXTURE};
static const g3::Material M_TEXG1 = {{1, 1, 1, 1},
                                     {1, 1, 1, 1},
                                     &TG1,
                                     g3::BlendMode::NONE,
                                     g3::MaterialFlags::TEXTURE};
static const g3::Material M_TEX444 = {{1, 1, 1, 1},
                                      {1, 1, 1, 1},
                                      &T444,
                                      g3::BlendMode::NONE,
                                      g3::MaterialFlags::TEXTURE};

static void genTextures() {
  for (int y = 0; y < 16; y++) {
    g2::CursorGray1 c1;
    c1.init(texG1 + y * 2, 0);
    g2::CursorRgb444 c4;
    c4.init(tex444 + y * 24, 0);
    for (int x = 0; x < 16; x++) {
      bool on = ((x >> 2) ^ (y >> 2)) & 1;
      tex565[y * 16 + x] =
          g2::packRgb565BE(on ? 1.0f : 0.2f, 0.5f, on ? 0.2f : 1.0f);
      tex4444[y * 16 + x] =
          g2::makeArgb4444(on ? 15 : 0, 15, 8, 2);  // alpha holes
      c1.write(on);
      c1.next();
      c4.write(g2::makeRgb444(on ? 15 : 3, 8, on ? 3 : 15));
      c4.next();
    }
  }
}

static void buildScene(g3::Renderer &r, const g3::Material *mat, float t) {
  r.setPerspectiveProjection(1.0f, (float)W / H, 0.3f, 50.0f);
  r.beginScene();
  r.translate(0, 0, -4);
  r.rotate(0.5f, 1, 0, 0);
  r.enableParallelLight({-0.5f, -1, -0.5f}, {1, 1, 1, 1});
  r.enableEnvironmentLight({0.3f, 0.3f, 0.3f, 1});
  r.pushState();
  r.rotate(t, 0.3f, 1, 0);
  r.setMaterial(*mat);
  r.putCube({0, 0, 0}, {1.5f, 1.5f, 1.5f});
  r.popState();
  r.pushState();
  r.translate(1.2f, 0, 0.5f);
  r.rotate(t * 1.3f, 1, 0.5f, 0);
  r.setMaterial(M_GLASS);
  r.putCube({0, 0, 0}, {1.0f, 1.0f, 1.0f});
  r.popState();
  r.endScene();
}

static g2::Color pixelAt(const g2::Surface &s, int x, int y) {
  g2::Graphics2D g(s);
  return g.getPixel(x, y);
}

static void testBandsAndClear() {
  g3::Renderer r;
  r.init(W, H, arena, sizeof(arena));
  CHECK(r.isInitialized());
  g2::OwnedSurface whole = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  g2::OwnedSurface banded = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  r.setClearColor({0.1f, 0.2f, 0.3f, 1});
  buildScene(r, &M_RED, 0.7f);
  r.beginRender();
  r.render(0, 0, W, H, whole);
  for (int y = 0; y < H; y += 10) r.render(0, y, W, 10, banded, 0, y);
  r.endRender();
  CHECK(std::memcmp(whole.pixels(), banded.pixels(), whole.bytes()) == 0);
  g3::Stats st = r.getStats();
  CHECK(st.triCount > 0 && st.triDropped == 0 && st.spanDropped == 0);

  // Rendering only a sub-rectangle into an offset target
  g2::OwnedSurface part = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  g2::Graphics2D gp(part);
  gp.clear(g2::Colors::MAGENTA);
  r.beginRender();
  r.render(10, 10, 30, 20, part, 12, 14);
  r.endRender();
  for (int y = 0; y < 20; y++) {
    for (int x = 0; x < 30; x++) {
      CHECK_EQ(pixelAt(part, 12 + x, 14 + y), pixelAt(whole, 10 + x, 10 + y));
    }
  }
  CHECK_EQ(pixelAt(part, 11, 14), g2::Colors::MAGENTA);
  CHECK_EQ(pixelAt(part, 12, 13), g2::Colors::MAGENTA);
  CHECK_EQ(pixelAt(part, 42, 14), g2::Colors::MAGENTA);

  // Transparent clear keeps the background where nothing is drawn
  g2::OwnedSurface bg = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  g2::Graphics2D gb(bg);
  gb.clear(g2::Colors::GREEN);
  r.disableClear();
  CHECK(!r.isClearEnabled());
  r.beginRender();
  r.render(0, 0, W, H, bg);
  r.endRender();
  const g2::Color clearCol = pixelAt(whole, 0, 0);
  int kept = 0, drawn = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      g2::Color a = pixelAt(whole, x, y), b = pixelAt(bg, x, y);
      if (a == clearCol) {
        CHECK_EQ(b, g2::Colors::GREEN);
        kept++;
      } else if (a == b) {
        drawn++;
      }
    }
  }
  CHECK(kept > 0 && drawn > 0);
  r.deinit();
  CHECK(!r.isInitialized());
}

static void testOutputFormats() {
  g3::Renderer r;
  r.init(W, H, arena, sizeof(arena));
  r.setClearColor({0.1f, 0.2f, 0.3f, 1});
  const g3::Material *mats[] = {&M_RED, &M_TEX565, &M_TEX4444, &M_TEXG1,
                                &M_TEX444};
  for (const g3::Material *m : mats) {
    g2::OwnedSurface s565 = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
    g2::OwnedSurface s444 = g2::createSurface(g2::PixelFormat::RGB444, W, H);
    buildScene(r, m, 0.4f);
    r.beginRender();
    r.render(0, 0, W, H, s565);
    // odd offsets exercise the nibble alignment of RGB444
    for (int y = 0; y < H; y += 7) r.render(1, y, W - 2, 7, s444, 1, y);
    r.endRender();
    CHECK_EQ(r.getStats().triDropped, 0);
    int differing = 0;
    for (int y = 0; y < H; y++) {
      for (int x = 1; x < W - 1; x++) {
        g2::Color a = pixelAt(s565, x, y), b = pixelAt(s444, x, y);
        if (std::abs(g2::colorR(a) - g2::colorR(b)) > 24 ||
            std::abs(g2::colorG(a) - g2::colorG(b)) > 24 ||
            std::abs(g2::colorB(a) - g2::colorB(b)) > 24) {
          differing++;
        }
      }
    }
    // A few pixels may differ where blending rounds differently
    CHECK(differing < W * H / 100);
  }
  // Unsupported output format is ignored without touching the buffer
  g2::OwnedSurface gray = g2::createSurface(g2::PixelFormat::GRAY1, W, H);
  std::memset(gray.pixels(), 0xFF, gray.bytes());
  r.beginRender();
  r.render(0, 0, W, H, gray);
  r.endRender();
  CHECK_EQ(((uint8_t *)gray.pixels())[0], 0xFF);
}

static void testTextureAlpha() {
  // Holes in an ARGB4444 texture show the background
  g3::Renderer r;
  r.init(W, H, arena, sizeof(arena));
  g2::OwnedSurface s = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  r.setClearColor({0, 1, 0, 1});
  r.setOrthographicProjection(-1, 1, -1, 1, 0.1f, 10);
  r.beginScene();
  r.translate(0, 0, -2);
  r.setMaterial(M_TEX4444);
  r.putCube({0, 0, 0}, {1.6f, 1.6f, 0.2f});
  r.endScene();
  r.beginRender();
  r.render(0, 0, W, H, s);
  r.endRender();
  int green = 0, other = 0;
  for (int y = H / 4; y < H * 3 / 4; y++) {
    for (int x = W / 4; x < W * 3 / 4; x++) {
      if (pixelAt(s, x, y) == g2::Colors::GREEN)
        green++;
      else
        other++;
    }
  }
  CHECK(green > 0 && other > 0);
}

void testGfx3D() {
  genTextures();
  testBandsAndClear();
  testOutputFormats();
  testTextureAlpha();
}
