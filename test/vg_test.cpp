// Vector graphics (vg.hpp): paths, fill rules, antialiasing, brushes,
// strokes, pictures, drawing without an arena, and vector attachments of rig

#include <cmath>
#include <cstdint>
#include <cstring>

#include "check.hpp"
#include "shapoco/gfx2d/fonts.hpp"
#include "shapoco/gfx2d/gfx2d.hpp"
#include "shapoco/gfx2d/rig.hpp"
#include "shapoco/gfx2d/surface_alloc.hpp"

using namespace shapoco::gfx2d;
namespace rig = shapoco::gfx2d::rig;

namespace {

constexpr float PI = 3.14159265f;

alignas(8) uint8_t vgArena[32768];

Graphics2D makeContext(const Surface &s, bool arena = true) {
  Graphics2D g(s);
  if (arena) g.init(vgArena, sizeof vgArena);
  g.clear(Colors::BLACK);
  return g;
}

bool samePixels(const OwnedSurface &a, const OwnedSurface &b) {
  return std::memcmp(a.pixels(), b.pixels(), a.bytes()) == 0;
}

int differingPixels(const Graphics2D &a, const Graphics2D &b) {
  int n = 0;
  for (int y = 0; y < a.bounds().height; y++)
    for (int x = 0; x < a.bounds().width; x++)
      if (a.getPixel(x, y, false) != b.getPixel(x, y, false)) n++;
  return n;
}

int countColor(const Graphics2D &g, Color c) {
  int n = 0;
  for (int y = 0; y < g.bounds().height; y++)
    for (int x = 0; x < g.bounds().width; x++)
      if (g.getPixel(x, y, false) == c) n++;
  return n;
}

// Brightness 0..255 of a pixel
int lum(const Graphics2D &g, int x, int y) {
  const Color c = g.getPixel(x, y, false);
  return (colorR(c) + colorG(c) + colorB(c)) / 3;
}

bool near(int v, int expected, int tol) { return std::abs(v - expected) <= tol; }

// A five-pointed star around (cx, cy)
vg::Path starPath(uint8_t *ops, float *co, float cx, float cy, float r,
                  vg::FillRule rule) {
  vg::PathBuilder pb(ops, 8, co, 16);
  for (int i = 0; i < 5; i++) {
    const float a = -PI / 2 + (float)i * 4.0f * PI / 5.0f;
    const float x = cx + r * std::cos(a), y = cy + r * std::sin(a);
    if (i == 0) pb.moveTo(x, y);
    else pb.lineTo(x, y);
  }
  pb.close();
  return pb.path(rule);
}

// --- Paths ------------------------------------------------------------------

void testPathBuilder() {
  uint8_t ops[8];
  float co[16];
  vg::PathBuilder pb(ops, 8, co, 16);
  pb.rect(1, 2, 3, 4);
  CHECK_EQ(pb.opCount(), 5);
  CHECK_EQ(pb.coordCount(), 8);
  CHECK(!pb.overflowed());
  vg::Path p = pb.path(vg::FillRule::EVEN_ODD);
  CHECK((vg::PathOp)p.ops[0] == vg::PathOp::MOVE);
  CHECK((vg::PathOp)p.ops[3] == vg::PathOp::LINE);
  CHECK((vg::PathOp)p.ops[4] == vg::PathOp::CLOSE);
  CHECK(p.rule == vg::FillRule::EVEN_ODD);
  CHECK(p.bounds.x == 1.0f && p.bounds.y == 2.0f && p.bounds.width == 3.0f &&
        p.bounds.height == 4.0f);
  CHECK_EQ(vg::pathOpCoords(vg::PathOp::CUBIC), 6);
  CHECK_EQ(vg::pathOpCoords(vg::PathOp::QUAD), 4);
  CHECK_EQ(vg::pathOpCoords(vg::PathOp::CLOSE), 0);
  // Beyond the capacity: dropped, flagged
  pb.lineTo(9, 9).lineTo(9, 9).lineTo(9, 9).lineTo(9, 9).lineTo(9, 9);
  CHECK(pb.overflowed());
  CHECK_EQ(pb.opCount(), 8);
  CHECK_EQ(pb.coordCount(), 14);
  // An ellipse is a move, four cubics and a close
  uint8_t ops2[8];
  float co2[32];
  vg::PathBuilder pe(ops2, 8, co2, 32);
  pe.circle(5, 5, 3);
  CHECK_EQ(pe.opCount(), 6);
  CHECK_EQ(pe.coordCount(), 26);
  const vg::Path e = pe.path();
  CHECK(near((int)e.bounds.x, 2, 0) && near((int)e.bounds.width, 6, 0));
  // Bounds computed when a hand-made path leaves them empty
  vg::Path h = e;
  h.bounds = {0, 0, 0, 0};
  const RectF b = vg::pathBounds(h);
  CHECK(b.x == 2.0f && b.y == 2.0f && b.width == 6.0f && b.height == 6.0f);
}

// Without antialiasing a rectangle path fills what fillRect() fills, under
// every kind of transform; a turned one what fillPolygon() fills
void testPathFillMatchesRect() {
  const RectF rects[] = {{3, 4, 10, 7}, {2.5f, 3.25f, 9.5f, 6.75f},
                         {0, 0, 40, 30}, {-5, -5, 20, 12}, {30, 20, 30, 30}};
  const affine2f transforms[] = {
      affine2f::identity(), affine2f::translation(0.3f, -1.7f),
      affine2f::translation(7, 3), affine2f::scaling(1.5f, 0.75f),
      affine2f::scaling(-1.0f, 2.0f).translate(-20, 0)};
  for (const RectF &r : rects) {
    for (const affine2f &m : transforms) {
      OwnedSurface sa(PixelFormat::RGB565_SWAPPED, 40, 30),
          sb(PixelFormat::RGB565_SWAPPED, 40, 30);
      Graphics2D ga = makeContext(sa), gb = makeContext(sb);
      ga.setTransform(m);
      gb.setTransform(m);
      ga.fillRect(r, Colors::WHITE);
      uint8_t ops[8];
      float co[16];
      vg::PathBuilder pb(ops, 8, co, 16);
      pb.rect(r.x, r.y, r.width, r.height);
      gb.setAntialias(false);
      gb.setFillColor(Colors::WHITE);
      gb.fillPath(pb.path());
      CHECK(samePixels(sa, sb));
    }
  }
#if SHAPOGFX2D_TRANSFORM
  // Turned: a pixel is filled where its center is inside (the polygon API
  // would snap the vertices to pixel centers, so the rule is checked
  // directly; centers within 1/20 pixel of an edge are left out)
  for (int k = 0; k < 4; k++) {
    OwnedSurface sb(PixelFormat::RGB565_SWAPPED, 64, 48);
    Graphics2D gb = makeContext(sb);
    const affine2f m = affine2f::placement(32, 24, 0.3f + 0.5f * (float)k, 1.2f, 0.8f);
    gb.setTransform(m);
    uint8_t ops[8];
    float co[16];
    vg::PathBuilder pb(ops, 8, co, 16);
    pb.rect(-15, -10, 30, 20);
    gb.setAntialias(false);
    gb.setFillColor(Colors::WHITE);
    gb.fillPath(pb.path());
    affine2f inv;
    CHECK(m.invert(inv));
    int bad = 0, filled = 0;
    for (int y = 0; y < 48; y++)
      for (int x = 0; x < 64; x++) {
        const vec2f u = inv.apply((float)x + 0.5f, (float)y + 0.5f);
        const float dx = 15.0f - std::fabs(u.x), dy = 10.0f - std::fabs(u.y);
        const bool in = gb.getPixel(x, y, false) == Colors::WHITE;
        filled += in;
        if (std::fabs(dx) < 0.05f || std::fabs(dy) < 0.05f) continue;
        if (in != (dx > 0.0f && dy > 0.0f)) bad++;
      }
    CHECK_EQ(bad, 0);
    CHECK(near(filled, 576, 40));  // 30 x 20 x 1.2 x 0.8
  }
#endif
  // Curves: a circle path against fillEllipse(), within the pixels along
  // the outline
  {
    OwnedSurface sa(PixelFormat::RGB565_SWAPPED, 64, 64),
        sb(PixelFormat::RGB565_SWAPPED, 64, 64);
    Graphics2D ga = makeContext(sa), gb = makeContext(sb);
    ga.fillEllipse(RectF{12, 12, 40, 40}, Colors::WHITE);
    uint8_t ops[8];
    float co[32];
    vg::PathBuilder pb(ops, 8, co, 32);
    pb.circle(32, 32, 20);
    gb.setAntialias(false);
    gb.setFillColor(Colors::WHITE);
    gb.fillPath(pb.path());
    const int d = differingPixels(ga, gb);
    CHECK(d <= 40);  // the circumference is 126 pixels
    CHECK(near(countColor(gb, Colors::WHITE), 1257, 40));  // pi r^2
  }
}

void testFillRules() {
  uint8_t ops[16];
  float co[32];
  OwnedSurface s(PixelFormat::RGB565_SWAPPED, 64, 64);
  Graphics2D g = makeContext(s);
  g.setAntialias(false);
  g.setFillColor(Colors::WHITE);
  g.fillPath(starPath(ops, co, 32, 32, 28, vg::FillRule::NONZERO));
  CHECK(g.getPixel(32, 32, false) == Colors::WHITE);  // the center: 2 windings
  CHECK(g.getPixel(32, 8, false) == Colors::WHITE);   // a point
  g.clear(Colors::BLACK);
  g.fillPath(starPath(ops, co, 32, 32, 28, vg::FillRule::EVEN_ODD));
  CHECK(g.getPixel(32, 32, false) == Colors::BLACK);
  CHECK(g.getPixel(32, 8, false) == Colors::WHITE);
  // A hole: a square inside a square, the inner one the other way round,
  // is a hole with both rules; the same way round it is a hole only with
  // even-odd
  for (int same = 0; same < 2; same++) {
    for (int rule = 0; rule < 2; rule++) {
      vg::PathBuilder pb(ops, 16, co, 32);
      pb.rect(10, 10, 40, 40);
      if (same) pb.rect(20, 20, 20, 20);
      else pb.moveTo(20, 20).lineTo(20, 40).lineTo(40, 40).lineTo(40, 20).close();
      g.clear(Colors::BLACK);
      g.fillPath(pb.path(rule ? vg::FillRule::EVEN_ODD : vg::FillRule::NONZERO));
      const bool hole = rule == 1 || !same;
      CHECK((g.getPixel(30, 30, false) == Colors::BLACK) == hole);
      CHECK(g.getPixel(15, 15, false) == Colors::WHITE);
    }
  }
}

void testAntialias() {
  uint8_t ops[8];
  float co[16];
  vg::PathBuilder pb(ops, 8, co, 16);
  pb.rect(10.5f, 10.5f, 20, 20);
  const vg::Path p = pb.path();
  OwnedSurface s(PixelFormat::RGB565_SWAPPED, 48, 48);
  Graphics2D g = makeContext(s);
  g.setFillColor(Colors::WHITE);
  CHECK(!g.antialias());  // off by default
  g.setAntialias(true);
  g.fillPath(p);
  if (SHAPOGFX2D_ANTIALIAS) {
    // Corners a quarter covered, edges half, the inside whole
    CHECK(near(lum(g, 10, 10), 64, 14));
    CHECK(near(lum(g, 11, 10), 128, 14));
    CHECK(near(lum(g, 10, 11), 128, 14));
    CHECK_EQ(lum(g, 11, 11), 255);
    CHECK_EQ(lum(g, 20, 20), 255);
    CHECK(near(lum(g, 30, 20), 128, 14));
    CHECK(near(lum(g, 30, 30), 64, 14));
    CHECK_EQ(lum(g, 31, 31), 0);
    CHECK_EQ(lum(g, 9, 9), 0);
    // A thin slanted stroke covers a pixel partially but never fully
    g.clear(Colors::BLACK);
    g.setStrokeColor(Colors::WHITE);
    g.setStrokeWidth(0.5f);
    const vec2f line[2] = {{5, 5}, {40, 20}};
    g.strokePolyline(line, 2);
    int partial = 0, full = 0;
    for (int y = 0; y < 48; y++)
      for (int x = 0; x < 48; x++) {
        const int l = lum(g, x, y);
        if (l == 255) full++;
        else if (l > 0) partial++;
      }
    CHECK_EQ(full, 0);
    CHECK(partial > 30);
  }
  // Off: every pixel is black or white
  g.clear(Colors::BLACK);
  g.setAntialias(false);
  g.fillPath(p);
  CHECK_EQ(countColor(g, Colors::WHITE) + countColor(g, Colors::BLACK), 48 * 48);
  CHECK_EQ(countColor(g, Colors::WHITE), 20 * 20);
#if SHAPOGFX2D_BLEND
  // The NONE blend mode copies: no antialiasing either
  g.clear(Colors::BLACK);
  g.setAntialias(true);
  g.setBlendMode(BlendMode::NONE);
  g.fillPath(p);
  CHECK_EQ(countColor(g, Colors::WHITE) + countColor(g, Colors::BLACK), 48 * 48);
  g.setBlendMode(BlendMode::ALPHA);
#endif
  // Antialiasing on every format
  const PixelFormat fmts[] = {
#if SHAPOGFX_FORMAT_GRAY1
      PixelFormat::GRAY1,
#endif
#if SHAPOGFX_FORMAT_RGB444
      PixelFormat::RGB444,
#endif
#if SHAPOGFX_FORMAT_ARGB4444
      PixelFormat::ARGB4444,
#endif
#if SHAPOGFX_FORMAT_RGB565
      PixelFormat::RGB565,
#endif
  };
  for (PixelFormat f : fmts) {
    OwnedSurface t(f, 48, 48);
    Graphics2D h = makeContext(t);
    h.setAntialias(true);
    h.setFillColor(Colors::WHITE);
    h.fillPath(p);
    CHECK(h.getPixel(20, 20, false) == Colors::WHITE);
    CHECK(h.getPixel(5, 5, false) == Colors::BLACK);
    if (f != PixelFormat::GRAY1 && SHAPOGFX2D_ANTIALIAS)
      CHECK(near(lum(h, 11, 10), 128, 20));
  }
}

// With antialiasing on, the area fills go through the path rasterizer: the
// same pixels as the path of the same shape
void testAreaFillsAntialiased() {
  OwnedSurface sa(PixelFormat::RGB565_SWAPPED, 64, 64),
      sb(PixelFormat::RGB565_SWAPPED, 64, 64);
  Graphics2D ga = makeContext(sa), gb = makeContext(sb);
  ga.setAntialias(true);
  gb.setAntialias(true);
  gb.setFillColor(0xC0FFFFFF);
  uint8_t ops[16];
  float co[64];
  // A float rectangle
  ga.fillRect(RectF{10.5f, 10.25f, 20, 20}, 0xC0FFFFFF);
  vg::PathBuilder pr(ops, 16, co, 64);
  pr.rect(10.5f, 10.25f, 20, 20);
  gb.fillPath(pr.path());
  // (without SHAPOGFX2D_ANTIALIAS the fills keep their own code, which may
  // differ by a pixel from the path rasterizer: the identities are for the
  // antialiased build)
  const bool same = SHAPOGFX2D_ANTIALIAS != 0;
  if (same) CHECK(samePixels(sa, sb));
  if (same) CHECK(lum(ga, 10, 15) > 0 && lum(ga, 10, 15) < 200);
  // An integer rectangle under a rotation
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(Colors::BLACK);
    g->setTransform(affine2f::rotation(0.4f, 32, 32));
  }
  ga.fillRect(10, 10, 30, 20, Colors::WHITE);
  vg::PathBuilder pi(ops, 16, co, 64);
  pi.rect(10, 10, 30, 20);
  gb.setFillColor(Colors::WHITE);
  gb.fillPath(pi.path());
  if (same) CHECK(samePixels(sa, sb));
  // ... but not without one: whole pixels, the plain fill
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(Colors::BLACK);
    g->setTransform(affine2f::translation(3, 2));
  }
  ga.fillRect(10, 10, 30, 20, Colors::WHITE);
  gb.setAntialias(false);
  gb.fillRect(10, 10, 30, 20, Colors::WHITE);
  gb.setAntialias(true);
  CHECK(samePixels(sa, sb));
  // Ellipses, circles and rounded rectangles
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(Colors::BLACK);
    g->resetTransform();
  }
  ga.fillEllipse(RectF{8, 8, 40, 30}, Colors::WHITE);
  ga.fillCircle(50, 50, 8, Colors::RED);
  ga.fillRoundRect(RectF{2, 44, 30, 18}, 5, Colors::GREEN);
  vg::PathBuilder pe(ops, 16, co, 64);
  pe.ellipse(28, 23, 20, 15);
  gb.fillPath(pe.path());
  vg::PathBuilder pc(ops, 16, co, 64);
  pc.ellipse(50.5f, 50.5f, 8.5f, 8.5f);  // fillCircle(): 2 r + 1 wide
  gb.setFillColor(Colors::RED);
  gb.fillPath(pc.path());
  vg::PathBuilder pq(ops, 16, co, 64);
  pq.roundRect(2, 44, 30, 18, 5, 5);
  gb.setFillColor(Colors::GREEN);
  gb.fillPath(pq.path());
  if (same) CHECK(samePixels(sa, sb));
  if (same) CHECK(lum(ga, 8, 23) > 0 && lum(ga, 8, 23) < 255);
  // Polygons: the vertices are pixels, their centers the corners of the path
  for (Graphics2D *g : {&ga, &gb}) g->clear(Colors::BLACK);
  const vec2f tri[3] = {{5, 5}, {40, 12}, {20, 40}};
  const vec2i quad[4] = {{30, 30}, {60, 32}, {58, 60}, {33, 55}};
  ga.fillPolygon(tri, 3, Colors::WHITE);
  ga.fillPolygon(quad, 4, 0x80FF0000);
  vg::PathBuilder pt(ops, 16, co, 64);
  pt.moveTo(5.5f, 5.5f).lineTo(40.5f, 12.5f).lineTo(20.5f, 40.5f).close();
  gb.setFillColor(Colors::WHITE);
  gb.fillPath(pt.path());
  vg::PathBuilder pp(ops, 16, co, 64);
  pp.moveTo(30.5f, 30.5f).lineTo(60.5f, 32.5f).lineTo(58.5f, 60.5f).lineTo(33.5f, 55.5f).close();
  gb.setFillColor(0x80FF0000);
  gb.fillPath(pp.path());
  if (same) CHECK(samePixels(sa, sb));
  // Off (or the NONE blend mode): the plain fills, no gray
  ga.clear(Colors::BLACK);
  ga.setAntialias(false);
  ga.fillEllipse(RectF{8, 8, 40, 30}, Colors::WHITE);
  ga.fillPolygon(tri, 3, Colors::WHITE);
  CHECK_EQ(countColor(ga, Colors::WHITE) + countColor(ga, Colors::BLACK), 64 * 64);
#if SHAPOGFX2D_BLEND
  ga.clear(Colors::BLACK);
  ga.setAntialias(true);
  ga.setBlendMode(BlendMode::NONE);
  ga.fillEllipse(RectF{8, 8, 40, 30}, Colors::WHITE);
  CHECK_EQ(countColor(ga, Colors::WHITE) + countColor(ga, Colors::BLACK), 64 * 64);
  ga.setBlendMode(BlendMode::ALPHA);
#endif
}

// With antialiasing on, lines and outlines are strokes a pixel wide on the
// target, sectors and frames paths
void testOutlinesAntialiased() {
  OwnedSurface sa(PixelFormat::RGB565_SWAPPED, 64, 64),
      sb(PixelFormat::RGB565_SWAPPED, 64, 64);
  Graphics2D ga = makeContext(sa), gb = makeContext(sb);
  ga.setAntialias(true);
  // Axis-aligned lines cover the same pixels as without (the square caps
  // reach the end pixels)
  ga.drawLine(10, 20, 50, 20, Colors::WHITE);
  ga.drawLine(30, 5, 30, 60, Colors::WHITE);
  ga.drawHLine(5, 40, 20, Colors::WHITE);
  gb.drawLine(10, 20, 50, 20, Colors::WHITE);
  gb.drawLine(30, 5, 30, 60, Colors::WHITE);
  gb.drawHLine(5, 40, 20, Colors::WHITE);
  CHECK(samePixels(sa, sb));
  // A diagonal one is a smooth line (partial pixels) along the same way
  for (Graphics2D *g : {&ga, &gb}) g->clear(Colors::BLACK);
  ga.drawLine(5, 5, 50, 30, Colors::WHITE);
  gb.drawLine(5, 5, 50, 30, Colors::WHITE);
  if (SHAPOGFX2D_ANTIALIAS) {
    int partial = 0, bad = 0;
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
        const int l = lum(ga, x, y);
        if (l > 0 && l < 255) partial++;
        // Every lit pixel is within a pixel of the plain line
        if (l > 0) {
          bool nearLine = false;
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
              if (gb.getPixel(x + dx, y + dy, false) == Colors::WHITE) nearLine = true;
          if (!nearLine) bad++;
        }
      }
    CHECK(partial > 20);
    CHECK_EQ(bad, 0);
    CHECK(ga.getPixel(5, 5, false) != Colors::BLACK);  // the end points
    CHECK(ga.getPixel(50, 30, false) != Colors::BLACK);
  }
  // A polygon outline: the same as the polyline plus the closing line, and
  // an axis-aligned rectangle's outline is its plain one
  for (Graphics2D *g : {&ga, &gb}) g->clear(Colors::BLACK);
  const vec2i box[4] = {{10, 10}, {50, 10}, {50, 40}, {10, 40}};
  ga.drawPolygon(box, 4, Colors::WHITE);
  gb.drawPolygon(box, 4, Colors::WHITE);
  CHECK(samePixels(sa, sb));
  CHECK_EQ(countColor(ga, Colors::WHITE), 41 * 2 + 29 * 2);
  // Frames: an integer frame under a translation stays plain, a float one
  // is the even-odd path of its two rectangles
  for (Graphics2D *g : {&ga, &gb}) g->clear(Colors::BLACK);
  ga.drawRect(5, 5, 30, 20, Colors::WHITE, 3);
  gb.drawRect(5, 5, 30, 20, Colors::WHITE, 3);
  CHECK(samePixels(sa, sb));
  for (Graphics2D *g : {&ga, &gb}) g->clear(Colors::BLACK);
  ga.drawRect(RectF{5.5f, 5.5f, 30, 20}, Colors::WHITE, 3.0f);
  uint8_t ops[16];
  float co[64];
  vg::PathBuilder pf(ops, 16, co, 64);
  pf.rect(5.5f, 5.5f, 30, 20).rect(8.5f, 8.5f, 24, 14);
  gb.setAntialias(true);
  gb.setFillColor(Colors::WHITE);
  gb.fillPath(pf.path(vg::FillRule::EVEN_ODD));
  CHECK(samePixels(sa, sb));
  CHECK(ga.getPixel(20, 15, false) == Colors::BLACK);  // the hole
  gb.setAntialias(false);
  // Ellipse outlines lie within the fill, a pixel wide, with no pixel
  // beyond the plain outline's neighbors
  for (Graphics2D *g : {&ga, &gb}) g->clear(Colors::BLACK);
  ga.drawEllipse(RectF{8, 8, 48, 36}, Colors::WHITE);
  gb.fillEllipse(RectF{8, 8, 48, 36}, Colors::WHITE);
  // (a partially covered pixel may have its center just outside the
  // ellipse: those are dim and next to the fill)
  auto nextToFill = [&](int x, int y) {
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++)
        if (gb.getPixel(x + dx, y + dy, false) == Colors::WHITE) return true;
    return false;
  };
  int outside = 0, lit = 0;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      const int l = lum(ga, x, y);
      if (l == 0) continue;
      lit++;
      if (gb.getPixel(x, y, false) != Colors::WHITE && !nextToFill(x, y)) outside++;
    }
  CHECK_EQ(outside, 0);
  CHECK(lit > 100 && lit < 300);
  CHECK(ga.getPixel(32, 26, false) == Colors::BLACK);  // the inside is empty
  // Arcs: a quarter turn lights only its quadrant
  ga.clear(Colors::BLACK);
  ga.drawArc(RectF{8, 8, 48, 48}, 0.0f, PI / 2, Colors::WHITE);
  int quad = 0, other = 0;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      if (lum(ga, x, y) == 0) continue;
      if (x >= 32 && y >= 32) quad++;
      else other++;
    }
  CHECK(quad > 20);
  CHECK(other <= 2);
  // Sectors: the pie of the quadrant; a full turn is the ellipse
  ga.clear(Colors::BLACK);
  gb.clear(Colors::BLACK);
  ga.fillSector(RectF{8, 8, 48, 48}, 0.0f, PI / 2, Colors::WHITE);
  CHECK(ga.getPixel(40, 40, false) == Colors::WHITE);
  CHECK(ga.getPixel(40, 24, false) == Colors::BLACK);
  CHECK(ga.getPixel(24, 40, false) == Colors::BLACK);
  ga.clear(Colors::BLACK);
  ga.fillSector(RectF{8, 8, 48, 48}, 0.0f, 2 * PI, Colors::WHITE);
  gb.setAntialias(true);
  gb.fillEllipse(RectF{8, 8, 48, 48}, Colors::WHITE);
  CHECK(samePixels(sa, sb));
  // A rounded rectangle outline lies within its fill
  ga.clear(Colors::BLACK);
  gb.clear(Colors::BLACK);
  ga.drawRoundRect(RectF{5, 5, 50, 30}, 8, Colors::WHITE);
  gb.setAntialias(false);
  gb.fillRoundRect(RectF{5, 5, 50, 30}, 8, Colors::WHITE);
  outside = 0;  // (the plain corners round their own way: neighbors count)
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++)
      if (lum(ga, x, y) > 0 && !nextToFill(x, y)) outside++;
  CHECK_EQ(outside, 0);
  CHECK(ga.getPixel(30, 20, false) == Colors::BLACK);
  CHECK(ga.getPixel(30, 5, false) != Colors::BLACK);
#if SHAPOGFX2D_TRANSFORM
  // Under a scale the lines and outlines stay a pixel wide
  ga.clear(Colors::BLACK);
  ga.setTransform(affine2f::scaling(3.0f));
  ga.drawLine(2, 5, 18, 5, Colors::WHITE);
  ga.resetTransform();
  CHECK(ga.getPixel(30, 16, false) == Colors::WHITE);
  CHECK(ga.getPixel(30, 14, false) == Colors::BLACK);
  CHECK(ga.getPixel(30, 18, false) == Colors::BLACK);
  ga.clear(Colors::BLACK);
  ga.setTransform(affine2f::scaling(4.0f));
  ga.drawCircle(8, 8, 6, Colors::WHITE);
  ga.resetTransform();
  int wide = 0;  // the lit pixels along the row through the center
  for (int x = 0; x < 64; x++)
    if (lum(ga, x, 34) > 0) wide++;
  CHECK(wide >= 2 && wide <= 6);
#endif
}

// Text and bitmaps under a transform sample the mask 2 x 2 per pixel
void testMaskAntialiased() {
#if SHAPOGFX2D_TRANSFORM && SHAPOGFX_FORMAT_GRAY1
  OwnedSurface sa(PixelFormat::RGB565_SWAPPED, 96, 48),
      sb(PixelFormat::RGB565_SWAPPED, 96, 48);
  Graphics2D ga = makeContext(sa), gb = makeContext(sb);
  for (Graphics2D *g : {&ga, &gb}) {
    g->setFont(&ShapoSansP_s08c07);
    g->setTextColor(Colors::WHITE);
  }
  ga.setAntialias(true);
  // Untransformed: the same pixels (nothing to sample)
  ga.drawString(2, 2, "Ab");
  gb.drawString(2, 2, "Ab");
  CHECK(samePixels(sa, sb));
  // Scaled by a fraction: gray along the edges, the same shape (every lit
  // pixel of the sampled text is within a pixel of the plain scaled text
  // and the plain pixels are lit); at a whole scale the samples fall into
  // the same texel and nothing is gray
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(Colors::BLACK);
    g->setTransform(affine2f::scaling(2.5f));
    g->drawString(1, 1, "Ab");
    g->resetTransform();
  }
  int partial = 0, bad = 0, missing = 0;
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 96; x++) {
      const int l = lum(ga, x, y);
      if (l > 0 && l < 255) partial++;
      const bool plain = gb.getPixel(x, y, false) == Colors::WHITE;
      auto nearOther = [&](const Graphics2D &o) {
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++)
            if (lum(o, x + dx, y + dy) > 0) return true;
        return false;
      };
      if (plain && l == 0 && !nearOther(ga)) missing++;
      if (l > 0 && !plain && !nearOther(gb)) bad++;
    }
  if (SHAPOGFX2D_ANTIALIAS) CHECK(partial > 10);
  CHECK_EQ(bad, 0);
  CHECK_EQ(missing, 0);
  // A bitmap with a background: both colors, through a rotation
  OwnedSurface bits(PixelFormat::GRAY1, 8, 8);
  Graphics2D gm(bits);
  gm.clear(Colors::BLACK);
  gm.fillRect(2, 2, 4, 4, Colors::WHITE);
  const Texture bmp = bits;
  ga.clear(Colors::BLACK);
  ga.setTransform(affine2f::placement(48, 24, 0.5f, 3.0f, 3.0f, 4, 4));
  ga.drawBitmap(bmp, 0, 0, Colors::RED, Colors::BLUE);
  ga.resetTransform();
  CHECK(ga.getPixel(48, 24, false) == Colors::RED);
  CHECK(countColor(ga, Colors::BLUE) > 50);
  CHECK(countColor(ga, Colors::RED) > 50);
  if (SHAPOGFX2D_ANTIALIAS) {
    int mixed = 0;  // pixels that are neither of the colors nor black
    for (int y = 0; y < 48; y++)
      for (int x = 0; x < 96; x++) {
        const Color c = ga.getPixel(x, y, false);
        if (c != Colors::RED && c != Colors::BLUE && c != Colors::BLACK) mixed++;
      }
    CHECK(mixed > 10);
  }
#endif
}

// Images under a transform with antialiasing: the edges by coverage, the
// pixels sampled bilinearly
void testImagesAntialiased() {
#if SHAPOGFX2D_TRANSFORM
  OwnedSurface sa(PixelFormat::RGB565_SWAPPED, 64, 64),
      sb(PixelFormat::RGB565_SWAPPED, 64, 64);
  Graphics2D ga = makeContext(sa), gb = makeContext(sb);
  ga.setAntialias(true);
  gb.setAntialias(true);
  // A solid image turned: the same pixels as the antialiased quadrilateral
  // (bilinear sampling of one color is that color)
  OwnedSurface solid(PixelFormat::RGB565_SWAPPED, 16, 12);
  Graphics2D gs(solid);
  gs.clear(Colors::WHITE);
  const Texture white = solid;
  const affine2f m = affine2f::placement(32, 32, 0.4f, 1.5f, 1.5f, 8, 6);
  ga.setTransform(m);
  ga.drawImage(white, 0, 0);
  ga.resetTransform();
  uint8_t ops[8];
  float co[16];
  vg::PathBuilder pb(ops, 8, co, 16);
  pb.rect(0, 0, 16, 12);
  gb.setTransform(m);
  gb.setFillColor(Colors::WHITE);
  gb.fillPath(pb.path());
  gb.resetTransform();
  // (without SHAPOGFX2D_ANTIALIAS the images keep their nearest-neighbor
  // path: the identities and the smoothness are for the antialiased build.
  // The image's coverage is approximated from its edges: exact along an
  // edge, a product at the corners, so the edge pixels may differ a little
  // from the rasterized quadrilateral, the others not at all)
  const bool aa = SHAPOGFX2D_ANTIALIAS != 0;
  auto nearlySame = [&]() {
    int off = 0;
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
        const int a = lum(ga, x, y), b = lum(gb, x, y);
        const bool edge = b > 0 && b < 255;
        if (std::abs(a - b) > (edge ? 96 : 24)) off++;
      }
    return off == 0;
  };
  if (aa) CHECK(nearlySame());
  if (aa) {
    int partial = 0;
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++)
        if (lum(ga, x, y) > 0 && lum(ga, x, y) < 255) partial++;
    CHECK(partial > 20);
  }
  // The polygon overload: the antialiased triangle
  const int16_t tri[6] = {0, 0, 16, 0, 0, 12};
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(Colors::BLACK);
    g->setTransform(m);
  }
  ga.drawImage(white, 0, 0, Rect{0, 0, 16, 12}, tri, 3);
  vg::PathBuilder pt(ops, 8, co, 16);
  pt.moveTo(0, 0).lineTo(16, 0).lineTo(0, 12).close();
  gb.fillPath(pt.path());
  for (Graphics2D *g : {&ga, &gb}) g->resetTransform();
  if (aa) CHECK(nearlySame());
  // A part of an atlas lands where the whole image would: the same as
  // drawing the part cut out as an image of its own
  OwnedSurface atlas(PixelFormat::RGB565_SWAPPED, 32, 32);
  Graphics2D gat(atlas);
  gat.clear(Colors::RED);
  gat.fillRect(20, 10, 8, 6, Colors::WHITE);
  gat.setPixel(21, 11, Colors::BLUE);
  OwnedSurface part(PixelFormat::RGB565_SWAPPED, 8, 6);
  Graphics2D gpt(part);
  gpt.clear(Colors::WHITE);
  gpt.setPixel(1, 1, Colors::BLUE);
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(Colors::BLACK);
    g->setTransform(affine2f::placement(30, 30, 0.3f, 3.0f, 3.0f, 4, 3));
  }
  ga.drawImage(atlas, 0, 0, Rect{20, 10, 8, 6});
  gb.drawImage(part, 0, 0);
  for (Graphics2D *g : {&ga, &gb}) g->resetTransform();
  CHECK(samePixels(sa, sb));
  CHECK(countColor(ga, Colors::RED) == 0);
  ga.clear(Colors::BLACK);
  gb.clear(Colors::BLACK);
  ga.setTransform(affine2f::placement(30, 30, 0.3f, 3.0f, 3.0f, 4, 3));
  gb.setTransform(affine2f::placement(30, 30, 0.3f, 3.0f, 3.0f, 4, 3));
  ga.drawImage(atlas, 0, 0, Rect{20, 10, 8, 6}, tri, 3);
  gb.drawImage(part, 0, 0, Rect{0, 0, 8, 6}, tri, 3);
  for (Graphics2D *g : {&ga, &gb}) g->resetTransform();
  CHECK(samePixels(sa, sb));
  // Bilinear sampling: a 2 x 2 checker scaled 8 x is a smooth gradient
  // between the texel centers (the corners keep their colors)
  OwnedSurface check(PixelFormat::RGB565_SWAPPED, 2, 2);
  Graphics2D gc(check);
  gc.clear(Colors::BLACK);
  gc.setPixel(0, 0, Colors::WHITE);
  gc.setPixel(1, 1, Colors::WHITE);
  const Texture checker = check;
  ga.clear(Colors::BLACK);
  ga.setTransform(affine2f::scaling(8.0f));
  ga.drawImage(checker, 2, 2);
  ga.resetTransform();
  // (the pixel centers are 1/16 texel off the texel centers)
  if (aa) CHECK(near(lum(ga, 20, 20), 255, 40));  // about the first texel's center
  if (aa) CHECK(near(lum(ga, 28, 20), 0, 40));
  if (aa) CHECK(near(lum(ga, 24, 20), 128, 16));  // halfway between
  if (aa) CHECK(near(lum(ga, 24, 24), 128, 16));
  bool monotonic = true;
  for (int x = 21; x <= 28; x++)
    if (lum(ga, x, 20) > lum(ga, x - 1, 20)) monotonic = false;
  CHECK(monotonic);
  // Under a translation (or none) the copy is plain, nothing to smooth
  gb.clear(Colors::BLACK);
  gb.drawImage(checker, 10, 10);
  CHECK(gb.getPixel(10, 10, false) == Colors::WHITE);
  CHECK(gb.getPixel(11, 10, false) == Colors::BLACK);
  // A keyed texel is transparent and lends no color to its neighbors
  OwnedSurface half(PixelFormat::RGB565_SWAPPED, 4, 4);
  Graphics2D gh(half);
  gh.clear(Colors::MAGENTA);
  gh.fillRect(0, 0, 2, 4, Colors::WHITE);
  const Texture keyed = half;
  ga.clear(Colors::BLACK);
  ga.setColorKey(Colors::MAGENTA);
  ga.setTransform(affine2f::scaling(6.0f));
  ga.drawImage(keyed, 1, 1);
  ga.resetTransform();
  ga.clearColorKey();
  int tinted = 0;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      const Color c = ga.getPixel(x, y, false);
      if (colorR(c) > colorG(c) + 24 || colorB(c) > colorG(c) + 24) tinted++;
    }
  CHECK_EQ(tinted, 0);
  CHECK(ga.getPixel(9, 20, false) == Colors::WHITE);
  CHECK(ga.getPixel(28, 20, false) == Colors::BLACK);
  if (aa) CHECK(lum(ga, 18, 20) > 0 && lum(ga, 18, 20) < 255);  // the keyed edge fades
  // An image with alpha: the alpha and the coverage multiply
#if SHAPOGFX_FORMAT_ARGB4444
  OwnedSurface tr(PixelFormat::ARGB4444, 8, 8);
  Graphics2D gt(tr);
  gt.setBlendMode(BlendMode::NONE);
  gt.clear(0x80FFFFFF);
  const Texture trans = tr;
  ga.clear(Colors::BLACK);
  ga.setTransform(affine2f::scaling(3.0f));
  ga.drawImage(trans, 2, 2);
  ga.resetTransform();
  CHECK(near(lum(ga, 15, 15), 128, 20));
  CHECK(ga.getPixel(40, 40, false) == Colors::BLACK);
#endif
  // Off: the plain transformed copy
  ga.clear(Colors::BLACK);
  ga.setAntialias(false);
  ga.setTransform(affine2f::scaling(8.0f));
  ga.drawImage(checker, 2, 2);
  ga.resetTransform();
  CHECK_EQ(countColor(ga, Colors::WHITE) + countColor(ga, Colors::BLACK), 64 * 64);
#endif
}

// --- Brushes ------------------------------------------------------------------

void testGradients() {
  uint8_t ops[8];
  float co[16];
  const vg::GradientStop bw[2] = {{0.0f, Colors::BLACK}, {1.0f, Colors::WHITE}};
  OwnedSurface s(PixelFormat::RGB565_SWAPPED, 80, 64);
  Graphics2D g = makeContext(s);
  g.setAntialias(false);
  // Linear, along x
  vg::Gradient lin = vg::linearGradient({0, 0}, {64, 0}, bw, 2);
  g.setFillBrush(vg::gradientBrush(&lin));
  vg::PathBuilder pb(ops, 8, co, 16);
  pb.rect(0, 0, 80, 8);
  g.fillPath(pb.path());
  CHECK(lum(g, 1, 4) < 24);
  CHECK(lum(g, 62, 4) > 230);
  CHECK(near(lum(g, 32, 4), 128, 16));
  bool monotonic = true;
  for (int x = 1; x < 64; x++)
    if (lum(g, x, 4) < lum(g, x - 1, 4)) monotonic = false;
  CHECK(monotonic);
  CHECK(lum(g, 0, 2) == lum(g, 0, 6));  // no change along y
  // PAD holds the end color beyond the end
  CHECK_EQ(lum(g, 75, 4), lum(g, 63, 4));
  // REPEAT: the same every period; REFLECT: mirrored
  vg::Gradient rep = vg::linearGradient({0, 0}, {16, 0}, bw, 2, vg::Spread::REPEAT);
  g.setFillBrush(vg::gradientBrush(&rep));
  vg::PathBuilder pr(ops, 8, co, 16);
  pr.rect(0, 10, 80, 8);
  g.fillPath(pr.path());
  for (int x = 0; x < 48; x++) CHECK_EQ(lum(g, x, 14), lum(g, x + 16, 14));
  CHECK(lum(g, 1, 14) < 40 && lum(g, 14, 14) > 200);
  vg::Gradient ref = vg::linearGradient({0, 0}, {16, 0}, bw, 2, vg::Spread::REFLECT);
  g.setFillBrush(vg::gradientBrush(&ref));
  vg::PathBuilder pf(ops, 8, co, 16);
  pf.rect(0, 20, 80, 8);
  g.fillPath(pf.path());
  for (int x = 0; x < 16; x++) CHECK_EQ(lum(g, x, 24), lum(g, 31 - x, 24));
  for (int x = 0; x < 48; x++) CHECK_EQ(lum(g, x, 24), lum(g, x + 32, 24));
  // Radial: dark at the center, bright at the radius, symmetric
  vg::Gradient rad = vg::radialGradient({40, 44}, 16, bw, 2);
  g.setFillBrush(vg::gradientBrush(&rad));
  vg::PathBuilder pc(ops, 8, co, 16);
  pc.rect(0, 28, 80, 36);
  g.fillPath(pc.path());
  CHECK(lum(g, 40, 44) < 40);
  CHECK(lum(g, 40, 29) > 230);
  CHECK(lum(g, 70, 44) > 230);
  // (pixel centers 8.5 from the center of the gradient)
  CHECK(near(lum(g, 31, 44), lum(g, 48, 44), 16));
  CHECK(near(lum(g, 40, 35), lum(g, 31, 44), 16));
  CHECK(near(lum(g, 48, 44), 128, 20));
#if SHAPOGFX2D_TRANSFORM
  // Three stops, with a color in the middle; the gradient's space turned
  // by the transform of the context
  const vg::GradientStop rgb[3] = {{0.0f, Colors::RED}, {0.5f, Colors::GREEN}, {1.0f, Colors::BLUE}};
  vg::Gradient tri = vg::linearGradient({0, 0}, {0, 40}, rgb, 3);
  g.clear(Colors::BLACK);
  g.setTransform(affine2f::rotation(PI / 2).translate(0, -80));  // y -> x
  g.setFillBrush(vg::gradientBrush(&tri));
  vg::PathBuilder pt(ops, 8, co, 16);
  pt.rect(0, 0, 64, 40);
  g.fillPath(pt.path());
  g.resetTransform();
  // The rectangle lands on x in [40, 80), y in [0, 64); the gradient runs
  // along x now, y = 0 at x = 80
  CHECK(colorB(g.getPixel(41, 10, false)) > 200);
  CHECK(colorG(g.getPixel(60, 10, false)) > 200);
  CHECK(colorR(g.getPixel(78, 10, false)) > 200);
  CHECK(g.getPixel(20, 10, false) == Colors::BLACK);
#endif
  // The brush's alpha scales the gradient: half of white over black (the
  // builders above share the arrays: the strip is built again)
  g.clear(Colors::BLACK);
  vg::PathBuilder pb2(ops, 8, co, 16);
  pb2.rect(0, 0, 80, 8);
  vg::Gradient ww = vg::linearGradient({0, 0}, {64, 0}, bw, 2);
  g.setFillBrush(vg::gradientBrush(&ww, 128));
  g.fillPath(pb2.path());
  CHECK(near(lum(g, 62, 4), 128, 16));
  // Every stop count and a gradient with no stops draws nothing
  vg::Gradient none = vg::linearGradient({0, 0}, {64, 0}, bw, 0);
  g.clear(Colors::BLACK);
  g.setFillBrush(vg::gradientBrush(&none));
  g.fillPath(pb2.path());
  CHECK_EQ(countColor(g, Colors::BLACK), 80 * 64);
#if SHAPOGFX2D_BLEND
  // ADD and NONE with a gradient
  g.setFillBrush(vg::gradientBrush(&lin));
  g.setBlendMode(BlendMode::ADD);
  g.fillPath(pb2.path());
  g.fillPath(pb2.path());
  CHECK(near(lum(g, 32, 4), 255, 8));  // twice half: saturated
  g.setBlendMode(BlendMode::NONE);
  g.fillPath(pb2.path());
  CHECK(near(lum(g, 32, 4), 128, 16));
  g.setBlendMode(BlendMode::ALPHA);
#endif
  // Antialiased gradient: a half-covered edge pixel is half the color
  if (SHAPOGFX2D_ANTIALIAS) {
    g.clear(Colors::BLACK);
    g.setAntialias(true);
    const vg::GradientStop wh[2] = {{0.0f, Colors::WHITE}, {1.0f, Colors::WHITE}};
    vg::Gradient flat = vg::linearGradient({0, 0}, {64, 0}, wh, 2);
    g.setFillBrush(vg::gradientBrush(&flat));
    vg::PathBuilder ph(ops, 8, co, 16);
    ph.rect(10.5f, 10, 20, 20);
    g.fillPath(ph.path());
    CHECK(near(lum(g, 10, 15), 128, 14));
    CHECK_EQ(lum(g, 15, 15), 255);
  }
}

// --- Strokes --------------------------------------------------------------------

void testStrokes() {
  OwnedSurface s(PixelFormat::RGB565_SWAPPED, 64, 48);
  Graphics2D g = makeContext(s);
  g.setAntialias(false);
  g.setStrokeColor(Colors::WHITE);
  const vec2f line[2] = {{10, 20}, {50, 20}};
  // Butt caps: exactly the rectangle of the width
  g.setStrokeStyle(vg::strokeStyle(4));
  g.strokePolyline(line, 2);
  CHECK_EQ(countColor(g, Colors::WHITE), 40 * 4);
  CHECK(g.getPixel(10, 18, false) == Colors::WHITE);
  CHECK(g.getPixel(49, 21, false) == Colors::WHITE);
  CHECK(g.getPixel(9, 20, false) == Colors::BLACK);
  CHECK(g.getPixel(50, 20, false) == Colors::BLACK);
  CHECK(g.getPixel(10, 17, false) == Colors::BLACK);
  CHECK(g.getPixel(10, 22, false) == Colors::BLACK);
  // Square caps: half the width further
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(4, vg::LineCap::SQUARE));
  g.strokePolyline(line, 2);
  CHECK_EQ(countColor(g, Colors::WHITE), 44 * 4);
  CHECK(g.getPixel(8, 20, false) == Colors::WHITE);
  CHECK(g.getPixel(51, 20, false) == Colors::WHITE);
  // Round caps: a half disc, so the corners of the square cap are left out
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(4, vg::LineCap::ROUND));
  g.strokePolyline(line, 2);
  CHECK(g.getPixel(8, 20, false) == Colors::WHITE);
  CHECK(g.getPixel(8, 18, false) == Colors::BLACK);
  CHECK(g.getPixel(51, 21, false) == Colors::BLACK);
  const int n = countColor(g, Colors::WHITE);
  CHECK(n > 40 * 4 && n < 44 * 4);
  // Joins at a right angle: miter fills the corner square, bevel cuts it
  const vec2f corner[3] = {{10, 10}, {40, 10}, {40, 40}};
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(6, vg::LineCap::BUTT, vg::LineJoin::MITER));
  g.strokePolyline(corner, 3);
  CHECK(g.getPixel(42, 8, false) == Colors::WHITE);
  CHECK(g.getPixel(40, 8, false) == Colors::WHITE);
  CHECK(g.getPixel(38, 12, false) == Colors::WHITE);  // the inside of the corner
  CHECK(g.getPixel(36, 8, false) == Colors::WHITE);
  const int miter = countColor(g, Colors::WHITE);
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(6, vg::LineCap::BUTT, vg::LineJoin::BEVEL));
  g.strokePolyline(corner, 3);
  CHECK(g.getPixel(42, 8, false) == Colors::BLACK);
  CHECK(g.getPixel(40, 8, false) == Colors::WHITE);
  CHECK(g.getPixel(38, 12, false) == Colors::WHITE);
  const int bevel = countColor(g, Colors::WHITE);
  CHECK(near(miter - bevel, 4, 2));  // half of the 3 x 3 corner
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(6, vg::LineCap::BUTT, vg::LineJoin::ROUND));
  g.strokePolyline(corner, 3);
  const int round = countColor(g, Colors::WHITE);
  CHECK(round > bevel && round <= miter);
  // A sharp angle beyond the miter limit is beveled
  const vec2f sharp[3] = {{10, 30}, {50, 32}, {10, 34}};
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(4, vg::LineCap::BUTT, vg::LineJoin::MITER, 4.0f));
  g.strokePolyline(sharp, 3);
  CHECK(g.getPixel(55, 32, false) == Colors::BLACK);
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(4, vg::LineCap::BUTT, vg::LineJoin::MITER, 100.0f));
  g.strokePolyline(sharp, 3);
  CHECK(g.getPixel(55, 32, false) == Colors::WHITE);
  // A closed polyline: joined at the start, no caps; the inside stays empty
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(2));
  const vec2f box[4] = {{10, 10}, {50, 10}, {50, 40}, {10, 40}};
  g.strokePolyline(box, 4, true);
  CHECK(g.getPixel(9, 9, false) == Colors::WHITE);  // the outer corner
  CHECK(g.getPixel(30, 25, false) == Colors::BLACK);
  CHECK(g.getPixel(30, 9, false) == Colors::WHITE);
  CHECK(g.getPixel(30, 11, false) == Colors::BLACK);
  CHECK_EQ(countColor(g, Colors::WHITE), 42 * 32 - 38 * 28);
  // Translucent: the union is painted once, joins included
  g.clear(Colors::BLACK);
  g.setStrokeColor(0x80FFFFFF);
  g.setStrokeStyle(vg::strokeStyle(6, vg::LineCap::ROUND, vg::LineJoin::ROUND));
  g.strokePolyline(corner, 3);
  int shades = 0;
  Color shade = 0;
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 64; x++) {
      const Color c = g.getPixel(x, y, false);
      if (c != Colors::BLACK && c != shade) {
        shade = c;
        shades++;
      }
    }
  CHECK_EQ(shades, 1);
  g.setStrokeColor(Colors::WHITE);
  // Width 0 or a brush without alpha: nothing
  g.clear(Colors::BLACK);
  g.setStrokeWidth(0.0f);
  g.strokePolyline(line, 2);
  g.setStrokeStyle(vg::strokeStyle(4));
  g.setStrokeBrush(vg::NO_BRUSH);
  g.strokePolyline(line, 2);
  CHECK_EQ(countColor(g, Colors::WHITE), 0);
  g.setStrokeColor(Colors::WHITE);
  // A zero-length subpath: a dot with round caps, a square with square
  // ones, nothing with butt ones (a lone MOVE draws nothing, as in SVG)
  uint8_t ops[4];
  float co[8];
  vg::PathBuilder pm(ops, 4, co, 8);
  pm.moveTo(30, 20);
  g.setStrokeStyle(vg::strokeStyle(6, vg::LineCap::ROUND));
  g.strokePath(pm.path());
  CHECK_EQ(countColor(g, Colors::WHITE), 0);
  vg::PathBuilder pp(ops, 4, co, 8);
  pp.moveTo(30, 20).lineTo(30, 20);
  g.setStrokeStyle(vg::strokeStyle(6, vg::LineCap::ROUND));
  g.strokePath(pp.path());
  CHECK(countColor(g, Colors::WHITE) > 20 && countColor(g, Colors::WHITE) < 36);
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(6, vg::LineCap::SQUARE));
  g.strokePath(pp.path());
  CHECK_EQ(countColor(g, Colors::WHITE), 36);
  g.clear(Colors::BLACK);
  g.setStrokeStyle(vg::strokeStyle(6, vg::LineCap::BUTT));
  g.strokePath(pp.path());
  CHECK_EQ(countColor(g, Colors::WHITE), 0);
#if SHAPOGFX2D_TRANSFORM
  // The width is in the coordinates of the calls: scaled by the transform,
  // non-uniformly too
  g.clear(Colors::BLACK);
  g.setTransform(affine2f::scaling(1.0f, 2.0f));
  g.setStrokeStyle(vg::strokeStyle(4));
  const vec2f half[2] = {{10, 10}, {50, 10}};
  g.strokePolyline(half, 2);
  g.resetTransform();
  CHECK_EQ(countColor(g, Colors::WHITE), 40 * 8);
#endif
  // A stroke with a gradient
  const vg::GradientStop bw[2] = {{0.0f, Colors::BLACK}, {1.0f, Colors::WHITE}};
  vg::Gradient lin = vg::linearGradient({10, 0}, {50, 0}, bw, 2);
  g.clear(Colors::BLACK);
  g.setStrokeBrush(vg::gradientBrush(&lin));
  g.strokePolyline(line, 2);
  CHECK(lum(g, 12, 20) < 40 && lum(g, 48, 20) > 215);
  g.setStrokeColor(Colors::WHITE);
#if SHAPOGFX2D_TRANSFORM
  // The box under a mirrored transform
  g.clear(Colors::BLACK);
  g.setTransform(affine2f::scaling(-1.0f, 1.0f).translate(-64, 0));
  g.setStrokeStyle(vg::strokeStyle(2));
  g.strokePolyline(box, 4, true);
  g.resetTransform();
  CHECK_EQ(countColor(g, Colors::WHITE), 42 * 32 - 38 * 28);
  CHECK(g.getPixel(64 - 10, 9, false) == Colors::WHITE);
#endif
}

// Curves are flattened for the transform in force: a circle scaled up is
// still round
void testFlattening() {
#if SHAPOGFX2D_TRANSFORM
  OwnedSurface s(PixelFormat::RGB565_SWAPPED, 128, 128);
  Graphics2D g = makeContext(s);
  g.setAntialias(false);
  g.setFillColor(Colors::WHITE);
  uint8_t ops[8];
  float co[32];
  vg::PathBuilder pb(ops, 8, co, 32);
  pb.circle(0, 0, 1);  // the unit circle
  g.setTransform(affine2f::translation(64, 64).scale(60));
  g.fillPath(pb.path());
  g.resetTransform();
  // Within a pixel of the true circle all around
  int bad = 0;
  for (int y = 0; y < 128; y++)
    for (int x = 0; x < 128; x++) {
      const float d = std::hypot((float)x + 0.5f - 64.0f, (float)y + 0.5f - 64.0f);
      const bool in = g.getPixel(x, y, false) == Colors::WHITE;
      if (d < 59.0f && !in) bad++;
      if (d > 61.0f && in) bad++;
    }
  CHECK_EQ(bad, 0);
  CHECK(near(countColor(g, Colors::WHITE), 11310, 120));
  // Quadratic curves too: the control point pulls the curve a quarter of
  // the way
  g.clear(Colors::BLACK);
  vg::PathBuilder pq(ops, 8, co, 32);
  pq.moveTo(10, 100).quadTo(64, 20, 118, 100).close();
  g.fillPath(pq.path());
  CHECK(g.getPixel(64, 61, false) == Colors::WHITE);  // y(0.5) = 60
  CHECK(g.getPixel(64, 58, false) == Colors::BLACK);
#endif
}

// --- Pictures ---------------------------------------------------------------------

void testPictures() {
  static const uint8_t rectOps[] = {0, 1, 1, 1, 4};
  static const float rectCo[] = {0, 0, 20, 0, 20, 10, 0, 10};
  static const vg::Path rect = {rectOps, rectCo, 5, 8, vg::FillRule::NONZERO,
                                {0, 0, 0}, {0, 0, 20, 10}};
  static const RectF clip = {0, 0, 30, 30};
  static const vg::Shape shapes[] = {
      // A red rectangle at (10, 10)
      {vg::ShapeKind::PATH, 0, {0, 0}, &rect, vg::solidBrush(Colors::RED),
       vg::NO_BRUSH, vg::strokeStyle(1), affine2f::translation(10, 10), nullptr},
      // The current color, turned a quarter, clipped to the top-left 30 x 30
      {vg::ShapeKind::PATH, vg::SHAPE_FILL_CURRENT_COLOR, {0, 0}, &rect,
       vg::solidBrush(Colors::WHITE), vg::NO_BRUSH, vg::strokeStyle(1),
       affine2f::placement(5, 5, PI / 2), &clip},
      // Stroked only, in the current color at half alpha
      {vg::ShapeKind::PATH, vg::SHAPE_STROKE_CURRENT_COLOR, {0, 0}, &rect,
       vg::NO_BRUSH, vg::solidBrush(0x80000000), vg::strokeStyle(2),
       affine2f::translation(30, 30), nullptr},
  };
  static const vg::Picture pic = {shapes, 3, 0, {0, 0, 64, 48}};
  OwnedSurface s(PixelFormat::RGB565_SWAPPED, 64, 48);
  Graphics2D g = makeContext(s);
  g.setAntialias(false);
  g.setFillColor(Colors::YELLOW);  // not used by the picture
  g.drawPicture(pic, Colors::GREEN);
  CHECK(g.getPixel(15, 15, false) == Colors::RED);
  CHECK(g.getPixel(5, 5, false) == Colors::BLACK);
  // The turned rectangle: x in [-5, 5), y in [5, 25) -> clipped to x >= 0
  CHECK(g.getPixel(2, 10, false) == Colors::GREEN);
  CHECK(g.getPixel(2, 26, false) == Colors::BLACK);
  // The stroke: the 2-wide outline of (30, 30)-(50, 40), half green
  CHECK(g.getPixel(40, 35, false) == Colors::BLACK);
  const Color edge = g.getPixel(40, 30, false);
  CHECK(edge != Colors::BLACK && colorG(edge) > 100 && colorG(edge) < 160);
  CHECK(colorR(edge) == 0);
  // The state is as before
  CHECK(g.fillBrush().color == Colors::YELLOW);
  CHECK(g.transform().tx == 0.0f && g.transform().a == 1.0f);
  CHECK(g.clipRect().width == 64 && g.clipRect().height == 48);
#if SHAPOGFX2D_TRANSFORM
  // Under a transform of the context: the whole picture moves; the clip
  // rectangle moves with it
  g.clear(Colors::BLACK);
  g.setTransform(affine2f::translation(10, 0));
  g.drawPicture(pic, Colors::GREEN);
  CHECK(g.getPixel(25, 15, false) == Colors::RED);
  CHECK(g.getPixel(12, 10, false) == Colors::GREEN);
  CHECK(g.getPixel(8, 10, false) == Colors::BLACK);  // the clip moved too
  CHECK(g.getPixel(4, 10, false) == Colors::BLACK);
  g.resetTransform();
#endif
  // A feature bit this build lacks: nothing drawn
  vg::Picture future = pic;
  future.features = 0x8000;
  g.clear(Colors::BLACK);
  g.drawPicture(future, Colors::GREEN);
  CHECK_EQ(countColor(g, Colors::BLACK), 64 * 48);
  // Clipped away entirely: skipped
  static const RectF far = {100, 100, 10, 10};
  vg::Shape away = shapes[0];
  away.clip = &far;
  const vg::Picture pa = {&away, 1, 0, {0, 0, 64, 48}};
  g.drawPicture(pa, Colors::GREEN);
  CHECK_EQ(countColor(g, Colors::BLACK), 64 * 48);
  CHECK(g.clipRect().width == 64);

  // An image shape: drawImage() under the shape's transform, with the
  // alpha of the fill as its opacity
  OwnedSurface img(PixelFormat::RGB565_SWAPPED, 4, 4);
  Graphics2D gi(img);
  gi.clear(Colors::BLUE);
  gi.setPixel(0, 0, Colors::WHITE);
  const Texture tex = img;
  vg::Shape im = {vg::ShapeKind::IMAGE, 0, {0, 0}, &tex, vg::solidBrush(Colors::WHITE),
                  vg::NO_BRUSH, vg::strokeStyle(1), affine2f::translation(20, 20).scale(3), nullptr};
  const vg::Picture pi = {&im, 1, 0, {0, 0, 64, 48}};
  OwnedSurface sb(PixelFormat::RGB565_SWAPPED, 64, 48);
  Graphics2D gb = makeContext(sb);
  gb.setTransform(affine2f::translation(20, 20).scale(3));
  gb.drawImage(tex, 0, 0);
  gb.resetTransform();
  g.clear(Colors::BLACK);
  g.drawPicture(pi);
  CHECK(samePixels(s, sb));
#if SHAPOGFX2D_TRANSFORM
  CHECK(g.getPixel(21, 21, false) == Colors::WHITE);
  CHECK(g.getPixel(30, 30, false) == Colors::BLUE);
#endif
#if SHAPOGFX2D_BLEND && SHAPOGFX2D_TRANSFORM
  im.fill = vg::solidBrush(0x80FFFFFF);
  g.clear(Colors::BLACK);
  g.drawPicture(pi);
  CHECK(near(colorB(g.getPixel(30, 30, false)), 128, 10));
  CHECK_EQ(g.opacity(), 255);
#endif

  // A text shape: drawString() with the baseline at (x, y)
  const vg::Text txt = {"Ab", &ShapoSansP_s08c07, 5, 20};
  vg::Shape ts = {vg::ShapeKind::TEXT, 0, {0, 0}, &txt, vg::solidBrush(Colors::CYAN),
                  vg::NO_BRUSH, vg::strokeStyle(1), affine2f::translation(3, 0), nullptr};
  const vg::Picture pt = {&ts, 1, 0, {0, 0, 64, 48}};
  g.clear(Colors::BLACK);
  g.drawPicture(pt);
  gb.clear(Colors::BLACK);
  gb.setFont(&ShapoSansP_s08c07);
  gb.setTextColor(Colors::CYAN);
  gb.setTransform(affine2f::translation(3, 0));
  gb.drawString(5, 20 - gb.textState().ascent, "Ab");
  gb.resetTransform();
  CHECK(samePixels(s, sb));
  CHECK(countColor(g, Colors::CYAN) > 10);
  CHECK(g.font() == nullptr);  // the font of the context is unchanged
}

// --- Memory ---------------------------------------------------------------------------

// Without an arena the calls draw from buffers on the stack, in parts: the
// same pixels for opaque colors
void testWithoutArena() {
  OwnedSurface sa(PixelFormat::RGB565_SWAPPED, 96, 64),
      sb(PixelFormat::RGB565_SWAPPED, 96, 64);
  Graphics2D ga = makeContext(sa), gb = makeContext(sb, false);
  CHECK(ga.isInitialized() && !gb.isInitialized());
  // A long polyline: many edges, drawn in several parts without an arena
  vec2f pts[100];
  for (int i = 0; i < 100; i++)
    pts[i] = {4.0f + (float)i * 0.9f, 32.0f + 24.0f * std::sin((float)i * 0.4f)};
  for (Graphics2D *g : {&ga, &gb}) {
    g->setAntialias(false);
    g->setStrokeColor(Colors::WHITE);
    g->setStrokeStyle(vg::strokeStyle(5, vg::LineCap::ROUND, vg::LineJoin::ROUND));
    g->strokePolyline(pts, 100);
  }
  CHECK(samePixels(sa, sb));
  CHECK(countColor(ga, Colors::WHITE) > 500);
  // A star (12 edges, a hole in the middle with even-odd) fits the stack
  // buffer: identical with antialiasing as well
  uint8_t ops[8];
  float co[16];
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(Colors::BLACK);
    g->setAntialias(true);
    g->setFillColor(Colors::WHITE);
    g->fillPath(starPath(ops, co, 48, 32, 28, vg::FillRule::EVEN_ODD));
  }
  CHECK(samePixels(sa, sb));
  CHECK(ga.getPixel(48, 32, false) == Colors::BLACK);
  // A path wider than the stack's coverage buffer: drawn, without
  // antialiasing where the buffer is too small
  for (Graphics2D *g : {&ga, &gb}) {
    g->clear(Colors::BLACK);
    g->setFillColor(Colors::WHITE);
    vg::PathBuilder pb(ops, 8, co, 16);
    pb.rect(0.5f, 10, 95, 20);
    g->fillPath(pb.path());
  }
  CHECK(countColor(gb, Colors::WHITE) >= 94 * 20);
  CHECK(countColor(ga, Colors::WHITE) >= 94 * 20);
  // An arena too small for the edges of a shape: still drawn (in parts)
  alignas(8) static uint8_t tiny[2048];
  Graphics2D gc(sb);
  gc.init(tiny, sizeof tiny);
  gc.clear(Colors::BLACK);
  gc.setAntialias(false);
  gc.setStrokeColor(Colors::WHITE);
  gc.setStrokeStyle(vg::strokeStyle(5, vg::LineCap::ROUND, vg::LineJoin::ROUND));
  gc.strokePolyline(pts, 100);
  ga.clear(Colors::BLACK);
  ga.setAntialias(false);
  ga.setStrokeStyle(vg::strokeStyle(5, vg::LineCap::ROUND, vg::LineJoin::ROUND));
  ga.strokePolyline(pts, 100);
  CHECK(samePixels(sa, sb));
}

// The state stack keeps the brushes, the stroke style and the flag
void testVgState() {
  OwnedSurface s(PixelFormat::RGB565_SWAPPED, 8, 8);
  Graphics2D g = makeContext(s);
  const vg::GradientStop bw[2] = {{0.0f, Colors::BLACK}, {1.0f, Colors::WHITE}};
  vg::Gradient lin = vg::linearGradient({0, 0}, {8, 0}, bw, 2);
  g.setFillBrush(vg::gradientBrush(&lin, 100));
  g.setStrokeColor(Colors::RED);
  g.setStrokeStyle(vg::strokeStyle(3, vg::LineCap::ROUND, vg::LineJoin::BEVEL, 2.5f));
  g.setAntialias(false);
  CHECK(g.pushState());
  g.setFillColor(Colors::BLUE);
  g.setStrokeWidth(7);
  g.setAntialias(true);
  CHECK(g.fillBrush().gradient == nullptr);
  CHECK(g.strokeStyle().width == 7.0f);
  g.popState();
  CHECK(g.fillBrush().gradient == &lin);
  CHECK_EQ(colorA(g.fillBrush().color), 100);
  CHECK(g.strokeBrush().color == Colors::RED);
  CHECK(g.strokeStyle().width == 3.0f);
  CHECK(g.strokeStyle().cap == vg::LineCap::ROUND);
  CHECK(g.strokeStyle().join == vg::LineJoin::BEVEL);
  CHECK(g.strokeStyle().miterLimit == 2.5f);
  CHECK(!g.antialias());
  // Opacity and clipping apply to the vector calls
  g.setAntialias(false);
  g.setFillColor(Colors::WHITE);
  g.setOpacity(128);
  g.setClipRect(0, 0, 4, 8);
  uint8_t ops[8];
  float co[16];
  vg::PathBuilder pb(ops, 8, co, 16);
  pb.rect(0, 0, 8, 8);
  g.fillPath(pb.path());
  if (SHAPOGFX2D_BLEND) CHECK(near(lum(g, 2, 2), 128, 10));
  CHECK_EQ(lum(g, 6, 2), 0);
}

// --- rig ------------------------------------------------------------------------------

#if SHAPOGFX2D_RIG && SHAPOGFX2D_TRANSFORM
void testVgRig() {
  static const uint8_t rectOps[] = {0, 1, 1, 1, 4};
  static const float rectCo[] = {0, 0, 20, 0, 20, 10, 0, 10};
  static const vg::Path rect = {rectOps, rectCo, 5, 8, vg::FillRule::NONZERO,
                                {0, 0, 0}, {0, 0, 20, 10}};
  static const vg::Shape shapes[] = {
      {vg::ShapeKind::PATH, vg::SHAPE_FILL_CURRENT_COLOR, {0, 0}, &rect,
       vg::solidBrush(Colors::WHITE), vg::NO_BRUSH, vg::strokeStyle(1),
       affine2f::identity(), nullptr},
      {vg::ShapeKind::PATH, vg::SHAPE_STROKE_CURRENT_WIDTH, {0, 0}, &rect,
       vg::solidBrush(Colors::BLUE), vg::solidBrush(Colors::YELLOW),
       vg::strokeStyle(1), affine2f::translation(0, 12), nullptr},
  };
  static const vg::Picture pic = {shapes, 2, 0, {0, 0, 20, 22}};
  static const rig::Bone bones[] = {
      {"root", 10.0f, 10.0f, 0, 0, rig::SCALE_ONE, rig::SCALE_ONE, rig::NO_PARENT, 0},
      {"part", 5.0f, 0.0f, 0, 0, rig::SCALE_ONE, rig::SCALE_ONE, 0, 0},
  };
  static const rig::Attachment att = {
      nullptr, {0, 0, 0, 0}, affine2f::translation(1, 0), nullptr, 0,
      rig::AttachmentKind::VECTOR, {0, 0}, &pic};
  static const RectF clip = {0, 0, 12, 100};  // in the space of bone 1
  static const rig::Slot slots[] = {
      {"shape", &att, 1, 0, 1, 255, BlendMode::ALPHA, 255, 0, 0, nullptr, 0, {0, 0, 0}, 2.0f},
      {"clipped", &att, 1, 0, 1, 255, BlendMode::ALPHA, 0, 255, 0, &clip, 1, {0, 0, 0}, 0.0f},
  };
  static const rig::Armature arm = {"vg", bones, slots, 2, 2, false, 0,
                                    {0, 0, 40, 40}, 0x1234u,
                                    rig::FEATURE_SLOT_COLOR | rig::FEATURE_STROKE_WIDTH};
  // The slot "shape" is red (its color), the picture at (16, 10); the
  // second slot, green, is clipped to x in [15, 27)
  alignas(4) static uint8_t mem[256];
  rig::Instance inst;
  CHECK(inst.init(arm, mem, sizeof mem));
  CHECK(inst.colorOf(0) == Colors::RED);
  CHECK(inst.colorOf(1) == Colors::GREEN);
  CHECK(inst.colorOf(2) == Colors::WHITE);
  CHECK(inst.strokeWidthOf(0) == 2.0f);
  CHECK(inst.strokeWidthOf(1) == 0.0f);
  CHECK(inst.strokeWidthOf(2) == 1.0f);
  OwnedSurface s(PixelFormat::RGB565_SWAPPED, 64, 48);
  Graphics2D g = makeContext(s);
  g.setAntialias(false);
  // Only the first slot: by hand, the same
  OwnedSurface sb(PixelFormat::RGB565_SWAPPED, 64, 48);
  Graphics2D gb = makeContext(sb);
  gb.setAntialias(false);
  gb.setTransform(affine2f::translation(16, 10));
  gb.drawPicture(pic, Colors::RED, 2.0f);
  gb.resetTransform();
  inst.draw(g, 0, 1);
  CHECK(samePixels(s, sb));
  CHECK(g.getPixel(20, 15, false) == Colors::RED);
  CHECK(g.getPixel(20, 25, false) == Colors::BLUE);
  // The slot's stroke width (2): the yellow outline a pixel each side of
  // the blue rectangle's edge at y = 22
  CHECK(g.getPixel(20, 21, false) == Colors::YELLOW);
  CHECK(g.getPixel(20, 22, false) == Colors::YELLOW);
  CHECK(g.getPixel(20, 20, false) == Colors::BLACK);
  CHECK(g.getPixel(20, 23, false) == Colors::BLUE);
  // Both: the second over the first, green where the clip lets it
  g.clear(Colors::BLACK);
  inst.draw(g);
  CHECK(g.getPixel(20, 15, false) == Colors::GREEN);
  CHECK(g.getPixel(30, 15, false) == Colors::RED);
  CHECK(g.getPixel(26, 15, false) == Colors::GREEN);
  CHECK(g.getPixel(27, 15, false) == Colors::RED);
  CHECK(g.transform().tx == 0.0f && g.clipRect().width == 64);
  // The bounds cover the picture
  const RectF b = inst.bounds();
  CHECK(b.x <= 16.0f && b.y <= 10.0f && b.right() >= 36.0f && b.bottom() >= 32.0f);
  // drawBind(): the same picture without an Instance
  gb.clear(Colors::BLACK);
  rig::drawBind(gb, arm);
  CHECK(samePixels(s, sb));
  // Under a placement, the clip follows the bone
  g.clear(Colors::BLACK);
  gb.clear(Colors::BLACK);
  g.setTransform(affine2f::translation(5, 3));
  gb.setTransform(affine2f::translation(5, 3));
  inst.draw(g);
  rig::drawBind(gb, arm);
  CHECK(samePixels(s, sb));
  CHECK(g.getPixel(31, 18, false) == Colors::GREEN);
  CHECK(g.getPixel(32, 18, false) == Colors::RED);
  g.resetTransform();
  gb.resetTransform();
  // The color override, and a COLOR timeline
  inst.setColor(0, Colors::CYAN);
  CHECK(inst.colorOf(0) == Colors::CYAN);
  g.clear(Colors::BLACK);
  inst.draw(g, 0, 1);
  CHECK(g.getPixel(20, 15, false) == Colors::CYAN);
  static const rig::ColorKey keys[] = {{0, rig::CURVE_LINEAR, 255, 0, 0, {0, 0}},
                                       {10, rig::CURVE_LINEAR, 0, 0, 255, {0, 0}}};
  static const rig::SlotTimeline tls[] = {{keys, 2, 0, rig::Channel::COLOR}};
  static const rig::Animation anim = {"c", 10, 10, 0, 1, 0, 0, nullptr, tls,
                                      nullptr, nullptr, 0x1234u, rig::FEATURE_SLOT_COLOR};
  CHECK(inst.pose(anim, 5.0f));
  CHECK(inst.colorOf(0) == makeColor(127, 0, 127));
  CHECK(inst.colorOf(1) == Colors::GREEN);
  CHECK(inst.pose(anim, 10.0f));
  CHECK(inst.colorOf(0) == Colors::BLUE);
  // A STROKE_WIDTH timeline, and the override
  static const rig::StrokeWidthKey wkeys[] = {{0, rig::CURVE_LINEAR, 0, 2.0f},
                                              {10, rig::CURVE_LINEAR, 0, 6.0f}};
  static const rig::SlotTimeline wtls[] = {{wkeys, 2, 0, rig::Channel::STROKE_WIDTH}};
  static const rig::Animation wanim = {"w", 10, 10, 0, 1, 0, 0, nullptr, wtls,
                                       nullptr, nullptr, 0x1234u, rig::FEATURE_STROKE_WIDTH};
  CHECK(inst.pose(wanim, 5.0f));
  CHECK(std::fabs(inst.strokeWidthOf(0) - 4.0f) < 0.01f);
  CHECK(inst.strokeWidthOf(1) == 0.0f);
  g.clear(Colors::BLACK);
  inst.draw(g, 0, 1);
  CHECK(g.getPixel(20, 20, false) == Colors::YELLOW);  // 4 wide: y 20..23
  CHECK(g.getPixel(20, 19, false) == Colors::RED);  // the first rectangle
  inst.setStrokeWidth(0, 0.0f);
  g.clear(Colors::BLACK);
  inst.draw(g, 0, 1);
  CHECK(g.getPixel(20, 22, false) == Colors::BLUE);
  // Without the feature bits the slots are white and the stroke width is 1
  rig::Armature plain = arm;
  plain.features = 0;
  CHECK(inst.init(plain, mem, sizeof mem));
  CHECK(inst.colorOf(0) == Colors::WHITE);
  CHECK(inst.strokeWidthOf(0) == 1.0f);
  g.clear(Colors::BLACK);
  inst.draw(g, 0, 1);
  CHECK(g.getPixel(20, 15, false) == Colors::WHITE);
  gb.clear(Colors::BLACK);
  rig::drawBind(gb, plain);
  CHECK(samePixels(s, sb));
  // Memory: 20 bytes per slot state now
  CHECK_EQ(rig::Instance::bytes(arm), 3u + 2 * 24 + 2 * 20 + 4);
  inst.deinit();
}
#endif

}  // namespace

void testVg() {
  testPathBuilder();
  testPathFillMatchesRect();
  testFillRules();
  testAntialias();
  testAreaFillsAntialiased();
  testOutlinesAntialiased();
  testMaskAntialiased();
  testImagesAntialiased();
  testGradients();
  testStrokes();
  testFlattening();
  testPictures();
  testWithoutArena();
  testVgState();
#if SHAPOGFX2D_RIG && SHAPOGFX2D_TRANSFORM
  testVgRig();
#endif
}
