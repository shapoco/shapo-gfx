#include "demorig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shapoco/gfx2d/font/ShapoSansP_s12c09a01w02.h"

namespace demorig {

namespace g2 = shapoco::gfx2d;

// Rate of the zoom animation: the remaining distance (in log2) shrinks by
// e^-ZOOM_RATE per second, so a step is mostly done in a quarter second
static constexpr float ZOOM_RATE = 14.0f;

void Demo::init(int width, int height, bool controls) {
  width_ = width;
  height_ = height;
  controls_ = controls;
  scene_.init(width, height);
  centerX_ = width / 2.0f;
  centerY_ = height / 2.0f;
  zoomLog2_ = 0.0f;
  zoomTarget_ = 0;

  // Buttons at the bottom of the right edge, (+) above (-), large enough
  // for a finger on the 2-inch screen of the M5Stack CoreS3
  buttonR_ = std::max(24, height / 10);
  const int margin = buttonR_ / 2;
  zoomOutX_ = zoomInX_ = width - margin - buttonR_;
  zoomOutY_ = height - margin - buttonR_;
  zoomInY_ = zoomOutY_ - buttonR_ * 2 - margin;
  // The antialiasing button at the bottom of the left edge
  aaX_ = margin + buttonR_;
  aaY_ = zoomOutY_;
}

float Demo::zoom() const { return std::exp2(zoomLog2_); }

Demo::Button Demo::buttonAt(int x, int y) const {
  if (!controls_) return Button::NONE;
  // A little larger than drawn
  const int r = buttonR_ * 5 / 4;
  auto inside = [&](int cx, int cy) {
    const int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
  };
  if (inside(zoomInX_, zoomInY_)) return Button::ZOOM_IN;
  if (inside(zoomOutX_, zoomOutY_)) return Button::ZOOM_OUT;
  if (inside(aaX_, aaY_)) return Button::ANTIALIAS;
  return Button::NONE;
}

void Demo::pointerDown(int x, int y) {
  if (!controls_) return;
  pressed_ = buttonAt(x, y);
  if (pressed_ == Button::ZOOM_IN) {
    zoomTarget_ = std::min(zoomTarget_ + 1, ZOOM_MAX_LOG2);
  } else if (pressed_ == Button::ZOOM_OUT) {
    zoomTarget_ = std::max(zoomTarget_ - 1, ZOOM_MIN_LOG2);
  } else if (pressed_ == Button::ANTIALIAS) {
    antialias_ = !antialias_;
  } else {
    dragging_ = true;
    lastX_ = x;
    lastY_ = y;
    pointerHover(x, y);
  }
}

void Demo::pointerHover(int x, int y) {
  hover_ = true;
  hoverX_ = x;
  hoverY_ = y;
}

void Demo::pointerMove(int x, int y) {
  if (!dragging_) return;
  pointerHover(x, y);
  // The scene follows the finger
  const float z = zoom();
  centerX_ -= (x - lastX_) / z;
  centerY_ -= (y - lastY_) / z;
  lastX_ = x;
  lastY_ = y;
  clampCenter();
}

void Demo::pointerUp() {
  pressed_ = Button::NONE;
  dragging_ = false;
  // (a mouse points again with its next move)
  hover_ = false;
}

void Demo::setView(float zoom, float centerX, float centerY) {
  zoomLog2_ = std::log2(zoom);
  zoomTarget_ = std::clamp((int)std::lround(zoomLog2_), ZOOM_MIN_LOG2,
                           ZOOM_MAX_LOG2);
  centerX_ = centerX;
  centerY_ = centerY;
}

void Demo::clampCenter() {
  centerX_ = std::clamp(centerX_, 0.0f, (float)width_);
  centerY_ = std::clamp(centerY_, 0.0f, (float)height_);
}

void Demo::update(float t) {
  if (!started_) {
    started_ = true;
    lastT_ = fpsT0_ = t;
  }
  const float dt = std::clamp(t - lastT_, 0.0f, 0.1f);
  lastT_ = t;

  // Zoom about the screen center, evenly in log2
  const float diff = zoomTarget_ - zoomLog2_;
  if (std::fabs(diff) < 0.002f) {
    zoomLog2_ = (float)zoomTarget_;
  } else {
    zoomLog2_ += diff * (1.0f - std::exp(-dt * ZOOM_RATE));
  }

  fpsFrames_++;
  if (t - fpsT0_ >= 0.5f) {
    fps_ = fpsFrames_ / (t - fpsT0_);
    fpsFrames_ = 0;
    fpsT0_ = t;
  }
  // Tenths by hand: printf may lack floating point on the targets
  const int fps10 = (int)(fps_ * 10 + 0.5f);
  if (zoomTarget_ >= 0) {
    std::snprintf(label_, sizeof(label_), "%d.%d fps  x%d", fps10 / 10,
                  fps10 % 10, 1 << zoomTarget_);
  } else {
    std::snprintf(label_, sizeof(label_), "%d.%d fps  x1/%d", fps10 / 10,
                  fps10 % 10, 1 << -zoomTarget_);
  }

  // The pointer to world coordinates: the view of draw() undone
  if (hover_) {
    const float z = zoom();
    scene_.setPointer((hoverX_ + 0.5f - width_ / 2.0f) / z + centerX_,
                      (hoverY_ + 0.5f - height_ / 2.0f) / z + centerY_);
  } else {
    scene_.clearPointer();
  }
  scene_.update(t);
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

// (AA): lit when antialiasing is on
void Demo::drawAAButton(g2::Graphics2D &g, bool down) const {
  const int r = buttonR_;
  const bool on = antialias_;
  g.fillCircle(aaX_, aaY_, r,
               down ? g2::makeColor(255, 255, 255, 110)
                    : (on ? g2::makeColor(255, 255, 255, 60)
                          : g2::makeColor(0, 0, 0, 110)));
  const g2::Color line = g2::makeColor(255, 255, 255, 220);
  g.drawCircle(aaX_, aaY_, r, line);
  g.setFont(&g2::ShapoSansP_s12c09a01w02);
  const g2::TextMetrics m = g.textMetrics("AA");
  g.setTextColor(on ? line : g2::makeColor(255, 255, 255, 140));
  g.drawString(aaX_ - m.width / 2, aaY_ - m.height / 2, "AA");
}

void Demo::draw(g2::Graphics2D &g, int bandY) const {
  g.resetClipRect();
  // Everything drawn with or without antialiasing, the overlay included
  g.setAntialias(antialias_);
  // View: the center point of the scene to the center of the screen
  const float z = zoom();
  g.setTransform(g2::affine2f::translation(width_ / 2.0f, height_ / 2.0f - bandY));
  g.scale(z, z);
  g.translate(-centerX_, -centerY_);
  scene_.draw(g);

  // Overlay, in screen pixels
  g.setTransform(g2::affine2f::translation(0, -bandY));
  g.setBlend(g2::BlendMode::ALPHA, 255);
  if (controls_) {
    drawButton(g, zoomInX_, zoomInY_, true, pressed_ == Button::ZOOM_IN);
    drawButton(g, zoomOutX_, zoomOutY_, false, pressed_ == Button::ZOOM_OUT);
    drawAAButton(g, pressed_ == Button::ANTIALIAS);
  }
  g.setFont(&g2::ShapoSansP_s12c09a01w02);
  g.setTextColor(g2::makeColor(0, 0, 0));
  g.drawString(5, 5, label_);
  g.setTextColor(g2::makeColor(255, 255, 255));
  g.drawString(4, 4, label_);
}

}  // namespace demorig
