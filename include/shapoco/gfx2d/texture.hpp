#ifndef SHAPOGFX2D_TEXTURE_HPP
#define SHAPOGFX2D_TEXTURE_HPP

#include <cstdint>

namespace shapoco::gfx2d {

// Read-only RGB565 image. Pixel data is typically placed in flash (const).
// Note: the 3D renderer requires width and height to be powers of two
// (texture coordinates are wrapped with a bit mask).
struct Texture {
  int16_t width;
  int16_t height;
  const uint16_t *pixels;  // RGB565, row-major, width * height entries
};

}  // namespace shapoco::gfx2d

#endif
