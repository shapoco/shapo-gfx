// Basic shapes for Graphics3D.
//
// Shapes are generated on the fly: parametric surfaces are emitted as
// TRIANGLE_STRIPs one latitude band at a time, in chunks of at most CHUNK
// segments, from a small stack buffer. Trigonometric values are tabulated per
// call, so regenerating a shape every frame costs little more than drawing a
// pre-built mesh.

#include <algorithm>
#include <cmath>

#include "shapoco/gfx3d/gfx3d.hpp"

namespace shapoco::gfx3d {

namespace {

constexpr float PI = 3.14159265358979f;
constexpr int MAX_SEGMENTS = 64;
constexpr int CHUNK = 16;                     // segments per strip chunk
constexpr int STRIP_VERTS = 2 * (CHUNK + 1);  // vertices per strip chunk

// Strip indices 0, 1, 2, ... (vertices are stored in strip order)
struct SequentialIndices {
  uint16_t idx[STRIP_VERTS];
  constexpr SequentialIndices() : idx() {
    for (int i = 0; i < STRIP_VERTS; i++) idx[i] = (uint16_t)i;
  }
};
constexpr SequentialIndices SEQ;

int clampSegments(int n, int lo = 3) { return std::clamp(n, lo, MAX_SEGMENTS); }

struct SinCos {
  float s[MAX_SEGMENTS + 1], c[MAX_SEGMENTS + 1];
  // n divisions of the angle range [0, span]
  void init(int n, float span) {
    for (int i = 0; i <= n; i++) {
      float a = span * (float)i / (float)n;
      s[i] = std::sin(a);
      c[i] = std::cos(a);
    }
  }
};

// Emit a parametric surface with `rows` bands (v direction) and `cols`
// segments (u direction). vertexAt(i, j) returns the vertex of row i (0..rows),
// column j (0..cols). With flip == false the front face is the side where
// (dP/du x dP/dv) points; flip reverses it.
template <typename F>
void putParametric(Graphics3D &g, int rows, int cols, bool flip, F vertexAt) {
  Vertex buf[STRIP_VERTS];
  VertexBuffer vb = {0, buf};
  Primitive prim = {PrimitiveType::TRIANGLE_STRIP, &vb, 0, SEQ.idx, nullptr};
  for (int i = 0; i < rows; i++) {
    for (int j0 = 0; j0 < cols; j0 += CHUNK) {
      const int n = std::min(CHUNK, cols - j0);
      for (int k = 0; k <= n; k++) {
        // Pair order (row i+1, row i) gives a front face towards dP/du x dP/dv
        buf[2 * k + (flip ? 1 : 0)] = vertexAt(i + 1, j0 + k);
        buf[2 * k + (flip ? 0 : 1)] = vertexAt(i, j0 + k);
      }
      vb.vertexCount = (uint16_t)(2 * (n + 1));
      prim.indexCount = vb.vertexCount;
      g.putPrimitive(prim);
    }
  }
}

// Emit a fan around `centerVertex` through rim vertices rimAt(0..segments)
// (the rim is closed: rimAt(segments) == rimAt(0)). With flip == false the
// front face is the side from which the rim runs counter-clockwise.
template <typename F>
void putFan(Graphics3D &g, const Vertex &centerVertex, int segments, bool flip,
            F rimAt) {
  Vertex buf[CHUNK + 2];
  VertexBuffer vb = {0, buf};
  Primitive prim = {PrimitiveType::TRIANGLE_FAN, &vb, 0, SEQ.idx, nullptr};
  buf[0] = centerVertex;
  for (int j0 = 0; j0 < segments; j0 += CHUNK) {
    const int n = std::min(CHUNK, segments - j0);
    for (int k = 0; k <= n; k++)
      buf[1 + k] = rimAt(flip ? (j0 + n - k) : (j0 + k));
    vb.vertexCount = (uint16_t)(n + 2);
    prim.indexCount = vb.vertexCount;
    g.putPrimitive(prim);
  }
}

}  // namespace

// ---------------------------------------------------------------------------

void Graphics3D::putPlane(const vec3f &c, float sizeX, float sizeZ, int divsX,
                          int divsZ) {
  divsX = clampSegments(divsX, 1);
  divsZ = clampSegments(divsZ, 1);
  // u along +X, v along +Z; dP/du x dP/dv = -Y, so flip for a +Y front face
  putParametric(*this, divsZ, divsX, true, [&](int i, int j) {
    float u = (float)j / divsX, v = (float)i / divsZ;
    Vertex vt;
    vt.position = {c.x - sizeX * 0.5f + u * sizeX, c.y,
                   c.z - sizeZ * 0.5f + v * sizeZ};
    vt.normal = {0, 1, 0};
    vt.uv = {u, v};
    vt.color = VERTEX_WHITE;
    return vt;
  });
}

void Graphics3D::putDisk(const vec3f &c, float radius, int segments) {
  segments = clampSegments(segments);
  SinCos sc;
  sc.init(segments, 2 * PI);
  Vertex center = {c, {0, 1, 0}, {0.5f, 0.5f}, VERTEX_WHITE};
  // Rim runs from +X towards +Z; seen from +Y that is clockwise, so flip
  putFan(*this, center, segments, true, [&](int j) {
    Vertex vt;
    vt.position = {c.x + radius * sc.c[j], c.y, c.z + radius * sc.s[j]};
    vt.normal = {0, 1, 0};
    vt.uv = {0.5f + 0.5f * sc.c[j], 0.5f + 0.5f * sc.s[j]};
    vt.color = VERTEX_WHITE;
    return vt;
  });
}

void Graphics3D::putSphereUV(const vec3f &c, float radius, int segmentsU,
                             int segmentsV) {
  segmentsU = clampSegments(segmentsU);
  segmentsV = clampSegments(segmentsV, 2);
  SinCos lon, lat;
  lon.init(segmentsU, 2 * PI);
  lat.init(segmentsV, PI);
  // P = (sin(phi) cos(theta), cos(phi), sin(phi) sin(theta)); dP/du x dP/dv
  // points outward
  putParametric(*this, segmentsV, segmentsU, false, [&](int i, int j) {
    vec3f n = {lat.s[i] * lon.c[j], lat.c[i], lat.s[i] * lon.s[j]};
    Vertex vt;
    vt.position = c + n * radius;
    vt.normal = n;
    vt.uv = {(float)j / segmentsU, (float)i / segmentsV};
    vt.color = VERTEX_WHITE;
    return vt;
  });
}

void Graphics3D::putCone(const vec3f &c, float radiusBottom, float radiusTop,
                         float height, int segments, int heightDivs,
                         bool caps) {
  segments = clampSegments(segments);
  heightDivs = clampSegments(heightDivs, 1);
  SinCos sc;
  sc.init(segments, 2 * PI);
  const float halfH = height * 0.5f;
  // Side normal: (h cos, dR, h sin) normalized, dR = radiusBottom - radiusTop
  const float dR = radiusBottom - radiusTop;
  const float nInv = 1.0f / std::sqrt(height * height + dR * dR);
  const float nh = height * nInv, nr = dR * nInv;
  // v = 0 at the top: dP/du x dP/dv points outward
  putParametric(*this, heightDivs, segments, false, [&](int i, int j) {
    float v = (float)i / heightDivs;
    float r = radiusTop + (radiusBottom - radiusTop) * v;
    Vertex vt;
    vt.position = {c.x + r * sc.c[j], c.y + halfH - v * height,
                   c.z + r * sc.s[j]};
    vt.normal = {nh * sc.c[j], nr, nh * sc.s[j]};
    vt.uv = {(float)j / segments, v};
    vt.color = VERTEX_WHITE;
    return vt;
  });
  if (!caps) return;
  if (radiusTop > 0.0f) {
    putDisk({c.x, c.y + halfH, c.z}, radiusTop, segments);
  }
  if (radiusBottom > 0.0f) {
    // Bottom cap faces -Y: same rim, opposite winding
    Vertex center = {
        {c.x, c.y - halfH, c.z}, {0, -1, 0}, {0.5f, 0.5f}, VERTEX_WHITE};
    putFan(*this, center, segments, false, [&](int j) {
      Vertex vt;
      vt.position = {c.x + radiusBottom * sc.c[j], c.y - halfH,
                     c.z + radiusBottom * sc.s[j]};
      vt.normal = {0, -1, 0};
      vt.uv = {0.5f + 0.5f * sc.c[j], 0.5f - 0.5f * sc.s[j]};
      vt.color = VERTEX_WHITE;
      return vt;
    });
  }
}

void Graphics3D::putCylinder(const vec3f &c, float radius, float height,
                             int segments, int heightDivs, bool caps) {
  putCone(c, radius, radius, height, segments, heightDivs, caps);
}

void Graphics3D::putTorus(const vec3f &c, float majorRadius, float minorRadius,
                          int majorSegments, int minorSegments) {
  majorSegments = clampSegments(majorSegments);
  minorSegments = clampSegments(minorSegments);
  SinCos ring, tube;
  ring.init(majorSegments, 2 * PI);
  tube.init(minorSegments, 2 * PI);
  // u around the ring (theta), v around the tube (phi) with y = -sin(phi):
  // dP/du x dP/dv points outward
  putParametric(*this, minorSegments, majorSegments, false, [&](int i, int j) {
    vec3f n = {tube.c[i] * ring.c[j], -tube.s[i], tube.c[i] * ring.s[j]};
    Vertex vt;
    vt.position = {c.x + (majorRadius + minorRadius * tube.c[i]) * ring.c[j],
                   c.y - minorRadius * tube.s[i],
                   c.z + (majorRadius + minorRadius * tube.c[i]) * ring.s[j]};
    vt.normal = n;
    vt.uv = {(float)j / majorSegments, (float)i / minorSegments};
    vt.color = VERTEX_WHITE;
    return vt;
  });
}

// ---------------------------------------------------------------------------
// Icosphere

namespace {

// Icosahedron: 12 vertices, 20 counter-clockwise faces
constexpr float ICO_T = 0.85065080835f;  // phi / sqrt(1 + phi^2)
constexpr float ICO_S = 0.52573111212f;  // 1 / sqrt(1 + phi^2)
const vec3f ICO_VERTS[12] = {
    {-ICO_S, ICO_T, 0},  {ICO_S, ICO_T, 0},   {-ICO_S, -ICO_T, 0},
    {ICO_S, -ICO_T, 0},  {0, -ICO_S, ICO_T},  {0, ICO_S, ICO_T},
    {0, -ICO_S, -ICO_T}, {0, ICO_S, -ICO_T},  {ICO_T, 0, -ICO_S},
    {ICO_T, 0, ICO_S},   {-ICO_T, 0, -ICO_S}, {-ICO_T, 0, ICO_S},
};
const uint8_t ICO_FACES[20][3] = {
    {0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
    {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
    {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
    {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1},
};

// Equirectangular UV of a unit vector, matching putSphereUV: u = longitude
// from +X towards +Z, v = 0 at +Y
vec2f sphereUv(const vec3f &n) {
  float u = std::atan2(n.z, n.x) * (0.5f / PI);
  if (u < 0.0f) u += 1.0f;
  float v = std::acos(std::clamp(n.y, -1.0f, 1.0f)) * (1.0f / PI);
  return {u, v};
}

}  // namespace

void Graphics3D::putIcosphere(const vec3f &c, float radius, int level) {
  level = std::clamp(level, 0, 4);
  const int n = 1 << level;  // subdivisions per icosahedron edge

  Vertex buf[STRIP_VERTS];
  VertexBuffer vb = {0, buf};
  Primitive prim = {PrimitiveType::TRIANGLE_STRIP, &vb, 0, SEQ.idx, nullptr};

  for (int f = 0; f < 20; f++) {
    const vec3f a = ICO_VERTS[ICO_FACES[f][0]];
    const vec3f b = ICO_VERTS[ICO_FACES[f][1]];
    const vec3f cc = ICO_VERTS[ICO_FACES[f][2]];
    // Face point (i, j): i rows from a towards the edge b-c, j along the row
    auto point = [&](int i, int j) -> vec3f {
      if (i == 0) return a;
      float t = (float)i / n;
      vec3f left = lerp(a, b, t), right = lerp(a, cc, t);
      return normalize(lerp(left, right, (float)j / i));
    };
    // The face's UV seam handling: reference u of the face center
    const vec2f uvCenter = sphereUv(normalize(a + b + cc));
    auto vertexAt = [&](int i, int j) {
      vec3f nrm = point(i, j);
      vec2f uv = sphereUv(nrm);
      // Keep u continuous across the seam (texture coordinates wrap anyway)
      if (uv.x - uvCenter.x > 0.5f) uv.x -= 1.0f;
      if (uvCenter.x - uv.x > 0.5f) uv.x += 1.0f;
      // At the poles use the face center's u
      if (std::fabs(nrm.y) > 0.99999f) uv.x = uvCenter.x;
      Vertex vt;
      vt.position = c + nrm * radius;
      vt.normal = nrm;
      vt.uv = uv;
      vt.color = VERTEX_WHITE;
      return vt;
    };
    // Row band i..i+1: a strip alternating row i (i+1 points) and row i+1
    // (i+2 points): (i, 0), (i+1, 0), (i, 1), (i+1, 1), ..., (i, i), (i+1, i),
    // (i+1, i+1). Rows run from a towards the edge b-c, so this order faces
    // outward.
    for (int i = 0; i < n; i++) {
      int k = 0;
      for (int j = 0; j <= i; j++) {
        buf[k++] = vertexAt(i, j);
        buf[k++] = vertexAt(i + 1, j);
      }
      buf[k++] = vertexAt(i + 1, i + 1);
      vb.vertexCount = (uint16_t)k;
      prim.indexCount = vb.vertexCount;
      putPrimitive(prim);
    }
  }
}

}  // namespace shapoco::gfx3d

// ---------------------------------------------------------------------------
// Lines

namespace shapoco::gfx3d {

void Graphics3D::putLine(const vec3f &a, const vec3f &b) {
  const Vertex v[2] = {{a, {0, 1, 0}, {0, 0}, VERTEX_WHITE},
                       {b, {0, 1, 0}, {0, 0}, VERTEX_WHITE}};
  static const uint16_t IDX[2] = {0, 1};
  const VertexBuffer vb = {2, v};
  const Primitive prim = {PrimitiveType::LINES, &vb, 2, IDX, nullptr};
  putPrimitive(prim);
}

void Graphics3D::putWireCube(const vec3f &c, const vec3f &size) {
  const vec3f h = size * 0.5f;
  Vertex v[8];
  for (int i = 0; i < 8; i++) {
    v[i].position = {c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y),
                     c.z + ((i & 4) ? h.z : -h.z)};
    v[i].normal = {0, 1, 0};
    v[i].uv = {0, 0};
    v[i].color = VERTEX_WHITE;
  }
  // 12 edges: along x, y and z
  static const uint16_t IDX[24] = {0, 1, 2, 3, 4, 5, 6, 7,   // x
                                   0, 2, 1, 3, 4, 6, 5, 7,   // y
                                   0, 4, 1, 5, 2, 6, 3, 7};  // z
  const VertexBuffer vb = {8, v};
  const Primitive prim = {PrimitiveType::LINES, &vb, 24, IDX, nullptr};
  putPrimitive(prim);
}

}  // namespace shapoco::gfx3d
