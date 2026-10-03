#include "scene.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "shapoco/gfx2d/fonts.hpp"

namespace demo2d {

namespace g2 = shapoco::gfx2d;
using g2::Color;
namespace Colors = g2::Colors;
using g2::Rect;
using g2::vec2i;

static constexpr float PI = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Layout: where the groups of the scene go, for 480x320 and for 320x240

struct Layout {
  Rect textBox;
  const char *subtitle;  // in the 12 px font
  const char *formats;   // two lines in the 8 px font
  int panelX, panelY;    // the RGB444 panel
  int keyX, keyY;        // its left half with the background keyed out
  int thumbX, thumbY, thumbW, thumbH;  // scaled copy of the panel
  bool thumbMirror;                    // and a mirrored one to its right
  float turnX, turnY, turnScale;       // the panel turning about its center
  float spinX, spinY, spinScale;       // the squashed, spinning ball
  Rect pie;
  int ringX, ringY;  // progress ring
  int shapesY;       // the row of shapes: its top and the left of each
  int roundX, circleX, triX, radarX, rectX;
  int sparkX, sparkY;  // center of the sparkle pattern
  int iconX, iconY, iconStep;
  Rect viewport;  // clipped stripes
};

static const Layout LAYOUT_LARGE = {
    {14, 14, 300, 124},
    "Shapes, sprites, transforms, fonts",
    "ShapoSansP_s08c07: proportional 8 px\nGRAY1 / RGB444 / ARGB4444 / "
    "RGB565_SWAPPED",
    372, 12, 316, 36,
    334, 100, 64, 42, true,
    372, 200, 0.7f,
    440, 200, 1.4f,
    {20, 160, 120, 64}, 170, 192,
    262, 12, 112, 150, 236, 272, 362, 284,
    392, 268, 40,
    {200, 210, 120, 40},
};

// The same groups packed into a quarter of the area: some are smaller, the
// mirrored thumbnail is left out, and a few overlap a little
static const Layout LAYOUT_COMPACT = {
    {4, 4, 212, 94},
    "Shapes, sprites, fonts",
    "ShapoSansP_s08c07: proportional 8 px\nGRAY1, RGB444, ARGB4444, RGB565",
    220, 4, 220, 72,
    164, 140, 48, 32, false,
    293, 100, 0.45f,
    296, 150, 1.0f,
    {4, 104, 112, 60}, 138, 134,
    182, 2, 82, 108, 170, 194, 238, 156,
    256, 188, 32,
    {162, 102, 80, 32},
};

static int screenW = LARGE_W, screenH = LARGE_H;
static const Layout *lay = &LAYOUT_LARGE;

// ---------------------------------------------------------------------------
// Procedural assets (on a real target these would be const data in flash)

// ARGB4444 ball sprite with a soft edge and a highlight
static constexpr int BALL = 32;
static uint16_t ballPixels[BALL * BALL];
static const g2::Texture texBall = {g2::PixelFormat::ARGB4444, BALL, BALL,
                                    BALL * 2, ballPixels};

// GRAY1 icon (a smiley), 1 = set
static constexpr int ICON = 32;
static uint8_t iconBits[ICON * ICON / 8];
static const g2::Texture texIcon = {g2::PixelFormat::GRAY1, ICON, ICON,
                                    ICON / 8, iconBits};

// RGB444 offscreen surface drawn every frame and then copied to the screen
static constexpr int PANEL_W = 96, PANEL_H = 64;
static uint8_t panelPixels[(PANEL_W * 3 / 2) * PANEL_H];
static constexpr Color PANEL_BG = g2::makeColor(24, 28, 44);
static const g2::Surface panelSurface = {g2::PixelFormat::RGB444, PANEL_W,
                                         PANEL_H, PANEL_W * 3 / 2, panelPixels};

static void generateAssets() {
  for (int y = 0; y < BALL; y++) {
    for (int x = 0; x < BALL; x++) {
      float dx = x + 0.5f - BALL / 2.0f, dy = y + 0.5f - BALL / 2.0f;
      float d =
          std::sqrt(dx * dx + dy * dy) / (BALL / 2.0f);  // 0 center .. 1 edge
      float a = d < 0.8f ? 1.0f : (d < 1.0f ? (1.0f - d) * 5.0f : 0.0f);
      float hl = std::exp(-((dx + 5) * (dx + 5) + (dy + 5) * (dy + 5)) /
                          30.0f);  // highlight
      float shade = 1.0f - 0.5f * d * d;
      Color c = g2::makeColorF(0.3f * shade + hl, 0.9f * shade + hl,
                               1.0f * shade + hl, a);
      ballPixels[y * BALL + x] = g2::colorToArgb4444(c);
    }
  }
  for (int y = 0; y < ICON; y++) {
    g2::CursorGray1 cur;
    cur.init(iconBits + y * (ICON / 8), 0);
    for (int x = 0; x < ICON; x++) {
      float dx = x + 0.5f - ICON / 2.0f, dy = y + 0.5f - ICON / 2.0f;
      float d = std::sqrt(dx * dx + dy * dy);
      bool ring = d <= 15.0f && d >= 12.5f;
      bool eye = (std::abs(dx + 5.5f) < 2.2f || std::abs(dx - 5.5f) < 2.2f) &&
                 std::abs(dy + 4.0f) < 2.2f;
      bool mouth = d >= 7.0f && d <= 9.5f && dy > 2.0f;
      cur.write((ring || eye || mouth) ? 1u : 0u);
      cur.next();
    }
  }
}

// ---------------------------------------------------------------------------
// Moving objects

static constexpr int NUM_STARS = 20;
struct Star {
  float x, y, zInv;
};
static Star stars[NUM_STARS];

static constexpr int NUM_BALLS = 6;
struct Ball {
  float x, y, vx, vy;
};
static Ball balls[NUM_BALLS];

static float hash01(uint32_t n) {
  n = (n ^ 61u) ^ (n >> 16);
  n *= 9u;
  n ^= n >> 4;
  n *= 0x27d4eb2du;
  n ^= n >> 15;
  return (float)(n & 0xFFFFu) / 65535.0f;
}

static void initObjects() {
  for (int i = 0; i < NUM_STARS; i++) {
    stars[i].x = hash01(i * 3 + 1) * screenW;
    stars[i].y = hash01(i * 3 + 2) * screenH;
    stars[i].zInv = 1.0f / (hash01(i * 3 + 3) * 2.0f + 1.0f);
  }
  for (int i = 0; i < NUM_BALLS; i++) {
    balls[i].x = 40 + hash01(100 + i * 4) * (screenW - 80);
    balls[i].y = 40 + hash01(101 + i * 4) * (screenH - 80);
    balls[i].vx = (hash01(102 + i * 4) - 0.5f) * 160.0f;
    balls[i].vy = (hash01(103 + i * 4) - 0.5f) * 160.0f;
  }
}

// ---------------------------------------------------------------------------
// Drawing helpers

static void starPolygon(vec2i *out, float cx, float cy, float r, float angle) {
  for (int i = 0; i < 10; i++) {
    float dist = (i & 1) ? r : r * 0.45f;
    float th = angle + i * (PI * 2 / 10);
    out[i] = {(int)std::lround(cx + std::cos(th) * dist),
              (int)std::lround(cy + std::sin(th) * dist)};
  }
}

static void drawDrops(g2::Graphics2D &g, float t) {
  // Scrolling grid of ellipses in the background
  constexpr int SIZE = 28, DX = SIZE * 3 / 2, DY = SIZE * 13 / 10;
  const Color col = g2::makeColor(206, 226, 250);
  int shift = (int)(t * 20.0f);
  for (int row = -1; row < screenH / DY + 2; row++) {
    for (int c = -1; c < screenW / DX + 2; c++) {
      int x = c * DX + (row & 1) * (DX / 2) + shift % DX;
      int y = row * DY + shift % DY;
      g.fillEllipse(x - SIZE / 2, y - SIZE / 2, SIZE, SIZE * 3 / 4, col);
    }
  }
}

static void drawStars(g2::Graphics2D &g, float t) {
  for (int i = 0; i < NUM_STARS; i++) {
    Star &s = stars[i];
    float r = 30.0f * s.zInv;
    // Parallax drift, wrapping around the screen
    float x = std::fmod(s.x - t * 25.0f * s.zInv + r, screenW + 2 * r);
    float y = std::fmod(s.y + t * 35.0f * s.zInv + r, screenH + 2 * r);
    if (x < 0) x += screenW + 2 * r;
    if (y < 0) y += screenH + 2 * r;
    x -= r;
    y -= r;
    float angle = t * 0.8f + i;
    Color col = g2::makeColorHsv(i * 360 / NUM_STARS + (int)(t * 40), 200, 230);
    vec2i v[10];
    starPolygon(v, x, y, r, angle);
    if (i & 1) {
      g.fillPolygon(v, 10, g2::colorWithAlpha(col, 200));
    } else {
      g.drawPolygon(v, 10, col);
    }
  }
}

static void moveBalls(float dt) {
  for (int i = 0; i < NUM_BALLS; i++) {
    Ball &b = balls[i];
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    if (b.x < 0) b.x = 0, b.vx = std::abs(b.vx);
    if (b.x > screenW - BALL) b.x = screenW - BALL, b.vx = -std::abs(b.vx);
    if (b.y < 0) b.y = 0, b.vy = std::abs(b.vy);
    if (b.y > screenH - BALL) b.y = screenH - BALL, b.vy = -std::abs(b.vy);
  }
}

static void drawBalls(g2::Graphics2D &g, float t) {
  for (int i = 0; i < NUM_BALLS; i++) {
    const Ball &b = balls[i];
    int x = (int)b.x, y = (int)b.y;
    if (i < 4) {
      // Alpha-blended sprite with a soft shadow underneath
      g.fillEllipse(x + 4, y + BALL - 6, BALL - 4, 8,
                    g2::makeColor(0, 0, 40, 60));
      g.drawImage(texBall, x, y);
    } else {
      // Additive "glow": pulsating opacity
      int op = 120 + (int)(100 * std::sin(t * 3.0f + i));
      g.setBlend(g2::BlendMode::ADD, op);
      g.drawImage(texBall, x, y);
      g.setBlend(g2::BlendMode::ALPHA);
    }
  }
}

static void drawShapes(g2::Graphics2D &g, float t) {
  // A row of outline and filled primitives along the bottom
  const int y = lay->shapesY;
  const int rx = lay->roundX;
  g.fillRoundRect(rx, y, 60, 44, 10, g2::makeColor(255, 120, 40, 200));
  g.drawRoundRect(rx, y, 60, 44, 10, Colors::WHITE);
  g.drawRoundRect(rx + 4, y + 4, 52, 36, 6, g2::makeColor(255, 255, 255, 120));

  const int ccx = lay->circleX;
  g.fillCircle(ccx, y + 22, 20, g2::makeColor(40, 180, 90));
  g.drawCircle(ccx, y + 22, 20, g2::makeColor(10, 90, 40));
  g.drawEllipse(ccx - 24, y + 12, 48, 20, g2::makeColor(255, 255, 255, 180));

  const int tx = lay->triX;
  g.fillTriangle(tx, y + 42, tx + 40, y + 42, tx + 20, y + 2,
                 g2::makeColor(80, 120, 255, 220));
  g.drawTriangle(tx, y + 42, tx + 40, y + 42, tx + 20, y + 2, Colors::WHITE);

  // "Radar": circle, rotating sweep line and a fading trail
  const int cx = lay->radarX, cy = y + 22, rr = 21;
  g.fillCircle(cx, cy, rr, g2::makeColor(10, 40, 30, 200));
  for (int i = 0; i < 8; i++) {
    float a = t * 2.0f - i * 0.12f;
    g.drawLine(cx, cy, cx + (int)(std::cos(a) * rr),
               cy + (int)(std::sin(a) * rr),
               g2::makeColor(80, 255, 120, 220 - i * 26));
  }
  g.drawCircle(cx, cy, rr, g2::makeColor(80, 255, 120));
  g.drawCircle(cx, cy, rr / 2, g2::makeColor(80, 255, 120, 100));

  // Rectangles with thickness and semi-transparent fill
  const int qx = lay->rectX;
  g.fillRect(qx, y, 60, 44, g2::makeColor(255, 255, 255, 90));
  g.drawRect(qx, y, 60, 44, g2::makeColor(60, 60, 90), 3);
  g.drawRect(qx + 8, y + 8, 44, 28, g2::makeColor(200, 40, 40), 1);

  // Individual pixels: a sparkle pattern
  for (int i = 0; i < 40; i++) {
    float a = i * 0.7f + t;
    g.setPixel(lay->sparkX + (int)(std::cos(a) * (i * 0.6f)),
               lay->sparkY + (int)(std::sin(a) * (i * 0.55f)),
               g2::makeColorHsv(i * 9, 255, 255));
  }
}

static void drawIcons(g2::Graphics2D &g) {
  // GRAY1 bitmap: foreground only (transparent background), then with a
  // background
  const int x = lay->iconX, y = lay->iconY;
  g.drawBitmap(texIcon, x, y, g2::makeColor(40, 40, 60));
  g.drawBitmap(texIcon, x + lay->iconStep, y, Colors::YELLOW,
               g2::makeColor(60, 40, 120));
}

static void updatePanel(float t) {
  // Draw into the RGB444 offscreen surface (blitted to the screen by
  // drawPanel)
  g2::Graphics2D p(panelSurface);
  p.clear(PANEL_BG);
  for (int i = 0; i < PANEL_W; i += 6) {
    int h = 14 + (int)(12 * std::sin(t * 2.5f + i * 0.15f));
    p.fillRect(i, PANEL_H - 6 - h, 5, h,
               g2::makeColorHsv(i * 3 + (int)(t * 60), 220, 255));
  }
  p.setFont(&g2::ShapoSansMono_s08c07);
  p.setTextColor(Colors::WHITE);
  p.drawString(3, 3, "RGB444 offscreen");
  p.drawRect(0, 0, PANEL_W, PANEL_H, g2::makeColor(120, 140, 200));
}

static void drawPanel(g2::Graphics2D &g) {
  g.drawImage(panelSurface, lay->panelX, lay->panelY);
  // Partial copy (left half) with a source rectangle, its background keyed
  // out
  g.setColorKey(PANEL_BG);
  g.drawImage(panelSurface, lay->keyX, lay->keyY,
              Rect{0, 0, PANEL_W / 2, PANEL_H - 24});
  g.clearColorKey();
}

static void drawTransforms(g2::Graphics2D &g, float t) {
  // Scaled: a thumbnail of the RGB444 panel and a mirrored copy of it
  const int x = lay->thumbX, y = lay->thumbY, tw = lay->thumbW,
            th = lay->thumbH;
  g.drawImage(panelSurface, Rect{x, y, tw, th});
  if (lay->thumbMirror) {
    g.drawImage(panelSurface, Rect{x + tw * 2 + 6, y, -tw, th});
  }
  // Transformed: the panel turning about its center, with a frame and a
  // caption that turn with it (applied to the band's translation)
  g.pushState();
  g.applyTransform(g2::affine2f::placement(
      lay->turnX, lay->turnY, t * 0.6f, lay->turnScale, lay->turnScale,
      PANEL_W * 0.5f, PANEL_H * 0.5f));
  g.drawImage(panelSurface, 0, 0);
  g.drawRect(-3, -3, PANEL_W + 6, PANEL_H + 6, g2::makeColor(40, 40, 70), 3);
  g.setFont(&g2::ShapoSansP_s12c09a01w02);
  g.setTextColor(g2::makeColor(40, 40, 70));
  g.drawString(0, PANEL_H + 4, "rotate()");
  g.popState();
  // The ball sprite squashed and spun about its own center
  const float squash = 1.0f + 0.35f * std::sin(t * 4.0f);
  const float s = lay->spinScale;
  g.pushState();
  g.applyTransform(g2::affine2f::placement(lay->spinX, lay->spinY, -t * 1.5f,
                                           s * squash, s / squash,
                                           BALL * 0.5f, BALL * 0.5f));
  g.drawImage(texBall, 0, 0);
  g.popState();
}

static void drawCharts(g2::Graphics2D &g, float t) {
  // Pie chart on a flat ellipse: parametric angles keep the slices'
  // areas in proportion
  static const int values[] = {35, 25, 20, 12, 8};
  const Rect pie = lay->pie;
  g.fillEllipse(pie.offset(0, 6), g2::makeColor(0, 0, 40, 60));  // shadow
  float a = t * 0.3f;
  for (int i = 0; i < 5; i++) {
    const float sweep = values[i] * (2 * PI / 100);
    g.fillSector(pie, a, a + sweep, g2::makeColorHsv(200 + i * 36, 170, 240));
    a += sweep;
  }
  g.drawEllipse(pie, g2::makeColor(40, 40, 70));
  // Progress ring: two concentric arcs
  const int cx = lay->ringX, cy = lay->ringY, r = 20;
  const float progress = std::fmod(t * 0.25f, 1.0f);
  g.drawCircle(cx, cy, r, g2::makeColor(40, 40, 70, 80));
  for (int k = 0; k < 3; k++) {
    g.drawCircleArc(cx, cy, r - k, -PI / 2, -PI / 2 + progress * 2 * PI,
                    g2::makeColor(230, 90, 40));
  }
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%d%%", (int)(progress * 100));
  g.setFont(&g2::ShapoSansP_s08c07);
  g.setTextColor(g2::makeColor(40, 40, 70));
  const g2::TextMetrics m = g.textMetrics(buf);
  g.drawString(cx - m.width / 2, cy - m.height / 2, buf);
}

static void drawText(g2::Graphics2D &g, float t) {
  const Rect box = lay->textBox;
  g.fillRoundRect(box, 12, g2::makeColor(20, 24, 40, 180));
  g.drawRoundRect(box, 12, g2::makeColor(255, 255, 255, 160));

  int x = box.x + 12, y = box.y + 8;
  g.setFont(&g2::ShapoSansP_s21c16a01w03);
  g.setTextColor(Colors::WHITE);
  g.drawString(x, y, "ShapoGFX 2D");
  // Right-aligned elapsed time, measured with the same font
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1fs", (double)t);
  g.setFont(&g2::ShapoSansP_s12c09a01w02);
  g.setTextColor(g2::makeColor(255, 220, 120));
  g.drawString(box.right() - 12 - g.textMetrics(buf).width, y + 6, buf);
  y += 30;

  g.setTextColor(g2::makeColor(200, 220, 255));
  g.drawString(x, y, lay->subtitle);
  y += g.textMetrics("").lineAdvance + 2;

  g.setFont(&g2::ShapoSansP_s08c07);
  g.setTextColor(Colors::WHITE);
  g.drawString(x, y, lay->formats);
  y += g.textMetrics("").lineAdvance * 2 + 2;

  g.setFont(&g2::ShapoSansMono_s08c07);
  g.setTextColor(g2::makeColor(140, 255, 160), g2::makeColor(0, 60, 30));
  g.drawString(x, y, "Mono 8px, bg color ");
  // Enlarged by the transform: scaled about the cursor
  const g2::vec2i c = g.cursor();
  g.pushState();
  g.translate((float)c.x, (float)c.y);
  g.scale(2);
  g.setTextColor(g2::makeColor(255, 160, 160));
  g.drawString(0, 0, "x2");
  g.popState();
}

// ---------------------------------------------------------------------------
// API

static float lastT = 0.0f;
static float t_ = 0.0f;  // time of the frame sceneDraw() draws

void sceneInit(int width, int height) {
  screenW = width;
  screenH = height;
  lay = (width < LARGE_W || height < LARGE_H) ? &LAYOUT_COMPACT : &LAYOUT_LARGE;
  generateAssets();
  initObjects();
  lastT = 0.0f;
}

void sceneUpdate(float t) {
  float dt = t - lastT;
  if (dt < 0.0f || dt > 0.1f) dt = 0.016f;
  lastT = t;
  t_ = t;
  updatePanel(t);
  moveBalls(dt);
}

void sceneDraw(g2::Graphics2D &g, int bandY) {
  // The scene in screen pixels, the band's rows moved to the top of g
  g.resetClipRect();
  g.setTransform(g2::affine2f::translation(0.0f, (float)-bandY));
  g.clear(g2::makeColor(236, 240, 248));
  drawDrops(g, t_);
  drawStars(g, t_);
  drawPanel(g);
  drawShapes(g, t_);
  drawCharts(g, t_);
  drawTransforms(g, t_);
  drawIcons(g);
  drawBalls(g, t_);
  drawText(g, t_);

  // Clip rectangle: a viewport showing a striped pattern. The clip
  // rectangle is in target pixels, so it moves with the band.
  const Rect vp = lay->viewport;
  g.setClipRect(vp.offset(0, -bandY));
  for (int i = -40; i < 160; i += 12) {
    int shift = (int)(t_ * 30) % 24;
    g.fillTriangle(vp.x + i + shift, vp.bottom(), vp.x + i + 12 + shift,
                   vp.bottom(), vp.x + i + 6 + shift, vp.y,
                   g2::makeColor(255, 80, 80, 150));
  }
  g.resetClipRect();
  g.drawRect(vp, g2::makeColor(60, 60, 90));
}

}  // namespace demo2d
