#ifndef SHAPOGFX2D_VG_HPP
#define SHAPOGFX2D_VG_HPP

// Vector graphics: paths of lines and bezier curves filled with brushes (a
// color or a gradient) and stroked with a width, grouped into pictures.
// Graphics2D draws them (fillPath(), strokePath(), drawPicture()); the data is
// plain structs that live in flash, generated from SVG by bin/svg2cpp or
// written by hand. The names are those of vector formats in general (a path,
// a brush, a stroke), not of SVG alone.
//
// Coordinates are continuous like those of affine2f: a path along the edges
// of the rectangle (x, y)-(x + w, y + h) fills the pixels of Rect{x, y, w, h}.
// Curves are flattened when drawn, finely enough for the transform in force,
// so a picture scaled up keeps its round corners.

#include <cstddef>
#include <cstdint>

#include "shapoco/gfx2d/gfxfont.h"
#include "shapoco/gfx2d/math2d.hpp"
#include "shapoco/gfx2d/pixel.hpp"
#include "shapoco/gfx2d/surface.hpp"

namespace shapoco::gfx2d::vg {

// --- Room for later features -------------------------------------------------
//
// Like rig: generated headers initialize the structures by position, members
// are only appended, and zero means "not used" for every appended member.
// FORMAT_VERSION counts the members added this way; a generated header
// static_asserts on the version it needs. Picture::features holds bits for
// features defined later (none yet); drawPicture() draws nothing of a picture
// with a bit outside SUPPORTED_FEATURES.
constexpr uint16_t FORMAT_VERSION = 1;
constexpr uint16_t SUPPORTED_FEATURES = 0;

// --- Paths ---------------------------------------------------------------------

// A path is a list of ops with their coordinates: MOVE x y, LINE x y, QUAD
// cx cy x y, CUBIC c1x c1y c2x c2y x y, CLOSE. Every op but the first of a
// path continues from the current point; a LINE, QUAD or CUBIC right after
// CLOSE (or first) starts at the last MOVE. Filling closes every subpath;
// stroking closes only those ending in CLOSE.
enum class PathOp : uint8_t { MOVE, LINE, QUAD, CUBIC, CLOSE };

constexpr int pathOpCoords(PathOp op) {
  return op == PathOp::MOVE || op == PathOp::LINE ? 2
         : op == PathOp::QUAD                      ? 4
         : op == PathOp::CUBIC                     ? 6
                                                   : 0;
}

// Which points a path covers when there are overlaps: NONZERO (the default
// of SVG) counts the windings, EVEN_ODD alternates
enum class FillRule : uint8_t { NONZERO, EVEN_ODD };

struct Path {
  const uint8_t *ops;   // PathOp values
  const float *coords;  // pathOpCoords() of them per op
  uint16_t opCount;
  uint16_t coordCount;
  FillRule rule;
  uint8_t pad[3];
  // Of the points (control points included); empty: computed when needed
  RectF bounds;
};

// Bounding box of the points of a path (control points included)
RectF pathBounds(const Path &p);

// Builds a Path into arrays the caller provides. Ops beyond the capacity are
// dropped (overflowed() tells).
class PathBuilder {
 public:
  PathBuilder(uint8_t *ops, int opCapacity, float *coords, int coordCapacity)
      : ops_(ops), coords_(coords), opCap_(opCapacity), coordCap_(coordCapacity) {}
  PathBuilder &moveTo(float x, float y) { return op(PathOp::MOVE, x, y); }
  PathBuilder &lineTo(float x, float y) { return op(PathOp::LINE, x, y); }
  PathBuilder &quadTo(float cx, float cy, float x, float y) {
    return op(PathOp::QUAD, cx, cy, x, y);
  }
  PathBuilder &cubicTo(float c1x, float c1y, float c2x, float c2y, float x,
                       float y) {
    return op(PathOp::CUBIC, c1x, c1y, c2x, c2y, x, y);
  }
  PathBuilder &close() { return op(PathOp::CLOSE); }
  // Shapes, as subpaths (an ellipse is four cubics)
  PathBuilder &rect(float x, float y, float w, float h);
  PathBuilder &roundRect(float x, float y, float w, float h, float rx,
                         float ry);
  PathBuilder &ellipse(float cx, float cy, float rx, float ry);
  PathBuilder &circle(float cx, float cy, float r) {
    return ellipse(cx, cy, r, r);
  }
  // The arc of that ellipse from startAngle to endAngle (radians, clockwise
  // on screen, parametric like Graphics2D::drawArc()), as cubics of at most
  // a quarter turn: a line from the current point to its start (a move when
  // the path is empty), then the curves
  PathBuilder &arc(float cx, float cy, float rx, float ry, float startAngle,
                   float endAngle);
  PathBuilder &polyline(const vec2f *pts, int n, bool closed);

  bool overflowed() const { return overflow_; }
  int opCount() const { return nOps_; }
  int coordCount() const { return nCoords_; }
  // The path built so far (bounds included)
  Path path(FillRule rule = FillRule::NONZERO) const;

 private:
  uint8_t *ops_;
  float *coords_;
  int opCap_, coordCap_;
  int nOps_ = 0, nCoords_ = 0;
  bool overflow_ = false;
  PathBuilder &op(PathOp o, float a = 0, float b = 0, float c = 0, float d = 0,
                  float e = 0, float f = 0);
};

// --- Brushes -------------------------------------------------------------------

enum class GradientKind : uint8_t { LINEAR, RADIAL };
// What a gradient shows beyond its ends: its end colors, itself mirrored, or
// itself again
enum class Spread : uint8_t { PAD, REFLECT, REPEAT };

struct GradientStop {
  float offset;  // 0..1, in order
  Color color;
};

// A gradient in its own space: LINEAR runs along x from 0 to 1, RADIAL from
// the origin to the unit circle. `toGradient` maps the coordinates of the
// drawing calls (before the transform of Graphics2D) to that space, so a
// gradient between two points, an ellipse or a tilted one is all in it;
// linearGradient() and radialGradient() build the common ones.
struct Gradient {
  GradientKind kind;
  Spread spread;
  uint8_t stopCount;  // 1..255; 0 draws nothing
  uint8_t pad;
  const GradientStop *stops;
  affine2f toGradient;
};

// From p0 (offset 0) to p1 (offset 1)
Gradient linearGradient(const vec2f &p0, const vec2f &p1,
                        const GradientStop *stops, int stopCount,
                        Spread spread = Spread::PAD);
// Around `center` out to `radius`
Gradient radialGradient(const vec2f &center, float radius,
                        const GradientStop *stops, int stopCount,
                        Spread spread = Spread::PAD);

// What fills a shape: a color, or a gradient (the alpha of `color` then
// scales the gradient's; its RGB is ignored). Alpha 0 without a gradient
// draws nothing.
struct Brush {
  Color color;
  const Gradient *gradient;
};

constexpr Brush solidBrush(Color c) { return {c, nullptr}; }
constexpr Brush gradientBrush(const Gradient *g, int opacity = 255) {
  return {makeColor(0, 0, 0, opacity), g};
}
constexpr Brush NO_BRUSH = {Colors::TRANSPARENT, nullptr};

// --- Strokes -------------------------------------------------------------------

enum class LineCap : uint8_t { BUTT, ROUND, SQUARE };
enum class LineJoin : uint8_t { MITER, ROUND, BEVEL };

// The outline of a stroke: `width` wide, centered on the path, in the
// coordinates of the drawing calls (so the transform scales it). Joins
// beyond `miterLimit` x width / 2 from the corner are beveled.
struct StrokeStyle {
  float width;
  LineCap cap;
  LineJoin join;
  uint8_t pad[2];
  float miterLimit;  // 4 is SVG's default
};

constexpr StrokeStyle strokeStyle(float width, LineCap cap = LineCap::BUTT,
                                  LineJoin join = LineJoin::MITER,
                                  float miterLimit = 4.0f) {
  return {width, cap, join, {0, 0}, miterLimit};
}

// --- Pictures ------------------------------------------------------------------

// What a shape of a picture draws, by Shape::data
enum class ShapeKind : uint8_t {
  PATH,   // a Path, filled with `fill` and stroked with `stroke`
  IMAGE,  // a Texture, its pixels at (0, 0)-(width, height) of the shape's
          // space, with the alpha of `fill.color` as its opacity
  TEXT,   // a Text drawn with its bitmap font in `fill.color` (svg2cpp
          // outlines text when it has the font; this is its fallback)
};

// Bits of Shape::flags
enum ShapeFlags : uint8_t {
  // The brush takes the color drawPicture() is given (SVG's currentColor;
  // the color of the slot in rig) in place of its own; its alpha and a
  // gradient stay
  SHAPE_FILL_CURRENT_COLOR = 1,
  SHAPE_STROKE_CURRENT_COLOR = 2,
  // The stroke takes the width drawPicture() is given (the stroke width of
  // the slot in rig, which an animation changes) in place of its own
  SHAPE_STROKE_CURRENT_WIDTH = 4,
};

struct Text {
  const char *text;
  const GFXfont *font;  // nullptr: the font of the Graphics2D
  float x, y;           // the baseline starts here
};

struct Shape {
  ShapeKind kind;
  uint8_t flags;  // ShapeFlags
  uint8_t pad[2];
  const void *data;  // const Path *, const Texture * or const Text * by `kind`
  Brush fill;
  Brush stroke;  // PATH only; NO_BRUSH or width 0: not stroked
  StrokeStyle strokeStyle;
  affine2f transform;  // the shape's space to the picture's
  // In the picture's space, nullptr: none. Drawn as the clip rectangle of
  // Graphics2D, so a turned one clips to its bounding box.
  const RectF *clip;
};

struct Picture {
  const Shape *shapes;  // in drawing order
  uint16_t shapeCount;
  uint16_t features;  // bits to be defined; 0 today
  RectF bounds;       // in the picture's space, for reference
};

}  // namespace shapoco::gfx2d::vg

#endif
