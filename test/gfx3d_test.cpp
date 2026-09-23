// gfx3d: banded rendering, output formats, transparent clear, texture formats

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "check.hpp"
#include "shapoco/gfx2d/surface_alloc.hpp"
#include "shapoco/gfx3d/gfx3d.hpp"

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;

#ifndef SHAPOGFX3D_LAYER_MAX  // mirrors the library default
#define SHAPOGFX3D_LAYER_MAX 8
#endif

static constexpr int W = 96, H = 64;
static uint8_t arena[256 * 1024];

static uint16_t tex565[16 * 16];
static uint16_t tex4444[16 * 16];
static uint8_t texG1[16 * 2];
static uint8_t tex444[24 * 16];
static const g3::Texture T565 = {g3::PixelFormat::RGB565BE, 16, 16, 32, tex565};
static const g3::Texture T4444 = {g3::PixelFormat::ARGB4444, 16, 16, 32,
                                  tex4444};
static const g3::Texture TG1 = {g3::PixelFormat::GRAY1, 16, 16, 2, texG1};
static uint16_t tex565n[16 * 16];  // tex565 in native byte order
static const g3::Texture T565N = {g3::PixelFormat::RGB565, 16, 16, 32, tex565n};
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
static const g3::Material M_TEX565N = {{1, 1, 1, 1},
                                       {1, 1, 1, 1},
                                       &T565N,
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
      tex565n[y * 16 + x] = g2::bswap16(tex565[y * 16 + x]);
      tex4444[y * 16 + x] =
          g2::makeArgb4444(on ? 15 : 0, 15, 8, 2);  // alpha holes
      c1.write(on);
      c1.next();
      c4.write(g2::makeRgb444(on ? 15 : 3, 8, on ? 3 : 15));
      c4.next();
    }
  }
}

static void buildScene(g3::Graphics3D &r, const g3::Material *mat, float t) {
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
  g3::Graphics3D r;
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
  g3::Graphics3D r;
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
#if SHAPOGFX_FORMAT_RGB565
  // Native RGB565 output and textures: the same pixels as RGB565BE,
  // byte-swapped. The BE-textured scene into a native target, the
  // native-textured one into a BE target.
  for (const g3::Material *m : {&M_RED, &M_TEX565, &M_GLASS}) {
    g2::OwnedSurface be = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
    g2::OwnedSurface nat = g2::createSurface(g2::PixelFormat::RGB565, W, H);
    g2::OwnedSurface beN = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
    buildScene(r, m, 0.4f);
    r.beginRender();
    r.render(0, 0, W, H, be);
    r.render(0, 0, W, H, nat);
    r.endRender();
    buildScene(r, m == &M_TEX565 ? &M_TEX565N : m, 0.4f);
    r.beginRender();
    r.render(0, 0, W, H, beN);
    r.endRender();
    int bad = 0;
    const uint16_t *pb = (const uint16_t *)be.pixels();
    const uint16_t *pn = (const uint16_t *)nat.pixels();
    for (int i = 0; i < W * H; i++) bad += (pn[i] != g2::bswap16(pb[i]));
    CHECK_EQ(bad, 0);
    CHECK(std::memcmp(be.pixels(), beN.pixels(), be.bytes()) == 0);
  }
#endif

  // Unsupported output format is ignored without touching the buffer
  g2::OwnedSurface gray = g2::createSurface(g2::PixelFormat::GRAY1, W, H);
  std::memset(gray.pixels(), 0xFF, gray.bytes());
  r.beginRender();
  r.render(0, 0, W, H, gray);
  r.endRender();
  CHECK_EQ(((uint8_t *)gray.pixels())[0], 0xFF);
}

#if SHAPOGFX3D_TEXTURE && SHAPOGFX3D_BLEND
static void testTextureAlpha() {
  // Holes in an ARGB4444 texture show the background
  g3::Graphics3D r;
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
#endif  // SHAPOGFX3D_TEXTURE && SHAPOGFX3D_BLEND

// Every shape must render identically with and without back-face culling:
// otherwise its winding is wrong (the culled version would show the inside).
static void testShapeWinding() {
  static const g3::Material M_CULL = {{0.8f, 0.8f, 0.8f, 1},
                                      {0.8f, 0.8f, 0.8f, 1},
                                      nullptr,
                                      g3::BlendMode::NONE,
                                      0};
  static const g3::Material M_BOTH = {{0.8f, 0.8f, 0.8f, 1},
                                      {0.8f, 0.8f, 0.8f, 1},
                                      nullptr,
                                      g3::BlendMode::NONE,
                                      g3::MaterialFlags::DOUBLE_SIDED};
  struct ShapeCase {
    const char *name;
    void (*put)(g3::Graphics3D &);
  };
  const ShapeCase cases[] = {
      {"cube",
       [](g3::Graphics3D &g) { g.putCube({0, 0, 0}, {1.2f, 1.2f, 1.2f}, 2); }},
      {"plane",
       [](g3::Graphics3D &g) { g.putPlane({0, -0.3f, 0}, 1.5f, 1.5f, 2, 3); }},
      {"disk", [](g3::Graphics3D &g) { g.putDisk({0, -0.3f, 0}, 0.9f, 20); }},
      {"sphere",
       [](g3::Graphics3D &g) { g.putSphereUV({0, 0, 0}, 0.9f, 20, 10); }},
      {"icosphere",
       [](g3::Graphics3D &g) { g.putIcosphere({0, 0, 0}, 0.9f, 2); }},
      {"cylinder",
       [](g3::Graphics3D &g) { g.putCylinder({0, 0, 0}, 0.6f, 1.4f, 18, 2); }},
      {"cone",
       [](g3::Graphics3D &g) {
         g.putCone({0, 0, 0}, 0.8f, 0.2f, 1.4f, 18, 3);
       }},
      {"cone-apex",
       [](g3::Graphics3D &g) {
         g.putCone({0, 0, 0}, 0.8f, 0.0f, 1.4f, 12, 1);
       }},
      {"torus",
       [](g3::Graphics3D &g) { g.putTorus({0, 0, 0}, 0.7f, 0.25f, 20, 10); }},
  };
  g3::Graphics3D r;
  r.init(W, H, arena, sizeof(arena));
  r.setClearColor({0, 0, 0, 1});
  for (const ShapeCase &sc : cases) {
    g2::OwnedSurface a = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
    g2::OwnedSurface b = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
    for (int pass = 0; pass < 2; pass++) {
      r.setPerspectiveProjection(1.0f, (float)W / H, 0.3f, 50.0f);
      r.beginScene();
      r.lookAt({2.2f, 1.6f, 2.6f}, {0, 0, 0});
      r.enableParallelLight({-0.3f, -1, -0.4f}, {1, 1, 1, 1});
      r.enableEnvironmentLight({0.2f, 0.2f, 0.2f, 1});
      r.setMaterial(pass == 0 ? M_CULL : M_BOTH);
      sc.put(r);
      r.endScene();
      r.beginRender();
      r.render(0, 0, W, H, pass == 0 ? a : b);
      r.endRender();
      CHECK_EQ(r.getStats().triDropped, 0);
      CHECK_EQ(r.getStats().badIndices, 0);
    }
    bool same = std::memcmp(a.pixels(), b.pixels(), a.bytes()) == 0;
    if (!same) std::printf("  winding mismatch: %s\n", sc.name);
    CHECK(same);
    // Something must have been drawn
    int drawn = 0;
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        if (pixelAt(a, x, y) != g2::Colors::BLACK) drawn++;
      }
    }
    CHECK(drawn > 100);  // flat shapes are foreshortened at this camera angle
  }
}

// Vertex colors modulate the lit color; out-of-range indices are counted and
// dropped
static void testVertexColorAndIndices() {
  static const g3::Vertex verts[4] = {
      {{-1, -1, 0}, {0, 0, 1}, {0, 1}, 0xFFFF0000u},
      {{1, -1, 0}, {0, 0, 1}, {1, 1}, 0xFFFF0000u},
      {{1, 1, 0}, {0, 0, 1}, {1, 0}, 0xFFFF0000u},
      {{-1, 1, 0}, {0, 0, 1}, {0, 0}, 0xFFFF0000u},
  };
  static const g3::VertexBuffer vb = {4, verts};
  static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
  static const uint16_t badIdx[6] = {0, 1, 2, 0, 2, 9};
  static const g3::Material M_VC = {{1, 1, 1, 1},
                                    {1, 1, 1, 1},
                                    nullptr,
                                    g3::BlendMode::NONE,
                                    g3::MaterialFlags::VERTEX_COLOR};
  static const g3::Material M_NOVC = {
      {1, 1, 1, 1}, {1, 1, 1, 1}, nullptr, g3::BlendMode::NONE, 0};
  g3::Graphics3D r;
  r.init(W, H, arena, sizeof(arena));
  r.setClearColor({0, 0, 0, 1});
  r.setOrthographicProjection(-2, 2, -2, 2, 0.1f, 10);
  g2::OwnedSurface s = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  for (int pass = 0; pass < 3; pass++) {
    r.beginScene();
    r.translate(0, 0, -3);
    r.enableEnvironmentLight({1, 1, 1, 1});
    g3::Primitive prim = {g3::PrimitiveType::TRIANGLES, &vb, 6,
                          pass == 2 ? badIdx : idx,
                          pass == 0 ? &M_VC : &M_NOVC};
    r.putPrimitive(prim);
    r.endScene();
    r.beginRender();
    r.render(0, 0, W, H, s);
    r.endRender();
    g2::Color c = pixelAt(s, W / 2, H / 2);
    if (pass == 0) {
      CHECK(g2::colorR(c) > 200 && g2::colorG(c) < 20 &&
            g2::colorB(c) < 20);  // red
    } else if (pass == 1) {
      CHECK_EQ(c, g2::Colors::WHITE);
      CHECK_EQ(r.getStats().badIndices, 0);
    } else {
      CHECK_EQ(r.getStats().badIndices, 1);
      CHECK_EQ(r.getStats().triCount, 1);  // the second triangle was dropped
    }
  }
}

#if SHAPOGFX3D_LINES && SHAPOGFX3D_POINTS
// Points and lines: Bresenham-like coverage, end points, hidden-line removal,
// point size, near-plane clipping, depth bias
static void testPointsAndLines() {
  static const g3::Material M_LINE = {
      {1, 1, 1, 1}, {1, 1, 1, 1}, nullptr, g3::BlendMode::NONE, 0};
  static const g3::Material M_SOLID = {{0.5f, 0.5f, 0.5f, 1},
                                       {0.5f, 0.5f, 0.5f, 1},
                                       nullptr,
                                       g3::BlendMode::NONE,
                                       0};
  g3::Graphics3D r;
  r.init(W, H, arena, sizeof(arena));
  r.setClearColor({0, 0, 0, 1});
  g2::OwnedSurface s = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  auto render = [&]() {
    r.beginRender();
    r.render(0, 0, W, H, s);
    r.endRender();
  };
  auto countWhite = [&]() {
    int n = 0;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (pixelAt(s, x, y) == g2::Colors::WHITE) n++;
    return n;
  };
  // Orthographic view: x in [-1, 1] spans the width, y scaled by the aspect
  // ratio
  const float ay = (float)H / W;
  auto ortho = [&]() {
    r.setOrthographicProjection(-1, 1, -ay, ay, 0.1f, 10);
    r.beginScene();
    r.translate(0, 0, -2);
    r.setMaterial(M_LINE);
  };
  auto sx = [&](float x) { return (int)((x * 0.5f + 0.5f) * W); };
  auto sy = [&](float y) { return (int)((0.5f - y / ay * 0.5f) * H); };

  // Shallow line across the screen: every column has exactly one white pixel
  ortho();
  r.putLine({-0.9f, -0.2f, 0}, {0.9f, 0.3f, 0});
  r.endScene();
  render();
  int badCols = 0, cols = 0;
  for (int x = 0; x < W; x++) {
    int n = 0;
    for (int y = 0; y < H; y++)
      if (pixelAt(s, x, y) == g2::Colors::WHITE) n++;
    if (n == 1)
      cols++;
    else if (n > 1)
      badCols++;
  }
  CHECK(badCols <= 2);  // the end point rows may add one pixel each
  CHECK(cols >= (int)(W * 0.9f) - 2);
  CHECK_EQ(pixelAt(s, sx(-0.9f), sy(-0.2f)),
           g2::Colors::WHITE);  // end points drawn
  CHECK_EQ(pixelAt(s, sx(0.9f), sy(0.3f)), g2::Colors::WHITE);

  // Steep line: exactly one pixel per row over its extent
  ortho();
  r.putLine({-0.1f, -0.5f, 0}, {0.2f, 0.5f, 0});
  r.endScene();
  render();
  int rows = 0, badRows = 0;
  for (int y = 0; y < H; y++) {
    int n = 0;
    for (int x = 0; x < W; x++)
      if (pixelAt(s, x, y) == g2::Colors::WHITE) n++;
    if (n == 1)
      rows++;
    else if (n > 1)
      badRows++;
  }
  CHECK_EQ(badRows, 0);
  CHECK(rows > H / 2);

  // LINE_LOOP closes the loop: a rectangle has pixels on all four edges
  ortho();
  {
    static const g3::Vertex v[4] = {
        {{-0.5f, -0.3f, 0}, {0, 1, 0}, {0, 0}, g3::VERTEX_WHITE},
        {{0.5f, -0.3f, 0}, {0, 1, 0}, {0, 0}, g3::VERTEX_WHITE},
        {{0.5f, 0.3f, 0}, {0, 1, 0}, {0, 0}, g3::VERTEX_WHITE},
        {{-0.5f, 0.3f, 0}, {0, 1, 0}, {0, 0}, g3::VERTEX_WHITE}};
    static const g3::VertexBuffer vb = {4, v};
    static const uint16_t idx[4] = {0, 1, 2, 3};
    g3::Primitive loop = {g3::PrimitiveType::LINE_LOOP, &vb, 4, idx, nullptr};
    r.putPrimitive(loop);
  }
  r.endScene();
  render();
  CHECK(countWhite() > W / 2);
  CHECK_EQ(pixelAt(s, W / 2, sy(0.3f)), g2::Colors::WHITE);   // top edge
  CHECK_EQ(pixelAt(s, W / 2, sy(-0.3f)), g2::Colors::WHITE);  // bottom edge
  CHECK_EQ(pixelAt(s, sx(-0.5f), H / 2), g2::Colors::WHITE);  // left edge
  CHECK_EQ(pixelAt(s, sx(0.5f), H / 2), g2::Colors::WHITE);   // right edge

  // Hidden-line removal: a cube in front hides the middle of the line
  ortho();
  r.putLine({-0.9f, 0, -1}, {0.9f, 0, -1});
  r.setMaterial(M_SOLID);
  r.putCube({0, 0, 0}, {0.6f, 0.6f, 0.6f});
  r.endScene();
  render();
  CHECK(pixelAt(s, W / 2, H / 2) != g2::Colors::WHITE);  // covered by the cube
  CHECK_EQ(pixelAt(s, W / 10, H / 2), g2::Colors::WHITE);  // visible part

  // Points: size 1 draws one pixel, size 3 draws nine
  {
    static const g3::Vertex v[1] = {
        {{0.11f, 0.07f, 0}, {0, 1, 0}, {0, 0}, g3::VERTEX_WHITE}};
    static const g3::VertexBuffer vb = {1, v};
    static const uint16_t idx[1] = {0};
    g3::Primitive pts = {g3::PrimitiveType::POINTS, &vb, 1, idx, nullptr};
    ortho();
    r.setPointSize(1);
    r.putPrimitive(pts);
    r.endScene();
    render();
    CHECK_EQ(countWhite(), 1);
    ortho();
    r.setPointSize(3);
    r.putPrimitive(pts);
    r.endScene();
    render();
    CHECK_EQ(countWhite(), 9);
    r.setPointSize(1);
  }

  // Near-plane clipping: a line ending behind the camera is drawn partially
  ortho();
  r.putLine({-0.5f, 0, -1},
            {0.5f, 0, 5});  // second end is behind the camera (view z > -zNear)
  r.endScene();
  render();
  CHECK(countWhite() >= 1);
  CHECK_EQ(r.getStats().triDropped, 0);

  // Depth bias: a line coplanar with a quad is visible with a negative bias,
  // hidden with a positive one
  for (int pass = 0; pass < 2; pass++) {
    r.setOrthographicProjection(-1, 1, -ay, ay, 0.1f, 10);
    r.beginScene();
    r.translate(0, 0, -2);
    r.rotate(1.5707963f, 1, 0, 0);  // the plane's +Y now points at the camera
    r.setMaterial(M_SOLID);
    r.putPlane({0, 0, 0}, 1.6f, 1.0f);
    r.setMaterial(M_LINE);
    r.setDepthBias(pass == 0 ? -0.01f : 0.01f);
    r.putLine({-0.7f, 0, 0.3f}, {0.7f, 0, -0.3f});  // in the plane (y = 0)
    r.setDepthBias(0.0f);
    r.endScene();
    render();
    int n = countWhite();
    if (pass == 0) {
      CHECK(n > W / 2);
    } else {
      CHECK_EQ(n, 0);
    }
  }
  CHECK_EQ(r.getStats().badIndices, 0);
}
#endif  // SHAPOGFX3D_LINES && SHAPOGFX3D_POINTS

// Layers, the record layouts and the configurable span pool
static void testLayersAndConfig() {
  static const g3::Material M_BLUE = {{0.1f, 0.2f, 0.9f, 1},
                                      {0.1f, 0.2f, 0.9f, 1},
                                      nullptr,
                                      g3::BlendMode::NONE,
                                      0};
  g3::Graphics3D r;
  g3::Config cfg = g3::defaultConfig(W, H, arena, sizeof(arena));
  cfg.spanCapacity = 48;
  r.init(cfg);
  CHECK(r.isInitialized());
  CHECK_EQ(r.getStats().spanCapacity, 48);

  g2::OwnedSurface s = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  r.setClearColor({0, 0, 0, 1});
  const float ay = (float)H / W;
  // near: a small blue cube, far: a large red one behind it
  enum Which { FAR_ONLY, NEAR_ONLY, BOTH, BOTH_LAYERED, BOTH_NO_DEPTH };
  auto build = [&](Which which) {
    r.setOrthographicProjection(-1, 1, -ay, ay, 0.1f, 10);
    r.beginScene();
    r.disableParallelLight();
    r.enableEnvironmentLight({1, 1, 1, 1});
    if (which == BOTH_NO_DEPTH) r.beginLayer(g3::LayerFlags::NO_DEPTH);
    if (which != FAR_ONLY) {
      r.setMaterial(M_BLUE);
      r.putCube({0, 0, -2}, {0.5f, 0.5f, 0.5f});
    }
    if (which == BOTH_LAYERED) r.beginLayer();
    if (which != NEAR_ONLY) {
      r.setMaterial(M_RED);
      r.putCube({0, 0, -3}, {1.0f, 1.0f, 1.0f});
    }
    r.endScene();
  };
  auto center = [&]() {
    r.beginRender();
    r.render(0, 0, W, H, s);
    r.endRender();
    return pixelAt(s, W / 2, H / 2);
  };

  build(FAR_ONLY);
  const g2::Color colFar = center();
  build(NEAR_ONLY);
  const g2::Color colNear = center();
  CHECK(colFar != colNear);
  CHECK(colFar != g2::Colors::BLACK && colNear != g2::Colors::BLACK);

  // One layer: the depth test puts the near cube in front
  build(BOTH);
  CHECK_EQ(center(), colNear);
  CHECK_EQ(r.getStats().layerCount, 1);
  CHECK_EQ(r.getStats().triDropped, 0);
  CHECK_EQ(r.getStats().spanDropped, 0);
  const size_t bytesWithDepth = r.getStats().triBytes;

#if SHAPOGFX3D_LAYER_MAX >= 2
  // The far cube in a later layer is drawn in front of the near one
  build(BOTH_LAYERED);
  CHECK_EQ(center(), colFar);
  CHECK_EQ(r.getStats().layerCount, 2);
#endif

  // Without depth the primitive added later wins, and its records are smaller
  build(BOTH_NO_DEPTH);
  CHECK_EQ(center(), colFar);
  CHECK_EQ(r.getStats().layerCount, 1);
  const g3::Stats stNoDepth = r.getStats();
  CHECK(stNoDepth.triBytes < bytesWithDepth);
  // 12 bytes of depth plane per primitive (more where the alignment of a
  // 64-bit host rounds the records up)
  const size_t saved = bytesWithDepth - stNoDepth.triBytes;
  CHECK(saved >= 8u * (size_t)stNoDepth.triCount);
  CHECK_EQ(saved % (size_t)stNoDepth.triCount, 0u);

  // beginScene() opens a layer implicitly; beyond the table the calls are
  // counted and the primitives stay in the current layer
  r.beginScene();
  for (int i = 0; i < 200; i++) {
    r.beginLayer();
    r.setMaterial(M_RED);
    r.putCube({0, 0, -3}, {0.2f, 0.2f, 0.2f});
  }
  r.endScene();
  const g3::Stats stMany = r.getStats();
  CHECK(stMany.layerCount >= 1);
  CHECK_EQ(stMany.layersDropped, 200 - stMany.layerCount);
  CHECK_EQ(stMany.triDropped, 0);

  // A flat untextured primitive needs a smaller record than a textured one
  auto bytesPerTri = [&](const g3::Material &m) {
    r.setOrthographicProjection(-1, 1, -ay, ay, 0.1f, 10);
    r.beginScene();
    r.disableParallelLight();
    r.enableEnvironmentLight({1, 1, 1, 1});
    r.setMaterial(m);
    r.putCube({0, 0, -3}, {1.0f, 1.0f, 1.0f});
    r.endScene();
    const g3::Stats st = r.getStats();
    CHECK(st.triCount > 0);
    return (double)st.triBytes / st.triCount;
  };
  const double plain = bytesPerTri(M_RED);
#if SHAPOGFX3D_TEXTURE
  CHECK(bytesPerTri(M_TEX565) > plain);
#endif
  CHECK(plain < (double)(sizeof(float) * 32));

  // A span pool too small for the scene drops spans instead of overflowing
  cfg.spanCapacity = 1;
  r.init(cfg);
  r.setClearColor({0, 0, 0, 1});
  build(BOTH);
  r.beginRender();
  r.render(0, 0, W, H, s);
  r.endRender();
  CHECK(r.getStats().spanDropped > 0);

  // A screen beyond SHAPOGFX_COORD_MAX is refused
#if SHAPOGFX_COORD_MAX < 32767
  r.init((int16_t)(SHAPOGFX_COORD_MAX + 1), 8, arena, sizeof(arena));
  CHECK(!r.isInitialized());
#endif
  r.init((int16_t)SHAPOGFX_COORD_MAX, 8, arena, sizeof(arena));
  CHECK(r.isInitialized());
}

// Triangles sharing an edge leave no pixel between them: every row of a
// convex silhouette made of many triangles is one unbroken run
static void testWatertight() {
  static const g3::Material M_W = {{1, 1, 1, 1},
                                   {1, 1, 1, 1},
                                   nullptr,
                                   g3::BlendMode::NONE,
                                   g3::MaterialFlags::DOUBLE_SIDED};
  g3::Graphics3D r;
  r.init(W, H, arena, sizeof(arena));
  g2::OwnedSurface s = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  r.setClearColor({1, 0, 1, 1});
  const g2::Color clear = g2::makeColor(255, 0, 255);
  int gaps = 0, lit = 0;
  for (int i = 0; i < 12; i++) {
    const float t = 0.37f * (float)i;
    r.setPerspectiveProjection(1.0f, (float)W / H, 0.3f, 50.0f);
    r.beginScene();
    r.translate(0, 0, -3.2f);
    r.rotate(0.4f + t, 1, 0.2f, 0);
    r.rotate(t * 1.7f, 0, 1, 0);
    r.enableEnvironmentLight({0.3f, 0.3f, 0.3f, 1});
    r.enableParallelLight({-0.5f, -1, -0.5f}, {1, 1, 1, 1});
    r.setMaterial(M_W);
    if (i & 1) {
      r.putIcosphere({0, 0, 0}, 1.2f, 3);
    } else {
      r.putPlane({0, 0, 0}, 2.0f, 2.0f, 13, 11);
    }
    r.endScene();
    r.beginRender();
    r.render(0, 0, W, H, s);
    r.endRender();
    for (int y = 0; y < H; y++) {
      int x0 = W, x1 = -1;
      for (int x = 0; x < W; x++) {
        if (pixelAt(s, x, y) != clear) {
          x0 = std::min(x0, x);
          x1 = std::max(x1, x);
        }
      }
      for (int x = x0 + 1; x < x1; x++) {
        if (pixelAt(s, x, y) == clear) gaps++;
      }
      if (x1 >= x0) lit += x1 - x0 + 1;
    }
  }
  CHECK_EQ(gaps, 0);
  CHECK(lit > W * H);
}

// A vertex far beyond the screen (and the guard band of the setup): the
// float build clips the triangle to the guard band in screen space, so its
// coverage stays exact; the fixed-point build clamps the vertex, which bends
// the edges (only checked for the float build)
static void testFarVertex() {
  static const g3::Material M_F = {{1, 1, 1, 1},
                                   {1, 1, 1, 1},
                                   nullptr,
                                   g3::BlendMode::NONE,
                                   g3::MaterialFlags::DOUBLE_SIDED};
  g3::Graphics3D r;
  r.init(W, H, arena, sizeof(arena));
  g2::OwnedSurface s = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  r.setClearColor({0, 0, 0, 1});
  const float ay = (float)H / W;
  // Orthographic: x in [-1, 1] spans the width
  const float px[3] = {-0.5f, -0.3f, 400.0f}, py[3] = {0.2f, 0.5f, -150.0f};
  r.setOrthographicProjection(-1, 1, -ay, ay, 0.1f, 10);
  r.beginScene();
  r.translate(0, 0, -2);
  r.setMaterial(M_F);
  {
    g3::Vertex v[3];
    for (int i = 0; i < 3; i++) {
      v[i] = {{px[i], py[i], 0}, {0, 0, 1}, {0, 0}, g3::VERTEX_WHITE};
    }
    const g3::VertexBuffer vb = {3, v};
    static const uint16_t idx[3] = {0, 1, 2};
    r.putPrimitive({g3::PrimitiveType::TRIANGLES, &vb, 3, idx, nullptr});
  }
  r.endScene();
  r.beginRender();
  r.render(0, 0, W, H, s);
  r.endRender();
  // Exact coverage of the pixel centers, by edge functions in screen space
  float sxv[3], syv[3];
  for (int i = 0; i < 3; i++) {
    sxv[i] = (px[i] * 0.5f + 0.5f) * W;
    syv[i] = (0.5f - py[i] / ay * 0.5f) * H;
  }
  int wrong = 0, inside = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const double cx = x + 0.5, cy = y + 0.5;
      double e[3];
      bool in = true, near = false;
      for (int k = 0; k < 3; k++) {
        const int j = (k + 1) % 3;
        const double ex = sxv[j] - sxv[k], ey = syv[j] - syv[k];
        e[k] = ((cx - sxv[k]) * ey - (cy - syv[k]) * ex) /
               std::sqrt(ex * ex + ey * ey);
        if (std::fabs(e[k]) < 0.01) near = true;
      }
      const bool pos = e[0] > 0 && e[1] > 0 && e[2] > 0;
      const bool neg = e[0] < 0 && e[1] < 0 && e[2] < 0;
      in = pos || neg;
      if (in) inside++;
      if (!near && in != (pixelAt(s, x, y) != g2::Colors::BLACK)) wrong++;
    }
  }
  CHECK(inside > W * H / 8);
#if SHAPOGFX3D_FIXED_POINT
  (void)wrong;  // clamped: see SPEC.md, "Fixed-point vertex stage"
#else
  CHECK_EQ(wrong, 0);
#endif
}

// Two render contexts: the halves of the frame rendered at the same time on
// two threads match a render in one call byte for byte
static void testRenderContexts() {
  g3::Config cfg = g3::defaultConfig(W, H, arena, sizeof(arena));
  cfg.renderContexts = 2;
  g3::Graphics3D r;
  r.init(cfg);
  CHECK(r.isInitialized());
  g2::OwnedSurface whole = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  g2::OwnedSurface split = g2::createSurface(g2::PixelFormat::RGB565BE, W, H);
  r.setClearColor({0.1f, 0.2f, 0.3f, 1});
  for (int i = 0; i < 4; i++) {
    buildScene(r, &M_RED, 0.3f + 0.9f * (float)i);
    r.beginRender();
    r.render(0, 0, W, H, whole);
    std::thread t1(
        [&] { r.render(1, 0, H / 2, W, H - H / 2, split, 0, H / 2); });
    r.render(0, 0, 0, W, H / 2, split);
    t1.join();
    r.render(2, 0, 0, W, H, split);  // no such context: draws nothing
    r.endRender();
    CHECK(std::memcmp(whole.pixels(), split.pixels(), whole.bytes()) == 0);
  }
  CHECK(r.getStats().spanPeak > 0);
  CHECK_EQ(r.getStats().spanDropped, 0);
}

void testGfx3D() {
  genTextures();
#if SHAPOGFX3D_LINES && SHAPOGFX3D_POINTS
  testPointsAndLines();
#endif
  testShapeWinding();
  testVertexColorAndIndices();
  testBandsAndClear();
  testOutputFormats();
  testLayersAndConfig();
  testWatertight();
  testFarVertex();
  testRenderContexts();
#if SHAPOGFX3D_TEXTURE && SHAPOGFX3D_BLEND
  testTextureAlpha();
#endif
}
