#ifndef SHAPOGFX2D_RIG_HPP
#define SHAPOGFX2D_RIG_HPP

// 2D skeletal animation: armatures of bones carrying images, posed from
// keyframed animations and drawn with Graphics2D. The data (Armature,
// Animation) is static and lives in flash; bin/dbones2cpp generates it from
// DragonBones. An Instance keeps the pose of one character in memory the user
// provides (Instance::bytes()). Optional: gfx2d.hpp does not include this
// header.
//
//   alignas(4) static uint8_t mem[...];  // >= rig::Instance::bytes(chara::armature)
//   rig::Instance inst;
//   inst.init(chara::armature, mem, sizeof mem);
//   inst.pose(chara::anim_walk, rig::frameAt(chara::anim_walk, seconds));
//   g.setTransform(affine2f::translation(x, y));  // placement of the armature
//   inst.draw(g);
//
// Conventions (those of DragonBones): y points down, angles turn clockwise on
// screen, a transform maps x' = a x + c y + tx like affine2f. A bone's local
// transform is
//   a = cos(rotY) scaleX, b = sin(rotY) scaleX,
//   c = -sin(rotX) scaleY, d = cos(rotX) scaleY, (tx, ty) = (x, y)
// (rotX == rotY: a rotation; otherwise a skew) and its world transform is its
// parent's world transform times the local one.

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "shapoco/gfx2d/graphics2d.hpp"

namespace shapoco::gfx2d::rig {

// 1/65536 of a turn: the difference of two angles wraps to the shortest way
using angle16_t = int16_t;
// Q12: 4096 = 1.0
using scale16_t = int16_t;

constexpr scale16_t SCALE_ONE = 4096;
constexpr uint8_t NO_PARENT = 0xFF;
constexpr int MAX_BONES = 255;  // bone and slot indices are uint8_t
constexpr int MAX_SLOTS = 255;
constexpr int MAX_ATTACHMENTS = 127;

// --- Armature (static data) ---------------------------------------------------

struct Bone {
  const char *name;
  float x, y;             // origin in the parent's space (bind pose)
  angle16_t rotX, rotY;   // DragonBones skX / skY
  scale16_t scaleX, scaleY;
  uint8_t parent;         // a smaller index, or NO_PARENT
};

// An image a slot can show (a DragonBones "display")
struct Attachment {
  const Texture *texture;  // nullptr: not drawn (an unsupported display)
  Rect src;                // the part of `texture` (a texture atlas)
  affine2f local;          // the top-left corner of `src` to the bone's space
};

struct Slot {
  const char *name;
  const Attachment *attachments;
  uint8_t attachmentCount;
  int8_t defaultAttachment;  // -1: hidden
  uint8_t bone;
  uint8_t alpha;             // 0..255
  BlendMode blend;           // ALPHA or ADD
};

struct Armature {
  const char *name;
  const Bone *bones;  // parents before children
  const Slot *slots;  // in the base draw order
  uint8_t boneCount;
  uint8_t slotCount;
  bool colorKeyEnabled;  // the images are RGB565 with this key color
  Color colorKey;
  RectF bounds;          // of the bind pose, for reference
  uint32_t signature;    // of the bone and slot names (see Animation)
};

// --- Animation (static data) ----------------------------------------------------
//
// Each timeline is one channel of one bone or slot: keys in frame order, the
// first at frame 0. The key values of bones are offsets added to the bind pose
// (scales multiply it); those of slots replace the slot's values. `curve` is
// the easing from a key to the next: CURVE_LINEAR, CURVE_STEP (hold) or the
// index of a table in Animation::curves.

// Easing y(x) sampled at x = i / 16, Q14 (y[0] = 0, y[16] = 16384)
struct Curve {
  int16_t y[17];
};
constexpr uint8_t CURVE_LINEAR = 0xFF;
constexpr uint8_t CURVE_STEP = 0xFE;

struct TranslateKey {
  uint16_t frame;
  uint8_t curve;
  uint8_t pad;
  float x, y;
};
struct RotateKey {
  uint16_t frame;
  uint8_t curve;
  uint8_t pad;
  angle16_t rotX, rotY;
};
struct ScaleKey {
  uint16_t frame;
  uint8_t curve;
  uint8_t pad;
  scale16_t scaleX, scaleY;  // factors of the bind pose's
};
struct AttachmentKey {  // not interpolated
  uint16_t frame;
  int8_t attachment;  // -1: hidden
  uint8_t pad;
};
struct AlphaKey {
  uint16_t frame;
  uint8_t curve;
  uint8_t alpha;
};

enum class Channel : uint8_t { TRANSLATE, ROTATE, SCALE, ATTACHMENT, ALPHA };

struct BoneTimeline {  // TRANSLATE, ROTATE or SCALE
  const void *keys;    // TranslateKey / RotateKey / ScaleKey by the channel
  uint16_t keyCount;
  uint8_t bone;
  Channel channel;
};
struct SlotTimeline {  // ATTACHMENT or ALPHA
  const void *keys;
  uint16_t keyCount;
  uint8_t slot;
  Channel channel;
};
struct DrawOrderKey {
  uint16_t frame;
  const uint8_t *order;  // draw position -> slot (slotCount), nullptr: base
};

struct Animation {
  const char *name;
  uint16_t duration;  // frames; poses are taken in [0, duration]
  uint8_t frameRate;  // frames per second
  uint8_t boneTimelineCount;
  uint8_t slotTimelineCount;
  uint8_t curveCount;
  uint16_t drawOrderKeyCount;
  const BoneTimeline *boneTimelines;  // by bone, then channel
  const SlotTimeline *slotTimelines;  // by slot, then channel
  const DrawOrderKey *drawOrderKeys;  // by frame
  const Curve *curves;
  uint32_t signature;  // Armature::signature of the armature it animates
};

// The frame at `seconds` of playing from frame 0; with `loop`, wrapped to
// [0, duration), otherwise clamped to [0, duration]
inline float frameAt(const Animation &a, float seconds, bool loop = true) {
  const float f = seconds * (float)a.frameRate;
  const float d = (float)a.duration;
  if (!(d > 0.0f)) return 0.0f;
  if (loop) {
    const float r = std::fmod(f, d);
    return r < 0.0f ? r + d : r;
  }
  return f < 0.0f ? 0.0f : (f > d ? d : f);
}

// --- Instance -------------------------------------------------------------------

// A bone's local pose, bind pose and animation combined
struct BonePose {
  float x, y;
  angle16_t rotX, rotY;
  scale16_t scaleX, scaleY;
};

// Procedural changes to the pose (turn a head toward a point, ...)
class BoneVisitor {
 public:
  virtual ~BoneVisitor() = default;
  // Called per bone, parents first, before its world transform is computed
  virtual void onBone(int bone, BonePose &local) = 0;
};

// The pose of one armature: the world transforms of the bones, the state of
// the slots and the draw order, in memory the user provides. No memory is
// allocated. Copying an Instance makes a second handle to the same memory.
class Instance {
 public:
  // Memory init() needs for the armature (bone transforms, slot states, draw
  // order), alignment slack included
  static size_t bytes(const Armature &a);
  // False (and not initialized) if `size` is too small. Starts in the bind
  // pose.
  bool init(const Armature &a, void *memory, size_t size);
  void deinit();
  bool isInitialized() const { return arm_ != nullptr; }
  const Armature *armature() const { return arm_; }

  // Pose at `frame` (fractions interpolate; clamped to [0, duration]). False
  // (and nothing changes) if the animation belongs to another armature.
  bool pose(const Animation &anim, float frame, BoneVisitor *visitor = nullptr);
  void poseBind(BoneVisitor *visitor = nullptr);  // without animation

  // Draw with the transform of `g` as the placement of the armature (its
  // clip, blend mode and opacity apply too). Restores the transform,
  // opacity, blend mode and color key of `g` afterwards. Needs
  // SHAPOGFX2D_TRANSFORM (draws nothing without).
  void draw(Graphics2D &g) const { draw(g, 0, arm_ ? arm_->slotCount : 0); }
  // Only the draw positions [first, end), to draw something between slots
  void draw(Graphics2D &g, int first, int end) const;

  int drawIndexOf(int slot) const;  // -1 if out of range
  int slotAt(int drawIndex) const;  // -1 if out of range
  int boneIndex(const char *name) const;  // -1 if there is none
  int slotIndex(const char *name) const;
  // World transform of a bone in the armature's space (identity if out of
  // range)
  const affine2f &boneTransform(int bone) const;
  // Overrides until the next pose(): the attachment (-1 hides the slot) and
  // the alpha (0..255) of a slot
  int attachmentOf(int slot) const;
  void setAttachment(int slot, int attachment);
  int alphaOf(int slot) const;
  void setAlpha(int slot, int alpha);
  // Bounding box of the visible attachments, in the armature's space and
  // after `placement` (conservative)
  RectF bounds() const { return bounds_; }
  RectF bounds(const affine2f &placement) const;

 private:
  struct SlotState {
    int16_t x0, y0, x1, y1;  // bounding box; x1 < x0 when not drawn
    int8_t attachment;
    uint8_t alpha;
    uint8_t pad[2];
  };

  const Armature *arm_ = nullptr;
  affine2f *world_ = nullptr;
  SlotState *slots_ = nullptr;
  uint8_t *order_ = nullptr;
  const uint8_t *orderSrc_ = nullptr;  // the key order_ holds (nullptr: base)
  RectF bounds_ = {0, 0, 0, 0};

  void apply(const Animation *anim, float frame, BoneVisitor *visitor);
  void setOrder(const uint8_t *order);
  void updateSlot(int slot);
  void updateBounds();
};

}  // namespace shapoco::gfx2d::rig

#endif
