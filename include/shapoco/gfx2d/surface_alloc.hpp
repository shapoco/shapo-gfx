#ifndef SHAPOGFX2D_SURFACE_ALLOC_HPP
#define SHAPOGFX2D_SURFACE_ALLOC_HPP

// Optional convenience header: a Surface that owns its pixel buffer on the
// heap. The core library never allocates memory and works on caller-provided
// buffers; include this header only in application code that wants automatic
// lifetime management (e.g. off-screen buffers created at startup).

#include <cstdint>
#include <memory>
#include <utility>

#include "shapoco/gfx2d/surface.hpp"

namespace shapoco::gfx2d {

// Heap-allocated, zero-initialized pixel buffer wrapped in a Surface.
// Movable, not copyable. The Surface it exposes stays valid as long as the
// OwnedSurface object is alive and not moved from.
class OwnedSurface {
 public:
  OwnedSurface() = default;

  OwnedSurface(PixelFormat format, int width, int height)
      : storage_(new uint8_t[surfaceBytes(format, width, height)]()),
        surface_{format, (int16_t)width, (int16_t)height,
                 minStride(format, width), storage_.get()} {}

  OwnedSurface(const OwnedSurface &) = delete;
  OwnedSurface &operator=(const OwnedSurface &) = delete;

  OwnedSurface(OwnedSurface &&o) noexcept
      : storage_(std::move(o.storage_)), surface_(o.surface_) {
    o.surface_ = EMPTY;
  }
  OwnedSurface &operator=(OwnedSurface &&o) noexcept {
    if (this != &o) {
      storage_ = std::move(o.storage_);
      surface_ = o.surface_;
      o.surface_ = EMPTY;
    }
    return *this;
  }

  bool valid() const { return surface_.pixels != nullptr; }
  const Surface &surface() const { return surface_; }
  operator const Surface &() const { return surface_; }
  operator Texture() const { return surface_.asTexture(); }

  PixelFormat format() const { return surface_.format; }
  int width() const { return surface_.width; }
  int height() const { return surface_.height; }
  uint32_t stride() const { return surface_.stride; }
  void *pixels() const { return surface_.pixels; }
  size_t bytes() const {
    return (size_t)surface_.stride * (size_t)surface_.height;
  }

 private:
  static constexpr Surface EMPTY = {PixelFormat::RGB565BE, 0, 0, 0, nullptr};
  std::unique_ptr<uint8_t[]> storage_;
  Surface surface_ = EMPTY;
};

static inline OwnedSurface createSurface(PixelFormat format, int width,
                                         int height) {
  return OwnedSurface(format, width, height);
}

}  // namespace shapoco::gfx2d

#endif
