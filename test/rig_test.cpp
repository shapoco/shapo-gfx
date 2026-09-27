// rig::Instance against the poses bin/shapogfx_dbones.py computed for the test
// armature (test/data/test_rig*.hpp; regenerate with
// test/tools/make_test_rig.py).

#include <cmath>
#include <cstdint>
#include <cstring>

#include "check.hpp"
#include "data/test_rig.hpp"
#include "data/test_rig_expected.hpp"
#include "data/test_rig_keyed.hpp"
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
  // 4 world transforms, 4 slot states, 4 order bytes; 3 bytes of slack
  CHECK_EQ(rig::Instance::bytes(arm), 3u + 4 * 24 + 4 * 12 + 4);
  rig::Instance inst;
  CHECK(!inst.isInitialized());
  CHECK(!inst.init(arm, rigMemory, 4 * 24 + 4 * 12 + 4 - 1));
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
static void drawByHand(g2::Graphics2D &g, const rig::Instance &inst) {
  const rig::Armature &arm = *inst.armature();
  const g2::affine2f base = g.transform();
  for (int i = 0; i < arm.slotCount; i++) {
    const int s = inst.slotAt(i);
    const int att = inst.attachmentOf(s), alpha = inst.alphaOf(s);
    if (att < 0 || alpha == 0) continue;
    const rig::Slot &sl = arm.slots[s];
    const rig::Attachment &at = sl.attachments[att];
    g.setBlend(sl.blend, alpha == 255 ? 255 : (255 * alpha + 127) / 255);
    g.setTransform(base * inst.boneTransform(sl.bone) * at.local);
    g.drawImage(*at.texture, 0, 0, at.src);
  }
  g.setTransform(base);
  g.setBlend(g2::BlendMode::ALPHA, 255);
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
#endif  // RIG_DRAW_TESTS
#endif  // SHAPOGFX2D_RIG

void testRig() {
#if SHAPOGFX2D_RIG
  testRigInit();
  testRigPose();
  testRigFrames();
  testRigSignature();
  testRigVisitor();
  testRigSlots();
#if RIG_DRAW_TESTS
  testRigDraw();
  testRigSeparateTextures();
  testRigColorKey();
  testRigDrawRange();
  testRigBounds();
  testRigBands();
#else
  std::printf("  drawing skipped (transform or pixel formats disabled)\n");
#endif
#else
  std::printf("  skipped (SHAPOGFX2D_RIG=0)\n");
#endif
}
