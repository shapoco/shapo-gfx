#include "scene.hpp"

#include <cstring>

#include <cmath>
#include <cstdint>

// Windmill model generated from model/windmill.glb with bin/gltf2cpp
#include "model/windmill.hpp"

namespace demo3d {

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;
using g3::colorf;
using g3::vec2f;
using g3::vec3f;

static constexpr float PI = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Textures (on a real target these would be const data in flash;
// here they are generated at startup). RGB565BE: byte-swapped in memory.

static uint16_t checkerPixels[64 * 64];
static uint16_t envPixels[64 * 64];

static const g3::Texture texChecker = {g3::PixelFormat::RGB565BE, 64, 64, 128,
                                       checkerPixels};
static const g3::Texture texEnv = {g3::PixelFormat::RGB565BE, 64, 64, 128,
                                   envPixels};

// Deterministic 2D hash (0..1)
static float hash2(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (float)(h & 0xffff) / 65535.0f;
}

// Value noise (smoothly interpolated hash)
static float valueNoise(float x, float y) {
  int xi = (int)std::floor(x), yi = (int)std::floor(y);
  float fx = x - xi, fy = y - yi;
  fx = fx * fx * (3 - 2 * fx);  // smoothstep
  fy = fy * fy * (3 - 2 * fy);
  float n00 = hash2(xi, yi), n10 = hash2(xi + 1, yi);
  float n01 = hash2(xi, yi + 1), n11 = hash2(xi + 1, yi + 1);
  float n0 = n00 + (n10 - n00) * fx;
  float n1 = n01 + (n11 - n01) * fx;
  return n0 + (n1 - n0) * fy;
}

static void generateTextures() {
  // Checkerboard
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 64; x++) {
      bool c = ((x >> 4) ^ (y >> 4)) & 1;
      checkerPixels[y * 64 + x] = c ? g2::packRgb565BE(0.85f, 0.85f, 0.9f)
                                    : g2::packRgb565BE(0.35f, 0.4f, 0.5f);
    }
  }
  // Environment map:
  // - upper half (v < 0.5): sky gradient with additive cloud noise
  // - lower half (v >= 0.5): checkerboard that looks like a reflection of the
  // floor
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 64; x++) {
      float u = (x + 0.5f) / 64.0f;
      float v = (y + 0.5f) / 64.0f;
      float r, g, b;
      if (v < 0.5f) {
        // Sky: bright at the zenith (v=0), darker towards the horizon (v=0.5)
        float sky = 1.0f - v * 2.0f;
        r = 0.15f + 0.4f * sky;
        g = 0.2f + 0.5f * sky;
        b = 0.35f + 0.55f * sky;
        // Clouds: two octaves of value noise, added
        float n = valueNoise(u * 6.0f, v * 6.0f) * 0.65f +
                  valueNoise(u * 13.0f, v * 13.0f) * 0.35f;
        float cloud = (n - 0.45f) * 1.6f;
        if (cloud > 0) {
          r += cloud;
          g += cloud;
          b += cloud * 0.9f;
        }
      } else {
        // Floor reflection: checkerboard in the floor colors, fading towards
        // the bottom
        bool c = ((x >> 3) ^ ((y - 32) >> 3)) & 1;
        float fade = 1.0f - (v - 0.5f) * 1.2f;
        if (c) {
          r = 0.85f * fade;
          g = 0.85f * fade;
          b = 0.9f * fade;
        } else {
          r = 0.35f * fade;
          g = 0.4f * fade;
          b = 0.5f * fade;
        }
      }
      envPixels[y * 64 + x] = g2::packRgb565BE(r, g, b);
    }
  }
}

// ---------------------------------------------------------------------------
// Materials

static const g3::Material matFloor = {
    {0.9f, 0.9f, 0.9f, 1.0f}, {0.9f, 0.9f, 0.9f, 1.0f},   &texChecker,
    g3::BlendMode::NONE,      g3::MaterialFlags::TEXTURE,
};
static const g3::Material matChrome = {
    {0.9f, 0.95f, 1.0f, 1.0f},
    {0.9f, 0.95f, 1.0f, 1.0f},
    &texEnv,
    g3::BlendMode::NONE,
    g3::MaterialFlags::TEXTURE | g3::MaterialFlags::ENV_MAP,
};
static const g3::Material matRed = {
    {0.9f, 0.15f, 0.1f, 1.0f},
    {0.9f, 0.15f, 0.1f, 1.0f},
    nullptr,
    g3::BlendMode::NONE,
    0,
};
static const g3::Material matGlass = {
    {0.4f, 0.7f, 1.0f, 0.45f}, {0.4f, 0.7f, 1.0f, 1.0f},        nullptr,
    g3::BlendMode::ALPHA,      g3::MaterialFlags::DOUBLE_SIDED,
};
static const g3::Material matGlow = {
    {1.0f, 0.7f, 0.2f, 0.8f}, {1.0f, 0.7f, 0.2f, 1.0f},        nullptr,
    g3::BlendMode::ADD,       g3::MaterialFlags::DOUBLE_SIDED,
};

// ---------------------------------------------------------------------------
// Windmill (static scene from gltf2cpp); the "Blades" node is spun by a visitor

class BladeSpinner : public g3::NodeVisitor {
 public:
  float angle = 0.0f;
  bool onNode(const g3::Node &node, g3::mat4f &local) override {
    if (node.name && std::strcmp(node.name, "Blades") == 0) {
      local = local * g3::mat4f::rotation(angle, {0, 0, 1});
    }
    return true;
  }
};

// ---------------------------------------------------------------------------
// API

void sceneInit() { generateTextures(); }

void sceneBuild(g3::Graphics3D &r, float t, float yaw, float pitch, float dist,
                float aspect) {
  r.setPerspectiveProjection(60.0f * PI / 180.0f, aspect, 0.3f, 100.0f);

  r.beginScene();

  // Camera
  r.loadIdentity();
  r.translate(0, 0, -dist);
  r.rotate(pitch, 1, 0, 0);
  r.rotate(yaw, 0, 1, 0);
  r.translate(0, -0.2f, 0);

  // Lights (specified in world space)
  r.enableParallelLight({-0.5f, -1.0f, -0.6f}, {1.0f, 0.98f, 0.9f, 1.0f});
  r.enableEnvironmentLight({0.25f, 0.28f, 0.38f, 1.0f});

  // Floor: one large quad per face. With the default vertical perspective
  // correction (SHAPOGFX3D_CORRECT_PERSPECTIVE=1) the texture stays straight
  // without subdividing; with level 0 you would want divs=4 or so.
  r.setMaterial(matFloor);
  r.putCube({0, -1.35f, 0}, {7.0f, 0.3f, 7.0f});

  // Chrome torus
  r.pushState();
  r.translate(0, 0.5f, 0);
  r.rotate(t * 0.6f, 0, 1, 0);
  r.rotate(0.9f + 0.3f * std::sin(t * 0.4f), 1, 0, 0.2f);
  r.setMaterial(matChrome);
  r.putTorus({0, 0, 0}, 1.0f, 0.35f, 24, 12);
  r.popState();

  // Red cube
  r.pushState();
  r.translate(-2.2f, -0.55f, 1.0f);
  r.rotate(t * 0.9f, 0.3f, 1, 0);
  r.setMaterial(matRed);
  r.putCube({0, 0, 0}, {1.0f, 1.0f, 1.0f});
  r.popState();

  // Translucent cube (orbiting)
  r.pushState();
  r.rotate(t * 0.5f, 0, 1, 0);
  r.translate(2.4f, -0.3f, 0);
  r.rotate(t * 1.2f, 1, 0.5f, 0);
  r.setMaterial(matGlass);
  r.putCube({0, 0, 0}, {1.2f, 1.2f, 1.2f});
  r.popState();

  // Windmill: a glTF model drawn with putScene(); the blades rotate through the
  // visitor
  BladeSpinner spinner;
  spinner.angle = t * 1.5f;
  r.pushState();
  r.translate(-2.4f, -1.2f, -2.2f);
  r.rotate(0.6f, 0, 1, 0);
  r.scale(0.75f, 0.75f, 0.75f);
  r.putScene(windmill::scene, &spinner);
  r.popState();

  // Additive glowing cube (orbiting the other way)
  r.pushState();
  r.rotate(-t * 0.8f, 0, 1, 0);
  r.translate(1.7f, 0.4f + 0.4f * std::sin(t * 1.7f), 1.7f);
  r.rotate(t * 2.0f, 1, 1, 0);
  r.setMaterial(matGlow);
  r.putCube({0, 0, 0}, {0.5f, 0.5f, 0.5f});
  r.popState();

  r.endScene();
}

}  // namespace demo3d
