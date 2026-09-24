// Graphics2D: images (plain, scaled and transformed, with the blend and the
// color key of the state) and 1-bit masks (bitmaps and glyphs).

#include "arch.hpp"
#include "internal.hpp"

namespace shapoco::gfx2d {

using namespace detail;

// ---------------------------------------------------------------------------
// Direct format-to-format row conversions (no detour through Color). They
// produce exactly the pixels of the Color path: all formats convert through
// RGB565 (which is lossless for their color depth), except GRAY1 targets,
// whose luminance threshold is defined on Color.

// Native pixel of format S as native RGB565 plus its 4-bit alpha (15 = opaque)
template <PixelFormat S>
static inline uint32_t nativeToRgb565(uint32_t p, uint32_t &a4) {
  a4 = 15;
  if constexpr (S == PixelFormat::GRAY1) {
    return p ? 0xFFFFu : 0u;
  } else if constexpr (S == PixelFormat::RGB444) {
    return rgb444ToRgb565((uint16_t)p);
  } else if constexpr (S == PixelFormat::ARGB4444) {
    a4 = p >> 12;
    return rgb444ToRgb565((uint16_t)(p & 0x0FFFu));
  } else {
    return p;
  }
}

// Convert a native pixel of S to an opaque native pixel of D
template <PixelFormat S, PixelFormat D>
static inline uint32_t convertPixel(uint32_t p) {
  if constexpr (S == D) {
    if constexpr (D == PixelFormat::ARGB4444) return p | 0xF000u;
    return p;
  } else if constexpr (D == PixelFormat::GRAY1) {
    return FormatTraits<D>::fromColor(FormatTraits<S>::toColor(p));
  } else {
    uint32_t a4;
    return FormatTraits<D>::fromRgb565(nativeToRgb565<S>(p, a4));
  }
}

// Copy n pixels of S at (sl, sx) to D at (dl, dx), converting the format
// (kept out of line: inlined into the dispatch, the loops come out longer)
template <PixelFormat S, PixelFormat D>
__attribute__((noinline)) static void copyRowT(uint8_t *dl, int dx, const uint8_t *sl, int sx, int n) {
  typename FormatTraits<S>::Cursor src;
  typename FormatTraits<D>::Cursor dst;
  src.init((void *)sl, sx);
  dst.init(dl, dx);
  for (int i = 0; i < n; i++) {
    dst.write(convertPixel<S, D>(src.read()));
    src.next();
    dst.next();
  }
}

// Blend n pixels of S over D with (source alpha x opacity64)
template <PixelFormat S, PixelFormat D>
static void blendRowT(uint8_t *dl, int dx, const uint8_t *sl, int sx, int n,
                      uint32_t opacity64) {
  typename FormatTraits<S>::Cursor src;
  typename FormatTraits<D>::Cursor dst;
  src.init((void *)sl, sx);
  dst.init(dl, dx);
  if constexpr (S == PixelFormat::ARGB4444) {
    // Opacity of each 4-bit alpha, as the Color path computes it
    uint32_t alpha[16];
    for (uint32_t a4 = 0; a4 < 16; a4++)
      alpha[a4] = (alpha255To64(a4 * 17u) * opacity64) >> 6;
    for (int i = 0; i < n; i++) {
      const uint32_t p = src.read();
      const uint32_t a = alpha[p >> 12];
      if (a != 0) {
        dst.write(blendNative<D>(dst.read(), convertPixel<S, D>(p), a));
      }
      src.next();
      dst.next();
    }
  } else {
    for (int i = 0; i < n; i++) {
      dst.write(blendNative<D>(dst.read(), convertPixel<S, D>(src.read()),
                               opacity64));
      src.next();
      dst.next();
    }
  }
}

static void copyRowFmt(PixelFormat dstFmt, PixelFormat srcFmt, uint8_t *dl,
                       int dx, const uint8_t *sl, int sx, int n) {
  withFormat(dstFmt, [&](auto dtag) {
    withFormat(srcFmt, [&](auto stag) {
      copyRowT<decltype(stag)::value, decltype(dtag)::value>(dl, dx, sl, sx,
                                                             n);
    });
  });
}

// Blending is specialized for ARGB4444 sources (sprites) and same-format
// sources with an opacity; other combinations return false and go through
// Color.
static bool blendRowFmt(PixelFormat dstFmt, PixelFormat srcFmt, uint8_t *dl,
                        int dx, const uint8_t *sl, int sx, int n,
                        uint32_t opacity64) {
  bool done = false;
  withFormat(dstFmt, [&](auto tag) {
    constexpr PixelFormat D = decltype(tag)::value;
#if SHAPOGFX_FORMAT_ARGB4444
    if (srcFmt == PixelFormat::ARGB4444) {
      blendRowT<PixelFormat::ARGB4444, D>(dl, dx, sl, sx, n, opacity64);
      done = true;
      return;
    }
#endif
    if (srcFmt == D) {
      blendRowT<D, D>(dl, dx, sl, sx, n, opacity64);
      done = true;
    }
  });
  return done;
}

// ---------------------------------------------------------------------------
// Scaled and transformed images
//
// A row is drawn by a walker, which fetches the source pixels in target order,
// and an op, which writes them. The walkers: runs of target pixels showing
// the same source pixel (magnification), one source pixel per target pixel
// (minification), the affine walk and its RP2 interpolator version. The ops:
// a plain copy within a format (with or without a color key), alpha blending
// of ARGB4444 (sprites) and of a format onto itself, and for everything else
// the Color conversion of the plain drawImage(). The format switch happens
// once per row.

namespace {

// Pixel x of a row as stored: the uint16_t as it is in memory for the 16-bit
// formats (a copy needs no swap), the native pixel for the others
template <PixelFormat S>
inline uint32_t loadRaw(const uint8_t *line, int x) {
  if constexpr (S == PixelFormat::GRAY1) {
    return (line[x >> 3] >> (7 - (x & 7))) & 1u;
  } else if constexpr (S == PixelFormat::RGB444) {
    const uint8_t *p = line + (x >> 1) * 3;
    if (x & 1) return ((uint32_t)(p[1] & 0x0Fu) << 8) | p[2];
    return ((uint32_t)p[0] << 4) | (p[1] >> 4);
  } else {
    return ((const uint16_t *)line)[x];
  }
}

template <PixelFormat S>
inline uint32_t rawToNative(uint32_t raw) {
  if constexpr (S == PixelFormat::RGB565_SWAPPED) {
    return bswap16((uint16_t)raw);
  } else {
    return raw;
  }
}

// Ops: put() writes one target pixel, run() n pixels of the same source
// pixel. SRC is the source format they read.

// Same 16-bit format: the stored values unchanged (the ARGB4444 alpha too)
struct OpCopy16 {
  static constexpr PixelFormat SRC = PixelFormat::RGB565;  // any 16-bit one
  uint16_t *p;
  void put(uint32_t raw) { *p++ = (uint16_t)raw; }
  void run(uint32_t raw, int n) {
    fill16(p, n, (uint16_t)raw);
    p += n;
  }
};

// Same format, GRAY1 or RGB444
template <PixelFormat D>
struct OpCopy {
  static constexpr PixelFormat SRC = D;
  typename FormatTraits<D>::Cursor cur;
  void put(uint32_t raw) {
    cur.write(raw);
    cur.next();
  }
  void run(uint32_t raw, int n) { cur.fill(n, raw); }
};

// The same with a color key (the key as stored)
struct OpCopyKey16 {
  static constexpr PixelFormat SRC = PixelFormat::RGB565;
  uint16_t *p;
  uint32_t key;
  void put(uint32_t raw) {
    if (raw != key) *p = (uint16_t)raw;
    p++;
  }
  void run(uint32_t raw, int n) {
    if (raw != key) fill16(p, n, (uint16_t)raw);
    p += n;
  }
};

template <PixelFormat D>
struct OpCopyKey {
  static constexpr PixelFormat SRC = D;
  typename FormatTraits<D>::Cursor cur;
  uint32_t key;
  void put(uint32_t raw) {
    if (raw != key) cur.write(raw);
    cur.next();
  }
  void run(uint32_t raw, int n) {
    if (raw == key)
      cur.skip(n);
    else
      cur.fill(n, raw);
  }
};

#if SHAPOGFX_FORMAT_ARGB4444
// ARGB4444 blended with its alpha x opacity; opaque runs become fills
template <PixelFormat D>
struct OpBlendArgb {
  static constexpr PixelFormat SRC = PixelFormat::ARGB4444;
  typename FormatTraits<D>::Cursor cur;
  const uint32_t *alpha;  // weight (0..64) of each 4-bit alpha
  void put(uint32_t raw) {
    const uint32_t a = alpha[raw >> 12];
    if (a != 0) {
      const uint32_t s = convertPixel<SRC, D>(raw);
      cur.write(a >= 64 ? s : blendNative<D>(cur.read(), s, a));
    }
    cur.next();
  }
  void run(uint32_t raw, int n) {
    const uint32_t a = alpha[raw >> 12];
    if (a == 0) {
      cur.skip(n);
      return;
    }
    const uint32_t s = convertPixel<SRC, D>(raw);
    if (a >= 64) {
      cur.fill(n, s);
      return;
    }
    for (; n > 0; n--) {
      cur.write(blendNative<D>(cur.read(), s, a));
      cur.next();
    }
  }
};
#endif

// A format without alpha onto itself with an opacity
template <PixelFormat D>
struct OpBlendSame {
  static constexpr PixelFormat SRC = D;
  typename FormatTraits<D>::Cursor cur;
  uint32_t a;
  void put(uint32_t raw) {
    cur.write(blendNative<D>(cur.read(), rawToNative<D>(raw), a));
    cur.next();
  }
  void run(uint32_t raw, int n) {
    const uint32_t s = rawToNative<D>(raw);
    for (; n > 0; n--) {
      cur.write(blendNative<D>(cur.read(), s, a));
      cur.next();
    }
  }
};

// Anything else: into Colors, written by writeColorsFmt(). A keyed pixel
// becomes the Color 0, which a keyed copy skips (and an opaque pixel of a
// copy is made opaque, so that it cannot be 0) and a blend draws with alpha 0.
template <PixelFormat S>
struct OpColor {
  static constexpr PixelFormat SRC = S;
  Color *out;
  uint32_t key;  // as stored; beyond 0xFFFF without a key
  Color opaque;  // ORed into the Colors of a keyed copy
  Color convert(uint32_t raw) const {
    return raw == key ? 0u
                      : (FormatTraits<S>::toColor(rawToNative<S>(raw)) | opaque);
  }
  void put(uint32_t raw) { *out++ = convert(raw); }
  void run(uint32_t raw, int n) {
    const Color c = convert(raw);
    for (; n > 0; n--) *out++ = c;
  }
};

enum class BlitPath : uint8_t {
  COPY16,
  COPY,
  COPY_KEY16,
  COPY_KEY,
  BLEND_ARGB,
  BLEND_SAME,
  COLOR
};

// What a drawImage() does per pixel
struct ImageBlit {
  PixelFormat dst, src;
  BlendMode mode;
  BlitPath path;
  WriteMode write;  // COLOR
  bool keyed;
  uint32_t key;  // as stored, or NO_KEY
  uint32_t op64;
  uint32_t alpha[16];  // BLEND_ARGB

  static constexpr uint32_t NO_KEY = 0xFFFFFFFFu;

  // false if nothing is drawn
  bool init(const Graphics2D &g, PixelFormat s) {
    dst = g.format(), src = s;
    mode = BLEND ? g.blendMode() : BlendMode::ALPHA;
    op64 = alpha255To64((uint32_t)(BLEND ? g.opacity() : 255));
    if (mode != BlendMode::NONE && op64 == 0) return false;
    keyed = COLOR_KEY && g.hasColorKey();
    key = NO_KEY;
    if (keyed) {
      const uint32_t native = colorToNative(s, g.colorKey());
      key = s == PixelFormat::RGB565_SWAPPED ? bswap16((uint16_t)native)
                                             : native;
    }
    const bool srcAlpha = (s == PixelFormat::ARGB4444);
    // A format without alpha drawn with ALPHA at full opacity is a copy
    if (mode == BlendMode::ALPHA && !srcAlpha && op64 >= 64)
      mode = BlendMode::NONE;
    write = mode == BlendMode::NONE
                ? (keyed ? WriteMode::COPY_KEYED : WriteMode::COPY)
                : (mode == BlendMode::ADD ? WriteMode::ADD : WriteMode::ALPHA);
    const bool is16 = bitsPerPixel(dst) == 16;
    if (mode == BlendMode::NONE && s == dst) {
      path = keyed ? (is16 ? BlitPath::COPY_KEY16 : BlitPath::COPY_KEY)
                   : (is16 ? BlitPath::COPY16 : BlitPath::COPY);
    } else if (!keyed && mode == BlendMode::ALPHA && srcAlpha) {
      path = BlitPath::BLEND_ARGB;
      for (uint32_t a4 = 0; a4 < 16; a4++)
        alpha[a4] = (alpha255To64(a4 * 17u) * op64) >> 6;
    } else if (!keyed && mode == BlendMode::ALPHA && s == dst) {
      path = BlitPath::BLEND_SAME;
    } else {
      path = BlitPath::COLOR;
    }
    return true;
  }

  // The ops that write runs at once (the others gain little from runs)
  bool takesRuns() const {
    return path == BlitPath::COPY16 || path == BlitPath::COPY ||
           path == BlitPath::COPY_KEY16 || path == BlitPath::COPY_KEY ||
           path == BlitPath::BLEND_ARGB;
  }
};

constexpr int IMAGE_CHUNK = 64;

// Draw n pixels of the target row `dl` from x, fetched by `walk`. A walker
// with SRC16_ONLY reads 16-bit sources only (the caller guarantees one); one
// with RUNS takes only the paths of takesRuns() (the caller walks the others
// pixel by pixel instead).
template <typename Walk>
__attribute__((noinline)) void blitRow(const ImageBlit &b, uint8_t *dl, int x,
                                       int n, Walk &walk) {
  switch (b.path) {
    case BlitPath::COPY16: {
      OpCopy16 op{(uint16_t *)dl + x};
      walk(op, n);
      break;
    }
    case BlitPath::COPY_KEY16:
      if constexpr (COLOR_KEY) {
        OpCopyKey16 op{(uint16_t *)dl + x, b.key};
        walk(op, n);
      }
      break;
    case BlitPath::COPY:
    case BlitPath::COPY_KEY:
      if constexpr (!Walk::SRC16_ONLY) {
        withFormat(b.dst, [&](auto tag) {
          constexpr PixelFormat D = decltype(tag)::value;
          if constexpr (bitsPerPixel(D) != 16) {
            if (COLOR_KEY && b.path == BlitPath::COPY_KEY) {
              OpCopyKey<D> op;
              op.cur.init(dl, x);
              op.key = b.key;
              walk(op, n);
            } else {
              OpCopy<D> op;
              op.cur.init(dl, x);
              walk(op, n);
            }
          }
        });
      }
      break;
    case BlitPath::BLEND_ARGB:
#if SHAPOGFX_FORMAT_ARGB4444
      withFormat(b.dst, [&](auto tag) {
        OpBlendArgb<decltype(tag)::value> op;
        op.cur.init(dl, x);
        op.alpha = b.alpha;
        walk(op, n);
      });
#endif
      break;
    case BlitPath::BLEND_SAME:
      if constexpr (!Walk::RUNS) {
        withFormat(b.dst, [&](auto tag) {
          constexpr PixelFormat D = decltype(tag)::value;
          if constexpr (D != PixelFormat::ARGB4444 &&
                        (!Walk::SRC16_ONLY || bitsPerPixel(D) == 16)) {
            OpBlendSame<D> op;
            op.cur.init(dl, x);
            op.a = b.op64;
            walk(op, n);
          }
        });
      }
      break;
    case BlitPath::COLOR:
      if constexpr (!Walk::RUNS) {
        withFormat(b.src, [&](auto tag) {
          constexpr PixelFormat S = decltype(tag)::value;
          if constexpr (!Walk::SRC16_ONLY || bitsPerPixel(S) == 16) {
            Color tmp[IMAGE_CHUNK];
            const Color opaque =
                b.write == WriteMode::COPY_KEYED ? 0xFF000000u : 0u;
            for (int i = 0; i < n; i += IMAGE_CHUNK) {
              const int k = std::min(IMAGE_CHUNK, n - i);
              OpColor<S> op{tmp, b.key, opaque};
              walk(op, k);
              writeColorsFmt(b.dst, dl, x + i, k, tmp, b.write, b.op64);
            }
          }
        });
      }
      break;
  }
}

// Walkers. They keep their position between calls, so that the Color path
// can take a row in chunks.

// Minification: one source pixel per target pixel, pos advancing by step,
// plus dir whenever the remainder reaches den
struct StepWalk {
  static constexpr bool SRC16_ONLY = false, RUNS = false;
  const uint8_t *line;
  int pos, step, dir;
  uint32_t rem, rStep, den;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) {
      op.put(loadRaw<Op::SRC>(line, pos));
      pos += step;
      rem += rStep;
      if (rem >= den) {
        rem -= den;
        pos += dir;
      }
    }
  }
};

// Magnification: runs of q or q + 1 target pixels per source pixel
// (Bresenham's run-slice), `left` pixels left of the current one
struct RunWalk {
  static constexpr bool SRC16_ONLY = false, RUNS = true;
  const uint8_t *line;
  int pos, dir, left;
  uint32_t e, q, r, den;
  template <typename Op>
  void operator()(Op &op, int n) {
    while (n > 0) {
      if (left == 0) {
        pos += dir;
        left = (int)q;
        if (r > e) {
          left++;
          e += den - r;
        } else {
          e -= r;
        }
      }
      const int k = left < n ? left : n;
      op.run(loadRaw<Op::SRC>(line, pos), k);
      left -= k;
      n -= k;
    }
  }
};

// Affine: 16.16 source coordinates, inside the image for every pixel
struct AffineWalk {
  static constexpr bool SRC16_ONLY = false, RUNS = false;
  const uint8_t *pixels;
  uint32_t stride;
  int32_t u, v, du, dv;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) {
      op.put(loadRaw<Op::SRC>(pixels + (uint32_t)(v >> 16) * stride, u >> 16));
      u += du;
      v += dv;
    }
  }
};

// The same over a mask, addressed in bits
struct AffineBitWalk {
  const uint8_t *bits;
  uint32_t base, stride;
  int32_t u, v, du, dv;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) {
      op.put(loadRaw<PixelFormat::GRAY1>(
          bits, (int)(base + (uint32_t)(v >> 16) * stride + (uint32_t)(u >> 16))));
      u += du;
      v += dv;
    }
  }
};

#if SHAPOGFX2D_RP2_INTERP
struct InterpWalk {
  static constexpr bool SRC16_ONLY = true, RUNS = false;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) op.put(arch::rp2::InterpAffine::fetch());
  }
};
#endif

// One axis of a scaled image: target pixel t of dn takes source pixel
// k(t) = floor((2t + 1) sn / 2dn) of sn (the one under its center), counted
// from the far end when mirrored. Exact, in 32 bits: sn, dn <= 32767.
struct ScaleAxis {
  int start, count;  // visible target pixels
  int pos, dir;      // source pixel of the first one, +1 or -1
  // per target pixel: pos += step, and dir more when rem reaches den
  int step;
  uint32_t rem, rStep, den;
  // magnification (sn < dn): the first run is run0 long
  bool runs;
  int run0;
  uint32_t e, q, r, rden;
};

constexpr int SCALE_MAX = 32767;

// Map dn target pixels at dstPos onto sn source pixels at srcPos, keeping the
// target pixels inside [clip0, clip1) whose source pixel lies in [0, size)
bool mapAxis(int srcPos, int sn, int size, int dstPos, int dn, int clip0,
             int clip1, bool mirror, ScaleAxis &m) {
  // Beyond this, the source or target range is out of reach anyway
  srcPos = clampInt(-(1 << 20), 1 << 20, srcPos);
  dstPos = clampInt(-(1 << 20), 1 << 20, dstPos);
  int kLo, kHi;  // usable values of k
  if (!mirror) {
    kLo = std::max(0, -srcPos);
    kHi = std::min(sn, size - srcPos) - 1;
  } else {
    kLo = std::max(0, srcPos + sn - size);
    kHi = std::min(sn, srcPos + sn) - 1;
  }
  if (kLo > kHi) return false;
  // First target pixel whose k is k or more: 2k dn <= (2t + 1) sn
  auto firstOf = [&](int k) -> int {
    const int a = 2 * k * dn - sn;  // < 2^31
    return a <= 0 ? 0 : (int)(((uint32_t)a + 2u * sn - 1u) / (2u * sn));
  };
  const int t0 = std::max(firstOf(kLo), clip0 - dstPos);
  const int t1 = std::min(firstOf(kHi + 1), clip1 - dstPos);
  if (t0 >= t1) return false;
  m.start = dstPos + t0;
  m.count = t1 - t0;
  m.dir = mirror ? -1 : 1;
  const uint32_t n0 = (2u * t0 + 1u) * (uint32_t)sn;
  m.den = 2u * dn;
  const int k0 = (int)(n0 / m.den);
  m.rem = n0 % m.den;
  m.pos = (mirror ? srcPos + sn - 1 : srcPos) + m.dir * k0;
  m.step = m.dir * (sn / dn);
  m.rStep = 2u * (uint32_t)(sn % dn);
  m.runs = sn < dn;
  if (m.runs) {
    // Run k starts at s(k) = ceil(a(k) / 2sn), a(k) = 2k dn - sn; e(k) =
    // s(k) 2sn - a(k) decides whether run k is q or q + 1 long
    const uint32_t a = 2u * (uint32_t)(k0 + 1) * dn - sn;
    const uint32_t s1 = (a + 2u * sn - 1u) / (2u * sn);
    m.run0 = (int)s1 - t0;
    m.e = s1 * 2u * sn - a;
    m.q = (uint32_t)(dn / sn);
    m.r = 2u * (uint32_t)(dn % sn);
    m.rden = 2u * sn;
  }
  return true;
}

// Both axes of a scaled image or mask: `dst` in target pixels (a negative
// size mirrors), `s` the source rectangle, `size` the source's extent
bool mapAxes(const Rect &dst, const Rect &s, int width, int height,
             const Rect &clip, ScaleAxis &ax, ScaleAxis &ay) {
  const Rect d = dst.normalized();
  if (d.isEmpty() || s.isEmpty() || d.width > SCALE_MAX ||
      d.height > SCALE_MAX || s.width > SCALE_MAX || s.height > SCALE_MAX)
    return false;
  return mapAxis(s.x, s.width, width, d.x, d.width, clip.x, clip.right(),
                 dst.width < 0, ax) &&
         mapAxis(s.y, s.height, height, d.y, d.height, clip.y, clip.bottom(),
                 dst.height < 0, ay);
}

// Narrow [k0, k1] to the k with lo <= c + k d <= hi. Callers keep lo - c
// and hi - c within 32 bits.
bool narrowSpan(int32_t c, int32_t d, int32_t lo, int32_t hi, int &k0,
                int &k1) {
  const int32_t p = lo - c, q = hi - c;
  if (d > 0) {
    k0 = std::max(k0, ceilDiv(p, d));
    k1 = std::min(k1, floorDiv(q, d));
  } else if (d < 0) {
    k0 = std::max(k0, ceilDiv(q, d));
    k1 = std::min(k1, floorDiv(p, d));
  } else if (p > 0 || q < 0) {
    return false;
  }
  return k0 <= k1;
}

// Float estimate of the columns x where c + g x lies in [lo, hi), a pixel
// wider on either side (ig = 1 / g)
bool estimateSpan(float c, float g, float ig, float lo, float hi, float &x0,
                  float &x1) {
  if (g == 0.0f) return c > lo - 1.0f && c < hi + 1.0f;
  float t0 = (lo - c) * ig, t1 = (hi - c) * ig;
  if (g < 0.0f) std::swap(t0, t1);
  x0 = std::max(x0, t0 - 1.0f);
  x1 = std::min(x1, t1 + 1.0f);
  return x0 <= x1;
}

// Limits of the affine walk: 16.16 source coordinates of an image up to
// 16384 pixels and steps of up to 4096 pixels (one pixel of the target
// spanning 4096 of the source) stay within 31 bits, and so do the offsets
// narrowSpan() divides.
constexpr int AFFINE_SIZE_MAX = 16384;
constexpr float AFFINE_STEP_MAX = 4096.0f;

// The rows of a transformed image: `m` maps source coordinates relative to
// the top-left corner of `s` to the target, `in` is the part of the source
// that may be read. The inverse transform is computed once in float; per
// row a float estimate picks a reference column inside the image's
// footprint, and from there the row is clipped exactly in 16.16 fixed point,
// so that the per-pixel walk (u += du, v += dv) never leaves `in`.
struct AffineRows {
  int32_t du = 0, dv = 0;
  float A, B, C, D, E, F, iA, iB;
  int bx0, bx1, by0, by1;
  int32_t uLo, uHi, vLo, vHi;
  Rect in;

  bool init(const affine2f &m, const Rect &s, const Rect &readable,
            const Rect &clip) {
    in = readable;
    if (in.isEmpty() || in.right() > AFFINE_SIZE_MAX ||
        in.bottom() > AFFINE_SIZE_MAX || in.x < 0 || in.y < 0)
      return false;
    affine2f inv;
    if (!m.invert(inv)) return false;
    // Source coordinates of the center of target pixel (x, y), absolute:
    // u = A x + C y + E, v = B x + D y + F
    A = inv.a, B = inv.b, C = inv.c, D = inv.d;
    if (!(std::fabs(A) <= AFFINE_STEP_MAX && std::fabs(B) <= AFFINE_STEP_MAX &&
          std::fabs(C) <= AFFINE_STEP_MAX && std::fabs(D) <= AFFINE_STEP_MAX))
      return false;  // also NaN
    E = inv.tx + (float)s.x + 0.5f * (A + C);
    F = inv.ty + (float)s.y + 0.5f * (B + D);

    // Bounding box of the drawn part, a pixel wider
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (int i = 0; i < 4; i++) {
      const vec2f p = m.apply((float)(in.x - s.x + ((i & 1) ? in.width : 0)),
                              (float)(in.y - s.y + ((i & 2) ? in.height : 0)));
      if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
      x0 = i ? std::min(x0, p.x) : p.x;
      x1 = i ? std::max(x1, p.x) : p.x;
      y0 = i ? std::min(y0, p.y) : p.y;
      y1 = i ? std::max(y1, p.y) : p.y;
    }
    const float lim = (float)(1 << 20);
    bx0 = std::max(clip.x, (int)std::floor(std::max(x0, -lim)) - 1);
    bx1 = std::min(clip.right() - 1, (int)std::ceil(std::min(x1, lim)) + 1);
    by0 = std::max(clip.y, (int)std::floor(std::max(y0, -lim)) - 1);
    by1 = std::min(clip.bottom() - 1, (int)std::ceil(std::min(y1, lim)) + 1);
    if (bx0 > bx1 || by0 > by1) return false;
    du = roundToInt(A * 65536.0f);
    dv = roundToInt(B * 65536.0f);
    iA = A != 0.0f ? 1.0f / A : 0.0f;
    iB = B != 0.0f ? 1.0f / B : 0.0f;
    uLo = in.x << 16, uHi = (in.right() << 16) - 1;
    vLo = in.y << 16, vHi = (in.bottom() << 16) - 1;
    return true;
  }

  // fn(y, x, n, u, v): n pixels of row y from x, (u, v) at the first one
  template <typename Fn>
  void forEach(Fn &&fn) const {
    const float margin = AFFINE_STEP_MAX * 2.0f;
    for (int y = by0; y <= by1; y++) {
      const float uc = C * (float)y + E, vc = D * (float)y + F;
      float ex0 = (float)bx0, ex1 = (float)bx1;
      if (!estimateSpan(uc, A, iA, (float)in.x, (float)in.right(), ex0, ex1) ||
          !estimateSpan(vc, B, iB, (float)in.y, (float)in.bottom(), ex0, ex1))
        continue;
      const int xr = clampInt(bx0, bx1, (int)std::floor((ex0 + ex1) * 0.5f));
      const float ur = A * (float)xr + uc, vr = B * (float)xr + vc;
      if (!(ur > (float)in.x - margin && ur < (float)in.right() + margin &&
            vr > (float)in.y - margin && vr < (float)in.bottom() + margin))
        continue;
      const int32_t u = roundToInt(ur * 65536.0f);
      const int32_t v = roundToInt(vr * 65536.0f);
      int k0 = bx0 - xr, k1 = bx1 - xr;
      if (!narrowSpan(u, du, uLo, uHi, k0, k1) ||
          !narrowSpan(v, dv, vLo, vHi, k0, k1))
        continue;
      fn(y, xr + k0, k1 - k0 + 1, u + k0 * du, v + k0 * dv);
    }
  }
};

// ---------------------------------------------------------------------------
// Image paths, in target pixels

// Plain: `s` (normalized) with its top-left corner at (x, y), no color key
void blitPlain(const Graphics2D &g, const Texture &img, int x, int y,
               const Rect &s) {
  const Surface &target = g.target();
  const Rect clip = g.clipRect();
  // The part of the source inside the image, where it lands, clipped
  Rect src = s.intersect({0, 0, img.width, img.height});
  const int dx = x + (src.x - s.x), dy = y + (src.y - s.y);
  const Rect dst = Rect{dx, dy, src.width, src.height}.intersect(clip);
  if (dst.isEmpty()) return;
  src.x += dst.x - dx;
  src.y += dst.y - dy;
  ImageBlit b;
  if (!b.init(g, img.format)) return;

  // Fast path: plain copy of 16-bit formats
  if (b.path == BlitPath::COPY16) {
    for (int j = 0; j < dst.height; j++) {
      std::memcpy(target.linePtr(dst.y + j) + (size_t)dst.x * 2,
                  img.linePtr(src.y + j) + (size_t)src.x * 2,
                  (size_t)dst.width * 2);
    }
    return;
  }
  if (b.mode == BlendMode::NONE) {
    for (int j = 0; j < dst.height; j++) {
      copyRowFmt(target.format, img.format, target.linePtr(dst.y + j), dst.x,
                 img.linePtr(src.y + j), src.x, dst.width);
    }
    return;
  }
  if (b.mode == BlendMode::ALPHA &&
      blendRowFmt(target.format, img.format, target.linePtr(dst.y), dst.x,
                  img.linePtr(src.y), src.x, dst.width, b.op64)) {
    for (int j = 1; j < dst.height; j++) {
      blendRowFmt(target.format, img.format, target.linePtr(dst.y + j), dst.x,
                  img.linePtr(src.y + j), src.x, dst.width, b.op64);
    }
    return;
  }

  // Additive, and alpha blends of other format pairs: convert through Color
  // in chunks
  Color tmp[IMAGE_CHUNK];
  for (int j = 0; j < dst.height; j++) {
    const uint8_t *sl = img.linePtr(src.y + j);
    uint8_t *dl = target.linePtr(dst.y + j);
    for (int i = 0; i < dst.width; i += IMAGE_CHUNK) {
      const int n = std::min(IMAGE_CHUNK, dst.width - i);
      readColorsFmt(img.format, sl, src.x + i, n, tmp);
      writeColorsFmt(target.format, dl, dst.x + i, n, tmp, b.write, b.op64);
    }
  }
}

// Scaled: `s` (normalized) stretched over `dst` (a negative size mirrors).
// Destination pixel t of dw shows source pixel floor((2t + 1) sw / 2dw).
// Per axis the call finds the visible range and the walker state with a few
// divisions; rows then step a DDA. Horizontally a reduction steps the same
// way per destination pixel; an enlargement walks the source pixels
// instead, each covering a run of q or q + 1 destination pixels, so that
// copies and ARGB4444 sprites write runs with fill() and convert every
// source pixel once.
void blitScaled(const Graphics2D &g, const Texture &img, const Rect &dst,
                const Rect &s) {
  ImageBlit b;
  if (!b.init(g, img.format)) return;
  if (dst.width == s.width && dst.height == s.height && !b.keyed) {
    blitPlain(g, img, dst.x, dst.y, s);
    return;
  }
  ScaleAxis ax, ay;
  if (!mapAxes(dst, s, img.width, img.height, g.clipRect(), ax, ay)) return;
  const Surface &target = g.target();

  const bool runs = ax.runs && b.takesRuns();
  // A plain copy repeats the previous target row for the same source row
  const bool repeat = b.write == WriteMode::COPY &&
                      bitsPerPixel(target.format) == 16;
  const size_t xOff = (size_t)ax.start * 2, bytes = (size_t)ax.count * 2;
  int row = ay.pos, prevRow = -1;
  uint32_t rem = ay.rem;
  const uint8_t *prevLine = nullptr;
  for (int j = 0; j < ay.count; j++) {
    uint8_t *dl = target.linePtr(ay.start + j);
    if (repeat && row == prevRow) {
      std::memcpy(dl + xOff, prevLine + xOff, bytes);
    } else {
      const uint8_t *sl = img.linePtr(row);
      if (runs) {
        RunWalk w{sl, ax.pos, ax.dir, ax.run0, ax.e, ax.q, ax.r, ax.rden};
        blitRow(b, dl, ax.start, ax.count, w);
      } else {
        StepWalk w{sl, ax.pos, ax.step, ax.dir, ax.rem, ax.rStep, ax.den};
        blitRow(b, dl, ax.start, ax.count, w);
      }
    }
    prevRow = row;
    prevLine = dl;
    row += ay.step;
    rem += ay.rStep;
    if (rem >= ay.den) {
      rem -= ay.den;
      row += ay.dir;
    }
  }
}

// Transformed: `m` maps coordinates relative to the top-left corner of `s`
// (normalized) to the target
void blitAffine(const Graphics2D &g, const Texture &img, const affine2f &m,
                const Rect &s) {
  AffineRows ar;
  if (!ar.init(m, s, s.intersect(Rect{0, 0, img.width, img.height}),
               g.clipRect()))
    return;
  ImageBlit b;
  if (!b.init(g, img.format)) return;
  const Surface &target = g.target();
#if SHAPOGFX2D_RP2_INTERP
  arch::rp2::InterpAffine interp;
  const bool useInterp = arch::rp2::InterpAffine::usable(img);
  if (useInterp) interp.begin(img, ar.du, ar.dv);
#endif
  ar.forEach([&](int y, int x, int n, int32_t u, int32_t v) {
    uint8_t *dl = target.linePtr(y);
#if SHAPOGFX2D_RP2_INTERP
    if (useInterp) {
      interp.row(u, v);
      InterpWalk w;
      blitRow(b, dl, x, n, w);
      return;
    }
#endif
    AffineWalk w{(const uint8_t *)img.pixels, img.stride, u, v, ar.du, ar.dv};
    blitRow(b, dl, x, n, w);
  });
#if SHAPOGFX2D_RP2_INTERP
  if (useInterp) interp.end();
#endif
}

}  // namespace

void Graphics2D::drawImage(const Texture &img, int dx, int dy,
                           const Rect &srcRect) {
  if (!hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  const Rect s = srcRect.normalized();
  if (s.isEmpty()) return;
  if (!TRANSFORM || kind_ <= TransformKind::TRANSLATE) {
    if (COLOR_KEY && hasColorKey())
      blitScaled(*this, img, Rect{dx + ox_, dy + oy_, s.width, s.height}, s);
    else
      blitPlain(*this, img, dx + ox_, dy + oy_, s);
  } else if (kind_ == TransformKind::SCALE) {
    blitScaled(*this, img,
               G2Impl::mapRectSigned(*this, RectF{(float)dx, (float)dy,
                                                  (float)s.width,
                                                  (float)s.height}),
               s);
  } else {
    blitAffine(*this, img,
               state_.transform * affine2f::translation((float)dx, (float)dy),
               s);
  }
}

void Graphics2D::drawImage(const Texture &img, const Rect &dst,
                           const Rect &srcRect) {
  if (!hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  const Rect s = srcRect.normalized();
  if (s.isEmpty() || dst.width == 0 || dst.height == 0) return;
  if (!TRANSFORM || kind_ <= TransformKind::TRANSLATE) {
    blitScaled(*this, img, dst.offset(ox_, oy_), s);
  } else if (kind_ == TransformKind::SCALE) {
    blitScaled(*this, img, G2Impl::mapRectSigned(*this, RectF(dst)), s);
  } else {
    affine2f m = state_.transform;
    m.translate((float)dst.x, (float)dst.y)
        .scale((float)dst.width / (float)s.width,
               (float)dst.height / (float)s.height);
    blitAffine(*this, img, m, s);
  }
}

// ---------------------------------------------------------------------------
// Masks: bitmaps and glyphs. Set bits are painted with fg, clear ones with bg
// (either may be absent); runs of equal bits become spans.

namespace {

// Where the runs of a row go
struct MaskSink {
  const Raster *ras;
  const Paint *fg, *bg;
  int y;
  void operator()(int x0, int x1, uint32_t bit) const {
    const Paint *p = bit ? fg : bg;
    if (p) ras->spanRaw(y, x0, x1, *p);
  }
};

// An op for the image walkers that collects runs of equal bits
struct OpMask {
  static constexpr PixelFormat SRC = PixelFormat::GRAY1;
  const MaskSink *sink;
  int x, runX;
  uint32_t runBit;
  OpMask(const MaskSink *sink, int x)
      : sink(sink), x(x), runX(x), runBit(2) {}
  void put(uint32_t b) {
    if (b != runBit) {
      flush();
      runBit = b;
    }
    x++;
  }
  void run(uint32_t b, int n) {
    if (b != runBit) {
      flush();
      runBit = b;
    }
    x += n;
  }
  void flush() {
    if (x > runX) (*sink)(runX, x, runBit);
    runX = x;
  }
};

inline bool maskBit(const MaskSource &m, uint32_t i) {
  return (m.bits[i >> 3] >> (7u - (i & 7u))) & 1u;
}

// Set bits only (text): written pixel by pixel
template <PixelFormat F>
void maskPixelsT(const Surface &target, const MaskSource &m, int sx, int sy,
                 const Rect &dst, const Paint &p) {
  for (int j = 0; j < dst.height; j++) {
    typename FormatTraits<F>::Cursor cur;
    cur.init(target.linePtr(dst.y + j), dst.x);
    uint32_t bi = m.base + (uint32_t)(sy + j) * m.stride + (uint32_t)sx;
    if (p.op == PaintOp::FILL) {
      for (int i = 0; i < dst.width; i++, bi++) {
        if (maskBit(m, bi)) cur.write(p.native);
        cur.next();
      }
      continue;
    }
    for (int i = 0; i < dst.width; i++, bi++) {
      if (maskBit(m, bi)) {
        cur.write(BLEND && p.op == PaintOp::ADD
                      ? addNative<F>(cur.read(), p.native)
                      : blendNative<F>(cur.read(), p.native, p.alpha64));
      }
      cur.next();
    }
  }
}

void maskPlain(const Raster &ras, const MaskSource &m, const Rect &src, int x,
               int y, const Paint *fg, const Paint *bg) {
  const Rect dst = Rect{x, y, src.width, src.height}.intersect(ras.clip);
  if (dst.isEmpty()) return;
  const int sx = src.x + dst.x - x, sy = src.y + dst.y - y;
  if (fg && !bg) {
    withFormat(ras.target.format, [&](auto tag) {
      maskPixelsT<decltype(tag)::value>(ras.target, m, sx, sy, dst, *fg);
    });
    return;
  }
  for (int j = 0; j < dst.height; j++) {
    const MaskSink sink = {&ras, fg, bg, dst.y + j};
    uint32_t bi = m.base + (uint32_t)(sy + j) * m.stride + (uint32_t)sx;
    int runStart = 0;
    uint32_t runBit = maskBit(m, bi);
    for (int i = 1; i <= dst.width; i++) {
      const uint32_t bit = i < dst.width ? maskBit(m, bi + (uint32_t)i) : 2u;
      if (bit == runBit) continue;
      sink(dst.x + runStart, dst.x + i, runBit);
      runStart = i;
      runBit = bit;
    }
  }
}

void maskScaled(const Raster &ras, const MaskSource &m, const Rect &src,
                const Rect &dst, const Paint *fg, const Paint *bg) {
  ScaleAxis ax, ay;
  if (!mapAxes(dst, src, src.right(), src.bottom(), ras.clip, ax, ay)) return;
  int row = ay.pos;
  uint32_t rem = ay.rem;
  for (int j = 0; j < ay.count; j++) {
    const MaskSink sink = {&ras, fg, bg, ay.start + j};
    OpMask op(&sink, ax.start);
    const int pos = (int)(m.base + (uint32_t)row * m.stride) + ax.pos;
    if (ax.runs) {
      RunWalk w{m.bits, pos, ax.dir, ax.run0, ax.e, ax.q, ax.r, ax.rden};
      w(op, ax.count);
    } else {
      StepWalk w{m.bits, pos, ax.step, ax.dir, ax.rem, ax.rStep, ax.den};
      w(op, ax.count);
    }
    op.flush();
    row += ay.step;
    rem += ay.rStep;
    if (rem >= ay.den) {
      rem -= ay.den;
      row += ay.dir;
    }
  }
}

void maskAffine(const Raster &ras, const MaskSource &m, const Rect &src,
                const affine2f &mat, const Paint *fg, const Paint *bg) {
  AffineRows ar;
  if (!ar.init(mat, src, src, ras.clip)) return;
  ar.forEach([&](int y, int x, int n, int32_t u, int32_t v) {
    const MaskSink sink = {&ras, fg, bg, y};
    OpMask op(&sink, x);
    AffineBitWalk w{m.bits, m.base, m.stride, u, v, ar.du, ar.dv};
    w(op, n);
    op.flush();
  });
}

}  // namespace

void detail::G2Impl::drawMask(Graphics2D &g, const MaskSource &m,
                              const Rect &src, int dx, int dy, const Paint *fg,
                              const Paint *bg) {
  if (src.isEmpty()) return;
  const Raster ras = raster(g);
  if (!TRANSFORM || g.kind_ <= TransformKind::TRANSLATE) {
    maskPlain(ras, m, src, dx + g.ox_, dy + g.oy_, fg, bg);
  } else if (g.kind_ == TransformKind::SCALE) {
    maskScaled(ras, m, src,
               mapRectSigned(g, RectF{(float)dx, (float)dy, (float)src.width,
                                      (float)src.height}),
               fg, bg);
  } else {
    maskAffine(ras, m, src,
               g.state_.transform * affine2f::translation((float)dx, (float)dy),
               fg, bg);
  }
}

void Graphics2D::drawBitmap(const Texture &bmp, int dx, int dy,
                            const Rect &srcRect, Color fg, Color bg) {
  if (!hasTarget() || !bmp.pixels || bmp.format != PixelFormat::GRAY1) return;
#if SHAPOGFX_FORMAT_GRAY1
  const Rect s = srcRect.normalized();
  const Rect in = s.intersect({0, 0, bmp.width, bmp.height});
  if (in.isEmpty()) return;
  Paint pf, pb;
  const bool hasFg = G2Impl::makePaint(*this, fg, pf);
  const bool hasBg = colorA(bg) != 0 && G2Impl::makePaint(*this, bg, pb);
  if (!hasFg && !hasBg) return;
  const MaskSource m = {(const uint8_t *)bmp.pixels, 0, bmp.stride * 8u};
  G2Impl::drawMask(*this, m, in, dx + (in.x - s.x), dy + (in.y - s.y),
                   hasFg ? &pf : nullptr, hasBg ? &pb : nullptr);
#else
  (void)dx, (void)dy, (void)srcRect, (void)fg, (void)bg;
#endif
}

}  // namespace shapoco::gfx2d
