#ifndef SHAPOGFX3D_ARCH_GENERIC_HPP
#define SHAPOGFX3D_ARCH_GENERIC_HPP

// Portable implementations of the architecture hooks (see arch.hpp). An
// architecture header may replace a hook by defining its SHAPOGFX3D_ARCH_HAS_*
// macro before this point; everything not replaced falls back to these.

#include <cstdint>

namespace shapoco::gfx3d::arch::generic {

// (a * b) >> sh with a 64-bit product (0 <= sh < 32)
static inline int32_t mulShift(int32_t a, int32_t b, int sh) {
  return (int32_t)(((int64_t)a * b) >> sh);
}

// Hardware state render() has to preserve for the caller
struct RenderState {
  void begin() {}
  void end() {}
};

}  // namespace shapoco::gfx3d::arch::generic

#endif
