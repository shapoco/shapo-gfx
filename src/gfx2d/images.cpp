// Graphics2D: images (plain, scaled and transformed, with the blend and the
// color key of the state), their silhouettes and 1-bit masks (bitmaps and
// glyphs).

#include "arch.hpp"
#include <climits>

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

// The opacity of an ARGB4444 texel under the opacity of the state, in
// 0..1024 (10 bits: the blend keeps the texel's precision): the alpha field
// of the native pixel times ImageBlit::alphaMul, shifted
static inline uint32_t argbAlpha1024(uint32_t native, uint32_t alphaMul) {
  return ((native & 0xF000u) * alphaMul) >> 16;
}

// Blend n pixels of S over D with (source alpha x opacity64). For an
// ARGB4444 source `alphaMul` is the factor of its alpha (ImageBlit::alphaMul,
// computed once per drawImage()) and pixels equal to `key` (as stored;
// ImageBlit::NO_KEY for none) are skipped.
template <PixelFormat S, PixelFormat D>
static void blendRowT(uint8_t *dl, int dx, const uint8_t *sl, int sx, int n,
                      uint32_t opacity64, uint32_t alphaMul, uint32_t key) {
  typename FormatTraits<S>::Cursor src;
  typename FormatTraits<D>::Cursor dst;
  src.init((void *)sl, sx);
  dst.init(dl, dx);
  if constexpr (S == PixelFormat::ARGB4444) {
    (void)opacity64;
    for (int i = 0; i < n; i++) {
      const uint32_t p = src.read();
      const uint32_t a = p == key ? 0u : argbAlpha1024(p, alphaMul);
      if (a != 0) {
        const uint32_t s = convertPixel<S, D>(p);
        // Opaque pixels are written directly (like OpBlendArgb::put)
        dst.write(a >= 1024 ? s : blendNative<D, 10>(dst.read(), s, a));
      }
      src.next();
      dst.next();
    }
  } else {
    (void)alphaMul, (void)key;
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
                        uint32_t opacity64, uint32_t alphaMul, uint32_t key) {
  bool done = false;
  withFormat(dstFmt, [&](auto tag) {
    constexpr PixelFormat D = decltype(tag)::value;
#if SHAPOGFX_FORMAT_ARGB4444
    if (srcFmt == PixelFormat::ARGB4444) {
      blendRowT<PixelFormat::ARGB4444, D>(dl, dx, sl, sx, n, opacity64,
                                          alphaMul, key);
      done = true;
      return;
    }
#endif
    if (srcFmt == D) {
      blendRowT<D, D>(dl, dx, sl, sx, n, opacity64, alphaMul, key);
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
  uint32_t alphaMul;  // ImageBlit::alphaMul
  uint32_t key;       // as stored; ImageBlit::NO_KEY for none
  void put(uint32_t raw) {
    const uint32_t a = raw == key ? 0u : argbAlpha1024(raw, alphaMul);
    if (a != 0) {
      const uint32_t s = convertPixel<SRC, D>(raw);
      cur.write(a >= 1024 ? s : blendNative<D, 10>(cur.read(), s, a));
    }
    cur.next();
  }
  void run(uint32_t raw, int n) {
    const uint32_t a = raw == key ? 0u : argbAlpha1024(raw, alphaMul);
    if (a == 0) {
      cur.skip(n);
      return;
    }
    const uint32_t s = convertPixel<SRC, D>(raw);
    if (a >= 1024) {
      cur.fill(n, s);
      return;
    }
    for (; n > 0; n--) {
      cur.write(blendNative<D, 10>(cur.read(), s, a));
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

// drawSilhouette() from a 16-bit source: the opacity level of the stored
// pixel (0..15: the alpha nibble of ARGB4444, 15 for the other formats, 0
// for the color key) picks the Paint of the color at that level, made once
// per call (ImageBlit::level); a level that draws nothing is skipped
// (ImageBlit::drawn), so the transparent pixels cost a shift and a branch
template <PixelFormat D>
struct OpSilhouette {
  static constexpr PixelFormat SRC = PixelFormat::RGB565;  // any 16-bit one
  typename FormatTraits<D>::Cursor cur;
  const Paint *level;
  uint32_t drawn, key;  // the key as stored; ImageBlit::NO_KEY for none
  uint32_t aMask, aOr;  // level = ((raw & aMask) >> 12) | aOr
  uint32_t levelOf(uint32_t raw) const {
    return raw == key ? 0u : (((raw & aMask) >> 12) | aOr);
  }
  static uint32_t paint(uint32_t dst, const Paint &p) {
    if (p.op == PaintOp::FILL) return p.native;
    if (BLEND && p.op == PaintOp::ADD) return addNative<D>(dst, p.native);
    return blendNative<D>(dst, p.native, p.alpha64);
  }
  void put(uint32_t raw) {
    const uint32_t l = levelOf(raw);
    if ((drawn >> l) & 1u) {
      const Paint &p = level[l];
      cur.write(p.op == PaintOp::FILL ? p.native : paint(cur.read(), p));
    }
    cur.next();
  }
  void run(uint32_t raw, int n) {
    const uint32_t l = levelOf(raw);
    if (!((drawn >> l) & 1u)) {
      cur.skip(n);
      return;
    }
    const Paint &p = level[l];
    if (p.op == PaintOp::FILL) {
      cur.fill(n, p.native);
      return;
    }
    for (; n > 0; n--) {
      cur.write(paint(cur.read(), p));
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
  SILHOUETTE,
  COLOR
};

struct ImageBlit;
// The COLOR path's writers of a chunk of Colors: the image's (writeColorsFmt
// with the write mode) and the silhouette's (paintLevels)
using ColorWriter = void (*)(const ImageBlit &b, uint8_t *dl, int x, int n,
                             const Color *src);
void writeColorsImage(const ImageBlit &b, uint8_t *dl, int x, int n,
                      const Color *src);
void paintLevels(const ImageBlit &b, uint8_t *dl, int x, int n,
                 const Color *src);

// What a drawImage() or drawSilhouette() does per pixel
struct ImageBlit {
  PixelFormat dst, src;
  BlendMode mode;
  BlitPath path;
  WriteMode write;  // COLOR
  bool keyed;
  uint32_t key;  // as stored, or NO_KEY
  uint32_t op64;
  // ARGB4444 blends: the factor that takes the alpha field of a native
  // pixel (a4 << 12) to its opacity under op64 in 0..1024, over 16 bits:
  // ((p & 0xF000) * alphaMul) >> 16 = a4 * op64 / 15 * 16 (a 10-bit alpha,
  // which the blend takes as it is; one multiply and shift per pixel in
  // place of a table). 1093 = ceil(65536 * 16 / (15 * 64)): 15 * 1093 * 4096
  // >> 16 is exactly 1024.
  uint32_t alphaMul;
  // drawSilhouette(): the Paint of the color at each opacity level of an
  // image pixel (level l of 15: the alpha nibble of ARGB4444, 15 for the
  // other formats, 0 for the color key), bit l of `drawn` set where that
  // level draws anything (never bit 0)
  bool silhouette;
  Color color;
  uint32_t drawn;
  uint32_t aMask, aOr;  // OpSilhouette: level = ((raw & aMask) >> 12) | aOr
  Paint level[16];
  ColorWriter writeColors;

  static constexpr uint32_t NO_KEY = 0xFFFFFFFFu;

  // false if nothing is drawn. SIL: a drawSilhouette() in the color `sil`
  // (the silhouette code is referenced from the SIL instantiations only, so
  // a program without silhouettes does not link it)
  template <bool SIL>
  bool init(const Graphics2D &g, PixelFormat s, const Color *sil) {
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
    alphaMul = (op64 * 1093u + 32u) >> 6;
    silhouette = SIL;
    writeColors = writeColorsImage;
    if constexpr (SIL) {
      // The color at each level under the blend of the state, like a
      // fillRect() of it; the 16-bit sources are read as they are, the
      // others through Color (the COLOR path turns the alpha into a level)
      color = *sil;
      drawn = 0;
      level[0] = {0, 0, PaintOp::BLEND};
      for (int l = 1; l < 16; l++) {
        const Color cl = colorWithAlpha(color, (colorA(color) * l + 7) / 15);
        if (G2Impl::makePaint(g, cl, level[l])) drawn |= 1u << l;
      }
      if (drawn == 0) return false;
      aMask = srcAlpha ? 0xF000u : 0u;
      aOr = srcAlpha ? 0u : 15u;
      write = WriteMode::ALPHA;  // (not used: paintLevels writes)
      writeColors = paintLevels;
      path = bitsPerPixel(s) == 16 ? BlitPath::SILHOUETTE : BlitPath::COLOR;
      return true;
    } else {
      (void)sil;
    }
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
    } else if (mode == BlendMode::ALPHA && srcAlpha) {
      path = BlitPath::BLEND_ARGB;  // (keyed too: the op skips the key)
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
           path == BlitPath::BLEND_ARGB || path == BlitPath::SILHOUETTE;
  }
};

// The levels of a silhouette already extracted (the COLOR path), as the
// alpha nibble of 16-bit values
struct LevelWalk {
  const uint16_t *p;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) op.put(*p++);
  }
};

constexpr int IMAGE_CHUNK = 64;

void writeColorsImage(const ImageBlit &b, uint8_t *dl, int x, int n,
                      const Color *src) {
  writeColorsFmt(b.dst, dl, x, n, src, b.write, b.op64);
}

// The SILHOUETTE path of a row (out of line, so that blitRow() stays the
// size it has without it: the inlining of the image ops depends on it)
template <typename Walk>
__attribute__((noinline)) void blitRowSilhouette(const ImageBlit &b,
                                                 uint8_t *dl, int x, int n,
                                                 Walk &walk) {
  withFormat(b.dst, [&](auto tag) {
    OpSilhouette<decltype(tag)::value> op;
    op.cur.init(dl, x);
    op.level = b.level;
    op.drawn = b.drawn;
    op.key = b.key;
    op.aMask = b.aMask;
    op.aOr = b.aOr;
    walk(op, n);
  });
}

// The silhouette of n Colors (the COLOR path): the alpha of each (0 for a
// keyed pixel) as a level, painted by the op of the 16-bit sources. One
// function for every source format and walker.
void paintLevels(const ImageBlit &b, uint8_t *dl, int x, int n,
                 const Color *src) {
  uint16_t lv[IMAGE_CHUNK];
  for (int i = 0; i < n; i++) lv[i] = (uint16_t)((colorA(src[i]) >> 4) << 12);
  LevelWalk walk{lv};
  withFormat(b.dst, [&](auto tag) {
    OpSilhouette<decltype(tag)::value> op;
    op.cur.init(dl, x);
    op.level = b.level;
    op.drawn = b.drawn;
    op.key = ImageBlit::NO_KEY;
    op.aMask = 0xF000u;
    op.aOr = 0;
    walk(op, n);
  });
}

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
        op.alphaMul = b.alphaMul;
        op.key = b.key;
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
    case BlitPath::SILHOUETTE:  // (blitRowOf takes it before this)
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
              b.writeColors(b, dl, x + i, k, tmp);
            }
          }
        });
      }
      break;
  }
}

// A row of an image, or of a silhouette (SIL): the SILHOUETTE path has a
// row function of its own, the others are blitRow()
template <bool SIL, typename Walk>
inline void blitRowOf(const ImageBlit &b, uint8_t *dl, int x, int n,
                      Walk &walk) {
  if constexpr (SIL) {
    if (b.path == BlitPath::SILHOUETTE) {
      blitRowSilhouette(b, dl, x, n, walk);
      return;
    }
  }
  blitRow(b, dl, x, n, walk);
}

// Walkers. They keep their position between calls, so that the Color path
// can take a row in chunks.

// Untransformed: the source pixels in order (the 16-bit silhouettes; the
// images have row loops of their own). Used with blitRowSilhouette() only,
// so it has no SRC16_ONLY / RUNS.
struct PlainWalk {
  const uint8_t *line;
  int pos;
  template <typename Op>
  void operator()(Op &op, int n) {
    for (; n > 0; n--) op.put(loadRaw<Op::SRC>(line, pos++));
  }
};

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
  // The convex polygon, if any, as bounds on the column x per row: an edge
  // whose inward normal has a component along the row is x >= (or <=) a
  // bound that changes linearly with the row (x0 at row by0, dx per row);
  // one across the rows only narrows [by0, by1] once.
  struct Bound {
    float x0, dx;
  };
  Bound lo[Graphics2D::IMAGE_POLYGON_MAX], hi[Graphics2D::IMAGE_POLYGON_MAX];
  int loCount = 0, hiCount = 0;
  bool hasPolygon = false;

  bool init(const affine2f &m, const Rect &s, const Rect &readable,
            const Rect &clip, const int16_t *polygon = nullptr,
            int count = 0) {
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
    if (!polygon || count < 3) return true;
    if (count > Graphics2D::IMAGE_POLYGON_MAX) return false;
    vec2f pf[Graphics2D::IMAGE_POLYGON_MAX];
    for (int i = 0; i < count; i++)
      pf[i] = {(float)polygon[2 * i], (float)polygon[2 * i + 1]};
    return initPolygonF(pf, count, s);
  }

  // `poly`: `count` vertices relative to the top-left corner of `s`. False
  // for a degenerate polygon or too many vertices (nothing is drawn); fewer
  // than 3 vertices mean no polygon.
  bool initPolygonF(const vec2f *poly, int count, const Rect &s) {
    loCount = hiCount = 0;
    hasPolygon = false;
    if (!poly || count < 3) return true;
    if (count > Graphics2D::IMAGE_POLYGON_MAX) return false;
    // The winding, from the doubled signed area
    float area2 = 0.0f;
    for (int i = 0; i < count; i++) {
      const int j = (i + 1) % count;
      area2 += poly[i].x * poly[j].y - poly[j].x * poly[i].y;
    }
    if (!(area2 != 0.0f)) return false;
    const float sign = area2 > 0 ? 1.0f : -1.0f;
    const float lim = 1e6f;
    hasPolygon = true;
    for (int i = 0; i < count; i++) {
      const int j = (i + 1) % count;
      const float ex = poly[j].x - poly[i].x;
      const float ey = poly[j].y - poly[i].y;
      if (ex == 0.0f && ey == 0.0f) continue;  // a repeated vertex
      // The inward unit normal of the edge (for a positive area, its left
      // side): inside is nx u + ny v >= c, in absolute source coordinates
      const float len = std::sqrt(ex * ex + ey * ey);
      const float nx = -ey * sign / len, ny = ex * sign / len;
      const float c = nx * (poly[i].x + (float)s.x) +
                      ny * (poly[i].y + (float)s.y);
      // With u = A x + C y + E, v = B x + D y + F that is
      // g x >= h0 + hy y
      const float g = nx * A + ny * B;
      const float h0 = c - nx * E - ny * F, hy = -(nx * C + ny * D);
      if (g == 0.0f) {
        // Across the rows: hy y <= -h0
        if (hy > 0.0f) {
          by1 = std::min(by1, (int)std::floor(std::clamp(-h0 / hy, -lim, lim)));
        } else if (hy < 0.0f) {
          by0 = std::max(by0, (int)std::ceil(std::clamp(-h0 / hy, -lim, lim)));
        } else if (h0 > 0.0f) {
          return false;
        }
        continue;
      }
      const float ig = 1.0f / g;
      const Bound b = {(h0 + hy * (float)by0) * ig, hy * ig};
      if (g > 0.0f) {
        lo[loCount++] = b;
      } else {
        hi[hiCount++] = b;
      }
    }
    return by0 <= by1;
  }

  // Narrow [k0, k1] (columns relative to xr) to those inside the polygon on
  // row y: the tightest of the lower and of the upper bounds, one multiply
  // and add per edge. In float; the rectangle above keeps the walk within
  // the image whatever this rounds to.
  bool narrowToPolygon(int y, int xr, int &k0, int &k1) const {
    const float dy = (float)(y - by0);
    float xlo = -1e9f, xhi = 1e9f;
    for (int i = 0; i < loCount; i++)
      xlo = std::max(xlo, lo[i].x0 + lo[i].dx * dy);
    for (int i = 0; i < hiCount; i++)
      xhi = std::min(xhi, hi[i].x0 + hi[i].dx * dy);
    const float lim = 1e6f;
    k0 = std::max(k0, (int)std::ceil(std::clamp(xlo - (float)xr, -lim, lim)));
    k1 = std::min(k1, (int)std::floor(std::clamp(xhi - (float)xr, -lim, lim)));
    return k0 <= k1;
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
      if (hasPolygon && !narrowToPolygon(y, xr, k0, k1)) continue;
      // The 16.16 offsets are formed in uint32 so that an intermediate wraps
      // instead of overflowing (the sums lie within the image)
      fn(y, xr + k0, k1 - k0 + 1,
         (int32_t)((uint32_t)u + (uint32_t)k0 * (uint32_t)du),
         (int32_t)((uint32_t)v + (uint32_t)k0 * (uint32_t)dv));
    }
  }
};

// ---------------------------------------------------------------------------
// Image paths, in target pixels

// Plain: `s` (normalized) with its top-left corner at (x, y), no color key.
// SIL: the silhouette in the color `sil` (see ImageBlit::init)
template <bool SIL>
void blitPlain(const Graphics2D &g, const Texture &img, int x, int y,
               const Rect &s, const Color *sil) {
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
  if (!b.init<SIL>(g, img.format, sil)) return;

  if constexpr (SIL) {
    for (int j = 0; j < dst.height; j++) {
      uint8_t *dl = target.linePtr(dst.y + j);
      const uint8_t *sl = img.linePtr(src.y + j);
      if (b.path == BlitPath::SILHOUETTE) {
        PlainWalk w{sl, src.x};
        blitRowSilhouette(b, dl, dst.x, dst.width, w);
      } else {
        // (through Color: a unit step of the scaled walker, so that no
        // path is compiled for this walker alone)
        StepWalk w{sl, src.x, 1, 0, 0, 0, 1};
        blitRow(b, dl, dst.x, dst.width, w);
      }
    }
    return;
  }

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
      (!b.keyed || img.format == PixelFormat::ARGB4444) &&
      blendRowFmt(target.format, img.format, target.linePtr(dst.y), dst.x,
                  img.linePtr(src.y), src.x, dst.width, b.op64, b.alphaMul,
                  b.key)) {
    for (int j = 1; j < dst.height; j++) {
      blendRowFmt(target.format, img.format, target.linePtr(dst.y + j), dst.x,
                  img.linePtr(src.y + j), src.x, dst.width, b.op64, b.alphaMul,
                  b.key);
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
template <bool SIL>
void blitScaled(const Graphics2D &g, const Texture &img, const Rect &dst,
                const Rect &s, const Color *sil) {
  ImageBlit b;
  if (!b.init<SIL>(g, img.format, sil)) return;
  if (dst.width == s.width && dst.height == s.height && !b.keyed) {
    blitPlain<SIL>(g, img, dst.x, dst.y, s, sil);
    return;
  }
  ScaleAxis ax, ay;
  if (!mapAxes(dst, s, img.width, img.height, g.clipRect(), ax, ay)) return;
  const Surface &target = g.target();

  const bool runs = ax.runs && b.takesRuns();
  // A plain copy repeats the previous target row for the same source row
  const bool repeat = !SIL && b.write == WriteMode::COPY &&
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
        blitRowOf<SIL>(b, dl, ax.start, ax.count, w);
      } else {
        StepWalk w{sl, ax.pos, ax.step, ax.dir, ax.rem, ax.rStep, ax.den};
        blitRowOf<SIL>(b, dl, ax.start, ax.count, w);
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
// (normalized) to the target; `polygon`, if any, clips the source further
// (see Graphics2D::drawImage)
template <bool SIL>
void blitAffine(const Graphics2D &g, const Texture &img, const affine2f &m,
                const Rect &s, const int16_t *polygon, int count,
                const Color *sil) {
  AffineRows ar;
  if (!ar.init(m, s, s.intersect(Rect{0, 0, img.width, img.height}),
               g.clipRect(), polygon, count))
    return;
  ImageBlit b;
  if (!b.init<SIL>(g, img.format, sil)) return;
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
      blitRowOf<SIL>(b, dl, x, n, w);
      return;
    }
#endif
    AffineWalk w{(const uint8_t *)img.pixels, img.stride, u, v, ar.du, ar.dv};
    blitRowOf<SIL>(b, dl, x, n, w);
  });
#if SHAPOGFX2D_RP2_INTERP
  if (useInterp) interp.end();
#endif
}

// The bodies of drawImage() (SIL false, `sil` null) and drawSilhouette()
// (SIL true, `sil` its color)
template <bool SIL>
void imageAt(Graphics2D &g, const Texture &img, int dx, int dy,
             const Rect &srcRect, const Color *sil) {
  if (!g.hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  const Rect s = srcRect.normalized();
  if (s.isEmpty()) return;
  const TransformKind kind = G2Impl::kind(g);
  const int ox = G2Impl::offsetX(g), oy = G2Impl::offsetY(g);
  if (!TRANSFORM || kind <= TransformKind::TRANSLATE) {
    if (COLOR_KEY && g.hasColorKey())
      blitScaled<SIL>(g, img, Rect{dx + ox, dy + oy, s.width, s.height}, s,
                      sil);
    else
      blitPlain<SIL>(g, img, dx + ox, dy + oy, s, sil);
  } else if (G2Impl::wantsAntialias(g)) {
    G2Impl::drawImageAA(
        g, img, s,
        G2Impl::matrix(g) * affine2f::translation((float)dx, (float)dy),
        nullptr, 0, sil);
  } else if (kind == TransformKind::SCALE) {
    blitScaled<SIL>(g, img,
                    G2Impl::mapRectSigned(g, RectF{(float)dx, (float)dy,
                                                   (float)s.width,
                                                   (float)s.height}),
                    s, sil);
  } else {
    blitAffine<SIL>(
        g, img, G2Impl::matrix(g) * affine2f::translation((float)dx, (float)dy),
        s, nullptr, 0, sil);
  }
}

template <bool SIL>
void imageInPolygon(Graphics2D &g, const Texture &img, int dx, int dy,
                    const Rect &srcRect, const int16_t *polygon, int count,
                    const Color *sil) {
  if (!polygon || count < 3) {
    imageAt<SIL>(g, img, dx, dy, srcRect, sil);
    return;
  }
  if (!g.hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  const Rect s = srcRect.normalized();
  if (s.isEmpty()) return;
  // The transformed path whatever the transform: the plain and scaled paths
  // have no polygon (and TRANSLATE snaps the offset, which this does not)
  const affine2f m =
      G2Impl::matrix(g) * affine2f::translation((float)dx, (float)dy);
  if (G2Impl::wantsAntialias(g)) {
    G2Impl::drawImageAA(g, img, s, m, polygon, count, sil);
    return;
  }
  blitAffine<SIL>(g, img, m, s, polygon, count, sil);
}

template <bool SIL>
void imageScaled(Graphics2D &g, const Texture &img, const Rect &dst,
                 const Rect &srcRect, const Color *sil) {
  if (!g.hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  const Rect s = srcRect.normalized();
  if (s.isEmpty() || dst.width == 0 || dst.height == 0) return;
  const TransformKind kind = G2Impl::kind(g);
  if (G2Impl::wantsAntialias(g) && kind > TransformKind::TRANSLATE) {
    // (a destination rectangle of whole pixels has no edge to smooth)
    affine2f m = G2Impl::matrix(g);
    m.translate((float)dst.x, (float)dst.y)
        .scale((float)dst.width / (float)s.width,
               (float)dst.height / (float)s.height);
    G2Impl::drawImageAA(g, img, s, m, nullptr, 0, sil);
    return;
  }
  if (!TRANSFORM || kind <= TransformKind::TRANSLATE) {
    blitScaled<SIL>(g, img,
                    dst.offset(G2Impl::offsetX(g), G2Impl::offsetY(g)), s, sil);
  } else if (kind == TransformKind::SCALE) {
    blitScaled<SIL>(g, img, G2Impl::mapRectSigned(g, RectF(dst)), s, sil);
  } else {
    affine2f m = G2Impl::matrix(g);
    m.translate((float)dst.x, (float)dst.y)
        .scale((float)dst.width / (float)s.width,
               (float)dst.height / (float)s.height);
    blitAffine<SIL>(g, img, m, s, nullptr, 0, sil);
  }
}

}  // namespace

void Graphics2D::drawImage(const Texture &img, int dx, int dy,
                           const Rect &srcRect) {
  imageAt<false>(*this, img, dx, dy, srcRect, nullptr);
}

void Graphics2D::drawImage(const Texture &img, int dx, int dy,
                           const Rect &srcRect, const int16_t *polygon,
                           int count) {
  imageInPolygon<false>(*this, img, dx, dy, srcRect, polygon, count, nullptr);
}

void Graphics2D::drawImage(const Texture &img, const Rect &dst,
                           const Rect &srcRect) {
  imageScaled<false>(*this, img, dst, srcRect, nullptr);
}

void Graphics2D::drawSilhouette(const Texture &img, int dx, int dy,
                                const Rect &srcRect, Color c) {
  imageAt<true>(*this, img, dx, dy, srcRect, &c);
}

void Graphics2D::drawSilhouette(const Texture &img, int dx, int dy,
                                const Rect &srcRect, const int16_t *polygon,
                                int count, Color c) {
  imageInPolygon<true>(*this, img, dx, dy, srcRect, polygon, count, &c);
}

void Graphics2D::drawSilhouette(const Texture &img, const Rect &dst,
                                const Rect &srcRect, Color c) {
  imageScaled<true>(*this, img, dst, srcRect, &c);
}

// ---------------------------------------------------------------------------
// Images with antialiasing: every pixel whose center lies within 0.71 pixel
// of the image's outline (its rectangle, or the polygon) or inside it is
// sampled bilinearly at the source point under its center (premultiplied,
// so that a transparent or keyed texel lends no color), and the ones along
// the outline take its coverage as a factor of their alpha. The coverage is
// approximated from the outline's edges: for each edge the overlap of the
// pixel with its inner side (the signed distance of the center plus a half,
// clamped to 0..1), multiplied over the edges, which is exact along an edge
// and a fair product at the corners; pixels deeper than 0.71 inside every
// edge are whole and skip the arithmetic. No rasterizer, buffer or sub-rows
// are needed.

namespace {

// 65536 / a for a in 1..255, to undo the premultiplication without a
// division per pixel
struct Reciprocal {
  uint32_t v[256];
  Reciprocal() {
    v[0] = 0;
    for (int a = 1; a < 256; a++) v[a] = 65536u / (uint32_t)a;
  }
};
const Reciprocal RCP;

struct ImageAA {
  Graphics2D *g;
  const Texture *img;
  Rect src;        // the texels sampled (clamped to it)
  affine2f inv;    // target pixels to image pixels
  int32_t duQ, dvQ;  // inv.a, inv.b in 16.16: the step along a row
  bool keyed;
  uint32_t keyNative;
  WriteMode mode;
  uint32_t opacity64;
  bool silhouette;  // drawSilhouette(): the alpha only, into `color`
  Color color;
  // The outline on the target as its edges, inside is nx x + ny y >= c, in
  // 16.16 (the unit normals, so d below is a distance in pixels); and, for
  // the spans, the bound each edge puts on x along a row, x >= (or <=)
  // xb0 + xbdy * y in 24.8, for the outer and the inner inset (kind: 1
  // lower bound, -1 upper, 0 the edge is across the rows: then xb0 + xbdy
  // * y > 0 means the row is outside)
  int n = 0;
  int32_t nx[Graphics2D::IMAGE_POLYGON_MAX], ny[Graphics2D::IMAGE_POLYGON_MAX],
      c[Graphics2D::IMAGE_POLYGON_MAX];
  int8_t kind[Graphics2D::IMAGE_POLYGON_MAX];
  // (64 bits: where a row meets an edge that is nearly along it lies far
  // away, up to a thousand times the coordinates)
  int64_t xbOut0[Graphics2D::IMAGE_POLYGON_MAX], xbIn0[Graphics2D::IMAGE_POLYGON_MAX],
      xbdy[Graphics2D::IMAGE_POLYGON_MAX];
  // Pixels with centers up to OUT outside the outline are partly covered;
  // those IN or more inside every edge are whole
  static constexpr float OUT = 0.71f, IN = 0.71f;

  static int32_t q16(float v) {
    return (int32_t)std::clamp(v * 65536.0f, -2147483520.0f, 2147483520.0f);
  }
  static int64_t q8(float v) {
    return (int64_t)std::clamp(v * 256.0f, -1e15f, 1e15f);
  }

  bool initEdges(const vec2f *pts, int count) {
    float area2 = 0.0f;
    for (int i = 0; i < count; i++) {
      const int j = (i + 1) % count;
      area2 += pts[i].x * pts[j].y - pts[j].x * pts[i].y;
    }
    if (!(std::fabs(area2) > 1e-6f)) return false;
    const float sign = area2 > 0.0f ? 1.0f : -1.0f;
    n = 0;
    for (int i = 0; i < count; i++) {
      const int j = (i + 1) % count;
      const float ex = pts[j].x - pts[i].x, ey = pts[j].y - pts[i].y;
      const float len = std::sqrt(ex * ex + ey * ey);
      if (!(len > 1e-6f)) continue;
      const float fx = -ey * sign / len, fy = ex * sign / len;
      const float fc = fx * pts[i].x + fy * pts[i].y;
      nx[n] = q16(fx);
      ny[n] = q16(fy);
      c[n] = q16(fc);
      // x >= (c + inset - fy * (y + 0.5)) / fx, with |1 / fx| kept within
      // 2^10 so that the bounds of the rows on screen fit 24.8
      if (std::fabs(fx) > 1e-3f) {
        const float ifx = 1.0f / fx;
        kind[n] = fx > 0.0f ? 1 : -1;
        xbOut0[n] = q8((fc - OUT - fy * 0.5f) * ifx);
        xbIn0[n] = q8((fc + IN - fy * 0.5f) * ifx);
        xbdy[n] = q8(-fy * ifx);
      } else {
        // Across the rows: outside where fc + inset - fy (y + 0.5) > 0
        kind[n] = 0;
        xbOut0[n] = q8(fc - OUT - fy * 0.5f);
        xbIn0[n] = q8(fc + IN - fy * 0.5f);
        xbdy[n] = q8(-fy);
      }
      n++;
    }
    return n >= 3;
  }

  // The pixels of row y whose centers lie inside the outline moved out by
  // OUT (`inner` false) or in by IN (true), as [a, b); false when there are
  // none. Integer: one multiply-add per edge.
  bool span(int y, bool inner, int &a, int &b) const {
    const int64_t *xb0 = inner ? xbIn0 : xbOut0;
    // The bounds, clamped to the pixels that exist (24.8)
    constexpr int64_t LIM = (int64_t)1 << 23;
    int64_t lo = -LIM, hi = LIM;
    for (int i = 0; i < n; i++) {
      const int64_t xb = xb0[i] + xbdy[i] * y;
      if (kind[i] > 0) {
        lo = std::max(lo, xb);
      } else if (kind[i] < 0) {
        hi = std::min(hi, xb);
      } else if (xb > 0) {
        return false;
      }
    }
    if (lo >= hi) return false;
    // The centers (x + 0.5) within (lo, hi): x from ceil(lo - 0.5)
    a = (int)((lo - 128 + 255) >> 8);
    b = (int)((hi - 128) >> 8) + 1;
    return a < b;
  }

  // The texel at column u of `row` as a premultiplied Color: (r a, g a,
  // b a, a) with the channels in 0..255 * 0..255
  template <PixelFormat F>
  inline void texel(const uint8_t *row, int u, uint32_t &r, uint32_t &gg,
                    uint32_t &b, uint32_t &a) const {
    typename FormatTraits<F>::Cursor cur;
    cur.init((void *)row, u);
    const uint32_t raw = cur.read();
    if (keyed && raw == keyNative) {
      r = gg = b = a = 0;
      return;
    }
    const Color col = FormatTraits<F>::toColor(raw);
    a = (uint32_t)colorA(col);
    r = (uint32_t)colorR(col) * a;
    gg = (uint32_t)colorG(col) * a;
    b = (uint32_t)colorB(col) * a;
  }

  // The pixels [xa, xa + n_) of row y: `edge`: their coverage is computed
  // (else they are whole). The four texels around the source point are
  // fetched again only when the point leaves their cell (a magnified image
  // keeps them over several pixels), and when all four are opaque the
  // channels are mixed without the premultiplication.
  template <PixelFormat F>
  void row(int y, int xa, int n_, bool edge, Color *out) const {
    const float yc = (float)y + 0.5f, xc = (float)xa + 0.5f;
    // The source point under the first pixel's center, less half a texel
    // (texel centers are at .5), in 16.16, stepped along the row in integers
    int32_t u = q16(inv.a * xc + inv.c * yc + inv.tx - 0.5f);
    int32_t v = q16(inv.b * xc + inv.d * yc + inv.ty - 0.5f);
    const int uMax = src.right() - 1, vMax = src.bottom() - 1;
    // The overlap of the first pixel with the inner side of each edge (its
    // signed distance plus a half) in 16.16, stepped along the row
    int32_t d[Graphics2D::IMAGE_POLYGON_MAX];
    if (edge) {
      const int32_t xcQ = q16(xc), ycQ = q16(yc);
      for (int k = 0; k < n; k++)
        d[k] = (int32_t)(((int64_t)nx[k] * xcQ + (int64_t)ny[k] * ycQ) >> 16) - c[k] + 32768;
    }
    // The cell of the last point, and its texels
    int cu = INT_MIN, cv = INT_MIN;
    uint32_t r00 = 0, g00 = 0, b00 = 0, a00 = 0, r10 = 0, g10 = 0, b10 = 0, a10 = 0,
             r01 = 0, g01 = 0, b01 = 0, a01 = 0, r11 = 0, g11 = 0, b11 = 0, a11 = 0;
    bool opaque = false;
    for (int i = 0; i < n_; i++, u += duQ, v += dvQ) {
      uint32_t cov = 64;
      if (edge) {
        // The product of the overlaps, 16.16
        int32_t f = 65536;
        for (int k = 0; k < n; k++) {
          const int32_t dk = d[k];
          d[k] = dk + nx[k];
          if (dk <= 0) f = 0;
          else if (dk < 65536) f = (int32_t)(((int64_t)f * dk) >> 16);
        }
        cov = (uint32_t)((f + 512) >> 10);
        if (cov == 0) {
          out[i] = 0;
          continue;
        }
      }
      const int iu = u >> 16, iv = v >> 16;
      if (iu != cu || iv != cv) {
        cu = iu;
        cv = iv;
        const int u0 = clampInt(src.x, uMax, iu), v0 = clampInt(src.y, vMax, iv);
        const int u1 = clampInt(src.x, uMax, iu + 1), v1 = clampInt(src.y, vMax, iv + 1);
        const uint8_t *row0 = img->linePtr(v0), *row1 = img->linePtr(v1);
        texel<F>(row0, u0, r00, g00, b00, a00);
        texel<F>(row0, u1, r10, g10, b10, a10);
        texel<F>(row1, u0, r01, g01, b01, a01);
        texel<F>(row1, u1, r11, g11, b11, a11);
        opaque = (a00 & a10 & a01 & a11) == 255;
      }
      const uint32_t wx = ((uint32_t)u >> 8) & 255u, wy = ((uint32_t)v >> 8) & 255u;
      const uint32_t w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy),
                     w01 = (256 - wx) * wy, w11 = wx * wy;  // sum 65536
      uint32_t r = 0, gg = 0, b = 0, a;
      if (opaque) {
        a = 255;
      } else {
        a = (a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11) >> 16;
        if (a == 0) {
          out[i] = 0;
          continue;
        }
      }
      const uint32_t ac = cov >= 64 ? a : (a * cov + 32u) >> 6;
      if (silhouette) {
        // The color with its alpha scaled by the image's
        out[i] = colorWithAlpha(color, (int)((ac * (uint32_t)colorA(color) + 127u) / 255u));
        continue;
      }
      if (opaque) {
        // The channels are premultiplied by 255: mix and divide by it
        r = (r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11) / (65536u * 255u);
        gg = (g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11) / (65536u * 255u);
        b = (b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11) / (65536u * 255u);
      } else {
        // The premultiplied sums are 65536 * 255 * 255 at most: fit 32 bits;
        // the division by the alpha through the reciprocal table
        const uint32_t rcp = RCP.v[a];
        r = std::min<uint32_t>((((r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11) >> 16) * rcp) >> 16, 255u);
        gg = std::min<uint32_t>((((g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11) >> 16) * rcp) >> 16, 255u);
        b = std::min<uint32_t>((((b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11) >> 16) * rcp) >> 16, 255u);
      }
      out[i] = makeColor((int)r, (int)gg, (int)b, (int)ac);
    }
  }

  // The pixels [xa, xb) of row y, clipped
  void part(int y, int xa, int xb, bool edge) const {
    const Rect &clip = g->clipRect();
    xa = std::max(xa, clip.x);
    xb = std::min(xb, clip.right());
    if (xb <= xa) return;
    constexpr int CHUNK = 32;
    Color buf[CHUNK];
    const Surface &t = g->target();
    for (int x = xa; x < xb; x += CHUNK) {
      const int k = std::min(CHUNK, xb - x);
      withFormat(img->format, [&](auto tag) {
        row<decltype(tag)::value>(y, x, k, edge, buf);
      });
      writeColorsFmt(t.format, t.linePtr(y), x, k, buf, mode, opacity64);
    }
  }

  // Every row in [y0, y1]: the edge pixels on either side, the whole ones
  // between
  void rows(int y0, int y1) const {
    for (int y = y0; y <= y1; y++) {
      int oa, ob, ia, ib;
      if (!span(y, false, oa, ob)) continue;
      if (!span(y, true, ia, ib)) {
        part(y, oa, ob, true);
        continue;
      }
      part(y, oa, std::min(ob, ia), true);
      part(y, std::max(oa, ia), std::min(ob, ib), false);
      part(y, std::max(oa, ib), ob, true);
    }
  }
};

}  // namespace

void detail::G2Impl::drawImageAA(Graphics2D &g, const Texture &img,
                                 const Rect &src, const affine2f &mSrc,
                                 const int16_t *polygon, int count,
                                 const Color *silhouette) {
  if (!g.hasTarget()) return;
  ImageAA ctx;
  ctx.g = &g;
  ctx.img = &img;
  ctx.src = src;
  ctx.silhouette = silhouette != nullptr;
  ctx.color = silhouette ? *silhouette : 0;
  if (ctx.silhouette && colorA(ctx.color) == 0 &&
      g.blendMode() != BlendMode::NONE)
    return;
  // mSrc maps points relative to the top-left corner of src (as the polygon
  // is given); m maps image pixels
  const affine2f m =
      mSrc * affine2f::translation(-(float)src.x, -(float)src.y);
  if (!m.invert(ctx.inv)) return;
  // Within a texel per 32768 target pixels in 16.16: the steps are kept
  // small enough for that (an image is at most AFFINE_SIZE_MAX texels)
  if (!(std::fabs(ctx.inv.a) < 16384.0f && std::fabs(ctx.inv.b) < 16384.0f)) return;
  ctx.duQ = ImageAA::q16(ctx.inv.a);
  ctx.dvQ = ImageAA::q16(ctx.inv.b);
  ctx.keyed = COLOR_KEY && g.hasColorKey();
  ctx.keyNative = ctx.keyed ? colorToNative(img.format, g.colorKey()) : 0;
  ctx.mode = g.blendMode() == BlendMode::ADD ? WriteMode::ADD : WriteMode::ALPHA;
  ctx.opacity64 = alpha255To64((uint32_t)g.opacity());
  // The outline on the target: the polygon (image pixels relative to src),
  // or the source rectangle
  vec2f pts[Graphics2D::IMAGE_POLYGON_MAX];
  int n = 0;
  if (polygon && count >= 3) {
    if (count > Graphics2D::IMAGE_POLYGON_MAX) return;
    for (int i = 0; i < count; i++)
      pts[n++] = mSrc.apply((float)polygon[2 * i], (float)polygon[2 * i + 1]);
  } else {
    pts[n++] = mSrc.apply(0.0f, 0.0f);
    pts[n++] = mSrc.apply((float)src.width, 0.0f);
    pts[n++] = mSrc.apply((float)src.width, (float)src.height);
    pts[n++] = mSrc.apply(0.0f, (float)src.height);
  }
  if (!ctx.initEdges(pts, n)) return;
  float y0 = pts[0].y, y1 = pts[0].y;
  for (int i = 1; i < n; i++) {
    y0 = std::min(y0, pts[i].y);
    y1 = std::max(y1, pts[i].y);
  }
  if (!(y0 <= y1)) return;
  const Rect &clip = g.clipRect();
  const float lim = 1e6f;
  const int ry0 = std::max(clip.y, (int)std::floor(std::clamp(y0, -lim, lim) - ImageAA::OUT));
  const int ry1 = std::min(clip.bottom() - 1, (int)std::ceil(std::clamp(y1, -lim, lim) + ImageAA::OUT));
  if (ry0 > ry1) return;
  ctx.rows(ry0, ry1);
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

// Untransformed, with a foreground only (text): pixel by pixel per format
void maskPixels(const Surface &target, const Rect &clip, const MaskSource &m,
                const Rect &src, int x, int y, const Paint &fg) {
  const Rect dst = Rect{x, y, src.width, src.height}.intersect(clip);
  if (dst.isEmpty()) return;
  const int sx = src.x + dst.x - x, sy = src.y + dst.y - y;
  withFormat(target.format, [&](auto tag) {
    maskPixelsT<decltype(tag)::value>(target, m, sx, sy, dst, fg);
  });
}

// Untransformed, as spans of equal bits
void maskPlain(const Raster &ras, const MaskSource &m, const Rect &src, int x,
               int y, const Paint *fg, const Paint *bg) {
  const Rect dst = Rect{x, y, src.width, src.height}.intersect(ras.clip);
  if (dst.isEmpty()) return;
  const int sx = src.x + dst.x - x, sy = src.y + dst.y - y;
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
  if (!TRANSFORM || g.kind_ <= TransformKind::TRANSLATE) {
    if (fg && !bg)
      maskPixels(g.target_, g.clipRect(), m, src, dx + g.ox_, dy + g.oy_, *fg);
    else
      maskPlain(raster(g), m, src, dx + g.ox_, dy + g.oy_, fg, bg);
    return;
  }
  const Raster ras = raster(g);
  if (g.kind_ == TransformKind::SCALE) {
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

// Under a transform with antialiasing: every target pixel samples the mask
// at four points (a 2 x 2 grid half a pixel apart) and takes the foreground
// with a quarter of its alpha per set bit, the background with a quarter
// per clear one
void detail::G2Impl::drawMaskAA(Graphics2D &g, const MaskSource &m,
                                const Rect &src, int dx, int dy, Color fg,
                                Color bg) {
  if (src.isEmpty() || !g.hasTarget()) return;
  const Raster ras = raster(g);
  if (ras.clip.isEmpty()) return;
  const affine2f mat =
      g.state_.transform * affine2f::translation((float)dx, (float)dy);
  affine2f inv;
  if (!mat.invert(inv)) return;
  // The target pixels the source rectangle may touch
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int k = 0; k < 4; k++) {
    const vec2f p = mat.apply((float)(k & 1 ? src.right() : src.x),
                              (float)(k & 2 ? src.bottom() : src.y));
    x0 = std::min(x0, p.x);
    x1 = std::max(x1, p.x);
    y0 = std::min(y0, p.y);
    y1 = std::max(y1, p.y);
  }
  if (!(x0 <= x1 && y0 <= y1)) return;
  const Rect area = Rect{snap(x0), snap(y0), snap(x1) + 1 - snap(x0),
                         snap(y1) + 1 - snap(y0)}
                        .intersect(ras.clip);
  if (area.isEmpty()) return;
  const WriteMode mode = g.blendMode() == BlendMode::ADD ? WriteMode::ADD
                                                         : WriteMode::ALPHA;
  const uint32_t op64 = alpha255To64((uint32_t)g.opacity());
  const bool hasBg = colorA(bg) != 0;
  if (colorA(fg) == 0 && !hasBg) return;
  // The four sample offsets in source pixels
  const float ox[4] = {-0.25f * inv.a - 0.25f * inv.c, 0.25f * inv.a - 0.25f * inv.c,
                       -0.25f * inv.a + 0.25f * inv.c, 0.25f * inv.a + 0.25f * inv.c};
  const float oy[4] = {-0.25f * inv.b - 0.25f * inv.d, 0.25f * inv.b - 0.25f * inv.d,
                       -0.25f * inv.b + 0.25f * inv.d, 0.25f * inv.b + 0.25f * inv.d};
  constexpr int CHUNK = 32;
  Color fgRow[CHUNK], bgRow[CHUNK];
  for (int y = area.y; y < area.bottom(); y++) {
    for (int x = area.x; x < area.right(); x += CHUNK) {
      const int n = std::min(CHUNK, area.right() - x);
      // The source point under the first pixel's center, stepped along x
      float u = inv.a * ((float)x + 0.5f) + inv.c * ((float)y + 0.5f) + inv.tx;
      float v = inv.b * ((float)x + 0.5f) + inv.d * ((float)y + 0.5f) + inv.ty;
      bool any = false;
      for (int i = 0; i < n; i++, u += inv.a, v += inv.b) {
        int set = 0, in = 0;
        for (int k = 0; k < 4; k++) {
          const int su = (int)std::floor(u + ox[k]), sv = (int)std::floor(v + oy[k]);
          if (su < src.x || su >= src.right() || sv < src.y || sv >= src.bottom())
            continue;
          in++;
          if (maskBit(m, m.base + (uint32_t)sv * m.stride + (uint32_t)su)) set++;
        }
        fgRow[i] = set ? colorWithAlpha(fg, (colorA(fg) * set + 2) / 4) : 0;
        bgRow[i] = in > set ? colorWithAlpha(bg, (colorA(bg) * (in - set) + 2) / 4) : 0;
        if (set || in > set) any = true;
      }
      if (!any) continue;
      uint8_t *line = ras.target.linePtr(y);
      if (hasBg) writeColorsFmt(ras.target.format, line, x, n, bgRow, mode, op64);
      if (colorA(fg) != 0) writeColorsFmt(ras.target.format, line, x, n, fgRow, mode, op64);
    }
  }
}

void Graphics2D::drawBitmap(const Texture &bmp, int dx, int dy,
                            const Rect &srcRect, Color fg, Color bg) {
  if (!hasTarget() || !bmp.pixels || bmp.format != PixelFormat::GRAY1) return;
#if SHAPOGFX_FORMAT_GRAY1
  const Rect s = srcRect.normalized();
  const Rect in = s.intersect({0, 0, bmp.width, bmp.height});
  if (in.isEmpty()) return;
  if (kind_ > TransformKind::TRANSLATE && G2Impl::wantsAntialias(*this)) {
    const MaskSource mm = {(const uint8_t *)bmp.pixels, 0, bmp.stride * 8u};
    G2Impl::drawMaskAA(*this, mm, in, dx + (in.x - s.x), dy + (in.y - s.y), fg,
                       bg);
    return;
  }
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
