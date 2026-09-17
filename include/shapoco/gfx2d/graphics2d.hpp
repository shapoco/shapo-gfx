#ifndef SHAPOGFX2D_GRAPHICS2D_HPP
#define SHAPOGFX2D_GRAPHICS2D_HPP

#include <cstdint>

#include "shapoco/gfx2d/gfxfont.h"
#include "shapoco/gfx2d/surface.hpp"

namespace shapoco::gfx2d {

// Text settings of a Graphics2D
struct TextState {
  const GFXfont *font = nullptr;
  int scale = 1;       // integer magnification
  int ascent = 0;      // top of the line box to the baseline (unscaled px)
  int lineHeight = 0;  // height of the line box (unscaled px)
  Color color = Colors::WHITE;  // foreground
  Color background =
      Colors::TRANSPARENT;  // background of the glyph box (TRANSPARENT: none)
  int cursorX = 0, cursorY = 0;  // top-left of the next glyph's line box
  int lineStartX = 0;            // x to return to after '\n'
};

struct GraphicsState2D {
  Rect clip = {0, 0, 0, 0};
  TextState text;
};

// 2D drawing context. Draws into a Surface of any enabled pixel format.
//
// - Colors are ARGB8888 (see Color). An alpha below 255 blends the primitive
//   over the existing pixels; alpha 0 draws nothing.
// - All drawing is clipped to the clip rectangle (the whole target by default).
// - The context stores a copy of the Surface struct, not the pixel data; the
//   pixel buffer must outlive the drawing calls.
// - No memory is allocated.
class Graphics2D {
 public:
  Graphics2D() = default;
  explicit Graphics2D(const Surface &target) { setTarget(target); }

  void setTarget(const Surface &target);
  const Surface &target() const { return target_; }
  bool hasTarget() const { return target_.pixels != nullptr; }
  PixelFormat format() const { return target_.format; }
  Rect bounds() const { return {0, 0, target_.width, target_.height}; }

  // --- State -------------------------------------------------------------
  void setClipRect(const Rect &r) {
    state_.clip = r.normalized().intersect(bounds());
  }
  void setClipRect(int x, int y, int w, int h) {
    setClipRect(Rect{x, y, w, h});
  }
  void resetClipRect() { state_.clip = bounds(); }
  const Rect &clipRect() const { return state_.clip; }
  const GraphicsState2D &state() const { return state_; }
  void setState(const GraphicsState2D &s) { state_ = s; }

  // --- Pixels and rectangles ----------------------------------------------
  void clear(Color c) { fillRect(state_.clip, c); }
  void setPixel(int x, int y, Color c);
  Color getPixel(int x, int y) const;  // TRANSPARENT outside the target

  void fillRect(const Rect &r, Color c);
  void fillRect(int x, int y, int w, int h, Color c) {
    fillRect(Rect{x, y, w, h}, c);
  }
  // Outline inside the rectangle, `thickness` pixels wide
  void drawRect(const Rect &r, Color c, int thickness = 1);
  void drawRect(int x, int y, int w, int h, Color c, int thickness = 1) {
    drawRect(Rect{x, y, w, h}, c, thickness);
  }
  void fillRoundRect(const Rect &r, int radius, Color c);
  void fillRoundRect(int x, int y, int w, int h, int radius, Color c) {
    fillRoundRect(Rect{x, y, w, h}, radius, c);
  }
  void drawRoundRect(const Rect &r, int radius, Color c);
  void drawRoundRect(int x, int y, int w, int h, int radius, Color c) {
    drawRoundRect(Rect{x, y, w, h}, radius, c);
  }
  void drawHLine(int x, int y, int w, Color c) { fillRect(x, y, w, 1, c); }
  void drawVLine(int x, int y, int h, Color c) { fillRect(x, y, 1, h, c); }

  // --- Ellipses ------------------------------------------------------------
  // Ellipses are inscribed in the rectangle.
  void fillEllipse(const Rect &r, Color c);
  void fillEllipse(int x, int y, int w, int h, Color c) {
    fillEllipse(Rect{x, y, w, h}, c);
  }
  void drawEllipse(const Rect &r, Color c);
  void drawEllipse(int x, int y, int w, int h, Color c) {
    drawEllipse(Rect{x, y, w, h}, c);
  }
  void fillCircle(int cx, int cy, int radius, Color c) {
    fillEllipse(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1, c);
  }
  void drawCircle(int cx, int cy, int radius, Color c) {
    drawEllipse(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1, c);
  }

  // --- Lines and polygons --------------------------------------------------
  void drawLine(int x0, int y0, int x1, int y1, Color c);
  void drawLine(const vec2i &a, const vec2i &b, Color c) {
    drawLine(a.x, a.y, b.x, b.y, c);
  }
  void drawPolyline(const vec2i *points, int count, Color c);
  void drawPolygon(const vec2i *points, int count, Color c);  // closed outline
  void fillPolygon(const vec2i *points, int count,
                   Color c);  // even-odd rule, up to 16 crossings per row
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, Color c) {
    vec2i v[3] = {{x0, y0}, {x1, y1}, {x2, y2}};
    fillPolygon(v, 3, c);
  }
  void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, Color c) {
    vec2i v[3] = {{x0, y0}, {x1, y1}, {x2, y2}};
    drawPolygon(v, 3, c);
  }

  // --- Images ----------------------------------------------------------------
  // Draw an image (any format) at (dx, dy), converting between formats.
  //   BlendMode::NONE  copy; ARGB4444 alpha is copied where the target has
  //   alpha,
  //                    otherwise ignored
  //   BlendMode::ALPHA use the source alpha (ARGB4444); other formats are
  //   copied BlendMode::ADD   additive
  // `opacity` (0..255) is multiplied in for ALPHA and ADD.
  void drawImage(const Texture &img, int dx, int dy,
                 BlendMode mode = BlendMode::ALPHA, int opacity = 255) {
    drawImage(img, dx, dy, Rect{0, 0, img.width, img.height}, mode, opacity);
  }
  void drawImage(const Texture &img, int dx, int dy, const Rect &src,
                 BlendMode mode = BlendMode::ALPHA, int opacity = 255);
  // Draw a GRAY1 image as a two-color mask: set bits in `fg`, clear bits in
  // `bg` (TRANSPARENT leaves them untouched).
  void drawBitmap(const Texture &bitmap, int dx, int dy, Color fg,
                  Color bg = Colors::TRANSPARENT) {
    drawBitmap(bitmap, dx, dy, Rect{0, 0, bitmap.width, bitmap.height}, fg, bg);
  }
  void drawBitmap(const Texture &bitmap, int dx, int dy, const Rect &src,
                  Color fg, Color bg = Colors::TRANSPARENT);

  // --- Text
  // -------------------------------------------------------------------- The
  // cursor is the top-left corner of the line box; the baseline is `ascent`
  // scaled pixels below it. '\n' moves the cursor to the next line.
  void setFont(const GFXfont *font, int scale = 1);
  void setTextScale(int scale) { state_.text.scale = scale < 1 ? 1 : scale; }
  void setTextColor(Color fg, Color bg = Colors::TRANSPARENT) {
    state_.text.color = fg;
    state_.text.background = bg;
  }
  void setCursor(int x, int y) {
    state_.text.cursorX = state_.text.lineStartX = x;
    state_.text.cursorY = y;
  }
  vec2i cursor() const { return {state_.text.cursorX, state_.text.cursorY}; }
  const TextState &textState() const { return state_.text; }

  // Draw one glyph with its line box at (x, y); returns the scaled x advance
  int drawChar(int x, int y, int code);
  void drawString(const char *str);  // at the cursor, advancing it
  void drawString(int x, int y, const char *str) {
    setCursor(x, y);
    drawString(str);
  }
  // Width of the widest line of `str` in scaled pixels (0 without a font)
  int measureText(const char *str) const;
  int charAdvance(
      int code) const;  // scaled x advance of one glyph (0 if missing)
  int textHeight() const {
    return state_.text.lineHeight * state_.text.scale;
  }  // line box height
  int lineAdvance()
      const;  // vertical distance between lines (font yAdvance x scale)

 private:
  Surface target_ = {PixelFormat::RGB565BE, 0, 0, 0, nullptr};
  GraphicsState2D state_;

  // Fill [x0, x1) on row y with a native color; already clipped.
  void fillSpanRaw(int y, int x0, int x1, uint32_t native, uint32_t alpha64);
  // Clip against the clip rect and fill
  void fillSpan(int y, int x0, int x1, uint32_t native, uint32_t alpha64);
  void plot(int x, int y, uint32_t native, uint32_t alpha64);
  void fillRectRaw(const Rect &clipped, uint32_t native, uint32_t alpha64);
};

// 0..255 opacity as 0..64 (64 = opaque): (a * 64 + 127) / 255, with the
// division by 255 replaced by a multiply-shift that is exact for 0..255
constexpr uint32_t alpha255To64(uint32_t a) {
  return ((a * 64u + 127u) * 0x8081u) >> 23;
}

// Opacity of a Color as 0..64 (64 = opaque)
constexpr uint32_t colorAlpha64(Color c) {
  return alpha255To64((uint32_t)colorA(c));
}

}  // namespace shapoco::gfx2d

#endif
