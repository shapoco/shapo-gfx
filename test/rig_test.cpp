// rig::Instance against the poses bin/shapogfx_dbones.py computed for the test
// armature (test/data/test_rig*.hpp; regenerate with
// test/tools/make_test_rig.py).

#include <cmath>
#include <cstdint>
#include <cstring>

#include "check.hpp"
#include "data/test_rig.hpp"
#include "data/test_rig_expected.hpp"
#include "shapoco/gfx2d/rig.hpp"

namespace g2 = shapoco::gfx2d;
namespace rig = shapoco::gfx2d::rig;
namespace ex = test_rig_expected;

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
  // Frame 9.25 lies between -350 and 0 degrees on bone b: the shortest way
  // is +10 -> 0, so b turns less than 10 degrees from its bind pose
  // relative to its parent
  CHECK(inst.pose(test_rig::anim_move, 9.25f));
  const g2::affine2f &pa = inst.boneTransform(1), &pb = inst.boneTransform(3);
  g2::affine2f ia;
  CHECK(pa.invert(ia));
  const g2::affine2f rel = ia * pb;
  const float angle = std::atan2(rel.b, rel.a) * 180.0f / 3.14159265f;
  CHECK(angle > 25.0f && angle < 35.0f);  // bind 25, offset (0, 10)
}

void testRig() {
#if SHAPOGFX2D_RIG
  testRigInit();
  testRigPose();
#else
  std::printf("  skipped (SHAPOGFX2D_RIG=0)\n");
#endif
}
