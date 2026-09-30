// rig::Instance against the poses bin/shapogfx_dbones.py computed for the test
// armature (test/data/test_rig*.hpp; regenerate with
// test/tools/make_test_rig.py).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

#include "check.hpp"
#include "data/test_rig.hpp"
#include "data/test_rig_expected.hpp"
#include "data/test_rig_fit.hpp"
#include "data/test_rig_keyed.hpp"
#include "data/test_rig_mixed.hpp"
#include "data/test_rig_sep.hpp"
#include "shapoco/gfx2d/rig.hpp"
#include "shapoco/gfx2d/surface_alloc.hpp"

namespace g2 = shapoco::gfx2d;
namespace rig = shapoco::gfx2d::rig;
namespace ex = test_rig_expected;

#if SHAPOGFX2D_RIG
alignas(4) static uint8_t rigMemory[1024];

static bool nearlyEqual(float a, float b, float tol = 1e-3f) {
  return std::fabs(a - b) <= tol;
}

static bool sameWorld(const rig::Instance &inst, const ex::Pose &p) {
  for (int i = 0; i < ex::BONES; i++) {
    const g2::affine2f &m = inst.boneTransform(i);
    const float v[6] = {m.a, m.b, m.c, m.d, m.tx, m.ty};
    for (int k = 0; k < 6; k++) {
      if (!nearlyEqual(v[k], p.world[i][k])) {
        std::printf("  bone %d [%d]: %f vs %f\n", i, k, v[k], p.world[i][k]);
        return false;
      }
    }
  }
  return true;
}

static void checkPose(const rig::Instance &inst, const ex::Pose &p) {
  CHECK(sameWorld(inst, p));
  for (int s = 0; s < ex::SLOTS; s++) {
    CHECK_EQ(inst.attachmentOf(s), p.attachment[s]);
    CHECK_EQ(inst.alphaOf(s), p.alpha[s]);
    CHECK_EQ(inst.slotAt(s), p.order[s]);
  }
  // The bounding box rounds outward from float corners: allow a pixel
  const g2::RectF b = inst.bounds();
  CHECK(nearlyEqual(b.x, p.bounds[0], 1.0f));
  CHECK(nearlyEqual(b.y, p.bounds[1], 1.0f));
  CHECK(nearlyEqual(b.width, p.bounds[2], 2.0f));
  CHECK(nearlyEqual(b.height, p.bounds[3], 2.0f));
}

// Memory, initialization and the bind pose
static void testRigInit() {
  const rig::Armature &arm = test_rig::armature;
  CHECK_EQ(arm.boneCount, ex::BONES);
  CHECK_EQ(arm.slotCount, ex::SLOTS);
  // 4 world transforms, 6 slot states, 6 order bytes rounded up to a
  // multiple of 4; 3 bytes of slack
  CHECK_EQ(rig::Instance::bytes(arm), 3u + 4 * 24 + 6 * 12 + 8);
  rig::Instance inst;
  CHECK(!inst.isInitialized());
  CHECK(!inst.init(arm, rigMemory, 4 * 24 + 6 * 12 + 6 - 1));
  CHECK(!inst.isInitialized());
  CHECK(!inst.init(arm, nullptr, sizeof(rigMemory)));
  // Unaligned memory of bytes(): the slack covers the alignment
  CHECK(inst.init(arm, rigMemory + 1, rig::Instance::bytes(arm)));
  CHECK(inst.isInitialized());
  CHECK(inst.armature() == &arm);
  // Frame 0 of "move" has no offsets: the bind pose
  checkPose(inst, ex::move[0]);
  inst.deinit();
  CHECK(!inst.isInitialized());
}

// Poses of the animations, the one from the .dbani (positions scaled by the
// tool) included
static void testRigPose() {
  rig::Instance inst;
  CHECK(inst.init(test_rig::armature, rigMemory, sizeof(rigMemory)));
  CHECK_EQ(test_rig::ANIMATION_COUNT, 2);
  CHECK(test_rig::animations[0] == &test_rig::anim_move);
  for (int i = 0; i < ex::MOVE_COUNT; i++) {
    CHECK(inst.pose(test_rig::anim_move, ex::move[i].frame));
    checkPose(inst, ex::move[i]);
  }
  for (int i = 0; i < ex::EXTRA_COUNT; i++) {
    CHECK(inst.pose(test_rig::anim_extra, ex::extra[i].frame));
    checkPose(inst, ex::extra[i]);
  }
  // Frame 7 lies between the offsets 170 and -170 degrees of bone b: the
  // shortest way is through 180, so b has turned 170 + 20 / 3 degrees from
  // its bind pose (25 degrees) relative to its parent
  CHECK(inst.pose(test_rig::anim_move, 7.0f));
  const g2::affine2f &pa = inst.boneTransform(1), &pb = inst.boneTransform(3);
  g2::affine2f ia = g2::affine2f::identity();
  CHECK(pa.invert(ia));
  const g2::affine2f rel = ia * pb;
  const float angle = std::atan2(rel.b, rel.a) * 180.0f / 3.14159265f;
  CHECK(nearlyEqual(angle, 25.0f + 170.0f + 20.0f / 3.0f - 360.0f, 0.05f));
}

static bool sameWorlds(const rig::Instance &a, const rig::Instance &b) {
  for (int i = 0; i < a.armature()->boneCount; i++) {
    if (std::memcmp(&a.boneTransform(i), &b.boneTransform(i),
                    sizeof(g2::affine2f)) != 0)
      return false;
  }
  return true;
}

// Frames out of range clamp; frameAt()
static void testRigFrames() {
  alignas(4) static uint8_t memA[256], memB[256];
  rig::Instance a, b;
  CHECK(a.init(test_rig::armature, memA, sizeof(memA)));
  CHECK(b.init(test_rig::armature, memB, sizeof(memB)));
  const rig::Animation &move = test_rig::anim_move;
  a.pose(move, -5.0f);
  b.pose(move, 0.0f);
  CHECK(sameWorlds(a, b));
  a.pose(move, 100.0f);
  b.pose(move, 12.0f);
  CHECK(sameWorlds(a, b));
  CHECK_EQ(a.slotAt(2), b.slotAt(2));
  a.pose(move, NAN);
  b.pose(move, 0.0f);
  CHECK(sameWorlds(a, b));

  CHECK_EQ(move.frameRate, 24);
  CHECK(nearlyEqual(rig::frameAt(move, 0.25f), 6.0f));
  CHECK(nearlyEqual(rig::frameAt(move, 0.5f), 0.0f));  // wrapped
  CHECK(nearlyEqual(rig::frameAt(move, 0.625f), 3.0f));
  CHECK(nearlyEqual(rig::frameAt(move, -0.125f), 9.0f));
  CHECK(nearlyEqual(rig::frameAt(move, 1.0f, false), 12.0f));
  CHECK(nearlyEqual(rig::frameAt(move, -1.0f, false), 0.0f));
  CHECK(nearlyEqual(rig::frameAt(move, 0.25f, false), 6.0f));
}

// An animation of another armature is refused and changes nothing
static void testRigSignature() {
  rig::Instance inst;
  CHECK(inst.init(test_rig::armature, rigMemory, sizeof(rigMemory)));
  CHECK(inst.pose(test_rig::anim_move, 3.5f));
  rig::Animation other = test_rig::anim_move;
  other.signature ^= 1;
  CHECK(!inst.pose(other, 9.25f));
  checkPose(inst, ex::move[1]);
  rig::Instance none;
  CHECK(!none.pose(test_rig::anim_move, 0.0f));
}

// Members reserved for later features (mesh, IK, tints, ...): a header
// generated before them leaves them zero, so the old positional initializers
// compile and mean what they did; data with a feature bit this build lacks is
// refused; attachments of the reserved kinds are kept but not drawn
static void testRigReserved() {
  static_assert(rig::FORMAT_VERSION >= 1, "generated headers assert on this");
  static_assert(rig::SUPPORTED_FEATURES == 0, "no feature bit is defined yet");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
  // Initializers as dbones2cpp wrote them before FORMAT_VERSION 1
  static const rig::Bone oldBone = {"b", 1.0f, 2.0f, 0, 0, 4096, 4096, 0xFF};
  static const rig::Attachment oldAtt = {
      nullptr, {0, 0, 1, 1}, {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, nullptr, 0};
  static const rig::Slot oldSlot = {"s", &oldAtt, 1, 0, 0, 255,
                                    g2::BlendMode::ALPHA};
  static const rig::Armature oldArm = {"a", &oldBone, &oldSlot, 1, 1, false, 0,
                                       {0, 0, 1, 1}, 0x12345678u};
  static const rig::RotateKey oldKey = {0, rig::CURVE_LINEAR, 0, 100, 100};
  static const rig::Animation oldAnim = {"m", 10, 24, 0, 0, 0, 0, nullptr,
                                         nullptr, nullptr, nullptr, 0x12345678u};
#pragma GCC diagnostic pop
  CHECK_EQ(oldBone.flags, 0);
  CHECK(oldAtt.kind == rig::AttachmentKind::IMAGE);
  CHECK(oldAtt.ext == nullptr);
  CHECK_EQ(oldSlot.tintR, 0);
  CHECK_EQ(oldSlot.tintG, 0);
  CHECK_EQ(oldSlot.tintB, 0);
  CHECK_EQ(oldArm.features, 0);
  CHECK_EQ(oldKey.turns, 0);
  CHECK_EQ(oldAnim.features, 0);
  rig::Instance inst;
  CHECK(inst.init(oldArm, rigMemory, sizeof(rigMemory)));
  CHECK(inst.pose(oldAnim, 5.0f));
  inst.deinit();

  // A feature bit this build does not know: refused, nothing changes
  rig::Armature arm = test_rig::armature;
  arm.features = 1;
  CHECK(!inst.init(arm, rigMemory, sizeof(rigMemory)));
  CHECK(!inst.isInitialized());
  CHECK(inst.init(test_rig::armature, rigMemory, sizeof(rigMemory)));
  CHECK(inst.pose(test_rig::anim_move, 3.5f));
  rig::Animation anim = test_rig::anim_move;
  anim.features = 1;
  CHECK(!inst.pose(anim, 9.25f));
  checkPose(inst, ex::move[1]);
  inst.deinit();

  // Every attachment of a reserved kind: nothing is visible
  rig::Attachment atts[16];
  rig::Slot slots[ex::SLOTS];
  int n = 0;
  for (int s = 0; s < ex::SLOTS; s++) {
    slots[s] = test_rig::slots[s];
    slots[s].attachments = atts + n;
    for (int a = 0; a < slots[s].attachmentCount; a++) {
      CHECK(n < 16);
      atts[n] = test_rig::slots[s].attachments[a];
      atts[n].kind = rig::AttachmentKind::MESH;
      n++;
    }
  }
  arm = test_rig::armature;
  arm.slots = slots;
  CHECK(inst.init(arm, rigMemory, sizeof(rigMemory)));
  CHECK(inst.bounds().isEmpty());
  CHECK(inst.pose(test_rig::anim_move, 3.5f));
  CHECK(inst.bounds().isEmpty());
  inst.deinit();
}

// Spins bone `bone` by `angle`
class SpinVisitor : public rig::BoneVisitor {
 public:
  int bone = -1, calls = 0;
  rig::angle16_t angle = 0;
  void onBone(int b, rig::BonePose &local) override {
    CHECK_EQ(b, calls);  // parents first, every bone
    calls++;
    if (b == bone) {
      local.rotX += angle;
      local.rotY += angle;
    }
  }
};

static void testRigVisitor() {
  alignas(4) static uint8_t memA[256], memB[256];
  rig::Instance a, b;
  CHECK(a.init(test_rig::armature, memA, sizeof(memA)));
  CHECK(b.init(test_rig::armature, memB, sizeof(memB)));
  const int boneA = a.boneIndex("a"), boneB = a.boneIndex("b"),
            boneC = a.boneIndex("c");
  CHECK(boneA >= 0 && boneB >= 0 && boneC >= 0);
  CHECK_EQ(a.boneIndex("nothing"), -1);
  CHECK_EQ(a.boneIndex(nullptr), -1);
  SpinVisitor v;
  v.bone = boneA;
  v.angle = 8192;  // 45 degrees
  a.pose(test_rig::anim_move, 3.5f, &v);
  CHECK_EQ(v.calls, ex::BONES);
  b.pose(test_rig::anim_move, 3.5f);
  // a turns and b follows it (the same pose relative to a); c stays
  const g2::affine2f &wa = a.boneTransform(boneA), &wb = b.boneTransform(boneA);
  CHECK(!nearlyEqual(wa.a, wb.a));
  CHECK(nearlyEqual(std::atan2(wa.b, wa.a) - std::atan2(wb.b, wb.a),
                    3.14159265f / 4.0f));
  g2::affine2f ia = g2::affine2f::identity(), ib = ia;
  CHECK(wa.invert(ia));
  CHECK(wb.invert(ib));
  const g2::affine2f ra = ia * a.boneTransform(boneB);
  const g2::affine2f rb = ib * b.boneTransform(boneB);
  const float va[6] = {ra.a, ra.b, ra.c, ra.d, ra.tx, ra.ty};
  const float vb[6] = {rb.a, rb.b, rb.c, rb.d, rb.tx, rb.ty};
  for (int k = 0; k < 6; k++) CHECK(nearlyEqual(va[k], vb[k]));
  CHECK(std::memcmp(&a.boneTransform(boneC), &b.boneTransform(boneC),
                    sizeof(g2::affine2f)) == 0);
  // poseBind() takes the visitor too
  SpinVisitor v2;
  a.poseBind(&v2);
  CHECK_EQ(v2.calls, ex::BONES);
  checkPose(a, ex::move[0]);
}

// Hiding a slot and the accessors of the draw order
static void testRigSlots() {
  rig::Instance inst;
  CHECK(inst.init(test_rig::armature, rigMemory, sizeof(rigMemory)));
  inst.pose(test_rig::anim_move, 9.25f);  // draw order changed at frame 7
  for (int i = 0; i < ex::SLOTS; i++) {
    CHECK_EQ(inst.drawIndexOf(inst.slotAt(i)), i);
  }
  CHECK_EQ(inst.slotAt(-1), -1);
  CHECK_EQ(inst.slotAt(ex::SLOTS), -1);
  CHECK_EQ(inst.drawIndexOf(ex::SLOTS), -1);
  const int sa = inst.slotIndex("sa"), sc = inst.slotIndex("sc");
  CHECK(sa >= 0 && sc >= 0);
  CHECK_EQ(inst.slotIndex("nothing"), -1);
  CHECK(inst.drawIndexOf(sa) > inst.drawIndexOf(sc));
  inst.pose(test_rig::anim_move, 3.0f);
  CHECK(inst.drawIndexOf(sa) < inst.drawIndexOf(sc));

  // Hidden slots leave the bounds
  const g2::RectF all = inst.bounds();
  for (int s = 0; s < ex::SLOTS; s++) inst.setAttachment(s, -1);
  CHECK_EQ(inst.attachmentOf(0), -1);
  CHECK(inst.bounds().width == 0.0f && inst.bounds().height == 0.0f);
  inst.setAttachment(0, 0);
  const g2::RectF one = inst.bounds();
  CHECK(one.width > 0.0f && one.width <= all.width);
  CHECK(one.x >= all.x && one.right() <= all.right());
  inst.setAlpha(0, 300);
  CHECK_EQ(inst.alphaOf(0), 255);
  inst.setAlpha(0, 7);
  CHECK_EQ(inst.alphaOf(0), 7);
  inst.pose(test_rig::anim_move, 3.0f);  // pose() overrides both
  CHECK_EQ(inst.attachmentOf(1), 0);
  CHECK(inst.alphaOf(0) > 7);
}

// --- Drawing -------------------------------------------------------------------

#if SHAPOGFX2D_TRANSFORM && SHAPOGFX_FORMAT_ARGB4444 && \
    SHAPOGFX_FORMAT_RGB565_SWAPPED
#define RIG_DRAW_TESTS 1
#else
#define RIG_DRAW_TESTS 0
#endif

#if RIG_DRAW_TESTS
static constexpr int DW = 72, DH = 56;

static g2::affine2f placement() {
  return g2::affine2f::placement(36, 28, 0.3f, 1.5f, 1.5f, 24, 20);
}

static void clearTarget(g2::Graphics2D &g) {
  g.resetClipRect();
  g.clear(g2::makeColor(30, 40, 50));
}

// What draw() is documented to do, spelled out with the public accessors
// (without the hulls: the whole rectangles). A keyed armature's key goes on
// for its keyed textures only.
static void drawByHand(g2::Graphics2D &g, const rig::Instance &inst,
                       bool hulls = true) {
  const rig::Armature &arm = *inst.armature();
  const g2::affine2f base = g.transform();
  for (int i = 0; i < arm.slotCount; i++) {
    const int s = inst.slotAt(i);
    const int att = inst.attachmentOf(s), alpha = inst.alphaOf(s);
    if (att < 0 || alpha == 0) continue;
    const rig::Slot &sl = arm.slots[s];
    const rig::Attachment &at = sl.attachments[att];
    if (arm.colorKeyEnabled) {
      if (at.texture->format != g2::PixelFormat::ARGB4444) {
        g.setColorKey(arm.colorKey);
      } else {
        g.clearColorKey();
      }
    }
    g.setBlend(sl.blend, alpha == 255 ? 255 : (255 * alpha + 127) / 255);
    g.setTransform(base * inst.boneTransform(sl.bone) * at.local);
    if (hulls) {
      g.drawImage(*at.texture, 0, 0, at.src, at.hull, at.hullCount);
    } else {
      g.drawImage(*at.texture, 0, 0, at.src);
    }
  }
  g.setTransform(base);
  g.setBlend(g2::BlendMode::ALPHA, 255);
  if (arm.colorKeyEnabled) g.clearColorKey();
}

static bool samePixels(const g2::OwnedSurface &a, const g2::OwnedSurface &b) {
  return std::memcmp(a.pixels(), b.pixels(), a.bytes()) == 0;
}

static int countDiffering(const g2::OwnedSurface &a,
                          const g2::OwnedSurface &b) {
  const uint16_t *pa = (const uint16_t *)a.pixels();
  const uint16_t *pb = (const uint16_t *)b.pixels();
  int n = 0;
  for (int i = 0; i < DW * DH; i++) n += pa[i] != pb[i];
  return n;
}

// Count, centroid and orientation (radians, of the longer axis) of the
// pixels drawn at least half opaque into a transparent ARGB4444 target
struct Blob {
  int count;
  float cx, cy, angle;
};
static Blob blobOf(const g2::OwnedSurface &s) {
  const g2::Graphics2D g(s);
  double n = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
  for (int y = 0; y < s.height(); y++) {
    for (int x = 0; x < s.width(); x++) {
      if (g2::colorA(g.getPixel(x, y)) < 128) continue;
      n++;
      sx += x;
      sy += y;
      sxx += (double)x * x;
      syy += (double)y * y;
      sxy += (double)x * y;
    }
  }
  if (n == 0) return {0, 0, 0, 0};
  const double mx = sx / n, my = sy / n;
  const double cxx = sxx / n - mx * mx, cyy = syy / n - my * my,
               cxy = sxy / n - mx * my;
  return {(int)n, (float)mx, (float)my,
          (float)(0.5 * std::atan2(2 * cxy, cxx - cyy))};
}

static void testRigDraw() {
  const float frames[] = {0.0f, 3.5f, 7.0f, 9.25f};
  for (g2::PixelFormat fmt :
       {g2::PixelFormat::RGB565_SWAPPED, g2::PixelFormat::ARGB4444}) {
    g2::OwnedSurface a = g2::createSurface(fmt, DW, DH);
    g2::OwnedSurface b = g2::createSurface(fmt, DW, DH);
    g2::Graphics2D ga(a), gb(b);
    rig::Instance inst;
    CHECK(inst.init(test_rig::armature, rigMemory, sizeof(rigMemory)));
    for (float f : frames) {
      inst.pose(test_rig::anim_move, f);
      clearTarget(ga);
      clearTarget(gb);
      ga.setTransform(placement());
      gb.setTransform(placement());
      inst.draw(ga);
      drawByHand(gb, inst);
      CHECK(samePixels(a, b));
    }
    // Something was drawn
    g2::OwnedSurface empty = g2::createSurface(fmt, DW, DH);
    g2::Graphics2D ge(empty);
    clearTarget(ge);
    CHECK(countDiffering(a, empty) > 200);
  }
}

// The hulls cut only pixels that would not have been drawn: with them or
// without, the picture is the same, on the ARGB4444 atlas and the keyed one
static void testRigHulls() {
  int hulls = 0;
  for (int s = 0; s < test_rig::armature.slotCount; s++) {
    const rig::Slot &sl = test_rig::armature.slots[s];
    for (int a = 0; a < sl.attachmentCount; a++) hulls += sl.attachments[a].hullCount >= 3;
  }
  CHECK_EQ(hulls, 6);  // every attachment of the test armature has one
  const struct {
    const rig::Armature *arm;
    const rig::Animation *anim;
  } cases[] = {{&test_rig::armature, &test_rig::anim_move},
               {&test_rig_keyed::armature, &test_rig_keyed::anim_move}};
  for (const auto &c : cases) {
    const rig::Armature *arm = c.arm;
    g2::OwnedSurface a = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
    g2::OwnedSurface b = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
    g2::Graphics2D ga(a), gb(b);
    rig::Instance inst;
    CHECK(inst.init(*arm, rigMemory, sizeof(rigMemory)));
    for (float f : {0.0f, 5.0f, 9.25f}) {
      inst.pose(*c.anim, f);
      for (float sc : {1.0f, 2.5f}) {
        clearTarget(ga);
        clearTarget(gb);
        const g2::affine2f p = g2::affine2f::placement(36, 28, 0.3f, sc, sc, 24, 20);
        ga.setTransform(p);
        gb.setTransform(p);
        inst.draw(ga);
        drawByHand(gb, inst, false);
        CHECK(samePixels(a, b));
      }
    }
  }
}

// --out-format auto: f, translucent inside, keeps its alpha (ARGB4444) while
// the others get the key color; each part draws what it draws in the
// armature converted to that format alone, and draw() switches the key
// between them
static void testRigMixed() {
  const rig::Armature &mixed = test_rig_mixed::armature;
  CHECK(mixed.colorKeyEnabled);
  CHECK_EQ(mixed.colorKey & 0xFFFFFFu, test_rig_keyed::armature.colorKey & 0xFFFFFFu);
  rig::Instance m, k, a;
  alignas(4) static uint8_t memM[256], memK[256], memA[256];
  CHECK(m.init(mixed, memM, sizeof(memM)));
  CHECK(k.init(test_rig_keyed::armature, memK, sizeof(memK)));
  CHECK(a.init(test_rig::armature, memA, sizeof(memA)));
  const int sf = m.slotIndex("sf");
  CHECK(sf >= 0);
  for (int s = 0; s < mixed.slotCount; s++) {
    const rig::Slot &sl = mixed.slots[s];
    for (int i = 0; i < sl.attachmentCount; i++) {
      CHECK_EQ((int)sl.attachments[i].texture->format,
               (int)(s == sf ? g2::PixelFormat::ARGB4444 : g2::PixelFormat::RGB565_SWAPPED));
    }
  }
  g2::OwnedSurface sm = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::OwnedSurface so = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::Graphics2D gm(sm), go(so);
  for (float f : {0.0f, 5.0f, 9.25f}) {
    m.pose(test_rig_mixed::anim_move, f);
    k.pose(test_rig_keyed::anim_move, f);
    a.pose(test_rig::anim_move, f);
    // Everything but f against the keyed conversion, f alone against the
    // ARGB4444 one
    for (int pass = 0; pass < 2; pass++) {
      rig::Instance &other = pass == 0 ? k : a;
      for (int s = 0; s < mixed.slotCount; s++) {
        if ((s == sf) == (pass == 0)) {
          m.setAttachment(s, -1);
          other.setAttachment(s, -1);
        }
      }
      clearTarget(gm);
      clearTarget(go);
      gm.setTransform(placement());
      go.setTransform(placement());
      m.draw(gm);
      other.draw(go);
      CHECK(samePixels(sm, so));
      m.pose(test_rig_mixed::anim_move, f);
      other.pose(pass == 0 ? test_rig_keyed::anim_move : test_rig::anim_move, f);
    }
    // The whole thing is drawByHand's picture too, with the key toggled
    clearTarget(gm);
    clearTarget(go);
    gm.setTransform(placement());
    go.setTransform(placement());
    m.draw(gm);
    drawByHand(go, m);
    CHECK(samePixels(sm, so));
  }
  // A key the caller had set is back afterwards
  gm.setColorKey(g2::Colors::RED);
  m.draw(gm);
  CHECK(gm.hasColorKey() && gm.colorKey() == g2::Colors::RED);
}

// --fit-rotate: the diagonal bar e is turned upright (a quarter of the
// texels) and drawn back where it was, resampled once
static void testRigFit() {
  const rig::Armature &fit = test_rig_fit::armature;
  const rig::Armature &ref = test_rig::armature;
  rig::Instance a, b;
  alignas(4) static uint8_t memA[256], memB[256];
  CHECK(a.init(fit, memA, sizeof(memA)));
  CHECK(b.init(ref, memB, sizeof(memB)));
  const int se = b.slotIndex("se");
  CHECK(se >= 0 && a.slotIndex("se") == se);
  const rig::Attachment &atFit = fit.slots[se].attachments[0];
  const rig::Attachment &atRef = ref.slots[se].attachments[0];
  CHECK(atFit.src.width * atFit.src.height * 2 < atRef.src.width * atRef.src.height);
  CHECK(atFit.hullCount >= 3);
  // Only e is turned: the others are the same pixels at the same places
  g2::OwnedSurface sa = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::OwnedSurface sb = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::Graphics2D ga(sa), gb(sb);
  a.poseBind();
  b.poseBind();
  for (int s = 0; s < ref.slotCount; s++) {
    if (s == se) {
      a.setAttachment(s, -1);
      b.setAttachment(s, -1);
    }
  }
  clearTarget(ga);
  clearTarget(gb);
  ga.setTransform(placement());
  gb.setTransform(placement());
  a.draw(ga);
  b.draw(gb);
  CHECK(samePixels(sa, sb));
  // e alone, four times enlarged: the bar lands where the original does
  // (centroid within a pixel, so within a quarter of a texel), at the same
  // angle and about the same size. Its resampled edges differ pixel by
  // pixel, which is why the pixels are not compared one by one.
  a.poseBind();
  b.poseBind();
  g2::OwnedSurface la = g2::createSurface(g2::PixelFormat::ARGB4444, 160, 120);
  g2::OwnedSurface lb = g2::createSurface(g2::PixelFormat::ARGB4444, 160, 120);
  g2::Graphics2D gla(la), glb(lb);
  const g2::affine2f p = g2::affine2f::placement(80, 60, 0.3f, 4.0f, 4.0f, 24, 20);
  const int k = b.drawIndexOf(se);
  gla.clear(g2::Colors::TRANSPARENT);
  glb.clear(g2::Colors::TRANSPARENT);
  gla.setTransform(p);
  glb.setTransform(p);
  a.draw(gla, k, k + 1);
  b.draw(glb, k, k + 1);
  const Blob fa = blobOf(la), fb = blobOf(lb);
  CHECK(fb.count > 400);
  CHECK(fa.count > fb.count * 7 / 10 && fa.count < fb.count * 14 / 10);
  CHECK(std::fabs(fa.cx - fb.cx) <= 1.0f && std::fabs(fa.cy - fb.cy) <= 1.0f);
  CHECK(std::fabs(fa.angle - fb.angle) <= 0.06f);
}

// One texture per image draws what the atlas draws
static void testRigSeparateTextures() {
  g2::OwnedSurface a = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::OwnedSurface b = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::Graphics2D ga(a), gb(b);
  alignas(4) static uint8_t memA[256], memB[256];
  rig::Instance ia, ib;
  CHECK(ia.init(test_rig::armature, memA, sizeof(memA)));
  CHECK(ib.init(test_rig_sep::armature, memB, sizeof(memB)));
  CHECK(test_rig_sep::slots[0].attachments[0].texture !=
        test_rig_sep::slots[0].attachments[1].texture);
  for (float f : {0.0f, 7.0f, 9.25f}) {
    ia.pose(test_rig::anim_move, f);
    CHECK(ib.pose(test_rig_sep::anim_move, f));
    clearTarget(ga);
    clearTarget(gb);
    ga.setTransform(placement());
    gb.setTransform(placement());
    ia.draw(ga);
    ib.draw(gb);
    CHECK(samePixels(a, b));
  }
}

// RGB565 images with a key color: the key never shows, and draw() sets the
// key only for its images
static void testRigColorKey() {
  const rig::Armature &arm = test_rig_keyed::armature;
  CHECK(arm.colorKeyEnabled);
  CHECK_EQ(arm.colorKey, g2::makeColor(255, 0, 255));
  CHECK_EQ((int)test_rig_keyed::atlas.format,
           (int)g2::PixelFormat::RGB565_SWAPPED);
  g2::OwnedSurface s = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::Graphics2D g(s);
  rig::Instance inst;
  CHECK(inst.init(arm, rigMemory, sizeof(rigMemory)));
  clearTarget(g);
  g.setTransform(placement());
  inst.draw(g);
  CHECK(!g.hasColorKey());
#if SHAPOGFX2D_COLOR_KEY
  const uint16_t key = g2::bswap16(0xF81F);
  const uint16_t *p = (const uint16_t *)s.pixels();
  int keyed = 0, drawn = 0;
  const uint16_t bg = p[0];
  for (int i = 0; i < DW * DH; i++) {
    keyed += p[i] == key;
    drawn += p[i] != bg;
  }
  CHECK_EQ(keyed, 0);
  CHECK(drawn > 200);
  // A key set by the caller is restored
  g.setColorKey(g2::Colors::WHITE);
  inst.draw(g);
  CHECK(g.hasColorKey());
  CHECK_EQ(g.colorKey(), g2::Colors::WHITE);
#endif
}

// Ranges of the draw order, and the state of the Graphics2D afterwards
static void testRigDrawRange() {
  g2::OwnedSurface a = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::OwnedSurface b = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::Graphics2D ga(a), gb(b);
  rig::Instance inst;
  CHECK(inst.init(test_rig::armature, rigMemory, sizeof(rigMemory)));
  inst.pose(test_rig::anim_move, 9.25f);
  for (int k = 0; k <= ex::SLOTS; k++) {
    clearTarget(ga);
    clearTarget(gb);
    ga.setTransform(placement());
    gb.setTransform(placement());
    inst.draw(ga);
    inst.draw(gb, -3, k);
    inst.draw(gb, k, 99);
    CHECK(samePixels(a, b));
  }
  // An empty range draws nothing
  clearTarget(gb);
  const g2::OwnedSurface &cleared = b;
  g2::OwnedSurface c = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::Graphics2D gc(c);
  clearTarget(gc);
  gb.setTransform(placement());
  inst.draw(gb, 2, 2);
  inst.draw(gb, 3, 1);
  CHECK(samePixels(cleared, c));

  // Hiding a slot changes the picture
  clearTarget(ga);
  ga.setTransform(placement());
  inst.setAttachment(inst.slotIndex("sc"), -1);
  inst.draw(ga);
  clearTarget(gb);
  gb.setTransform(placement());
  drawByHand(gb, inst);
  CHECK(samePixels(a, b));
  inst.pose(test_rig::anim_move, 9.25f);
  clearTarget(gb);
  gb.setTransform(placement());
  inst.draw(gb);
  CHECK(countDiffering(a, b) > 10);

  // The state is restored
  const g2::affine2f m = placement();
  ga.setTransform(m);
  ga.setBlend(g2::BlendMode::ALPHA, 200);
  ga.setColorKey(g2::Colors::RED);
  const g2::GraphicsState2D before = ga.state();
  inst.draw(ga);
  const g2::GraphicsState2D after = ga.state();
  CHECK(std::memcmp(&before.transform, &after.transform, sizeof(m)) == 0);
  CHECK_EQ(before.opacity, after.opacity);
  CHECK_EQ((int)before.blendMode, (int)after.blendMode);
  CHECK_EQ(before.colorKeyEnabled, after.colorKeyEnabled);
  CHECK_EQ(before.colorKey, after.colorKey);
}

// Every pixel drawn lies within bounds(placement)
static void testRigBounds() {
  g2::OwnedSurface s = g2::createSurface(g2::PixelFormat::ARGB4444, DW, DH);
  g2::Graphics2D g(s);
  rig::Instance inst;
  CHECK(inst.init(test_rig::armature, rigMemory, sizeof(rigMemory)));
  for (float f : {0.0f, 3.5f, 9.25f}) {
    inst.pose(test_rig::anim_move, f);
    g.resetClipRect();
    g.clear(g2::Colors::TRANSPARENT);
    g.setTransform(placement());
    inst.draw(g);
    const g2::RectF b = inst.bounds(placement());
    int drawn = 0, outside = 0;
    const uint16_t *p = (const uint16_t *)s.pixels();
    for (int y = 0; y < DH; y++) {
      for (int x = 0; x < DW; x++) {
        if (!p[y * DW + x]) continue;
        drawn++;
        if (x < std::floor(b.x) || x >= std::ceil(b.right()) ||
            y < std::floor(b.y) || y >= std::ceil(b.bottom()))
          outside++;
      }
    }
    CHECK(drawn > 100);
    CHECK_EQ(outside, 0);
  }
  // Placement of an empty box
  const g2::RectF e = rig::Instance().bounds(placement());
  CHECK(nearlyEqual(e.width, 0.0f) && nearlyEqual(e.height, 0.0f));
}

// Drawing in bands (clip rectangles) puts the same pixels as drawing at once:
// the slots skipped outside a band do not reach into it
static void testRigBands() {
  g2::OwnedSurface a = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::OwnedSurface b = g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
  g2::Graphics2D ga(a), gb(b);
  rig::Instance inst;
  CHECK(inst.init(test_rig::armature, rigMemory, sizeof(rigMemory)));
  for (float f : {0.0f, 7.0f, 9.25f}) {
    inst.pose(test_rig::anim_move, f);
    clearTarget(ga);
    clearTarget(gb);
    ga.setTransform(placement());
    inst.draw(ga);
    gb.setTransform(placement());
    for (int y = 0; y < DH; y += 5) {
      gb.setClipRect(0, y, DW, 5);
      inst.draw(gb);
    }
    CHECK(samePixels(a, b));
    clearTarget(gb);
    gb.setTransform(placement());
    for (int x = 0; x < DW; x += 3) {
      gb.setClipRect(x, 0, 3, DH);
      inst.draw(gb);
    }
    CHECK(samePixels(a, b));
  }
}

// draw() only reads the Instance: two contexts, each with its own arena, draw
// the halves of one frame from it at the same time on two threads and put
// the same pixels as one call. The halves are clip rectangles on the whole
// frame first and targets of their own then (as the M5Stack demorig does).
static void testRigThreads() {
  alignas(8) static uint8_t arenas[2][8192];
  CHECK(g2::Graphics2D::arenaBytes() <= sizeof(arenas[0]));
  const struct {
    const rig::Armature *arm;
    const rig::Animation *anim;
  } cases[] = {{&test_rig::armature, &test_rig::anim_move},
               {&test_rig_mixed::armature, &test_rig_mixed::anim_move}};
  constexpr int UPPER = 25, LOWER = DH - UPPER;
  for (const auto &c : cases) {
    g2::OwnedSurface whole =
        g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
    g2::OwnedSurface split =
        g2::createSurface(g2::PixelFormat::RGB565_SWAPPED, DW, DH);
    const g2::Surface upper = g2::makeSurface(split.format(), DW, UPPER,
                                              split.surface().linePtr(0));
    const g2::Surface lower = g2::makeSurface(split.format(), DW, LOWER,
                                              split.surface().linePtr(UPPER));
    g2::Graphics2D gw(whole), g[2];
    CHECK(g[0].init(arenas[0], sizeof(arenas[0])));
    CHECK(g[1].init(arenas[1], sizeof(arenas[1])));
    rig::Instance inst;
    CHECK(inst.init(*c.arm, rigMemory, sizeof(rigMemory)));
    for (int i = 0; i < 40; i++) {
      inst.pose(*c.anim, 0.25f * (float)i);
      clearTarget(gw);
      gw.setTransform(placement());
      inst.draw(gw);

      for (int k = 0; k < 2; k++) {
        g[k].setTarget(split);
        g[k].setTransform(placement());
      }
      clearTarget(g[0]);
      g[0].setClipRect(0, 0, DW, UPPER);
      g[1].setClipRect(0, UPPER, DW, LOWER);
      {
        std::thread t([&] { inst.draw(g[1]); });
        inst.draw(g[0]);
        t.join();
      }
      CHECK(samePixels(whole, split));

      clearTarget(g[0]);
      g[0].setTarget(upper);
      g[1].setTarget(lower);
      g[0].setTransform(placement());
      g[1].setTransform(g2::affine2f::translation(0, -(float)UPPER) *
                        placement());
      {
        std::thread t([&] { inst.draw(g[1]); });
        inst.draw(g[0]);
        t.join();
      }
      CHECK(samePixels(whole, split));
    }
  }
}
#endif  // RIG_DRAW_TESTS
#endif  // SHAPOGFX2D_RIG

void testRig() {
#if SHAPOGFX2D_RIG
  testRigInit();
  testRigPose();
  testRigFrames();
  testRigSignature();
  testRigReserved();
  testRigVisitor();
  testRigSlots();
#if RIG_DRAW_TESTS
  testRigDraw();
  testRigHulls();
  testRigMixed();
  testRigFit();
  testRigSeparateTextures();
  testRigColorKey();
  testRigDrawRange();
  testRigBounds();
  testRigBands();
  testRigThreads();
#else
  std::printf("  drawing skipped (transform or pixel formats disabled)\n");
#endif
#else
  std::printf("  skipped (SHAPOGFX2D_RIG=0)\n");
#endif
}
