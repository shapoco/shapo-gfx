// Graphics2D: images (plain, scaled and transformed, with the blend and the
// color key of the state), their silhouettes and 1-bit masks (bitmaps and
// glyphs).

#include "arch.hpp"
#include <climits>
#include <cstring>

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
    return arch::loadBit(line, (uint32_t)x);
  } else if constexpr (S == PixelFormat::RGB444) {
    const uint8_t *p = line + (x >> 1) * 3;
    if (x & 1) return ((uint32_t)(p[1] & 0x0Fu) << 8) | p[2];
    return ((uint32_t)p[0] << 4) | (p[1] >> 4);
  } else {
    return arch::loadPixel16(line, x);
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

  // The ops that write runs at once take them from this run length on (q =
  // floor(dn / sn), the magnification); below, the per-pixel walk is
  // cheaper: a blend converts and tests every source pixel once in a run,
  // a copy only stores, so a run pays later. 0: never (the others gain
  // little from runs).
  uint32_t runsFrom() const {
    switch (path) {
      case BlitPath::BLEND_ARGB:
      case BlitPath::SILHOUETTE:
        return 2;
      case BlitPath::COPY_KEY16:
      case BlitPath::COPY_KEY:
        return 3;
      case BlitPath::COPY16:
      case BlitPath::COPY:
        return 4;
      default:
        return 0;
    }
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
// with RUNS takes only the paths of runsFrom() (the caller walks the others
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
// can take a row in chunks. Within a call the position lives in locals: as
// members it would be stored for every pixel wherever the pixel load or the
// op's store may alias it (a word load of SHAPOGFX_ARCH_WORD_LOADS, a byte
// or a Color written by the op).

// Untransformed: the source pixels in order (the 16-bit silhouettes; the
// images have row loops of their own). Used with blitRowSilhouette() only,
// so it has no SRC16_ONLY / RUNS.
struct PlainWalk {
  const uint8_t *line;
  int pos;
  template <typename Op>
  void operator()(Op &op, int n) {
    int x = pos;
    for (; n > 0; n--) op.put(loadRaw<Op::SRC>(line, x++));
    pos = x;
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
    int x = pos;
    uint32_t rm = rem;
    for (; n > 0; n--) {
      op.put(loadRaw<Op::SRC>(line, x));
      // (without a branch: under magnification the carry comes every other
      // pixel or so, which no predictor guesses)
      rm += rStep;
      const bool carry = rm >= den;
      rm -= carry ? den : 0u;
      x += step + (carry ? dir : 0);
    }
    pos = x;
    rem = rm;
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
    int x = pos, l = left;
    uint32_t err = e;
    while (n > 0) {
      if (l == 0) {
        x += dir;
        l = (int)q;
        if (r > err) {
          l++;
          err += den - r;
        } else {
          err -= r;
        }
      }
      const int k = l < n ? l : n;
      op.run(loadRaw<Op::SRC>(line, x), k);
      l -= k;
      n -= k;
    }
    pos = x;
    left = l;
    e = err;
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
    int32_t uu = u, vv = v;
    for (; n > 0; n--) {
      op.put(
          loadRaw<Op::SRC>(pixels + (uint32_t)(vv >> 16) * stride, uu >> 16));
      uu += du;
      vv += dv;
    }
    u = uu;
    v = vv;
  }
};

// The same over a mask, addressed in bits
struct AffineBitWalk {
  const uint8_t *bits;
  uint32_t base, stride;
  int32_t u, v, du, dv;
  template <typename Op>
  void operator()(Op &op, int n) {
    int32_t uu = u, vv = v;
    for (; n > 0; n--) {
      op.put(arch::loadBit(
          bits, base + (uint32_t)(vv >> 16) * stride + (uint32_t)(uu >> 16)));
      uu += du;
      vv += dv;
    }
    u = uu;
    v = vv;
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
  // target pixel 0 and the source pixel of k = 0, for seek()
  int origin, base;
  uint32_t sn;

  // Start at target pixel `x` instead (one division, two with runs)
  void seek(int x) {
    const int t0 = x - origin;
    const uint32_t n0 = (2u * t0 + 1u) * sn;
    const int k0 = (int)(n0 / den);
    rem = n0 % den;
    pos = base + dir * k0;
    if (runs) {
      // Run k starts at s(k) = ceil(a(k) / 2sn), a(k) = 2k dn - sn; e(k) =
      // s(k) 2sn - a(k) decides whether run k is q or q + 1 long
      const uint32_t a = (uint32_t)(k0 + 1) * den - sn;
      const uint32_t s1 = (a + rden - 1u) / rden;
      run0 = (int)s1 - t0;
      e = s1 * rden - a;
    }
    start = x;
  }
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
  m.count = t1 - t0;
  m.dir = mirror ? -1 : 1;
  m.den = 2u * dn;
  m.step = m.dir * (sn / dn);
  m.rStep = 2u * (uint32_t)(sn % dn);
  m.runs = sn < dn;
  if (m.runs) {
    m.q = (uint32_t)(dn / sn);
    m.r = 2u * (uint32_t)(dn % sn);
    m.rden = 2u * sn;
  }
  m.origin = dstPos;
  m.base = mirror ? srcPos + sn - 1 : srcPos;
  m.sn = (uint32_t)sn;
  m.seek(dstPos + t0);
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

// The coefficients of a mapping from target pixels to the source, for
// PolygonRows: the source point (absolute) of the center of target pixel
// (x, y) is u = A x + C y + E, v = B x + D y + F. In float, or as 32.32
// with SHAPOGFX2D_FIXED_POINT (each build fills and reads its own).
struct PolygonMapping {
  float A, B, C, D, E, F;
  int64_t aQ, bQ, cQ, dQ, eQ, fQ;
};

#if SHAPOGFX2D_FIXED_POINT

// A convex polygon of the source as bounds on the target column per target
// row (the build for a core without an FPU, which spent more on the float
// version below than on the pixels the polygon saved: the same bounds from
// the 32.32 coefficients, as 44.20 fixed point). An edge is the half-plane
// of the source points on its inner side: for the pixel k columns right of
// column xRef and dy rows below row yRef that is
//   g k >= h0 + hy dy
// so an edge whose inward normal has a component along the row (g != 0) is a
// lower (g > 0) or an upper bound on k that changes linearly with the row,
// and one across the rows only narrows the row range once. The column is at
// least the highest lower bound and at most the lowest upper bound; of a
// convex polygon those are the edges of its left and of its right side, each
// the tightest over the rows it spans, in the order of their steps. Rows
// taken from top to bottom step through them: a row is four 64-bit
// additions.
struct PolygonRows {
  static constexpr int FRAC = 20;
  // An edge steeper than DX_MAX columns per row (or across the rows) limits
  // the rows instead: it crosses any target within a row. A bound farther
  // than X_MAX from xRef on row yRef never reaches the target.
  static constexpr int DX_BITS = 24, X_BITS = 40;
  static constexpr int ROW_LIM = 1 << 20;
  struct Bound {
    int64_t x0, dx;
  };
  Bound lo[Graphics2D::IMAGE_POLYGON_MAX], hi[Graphics2D::IMAGE_POLYGON_MAX];
  int loCount = 0, hiCount = 0;
  bool enabled = false;  // false: no polygon (every pixel passes)
  int xRef = 0, yRef = 0, width = 0;
  int top = 0, bottom = -1;  // the rows left by init()
  // The walk: the tightest bounds on row lastY and the ones after them
  int loAt = 0, hiAt = 0, lastY = 0;
  bool walking = false;
  int64_t xLo = 0, xLoNext = 0, xHi = 0, xHiNext = 0;

  // The edges in the order of their steps: `n` bounds of `b`, for s = 1 the
  // lower ones (ascending steps), for s = -1 the upper ones (descending).
  // Of parallel ones the tightest is kept. Returns how many are left.
  static int order(Bound *b, int n, int s) {
    // (insertion sort: n is at most IMAGE_POLYGON_MAX)
    for (int i = 1; i < n; i++) {
      const Bound v = b[i];
      int j = i;
      for (; j > 0; j--) {
        const Bound &u = b[j - 1];
        if (s > 0 ? (u.dx < v.dx || (u.dx == v.dx && u.x0 >= v.x0))
                  : (u.dx > v.dx || (u.dx == v.dx && u.x0 <= v.x0)))
          break;
        b[j] = u;
      }
      b[j] = v;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
      if (m > 0 && b[i].dx == b[m - 1].dx) continue;
      b[m++] = b[i];
    }
    return m;
  }

  // An edge that limits the rows: dy <= t (down) or dy >= t
  void limitRows(bool down, int t, int &y0, int &y1) const {
    t = clampInt(-ROW_LIM, ROW_LIM, t);
    if (down) {
      y1 = std::min(y1, yRef + t);
    } else {
      y0 = std::max(y0, yRef + t);
    }
  }

  // An edge as a bound; false if it leaves no pixel
  bool addBound(bool lower, int64_t x0, int64_t dx) {
    const int64_t xMax = (int64_t)1 << (X_BITS + FRAC);
    if (x0 > xMax || x0 < -xMax) {
      // Never within the target: all its pixels pass, or none
      return lower ? x0 < 0 : x0 > 0;
    }
    if (lower) {
      lo[loCount++] = {x0, dx};
    } else {
      hi[hiCount++] = {x0, dx};
    }
    return true;
  }

  // The edge with the inward normal (nx, ny): (u, v) are the source point
  // of the reference pixel less a vertex of the edge, shifted right by
  // `su` from 32.32. False if it leaves no pixel.
  bool edge(const PolygonMapping &m, int64_t u, int64_t v, int su, int nx,
            int ny, int &y0, int &y1) {
    const int64_t g = nx * m.aQ + ny * m.bQ;        // 32.32, below 2^61
    const int64_t hy = -(nx * m.cQ + ny * m.dQ);
    const int64_t h0 = -(nx * u + ny * v);          // 32.32 >> su
    const uint64_t ag = g < 0 ? 0 - (uint64_t)g : (uint64_t)g;
    const uint64_t ahy = hy < 0 ? 0 - (uint64_t)hy : (uint64_t)hy;
    if (ag <= (ahy >> DX_BITS)) {
      // (g is below 2^37 here, so g (width - 1) fits)
      const int64_t gw = g * (width - 1);
      const int64_t num = ((gw > 0 ? gw : 0) >> su) - h0;
      // The magnitudes to 31 bits for the division
      const uint64_t an = num < 0 ? 0 - (uint64_t)num : (uint64_t)num;
      const uint64_t ad = ahy >> su;
      const int sh = std::max(0, 33 - clz64(an | ad | 1));
      const int n = (int)(an >> sh), d = (int)(ad >> sh);
      // (hy nothing next to the rest: the edge does not move with the row)
      if (d == 0) return num >= 0;
      if (hy > 0) {
        limitRows(true, floorDiv(num < 0 ? -n : n, d), y0, y1);
      } else {
        limitRows(false, ceilDiv(num < 0 ? -n : n, -d), y0, y1);
      }
      return true;
    }
    // x0 = h0 / g and dx = hy / g through the reciprocal of |g|
    int sh;
    const uint32_t r = reciprocal64(ag, sh);
    const bool lower = g > 0;
    const int64_t x0 = mulShiftSat(lower ? h0 : -h0, r, 62 + sh - FRAC - su);
    const int64_t dx = mulShiftSat(lower ? hy : -hy, r, 62 + sh - FRAC);
    return addBound(lower, x0, dx);
  }

  // `polygon`: `count` (3 or more) vertices relative to the top-left corner
  // of `s`; columns [x0, x1] and rows [y0, y1] are the ones to narrow (the
  // rows are narrowed here by the edges across them). False for a
  // degenerate polygon or too many vertices, or if no row is left (nothing
  // is drawn).
  bool init(const int16_t *polygon, int count, const Rect &s,
            const PolygonMapping &m, int x0, int x1, int &y0, int &y1) {
    loCount = hiCount = 0;
    enabled = false;
    walking = false;
    xRef = x0;
    yRef = y0;
    width = x1 - x0 + 1;
    if (count > Graphics2D::IMAGE_POLYGON_MAX || width <= 0) return false;
    // The winding, from the doubled signed area (in 64 bits)
    int64_t area2 = 0;
    for (int i = 0; i < count; i++) {
      const int j = (i + 1) % count;
      area2 += (int64_t)polygon[2 * i] * polygon[2 * j + 1] -
               (int64_t)polygon[2 * j] * polygon[2 * i + 1];
    }
    if (area2 == 0) return false;
    const int sign = area2 > 0 ? 1 : -1;

    // Steps beyond the affine walk's, or a source too far away, would not
    // fit: the polygon is left out
    const int64_t stepMax = (int64_t)AFFINE_STEP_MAX << 32;
    const int64_t farMax = (int64_t)1 << 60;
    for (const int64_t c : {m.aQ, m.bQ, m.cQ, m.dQ}) {
      if (c > stepMax || c < -stepMax) return true;
    }
    if (m.eQ > farMax || m.eQ < -farMax || m.fQ > farMax || m.fQ < -farMax ||
        s.x > (1 << 20) || s.x < -(1 << 20) || s.y > (1 << 20) ||
        s.y < -(1 << 20))
      return true;
    // The source point of the reference pixel; less a vertex it is shifted
    // to 44 bits for the products with the normals
    const int64_t uQ = m.eQ + m.aQ * xRef + m.cQ * yRef;
    const int64_t vQ = m.fQ + m.bQ * xRef + m.dQ * yRef;
    const uint64_t au = uQ < 0 ? 0 - (uint64_t)uQ : (uint64_t)uQ;
    const uint64_t av = vQ < 0 ? 0 - (uint64_t)vQ : (uint64_t)vQ;
    const uint64_t vertexMax =
        (uint64_t)(std::abs(s.x) + std::abs(s.y) + 32768) << 32;
    const int su = std::max(0, 21 - clz64(au | av | vertexMax));
    const int64_t one = (int64_t)1 << 32;
    for (int i = 0; i < count; i++) {
      const int j = (i + 1) % count;
      const int px = polygon[2 * i], py = polygon[2 * i + 1];
      const int ex = polygon[2 * j] - px, ey = polygon[2 * j + 1] - py;
      if (ex == 0 && ey == 0) continue;  // a repeated vertex
      // An inward normal of the edge (for a positive area, its left side)
      if (!edge(m, (uQ - (px + s.x) * one) >> su,
                (vQ - (py + s.y) * one) >> su, su, -ey * sign, ex * sign, y0,
                y1))
        return false;
    }
    top = y0, bottom = y1;
    loCount = order(lo, loCount, 1);
    hiCount = order(hi, hiCount, -1);
    enabled = true;
    return y0 <= y1;
  }

  static int64_t at(const Bound &b, int dy) { return b.x0 + b.dx * dy; }

  // The bounds of row y by their definition (the first row, or one out of
  // order)
  __attribute__((noinline)) void seek(int y) {
    const int dy = y - yRef;
    if (!walking || y < lastY) loAt = hiAt = 0;
    walking = true;
    if (loCount > 0) {
      xLo = at(lo[loAt], dy);
      while (loAt + 1 < loCount) {
        const int64_t v = at(lo[loAt + 1], dy);
        if (v < xLo) {
          xLoNext = v;
          break;
        }
        xLo = v;
        loAt++;
      }
    }
    if (hiCount > 0) {
      xHi = at(hi[hiAt], dy);
      while (hiAt + 1 < hiCount) {
        const int64_t v = at(hi[hiAt + 1], dy);
        if (v > xHi) {
          xHiNext = v;
          break;
        }
        xHi = v;
        hiAt++;
      }
    }
  }

  // Narrow [k0, k1] (columns relative to xr) to those inside the polygon on
  // row y. Rows from top to bottom step the bounds; any other order works
  // through seek().
  bool narrow(int y, int xr, int &k0, int &k1) {
    if (!enabled) return true;
    if (!walking || y != lastY + 1) {
      seek(y);
    } else {
      // The next row: step the bounds, and the ones that follow them in
      // the order, which take over where they overtake
      const int dy = y - yRef;
      if (loCount > 0) {
        xLo += lo[loAt].dx;
        if (loAt + 1 < loCount) {
          xLoNext += lo[loAt + 1].dx;
          while (xLoNext >= xLo) {
            xLo = xLoNext;
            if (++loAt + 1 >= loCount) break;
            xLoNext = at(lo[loAt + 1], dy);
          }
        }
      }
      if (hiCount > 0) {
        xHi += hi[hiAt].dx;
        if (hiAt + 1 < hiCount) {
          xHiNext += hi[hiAt + 1].dx;
          while (xHiNext <= xHi) {
            xHi = xHiNext;
            if (++hiAt + 1 >= hiCount) break;
            xHiNext = at(hi[hiAt + 1], dy);
          }
        }
      }
    }
    lastY = y;
    const int off = xRef - xr;
    if (loCount > 0) {
      // ceil
      const int64_t k = ((xLo + ((1 << FRAC) - 1)) >> FRAC) + off;
      if (k > k0) k0 = k > k1 ? k1 + 1 : (int)k;
    }
    if (hiCount > 0) {
      const int64_t k = (xHi >> FRAC) + off;
      if (k < k1) k1 = k < k0 ? k0 - 1 : (int)k;
    }
    return k0 <= k1;
  }

  // The n columns of row y from x narrowed to the polygon (and false for a
  // row outside it)
  bool span(int y, int &x, int &n) {
    if (!enabled) return true;
    if (y < top || y > bottom) return false;
    int k0 = x, k1 = x + n - 1;
    if (!narrow(y, 0, k0, k1)) return false;
    x = k0;
    n = k1 - k0 + 1;
    return true;
  }
};

#else  // SHAPOGFX2D_FIXED_POINT

// A convex polygon of the source as bounds on the target column x per target
// row, in float (the build for a core with an FPU): an edge whose inward
// normal has a component along the row is x >= (or <=) a bound that changes
// linearly with the row (x0 at row yRef, dx per row); one across the rows
// only narrows the row range once. The column is at least the highest lower
// bound and at most the lowest upper bound: init() keeps of each kind the
// bounds that are the tightest on some row (their envelope), ordered by the
// rows where they are, so that rows taken from top to bottom step through
// them and a row costs two of them.
struct PolygonRows {
  struct Bound {
    float x0, dx;
  };
  Bound lo[Graphics2D::IMAGE_POLYGON_MAX], hi[Graphics2D::IMAGE_POLYGON_MAX];
  int loCount = 0, hiCount = 0;
  int yRef = 0;
  int top = 0, bottom = -1;  // the rows left by init()
  int loAt = 0, hiAt = 0, lastY = 0;  // the tightest bounds on row lastY

  // Reduce the n bounds of `b` to their envelope: for s = 1 the highest
  // (lower bounds), for s = -1 the lowest (upper bounds), ordered by s dx.
  // Returns how many are left.
  static int envelope(Bound *b, int n, float s) {
    // By s dx ascending, then by s x0 descending (insertion sort: n is at
    // most IMAGE_POLYGON_MAX)
    for (int i = 1; i < n; i++) {
      const Bound v = b[i];
      int j = i;
      for (; j > 0; j--) {
        const Bound &u = b[j - 1];
        if (s * u.dx < s * v.dx || (u.dx == v.dx && s * u.x0 >= s * v.x0))
          break;
        b[j] = u;
      }
      b[j] = v;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
      // A parallel one behind the previous never is the tightest
      if (m > 0 && b[i].dx == b[m - 1].dx) continue;
      // The last one is not either if the new one overtakes the one before
      // it no later than the last one does: with c the s x0 and k the s dx
      // of the three, (c1 - c3) / (k3 - k1) <= (c1 - c2) / (k2 - k1)
      while (m >= 2) {
        const float c1 = s * b[m - 2].x0, k1 = s * b[m - 2].dx;
        const float c2 = s * b[m - 1].x0, k2 = s * b[m - 1].dx;
        const float c3 = s * b[i].x0, k3 = s * b[i].dx;
        if ((c1 - c3) * (k2 - k1) > (c1 - c2) * (k3 - k1)) break;
        m--;
      }
      b[m++] = b[i];
    }
    return m;
  }

  // `polygon`: `count` (3 or more) vertices relative to the top-left corner
  // of `s`; [y0, y1] the rows to narrow. False for a degenerate polygon or
  // too many vertices, or if no row is left (nothing is drawn).
  bool init(const int16_t *polygon, int count, const Rect &s,
            const PolygonMapping &m, int, int, int &y0, int &y1) {
    const float A = m.A, B = m.B, C = m.C, D = m.D, E = m.E, F = m.F;
    loCount = hiCount = 0;
    yRef = y0;
    if (count > Graphics2D::IMAGE_POLYGON_MAX) return false;
    vec2f poly[Graphics2D::IMAGE_POLYGON_MAX];
    for (int i = 0; i < count; i++)
      poly[i] = {(float)polygon[2 * i], (float)polygon[2 * i + 1]};
    // The winding, from the doubled signed area
    float area2 = 0.0f;
    for (int i = 0; i < count; i++) {
      const int j = (i + 1) % count;
      area2 += poly[i].x * poly[j].y - poly[j].x * poly[i].y;
    }
    if (!(area2 != 0.0f)) return false;
    const float sign = area2 > 0 ? 1.0f : -1.0f;
    const float lim = 1e6f;
    for (int i = 0; i < count; i++) {
      const int j = (i + 1) % count;
      const float ex = poly[j].x - poly[i].x;
      const float ey = poly[j].y - poly[i].y;
      if (ex == 0.0f && ey == 0.0f) continue;  // a repeated vertex
      // An inward normal of the edge (for a positive area, its left side):
      // inside is nx u + ny v >= c, in absolute source coordinates (the
      // bounds below do not depend on its length)
      const float nx = -ey * sign, ny = ex * sign;
      const float c = nx * (poly[i].x + (float)s.x) +
                      ny * (poly[i].y + (float)s.y);
      // With u = A x + C y + E, v = B x + D y + F that is
      // g x >= h0 + hy y
      const float g = nx * A + ny * B;
      const float h0 = c - nx * E - ny * F, hy = -(nx * C + ny * D);
      if (g == 0.0f) {
        // Across the rows: hy y <= -h0
        if (hy > 0.0f) {
          y1 = std::min(y1, (int)std::floor(std::clamp(-h0 / hy, -lim, lim)));
        } else if (hy < 0.0f) {
          y0 = std::max(y0, (int)std::ceil(std::clamp(-h0 / hy, -lim, lim)));
        } else if (h0 > 0.0f) {
          return false;
        }
        continue;
      }
      // (clamped far beyond any target, so that envelope() multiplies finite
      // numbers)
      const float ig = 1.0f / g, big = 1e12f;
      const Bound b = {std::clamp((h0 + hy * (float)yRef) * ig, -big, big),
                       std::clamp(hy * ig, -big, big)};
      if (g > 0.0f) {
        lo[loCount++] = b;
      } else {
        hi[hiCount++] = b;
      }
    }
    top = y0, bottom = y1;
    loCount = envelope(lo, loCount, 1.0f);
    hiCount = envelope(hi, hiCount, -1.0f);
    loAt = hiAt = 0;
    lastY = yRef;
    return y0 <= y1;
  }

  // Narrow [k0, k1] (columns relative to xr) to those inside the polygon on
  // row y: the tightest of the lower and of the upper bounds, stepping to
  // the next of the envelope where it overtakes (rows from top to bottom;
  // a row above the previous one starts over). In float; the callers keep
  // the walks within the image whatever this rounds to.
  bool narrow(int y, int xr, int &k0, int &k1) {
    if (y < lastY) loAt = hiAt = 0;
    lastY = y;
    const float dy = (float)(y - yRef);
    float xlo = -1e9f, xhi = 1e9f;
    if (loCount > 0) {
      xlo = lo[loAt].x0 + lo[loAt].dx * dy;
      while (loAt + 1 < loCount) {
        const float v = lo[loAt + 1].x0 + lo[loAt + 1].dx * dy;
        if (v < xlo) break;
        xlo = v;
        loAt++;
      }
    }
    if (hiCount > 0) {
      xhi = hi[hiAt].x0 + hi[hiAt].dx * dy;
      while (hiAt + 1 < hiCount) {
        const float v = hi[hiAt + 1].x0 + hi[hiAt + 1].dx * dy;
        if (v > xhi) break;
        xhi = v;
        hiAt++;
      }
    }
    // (ceil and floor by truncation: no library calls on a row)
    const float lim = 1e6f;
    const float fl = std::clamp(xlo - (float)xr, -lim, lim);
    const float fh = std::clamp(xhi - (float)xr, -lim, lim);
    const int il = (int)fl, ih = (int)fh;
    k0 = std::max(k0, il + (fl > (float)il));
    k1 = std::min(k1, ih - (fh < (float)ih));
    return k0 <= k1;
  }

  // The n columns of row y from x narrowed to the polygon (and false for a
  // row outside it)
  bool span(int y, int &x, int &n) {
    if (y < top || y > bottom) return false;
    int k0 = x, k1 = x + n - 1;
    if (!narrow(y, 0, k0, k1)) return false;
    x = k0;
    n = k1 - k0 + 1;
    return true;
  }
};

#endif  // SHAPOGFX2D_FIXED_POINT

// The rows of a transformed image: `m` maps source coordinates relative to
// the top-left corner of `s` to the target, `in` is the part of the source
// that may be read. The transform is inverted once; when the source
// coordinates of the whole bounding box fit 16.16 (anything but extreme
// scales), the middle column of the box is the reference of every row, and
// its coordinates step down the rows in 32.32 accumulators: a row is two
// 64-bit additions and an exact clip in 16.16 fixed point, so that the
// per-pixel walk (u += du, v += dv) never leaves `in`. Otherwise a float
// estimate picks the reference column of each row inside the image's
// footprint, and the row is clipped from there in the same way.
//
// With SHAPOGFX2D_FIXED_POINT a transform that fits AffineQ is set up in
// integers as well (bounding box, inverse): a core without an FPU spent
// more on the float setup and the float rows than on the pixels they led to.
struct AffineRows {
  int32_t du = 0, dv = 0;
  // Source coordinates of the center of target pixel (x, y), absolute:
  // u = A x + C y + E, v = B x + D y + F, as 32.32 (haveQ) ...
  bool haveQ = false;
  int64_t aQ = 0, bQ = 0, cQ = 0, dQ = 0, eQ = 0, fQ = 0;
  // ... and in float (haveF: the float rows and the polygon)
  bool haveF = false;
  float A = 0, B = 0, C = 0, D = 0, E = 0, F = 0;
  float iA = 0, iB = 0;  // the float rows only
  // Fixed-point rows: (u, v) of column xRef on row by0, 32.32
  bool fixedRows = false;
  int64_t uRowQ = 0, vRowQ = 0;
  int xRef = 0;
  int bx0, bx1, by0, by1;
  int32_t uLo, uHi, vLo, vHi;
  Rect in;
  // The convex polygon, if any (see PolygonRows)
  PolygonRows poly;
  bool hasPolygon = false;

  static constexpr int64_t STEP_Q = (int64_t)AFFINE_STEP_MAX << 32;
  // How far from the image `s` may start for the integer setup
  static constexpr int SRC_NEAR = 1 << 16;

  // The transform is `mq` if given (SHAPOGFX2D_FIXED_POINT only), else `mf`
  bool init(const affine2f *mf, const AffineQ *mq, const Rect &s,
            const Rect &readable, const Rect &clip,
            const int16_t *polygon = nullptr, int count = 0) {
    in = readable;
    if (in.isEmpty() || in.right() > AFFINE_SIZE_MAX ||
        in.bottom() > AFFINE_SIZE_MAX || in.x < 0 || in.y < 0)
      return false;
    AffineQ q;
    bool fits = FIXED_POINT && s.x >= -SRC_NEAR && s.x <= SRC_NEAR &&
                s.y >= -SRC_NEAR && s.y <= SRC_NEAR;
    if (fits) {
      if (mq) {
        q = *mq;
      } else {
        fits = AffineQ::from(*mf, q);
      }
    }
    if (fits) {
      if (!setupFixed(q, s, clip)) return false;
    } else {
      affine2f f;
      if (mq) {
        f = mq->toFloat();
        mf = &f;
      }
      if (!setupFloat(*mf, s, clip)) return false;
    }
    if (haveQ) {
      du = (int32_t)((aQ + 0x8000) >> 16);
      dv = (int32_t)((bQ + 0x8000) >> 16);
    } else {
      du = roundToInt(A * 65536.0f);
      dv = roundToInt(B * 65536.0f);
    }
    uLo = in.x << 16, uHi = (in.right() << 16) - 1;
    vLo = in.y << 16, vHi = (in.bottom() << 16) - 1;
    if (polygon && count >= 3) {
      // (in float with an FPU; without the 32.32 coefficients, which only a
      // source far beyond the target leaves out, the polygon is not applied)
      if (!FIXED_POINT || haveQ) {
        hasPolygon = true;
        const PolygonMapping pm = {A, B, C, D, E, F, aQ, bQ, cQ, dQ, eQ, fQ};
        if (!poly.init(polygon, count, s, pm, bx0, bx1, by0, by1))
          return false;
      }
    }
    // (after the polygon, which may have dropped rows from the box)
    initFixedRows();
    if (!fixedRows) {
      needFloat();
      iA = A != 0.0f ? 1.0f / A : 0.0f;
      iB = B != 0.0f ? 1.0f / B : 0.0f;
    }
    return true;
  }

  // The bounding box of the drawn part (a pixel wider) and the inverse, in
  // integers
  bool setupFixed(const AffineQ &q, const Rect &s, const Rect &clip) {
    // The corner of `in` and its two edges on the target, 40.24
    const int x0 = in.x - s.x, y0 = in.y - s.y;
    const int64_t ox =
        (int64_t)q.a * x0 + (int64_t)q.c * y0 + (int64_t)q.tx * 256;
    const int64_t oy =
        (int64_t)q.b * x0 + (int64_t)q.d * y0 + (int64_t)q.ty * 256;
    const int64_t ax = (int64_t)q.a * in.width, cx = (int64_t)q.c * in.height;
    const int64_t bx = (int64_t)q.b * in.width, dx = (int64_t)q.d * in.height;
    const int64_t zero = 0, up = (1 << 24) - 1;
    const int64_t xl = ox + std::min(ax, zero) + std::min(cx, zero);
    const int64_t xh = ox + std::max(ax, zero) + std::max(cx, zero);
    const int64_t yl = oy + std::min(bx, zero) + std::min(dx, zero);
    const int64_t yh = oy + std::max(bx, zero) + std::max(dx, zero);
    bx0 = std::max(clip.x, (int)(xl >> 24) - 1);
    bx1 = std::min(clip.right() - 1, (int)((xh + up) >> 24) + 1);
    by0 = std::max(clip.y, (int)(yl >> 24) - 1);
    by1 = std::min(clip.bottom() - 1, (int)((yh + up) >> 24) + 1);
    if (bx0 > bx1 || by0 > by1) return false;

    // The inverse of the linear part: each entry times the reciprocal of
    // the determinant (16.48; 1 / det = r 2^-(62 + sh)), shifted to 32.32
    const int64_t det = (int64_t)q.a * q.d - (int64_t)q.b * q.c;
    if (det == 0) return false;
    const uint64_t ad = det < 0 ? 0 - (uint64_t)det : (uint64_t)det;
    int sh;
    const int64_t r = (int64_t)reciprocal64(ad, sh);
    const int rs = 6 + sh;  // -24 .. 38
    auto entry = [&](int32_t x, int64_t &out) {
      int64_t p = (int64_t)x * r;
      if (det < 0) p = -p;
      if (rs > 0) {
        p = (p + ((int64_t)1 << (rs - 1))) >> rs;
      } else if (rs < 0) {
        const int64_t lim = STEP_Q >> -rs;
        if (p > lim || p < -lim) return false;
        p *= (int64_t)1 << -rs;
      }
      // (steps beyond AFFINE_STEP_MAX are not drawn)
      if (p > STEP_Q || p < -STEP_Q) return false;
      out = p;
      return true;
    };
    if (!entry(q.d, aQ) || !entry(-q.b, bQ) || !entry(-q.c, cQ) ||
        !entry(q.a, dQ))
      return false;
    // The translation, the origin of `s` and the half pixel to the center
    eQ = -(mulShift16(aQ, q.tx) + mulShift16(cQ, q.ty)) +
         (int64_t)s.x * ((int64_t)1 << 32) + (aQ + cQ) / 2;
    fQ = -(mulShift16(bQ, q.tx) + mulShift16(dQ, q.ty)) +
         (int64_t)s.y * ((int64_t)1 << 32) + (bQ + dQ) / 2;
    haveQ = true;
    return true;
  }

  // The same in float (with SHAPOGFX2D_FIXED_POINT: for a transform beyond
  // AffineQ, the rare case, kept out of line)
#if SHAPOGFX2D_FIXED_POINT
  __attribute__((noinline))
#endif
  bool setupFloat(const affine2f &m, const Rect &s, const Rect &clip) {
    affine2f inv;
    if (!m.invert(inv)) return false;
    A = inv.a, B = inv.b, C = inv.c, D = inv.d;
    if (!(std::fabs(A) <= AFFINE_STEP_MAX && std::fabs(B) <= AFFINE_STEP_MAX &&
          std::fabs(C) <= AFFINE_STEP_MAX && std::fabs(D) <= AFFINE_STEP_MAX))
      return false;  // also NaN
    E = inv.tx + (float)s.x + 0.5f * (A + C);
    F = inv.ty + (float)s.y + 0.5f * (B + D);
    haveF = true;
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
    const float limE = (float)(1 << 27);
    if (std::fabs(E) < limE && std::fabs(F) < limE) {  // (false for NaN)
      aQ = toQ32(A), bQ = toQ32(B), cQ = toQ32(C), dQ = toQ32(D);
      eQ = toQ32(E), fQ = toQ32(F);
      haveQ = true;
    }
    return true;
  }

  // A float as 32.32, exactly (|f| < 2^28)
  static int64_t toQ32(float f) { return floatToFixed64(f, 32); }

  // The float coefficients after the integer setup, for the float rows
  // (the float setup has them)
  __attribute__((noinline)) static float fromQ32(int64_t v) {
    return fixed64ToFloat(v, 32);
  }
  __attribute__((noinline)) void needFloat() {
    if (!FIXED_POINT || haveF) return;
    A = fromQ32(aQ), B = fromQ32(bQ), C = fromQ32(cQ);
    D = fromQ32(dQ), E = fromQ32(eQ), F = fromQ32(fQ);
    haveF = true;
  }

  // Set up the fixed-point rows if u and v stay within 16.16 over the whole
  // bounding box (they are affine, so its corners are the extremes): then
  // the offsets narrowSpan() divides fit 32 bits from any column of the box
  void initFixedRows() {
    fixedRows = false;
    if (!haveQ) return;
    // (|A| .. |D| are at most AFFINE_STEP_MAX, |E| and |F| below 2^29 and
    // the box lies within the coordinate range, so the sums below stay
    // within 62 bits)
    const int64_t lim = (int64_t)(AFFINE_SIZE_MAX - 256) << 32;
    for (int i = 0; i < 4; i++) {
      const int x = (i & 1) ? bx1 : bx0, y = (i & 2) ? by1 : by0;
      const int64_t u = eQ + aQ * x + cQ * y, v = fQ + bQ * x + dQ * y;
      if (u <= -lim || u >= lim || v <= -lim || v >= lim) return;
    }
    // The steps along a row are rounded to 16.16: from the middle column
    // their error is halved
    xRef = bx0 + (bx1 - bx0) / 2;
    uRowQ = eQ + aQ * xRef + cQ * by0;
    vRowQ = fQ + bQ * xRef + dQ * by0;
    fixedRows = true;
  }

  // Row y by the float estimate: its reference column xr and (u, v) there,
  // 16.16; false for a row off the image. One copy for all the callers (it
  // is the rare path)
  __attribute__((noinline)) bool floatRow(int y, int &xr, int32_t &u,
                                          int32_t &v) const {
    const float margin = AFFINE_STEP_MAX * 2.0f;
    const float uc = C * (float)y + E, vc = D * (float)y + F;
    float ex0 = (float)bx0, ex1 = (float)bx1;
    if (!estimateSpan(uc, A, iA, (float)in.x, (float)in.right(), ex0, ex1) ||
        !estimateSpan(vc, B, iB, (float)in.y, (float)in.bottom(), ex0, ex1))
      return false;
    xr = clampInt(bx0, bx1, (int)std::floor((ex0 + ex1) * 0.5f));
    const float ur = A * (float)xr + uc, vr = B * (float)xr + vc;
    if (!(ur > (float)in.x - margin && ur < (float)in.right() + margin &&
          vr > (float)in.y - margin && vr < (float)in.bottom() + margin))
      return false;
    u = roundToInt(ur * 65536.0f);
    v = roundToInt(vr * 65536.0f);
    return true;
  }

  // fn(y, x, n, u, v): n pixels of row y from x, (u, v) at the first one
  template <typename Fn>
  void forEach(Fn &&fn) {
    // Row y from its reference column xr, where the source point is (u, v)
    auto row = [&](int y, int xr, int32_t u, int32_t v)
                   __attribute__((always_inline)) {
      int k0 = bx0 - xr, k1 = bx1 - xr;
      if (!narrowSpan(u, du, uLo, uHi, k0, k1) ||
          !narrowSpan(v, dv, vLo, vHi, k0, k1))
        return;
      if (hasPolygon && !poly.narrow(y, xr, k0, k1)) return;
      // The 16.16 offsets are formed in uint32 so that an intermediate wraps
      // instead of overflowing (the sums lie within the image)
      fn(y, xr + k0, k1 - k0 + 1,
         (int32_t)((uint32_t)u + (uint32_t)k0 * (uint32_t)du),
         (int32_t)((uint32_t)v + (uint32_t)k0 * (uint32_t)dv));
    };
    if (fixedRows) {
      int64_t uQ = uRowQ, vQ = vRowQ;
      for (int y = by0; y <= by1; y++, uQ += cQ, vQ += dQ) {
        // 32.32 to 16.16, rounded
        row(y, xRef, (int32_t)((uQ + 0x8000) >> 16),
            (int32_t)((vQ + 0x8000) >> 16));
      }
      return;
    }
    for (int y = by0; y <= by1; y++) {
      int xr;
      int32_t u, v;
      if (floatRow(y, xr, u, v)) row(y, xr, u, v);
    }
  }
};

// The transform of `g` with (dx, dy) as the origin (the product with a
// translation, less the multiplications by its ones and zeros)
affine2f matrixAt(const Graphics2D &g, int dx, int dy) {
  affine2f m = G2Impl::matrix(g);
  if (dx != 0 || dy != 0) {
    m.tx += m.a * (float)dx + m.c * (float)dy;
    m.ty += m.b * (float)dx + m.d * (float)dy;
  }
  return m;
}

// ---------------------------------------------------------------------------
// Image paths, in target pixels

// Plain: `s` (normalized) with its top-left corner at (x, y), no color key.
// SIL: the silhouette in the color `sil` (see ImageBlit::init). POLY: `poly`
// narrows each row to the pixels of a polygon (a separate instance, so that
// the rows without one stay as they are).
template <bool SIL, bool POLY = false>
void blitPlain(const Graphics2D &g, const Texture &img, int x, int y,
               const Rect &s, const Color *sil,
               PolygonRows *poly = nullptr) {
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
  // Row j: n pixels from target column x0, source column sx
  int x0, sx, n;
  auto row = [&](int j) {
    x0 = dst.x;
    n = dst.width;
    if constexpr (POLY) {
      if (!poly->span(dst.y + j, x0, n)) return false;
    }
    sx = src.x + (x0 - dst.x);
    return true;
  };

  if constexpr (SIL) {
    for (int j = 0; j < dst.height; j++) {
      if (!row(j)) continue;
      uint8_t *dl = target.linePtr(dst.y + j);
      const uint8_t *sl = img.linePtr(src.y + j);
      if (b.path == BlitPath::SILHOUETTE) {
        PlainWalk w{sl, sx};
        blitRowSilhouette(b, dl, x0, n, w);
      } else {
        // (through Color: a unit step of the scaled walker, so that no
        // path is compiled for this walker alone)
        StepWalk w{sl, sx, 1, 0, 0, 0, 1};
        blitRow(b, dl, x0, n, w);
      }
    }
    return;
  }

  // Fast path: plain copy of 16-bit formats
  if (b.path == BlitPath::COPY16) {
    for (int j = 0; j < dst.height; j++) {
      if (!row(j)) continue;
      std::memcpy(target.linePtr(dst.y + j) + (size_t)x0 * 2,
                  img.linePtr(src.y + j) + (size_t)sx * 2, (size_t)n * 2);
    }
    return;
  }
  if (b.mode == BlendMode::NONE) {
    for (int j = 0; j < dst.height; j++) {
      if (!row(j)) continue;
      copyRowFmt(target.format, img.format, target.linePtr(dst.y + j), x0,
                 img.linePtr(src.y + j), sx, n);
    }
    return;
  }
  // (blendRowFmt() tells whether it takes the pair by drawing a row; with
  // nothing to draw, an empty row asks)
  if (b.mode == BlendMode::ALPHA &&
      (!b.keyed || img.format == PixelFormat::ARGB4444) &&
      blendRowFmt(target.format, img.format, target.linePtr(dst.y), dst.x,
                  img.linePtr(src.y), src.x, 0, b.op64, b.alphaMul, b.key)) {
    for (int j = 0; j < dst.height; j++) {
      if (!row(j)) continue;
      blendRowFmt(target.format, img.format, target.linePtr(dst.y + j), x0,
                  img.linePtr(src.y + j), sx, n, b.op64, b.alphaMul, b.key);
    }
    return;
  }

  // Additive, and alpha blends of other format pairs: convert through Color
  // in chunks
  Color tmp[IMAGE_CHUNK];
  for (int j = 0; j < dst.height; j++) {
    if (!row(j)) continue;
    const uint8_t *sl = img.linePtr(src.y + j);
    uint8_t *dl = target.linePtr(dst.y + j);
    for (int i = 0; i < n; i += IMAGE_CHUNK) {
      const int c = std::min(IMAGE_CHUNK, n - i);
      readColorsFmt(img.format, sl, sx + i, c, tmp);
      writeColorsFmt(target.format, dl, x0 + i, c, tmp, b.write, b.op64);
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
// source pixel once. `polygon`, if any, keeps the pixels whose source point
// ((2t + 1) sw / 2dw on either axis) lies in it; a row whose span starts
// elsewhere than the previous one's seeks the walker there.
template <bool SIL>
void blitScaled(const Graphics2D &g, const Texture &img, const Rect &dst,
                const Rect &s, const Color *sil,
                const int16_t *polygon = nullptr, int count = 0) {
  PolygonRows pr;
  PolygonRows *poly = nullptr;
  const Rect d = dst.normalized();
  if (polygon && count >= 3) {
    const Rect clip = g.clipRect();
    if (d.intersect(clip).isEmpty() || s.isEmpty() || d.width > SCALE_MAX ||
        d.height > SCALE_MAX || s.width > SCALE_MAX || s.height > SCALE_MAX)
      return;
    // The source point of the center of target pixel (x, y): u = A x + E,
    // v = D y + F
    PolygonMapping pm = {};
    const int sx = s.x + (dst.width < 0 ? s.width : 0);
    const int sy = s.y + (dst.height < 0 ? s.height : 0);
    if (FIXED_POINT) {
      // 32.32: the integer part of the quotient and its fraction
      auto ratio = [](int num, int den) {
        const uint32_t n = (uint32_t)num, dn = (uint32_t)(den < 0 ? -den : den);
        const int64_t q = ((int64_t)(n / dn) << 32) + div64by32(n % dn, 0, dn);
        return den < 0 ? -q : q;
      };
      const int64_t one = (int64_t)1 << 32;
      pm.aQ = ratio(s.width, dst.width);
      pm.dQ = ratio(s.height, dst.height);
      pm.eQ = sx * one + pm.aQ / 2 - pm.aQ * d.x;
      pm.fQ = sy * one + pm.dQ / 2 - pm.dQ * d.y;
    } else {
      pm.A = (float)s.width / (float)dst.width;
      pm.D = (float)s.height / (float)dst.height;
      pm.E = (float)sx + (0.5f - (float)d.x) * pm.A;
      pm.F = (float)sy + (0.5f - (float)d.y) * pm.D;
    }
    int y0 = clip.y, y1 = clip.bottom() - 1;
    if (!pr.init(polygon, count, s, pm, clip.x, clip.right() - 1, y0, y1))
      return;
    poly = &pr;
  }
  ImageBlit b;
  if (!b.init<SIL>(g, img.format, sil)) return;
  if (dst.width == s.width && dst.height == s.height && !b.keyed) {
    if (poly)
      blitPlain<SIL, true>(g, img, dst.x, dst.y, s, sil, poly);
    else
      blitPlain<SIL>(g, img, dst.x, dst.y, s, sil);
    return;
  }
  ScaleAxis ax, ay;
  if (!mapAxes(dst, s, img.width, img.height, g.clipRect(), ax, ay)) return;
  const Surface &target = g.target();

  // Runs only from the magnification on where they pay (q is that of every
  // row)
  const uint32_t runsFrom = b.runsFrom();
  const bool runs = ax.runs && runsFrom != 0 && ax.q >= runsFrom;
  ax.runs = runs;  // (so that seek() skips the run state otherwise)
  // A plain copy repeats the previous target row for the same source row
  // (and the same span)
  const bool repeat = !SIL && b.write == WriteMode::COPY &&
                      bitsPerPixel(target.format) == 16;
  // The span of the row and its walker state: ax, or with a polygon rx
  // (within ax, whose walker seeks anywhere in it)
  ScaleAxis rx = ax;
  const ScaleAxis *cx = poly ? &rx : &ax;
  int row = ay.pos, prevRow = -1;
  uint32_t rem = ay.rem;
  const uint8_t *prevLine = nullptr;
  for (int j = 0; j < ay.count; j++) {
    const int y = ay.start + j;
    bool draw = true;
    if (poly) {
      int x = ax.start, n = ax.count;
      if (!poly->span(y, x, n)) {
        draw = false;
      } else if (x != rx.start || n != rx.count) {
        if (x != rx.start) rx.seek(x);
        rx.count = n;
        prevRow = -1;
      }
    }
    if (draw) {
      uint8_t *dl = target.linePtr(y);
      if (repeat && row == prevRow) {
        std::memcpy(dl + (size_t)cx->start * 2,
                    prevLine + (size_t)cx->start * 2, (size_t)cx->count * 2);
      } else {
        const uint8_t *sl = img.linePtr(row);
        if (runs) {
          RunWalk w{sl,     cx->pos, cx->dir, cx->run0, cx->e,
                    cx->q, cx->r,   cx->rden};
          blitRowOf<SIL>(b, dl, cx->start, cx->count, w);
        } else {
          StepWalk w{sl,       cx->pos, cx->step, cx->dir,
                     cx->rem, cx->rStep, cx->den};
          blitRowOf<SIL>(b, dl, cx->start, cx->count, w);
        }
      }
      prevRow = row;
      prevLine = dl;
    } else {
      prevRow = -1;
    }
    row += ay.step;
    rem += ay.rStep;
    if (rem >= ay.den) {
      rem -= ay.den;
      row += ay.dir;
    }
  }
}

// Transformed: `m` (or `mq`, see AffineRows::init) maps coordinates relative
// to the top-left corner of `s` (normalized) to the target; `polygon`, if
// any, clips the source further (see Graphics2D::drawImage)
template <bool SIL>
void blitAffine(const Graphics2D &g, const Texture &img, const affine2f *m,
                const AffineQ *mq, const Rect &s, const int16_t *polygon,
                int count, const Color *sil) {
  AffineRows ar;
  if (!ar.init(m, mq, s, s.intersect(Rect{0, 0, img.width, img.height}),
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
    G2Impl::drawImageAA(g, img, s, matrixAt(g, dx, dy), nullptr, 0, sil);
  } else if (kind == TransformKind::SCALE) {
    blitScaled<SIL>(g, img,
                    G2Impl::mapRectSigned(g, RectF{(float)dx, (float)dy,
                                                   (float)s.width,
                                                   (float)s.height}),
                    s, sil);
  } else {
    const affine2f m = matrixAt(g, dx, dy);
    blitAffine<SIL>(g, img, &m, nullptr, s, nullptr, 0, sil);
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
  // The paths of imageAt(), the plain one through the scaled one (which
  // takes it for an unscaled image); antialiasing smooths the polygon's
  // edges whatever the transform
  const TransformKind kind = G2Impl::kind(g);
  if (G2Impl::wantsAntialias(g)) {
    G2Impl::drawImageAA(g, img, s, matrixAt(g, dx, dy), polygon, count, sil);
  } else if (!TRANSFORM || kind <= TransformKind::TRANSLATE) {
    blitScaled<SIL>(g, img,
                    Rect{dx + G2Impl::offsetX(g), dy + G2Impl::offsetY(g),
                         s.width, s.height},
                    s, sil, polygon, count);
  } else if (kind == TransformKind::SCALE) {
    blitScaled<SIL>(g, img,
                    G2Impl::mapRectSigned(g, RectF{(float)dx, (float)dy,
                                                   (float)s.width,
                                                   (float)s.height}),
                    s, sil, polygon, count);
  } else {
    const affine2f m = matrixAt(g, dx, dy);
    blitAffine<SIL>(g, img, &m, nullptr, s, polygon, count, sil);
  }
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
    blitAffine<SIL>(g, img, &m, nullptr, s, nullptr, 0, sil);
  }
}

}  // namespace

void detail::G2Impl::drawImageFixed(Graphics2D &g, const Texture &img,
                                    const Rect &srcRect,
                                    const int16_t *polygon, int count,
                                    const AffineQ &q) {
  if (!g.hasTarget() || !img.pixels || !isFormatEnabled(img.format)) return;
  const Rect s = srcRect.normalized();
  if (s.isEmpty()) return;
  if (!polygon || count < 3) {
    polygon = nullptr;
    count = 0;
  }
  // The paths of imageAt() and imageInPolygon(), by the kind of the
  // transform as updateTransform() tells them apart
  constexpr int32_t EPS = 17, ONE = 1 << AffineQ::LIN;  // (1e-6 as 8.24)
  if (q.b <= -EPS || q.b >= EPS || q.c <= -EPS || q.c >= EPS) {
    blitAffine<false>(g, img, nullptr, &q, s, polygon, count, nullptr);
    return;
  }
  const bool scaled = q.a != ONE || q.d != ONE;
  const int x0 = snapQ16(q.tx), y0 = snapQ16(q.ty);
  Rect dst = {x0, y0, s.width, s.height};
  if (scaled) {
    // (mapRectSigned(): the far corner, snapped)
    dst.width = snapQ16((((int64_t)q.a * s.width) >> 8) + q.tx) - x0;
    dst.height = snapQ16((((int64_t)q.d * s.height) >> 8) + q.ty) - y0;
  }
  if (polygon || scaled || (COLOR_KEY && g.hasColorKey())) {
    blitScaled<false>(g, img, dst, s, nullptr, polygon, count);
  } else {
    blitPlain<false>(g, img, x0, y0, s, nullptr);
  }
}

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
  return arch::loadBit(m.bits, i) != 0;
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
  if (!ar.init(&mat, nullptr, src, src, ras.clip)) return;
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
               matrixAt(g, dx, dy),
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
      matrixAt(g, dx, dy);
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
