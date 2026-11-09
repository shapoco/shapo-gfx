// The fixed-point arithmetic behind SHAPOGFX2D_FIXED_POINT (helpers internal
// to src/gfx2d, tested here directly against wider arithmetic) and the
// transforms beyond its range, which every build draws through float.

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../src/gfx2d/internal.hpp"
#include "check.hpp"
#include "shapoco/gfx2d/gfx2d.hpp"
#include "shapoco/gfx2d/surface_alloc.hpp"

using namespace shapoco::gfx2d;
using namespace shapoco::gfx2d::detail;

static uint64_t rand64(uint64_t &s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

static void testDivision() {
  uint64_t seed = 0x9E3779B97F4A7C15ull;
  int bad = 0;
  for (int i = 0; i < 20000; i++) {
    // Divisors of every size, quotients that fit 32 bits
    uint32_t d = (uint32_t)rand64(seed) >> (rand64(seed) % 32);
    if (d == 0) d = 1;
    uint32_t hi = (uint32_t)rand64(seed) % d, lo = (uint32_t)rand64(seed);
    if (i % 7 == 0) hi = d - 1, lo = 0xFFFFFFFFu;
    if (i % 11 == 0) lo = 0;
    const uint64_t n = ((uint64_t)hi << 32) | lo;
    bad += div64by32(hi, lo, d) != (uint32_t)(n / d);
  }
  CHECK_EQ(bad, 0);

  // 1 / v = r 2^-(62 + sh), to 30 bits
  bad = 0;
  for (int i = 0; i < 20000; i++) {
    uint64_t v = rand64(seed) >> (rand64(seed) % 64);
    if (v == 0) v = 1;
    if (i % 13 == 0) v = (uint64_t)1 << (i % 63);  // the powers of two
    int sh;
    const uint32_t r = reciprocal64(v, sh);
    const long double one =
        (long double)r * std::ldexp(1.0L, -(62 + sh)) * (long double)v;
    bad += !(r >= 0x80000000u && std::fabs((double)(one - 1.0L)) < 4e-9);
  }
  CHECK_EQ(bad, 0);
}

static void testMulShift() {
#ifdef __SIZEOF_INT128__
  uint64_t seed = 12345;
  int bad = 0;
  const __int128 sat = (__int128)1 << 62;
  for (int i = 0; i < 40000; i++) {
    int64_t a = (int64_t)(rand64(seed) >> (2 + rand64(seed) % 60));
    if (rand64(seed) & 1) a = -a;
    const uint32_t r = (uint32_t)rand64(seed) | 0x80000000u;
    const int s = (int)(rand64(seed) % 120) - 24;  // -24 .. 95
    const __int128 p = (__int128)a * r;
    __int128 want = s >= 0 ? p >> s : p * ((__int128)1 << -s);
    const int64_t got = mulShiftSat(a, r, s);
    if (want >= sat || want < -sat) {
      // Saturated on the side of the product
      bad += got != (want > 0 ? MUL_SHIFT_SAT : -MUL_SHIFT_SAT);
    } else {
      bad += got != (int64_t)want;
    }
  }
  CHECK_EQ(bad, 0);
  bad = 0;
  for (int i = 0; i < 20000; i++) {
    int64_t a = (int64_t)(rand64(seed) >> (18 + rand64(seed) % 40));
    if (rand64(seed) & 1) a = -a;
    const int32_t b = (int32_t)rand64(seed);
    bad += mulShift16(a, b) != (int64_t)(((__int128)a * b) >> 16);
  }
  CHECK_EQ(bad, 0);
#endif
}

static void testConversions() {
  uint64_t seed = 777;
  int bad = 0;
  for (int i = 0; i < 40000; i++) {
    // Floats of every magnitude around the ranges of 8.24 and 16.16
    const int frac = i & 1 ? 24 : 16;
    const float mant = (float)((int32_t)rand64(seed)) / 2147483648.0f;
    const float f = std::ldexp(mant, (int)(rand64(seed) % 50) - 34);
    int32_t q = 0x55555555;
    const bool ok = floatToFixed(f, frac, q);
    const double want = (double)f * (double)(1 << frac);
    if (std::fabs(want) < 2147483647.0) {
      // (halves away from zero)
      bad += !ok || q != (int32_t)std::llround(want);
    } else {
      bad += ok;
    }
    // 64 bits: truncated
    if (std::fabs(want) < 4e18) {
      const long double exact = (long double)f * (long double)(1 << frac);
      bad += floatToFixed64(f, frac) != (int64_t)std::trunc(exact);
    }
  }
  CHECK_EQ(bad, 0);
  int32_t q = 0;
  CHECK(floatToFixed(0.0f, 24, q) && q == 0);
  CHECK(floatToFixed(-0.0f, 24, q) && q == 0);
  CHECK(floatToFixed(1.0f, 24, q) && q == 1 << 24);
  CHECK(floatToFixed(-127.5f, 24, q) && q == -(255 << 23));
  CHECK(!floatToFixed(128.0f, 24, q));
  CHECK(!floatToFixed(NAN, 24, q));
  CHECK(!floatToFixed(INFINITY, 16, q));
  CHECK(!floatToFixed(-INFINITY, 16, q));

  // Back to float: the nearest one (a tie may go either way)
  bad = 0;
  for (int i = 0; i < 40000; i++) {
    const int frac = i & 1 ? 24 : 16;
    const int32_t v = (int32_t)rand64(seed) >> (rand64(seed) % 32);
    const float got = fixedToFloat(v, frac);
    const double want = (double)v / (double)(1 << frac);
    const float near = (float)want;
    const float next =
        std::nextafter(near, got > near ? INFINITY : -INFINITY);
    bad += !(got == near || got == next) ||
           std::fabs((double)got - want) > std::fabs(want) * 6e-8;
  }
  CHECK_EQ(bad, 0);
  CHECK(fixedToFloat(0, 24) == 0.0f);
  CHECK(fixedToFloat(1 << 24, 24) == 1.0f);
  CHECK(fixedToFloat(-(3 << 15), 16) == -1.5f);
  CHECK(fixedToFloat(INT32_MIN, 16) == -32768.0f);
  bad = 0;
  for (int i = 0; i < 20000; i++) {
    int64_t v = (int64_t)(rand64(seed) >> (2 + rand64(seed) % 62));
    if (rand64(seed) & 1) v = -v;
    const double want = (double)v / 4294967296.0;
    bad += std::fabs((double)fixed64ToFloat(v, 32) - want) >
           std::fabs(want) * 1.2e-7;
  }
  CHECK_EQ(bad, 0);
  CHECK(fixed64ToFloat(0, 32) == 0.0f);
  CHECK(fixed64ToFloat(-((int64_t)3 << 31), 32) == -1.5f);
}

static void testAffineQ() {
  uint64_t seed = 4711;
  auto unit = [&seed]() {
    return (float)((int32_t)rand64(seed)) / 2147483648.0f;
  };
  int bad = 0;
  for (int i = 0; i < 2000; i++) {
    const affine2f m = {3 * unit(), 3 * unit(), 3 * unit(), 3 * unit(),
                        900 * unit(), 900 * unit()};
    const affine2f n = {3 * unit(), 3 * unit(), 3 * unit(), 3 * unit(),
                        900 * unit(), 900 * unit()};
    AffineQ mq, nq, pq;
    if (!AffineQ::from(m, mq) || !AffineQ::from(n, nq) ||
        !AffineQ::mul(mq, nq, pq)) {
      bad++;
      continue;
    }
    const affine2f a = mq.toFloat(), p = pq.toFloat(), want = m * n;
    bad += std::fabs(a.a - m.a) > 1e-7f || std::fabs(a.tx - m.tx) > 2e-5f;
    const float got[6] = {p.a, p.b, p.c, p.d, p.tx, p.ty};
    const float ref[6] = {want.a, want.b, want.c, want.d, want.tx, want.ty};
    for (int k = 0; k < 6; k++) {
      bad += std::fabs(got[k] - ref[k]) > (k < 4 ? 2e-6f : 5e-4f);
    }
  }
  CHECK_EQ(bad, 0);
  // Out of range: not converted, and a product that does not fit is told
  AffineQ q, r;
  CHECK(!AffineQ::from(affine2f::scaling(128.0f), q));
  CHECK(!AffineQ::from(affine2f::translation(32768.0f, 0), q));
  CHECK(!AffineQ::from({1, 0, 0, NAN, 0, 0}, q));
  CHECK(AffineQ::from(affine2f::scaling(100.0f), q));
  CHECK(!AffineQ::mul(q, q, r));
  CHECK(r.a == INT32_MAX && r.b == 0);
  CHECK(AffineQ::from(affine2f::translation(-20000.0f, 20000.0f), q));
  CHECK(!AffineQ::mul(q, q, r));
  CHECK(r.tx == -INT32_MAX && r.ty == INT32_MAX);
}

#if SHAPOGFX2D_TRANSFORM
// A transformed image against the source points computed in double: every
// pixel well inside a texel shows it, every pixel well outside the image is
// left alone
static void checkImage(const OwnedSurface &img, const affine2f &m, int w,
                       int h, int minDrawn) {
  OwnedSurface t = createSurface(PixelFormat::RGB565_SWAPPED, w, h);
  Graphics2D g(t);
  const Color back = makeColor(8, 16, 8);
  g.clear(back);
  g.setTransform(m);
  g.drawImage(img, 0, 0);
  g.resetTransform();
  const Graphics2D gi(img);
  const double det = (double)m.a * m.d - (double)m.b * m.c;
  int bad = 0, drawn = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const double X = x + 0.5 - m.tx, Y = y + 0.5 - m.ty;
      const double u = (m.d * X - m.c * Y) / det;
      const double v = (-m.b * X + m.a * Y) / det;
      const double fu = u - std::floor(u), fv = v - std::floor(v);
      if (fu < 0.02 || fu > 0.98 || fv < 0.02 || fv > 0.98) continue;
      const bool in = u > 0 && u < img.width() && v > 0 && v < img.height();
      const Color want = in ? gi.getPixel((int)u, (int)v) : back;
      drawn += in;
      bad += g.getPixel(x, y) != want;
    }
  }
  CHECK_EQ(bad, 0);
  CHECK(drawn >= minDrawn);
}

static void testBeyondRange() {
  // Four texels, each some hundred pixels wide and turned: beyond the 8.24
  // of an AffineQ
  OwnedSurface four = createSurface(PixelFormat::RGB565_SWAPPED, 2, 2);
  {
    Graphics2D g(four);
    g.setPixel(0, 0, Colors::RED);
    g.setPixel(1, 0, Colors::GREEN);
    g.setPixel(0, 1, Colors::BLUE);
    g.setPixel(1, 1, Colors::WHITE);
  }
  for (float sc : {100.0f, 130.0f, 400.0f}) {
    checkImage(four, affine2f::placement(48, 40, 0.5f, sc, sc * 0.7f, 1, 1), 96,
               80, 3000);
    checkImage(four, affine2f::placement(48, 40, -2.2f, -sc, sc, 1.2f, 0.9f),
               96, 80, 3000);
  }
  // A placement far outside the target, with the image reaching into it
  OwnedSurface img = createSurface(PixelFormat::RGB565_SWAPPED, 64, 48);
  {
    Graphics2D g(img);
    uint64_t seed = 99;
    for (int y = 0; y < 48; y++) {
      for (int x = 0; x < 64; x++) {
        g.setPixel(x, y, 0xFF000000u | (Color)(rand64(seed) & 0xF8FCF8u));
      }
    }
  }
  affine2f far = affine2f::scaling(100.0f);
  far.translate(-40000.0f / 100.0f, -20.0f).rotate(0.01f);
  far.translate(400.0f - 31.5f, 0);
  checkImage(img, far, 96, 80, 1500);
  // Steps of thousands of texels per pixel, along the row and down the
  // rows: the source coordinates of the bounding box leave 16.16, and the
  // rows are found in float. The transform is given by its inverse
  // (u = 4000 x + 3900 y + ..., v = 0.02 x + 0.05 y + ...).
  OwnedSurface strip = createSurface(PixelFormat::RGB565_SWAPPED, 2000, 2);
  Graphics2D(strip).clear(Colors::WHITE);
  OwnedSurface t = createSurface(PixelFormat::RGB565_SWAPPED, 32, 32);
  Graphics2D g(t);
  int white = 0;
  for (float cv : {-0.5f, -1.1f, -1.7f}) {
    const affine2f inv = {4000.0f, 0.02f, 3900.0f, 0.05f, -126000.0f, cv};
    affine2f m = affine2f::identity();
    CHECK(inv.invert(m));
    g.resetTransform();
    g.clear(Colors::BLACK);
    g.setTransform(m);
    g.drawImage(strip, 0, 0);
    g.resetTransform();
    for (int y = 0; y < 32; y++) {
      for (int x = 0; x < 32; x++) {
        const Color c = g.getPixel(x, y);
        if (c == Colors::WHITE) {
          // Its source point is in the strip (a pixel's rounding aside)
          const float u = inv.a * (x + 0.5f) + inv.c * (y + 0.5f) + inv.tx;
          const float v = inv.b * (x + 0.5f) + inv.d * (y + 0.5f) + inv.ty;
          CHECK(u > -40.0f && u < 2040.0f && v > -0.01f && v < 2.01f);
          white++;
        } else {
          CHECK(c == Colors::BLACK);
        }
      }
    }
  }
  CHECK(white >= 3);
}
#endif

void testFixed() {
  testDivision();
  testMulShift();
  testConversions();
  testAffineQ();
#if SHAPOGFX2D_TRANSFORM
  testBeyondRange();
#endif
}
