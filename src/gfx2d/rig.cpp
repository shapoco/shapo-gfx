// 2D skeletal animation (rig.hpp): posing an armature from keyframes and
// drawing its images with Graphics2D. bin/shapogfx_dbones.py evaluates poses
// with the same arithmetic (the reference of test/rig_test.cpp): keep the two
// in step.

#include "shapoco/gfx2d/rig.hpp"

#include "internal.hpp"

namespace shapoco::gfx2d::rig {

using namespace detail;

namespace {

constexpr float ANGLE_TO_RAD = 6.283185307179586f / 65536.0f;

// Q14 easing of the progress t in [0, 1) by a key's curve
int32_t ease(const Animation &a, uint8_t curve, float t) {
  if (curve == CURVE_STEP) return 0;
  if (curve >= a.curveCount || !a.curves) return (int32_t)(t * 16384.0f);
  const int16_t *y = a.curves[curve].y;
  const float u = t * 16.0f;
  int k = (int)u;
  if (k > 15) k = 15;
  return y[k] + (int32_t)((float)(y[k + 1] - y[k]) * (u - (float)k));
}

// The key at or before `frame` (k0), the next one (k1, k0 after the last)
// and the Q14 progress between them
template <typename K>
int32_t sample(const Animation &a, const void *keys, int n, float frame,
               const K *&k0, const K *&k1) {
  const K *ks = (const K *)keys;
  int i = 0;
  while (i + 1 < n && (float)ks[i + 1].frame <= frame) i++;
  k0 = k1 = &ks[i];
  if (i + 1 >= n || frame < (float)ks[i].frame) return 0;
  k1 = &ks[i + 1];
  const float t = (frame - (float)k0->frame) / (float)(k1->frame - k0->frame);
  return ease(a, k0->curve, t);
}

inline float lerpF(float a, float b, int32_t e) {
  return a + (b - a) * ((float)e * (1.0f / 16384.0f));
}
inline int32_t lerpI(int32_t a, int32_t b, int32_t e) {
  return a + (((b - a) * e) >> 14);
}
// The difference wraps to int16: the shortest way around
inline int16_t lerpAngle(int16_t a, int16_t b, int32_t e) {
  const int32_t d = (int16_t)(uint16_t)((uint16_t)b - (uint16_t)a);
  return (int16_t)(uint16_t)((uint32_t)(int32_t)a + (uint32_t)((d * e) >> 14));
}
inline int16_t addAngle(int16_t a, int16_t b) {
  return (int16_t)(uint16_t)((uint16_t)a + (uint16_t)b);
}

void applyBoneTimeline(const Animation &a, const BoneTimeline &tl, float frame,
                       BonePose &p) {
  switch (tl.channel) {
    case Channel::TRANSLATE: {
      const TranslateKey *k0, *k1;
      const int32_t e = sample(a, tl.keys, tl.keyCount, frame, k0, k1);
      p.x += lerpF(k0->x, k1->x, e);
      p.y += lerpF(k0->y, k1->y, e);
      break;
    }
    case Channel::ROTATE: {
      const RotateKey *k0, *k1;
      const int32_t e = sample(a, tl.keys, tl.keyCount, frame, k0, k1);
      p.rotX = addAngle(p.rotX, lerpAngle(k0->rotX, k1->rotX, e));
      p.rotY = addAngle(p.rotY, lerpAngle(k0->rotY, k1->rotY, e));
      break;
    }
    case Channel::SCALE: {
      const ScaleKey *k0, *k1;
      const int32_t e = sample(a, tl.keys, tl.keyCount, frame, k0, k1);
      p.scaleX = (int16_t)((p.scaleX * lerpI(k0->scaleX, k1->scaleX, e)) >> 12);
      p.scaleY = (int16_t)((p.scaleY * lerpI(k0->scaleY, k1->scaleY, e)) >> 12);
      break;
    }
    default:
      break;
  }
}

affine2f localMatrix(const BonePose &p) {
  const float sx = (float)p.scaleX * (1.0f / SCALE_ONE);
  const float sy = (float)p.scaleY * (1.0f / SCALE_ONE);
  const float ry = (float)p.rotY * ANGLE_TO_RAD;
  const float cy = std::cos(ry), sny = std::sin(ry);
  float cx = cy, snx = sny;
  if (p.rotX != p.rotY) {
    const float rx = (float)p.rotX * ANGLE_TO_RAD;
    cx = std::cos(rx);
    snx = std::sin(rx);
  }
  return {cy * sx, sny * sx, -snx * sy, cx * sy, p.x, p.y};
}

int16_t clampCoord(float v) {
  return (int16_t)(v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v));
}

}  // namespace

size_t Instance::bytes(const Armature &a) {
  const size_t n = sizeof(affine2f) * a.boneCount +
                   sizeof(SlotState) * a.slotCount + a.slotCount;
  return 3u + ((n + 3u) & ~(size_t)3u);  // 3: alignment of the memory
}

bool Instance::init(const Armature &a, void *memory, size_t size) {
  deinit();
  if (!RIG || !memory) return false;
  const uintptr_t p0 = (uintptr_t)memory, p = (p0 + 3u) & ~(uintptr_t)3u;
  const size_t need = sizeof(affine2f) * a.boneCount +
                      sizeof(SlotState) * a.slotCount + a.slotCount;
  if (size < (size_t)(p - p0) + need) return false;
  arm_ = &a;
  world_ = (affine2f *)p;
  slots_ = (SlotState *)(p + sizeof(affine2f) * a.boneCount);
  order_ = (uint8_t *)(slots_ + a.slotCount);
  orderSrc_ = order_;  // forces setOrder() to write the base order
  poseBind();
  return true;
}

void Instance::deinit() {
  arm_ = nullptr;
  world_ = nullptr;
  slots_ = nullptr;
  order_ = nullptr;
  orderSrc_ = nullptr;
  bounds_ = {0, 0, 0, 0};
}

bool Instance::pose(const Animation &anim, float frame, BoneVisitor *visitor) {
  if (!RIG || !arm_ || anim.signature != arm_->signature) return false;
  apply(&anim, frame, visitor);
  return true;
}

void Instance::poseBind(BoneVisitor *visitor) {
  if (!RIG || !arm_) return;
  apply(nullptr, 0.0f, visitor);
}

void Instance::apply(const Animation *anim, float frame, BoneVisitor *visitor) {
  const Armature &arm = *arm_;
  static const Animation NONE = {};
  const Animation &a = anim ? *anim : NONE;
  if (!(frame > 0.0f)) frame = 0.0f;  // NaN too
  if (frame > (float)a.duration) frame = (float)a.duration;

  // Bones, parents first; the timelines are sorted by bone
  int t = 0;
  for (int i = 0; i < arm.boneCount; i++) {
    const Bone &b = arm.bones[i];
    BonePose p = {b.x, b.y, b.rotX, b.rotY, b.scaleX, b.scaleY};
    for (; t < a.boneTimelineCount && a.boneTimelines[t].bone <= i; t++) {
      const BoneTimeline &tl = a.boneTimelines[t];
      if (tl.bone == i && tl.keyCount > 0 && tl.keys) {
        applyBoneTimeline(a, tl, frame, p);
      }
    }
    if (visitor) visitor->onBone(i, p);
    const affine2f l = localMatrix(p);
    world_[i] = b.parent < i ? world_[b.parent] * l : l;
  }

  // Slots
  t = 0;
  for (int s = 0; s < arm.slotCount; s++) {
    const Slot &sl = arm.slots[s];
    int att = sl.defaultAttachment, alpha = sl.alpha;
    for (; t < a.slotTimelineCount && a.slotTimelines[t].slot <= s; t++) {
      const SlotTimeline &tl = a.slotTimelines[t];
      if (tl.slot != s || tl.keyCount == 0 || !tl.keys) continue;
      if (tl.channel == Channel::ATTACHMENT) {
        const AttachmentKey *ks = (const AttachmentKey *)tl.keys;
        int i = 0;
        while (i + 1 < tl.keyCount && (float)ks[i + 1].frame <= frame) i++;
        att = ks[i].attachment;
      } else if (tl.channel == Channel::ALPHA) {
        const AlphaKey *k0, *k1;
        const int32_t e = sample(a, tl.keys, tl.keyCount, frame, k0, k1);
        alpha = clampInt(0, 255, lerpI(k0->alpha, k1->alpha, e));
      }
    }
    slots_[s].attachment = (int8_t)att;
    slots_[s].alpha = (uint8_t)alpha;
    updateSlot(s);
  }
  updateBounds();

  // Draw order: the last key at or before the frame
  const uint8_t *order = nullptr;
  for (int k = 0; k < a.drawOrderKeyCount && a.drawOrderKeys[k].frame <= frame;
       k++) {
    order = a.drawOrderKeys[k].order;
  }
  setOrder(order);
}

void Instance::setOrder(const uint8_t *order) {
  if (order == orderSrc_) return;
  const int n = arm_->slotCount;
  if (order) {
    for (int i = 0; i < n; i++) {
      if (order[i] >= n) {
        order = nullptr;  // broken: the base order
        break;
      }
    }
  }
  for (int i = 0; i < n; i++) order_[i] = order ? order[i] : (uint8_t)i;
  orderSrc_ = order;
}

void Instance::updateSlot(int s) {
  const Slot &sl = arm_->slots[s];
  SlotState &st = slots_[s];
  st.x0 = st.y0 = 1;
  st.x1 = st.y1 = 0;
  if (st.attachment < 0 || st.attachment >= sl.attachmentCount ||
      sl.bone >= arm_->boneCount)
    return;
  const Attachment &at = sl.attachments[st.attachment];
  if (!at.texture) return;
  const affine2f m = world_[sl.bone] * at.local;
  const float w = (float)at.src.width, h = (float)at.src.height;
  // The corners: m.apply(0, 0) + {0, a w} + {0, c h}
  const float ax = m.a * w, bx = m.b * w, cy = m.c * h, dy = m.d * h;
  const float x0 = m.tx + std::min(ax, 0.0f) + std::min(cy, 0.0f);
  const float x1 = m.tx + std::max(ax, 0.0f) + std::max(cy, 0.0f);
  const float y0 = m.ty + std::min(bx, 0.0f) + std::min(dy, 0.0f);
  const float y1 = m.ty + std::max(bx, 0.0f) + std::max(dy, 0.0f);
  if (!(x0 <= x1 && y0 <= y1)) return;  // NaN
  st.x0 = clampCoord(std::floor(x0));
  st.y0 = clampCoord(std::floor(y0));
  st.x1 = clampCoord(std::ceil(x1));
  st.y1 = clampCoord(std::ceil(y1));
}

void Instance::updateBounds() {
  int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
  for (int s = 0; s < arm_->slotCount; s++) {
    const SlotState &st = slots_[s];
    if (st.x1 < st.x0) continue;
    x0 = std::min(x0, (int)st.x0);
    y0 = std::min(y0, (int)st.y0);
    x1 = std::max(x1, (int)st.x1);
    y1 = std::max(y1, (int)st.y1);
  }
  if (x1 < x0) {
    bounds_ = {0, 0, 0, 0};
  } else {
    bounds_ = {(float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0)};
  }
}

void Instance::draw(Graphics2D &g, int first, int end) const {
  if (!RIG || !TRANSFORM || !arm_) return;
  const Armature &arm = *arm_;
  first = std::max(first, 0);
  end = std::min(end, (int)arm.slotCount);
  if (first >= end) return;

  const affine2f base = g.transform();
  const int opacity = g.opacity();
  const BlendMode blend = g.blendMode();
  const bool hadKey = g.hasColorKey();
  const Color oldKey = g.colorKey();
  if (arm.colorKeyEnabled) g.setColorKey(arm.colorKey);
  // The clip rectangle a pixel wider (the image paths round positions)
  const Rect clip = g.clipRect();
  const float cx0 = (float)(clip.x - 1), cy0 = (float)(clip.y - 1);
  const float cx1 = (float)(clip.right() + 1), cy1 = (float)(clip.bottom() + 1);
  int curOpacity = opacity;
  BlendMode curBlend = blend;
  for (int i = first; i < end; i++) {
    const int s = order_[i];
    const SlotState &st = slots_[s];
    if (st.x1 < st.x0 || st.alpha == 0) continue;
    // Skip what lies outside the clip (drawing in bands)
    const float w = (float)(st.x1 - st.x0), h = (float)(st.y1 - st.y0);
    const vec2f o = base.apply((float)st.x0, (float)st.y0);
    const float ax = base.a * w, bx = base.b * w, cy = base.c * h,
                dy = base.d * h;
    if (o.x + std::max(ax, 0.0f) + std::max(cy, 0.0f) < cx0 ||
        o.x + std::min(ax, 0.0f) + std::min(cy, 0.0f) > cx1 ||
        o.y + std::max(bx, 0.0f) + std::max(dy, 0.0f) < cy0 ||
        o.y + std::min(bx, 0.0f) + std::min(dy, 0.0f) > cy1)
      continue;
    const Slot &sl = arm.slots[s];
    const Attachment &at = sl.attachments[st.attachment];
    const int op = st.alpha == 255 ? opacity : (opacity * st.alpha + 127) / 255;
    if (op == 0) continue;
    const BlendMode bm = (sl.blend == BlendMode::ADD && blend != BlendMode::NONE)
                             ? BlendMode::ADD
                             : blend;
    if (op != curOpacity || bm != curBlend) {
      g.setBlend(bm, op);
      curOpacity = op;
      curBlend = bm;
    }
    g.setTransform(base * world_[sl.bone] * at.local);
    g.drawImage(*at.texture, 0, 0, at.src);
  }
  g.setTransform(base);
  if (curOpacity != opacity || curBlend != blend) g.setBlend(blend, opacity);
  if (arm.colorKeyEnabled) {
    if (hadKey) {
      g.setColorKey(oldKey);
    } else {
      g.clearColorKey();
    }
  }
}

int Instance::drawIndexOf(int slot) const {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return -1;
  for (int i = 0; i < arm_->slotCount; i++) {
    if (order_[i] == slot) return i;
  }
  return -1;
}

int Instance::slotAt(int drawIndex) const {
  if (!arm_ || drawIndex < 0 || drawIndex >= arm_->slotCount) return -1;
  return order_[drawIndex];
}

int Instance::boneIndex(const char *name) const {
  if (!arm_ || !name) return -1;
  for (int i = 0; i < arm_->boneCount; i++) {
    const char *n = arm_->bones[i].name;
    if (n && std::strcmp(n, name) == 0) return i;
  }
  return -1;
}

int Instance::slotIndex(const char *name) const {
  if (!arm_ || !name) return -1;
  for (int i = 0; i < arm_->slotCount; i++) {
    const char *n = arm_->slots[i].name;
    if (n && std::strcmp(n, name) == 0) return i;
  }
  return -1;
}

const affine2f &Instance::boneTransform(int bone) const {
  static const affine2f IDENTITY = {1, 0, 0, 1, 0, 0};
  if (!arm_ || bone < 0 || bone >= arm_->boneCount) return IDENTITY;
  return world_[bone];
}

int Instance::attachmentOf(int slot) const {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return -1;
  return slots_[slot].attachment;
}

void Instance::setAttachment(int slot, int attachment) {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return;
  slots_[slot].attachment = (int8_t)clampInt(-1, MAX_ATTACHMENTS, attachment);
  updateSlot(slot);
  updateBounds();
}

int Instance::alphaOf(int slot) const {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return 0;
  return slots_[slot].alpha;
}

void Instance::setAlpha(int slot, int alpha) {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return;
  slots_[slot].alpha = (uint8_t)clampInt(0, 255, alpha);
}

RectF Instance::bounds(const affine2f &placement) const {
  const RectF &b = bounds_;
  const vec2f o = placement.apply(b.x, b.y);
  const float ax = placement.a * b.width, bx = placement.b * b.width;
  const float cy = placement.c * b.height, dy = placement.d * b.height;
  const float x0 = o.x + std::min(ax, 0.0f) + std::min(cy, 0.0f);
  const float x1 = o.x + std::max(ax, 0.0f) + std::max(cy, 0.0f);
  const float y0 = o.y + std::min(bx, 0.0f) + std::min(dy, 0.0f);
  const float y1 = o.y + std::max(bx, 0.0f) + std::max(dy, 0.0f);
  return {x0, y0, x1 - x0, y1 - y0};
}

}  // namespace shapoco::gfx2d::rig
