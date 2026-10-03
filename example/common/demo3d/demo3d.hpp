#ifndef DEMO3D_DEMO3D_HPP
#define DEMO3D_DEMO3D_HPP

// demo3d on a touch screen: the scene (scene.hpp) behind a camera turned by
// dragging, with zoom buttons and the frame rate drawn over it. The browser
// build drives the camera from JavaScript instead (docs/example/viewer.js).
// Depends on ShapoGFX only.
//
//   Demo demo;
//   demo.init(w, h);
//   every frame:
//     demo.pointerDown / pointerMove / pointerUp as the input arrives
//     demo.update(seconds, g3d);          // builds the scene
//     g3d.beginRender();
//     for each band (on any core):
//       demo.drawBackdrop(g, bandY);
//       g3d.render(ctx, 0, bandY, w, rows, target, 0, 0);
//       demo.drawOverlay(g, bandY);
//     g3d.endRender();
//
// Controls: a drag turns the camera about the scene (yaw and pitch), the (+)
// / (-) buttons at the right edge move it closer / farther (animated).

#include "scene.hpp"
#include "shapoco/gfx2d/graphics2d.hpp"
#include "shapoco/gfx3d/gfx3d.hpp"

namespace demo3d {

class Demo {
 public:
  // A screen of width x height pixels
  void init(int width, int height);

  // Pointer input in screen pixels (one pointer)
  void pointerDown(int x, int y);
  void pointerMove(int x, int y);
  void pointerUp();

  // Advance to time t (seconds, increasing): the camera, the frame rate, and
  // the scene built into r (beginScene() .. endScene()). Call once per
  // frame, before drawing.
  void update(float t, shapoco::gfx3d::Graphics3D &r);

  // Draw the backdrop / the overlay (buttons and frame rate) of the screen
  // rows [bandY, bandY + g.target().height) into g. Const: several contexts
  // may draw different bands of one frame at once.
  void drawBackdrop(shapoco::gfx2d::Graphics2D &g, int bandY) const;
  void drawOverlay(shapoco::gfx2d::Graphics2D &g, int bandY) const;

  int width() const { return width_; }
  int height() const { return height_; }
  float fps() const { return fps_; }

 private:
  enum class Button { NONE, ZOOM_IN, ZOOM_OUT };

  int width_ = 0, height_ = 0;
  float t_ = 0.0f;

  // Camera: the distance as log2 (animated towards the target)
  float yaw_ = CAM_YAW_INIT, pitch_ = CAM_PITCH_INIT;
  float distLog2_ = 0.0f, distTarget_ = 0.0f;

  // Pointer
  Button pressed_ = Button::NONE;
  bool dragging_ = false;
  int lastX_ = 0, lastY_ = 0;

  // Buttons: centers and radius in screen pixels
  int buttonR_ = 0;
  int zoomInX_ = 0, zoomInY_ = 0, zoomOutX_ = 0, zoomOutY_ = 0;

  // Time and frame rate
  bool started_ = false;
  float lastT_ = 0.0f;
  float fpsT0_ = 0.0f;
  int fpsFrames_ = 0;
  float fps_ = 0.0f;
  char label_[24] = "";

  Button buttonAt(int x, int y) const;
  void drawButton(shapoco::gfx2d::Graphics2D &g, int cx, int cy, bool plus,
                  bool down) const;
};

}  // namespace demo3d

#endif
