#ifndef SHAPOGFX2D_GRAPHICS2D_HPP
#define SHAPOGFX2D_GRAPHICS2D_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "shapoco/gfx2d/gfxfont.h"
#include "shapoco/gfx2d/math2d.hpp"
#include "shapoco/gfx2d/surface.hpp"

namespace shapoco::gfx2d {

// A pixel position on a target as stored in the state (SHAPOGFX_COORD_BITS)
using coord_t =
    std::conditional_t<(SHAPOGFX_COORD_BITS <= 7), int8_t, int16_t>;
using ucoord_t =
    std::conditional_t<(SHAPOGFX_COORD_BITS <= 8), uint8_t, uint16_t>;

// Settings of Graphics2D::init() (none yet; members may be added later)
struct Config {};

// What the current transform does, classified whenever it changes. The
// drawing calls pick their code path by it: with IDENTITY and TRANSLATE they
// run the same code as without a transform.
enum class TransformKind : uint8_t {
  IDENTITY,
  TRANSLATE,  // translation only
  SCALE,      // scaling (mirroring included) and translation
  AFFINE,     // rotation or shear
};

// Size of a glyph or a text, in the coordinates of the drawing calls (not
// scaled by the transform, so that they can be used to lay out text drawn
// under it). Font metrics are whole pixels, so these are exact; computing
// them takes no floating point.
struct TextMetrics {
  int width = 0;        // advance width (of the widest line)
  int height = 0;       // line box height + lineAdvance per further line
  int ascent = 0;       // top of the line box to the baseline
  int lineAdvance = 0;  // baseline to baseline
};

// The same measured on the target: in target pixels along the text's own
// axes, i.e. width scaled by the transform's x axis and the others by its y
// axis (a rotation changes none of them)
struct TextMetricsF {
  float width = 0.0f;
  float height = 0.0f;
  float ascent = 0.0f;
  float lineAdvance = 0.0f;
};

// Text settings of a Graphics2D
struct TextState {
  const GFXfont *font = nullptr;
  Color color = Colors::WHITE;           // foreground
  Color background = Colors::TRANSPARENT;  // box behind each glyph (none)
  int cursorX = 0, cursorY = 0;  // top-left of the next glyph's line box
  int lineStartX = 0;            // x to return to after '\n'
  int16_t ascent = 0;            // top of the line box to the baseline
  int16_t lineHeight = 0;        // height of the line box
};

// Everything pushState() saves (the text cursor is restored separately)
struct GraphicsState2D {
  affine2f transform = {1, 0, 0, 1, 0, 0};
  TextState text;
  Color colorKey = Colors::TRANSPARENT;
  ucoord_t clipX = 0, clipY = 0, clipWidth = 0, clipHeight = 0;
  BlendMode blendMode = BlendMode::ALPHA;
  uint8_t opacity = 255;
  bool colorKeyEnabled = false;
};

namespace detail {
struct G2Impl;
}

// 2D drawing context. Draws into a Surface of any enabled pixel format.
//
// - Colors are ARGB8888 (see Color). The blend mode and opacity of the state
//   (setBlend()) decide how a color is put: ALPHA (default) blends with the
//   color's alpha x opacity, ADD adds the color weighted by the same, NONE
//   overwrites.
// - Coordinates go through the transform of the state (setTransform() and
//   friends), then everything is clipped to the clip rectangle, which is in
//   target pixels.
// - The context stores a copy of the Surface struct, not the pixel data; the
//   pixel buffer must outlive the drawing calls.
// - No memory is allocated. pushState() and some transformed shapes use the
//   arena passed to init(); without one the context still draws everything.
class Graphics2D {
 public:
  Graphics2D() = default;
  explicit Graphics2D(const Surface &target) { setTarget(target); }

  // --- Memory -------------------------------------------------------------
  // The arena holds the state stack (SHAPOGFX2D_STACK_DEPTH levels) and
  // scratch memory: polygons with more than 12 edges (24 bytes per edge)
  // and rotated rounded rectangles use it, and fall back to slower or
  // coarser code without it. False (and nothing changes) if the arena is
  // smaller than the stack.
  bool init(const Config &cfg, void *arena, size_t arenaSize);
  bool init(void *arena, size_t arenaSize) {
    return init(Config{}, arena, arenaSize);
  }
  void deinit();  // stop using the arena (the state stack is dropped)
  bool isInitialized() const { return stack_ != nullptr; }
  // Arena size for the state stack plus `scratchBytes` of scratch memory
  static size_t arenaBytes(size_t scratchBytes = 2048);

  // --- Target -------------------------------------------------------------
  void setTarget(const Surface &target);
  const Surface &target() const { return target_; }
  bool hasTarget() const { return target_.pixels != nullptr; }
  PixelFormat format() const { return target_.format; }
  Rect bounds() const { return {0, 0, target_.width, target_.height}; }

  // --- State --------------------------------------------------------------
  // Save the whole state (transform, clip, blend, color key, font, colors)
  // on the stack; false (and nothing saved) when the stack is full or there
  // is no arena. popState() restores it, except for the text cursor.
  bool pushState();
  void popState();
  int stateDepth() const { return stackTop_; }
  const GraphicsState2D &state() const { return state_; }
  void setState(const GraphicsState2D &s);

  // Clip rectangle, in target pixels (the transform does not apply)
  void setClipRect(const Rect &r);
  void setClipRect(int x, int y, int w, int h) {
    setClipRect(Rect{x, y, w, h});
  }
  void resetClipRect() { setClipRect(bounds()); }
  Rect clipRect() const {
    return {state_.clipX, state_.clipY, state_.clipWidth, state_.clipHeight};
  }

  // Transform: maps the coordinates of the drawing calls to target pixels.
  // translate(), scale(), rotate() and applyTransform() multiply on the
  // right, like a canvas context: the one given last applies first. Only
  // affine transforms. Coordinates are continuous like those of affine2f;
  // see "Coordinates" in SPEC.md for how the integer ones map. Does nothing
  // with SHAPOGFX2D_TRANSFORM=0.
  void setTransform(const affine2f &m);
  void resetTransform();
  const affine2f &transform() const { return state_.transform; }
  TransformKind transformKind() const { return kind_; }
  void applyTransform(const affine2f &m);  // transform = transform * m
  void translate(float x, float y);
  void scale(float sx, float sy);
  void scale(float s) { scale(s, s); }
  void rotate(float angle);                    // radians, clockwise on screen
  void rotate(float angle, float cx, float cy);  // about (cx, cy)

  // Blend mode and opacity (0..255) of everything drawn. Shapes and text
  // use their color's alpha x opacity, images their pixels' alpha (only
  // ARGB4444 has one) x opacity. NONE copies (the alpha of the color or
  // of an ARGB4444 image goes into an ARGB4444 target). Does nothing with
  // SHAPOGFX2D_BLEND=0 (ALPHA, 255).
  void setBlend(BlendMode mode, int opacity = 255);
  void setBlendMode(BlendMode mode) { setBlend(mode, state_.opacity); }
  void setOpacity(int opacity) { setBlend(state_.blendMode, opacity); }
  BlendMode blendMode() const { return state_.blendMode; }
  int opacity() const { return state_.opacity; }

  // Color key of drawImage(): image pixels of this color (compared after
  // converting it to the image's format, alpha included for ARGB4444) are
  // not drawn. Does nothing with SHAPOGFX2D_COLOR_KEY=0.
  void setColorKey(Color key);
  void clearColorKey();
  bool hasColorKey() const { return state_.colorKeyEnabled; }
  Color colorKey() const { return state_.colorKey; }

  // --- Pixels and rectangles ----------------------------------------------
  // Overwrite the clip rectangle with c (ignores the transform and the
  // blend mode)
  void clear(Color c);
  // `transformed`: whether (x, y) goes through the transform
  void setPixel(int x, int y, Color c, bool transformed = true);
  // TRANSPARENT outside the target
  Color getPixel(int x, int y, bool transformed = true) const;

  void fillRect(const Rect &r, Color c);
  void fillRect(int x, int y, int w, int h, Color c) {
    fillRect(Rect{x, y, w, h}, c);
  }
  void fillRect(const RectF &r, Color c);
  // Outline inside the rectangle, `thickness` wide (scaled by the transform)
  void drawRect(const Rect &r, Color c, int thickness = 1);
  void drawRect(int x, int y, int w, int h, Color c, int thickness = 1) {
    drawRect(Rect{x, y, w, h}, c, thickness);
  }
  void drawRect(const RectF &r, Color c, float thickness = 1.0f);
  void fillRoundRect(const Rect &r, int radius, Color c);
  void fillRoundRect(int x, int y, int w, int h, int radius, Color c) {
    fillRoundRect(Rect{x, y, w, h}, radius, c);
  }
  void fillRoundRect(const RectF &r, float radius, Color c);
  // One-pixel outline (the lines of outlines are not scaled)
  void drawRoundRect(const Rect &r, int radius, Color c);
  void drawRoundRect(int x, int y, int w, int h, int radius, Color c) {
    drawRoundRect(Rect{x, y, w, h}, radius, c);
  }
  void drawRoundRect(const RectF &r, float radius, Color c);
  // One-pixel lines from (x, y) over w (h) pixels
  void drawHLine(int x, int y, int w, Color c);
  void drawVLine(int x, int y, int h, Color c);

  // --- Ellipses -------------------------------------------------------------
  // Ellipses are inscribed in the rectangle; the outlines are one pixel wide.
  void fillEllipse(const Rect &r, Color c);
  void fillEllipse(int x, int y, int w, int h, Color c) {
    fillEllipse(Rect{x, y, w, h}, c);
  }
  void fillEllipse(const RectF &r, Color c);
  void drawEllipse(const Rect &r, Color c);
  void drawEllipse(int x, int y, int w, int h, Color c) {
    drawEllipse(Rect{x, y, w, h}, c);
  }
  void drawEllipse(const RectF &r, Color c);
  // A circle of `radius` pixels around the pixel (cx, cy)
  void fillCircle(int cx, int cy, int radius, Color c) {
    fillEllipse(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1, c);
  }
  void drawCircle(int cx, int cy, int radius, Color c) {
    drawEllipse(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1, c);
  }
  void fillCircle(const vec2f &center, float radius, Color c) {
    fillEllipse(circleRect(center, radius), c);
  }
  void drawCircle(const vec2f &center, float radius, Color c) {
    drawEllipse(circleRect(center, radius), c);
  }

  // --- Arcs and sectors -----------------------------------------------------
  // The part of the ellipse inscribed in the rectangle from startAngle to
  // endAngle (radians). Angles run clockwise on screen from the +x axis and
  // are parametric: the ellipse is a stretched circle and an angle is taken
  // on that circle, so 45 degrees points at the corner of the rectangle and
  // sectors of equal angle have equal area. endAngle is taken modulo 2 pi
  // after startAngle unless endAngle - startAngle >= 2 pi, which draws the
  // whole ellipse. Sectors sharing an angle do not overlap and leave no gap.
  // The angles are those before the transform.
  //
  // drawArc: the pixels of drawEllipse() within the angle range
  void drawArc(const Rect &r, float startAngle, float endAngle, Color c);
  void drawArc(int x, int y, int w, int h, float startAngle, float endAngle,
               Color c) {
    drawArc(Rect{x, y, w, h}, startAngle, endAngle, c);
  }
  void drawArc(const RectF &r, float startAngle, float endAngle, Color c);
  // fillSector: the pixels of fillEllipse() within the angle range (a pie)
  void fillSector(const Rect &r, float startAngle, float endAngle, Color c);
  void fillSector(int x, int y, int w, int h, float startAngle, float endAngle,
                  Color c) {
    fillSector(Rect{x, y, w, h}, startAngle, endAngle, c);
  }
  void fillSector(const RectF &r, float startAngle, float endAngle, Color c);
  void drawCircleArc(int cx, int cy, int radius, float startAngle,
                     float endAngle, Color c) {
    drawArc(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1,
            startAngle, endAngle, c);
  }
  void fillCircleSector(int cx, int cy, int radius, float startAngle,
                        float endAngle, Color c) {
    fillSector(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1,
               startAngle, endAngle, c);
  }
  void drawCircleArc(const vec2f &center, float radius, float startAngle,
                     float endAngle, Color c) {
    drawArc(circleRect(center, radius), startAngle, endAngle, c);
  }
  void fillCircleSector(const vec2f &center, float radius, float startAngle,
                        float endAngle, Color c) {
    fillSector(circleRect(center, radius), startAngle, endAngle, c);
  }

  // --- Lines and polygons --------------------------------------------------
  // Lines are one pixel wide and go from the pixel at one end point to the
  // pixel at the other, both included. Polygons are filled with the even-odd
  // rule, up to 32 crossings per row. Their vertices are pixels like the end
  // points of lines: the edges run through the centers of the vertex pixels
  // and a pixel is filled where its center is inside, on an edge only on the
  // left or top side. So the fill stays within the outline drawPolygon()
  // draws through the same vertices, and a polygon along the edges of a
  // rectangle (x, y)-(x + w, y + h) fills the pixels of Rect{x, y, w, h}.
  void drawLine(int x0, int y0, int x1, int y1, Color c);
  void drawLine(const vec2i &a, const vec2i &b, Color c) {
    drawLine(a.x, a.y, b.x, b.y, c);
  }
  void drawLine(const vec2f &a, const vec2f &b, Color c);
  void drawPolyline(const vec2i *points, int count, Color c);
  void drawPolyline(const vec2f *points, int count, Color c);
  void drawPolygon(const vec2i *points, int count, Color c);  // closed
  void drawPolygon(const vec2f *points, int count, Color c);
  void fillPolygon(const vec2i *points, int count, Color c);
  void fillPolygon(const vec2f *points, int count, Color c);
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, Color c) {
    const vec2i v[3] = {{x0, y0}, {x1, y1}, {x2, y2}};
    fillPolygon(v, 3, c);
  }
  void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, Color c) {
    const vec2i v[3] = {{x0, y0}, {x1, y1}, {x2, y2}};
    drawPolygon(v, 3, c);
  }
  void fillTriangle(const vec2f &a, const vec2f &b, const vec2f &c,
                    Color col) {
    const vec2f v[3] = {a, b, c};
    fillPolygon(v, 3, col);
  }
  void drawTriangle(const vec2f &a, const vec2f &b, const vec2f &c,
                    Color col) {
    const vec2f v[3] = {a, b, c};
    drawPolygon(v, 3, col);
  }

  // --- Images ----------------------------------------------------------------
  // Draw an image (any format), converting between formats, with the blend
  // mode, opacity and color key of the state. Nearest neighbor sampling
  // wherever the transform scales or turns it.
  void drawImage(const Texture &img, int dx, int dy) {
    drawImage(img, dx, dy, Rect{0, 0, img.width, img.height});
  }
  // The part `src` of the image with its top-left corner at (dx, dy). Parts
  // of `src` outside the image are not drawn and leave their place empty.
  void drawImage(const Texture &img, int dx, int dy, const Rect &src);
  // Scaled: `src` stretched over `dst`. A negative width or height of `dst`
  // mirrors the image in that direction. Sizes beyond 32767 draw nothing.
  void drawImage(const Texture &img, const Rect &dst, const Rect &src);
  void drawImage(const Texture &img, const Rect &dst) {
    drawImage(img, dst, Rect{0, 0, img.width, img.height});
  }
  void drawImage(const Texture &img, int dx, int dy, int dw, int dh, int sx,
                 int sy, int sw, int sh) {
    drawImage(img, Rect{dx, dy, dw, dh}, Rect{sx, sy, sw, sh});
  }
  // Draw a GRAY1 image as a two-color mask: set bits in `fg`, clear bits in
  // `bg` (TRANSPARENT leaves them untouched)
  void drawBitmap(const Texture &bitmap, int dx, int dy, Color fg,
                  Color bg = Colors::TRANSPARENT) {
    drawBitmap(bitmap, dx, dy, Rect{0, 0, bitmap.width, bitmap.height}, fg, bg);
  }
  void drawBitmap(const Texture &bitmap, int dx, int dy, const Rect &src,
                  Color fg, Color bg = Colors::TRANSPARENT);

  // --- Text ------------------------------------------------------------------
  // The cursor is the top-left corner of the line box; the baseline is
  // `ascent` below it. '\n' moves the cursor to the next line. To enlarge or
  // turn text, set a transform.
  void setFont(const GFXfont *font);
  const GFXfont *font() const { return state_.text.font; }
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

  // Draw one glyph with its line box at (x, y); returns the x advance
  int drawChar(int x, int y, int code);
  void drawString(const char *str);  // at the cursor, advancing it
  void drawString(int x, int y, const char *str) {
    setCursor(x, y);
    drawString(str);
  }
  // Size of one glyph (zero width if the font lacks it) and of a text
  // (widest line; '\n' starts a line). Zero without a font. charMetrics()
  // and textMetrics() measure in the coordinates of the drawing calls,
  // integer only; the device versions measure on the target, which takes
  // float math (software float on a core without an FPU).
  TextMetrics charMetrics(int code) const;
  TextMetrics textMetrics(const char *str) const;
  TextMetricsF deviceCharMetrics(int code) const;
  TextMetricsF deviceTextMetrics(const char *str) const;

 private:
  friend struct detail::G2Impl;

  static RectF circleRect(const vec2f &c, float r) {
    return {c.x - r, c.y - r, r * 2.0f + 1.0f, r * 2.0f + 1.0f};
  }

  Surface target_ = {PixelFormat::RGB565_SWAPPED, 0, 0, 0, nullptr};
  GraphicsState2D state_;
  GraphicsState2D *stack_ = nullptr;  // in the arena
  uint8_t *scratch_ = nullptr;        // the rest of the arena
  uint32_t scratchSize_ = 0, scratchTop_ = 0;
  int16_t stackTop_ = 0;
  // Derived from state_.transform by updateTransform()
  TransformKind kind_ = TransformKind::IDENTITY;
  int32_t ox_ = 0, oy_ = 0;  // IDENTITY / TRANSLATE: the offset in pixels

  void updateTransform();
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
