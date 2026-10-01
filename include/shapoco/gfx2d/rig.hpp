#ifndef SHAPOGFX2D_RIG_HPP
#define SHAPOGFX2D_RIG_HPP

// 2D skeletal animation: armatures of bones carrying images or vector
// pictures (vg.hpp), posed from keyframed animations and drawn with
// Graphics2D. The data (Armature, Animation) is static and lives in flash;
// bin/dbones2cpp generates it from DragonBones and bin/svg2cpp from animated
// SVG. An Instance keeps the pose of one character in memory the user
// provides (Instance::bytes()); drawBind() draws the bind pose without one.
// Optional: gfx2d.hpp does not include this header.
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
#include "shapoco/gfx2d/vg.hpp"

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

// --- Room for later features -------------------------------------------------
//
// The structures below are meant to grow (mesh deformation, skinning, IK,
// nested armatures, tints, ...). Generated headers initialize them by
// position, so members are only ever appended: a header generated before a
// member existed leaves it zero, and zero (or the enumerator 0) means "not
// used" for every appended member. Members that would mean something else at
// zero (the tint of a slot) are only read when a bit of `features` says the
// data has them.
//
// FORMAT_VERSION counts the members added this way; a generated header
// static_asserts on the version its members need, so that old library code
// refuses new data at compile time rather than misreading it.
//   1: the first layout
//   2: Slot::clip / clipBone, AttachmentKind::VECTOR, Channel::COLOR
//   3: Slot::strokeWidth, Channel::STROKE_WIDTH
constexpr uint16_t FORMAT_VERSION = 3;

// Bits of Armature::features and Animation::features: a feature takes a bit
// here, and generated data sets it when it uses the feature. SUPPORTED_FEATURES
// are the bits this build of rig honors; Instance::init() and pose() refuse
// data with any other bit set, since drawing it without the feature would
// show something else than what was made.
//
// FEATURE_SLOT_COLOR: Slot::colorR / G / B and COLOR timelines are meaningful
// (a header generated before them leaves 0, 0, 0; without the bit the slots
// are white). FEATURE_STROKE_WIDTH: Slot::strokeWidth and STROKE_WIDTH
// timelines are meaningful (without the bit the width is 1).
constexpr uint16_t FEATURE_SLOT_COLOR = 1;
constexpr uint16_t FEATURE_STROKE_WIDTH = 2;
constexpr uint16_t SUPPORTED_FEATURES = FEATURE_SLOT_COLOR | FEATURE_STROKE_WIDTH;

// What an attachment is. IMAGE and VECTOR are drawn; the others name what a
// DragonBones display can be, so that a later version can put its data
// behind Attachment::ext without moving the images.
enum class AttachmentKind : uint8_t {
  IMAGE,         // a part of a texture (Attachment::texture, src)
  MESH,          // reserved: a textured mesh, deformable / skinned
  ARMATURE,      // reserved: a nested armature
  BOUNDING_BOX,  // reserved: a hit area, not drawn
  VECTOR,        // a vg::Picture (Attachment::ext), drawn in the slot's color
};

// --- Armature (static data) ---------------------------------------------------

struct Bone {
  const char *name;
  float x, y;             // origin in the parent's space (bind pose)
  angle16_t rotX, rotY;   // DragonBones skX / skY
  scale16_t scaleX, scaleY;
  uint8_t parent;         // a smaller index, or NO_PARENT
  // Reserved for how the bone inherits from its parent (DragonBones'
  // inheritRotation / inheritScale / inheritReflection); 0 today: everything
  // is inherited. Fills the padding.
  uint8_t flags;
};

// What a slot can show (a DragonBones "display"): an image, or a vector
// picture
struct Attachment {
  const Texture *texture;  // IMAGE: nullptr is not drawn (an unsupported display)
  Rect src;                // the part of `texture` (a texture atlas)
  // IMAGE: the top-left corner of `src` to the bone's space; VECTOR: the
  // picture's space to the bone's
  affine2f local;
  // Convex polygon around the opaque pixels, as x, y pairs relative to the
  // top-left corner of `src` (Graphics2D::drawImage with a polygon); nullptr
  // / 0: the whole rectangle
  const int16_t *hull;
  uint8_t hullCount;
  AttachmentKind kind;
  uint8_t pad[2];
  // VECTOR: the const vg::Picture *. Reserved for the data of the other
  // kinds (a Mesh, an Armature, a hit area)
  const void *ext;
};

struct Slot {
  const char *name;
  const Attachment *attachments;
  uint8_t attachmentCount;
  int8_t defaultAttachment;  // -1: hidden
  uint8_t bone;
  uint8_t alpha;             // 0..255
  BlendMode blend;           // ALPHA or ADD
  // The color of the slot (FEATURE_SLOT_COLOR): the current color of a
  // VECTOR picture (vg::SHAPE_*_CURRENT_COLOR); for images reserved as a
  // tint (DragonBones' color transform), not applied today. dbones2cpp
  // writes 255, 255, 255.
  uint8_t colorR, colorG, colorB;
  // Clip rectangle of the slot in the space of bone `clipBone`, nullptr:
  // none. Drawn as the clip rectangle of Graphics2D, so one turned on
  // screen clips to its bounding box. Version 2.
  const RectF *clip;
  uint8_t clipBone;
  uint8_t pad[3];
  // The stroke width of the slot (FEATURE_STROKE_WIDTH): the width of the
  // strokes of its VECTOR picture flagged vg::SHAPE_STROKE_CURRENT_WIDTH, in
  // the picture's coordinates. Version 3.
  float strokeWidth;
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
  // The features the data uses (bits to be defined; 0 today). init() refuses
  // an armature with a bit outside SUPPORTED_FEATURES.
  uint16_t features;
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
  // Reserved for extra whole turns to the next key (DragonBones' clockwise /
  // tweenRotate); 0 today: the shortest way. Was padding.
  int8_t turns;
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
struct ColorKey {  // version 2; FEATURE_SLOT_COLOR
  uint16_t frame;
  uint8_t curve;
  uint8_t r, g, b;
  uint8_t pad[2];
};
struct StrokeWidthKey {  // version 3; FEATURE_STROKE_WIDTH
  uint16_t frame;
  uint8_t curve;
  uint8_t pad;
  float width;
};

enum class Channel : uint8_t {
  TRANSLATE,
  ROTATE,
  SCALE,
  ATTACHMENT,
  ALPHA,
  COLOR,         // version 2: the slot's color (ColorKey)
  STROKE_WIDTH,  // version 3: the slot's stroke width (StrokeWidthKey)
};

struct BoneTimeline {  // TRANSLATE, ROTATE or SCALE
  const void *keys;    // TranslateKey / RotateKey / ScaleKey by the channel
  uint16_t keyCount;
  uint8_t bone;
  Channel channel;
};
struct SlotTimeline {  // ATTACHMENT, ALPHA, COLOR or STROKE_WIDTH
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
  // The features the data uses (as Armature::features; 0 today). pose()
  // refuses an animation with a bit outside SUPPORTED_FEATURES.
  uint16_t features;
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

// --- Drawing without an Instance ------------------------------------------------

// The bind pose of an armature (its default attachments, alphas and colors,
// in the base draw order) with the transform of `g` as its placement, like
// Instance::draw(). The world transforms are computed on the fly, so this
// costs more per frame than an Instance for a deep tree, but needs no
// memory: the way to draw a picture that is not animated.
void drawBind(Graphics2D &g, const Armature &a);

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
  // Overrides until the next pose(): the attachment (-1 hides the slot),
  // the alpha (0..255) and the color (RGB; the alpha of the Color is
  // ignored) of a slot
  int attachmentOf(int slot) const;
  void setAttachment(int slot, int attachment);
  int alphaOf(int slot) const;
  void setAlpha(int slot, int alpha);
  Color colorOf(int slot) const;  // opaque; WHITE if out of range
  void setColor(int slot, Color c);
  float strokeWidthOf(int slot) const;  // 1 if out of range
  void setStrokeWidth(int slot, float width);
  // Bounding box of the visible attachments, in the armature's space and
  // after `placement` (conservative)
  RectF bounds() const { return bounds_; }
  RectF bounds(const affine2f &placement) const;

 private:
  struct SlotState {
    int16_t x0, y0, x1, y1;  // bounding box; x1 < x0 when not drawn
    int8_t attachment;
    uint8_t alpha;
    uint8_t r, g, b;
    uint8_t pad;
    float strokeWidth;
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
