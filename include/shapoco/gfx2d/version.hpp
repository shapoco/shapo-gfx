#ifndef SHAPOGFX2D_VERSION_HPP
#define SHAPOGFX2D_VERSION_HPP

// Version of ShapoGFX. gfx2d and gfx3d are released together and share this
// one number. Keep it in sync with "version" in library.json.
#define SHAPOGFX_VERSION_MAJOR 1
#define SHAPOGFX_VERSION_MINOR 0
#define SHAPOGFX_VERSION_PATCH 0
#define SHAPOGFX_VERSION_STRING "1.0.0"

// Single integer 0x00MMmmpp for #if comparisons, e.g.
//   #if SHAPOGFX_VERSION >= SHAPOGFX_MAKE_VERSION(1, 2, 0)
#define SHAPOGFX_MAKE_VERSION(major, minor, patch) \
  (((major) << 16) | ((minor) << 8) | (patch))
#define SHAPOGFX_VERSION                                              \
  SHAPOGFX_MAKE_VERSION(SHAPOGFX_VERSION_MAJOR, SHAPOGFX_VERSION_MINOR, \
                        SHAPOGFX_VERSION_PATCH)

namespace shapoco::gfx {

inline constexpr int VERSION_MAJOR = SHAPOGFX_VERSION_MAJOR;
inline constexpr int VERSION_MINOR = SHAPOGFX_VERSION_MINOR;
inline constexpr int VERSION_PATCH = SHAPOGFX_VERSION_PATCH;
inline constexpr int VERSION = SHAPOGFX_VERSION;
inline constexpr const char* VERSION_STRING = SHAPOGFX_VERSION_STRING;

}  // namespace shapoco::gfx

#endif
