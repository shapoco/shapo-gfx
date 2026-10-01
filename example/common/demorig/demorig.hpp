#ifndef DEMORIG_DEMORIG_HPP
#define DEMORIG_DEMORIG_HPP

// demorig: the scene (scene.hpp) behind a view that zooms and scrolls, with
// zoom buttons and the frame rate drawn over it. Shared by the WASM build
// (example/wasm/demorig/) and the M5Stack ones (example/m5*/demorig/); it
// depends on ShapoGFX only.
//
//   Demo demo;
//   demo.init(w, h, true);
//   every frame:
//     demo.pointerDown / pointerMove / pointerUp as the input arrives
//     demo.update(seconds);
//     demo.draw(g, 0);                // the whole screen, or
//     demo.draw(gBand, bandY);        // one band of it per call
//
// Controls: the (+) / (-) buttons at the right edge double / halve the zoom
// (1/4 to 16 times, animated), the (AA) button at the left edge toggles
// antialiasing (of everything Graphics2D antialiases: the vector pictures
// and the area fills), and a drag elsewhere scrolls. The view center stays
// within the scene; the space around it may be seen.

#include "scene.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"

namespace demorig {

class Demo {
 public:
  static constexpr int ZOOM_MIN_LOG2 = -2;  // 1/4
  static constexpr int ZOOM_MAX_LOG2 = 4;   // 16

  // A screen (and scene) of width x height pixels. controls: show the zoom
  // buttons and take the pointer.
  void init(int width, int height, bool controls);

  // Pointer input in screen pixels (one pointer: further ones are ignored by
  // the callers)
  void pointerDown(int x, int y);
  void pointerMove(int x, int y);
  void pointerUp();

  // Jump to a view at once: zoom factor and the scene point at the center
  // of the screen (for checks from the command line)
  void setView(float zoom, float centerX, float centerY);

  // Advance to time t (seconds, increasing): the zoom animation, the frame
  // rate and the scene. Call once per frame, before draw().
  void update(float t);

  // Draw the screen rows [bandY, bandY + g.target().height) into g, which
  // must have an arena (Graphics2D::init). Const: several contexts may draw
  // different bands of one frame at once.
  void draw(shapoco::gfx2d::Graphics2D &g, int bandY) const;

  int width() const { return width_; }
  int height() const { return height_; }
  float fps() const { return fps_; }
  float zoom() const;
  bool antialias() const { return antialias_; }
  void setAntialias(bool on) { antialias_ = on; }

 private:
  enum class Button { NONE, ZOOM_IN, ZOOM_OUT, ANTIALIAS };

  Scene scene_;
  int width_ = 0, height_ = 0;
  bool controls_ = false;

  // View: zoom as log2 (animated towards the target) and the scene point at
  // the screen center
  float zoomLog2_ = 0.0f;
  int zoomTarget_ = 0;
  float centerX_ = 0.0f, centerY_ = 0.0f;

  // Pointer
  Button pressed_ = Button::NONE;
  bool dragging_ = false;
  int lastX_ = 0, lastY_ = 0;

  // Buttons: centers and radius in screen pixels
  int buttonR_ = 0;
  int zoomInX_ = 0, zoomInY_ = 0, zoomOutX_ = 0, zoomOutY_ = 0;
  int aaX_ = 0, aaY_ = 0;
  bool antialias_ = false;

  // Time and frame rate
  bool started_ = false;
  float lastT_ = 0.0f;
  float fpsT0_ = 0.0f;
  int fpsFrames_ = 0;
  float fps_ = 0.0f;
  char label_[32] = "";

  Button buttonAt(int x, int y) const;
  void clampCenter();
  void drawButton(shapoco::gfx2d::Graphics2D &g, int cx, int cy, bool plus,
                  bool down) const;
  void drawAAButton(shapoco::gfx2d::Graphics2D &g, bool down) const;
};

}  // namespace demorig

#endif
