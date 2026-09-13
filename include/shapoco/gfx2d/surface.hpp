#ifndef SHAPOGFX2D_SURFACE_HPP
#define SHAPOGFX2D_SURFACE_HPP

#include <cstdint>

#include "shapoco/gfx2d/pixel.hpp"

namespace shapoco::gfx2d {

// Read-only image: textures for the 3D renderer, source images for the 2D API.
// Pixel data is typically placed in flash (const). Rows are `stride` bytes
// apart and each row starts on a byte boundary.
//
// The 3D renderer requires width and height to be powers of two (texture
// coordinates are wrapped with a bit mask).
struct Texture {
  PixelFormat format;
  int16_t width;
  int16_t height;
  uint32_t stride;  // bytes per row
  const void *pixels;

  const uint8_t *linePtr(int y) const {
    return (const uint8_t *)pixels + (size_t)y * stride;
  }
};

// Writable image: the target of the 2D API and the output of the 3D renderer.
struct Surface {
  PixelFormat format;
  int16_t width;
  int16_t height;
  uint32_t stride;  // bytes per row
  void *pixels;

  uint8_t *linePtr(int y) const {
    return (uint8_t *)pixels + (size_t)y * stride;
  }

  Texture asTexture() const { return {format, width, height, stride, pixels}; }
  operator Texture() const { return asTexture(); }
};

// Convenience constructors. stride = 0 selects the minimum stride.
static inline Texture makeTexture(PixelFormat format, int width, int height,
                                  const void *pixels, uint32_t stride = 0) {
  return {format, (int16_t)width, (int16_t)height,
          stride ? stride : minStride(format, width), pixels};
}

static inline Surface makeSurface(PixelFormat format, int width, int height,
                                  void *pixels, uint32_t stride = 0) {
  return {format, (int16_t)width, (int16_t)height,
          stride ? stride : minStride(format, width), pixels};
}

// Bytes needed for a tightly packed image
constexpr size_t surfaceBytes(PixelFormat format, int width, int height) {
  return (size_t)minStride(format, width) * (size_t)height;
}

}  // namespace shapoco::gfx2d

#endif
