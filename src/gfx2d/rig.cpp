// 2D skeletal animation (rig.hpp): posing an armature from keyframes and
// drawing its images with Graphics2D. bin/shapogfx_dbones.py evaluates poses
// with the same arithmetic (the reference of test/rig_test.cpp): keep the two
// in step.

#include "shapoco/gfx2d/rig.hpp"

#include "internal.hpp"

namespace shapoco::gfx2d::rig {

using namespace detail;

namespace {

// sin over a quarter turn in 256 steps, Q15: round(32767 sin(i pi / 512)).
// bin/shapogfx_dbones.py builds the same table.
const int16_t SIN_Q15[257] = {
    0, 201, 402, 603, 804, 1005, 1206, 1407, 1608, 1809, 2009, 2210,
    2410, 2611, 2811, 3012, 3212, 3412, 3612, 3811, 4011, 4210, 4410, 4609,
    4808, 5007, 5205, 5404, 5602, 5800, 5998, 6195, 6393, 6590, 6786, 6983,
    7179, 7375, 7571, 7767, 7962, 8157, 8351, 8545, 8739, 8933, 9126, 9319,
    9512, 9704, 9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605,
    11793, 11980, 12167, 12353, 12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828,
    14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269, 15446, 15623, 15800, 15976,
    16151, 16325, 16499, 16673, 16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000,
    20159, 20317, 20475, 20631, 20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856,
    22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027, 23170, 23311, 23452, 23592,
    23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674,
    26790, 26905, 27019, 27133, 27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001,
    28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803, 28898, 28992, 29085, 29177,
    29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050,
    31113, 31176, 31237, 31297, 31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736,
    31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098, 32137, 32176, 32213, 32250,
    32285, 32318, 32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737, 32745, 32752,
    32757, 32761, 32765, 32766, 32767,
};

// sin of an angle in 1/65536 turns, Q15, interpolated linearly between the
// entries (error below 5e-5)
int32_t sinQ15(uint32_t angle) {
  const uint32_t quadrant = (angle >> 14) & 3u, r = angle & 0x3FFFu;
  const uint32_t pos = (quadrant & 1u) ? 16384u - r : r;  // 0..16384
  const uint32_t i = pos >> 6, f = pos & 63u;
  int32_t v = SIN_Q15[i];
  if (f) v += ((SIN_Q15[i + 1] - v) * (int32_t)f) >> 6;
  return quadrant & 2u ? -v : v;
}

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

// Without libm's sinf() / cosf(), which cost thousands of cycles each in
// software floating point (Cortex-M0+)
affine2f localMatrix(const BonePose &p) {
  constexpr float K = 1.0f / (32767.0f * SCALE_ONE);
  const float sx = (float)p.scaleX * K, sy = (float)p.scaleY * K;
  const uint32_t ry = (uint16_t)p.rotY;
  const int32_t cy = sinQ15(ry + 16384u), sny = sinQ15(ry);
  int32_t cx = cy, snx = sny;
  if (p.rotX != p.rotY) {
    const uint32_t rx = (uint16_t)p.rotX;
    cx = sinQ15(rx + 16384u);
    snx = sinQ15(rx);
  }
  return {(float)cy * sx, (float)sny * sx, -(float)snx * sy, (float)cx * sy,
          p.x, p.y};
}

int16_t clampCoord(float v) {
  return (int16_t)(v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v));
}

// The rectangle an attachment covers in its own space; false if it is not
// drawn
bool attachmentRect(const Attachment &at, RectF &r) {
  if (at.kind == AttachmentKind::IMAGE) {
    if (!at.texture) return false;
    r = RectF{0, 0, (float)at.src.width, (float)at.src.height};
    return true;
  }
  if (at.kind == AttachmentKind::VECTOR) {
    if (!at.ext) return false;
    r = ((const vg::Picture *)at.ext)->bounds;
    return true;
  }
  return false;
}

// Bounding box of a rectangle under m
void rectBounds(const RectF &r, const affine2f &m, float &x0, float &y0,
                float &x1, float &y1) {
  const vec2f o = m.apply(r.x, r.y);
  const float ax = m.a * r.width, bx = m.b * r.width;
  const float cy = m.c * r.height, dy = m.d * r.height;
  x0 = o.x + std::min(ax, 0.0f) + std::min(cy, 0.0f);
  x1 = o.x + std::max(ax, 0.0f) + std::max(cy, 0.0f);
  y0 = o.y + std::min(bx, 0.0f) + std::min(dy, 0.0f);
  y1 = o.y + std::max(bx, 0.0f) + std::max(dy, 0.0f);
}

// Draws the slots of an armature one by one, keeping the blend, color key
// and clip of the Graphics2D in step and restoring them at the end
struct Drawer {
  Graphics2D &g;
  const Armature &arm;
  const affine2f base;
  const int opacity;
  const BlendMode blend;
  const bool hadKey;
  const Color oldKey;
  const Rect clip;
  // The clip rectangle a pixel wider (the image paths round positions)
  float cx0, cy0, cx1, cy1;
  bool keyOn;
  Color curKey;  // while keyOn
  int curOpacity;
  BlendMode curBlend;
  bool clipped = false;

  Drawer(Graphics2D &g, const Armature &arm)
      : g(g), arm(arm), base(g.transform()), opacity(g.opacity()),
        blend(g.blendMode()), hadKey(g.hasColorKey()), oldKey(g.colorKey()),
        clip(g.clipRect()) {
    cx0 = (float)(clip.x - 1);
    cy0 = (float)(clip.y - 1);
    cx1 = (float)(clip.right() + 1);
    cy1 = (float)(clip.bottom() + 1);
    // A keyed armature sets its key for its keyed textures only: an
    // ARGB4444 texture has alpha, and comparing its pixels with a key
    // would cost
    keyOn = hadKey;
    curKey = oldKey;
    curOpacity = opacity;
    curBlend = blend;
  }
  ~Drawer() {
    g.setTransform(base);
    if (curOpacity != opacity || curBlend != blend) g.setBlend(blend, opacity);
    if (keyOn != hadKey || (keyOn && curKey != oldKey)) {
      if (hadKey) {
        g.setColorKey(oldKey);
      } else {
        g.clearColorKey();
      }
    }
    if (clipped) g.setClipRect(clip);
  }

  // Whether a box in the armature's space meets the clip
  bool visible(float x0, float y0, float x1, float y1) const {
    const float w = x1 - x0, h = y1 - y0;
    const vec2f o = base.apply(x0, y0);
    const float ax = base.a * w, bx = base.b * w, cy = base.c * h,
                dy = base.d * h;
    return !(o.x + std::max(ax, 0.0f) + std::max(cy, 0.0f) < cx0 ||
             o.x + std::min(ax, 0.0f) + std::min(cy, 0.0f) > cx1 ||
             o.y + std::max(bx, 0.0f) + std::max(dy, 0.0f) < cy0 ||
             o.y + std::min(bx, 0.0f) + std::min(dy, 0.0f) > cy1);
  }

  void setKey(const Attachment &at) {
    if (!arm.colorKeyEnabled || at.kind != AttachmentKind::IMAGE) return;
    const bool wantKey = at.texture->format != PixelFormat::ARGB4444;
    if (wantKey == keyOn && (!wantKey || curKey == arm.colorKey)) return;
    if (wantKey) {
      g.setColorKey(arm.colorKey);
      curKey = arm.colorKey;
    } else {
      g.clearColorKey();
    }
    keyOn = wantKey;
  }

  // Sets back what a SlotPainter changed of the state the drawing tracks
  void resync() {
    if (g.opacity() != curOpacity || g.blendMode() != curBlend) {
      g.setBlend(curBlend, curOpacity);
    }
    if (g.hasColorKey() != keyOn || (keyOn && g.colorKey() != curKey)) {
      if (keyOn) {
        g.setColorKey(curKey);
      } else {
        g.clearColorKey();
      }
    }
  }

  // `world`: of the slot's bone; `clipWorld`: of the clip's bone; `paint`:
  // the slot is left to `painter` (nullptr: not drawn)
  void slot(const Slot &sl, const Attachment &at, int alpha, Color color,
            float strokeWidth, const affine2f &world,
            const affine2f &clipWorld, SlotPainter *painter = nullptr,
            const SlotPaint *paint = nullptr) {
    if (paint && !painter) return;
    const int op = alpha == 255 ? opacity : (opacity * alpha + 127) / 255;
    if (op == 0) return;
    const BlendMode bm =
        (sl.blend == BlendMode::ADD && blend != BlendMode::NONE) ? BlendMode::ADD
                                                                 : blend;
    if (op != curOpacity || bm != curBlend) {
      g.setBlend(bm, op);
      curOpacity = op;
      curBlend = bm;
    }
    if (sl.clip) {
      // In target pixels: exact without a rotation, else the bounding box
      const affine2f m = base * clipWorld;
      float x0, y0, x1, y1;
      rectBounds(*sl.clip, m, x0, y0, x1, y1);
      Rect r = {0, 0, 0, 0};
      if (x0 <= x1 && y0 <= y1) {
        const int ix0 = snap(x0), iy0 = snap(y0);
        r = {ix0, iy0, snap(x1) - ix0, snap(y1) - iy0};
      }
      g.setClipRect(r.intersect(clip));
      clipped = true;
      if (g.clipRect().isEmpty()) {
        g.setClipRect(clip);
        return;
      }
    } else if (clipped) {
      g.setClipRect(clip);
    }
    if (paint) {
      setKey(at);
      g.setTransform(base * world * at.local);
      painter->paintSlot(g, *paint);
      resync();
      if (!sl.clip) g.setClipRect(clip);
    } else if (at.kind == AttachmentKind::VECTOR) {
      g.setTransform(base * world * at.local);
      g.drawPicture(*(const vg::Picture *)at.ext, color, strokeWidth);
    } else {
      setKey(at);
      g.setTransform(base * world * at.local);
      g.drawImage(*at.texture, 0, 0, at.src, at.hull, at.hullCount);
    }
    if (sl.clip) g.setClipRect(clip);
  }
};

}  // namespace

void drawBind(Graphics2D &g, const Armature &arm) {
  if (!RIG || !TRANSFORM) return;
  if (arm.features & ~SUPPORTED_FEATURES) return;
  const bool hasColor = (arm.features & FEATURE_SLOT_COLOR) != 0;
  const bool hasWidth = (arm.features & FEATURE_STROKE_WIDTH) != 0;
  // The world transform of a bone, up its chain (parents come first, so
  // the chain is at most the bone's index long)
  auto world = [&](int bone) {
    affine2f m = affine2f::identity();
    for (int b = bone; b < arm.boneCount && b != NO_PARENT;) {
      const Bone &bn = arm.bones[b];
      const BonePose p = {bn.x, bn.y, bn.rotX, bn.rotY, bn.scaleX, bn.scaleY};
      m = localMatrix(p) * m;
      if (bn.parent >= b) break;  // parents before children
      b = bn.parent;
    }
    return m;
  };
  Drawer d(g, arm);
  for (int s = 0; s < arm.slotCount; s++) {
    const Slot &sl = arm.slots[s];
    if (sl.defaultAttachment < 0 || sl.defaultAttachment >= sl.attachmentCount ||
        sl.bone >= arm.boneCount || sl.alpha == 0)
      continue;
    const Attachment &at = sl.attachments[sl.defaultAttachment];
    RectF r;
    if (!attachmentRect(at, r)) continue;
    const affine2f w = world(sl.bone);
    float x0, y0, x1, y1;
    rectBounds(r, w * at.local, x0, y0, x1, y1);
    if (!(x0 <= x1 && y0 <= y1) || !d.visible(x0, y0, x1, y1)) continue;
    const Color c = hasColor ? makeColor(sl.colorR, sl.colorG, sl.colorB)
                             : Colors::WHITE;
    d.slot(sl, at, sl.alpha, c, hasWidth ? sl.strokeWidth : 1.0f, w,
           sl.clip && sl.clipBone < arm.boneCount ? world(sl.clipBone) : w);
  }
}

size_t Instance::bytes(const Armature &a) {
  const size_t n = sizeof(affine2f) * a.boneCount +
                   sizeof(SlotState) * a.slotCount + a.slotCount;
  return 3u + ((n + 3u) & ~(size_t)3u);  // 3: alignment of the memory
}

bool Instance::init(const Armature &a, void *memory, size_t size) {
  deinit();
  if (!RIG || !memory) return false;
  // Data made for a feature this build does not have would draw wrong
  if (a.features & ~SUPPORTED_FEATURES) return false;
  const uintptr_t p0 = (uintptr_t)memory, p = (p0 + 3u) & ~(uintptr_t)3u;
  const size_t need = sizeof(affine2f) * a.boneCount +
                      sizeof(SlotState) * a.slotCount + a.slotCount;
  if (size < (size_t)(p - p0) + need) return false;
  arm_ = &a;
  world_ = (affine2f *)p;
  slots_ = (SlotState *)(p + sizeof(affine2f) * a.boneCount);
  order_ = (uint8_t *)(slots_ + a.slotCount);
  orderSrc_ = order_;  // forces setOrder() to write the base order
  for (int i = 0; i < a.slotCount; i++) slots_[i].flags = 0;
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
  if (anim.features & ~SUPPORTED_FEATURES) return false;
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
  const bool hasColor = (arm.features & FEATURE_SLOT_COLOR) != 0;
  const bool hasWidth = (arm.features & FEATURE_STROKE_WIDTH) != 0;
  for (int s = 0; s < arm.slotCount; s++) {
    const Slot &sl = arm.slots[s];
    int att = sl.defaultAttachment, alpha = sl.alpha;
    int r = 255, gr = 255, b = 255;
    float width = hasWidth ? sl.strokeWidth : 1.0f;
    if (hasColor) {
      r = sl.colorR;
      gr = sl.colorG;
      b = sl.colorB;
    }
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
      } else if (tl.channel == Channel::COLOR) {
        const ColorKey *k0, *k1;
        const int32_t e = sample(a, tl.keys, tl.keyCount, frame, k0, k1);
        r = clampInt(0, 255, lerpI(k0->r, k1->r, e));
        gr = clampInt(0, 255, lerpI(k0->g, k1->g, e));
        b = clampInt(0, 255, lerpI(k0->b, k1->b, e));
      } else if (tl.channel == Channel::STROKE_WIDTH) {
        const StrokeWidthKey *k0, *k1;
        const int32_t e = sample(a, tl.keys, tl.keyCount, frame, k0, k1);
        width = lerpF(k0->width, k1->width, e);
      }
    }
    slots_[s].attachment = (int8_t)att;
    slots_[s].alpha = (uint8_t)alpha;
    slots_[s].r = (uint8_t)r;
    slots_[s].g = (uint8_t)gr;
    slots_[s].b = (uint8_t)b;
    slots_[s].strokeWidth = width;
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
  RectF r;
  if (!attachmentRect(at, r)) return;  // the other kinds are reserved
  const affine2f m = world_[sl.bone] * at.local;
  float x0, y0, x1, y1;
  rectBounds(r, m, x0, y0, x1, y1);
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

void Instance::draw(Graphics2D &g, int first, int end,
                    SlotPainter *painter) const {
  if (!RIG || !TRANSFORM || !arm_) return;
  const Armature &arm = *arm_;
  first = std::max(first, 0);
  end = std::min(end, (int)arm.slotCount);
  if (first >= end) return;
  Drawer d(g, arm);
  for (int i = first; i < end; i++) {
    const int s = order_[i];
    const SlotState &st = slots_[s];
    if (st.x1 < st.x0 || st.alpha == 0) continue;
    // Skip what lies outside the clip (drawing in bands)
    if (!d.visible((float)st.x0, (float)st.y0, (float)st.x1, (float)st.y1))
      continue;
    const Slot &sl = arm.slots[s];
    const Attachment &at = sl.attachments[st.attachment];
    const Color c = makeColor(st.r, st.g, st.b);
    const affine2f &cw = sl.clip && sl.clipBone < arm.boneCount
                             ? world_[sl.clipBone]
                             : world_[sl.bone];
    if (st.flags & FLAG_CUSTOM_PAINT) {
      const SlotPaint p = {s, st.attachment, sl, at, st.alpha, c,
                           st.strokeWidth};
      d.slot(sl, at, st.alpha, c, st.strokeWidth, world_[sl.bone], cw,
             painter, &p);
    } else {
      d.slot(sl, at, st.alpha, c, st.strokeWidth, world_[sl.bone], cw);
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

Color Instance::colorOf(int slot) const {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return Colors::WHITE;
  const SlotState &st = slots_[slot];
  return makeColor(st.r, st.g, st.b);
}

void Instance::setColor(int slot, Color c) {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return;
  SlotState &st = slots_[slot];
  st.r = (uint8_t)colorR(c);
  st.g = (uint8_t)colorG(c);
  st.b = (uint8_t)colorB(c);
}

float Instance::strokeWidthOf(int slot) const {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return 1.0f;
  return slots_[slot].strokeWidth;
}

void Instance::setStrokeWidth(int slot, float width) {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return;
  slots_[slot].strokeWidth = width;
}

bool Instance::customPaintOf(int slot) const {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return false;
  return (slots_[slot].flags & FLAG_CUSTOM_PAINT) != 0;
}

void Instance::setCustomPaint(int slot, bool enable) {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return;
  uint8_t &f = slots_[slot].flags;
  f = enable ? (uint8_t)(f | FLAG_CUSTOM_PAINT)
             : (uint8_t)(f & ~FLAG_CUSTOM_PAINT);
}

bool Instance::attachmentTransform(int slot, affine2f &out) const {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return false;
  return attachmentTransform(slot, slots_[slot].attachment, out);
}

bool Instance::attachmentTransform(int slot, int attachment,
                                   affine2f &out) const {
  if (!arm_ || slot < 0 || slot >= arm_->slotCount) return false;
  const Slot &sl = arm_->slots[slot];
  if (attachment < 0 || attachment >= sl.attachmentCount ||
      sl.bone >= arm_->boneCount)
    return false;
  out = world_[sl.bone] * sl.attachments[attachment].local;
  return true;
}

bool Instance::attachmentToArmature(int slot, const vec2f &p,
                                    vec2f &out) const {
  affine2f m = affine2f::identity();
  if (!attachmentTransform(slot, m)) return false;
  out = m.apply(p);
  return true;
}

bool Instance::attachmentToArmature(int slot, int attachment, const vec2f &p,
                                    vec2f &out) const {
  affine2f m = affine2f::identity();
  if (!attachmentTransform(slot, attachment, m)) return false;
  out = m.apply(p);
  return true;
}

bool Instance::armatureToAttachment(int slot, const vec2f &p,
                                    vec2f &out) const {
  affine2f m = affine2f::identity(), inv = m;
  if (!attachmentTransform(slot, m) || !m.invert(inv)) return false;
  out = inv.apply(p);
  return true;
}

bool Instance::armatureToAttachment(int slot, int attachment, const vec2f &p,
                                    vec2f &out) const {
  affine2f m = affine2f::identity(), inv = m;
  if (!attachmentTransform(slot, attachment, m) || !m.invert(inv)) return false;
  out = inv.apply(p);
  return true;
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
