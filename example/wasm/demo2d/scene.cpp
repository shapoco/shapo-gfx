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
    stars[i].x = hash01(i * 3 + 1) * SCREEN_W;
    stars[i].y = hash01(i * 3 + 2) * SCREEN_H;
    stars[i].zInv = 1.0f / (hash01(i * 3 + 3) * 2.0f + 1.0f);
  }
  for (int i = 0; i < NUM_BALLS; i++) {
    balls[i].x = 40 + hash01(100 + i * 4) * (SCREEN_W - 80);
    balls[i].y = 40 + hash01(101 + i * 4) * (SCREEN_H - 80);
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
  for (int row = -1; row < SCREEN_H / DY + 2; row++) {
    for (int c = -1; c < SCREEN_W / DX + 2; c++) {
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
    float x = std::fmod(s.x - t * 25.0f * s.zInv + r, SCREEN_W + 2 * r);
    float y = std::fmod(s.y + t * 35.0f * s.zInv + r, SCREEN_H + 2 * r);
    if (x < 0) x += SCREEN_W + 2 * r;
    if (y < 0) y += SCREEN_H + 2 * r;
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

static void drawBalls(g2::Graphics2D &g, float t, float dt) {
  for (int i = 0; i < NUM_BALLS; i++) {
    Ball &b = balls[i];
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    if (b.x < 0) b.x = 0, b.vx = std::abs(b.vx);
    if (b.x > SCREEN_W - BALL) b.x = SCREEN_W - BALL, b.vx = -std::abs(b.vx);
    if (b.y < 0) b.y = 0, b.vy = std::abs(b.vy);
    if (b.y > SCREEN_H - BALL) b.y = SCREEN_H - BALL, b.vy = -std::abs(b.vy);
    int x = (int)b.x, y = (int)b.y;
    if (i < 4) {
      // Alpha-blended sprite with a soft shadow underneath
      g.fillEllipse(x + 4, y + BALL - 6, BALL - 4, 8,
                    g2::makeColor(0, 0, 40, 60));
      g.drawImage(texBall, x, y, g2::BlendMode::ALPHA);
    } else {
      // Additive "glow": pulsating opacity
      int op = 120 + (int)(100 * std::sin(t * 3.0f + i));
      g.drawImage(texBall, x, y, g2::BlendMode::ADD, op);
    }
  }
}

static void drawShapes(g2::Graphics2D &g, float t) {
  // A row of outline and filled primitives along the bottom
  const int y = SCREEN_H - 58;
  g.fillRoundRect(12, y, 60, 44, 10, g2::makeColor(255, 120, 40, 200));
  g.drawRoundRect(12, y, 60, 44, 10, Colors::WHITE);
  g.drawRoundRect(16, y + 4, 52, 36, 6, g2::makeColor(255, 255, 255, 120));

  g.fillCircle(112, y + 22, 20, g2::makeColor(40, 180, 90));
  g.drawCircle(112, y + 22, 20, g2::makeColor(10, 90, 40));
  g.drawEllipse(88, y + 12, 48, 20, g2::makeColor(255, 255, 255, 180));

  g.fillTriangle(150, y + 42, 190, y + 42, 170, y + 2,
                 g2::makeColor(80, 120, 255, 220));
  g.drawTriangle(150, y + 42, 190, y + 42, 170, y + 2, Colors::WHITE);

  // "Radar": circle, rotating sweep line and a fading trail
  const int cx = 236, cy = y + 22, rr = 21;
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
  g.fillRect(272, y, 60, 44, g2::makeColor(255, 255, 255, 90));
  g.drawRect(272, y, 60, 44, g2::makeColor(60, 60, 90), 3);
  g.drawRect(280, y + 8, 44, 28, g2::makeColor(200, 40, 40), 1);

  // Individual pixels: a sparkle pattern
  for (int i = 0; i < 40; i++) {
    float a = i * 0.7f + t;
    g.setPixel(362 + (int)(std::cos(a) * (i * 0.6f)),
               y + 22 + (int)(std::sin(a) * (i * 0.55f)),
               g2::makeColorHsv(i * 9, 255, 255));
  }
}

static void drawIcons(g2::Graphics2D &g) {
  // GRAY1 bitmap: foreground only (transparent background), then with a
  // background
  const int x = 392, y = SCREEN_H - 58;
  g.drawBitmap(texIcon, x, y + 6, g2::makeColor(40, 40, 60));
  g.drawBitmap(texIcon, x + 40, y + 6, Colors::YELLOW,
               g2::makeColor(60, 40, 120));
}

static void drawPanel(g2::Graphics2D &g, float t) {
  // Draw into the RGB444 offscreen surface, then blit it to the screen
  g2::Graphics2D p(panelSurface);
  p.clear(g2::makeColor(24, 28, 44));
  for (int i = 0; i < PANEL_W; i += 6) {
    int h = 14 + (int)(12 * std::sin(t * 2.5f + i * 0.15f));
    p.fillRect(i, PANEL_H - 6 - h, 5, h,
               g2::makeColorHsv(i * 3 + (int)(t * 60), 220, 255));
  }
  p.setFont(&ShapoSansMono_s08c07);
  p.setTextColor(Colors::WHITE);
  p.drawString(3, 3, "RGB444 offscreen");
  p.drawRect(0, 0, PANEL_W, PANEL_H, g2::makeColor(120, 140, 200));

  const int x = SCREEN_W - PANEL_W - 12, y = 12;
  g.drawImage(panelSurface, x, y, g2::BlendMode::NONE);
  // Partial copy (left half) with a source rectangle
  g.drawImage(panelSurface, x - PANEL_W / 2 - 8, y + 24,
              Rect{0, 0, PANEL_W / 2, PANEL_H - 24}, g2::BlendMode::NONE);
}

static void drawText(g2::Graphics2D &g, float t) {
  const Rect box = {14, 14, 300, 124};
  g.fillRoundRect(box, 12, g2::makeColor(20, 24, 40, 180));
  g.drawRoundRect(box, 12, g2::makeColor(255, 255, 255, 160));

  int x = box.x + 12, y = box.y + 8;
  g.setFont(&ShapoSansP_s21c16a01w03);
  g.setTextColor(Colors::WHITE);
  g.drawString(x, y, "ShapoGFX 2D");
  // Right-aligned elapsed time, measured with the same font
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1fs", (double)t);
  g.setFont(&ShapoSansP_s12c09a01w02);
  g.setTextColor(g2::makeColor(255, 220, 120));
  g.drawString(box.right() - 12 - g.measureText(buf), y + 6, buf);
  y += 30;

  g.setTextColor(g2::makeColor(200, 220, 255));
  g.drawString(x, y, "Shapes, sprites, fonts, blending");
  y += g.lineAdvance() + 2;

  g.setFont(&ShapoSansP_s08c07);
  g.setTextColor(Colors::WHITE);
  g.drawString(x, y,
               "ShapoSansP_s08c07: proportional 8 px\nGRAY1 / RGB444 / "
               "ARGB4444 / RGB565_SWAPPED");
  y += g.lineAdvance() * 2 + 2;

  g.setFont(&ShapoSansMono_s08c07);
  g.setTextColor(g2::makeColor(140, 255, 160), g2::makeColor(0, 60, 30));
  g.drawString(x, y, "Mono 8px, bg color ");
  g.setFont(&ShapoSansMono_s08c07, 2);
  g.setTextColor(g2::makeColor(255, 160, 160));
  g.drawString("x2");
}

// ---------------------------------------------------------------------------
// API

void sceneInit() {
  generateAssets();
  initObjects();
}

void sceneRender(g2::Graphics2D &g, float t) {
  static float lastT = 0.0f;
  float dt = t - lastT;
  if (dt < 0.0f || dt > 0.1f) dt = 0.016f;
  lastT = t;

  g.resetClipRect();
  g.clear(g2::makeColor(236, 240, 248));
  drawDrops(g, t);
  drawStars(g, t);
  drawPanel(g, t);
  drawShapes(g, t);
  drawIcons(g);
  drawBalls(g, t, dt);
  drawText(g, t);

  // Clip rectangle: a viewport in the lower middle showing a striped pattern
  const Rect vp = {SCREEN_W / 2 - 40, SCREEN_H - 110, 120, 40};
  g.setClipRect(vp);
  for (int i = -40; i < 160; i += 12) {
    int shift = (int)(t * 30) % 24;
    g.fillTriangle(vp.x + i + shift, vp.bottom(), vp.x + i + 12 + shift,
                   vp.bottom(), vp.x + i + 6 + shift, vp.y,
                   g2::makeColor(255, 80, 80, 150));
  }
  g.resetClipRect();
  g.drawRect(vp, g2::makeColor(60, 60, 90));
}

}  // namespace demo2d
