#include "demo3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shapoco/gfx2d/fonts.hpp"

namespace demo3d {

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;

// Radians per pixel of a drag (the browser's 0.008, a little more for the
// smaller screens this is made for)
static constexpr float DRAG_RATE = 0.012f;
// A button press multiplies the distance by 2^(+-ZOOM_STEP)
static constexpr float ZOOM_STEP = 0.5f;
// Rate of the zoom animation: the remaining distance (in log2) shrinks by
// e^-ZOOM_RATE per second
static constexpr float ZOOM_RATE = 14.0f;

void Demo::init(int width, int height) {
  width_ = width;
  height_ = height;
  sceneInit();
  distLog2_ = distTarget_ = std::log2(CAM_DIST_INIT);

  // Buttons at the bottom of the right edge, (+) above (-), as in demorig
  buttonR_ = std::max(24, height / 10);
  const int margin = buttonR_ / 2;
  zoomOutX_ = zoomInX_ = width - margin - buttonR_;
  zoomOutY_ = height - margin - buttonR_;
  zoomInY_ = zoomOutY_ - buttonR_ * 2 - margin;
}

Demo::Button Demo::buttonAt(int x, int y) const {
  // A little larger than drawn
  const int r = buttonR_ * 5 / 4;
  auto inside = [&](int cx, int cy) {
    const int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
  };
  if (inside(zoomInX_, zoomInY_)) return Button::ZOOM_IN;
  if (inside(zoomOutX_, zoomOutY_)) return Button::ZOOM_OUT;
  return Button::NONE;
}

void Demo::pointerDown(int x, int y) {
  pressed_ = buttonAt(x, y);
  const float lo = std::log2(CAM_DIST_MIN), hi = std::log2(CAM_DIST_MAX);
  if (pressed_ == Button::ZOOM_IN) {
    distTarget_ = std::max(distTarget_ - ZOOM_STEP, lo);
  } else if (pressed_ == Button::ZOOM_OUT) {
    distTarget_ = std::min(distTarget_ + ZOOM_STEP, hi);
  } else {
    dragging_ = true;
    lastX_ = x;
    lastY_ = y;
  }
}

void Demo::pointerMove(int x, int y) {
  if (!dragging_) return;
  yaw_ += (x - lastX_) * DRAG_RATE;
  pitch_ = std::clamp(pitch_ + (y - lastY_) * DRAG_RATE, CAM_PITCH_MIN,
                      CAM_PITCH_MAX);
  lastX_ = x;
  lastY_ = y;
}

void Demo::pointerUp() {
  pressed_ = Button::NONE;
  dragging_ = false;
}

void Demo::update(float t, g3::Graphics3D &r) {
  if (!started_) {
    started_ = true;
    lastT_ = fpsT0_ = t;
  }
  const float dt = std::clamp(t - lastT_, 0.0f, 0.1f);
  lastT_ = t;
  t_ = t;

  const float diff = distTarget_ - distLog2_;
  if (std::fabs(diff) < 0.002f) {
    distLog2_ = distTarget_;
  } else {
    distLog2_ += diff * (1.0f - std::exp(-dt * ZOOM_RATE));
  }

  fpsFrames_++;
  if (t - fpsT0_ >= 0.5f) {
    fps_ = fpsFrames_ / (t - fpsT0_);
    fpsFrames_ = 0;
    fpsT0_ = t;
  }
  // Tenths by hand: printf may lack floating point on the targets
  const int fps10 = (int)(fps_ * 10 + 0.5f);
  std::snprintf(label_, sizeof(label_), "%d.%d fps", fps10 / 10, fps10 % 10);

  sceneBuild(r, t, yaw_, pitch_, std::exp2(distLog2_),
             (float)width_ / height_);
}

void Demo::drawBackdrop(g2::Graphics2D &g, int bandY) const {
  g.resetClipRect();
  g.setTransform(g2::affine2f::translation(0.0f, (float)-bandY));
  g.setBlend(g2::BlendMode::ALPHA, 255);
  demo3d::drawBackdrop(g, width_, height_, t_);
}

void Demo::drawButton(g2::Graphics2D &g, int cx, int cy, bool plus,
                      bool down) const {
  const int r = buttonR_;
  g.fillCircle(cx, cy, r, down ? g2::makeColor(255, 255, 255, 110)
                               : g2::makeColor(0, 0, 0, 110));
  const g2::Color line = g2::makeColor(255, 255, 255, 220);
  g.drawCircle(cx, cy, r, line);
  const int arm = r / 2, th = std::max(2, r / 6);
  g.fillRect(cx - arm, cy - th / 2, arm * 2, th, line);
  if (plus) {
    // In three pieces so that the middle is not blended twice
    g.fillRect(cx - th / 2, cy - arm, th, arm - th / 2, line);
    g.fillRect(cx - th / 2, cy - th / 2 + th, th, arm - th + th / 2, line);
  }
}

void Demo::drawOverlay(g2::Graphics2D &g, int bandY) const {
  g.resetClipRect();
  g.setTransform(g2::affine2f::translation(0.0f, (float)-bandY));
  g.setBlend(g2::BlendMode::ALPHA, 255);
  drawButton(g, zoomInX_, zoomInY_, true, pressed_ == Button::ZOOM_IN);
  drawButton(g, zoomOutX_, zoomOutY_, false, pressed_ == Button::ZOOM_OUT);
  // Frame rate at the bottom left
  g.setFont(&g2::ShapoSansP_s12c09a01w02);
  const int y = height_ - 4 - g.textMetrics(label_).height;
  g.setTextColor(g2::makeColor(0, 0, 0));
  g.drawString(5, y + 1, label_);
  g.setTextColor(g2::makeColor(255, 255, 255));
  g.drawString(4, y, label_);
}

}  // namespace demo3d
