"""Image conversion shared by img2cpp and gltf2cpp.

Converts PIL images to the ShapoGFX pixel formats (memory layout as documented
in include/shapoco/gfx2d/pixel.hpp) and emits C++ array / Texture declarations.
"""

import os
import re

import numpy as np
from PIL import Image, ImageColor

FORMATS = ("rgb565_swapped", "rgb565", "argb4444", "rgb444", "gray1")
DITHERS = ("none", "diffusion", "pattern")

PIXEL_FORMAT_ENUM = {
    "rgb565_swapped": "RGB565_SWAPPED",
    "rgb565": "RGB565",
    "argb4444": "ARGB4444",
    "rgb444": "RGB444",
    "gray1": "GRAY1",
}

_BAYER4 = np.array(
    [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float64
)


def parse_key_color(s):
    """Web color string ("#FF8000", "orange", "F80") to an (R, G, B) tuple."""
    for candidate in (s, "#" + s):
        try:
            return ImageColor.getrgb(candidate)[:3]
        except ValueError:
            pass
    raise ValueError(f"invalid color: {s}")


def apply_key_color(image, key_color):
    """Make pixels equal to key_color fully transparent."""
    img = image.convert("RGBA")
    arr = np.array(img)
    kr, kg, kb = key_color
    mask = (arr[:, :, 0] == kr) & (arr[:, :, 1] == kg) & (arr[:, :, 2] == kb)
    arr[mask, 3] = 0
    return Image.fromarray(arr, "RGBA")


def quantize(channels, bits, dither):
    """Quantize float channels (h, w, c) in 0..255 to integers with `bits[c]` bits.

    dither: "none" (rounding), "diffusion" (Floyd-Steinberg, sequential),
    "pattern" (4x4 Bayer).
    """
    h, w, c = channels.shape
    maxq = np.array([(1 << b) - 1 for b in bits], dtype=np.float64)
    if dither == "none":
        q = np.clip(np.floor(channels * maxq / 255.0 + 0.5), 0, maxq)
        return q.astype(np.int32)
    if dither == "pattern":
        # Threshold offset of +-half a quantization step
        t = (_BAYER4 / 16.0 - 0.5)[np.arange(h)[:, None] % 4, np.arange(w)[None, :] % 4]
        step = 255.0 / maxq
        v = channels + t[:, :, None] * step[None, None, :]
        q = np.clip(np.floor(v * maxq / 255.0 + 0.5), 0, maxq)
        return q.astype(np.int32)
    if dither == "diffusion":
        buf = channels.astype(np.float64).copy()
        out = np.zeros((h, w, c), dtype=np.int32)
        for y in range(h):
            for x in range(w):
                v = np.clip(buf[y, x], 0.0, 255.0)
                q = np.clip(np.floor(v * maxq / 255.0 + 0.5), 0, maxq)
                out[y, x] = q
                err = v - q * 255.0 / maxq
                if x + 1 < w:
                    buf[y, x + 1] += err * (7 / 16)
                if y + 1 < h:
                    if x > 0:
                        buf[y + 1, x - 1] += err * (3 / 16)
                    buf[y + 1, x] += err * (5 / 16)
                    if x + 1 < w:
                        buf[y + 1, x + 1] += err * (1 / 16)
        return out
    raise ValueError(f"unknown dither mode: {dither}")


def convert(image, fmt, dither="none", key_color=None):
    """Convert a PIL image. Returns (width, height, stride, data, elem_type).

    data is the pixel memory as a list of integers of `elem_type`
    ("uint8_t" or "uint16_t"), row-major with `stride` bytes per row.
    """
    if fmt not in FORMATS:
        raise ValueError(f"unknown format: {fmt}")
    if key_color is not None:
        image = apply_key_color(image, key_color)
    w, h = image.size

    if fmt == "gray1":
        arr = np.array(image.convert("L"), dtype=np.float64)[:, :, None]
        q = quantize(arr, [1], dither)[:, :, 0]
        stride = (w + 7) // 8
        data = []
        for y in range(h):
            row = np.zeros(stride * 8, dtype=np.uint8)
            row[:w] = q[y]
            data.extend(int(v) for v in np.packbits(row))  # MSB first
        return w, h, stride, data, "uint8_t"

    if fmt == "argb4444":
        arr = np.array(image.convert("RGBA"), dtype=np.float64)
        q = quantize(arr, [4, 4, 4, 4], dither)
        px = (q[:, :, 3] << 12) | (q[:, :, 0] << 8) | (q[:, :, 1] << 4) | q[:, :, 2]
        return w, h, w * 2, [int(v) for v in px.flatten()], "uint16_t"

    if fmt == "rgb444":
        arr = np.array(image.convert("RGB"), dtype=np.float64)
        q = quantize(arr, [4, 4, 4], dither)
        stride = (w * 3 + 1) // 2
        data = []
        for y in range(h):
            row = bytearray(stride)
            for x in range(w):
                r, g, b = (int(v) for v in q[y, x])
                i = (x >> 1) * 3
                if x & 1 == 0:
                    row[i] = (r << 4) | g
                    row[i + 1] = (row[i + 1] & 0x0F) | (b << 4)
                else:
                    row[i + 1] = (row[i + 1] & 0xF0) | r
                    row[i + 2] = (g << 4) | b
            data.extend(row)
        return w, h, stride, data, "uint8_t"

    if fmt == "rgb565":
        # Native byte order: emitted as uint16_t values, which the compiler
        # lays out for the target
        arr = np.array(image.convert("RGB"), dtype=np.float64)
        q = quantize(arr, [5, 6, 5], dither)
        val = (q[:, :, 0] << 11) | (q[:, :, 1] << 5) | q[:, :, 2]
        return w, h, w * 2, [int(v) for v in val.flatten()], "uint16_t"

    # rgb565_swapped: the uint16_t value with its bytes swapped (relative to
    # the CPU's order, whatever the target's: emitted as uint16_t values)
    arr = np.array(image.convert("RGB"), dtype=np.float64)
    q = quantize(arr, [5, 6, 5], dither)
    val = (q[:, :, 0] << 11) | (q[:, :, 1] << 5) | q[:, :, 2]
    val = ((val & 0xFF) << 8) | ((val >> 8) & 0xFF)
    return w, h, w * 2, [int(v) for v in val.flatten()], "uint16_t"


def is_power_of_two(n):
    return n > 0 and (n & (n - 1)) == 0


def nearest_power_of_two(n):
    if n <= 1:
        return 1
    lo = 1 << (n.bit_length() - 1)
    hi = lo << 1
    return lo if (n - lo) <= (hi - n) else hi


def sanitize_identifier(name):
    """Turn an arbitrary string into a C++ identifier."""
    ident = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not ident or ident[0].isdigit():
        ident = "_" + ident
    return ident


def identifiers_from_path(path):
    """(UPPER_SNAKE, camelCase) identifiers derived from a file name."""
    base = sanitize_identifier(os.path.splitext(os.path.basename(path))[0])
    upper = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", base).upper()
    parts = [p for p in re.split(r"_+", base) if p]
    camel = (parts[0][0].lower() + parts[0][1:] + "".join(p[0].upper() + p[1:] for p in parts[1:])) if parts else base
    return upper, camel


def format_array(name, data, elem_type, indent="  ", per_line=None):
    """C++ definition of a static const array (aligned for 16-bit access)."""
    if per_line is None:
        per_line = 16 if elem_type == "uint8_t" else 8
    width = 2 if elem_type == "uint8_t" else 4
    lines = [f"alignas(4) static const {elem_type} {name}[] = {{"]
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        lines.append(indent + ", ".join(f"0x{v:0{width}X}" for v in chunk) + ",")
    lines.append("};")
    return lines


def format_texture(name, fmt, w, h, stride, data_name):
    """C++ definition of a shapoco::gfx2d::Texture referencing the array."""
    enum = PIXEL_FORMAT_ENUM[fmt]
    return [
        f"static const shapoco::gfx2d::Texture {name} = {{",
        f"  shapoco::gfx2d::PixelFormat::{enum}, {w}, {h}, {stride}, {data_name},",
        "};",
    ]
