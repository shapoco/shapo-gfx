#include "scene.hpp"

#include <cmath>
#include <cstdint>

namespace demo3d {

namespace g2 = shapoco::gfx2d;
namespace g3 = shapoco::gfx3d;
using g3::colorf;
using g3::vec2f;
using g3::vec3f;

static constexpr float PI = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Textures (on a real target these would be const data in flash;
// here they are generated at startup)

static uint16_t checkerPixels[64 * 64];
static uint16_t envPixels[64 * 64];

static const g3::Texture texChecker = {64, 64, checkerPixels};
static const g3::Texture texEnv = {64, 64, envPixels};

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
      checkerPixels[y * 64 + x] = c ? g2::packRgb565(0.85f, 0.85f, 0.9f)
                                    : g2::packRgb565(0.35f, 0.4f, 0.5f);
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
      envPixels[y * 64 + x] = g2::packRgb565(r, g, b);
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
// Torus (also exercises TRIANGLE_STRIP)

static constexpr int TORUS_MAJOR = 24;   // segments around the major circle
static constexpr int TORUS_MINOR = 12;   // segments around the cross section
static constexpr float TORUS_R = 1.0f;   // major radius
static constexpr float TORUS_r = 0.35f;  // minor radius

static g3::Vertex torusVerts[TORUS_MAJOR * TORUS_MINOR];
static uint16_t
    torusIndices[TORUS_MAJOR * (TORUS_MINOR * 2 + 2) + (TORUS_MAJOR - 1) * 2];
static g3::VertexBuffer torusVb;
static g3::Primitive torusPrim;

static void generateTorus() {
  for (int i = 0; i < TORUS_MAJOR; i++) {
    float a = 2 * PI * i / TORUS_MAJOR;
    float ca = std::cos(a), sa = std::sin(a);
    for (int j = 0; j < TORUS_MINOR; j++) {
      float b = 2 * PI * j / TORUS_MINOR;
      float cb = std::cos(b), sb = std::sin(b);
      g3::Vertex &v = torusVerts[i * TORUS_MINOR + j];
      v.position = {(TORUS_R + TORUS_r * cb) * ca, TORUS_r * sb,
                    (TORUS_R + TORUS_r * cb) * sa};
      v.normal = {cb * ca, sb, cb * sa};
      v.uv = {(float)i / TORUS_MAJOR, (float)j / TORUS_MINOR};
    }
  }
  // One strip per ring, joined with degenerate triangles
  int n = 0;
  for (int i = 0; i < TORUS_MAJOR; i++) {
    int i1 = (i + 1) % TORUS_MAJOR;
    for (int j = 0; j <= TORUS_MINOR; j++) {
      int jj = j % TORUS_MINOR;
      torusIndices[n++] = (uint16_t)(i1 * TORUS_MINOR + jj);
      torusIndices[n++] = (uint16_t)(i * TORUS_MINOR + jj);
    }
    if (i < TORUS_MAJOR - 1) {
      // Degenerate join: repeat the last index and the first index of the next
      // ring
      torusIndices[n] = torusIndices[n - 1];
      n++;
      torusIndices[n] = (uint16_t)((i1 + 1) % TORUS_MAJOR * TORUS_MINOR);
      n++;
    }
  }
  torusVb = {TORUS_MAJOR * TORUS_MINOR, torusVerts};
  torusPrim = {g3::PrimitiveType::TRIANGLE_STRIP, &torusVb, (uint16_t)n,
               torusIndices, nullptr};
}

// ---------------------------------------------------------------------------
// API

void sceneInit() {
  generateTextures();
  generateTorus();
}

void sceneBuild(g3::Renderer &r, float t, float yaw, float pitch, float dist,
                float aspect) {
  r.setPerspectiveProjection(60.0f * PI / 180.0f, aspect, 0.3f, 100.0f);
  r.setClearColor({0.04f, 0.05f, 0.11f, 1.0f});

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

  // Floor (large, so subdivide to hide the affine texture distortion)
  r.setMaterial(matFloor);
  r.putCube({0, -1.35f, 0}, {7.0f, 0.3f, 7.0f}, 4);

  // Chrome torus
  r.pushState();
  r.translate(0, 0.5f, 0);
  r.rotate(t * 0.6f, 0, 1, 0);
  r.rotate(0.9f + 0.3f * std::sin(t * 0.4f), 1, 0, 0.2f);
  r.setMaterial(matChrome);
  r.putPrimitive(torusPrim);
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
