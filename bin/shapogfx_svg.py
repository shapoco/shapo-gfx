"""SVG conversion shared by svg2cpp and its tests.

Reads an SVG document (xml.etree) and turns it into the static structures of
shapoco::gfx2d::vg (vg.hpp: paths, brushes, pictures) and, when it is
animated with SMIL, of shapoco::gfx2d::rig (rig.hpp: bones, slots, keyed
animations whose VECTOR attachments point to pictures). The document is
parsed into a tree of Node objects with their computed styles (section
"Document"), the geometry is converted to paths of MOVE / LINE / QUAD / CUBIC
/ CLOSE ops ("Paths"), and either flattened into one Picture ("Picture
compile") or split into bones and slots ("Rig compile"); "Emitter" writes the
C++ text and "Dump" a JSON summary for the tests.

Supported: svg, g, a, path, rect, circle, ellipse, line, polyline, polygon,
image, text / tspan, defs, use (expanded), symbol, linearGradient,
radialGradient (href inheritance), clipPath holding one rect, switch (first
child), style sheets with simple selectors (type, .class, #id, *, comma lists,
descendant combinator), presentation attributes and the style attribute,
inheritance, transforms, every path command, dashes (the path is cut
geometrically, curves kept), currentColor, SMIL animate / set /
animateTransform / animateMotion (see Rig compile for what each becomes).

Not supported (warned and skipped or approximated): mask, pattern (the fill
becomes none), filter, marker, foreignObject, clip paths other than one rect,
masks on gradients, fx / fy of radial gradients, group opacity (multiplied
into the descendants' brush alphas, which differs where they overlap),
textPath, text on several lines, font fallback across fonts, GPOS kerning,
event-based begin times, `end`, accumulate, animation of d / points /
transform via plain animate, animation of x1 / y1 / x2 / y2 of
lines, animateTransform additive=replace over a static transform of another
type (the static transform is dropped), two animated colors (fill and stroke)
in one shape. Size animations (r, rx, ry, width, height) scale the whole
shape, stroke included, from a reference size (the base, or the largest
animated value when the base is 0 or the growth exceeds x8).
"""

import base64
import io
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

from PIL import Image

import shapogfx_dbones as db
import shapogfx_imgconv as conv

SVG_NS = "http://www.w3.org/2000/svg"
XLINK_NS = "http://www.w3.org/1999/xlink"
XML_NS = "http://www.w3.org/XML/1998/namespace"

KAPPA = 0.5522847498307936  # 4 (sqrt(2) - 1) / 3: a quarter circle as a cubic
Q12_MAX = 7.999
MAX_FRAMES = 65535
DURATION_CAP = 60.0  # seconds, of the default duration

PATH_OPS = {"M": 0, "L": 1, "Q": 2, "C": 3, "Z": 4}
OP_NAMES = {0: "OP_MOVE", 1: "OP_LINE", 2: "OP_QUAD", 3: "OP_CUBIC", 4: "OP_CLOSE"}
OP_COORDS = {0: 2, 1: 2, 2: 4, 3: 6, 4: 0}

_warnings = []
_verbose = False


class ConvertError(Exception):
    pass


def warn(msg):
    _warnings.append(msg)
    print(f"warning: {msg}", file=sys.stderr)


def note(msg):
    """Printed with --verbose only."""
    if _verbose:
        print(f"note: {msg}", file=sys.stderr)


def warnings():
    return list(_warnings)


def reset(verbose=False):
    global _verbose
    _warnings.clear()
    _verbose = verbose


# ---------------------------------------------------------------------------
# Numbers, lengths, colors

_NUM_RE = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")
_UNIT_PX = {"": 1.0, "px": 1.0, "pt": 4.0 / 3.0, "pc": 16.0, "mm": 96.0 / 25.4, "cm": 96.0 / 2.54,
            "in": 96.0}


def parse_numbers(s):
    return [float(m) for m in _NUM_RE.findall(s or "")]


def parse_length(s, default=0.0, ref=None, font_size=16.0):
    """A length with its unit to px. `ref` is what a % refers to; without it
    a % is a fraction of 1 (bounding-box units)."""
    if s is None:
        return default
    s = s.strip()
    m = _NUM_RE.match(s)
    if not m:
        return default
    v = float(m.group(0))
    unit = s[m.end():].strip().lower()
    if unit == "%":
        return v / 100.0 * (ref if ref is not None else 1.0)
    if unit in ("em",):
        return v * font_size
    if unit == "ex":
        return v * font_size * 0.5
    return v * _UNIT_PX.get(unit, 1.0)


def parse_clock(s):
    """A SMIL clock value in seconds, or None."""
    if s is None:
        return None
    s = s.strip()
    if not s:
        return None
    m = re.fullmatch(r"([-+]?)(\d+):(\d+):(\d+(?:\.\d*)?)", s)
    if m:
        v = int(m.group(2)) * 3600 + int(m.group(3)) * 60 + float(m.group(4))
        return -v if m.group(1) == "-" else v
    m = re.fullmatch(r"([-+]?)(\d+):(\d+(?:\.\d*)?)", s)
    if m:
        v = int(m.group(2)) * 60 + float(m.group(3))
        return -v if m.group(1) == "-" else v
    m = re.fullmatch(r"([-+]?(?:\d+\.?\d*|\.\d+))\s*(h|min|s|ms)?", s)
    if m:
        v = float(m.group(1))
        return v * {"h": 3600.0, "min": 60.0, "s": 1.0, "ms": 0.001, None: 1.0}[m.group(2)]
    return None


CSS_COLORS = {
    "aliceblue": "f0f8ff", "antiquewhite": "faebd7", "aqua": "00ffff", "aquamarine": "7fffd4",
    "azure": "f0ffff", "beige": "f5f5dc", "bisque": "ffe4c4", "black": "000000",
    "blanchedalmond": "ffebcd", "blue": "0000ff", "blueviolet": "8a2be2", "brown": "a52a2a",
    "burlywood": "deb887", "cadetblue": "5f9ea0", "chartreuse": "7fff00", "chocolate": "d2691e",
    "coral": "ff7f50", "cornflowerblue": "6495ed", "cornsilk": "fff8dc", "crimson": "dc143c",
    "cyan": "00ffff", "darkblue": "00008b", "darkcyan": "008b8b", "darkgoldenrod": "b8860b",
    "darkgray": "a9a9a9", "darkgreen": "006400", "darkgrey": "a9a9a9", "darkkhaki": "bdb76b",
    "darkmagenta": "8b008b", "darkolivegreen": "556b2f", "darkorange": "ff8c00",
    "darkorchid": "9932cc", "darkred": "8b0000", "darksalmon": "e9967a", "darkseagreen": "8fbc8f",
    "darkslateblue": "483d8b", "darkslategray": "2f4f4f", "darkslategrey": "2f4f4f",
    "darkturquoise": "00ced1", "darkviolet": "9400d3", "deeppink": "ff1493",
    "deepskyblue": "00bfff", "dimgray": "696969", "dimgrey": "696969", "dodgerblue": "1e90ff",
    "firebrick": "b22222", "floralwhite": "fffaf0", "forestgreen": "228b22", "fuchsia": "ff00ff",
    "gainsboro": "dcdcdc", "ghostwhite": "f8f8ff", "gold": "ffd700", "goldenrod": "daa520",
    "gray": "808080", "green": "008000", "greenyellow": "adff2f", "grey": "808080",
    "honeydew": "f0fff0", "hotpink": "ff69b4", "indianred": "cd5c5c", "indigo": "4b0082",
    "ivory": "fffff0", "khaki": "f0e68c", "lavender": "e6e6fa", "lavenderblush": "fff0f5",
    "lawngreen": "7cfc00", "lemonchiffon": "fffacd", "lightblue": "add8e6", "lightcoral": "f08080",
    "lightcyan": "e0ffff", "lightgoldenrodyellow": "fafad2", "lightgray": "d3d3d3",
    "lightgreen": "90ee90", "lightgrey": "d3d3d3", "lightpink": "ffb6c1", "lightsalmon": "ffa07a",
    "lightseagreen": "20b2aa", "lightskyblue": "87cefa", "lightslategray": "778899",
    "lightslategrey": "778899", "lightsteelblue": "b0c4de", "lightyellow": "ffffe0",
    "lime": "00ff00", "limegreen": "32cd32", "linen": "faf0e6", "magenta": "ff00ff",
    "maroon": "800000", "mediumaquamarine": "66cdaa", "mediumblue": "0000cd",
    "mediumorchid": "ba55d3", "mediumpurple": "9370db", "mediumseagreen": "3cb371",
    "mediumslateblue": "7b68ee", "mediumspringgreen": "00fa9a", "mediumturquoise": "48d1cc",
    "mediumvioletred": "c71585", "midnightblue": "191970", "mintcream": "f5fffa",
    "mistyrose": "ffe4e1", "moccasin": "ffe4b5", "navajowhite": "ffdead", "navy": "000080",
    "oldlace": "fdf5e6", "olive": "808000", "olivedrab": "6b8e23", "orange": "ffa500",
    "orangered": "ff4500", "orchid": "da70d6", "palegoldenrod": "eee8aa", "palegreen": "98fb98",
    "paleturquoise": "afeeee", "palevioletred": "db7093", "papayawhip": "ffefd5",
    "peachpuff": "ffdab9", "peru": "cd853f", "pink": "ffc0cb", "plum": "dda0dd",
    "powderblue": "b0e0e6", "purple": "800080", "rebeccapurple": "663399", "red": "ff0000",
    "rosybrown": "bc8f8f", "royalblue": "4169e1", "saddlebrown": "8b4513", "salmon": "fa8072",
    "sandybrown": "f4a460", "seagreen": "2e8b57", "seashell": "fff5ee", "sienna": "a0522d",
    "silver": "c0c0c0", "skyblue": "87ceeb", "slateblue": "6a5acd", "slategray": "708090",
    "slategrey": "708090", "snow": "fffafa", "springgreen": "00ff7f", "steelblue": "4682b4",
    "tan": "d2b48c", "teal": "008080", "thistle": "d8bfd8", "tomato": "ff6347",
    "turquoise": "40e0d0", "violet": "ee82ee", "wheat": "f5deb3", "white": "ffffff",
    "whitesmoke": "f5f5f5", "yellow": "ffff00", "yellowgreen": "9acd32",
}


def _hsl_to_rgb(h, s, l):
    h = (h % 360.0) / 360.0

    def f(n):
        k = (n + h * 12.0) % 12.0
        a = s * min(l, 1.0 - l)
        return l - a * max(-1.0, min(k - 3.0, 9.0 - k, 1.0))
    return f(0) * 255.0, f(8) * 255.0, f(4) * 255.0


def parse_color(s):
    """A CSS color to (r, g, b, a) with r, g, b in 0..255 and a in 0..1, or
    None when `s` is not a color (none, currentColor, url(...))."""
    if s is None:
        return None
    s = s.strip()
    low = s.lower()
    if low in CSS_COLORS:
        h = CSS_COLORS[low]
        return float(int(h[0:2], 16)), float(int(h[2:4], 16)), float(int(h[4:6], 16)), 1.0
    if low == "transparent":
        return 0.0, 0.0, 0.0, 0.0
    if low.startswith("#"):
        h = low[1:]
        if len(h) in (3, 4):
            h = "".join(c + c for c in h)
        if len(h) in (6, 8) and re.fullmatch(r"[0-9a-f]+", h):
            r, g, b = (float(int(h[i:i + 2], 16)) for i in (0, 2, 4))
            a = int(h[6:8], 16) / 255.0 if len(h) == 8 else 1.0
            return r, g, b, a
        return None
    m = re.fullmatch(r"(rgba?|hsla?)\((.*)\)", low)
    if m:
        parts = [p.strip() for p in re.split(r"[,\s/]+", m.group(2).strip()) if p.strip()]
        if len(parts) < 3:
            return None

        def chan(p, scale=255.0):
            return float(p[:-1]) * scale / 100.0 if p.endswith("%") else float(p)
        a = 1.0
        if len(parts) > 3:
            p = parts[3]
            a = float(p[:-1]) / 100.0 if p.endswith("%") else float(p)
        if m.group(1).startswith("rgb"):
            r, g, b = (chan(p) for p in parts[:3])
        else:
            hh = float(parts[0].rstrip("deg"))
            r, g, b = _hsl_to_rgb(hh, chan(parts[1], 1.0), chan(parts[2], 1.0))
        return (max(0.0, min(255.0, r)), max(0.0, min(255.0, g)), max(0.0, min(255.0, b)),
                max(0.0, min(1.0, a)))
    return None


def parse_paint(s):
    """A paint: ("none",), ("current",), ("color", (r, g, b, a)), or
    ("url", id, fallback paint or None)."""
    if s is None:
        return None
    s = s.strip()
    if not s:
        return None
    low = s.lower()
    if low == "none":
        return ("none",)
    if low == "currentcolor":
        return ("current",)
    m = re.match(r"url\(\s*['\"]?#([^)'\"]+)['\"]?\s*\)\s*(.*)", s)
    if m:
        fb = parse_paint(m.group(2)) if m.group(2).strip() else None
        return ("url", m.group(1), fb)
    c = parse_color(s)
    return ("color", c) if c else None


# ---------------------------------------------------------------------------
# Affine matrices: (a, b, c, d, tx, ty), x' = a x + c y + tx; mul(m, n) applies n first

IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
mat_mul = db.mat_mul
mat_apply = db.mat_apply


def mat_inv(m):
    a, b, c, d, tx, ty = m
    det = a * d - b * c
    if abs(det) < 1e-12:
        return None
    return db.mat_inv(m)


def mat_translate(x, y):
    return (1.0, 0.0, 0.0, 1.0, x, y)


def mat_scale(sx, sy):
    return (sx, 0.0, 0.0, sy, 0.0, 0.0)


def mat_rotate(deg, cx=0.0, cy=0.0):
    r = math.radians(deg)
    cs, sn = math.cos(r), math.sin(r)
    return (cs, sn, -sn, cs, cx - cs * cx + sn * cy, cy - sn * cx - cs * cy)


def mat_is_identity(m, eps=1e-9):
    return all(abs(v - w) <= eps for v, w in zip(m, IDENTITY))


def mat_is_translation(m, eps=1e-9):
    return all(abs(v - w) <= eps for v, w in zip(m[:4], IDENTITY[:4]))


def mat_is_axis_aligned(m, eps=1e-9):
    """No rotation or skew (b and c zero), so that a rectangle maps to one."""
    return abs(m[1]) <= eps and abs(m[2]) <= eps


def parse_transform(s):
    """A transform list to [(kind, args, matrix)] in order of application
    (first item outermost, as SVG composes them: ctm = parent * m1 * m2 ...)."""
    out = []
    if not s:
        return out
    for m in re.finditer(r"([a-zA-Z]+)\s*\(([^)]*)\)", s):
        kind = m.group(1)
        v = parse_numbers(m.group(2))
        if kind == "matrix" and len(v) == 6:
            out.append(("matrix", v, tuple(v)))
        elif kind == "translate" and 1 <= len(v) <= 2:
            tx, ty = v[0], (v[1] if len(v) > 1 else 0.0)
            out.append(("translate", [tx, ty], mat_translate(tx, ty)))
        elif kind == "scale" and 1 <= len(v) <= 2:
            sx, sy = v[0], (v[1] if len(v) > 1 else v[0])
            out.append(("scale", [sx, sy], mat_scale(sx, sy)))
        elif kind == "rotate" and len(v) in (1, 3):
            a = v[0]
            cx, cy = (v[1], v[2]) if len(v) == 3 else (0.0, 0.0)
            out.append(("rotate", [a, cx, cy], mat_rotate(a, cx, cy)))
        elif kind == "skewX" and len(v) == 1:
            out.append(("skewX", v, (1.0, 0.0, math.tan(math.radians(v[0])), 1.0, 0.0, 0.0)))
        elif kind == "skewY" and len(v) == 1:
            out.append(("skewY", v, (1.0, math.tan(math.radians(v[0])), 0.0, 1.0, 0.0, 0.0)))
        else:
            warn(f"transform {kind}({m.group(2).strip()}) is not understood, ignored")
    return out


def transform_matrix(items):
    m = IDENTITY
    for _, _, im in items:
        m = mat_mul(m, im)
    return m


def decompose(m):
    """A matrix to rig bone parameters (x, y, rotX deg, rotY deg, sx, sy):
    exact for every affine matrix (sx = hypot(a, b), rotY = atan2(b, a),
    sy = hypot(c, d), rotX = atan2(-c, d))."""
    a, b, c, d, tx, ty = m
    return (tx, ty, math.degrees(math.atan2(-c, d)), math.degrees(math.atan2(b, a)),
            math.hypot(a, b), math.hypot(c, d))


def rect_transform_bounds(r, m):
    """Bounding box of a rect (x, y, w, h) mapped by m."""
    x, y, w, h = r
    pts = [mat_apply(m, px, py) for px, py in ((x, y), (x + w, y), (x, y + h), (x + w, y + h))]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))


def rect_intersect(a, b):
    x0, y0 = max(a[0], b[0]), max(a[1], b[1])
    x1, y1 = min(a[0] + a[2], b[0] + b[2]), min(a[1] + a[3], b[1] + b[3])
    return (x0, y0, max(0.0, x1 - x0), max(0.0, y1 - y0))


# ---------------------------------------------------------------------------
# Paths


class PathData:
    """Ops (0 MOVE, 1 LINE, 2 QUAD, 3 CUBIC, 4 CLOSE) with their coordinates."""

    def __init__(self):
        self.ops = []
        self.coords = []

    def add(self, op, *xy):
        self.ops.append(op)
        self.coords.extend(float(v) for v in xy)

    def move(self, x, y):
        self.add(0, x, y)

    def line(self, x, y):
        self.add(1, x, y)

    def quad(self, cx, cy, x, y):
        self.add(2, cx, cy, x, y)

    def cubic(self, c1x, c1y, c2x, c2y, x, y):
        self.add(3, c1x, c1y, c2x, c2y, x, y)

    def close(self):
        self.add(4)

    def empty(self):
        return not any(op != 0 and op != 4 for op in self.ops)

    def segments(self):
        """[(op, coords list)] per op."""
        out = []
        i = 0
        for op in self.ops:
            n = OP_COORDS[op]
            out.append((op, self.coords[i:i + n]))
            i += n
        return out

    def extend(self, other):
        self.ops.extend(other.ops)
        self.coords.extend(other.coords)

    def transformed(self, m):
        p = PathData()
        p.ops = list(self.ops)
        for i in range(0, len(self.coords), 2):
            x, y = mat_apply(m, self.coords[i], self.coords[i + 1])
            p.coords.extend((x, y))
        return p

    def bounds(self):
        """(x, y, w, h) of every point (control points included), or None."""
        if not self.coords:
            return None
        xs = self.coords[0::2]
        ys = self.coords[1::2]
        return (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))

    def subpaths(self):
        """[(points as [(op, coords)], closed)] with each subpath starting at
        its MOVE; the start point is in the MOVE op."""
        out = []
        cur = None
        closed = False
        for op, c in self.segments():
            if op == 0:
                if cur:
                    out.append((cur, closed))
                cur, closed = [(op, c)], False
            elif op == 4:
                if cur:
                    closed = True
                    out.append((cur, closed))
                    # A segment after CLOSE starts at the last MOVE
                    cur, closed = [(0, list(cur[0][1]))], False
            else:
                if cur is None:
                    cur = [(0, [c[-2], c[-1]])]
                cur.append((op, c))
        if cur and len(cur) > 1:
            out.append((cur, closed))
        return out


def arc_to_cubics(path, x1, y1, rx, ry, phi_deg, large, sweep, x2, y2):
    """An SVG arc to cubic beziers (center parameterization, split at most
    every 90 degrees), appended to `path`."""
    if (x1, y1) == (x2, y2):
        return
    rx, ry = abs(rx), abs(ry)
    if rx == 0.0 or ry == 0.0:
        path.line(x2, y2)
        return
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2.0, (y1 - y2) / 2.0
    x1p = cp * dx + sp * dy
    y1p = -sp * dx + cp * dy
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1.0:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = 0.0 if den == 0.0 else math.sqrt(max(0.0, num / den))
    if large == sweep:
        coef = -coef
    cxp = coef * rx * y1p / ry
    cyp = -coef * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2.0
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2.0

    def ang(ux, uy, vx, vy):
        d = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return d
    t1 = math.atan2((y1p - cyp) / ry, (x1p - cxp) / rx)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(1, int(math.ceil(abs(dt) / (math.pi / 2) - 1e-9)))
    step = dt / n
    k = 4.0 / 3.0 * math.tan(step / 4.0)
    t = t1
    for i in range(n):
        c1, s1 = math.cos(t), math.sin(t)
        c2, s2 = math.cos(t + step), math.sin(t + step)
        p1 = (c1 - k * s1, s1 + k * c1)
        p2 = (c2 + k * s2, s2 - k * c2)
        p3 = (c2, s2)

        def to_user(p):
            ex, ey = rx * p[0], ry * p[1]
            return cp * ex - sp * ey + cx, sp * ex + cp * ey + cy
        a1, a2, a3 = to_user(p1), to_user(p2), to_user(p3)
        if i == n - 1:
            a3 = (x2, y2)
        path.cubic(a1[0], a1[1], a2[0], a2[1], a3[0], a3[1])
        t += step


_PATH_TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


def parse_path_d(d, what="path"):
    """Every SVG path command (absolute and relative, implicit repeats, arcs as
    cubics, S / T reflecting the previous control point) to a PathData."""
    p = PathData()
    tokens = _PATH_TOKEN.findall(d or "")
    i = 0
    cmd = None
    cx = cy = 0.0  # current point
    sx = sy = 0.0  # subpath start
    lcx = lcy = None  # last control point (for S / T)
    last_cmd = None
    arg_count = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2, "A": 7, "Z": 0}

    def num():
        nonlocal i
        if i >= len(tokens) or tokens[i].isalpha():
            raise ConvertError(f"{what}: malformed path data near token {i}")
        v = float(tokens[i])
        i += 1
        return v

    while i < len(tokens):
        t = tokens[i]
        if t.isalpha():
            cmd = t
            i += 1
        elif cmd is None:
            raise ConvertError(f"{what}: path data must start with a command")
        elif cmd in "Mm":
            cmd = "L" if cmd == "M" else "l"  # implicit lineto after moveto
        elif cmd in "Zz":
            raise ConvertError(f"{what}: numbers after Z")
        up = cmd.upper()
        rel = cmd.islower()
        n = arg_count[up]
        if up == "Z":
            p.close()
            cx, cy = sx, sy
            lcx = lcy = None
            last_cmd = "Z"
            continue
        if i + n > len(tokens) or any(tokens[i + k].isalpha() for k in range(n)):
            raise ConvertError(f"{what}: too few numbers for {cmd}")
        args = [num() for _ in range(n)]
        ox, oy = (cx, cy) if rel else (0.0, 0.0)
        if up == "M":
            cx, cy = ox + args[0], oy + args[1]
            sx, sy = cx, cy
            p.move(cx, cy)
            lcx = lcy = None
        elif up == "L":
            cx, cy = ox + args[0], oy + args[1]
            p.line(cx, cy)
            lcx = lcy = None
        elif up == "H":
            cx = ox + args[0]
            p.line(cx, cy)
            lcx = lcy = None
        elif up == "V":
            cy = oy + args[0]
            p.line(cx, cy)
            lcx = lcy = None
        elif up == "C":
            c1 = (ox + args[0], oy + args[1])
            c2 = (ox + args[2], oy + args[3])
            cx, cy = ox + args[4], oy + args[5]
            p.cubic(c1[0], c1[1], c2[0], c2[1], cx, cy)
            lcx, lcy = c2
        elif up == "S":
            if last_cmd in ("C", "S") and lcx is not None:
                c1 = (2 * cx - lcx, 2 * cy - lcy)
            else:
                c1 = (cx, cy)
            c2 = (ox + args[0], oy + args[1])
            cx, cy = ox + args[2], oy + args[3]
            p.cubic(c1[0], c1[1], c2[0], c2[1], cx, cy)
            lcx, lcy = c2
        elif up == "Q":
            c1 = (ox + args[0], oy + args[1])
            cx, cy = ox + args[2], oy + args[3]
            p.quad(c1[0], c1[1], cx, cy)
            lcx, lcy = c1
        elif up == "T":
            if last_cmd in ("Q", "T") and lcx is not None:
                c1 = (2 * cx - lcx, 2 * cy - lcy)
            else:
                c1 = (cx, cy)
            cx, cy = ox + args[0], oy + args[1]
            p.quad(c1[0], c1[1], cx, cy)
            lcx, lcy = c1
        elif up == "A":
            x2, y2 = ox + args[5], oy + args[6]
            arc_to_cubics(p, cx, cy, args[0], args[1], args[2], args[3] != 0, args[4] != 0, x2, y2)
            cx, cy = x2, y2
            lcx = lcy = None
        last_cmd = up
    return p


def rect_path(x, y, w, h, rx=0.0, ry=0.0):
    p = PathData()
    if w <= 0 or h <= 0:
        return p
    rx = min(max(rx, 0.0), w / 2.0)
    ry = min(max(ry, 0.0), h / 2.0)
    if rx <= 0 or ry <= 0:
        p.move(x, y)
        p.line(x + w, y)
        p.line(x + w, y + h)
        p.line(x, y + h)
        p.close()
        return p
    kx, ky = rx * KAPPA, ry * KAPPA
    p.move(x + rx, y)
    p.line(x + w - rx, y)
    p.cubic(x + w - rx + kx, y, x + w, y + ry - ky, x + w, y + ry)
    p.line(x + w, y + h - ry)
    p.cubic(x + w, y + h - ry + ky, x + w - rx + kx, y + h, x + w - rx, y + h)
    p.line(x + rx, y + h)
    p.cubic(x + rx - kx, y + h, x, y + h - ry + ky, x, y + h - ry)
    p.line(x, y + ry)
    p.cubic(x, y + ry - ky, x + rx - kx, y, x + rx, y)
    p.close()
    return p


def ellipse_path(cx, cy, rx, ry):
    p = PathData()
    if rx <= 0 or ry <= 0:
        return p
    kx, ky = rx * KAPPA, ry * KAPPA
    p.move(cx + rx, cy)
    p.cubic(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry)
    p.cubic(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy)
    p.cubic(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry)
    p.cubic(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy)
    p.close()
    return p


def polyline_path(points, closed):
    p = PathData()
    pts = parse_numbers(points)
    if len(pts) < 4:
        return p
    p.move(pts[0], pts[1])
    for i in range(2, len(pts) - 1, 2):
        p.line(pts[i], pts[i + 1])
    if closed:
        p.close()
    return p


# Curve evaluation and splitting (de Casteljau) for dashes and motion paths


def _seg_point(p0, op, c, t):
    """Point at parameter t of the segment from p0 with op / coords."""
    x0, y0 = p0
    u = 1.0 - t
    if op == 1:
        return x0 + (c[0] - x0) * t, y0 + (c[1] - y0) * t
    if op == 2:
        return (u * u * x0 + 2 * u * t * c[0] + t * t * c[2],
                u * u * y0 + 2 * u * t * c[1] + t * t * c[3])
    return (u * u * u * x0 + 3 * u * u * t * c[0] + 3 * u * t * t * c[2] + t * t * t * c[4],
            u * u * u * y0 + 3 * u * u * t * c[1] + 3 * u * t * t * c[3] + t * t * t * c[5])


def _seg_split(p0, op, c, t):
    """The segment cut at t: (coords of the first part, coords of the second
    part); both start where the whole did / at the cut."""
    x0, y0 = p0
    if op == 1:
        m = _seg_point(p0, op, c, t)
        return [m[0], m[1]], [c[0], c[1]]
    if op == 2:
        q0 = (x0 + (c[0] - x0) * t, y0 + (c[1] - y0) * t)
        q1 = (c[0] + (c[2] - c[0]) * t, c[1] + (c[3] - c[1]) * t)
        m = (q0[0] + (q1[0] - q0[0]) * t, q0[1] + (q1[1] - q0[1]) * t)
        return [q0[0], q0[1], m[0], m[1]], [q1[0], q1[1], c[2], c[3]]
    p1, p2, p3 = (c[0], c[1]), (c[2], c[3]), (c[4], c[5])

    def lerp(a, b):
        return a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
    q0, q1, q2 = lerp(p0, p1), lerp(p1, p2), lerp(p2, p3)
    r0, r1 = lerp(q0, q1), lerp(q1, q2)
    m = lerp(r0, r1)
    return [q0[0], q0[1], r0[0], r0[1], m[0], m[1]], [r1[0], r1[1], q2[0], q2[1], p3[0], p3[1]]


def _seg_samples(p0, op, c, n=16):
    """Cumulative lengths at parameters k / n."""
    pts = [p0] + [_seg_point(p0, op, c, k / n) for k in range(1, n + 1)]
    cum = [0.0]
    for a, b in zip(pts, pts[1:]):
        cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    return cum


def _seg_t_at_length(cum, s):
    """Parameter where the cumulative length reaches s (linear between samples)."""
    n = len(cum) - 1
    if s <= 0:
        return 0.0
    if s >= cum[-1]:
        return 1.0
    for k in range(n):
        if cum[k + 1] >= s:
            d = cum[k + 1] - cum[k]
            f = 0.0 if d <= 0 else (s - cum[k]) / d
            return (k + f) / n
    return 1.0


def path_dashed(path, dashes, offset):
    """The path cut into the "on" parts of the dash pattern along its length
    (curves are split, not flattened; the lengths are measured on 16 samples
    per curve). Closed subpaths are cut as open ones ending with a line back
    to their start."""
    dashes = [d for d in dashes]
    if not dashes or any(d < 0 for d in dashes) or sum(dashes) <= 0:
        return path
    if len(dashes) % 2:
        dashes = dashes * 2
    total = sum(dashes)
    out = PathData()
    for segs, closed in path.subpaths():
        pieces = []  # (p0, op, coords)
        p0 = (segs[0][1][0], segs[0][1][1])
        start = p0
        for op, c in segs[1:]:
            pieces.append((p0, op, c))
            p0 = (c[-2], c[-1])
        if closed and p0 != start:
            pieces.append((p0, 1, [start[0], start[1]]))
        # Dash state
        pos = offset % total if total > 0 else 0.0
        idx = 0
        while pos >= dashes[idx]:
            pos -= dashes[idx]
            idx = (idx + 1) % len(dashes)
        remain = dashes[idx] - pos  # length left in the current dash entry
        on = idx % 2 == 0
        pen_down = False
        for p0, op, c in pieces:
            cum = _seg_samples(p0, op, c)
            length = cum[-1]
            if on and not pen_down:
                out.move(p0[0], p0[1])
                pen_down = True
            s = 0.0  # length consumed in this piece
            cur_p0, cur_c = p0, list(c)
            while length - s > remain + 1e-9:
                s += remain
                t_abs = _seg_t_at_length(cum, s)
                # Parameter within the remaining part of the piece
                t_prev = _seg_t_at_length(cum, s - remain)
                t_rel = 0.0 if t_abs <= t_prev else (t_abs - t_prev) / (1.0 - t_prev) if t_prev < 1.0 else 1.0
                first, second = _seg_split(cur_p0, op, cur_c, t_rel)
                if on:
                    out.add(op, *first)
                cut = (first[-2], first[-1])
                cur_p0, cur_c = cut, second
                idx = (idx + 1) % len(dashes)
                remain = dashes[idx]
                on = idx % 2 == 0
                if on:
                    out.move(cut[0], cut[1])
                    pen_down = True
                else:
                    pen_down = False
                if remain <= 0:  # zero-length entries: skip them
                    idx = (idx + 1) % len(dashes)
                    remain = dashes[idx]
                    on = idx % 2 == 0
                    if on and not pen_down:
                        out.move(cut[0], cut[1])
                        pen_down = True
            remain -= length - s
            if on:
                out.add(op, *cur_c)
    return out


def path_flatten(path, tol=0.25):
    """[(points, closed)] polylines of the subpaths, for measuring."""
    out = []
    for segs, closed in path.subpaths():
        pts = [(segs[0][1][0], segs[0][1][1])]
        for op, c in segs[1:]:
            p0 = pts[-1]
            if op == 1:
                pts.append((c[0], c[1]))
            else:
                n = 16
                pts.extend(_seg_point(p0, op, c, k / n) for k in range(1, n + 1))
        out.append((pts, closed))
    return out


# ---------------------------------------------------------------------------
# Style sheets (simple selectors) and property computation

INHERITED = {
    "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity",
    "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
    "stroke-dashoffset", "color", "visibility", "font-family", "font-size", "font-weight",
    "font-style", "text-anchor", "letter-spacing",
}
PROPERTIES = INHERITED | {"opacity", "display", "clip-path", "stop-color", "stop-opacity",
                          "mask", "filter", "marker-start", "marker-mid", "marker-end"}
DEFAULTS = {
    "fill": "black", "fill-opacity": "1", "fill-rule": "nonzero", "stroke": "none",
    "stroke-width": "1", "stroke-opacity": "1", "stroke-linecap": "butt",
    "stroke-linejoin": "miter", "stroke-miterlimit": "4", "stroke-dasharray": "none",
    "stroke-dashoffset": "0", "color": "black", "visibility": "visible", "font-family": "sans-serif",
    "font-size": "16", "font-weight": "normal", "font-style": "normal", "text-anchor": "start",
    "letter-spacing": "0", "opacity": "1", "display": "inline", "clip-path": "none",
    "stop-color": "black", "stop-opacity": "1", "mask": "none", "filter": "none",
    "marker-start": "none", "marker-mid": "none", "marker-end": "none",
}


def parse_declarations(text):
    out = {}
    for decl in text.split(";"):
        if ":" not in decl:
            continue
        name, value = decl.split(":", 1)
        name = name.strip().lower()
        value = value.strip()
        if value.lower().endswith("!important"):
            value = value[:-len("!important")].strip()
        if name:
            out[name] = value
    return out


class StyleSheet:
    """Rules of <style> elements: selectors limited to type, .class, #id, *,
    comma lists and the descendant combinator (child / attribute / pseudo
    selectors are dropped with a warning)."""

    def __init__(self):
        self.rules = []  # (compounds, specificity, order, decls)

    def add(self, text):
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"@[^{]*\{[^{}]*\}", "", text)  # @font-face and the like
        for m in re.finditer(r"([^{}]+)\{([^{}]*)\}", text):
            decls = parse_declarations(m.group(2))
            for sel in m.group(1).split(","):
                sel = sel.strip()
                if not sel:
                    continue
                parsed = self._parse_selector(sel)
                if parsed is None:
                    warn(f"style sheet: selector '{sel}' is not supported, ignored")
                    continue
                self.rules.append((parsed[0], parsed[1], len(self.rules), decls))

    @staticmethod
    def _parse_selector(sel):
        if re.search(r"[>+~\[\]:()]", sel):
            return None
        compounds = []
        spec = [0, 0, 0]
        for part in sel.split():
            m = re.fullmatch(r"(\*|[A-Za-z][\w-]*)?((?:[.#][\w-]+)*)", part)
            if not m:
                return None
            typ = m.group(1)
            if typ == "*":
                typ = None
            elif typ:
                spec[2] += 1
            ids, classes = [], []
            for q in re.findall(r"[.#][\w-]+", m.group(2)):
                if q[0] == "#":
                    ids.append(q[1:])
                    spec[0] += 1
                else:
                    classes.append(q[1:])
                    spec[1] += 1
            compounds.append((typ, ids, classes))
        return compounds, tuple(spec)

    @staticmethod
    def _match_compound(comp, node):
        typ, ids, classes = comp
        if typ and node.tag != typ:
            return False
        if ids and any(node.id != i for i in ids):
            return False
        if classes:
            have = set((node.attrs.get("class") or "").split())
            if any(c not in have for c in classes):
                return False
        return True

    def matches(self, compounds, node):
        if not self._match_compound(compounds[-1], node):
            return False
        n = node.parent
        for comp in reversed(compounds[:-1]):
            while n is not None and not self._match_compound(comp, n):
                n = n.parent
            if n is None:
                return False
            n = n.parent
        return True

    def declarations_for(self, node):
        """Declarations of the matching rules, lower specificity first."""
        hits = [(spec, order, decls) for comps, spec, order, decls in self.rules if self.matches(comps, node)]
        hits.sort(key=lambda h: (h[0], h[1]))
        out = {}
        for _, _, decls in hits:
            out.update(decls)
        return out


# ---------------------------------------------------------------------------
# Document


class Node:
    def __init__(self, tag, attrs, parent=None):
        self.tag = tag
        self.attrs = attrs
        self.parent = parent
        self.children = []
        self.text = ""  # text content directly in this element
        self.tail = ""
        self.id = attrs.get("id")
        self.style = {}  # computed properties
        self.anims = []  # SMIL animations targeting this element
        self.index = 0  # document order
        self.name = None  # bone / slot name once assigned
        self.specified = {}  # the properties given on this element
        self.copy_of = None  # id of the element a <use> copied

    def iter(self):
        yield self
        for c in self.children:
            yield from c.iter()

    def get(self, name, default=None):
        return self.attrs.get(name, default)

    def ancestors(self):
        n = self.parent
        while n is not None:
            yield n
            n = n.parent


def _local(tag):
    if tag.startswith("{"):
        ns, _, local = tag[1:].partition("}")
        if ns == XLINK_NS:
            return "xlink:" + local
        if ns == XML_NS:
            return "xml:" + local
        return local
    return tag


ANIM_TAGS = {"animate", "set", "animateTransform", "animateMotion", "animateColor"}
IGNORED_TAGS = {"title", "desc", "metadata", "style", "script", "mpath", "stop"}
UNSUPPORTED_TAGS = {"mask", "pattern", "filter", "marker", "foreignObject", "textPath", "feGaussianBlur"}
DRAWABLE_TAGS = {"path", "rect", "circle", "ellipse", "line", "polyline", "polygon", "image", "text"}
CONTAINER_TAGS = {"svg", "g", "a", "switch", "symbol", "use"}


class Document:
    """The parsed SVG: the tree with computed styles, the elements by id, the
    gradients and clip paths, and the viewport (width, height, viewBox)."""

    def __init__(self, path, scale=1.0):
        self.path = path
        self.base_dir = os.path.dirname(os.path.abspath(path))
        self.scale = scale
        self.sheet = StyleSheet()
        self.by_id = {}
        self.anim_elements = []
        self.counter = 0
        tree = ET.parse(path)
        root = tree.getroot()
        if _local(root.tag) != "svg":
            raise ConvertError(f"{path}: the root element is <{_local(root.tag)}>, not <svg>")
        self.root = self._build(root, None)
        self._expand_uses()
        self._index()
        self._viewport()
        self._compute_styles(self.root, None)
        self._collect_animations()

    # --- tree -------------------------------------------------------------

    def _build(self, el, parent):
        tag = _local(el.tag)
        attrs = {_local(k): v for k, v in el.attrib.items()}
        node = Node(tag, attrs, parent)
        node.text = el.text or ""
        node.tail = el.tail or ""
        if tag == "style":
            self.sheet.add("".join(el.itertext()))
        for c in el:
            if not isinstance(c.tag, str):  # comments, processing instructions
                continue
            node.children.append(self._build(c, node))
        if node.id is not None:
            self.by_id.setdefault(node.id, node)
        return node

    def _expand_uses(self, depth=0):
        """<use> becomes a <g> (its transform, then translate(x, y)) holding a
        copy of the referenced element; a <symbol> copied this way is a <g>
        whose viewBox / width / height scale it (meet) when all are given."""
        for node in list(self.root.iter()):
            if node.tag != "use":
                continue
            ref = node.get("href") or node.get("xlink:href") or ""
            target = self.by_id.get(ref[1:]) if ref.startswith("#") else None
            if target is None:
                warn(f"use: '{ref}' is not an element of the document, ignored")
                node.tag = "g"
                node.children = []
                continue
            if target is node or target in node.ancestors():
                warn(f"use: '{ref}' refers to itself, ignored")
                node.tag = "g"
                node.children = []
                continue
            x = parse_length(node.get("x"), 0.0)
            y = parse_length(node.get("y"), 0.0)
            use_w, use_h = node.get("width"), node.get("height")
            t = node.get("transform", "")
            if x != 0.0 or y != 0.0:
                t = f"{t} translate({x:g} {y:g})".strip()
            node.tag = "g"
            node.attrs = {k: v for k, v in node.attrs.items()
                          if k not in ("href", "xlink:href", "x", "y", "width", "height", "transform")}
            if t:
                node.attrs["transform"] = t
            cp = self._copy(target, node)
            if cp.tag == "symbol":
                cp.tag = "g"
                vb = parse_numbers(cp.get("viewBox", ""))
                w, h = use_w, use_h
                if len(vb) == 4 and w and h and vb[2] > 0 and vb[3] > 0:
                    sw, sh = parse_length(w) / vb[2], parse_length(h) / vb[3]
                    s = min(sw, sh)
                    tx = (parse_length(w) - vb[2] * s) / 2 - vb[0] * s
                    ty = (parse_length(h) - vb[3] * s) / 2 - vb[1] * s
                    cp.attrs["transform"] = f"translate({tx:g} {ty:g}) scale({s:g})"
                elif len(vb) == 4:
                    warn(f"use of symbol '{ref}': its viewBox is ignored (give the use a width and height)")
                cp.attrs.pop("viewBox", None)
            node.children = [cp]
        # Nested uses inside copies
        if depth < 8 and any(n.tag == "use" for n in self.root.iter()):
            self._expand_uses(depth + 1)

    def _copy(self, src, parent):
        n = Node(src.tag, dict(src.attrs), parent)
        n.copy_of = src.id
        n.id = None
        n.attrs.pop("id", None)
        n.text, n.tail = src.text, src.tail
        n.children = [self._copy(c, n) for c in src.children]
        return n

    def _index(self):
        for i, n in enumerate(self.root.iter()):
            n.index = i

    # --- viewport ---------------------------------------------------------

    def _viewport(self):
        r = self.root
        vb = parse_numbers(r.get("viewBox", ""))
        if len(vb) != 4 or vb[2] <= 0 or vb[3] <= 0:
            if r.get("viewBox"):
                warn("svg: viewBox is malformed, ignored")
            vb = None
        w = r.get("width")
        h = r.get("height")

        def size(s, vb_size, what):
            if s is None or s.strip().endswith("%"):
                if vb_size is None:
                    warn(f"svg: no {what} and no viewBox, 300 x 150 assumed")
                    return 300.0 if what == "width" else 150.0
                return vb_size
            return parse_length(s)
        self.width = size(w, vb[2] if vb else None, "width")
        self.height = size(h, vb[3] if vb else None, "height")
        if self.width <= 0 or self.height <= 0:
            raise ConvertError("svg: the width and height must be positive")
        m = mat_scale(self.scale, self.scale)
        self.viewbox = vb or (0.0, 0.0, self.width, self.height)
        if vb:
            m = mat_mul(m, viewport_matrix(vb, self.width, self.height, r.get("preserveAspectRatio")))
        self.root_matrix = m
        self.picture_size = (self.width * self.scale, self.height * self.scale)

    # --- styles -----------------------------------------------------------

    def _compute_styles(self, node, parent_style):
        specified = {}
        for k, v in node.attrs.items():
            if k in PROPERTIES:
                specified[k] = v
        specified.update(self.sheet.declarations_for(node))
        specified.update(parse_declarations(node.attrs.get("style", "")))
        st = {}
        for prop in PROPERTIES:
            v = specified.get(prop)
            if v is not None and v.strip().lower() == "inherit":
                v = None
                force_inherit = True
            else:
                force_inherit = False
            if v is None:
                if parent_style is not None and (prop in INHERITED or force_inherit):
                    v = parent_style[prop]
                else:
                    v = DEFAULTS[prop]
            st[prop] = v
        node.style = st
        node.specified = specified
        for c in node.children:
            self._compute_styles(c, st)

    # --- animations -------------------------------------------------------

    def _collect_animations(self):
        for n in self.root.iter():
            if n.tag not in ANIM_TAGS:
                continue
            ref = n.get("href") or n.get("xlink:href")
            target = n.parent
            if ref:
                target = self.by_id.get(ref[1:]) if ref.startswith("#") else None
                if target is None:
                    warn(f"<{n.tag}>: target '{ref}' not found, ignored")
                    continue
            target.anims.append(n)
            self.anim_elements.append(n)

    def has_animation(self):
        return bool(self.anim_elements)

    def element(self, ref):
        """The element a url(#id) / #id reference names, or None."""
        if not ref:
            return None
        m = re.match(r"url\(\s*['\"]?#([^)'\"]+)['\"]?\s*\)", ref.strip())
        key = m.group(1) if m else (ref[1:] if ref.startswith("#") else ref)
        return self.by_id.get(key)

    def gen_name(self, tag):
        self.counter += 1
        name = f"{tag}{self.counter}"
        while name in self.by_id:
            self.counter += 1
            name = f"{tag}{self.counter}"
        return name


def viewport_matrix(vb, w, h, par):
    """viewBox (x, y, vw, vh) to a w x h viewport by preserveAspectRatio."""
    vx, vy, vw, vh = vb
    par = (par or "xMidYMid meet").split()
    align = par[0] if par else "xMidYMid"
    meet = "slice" not in par
    sx, sy = w / vw, h / vh
    if align.lower() == "none":
        return (sx, 0.0, 0.0, sy, -vx * sx, -vy * sy)
    s = min(sx, sy) if meet else max(sx, sy)
    tx = -vx * s
    ty = -vy * s
    ax = align[1:4].lower()
    ay = align[5:8].lower()
    if ax == "mid":
        tx += (w - vw * s) / 2
    elif ax == "max":
        tx += w - vw * s
    if ay == "mid":
        ty += (h - vh * s) / 2
    elif ay == "max":
        ty += h - vh * s
    return (s, 0.0, 0.0, s, tx, ty)


def node_transform_items(node):
    return parse_transform(node.get("transform"))


def node_matrix(node):
    return transform_matrix(node_transform_items(node))


def is_displayed(node):
    return node.style.get("display", "inline").strip().lower() != "none"


def switch_child(node):
    """The child a <switch> shows: the first element child (requiredFeatures
    and the like are not evaluated; systemLanguage must include en or be
    absent)."""
    for c in node.children:
        if c.tag in ANIM_TAGS or c.tag in IGNORED_TAGS:
            continue
        lang = c.get("systemLanguage")
        if lang and not any(l.strip().lower().startswith("en") for l in lang.split(",")):
            continue
        return c
    return None


# ---------------------------------------------------------------------------
# Compiled picture model (what the emitter writes)


class CGradient:
    def __init__(self, kind, spread, stops, matrix):
        self.kind = kind  # "linear" / "radial"
        self.spread = spread  # "pad" / "reflect" / "repeat"
        self.stops = stops  # [(offset, (r, g, b, a))] ints 0..255
        self.matrix = matrix  # toGradient: the shape's space to the gradient's


class CBrush:
    def __init__(self, color, gradient=None):
        self.color = color  # (r, g, b, a) ints 0..255; with a gradient only a counts
        self.gradient = gradient


class CImage:
    def __init__(self, image, key):
        self.image = image  # RGBA PIL image
        self.key = key  # dedup key
        self.name = None  # C++ name, once emitted


class CText:
    def __init__(self, text, x, y):
        self.text = text
        self.x, self.y = x, y


class CShape:
    def __init__(self, kind, data, transform):
        self.kind = kind  # "path", "image", "text"
        self.data = data  # PathData / CImage / CText
        self.transform = transform
        self.flags = 0
        self.rule = "nonzero"
        self.fill = None  # CBrush or None
        self.stroke = None
        self.stroke_style = (1.0, "butt", "miter", 4.0)
        self.clip = None  # (x, y, w, h) in the picture's space
        self.source = None  # the Node, for messages

    def bounds(self):
        """In the shape's own space, or None."""
        if self.kind == "path":
            return self.data.bounds()
        if self.kind == "image":
            return (0.0, 0.0, float(self.data.image.width), float(self.data.image.height))
        return None

    def picture_bounds(self):
        b = self.bounds()
        if b is None:
            return None
        if self.kind == "path" and self.stroke is not None:
            w = self.stroke_style[0] / 2.0
            b = (b[0] - w, b[1] - w, b[2] + 2 * w, b[3] + 2 * w)
        return rect_transform_bounds(b, self.transform)


class CPicture:
    def __init__(self, shapes, bounds):
        self.shapes = shapes
        self.bounds = bounds


def union_bounds(rects):
    rects = [r for r in rects if r is not None]
    if not rects:
        return (0.0, 0.0, 0.0, 0.0)
    x0 = min(r[0] for r in rects)
    y0 = min(r[1] for r in rects)
    x1 = max(r[0] + r[2] for r in rects)
    y1 = max(r[1] + r[3] for r in rects)
    return (x0, y0, x1 - x0, y1 - y0)


# ---------------------------------------------------------------------------
# Gradients


def _gradient_chain(doc, node):
    chain = []
    seen = set()
    n = node
    while n is not None and id(n) not in seen and n.tag in ("linearGradient", "radialGradient"):
        chain.append(n)
        seen.add(id(n))
        ref = n.get("href") or n.get("xlink:href")
        n = doc.element(ref) if ref else None
    return chain


def resolve_gradient(doc, gnode, bbox, what):
    """A gradient element (with its href chain) to a CGradient whose matrix
    maps the element's user space (bounding box `bbox`) to the gradient's,
    or None (warned) when it cannot be drawn."""
    chain = _gradient_chain(doc, gnode)

    def attr(name, default=None):
        for n in chain:
            v = n.get(name)
            if v is not None:
                return v
        return default
    stops_node = next((n for n in chain if any(c.tag == "stop" for c in n.children)), None)
    stops = []
    last = 0.0
    if stops_node is not None:
        for s in stops_node.children:
            if s.tag != "stop":
                continue
            off = s.get("offset", "0").strip()
            o = float(off[:-1]) / 100.0 if off.endswith("%") else (parse_numbers(off) or [0.0])[0]
            o = max(0.0, min(1.0, o))
            o = max(o, last)
            last = o
            sc = s.style.get("stop-color", "black")
            if sc.strip().lower() == "currentcolor":
                sc = s.style.get("color", "black")
            c = parse_color(sc) or (0.0, 0.0, 0.0, 1.0)
            so = parse_numbers(s.style.get("stop-opacity", "1"))
            a = c[3] * (so[0] if so else 1.0)
            stops.append((o, (round(c[0]), round(c[1]), round(c[2]), round(max(0.0, min(1.0, a)) * 255))))
    if not stops:
        warn(f"{what}: gradient '{gnode.id}' has no stops, not painted")
        return None
    if len(stops) > 255:
        warn(f"{what}: gradient '{gnode.id}' has {len(stops)} stops, the first 255 kept")
        stops = stops[:255]
    units = attr("gradientUnits", "objectBoundingBox")
    bbox_units = units != "userSpaceOnUse"
    spread = attr("spreadMethod", "pad").lower()
    if spread not in ("pad", "reflect", "repeat"):
        spread = "pad"
    vw, vh = doc.viewbox[2], doc.viewbox[3]
    diag = math.hypot(vw, vh) / math.sqrt(2.0)

    def length(name, default, ref):
        v = attr(name)
        if v is None:
            return default
        return parse_length(v, default, None if bbox_units else ref)
    if bbox_units:
        if bbox is None or bbox[2] <= 0 or bbox[3] <= 0:
            warn(f"{what}: gradient '{gnode.id}' in objectBoundingBox units on an element without "
                 "area, not painted")
            return None
        bt = (bbox[2], 0.0, 0.0, bbox[3], bbox[0], bbox[1])
    else:
        bt = IDENTITY
    gt = transform_matrix(parse_transform(attr("gradientTransform")))
    inv = mat_inv(mat_mul(bt, gt))
    if inv is None:
        warn(f"{what}: gradient '{gnode.id}' has a singular transform, not painted")
        return None
    if gnode.tag == "linearGradient":
        x1 = length("x1", 0.0, vw)
        y1 = length("y1", 0.0, vh)
        x2 = length("x2", 1.0 if bbox_units else vw, vw)
        y2 = length("y2", 0.0, vh)
        dx, dy = x2 - x1, y2 - y1
        l2 = dx * dx + dy * dy
        k = 1.0 / l2 if l2 > 0 else 0.0
        lin = (dx * k, 0.0, dy * k, 0.0, -(x1 * dx + y1 * dy) * k, 0.0)
        return CGradient("linear", spread, stops, mat_mul(lin, inv))
    cx = length("cx", 0.5 if bbox_units else vw / 2, vw)
    cy = length("cy", 0.5 if bbox_units else vh / 2, vh)
    r = length("r", 0.5 if bbox_units else diag / 2, diag)
    fx, fy = attr("fx"), attr("fy")
    if fx is not None or fy is not None:
        fxv = length("fx", cx, vw)
        fyv = length("fy", cy, vh)
        if abs(fxv - cx) > 1e-9 or abs(fyv - cy) > 1e-9:
            warn(f"{what}: gradient '{gnode.id}': fx / fy (focal point) are not supported, the center is used")
    k = 1.0 / r if r > 0 else 0.0
    rad = (k, 0.0, 0.0, k, -cx * k, -cy * k)
    return CGradient("radial", spread, stops, mat_mul(rad, inv))


# ---------------------------------------------------------------------------
# Element geometry


def _attr_len(node, name, default, ref=None, font_size=16.0):
    return parse_length(node.get(name), default, ref, font_size)


def element_geometry(doc, node):
    """(PathData in the user space, base position, base size) of a drawable
    element; the base position is what x / y / cx / cy animations move and
    the base size what width / height / r / rx / ry animations scale."""
    vw, vh = doc.viewbox[2], doc.viewbox[3]
    diag = math.hypot(vw, vh) / math.sqrt(2.0)
    tag = node.tag
    what = f"<{tag}{' id=' + node.id if node.id else ''}>"
    if tag == "path":
        return parse_path_d(node.get("d", ""), what), None, None
    if tag == "rect":
        x, y = _attr_len(node, "x", 0.0, vw), _attr_len(node, "y", 0.0, vh)
        w, h = _attr_len(node, "width", 0.0, vw), _attr_len(node, "height", 0.0, vh)
        rx, ry = node.get("rx"), node.get("ry")
        rxv = parse_length(rx, 0.0, vw) if rx is not None and rx.strip() != "auto" else None
        ryv = parse_length(ry, 0.0, vh) if ry is not None and ry.strip() != "auto" else None
        if rxv is None:
            rxv = ryv or 0.0
        if ryv is None:
            ryv = rxv or 0.0
        return rect_path(x, y, w, h, rxv, ryv), (x, y), (w, h)
    if tag == "circle":
        cx, cy = _attr_len(node, "cx", 0.0, vw), _attr_len(node, "cy", 0.0, vh)
        r = _attr_len(node, "r", 0.0, diag)
        return ellipse_path(cx, cy, r, r), (cx, cy), (r, r)
    if tag == "ellipse":
        cx, cy = _attr_len(node, "cx", 0.0, vw), _attr_len(node, "cy", 0.0, vh)
        rx, ry = node.get("rx"), node.get("ry")
        rxv = parse_length(rx, 0.0, vw) if rx is not None and rx.strip() != "auto" else None
        ryv = parse_length(ry, 0.0, vh) if ry is not None and ry.strip() != "auto" else None
        if rxv is None:
            rxv = ryv or 0.0
        if ryv is None:
            ryv = rxv or 0.0
        return ellipse_path(cx, cy, rxv, ryv), (cx, cy), (rxv, ryv)
    if tag == "line":
        p = PathData()
        p.move(_attr_len(node, "x1", 0.0, vw), _attr_len(node, "y1", 0.0, vh))
        p.line(_attr_len(node, "x2", 0.0, vw), _attr_len(node, "y2", 0.0, vh))
        return p, None, None
    if tag in ("polyline", "polygon"):
        return polyline_path(node.get("points", ""), tag == "polygon"), None, None
    raise ConvertError(f"{what}: not a shape")


def parse_dasharray(s, diag):
    if s is None or s.strip().lower() in ("none", ""):
        return None
    vals = [parse_length(v, 0.0, diag) for v in re.split(r"[\s,]+", s.strip()) if v]
    if not vals or any(v < 0 for v in vals) or sum(vals) <= 0:
        return None
    return vals


def image_matrix(x, y, w, h, iw, ih, par):
    """The texture's pixel space (0, 0)-(iw, ih) to the rect x, y, w, h by
    preserveAspectRatio."""
    par = (par or "xMidYMid meet").split()
    align = par[0] if par else "xMidYMid"
    if iw <= 0 or ih <= 0:
        return mat_translate(x, y)
    sx, sy = w / iw, h / ih
    if align.lower() == "none":
        return (sx, 0.0, 0.0, sy, x, y)
    s = min(sx, sy) if "slice" not in par else max(sx, sy)
    ax = align[1:4].lower()
    ay = align[5:8].lower()
    tx = x + ((w - iw * s) / 2 if ax == "mid" else (w - iw * s) if ax == "max" else 0.0)
    ty = y + ((h - ih * s) / 2 if ay == "mid" else (h - ih * s) if ay == "max" else 0.0)
    return (s, 0.0, 0.0, s, tx, ty)


# ---------------------------------------------------------------------------
# Images


class ImageLoader:
    def __init__(self, doc):
        self.doc = doc
        self.cache = {}

    def load(self, href, what):
        """RGBA PIL image of a data: URI or a file next to the SVG, or None."""
        if not href:
            warn(f"{what}: no href, skipped")
            return None
        if href in self.cache:
            return self.cache[href]
        img = None
        try:
            if href.startswith("data:"):
                head, _, payload = href.partition(",")
                if ";base64" in head:
                    data = base64.b64decode(re.sub(r"\s+", "", payload))
                else:
                    from urllib.parse import unquote_to_bytes
                    data = unquote_to_bytes(payload)
                if "svg" in head.lower():
                    warn(f"{what}: embedded SVG images are not supported, skipped")
                else:
                    img = Image.open(io.BytesIO(data))
            else:
                p = href if os.path.isabs(href) else os.path.join(self.doc.base_dir, href)
                if p.lower().endswith(".svg"):
                    warn(f"{what}: SVG images ({href}) are not supported, skipped")
                else:
                    img = Image.open(p)
            if img is not None:
                img.load()
                img = img.convert("RGBA")
        except Exception as e:  # noqa: BLE001
            warn(f"{what}: image cannot be read ({e}), skipped")
            img = None
        if img is not None:
            key = (img.width, img.height, hash(img.tobytes()))
        else:
            key = None
        cimg = CImage(img, key) if img is not None else None
        self.cache[href] = cimg
        return cimg


# ---------------------------------------------------------------------------
# Fonts (fontTools) and text


class Font:
    """A TrueType / OpenType font: glyph outlines as PathData in font units
    (y up), advances, and kern-table pairs."""

    def __init__(self, path):
        from fontTools.ttLib import TTFont
        self.path = path
        self.tt = TTFont(path)
        self.upem = self.tt["head"].unitsPerEm
        self.cmap = self.tt.getBestCmap() or {}
        self.glyphset = self.tt.getGlyphSet()
        self.hmtx = self.tt["hmtx"]
        self.kern = {}
        if "kern" in self.tt:
            try:
                for table in self.tt["kern"].kernTables:
                    if hasattr(table, "kernTable"):
                        self.kern.update(table.kernTable)
            except Exception:  # noqa: BLE001
                pass
        self.outlines = {}
        self.missing = set()

    def glyph_name(self, ch):
        return self.cmap.get(ord(ch))

    def advance(self, gname):
        try:
            return self.hmtx[gname][0]
        except KeyError:
            return 0

    def kerning(self, left, right):
        return self.kern.get((left, right), 0)

    def outline(self, gname):
        if gname in self.outlines:
            return self.outlines[gname]
        from fontTools.pens.basePen import BasePen

        class Pen(BasePen):
            def __init__(self, glyphset):
                super().__init__(glyphset)
                self.path = PathData()

            def _moveTo(self, p):
                self.path.move(p[0], p[1])

            def _lineTo(self, p):
                self.path.line(p[0], p[1])

            def _curveToOne(self, c1, c2, p):
                self.path.cubic(c1[0], c1[1], c2[0], c2[1], p[0], p[1])

            def _qCurveToOne(self, c, p):
                self.path.quad(c[0], c[1], p[0], p[1])

            def _closePath(self):
                self.path.close()

            def _endPath(self):
                pass
        pen = Pen(self.glyphset)
        try:
            self.glyphset[gname].draw(pen)
        except KeyError:
            pen.path = PathData()
        self.outlines[gname] = pen.path
        return pen.path


def font_names(path):
    """(family names, subfamily) read from a font file's name table."""
    from fontTools.ttLib import TTFont
    tt = TTFont(path, lazy=True)
    name = tt["name"]
    fams = set()
    for nid in (1, 16):
        v = name.getDebugName(nid)
        if v:
            fams.add(v.strip().lower())
    sub = (name.getDebugName(2) or "").strip().lower()
    tt.close()
    return fams, sub


class FontSet:
    """Fonts by family name: --font FAMILY=PATH (FAMILY * is the fallback) and
    the *.ttf / *.otf of --font-dir by the families of their name tables."""

    GENERIC = {"serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui"}

    def __init__(self):
        self.by_family = {}  # lower family -> [(path, subfamily)]
        self.fallback = None
        self.loaded = {}

    def add(self, family, path):
        if not os.path.isfile(path):
            raise ConvertError(f"font file not found: {path}")
        if family == "*":
            self.fallback = path
        else:
            self.by_family.setdefault(family.strip().lower(), []).append((path, ""))

    def add_dir(self, directory):
        if not os.path.isdir(directory):
            raise ConvertError(f"font directory not found: {directory}")
        for root, _, files in os.walk(directory):
            for f in sorted(files):
                if f.lower().endswith((".ttf", ".otf")):
                    p = os.path.join(root, f)
                    try:
                        fams, sub = font_names(p)
                    except Exception as e:  # noqa: BLE001
                        note(f"font {p} skipped ({e})")
                        continue
                    for fam in fams:
                        self.by_family.setdefault(fam, []).append((p, sub))

    def empty(self):
        return not self.by_family and self.fallback is None

    def find(self, families, weight, style):
        """The Font for a font-family list (or None)."""
        bold = weight.strip().lower() == "bold" or (weight.strip().isdigit() and int(weight) >= 600)
        italic = style.strip().lower() in ("italic", "oblique")
        path = None
        for fam in [f.strip().strip("'\"").lower() for f in families.split(",")]:
            if not fam:
                continue
            cands = self.by_family.get(fam)
            if not cands:
                continue

            def score(c):
                sub = c[1]
                s = 0
                if bold == ("bold" in sub):
                    s += 2
                if italic == ("italic" in sub or "oblique" in sub):
                    s += 1
                return s
            path = max(cands, key=score)[0]
            break
        if path is None:
            path = self.fallback
        if path is None:
            return None
        if path not in self.loaded:
            self.loaded[path] = Font(path)
        return self.loaded[path]


def _collapse_space(s, preserve):
    if preserve:
        return s.replace("\n", " ").replace("\t", " ")
    return re.sub(r"[ \t\r\n]+", " ", s)


def text_runs(node, preserve=None):
    """[(text, node, first)] runs of a <text> with its tspans: the characters
    with the element whose attributes (x, y, dx, dy, style) apply."""
    if preserve is None:
        preserve = node.get("xml:space") == "preserve"
    runs = []
    if node.text:
        runs.append([_collapse_space(node.text, preserve), node, True])
    for c in node.children:
        if c.tag == "tspan":
            runs.extend(text_runs(c, c.get("xml:space") == "preserve" if c.get("xml:space") else preserve))
        elif c.tag == "textPath":
            warn("textPath is not supported, its text is laid out straight")
            runs.extend(text_runs(c, preserve))
        elif c.tag not in ANIM_TAGS and c.tag not in IGNORED_TAGS and c.tag != "tspan":
            warn(f"<{c.tag}> inside text is ignored")
        if c.tail:
            runs.append([_collapse_space(c.tail, preserve), node, False])
    return runs


def text_string(node):
    runs = text_runs(node)
    s = "".join(r[0] for r in runs)
    if node.get("xml:space") != "preserve":
        s = s.strip()
    return s


def layout_text(doc, node, fonts, what):
    """The outline of a <text> as a PathData in its user space, or None when
    no font is found. Handles x / y lists (absolute positions start a new
    chunk), dx / dy, text-anchor, letter-spacing and kern pairs."""
    vw, vh = doc.viewbox[2], doc.viewbox[3]
    runs = text_runs(node)
    if not runs:
        return PathData()
    # Strip the leading / trailing space of the whole text (xml:space default)
    if node.get("xml:space") != "preserve":
        runs[0][0] = runs[0][0].lstrip()
        runs[-1][0] = runs[-1][0].rstrip()
    glyphs = []  # (font, gname, x, y, scale, chunk)
    chunks = []  # [anchor, start index, end index]
    cx, cy = 0.0, 0.0
    first = True
    for text, el, is_first in runs:
        st = el.style
        size = parse_length(st["font-size"], 16.0, None, 16.0)
        font = fonts.find(st["font-family"], st["font-weight"], st["font-style"])
        if font is None:
            return None
        xs = [parse_length(v, 0.0, vw) for v in re.split(r"[\s,]+", el.get("x", "").strip()) if v] if is_first else []
        ys = [parse_length(v, 0.0, vh) for v in re.split(r"[\s,]+", el.get("y", "").strip()) if v] if is_first else []
        dxs = [parse_length(v, 0.0, vw, size) for v in re.split(r"[\s,]+", el.get("dx", "").strip()) if v] if is_first else []
        dys = [parse_length(v, 0.0, vh, size) for v in re.split(r"[\s,]+", el.get("dy", "").strip()) if v] if is_first else []
        if el is not node and is_first and not xs and not ys and node.get("x") and first:
            pass
        letter = parse_length(st["letter-spacing"], 0.0, None, size) if st["letter-spacing"] != "normal" else 0.0
        scale = size / font.upem
        prev = None
        for i, ch in enumerate(text):
            new_chunk = False
            if i < len(xs):
                cx = xs[i]
                new_chunk = True
            if i < len(ys):
                cy = ys[i]
                new_chunk = True
            if i < len(dxs):
                cx += dxs[i]
            if i < len(dys):
                cy += dys[i]
            if new_chunk or not chunks:
                chunks.append([st["text-anchor"].strip().lower(), len(glyphs), len(glyphs)])
                prev = None
            if ch == "\n":
                continue
            g = font.glyph_name(ch)
            if g is None:
                if ch not in font.missing:
                    font.missing.add(ch)
                    warn(f"{what}: '{ch}' (U+{ord(ch):04X}) is not in {os.path.basename(font.path)}, skipped")
                continue
            if prev is not None:
                cx += font.kerning(prev, g) * scale
            glyphs.append((font, g, cx, cy, scale, len(chunks) - 1))
            cx += font.advance(g) * scale + letter
            chunks[-1][2] = len(glyphs)
            prev = g
        first = False
    # text-anchor: shift each chunk by its advance
    shifts = {}
    for k, (anchor, s, e) in enumerate(chunks):
        if e <= s or anchor == "start":
            continue
        font, g, gx, gy, scale, _ = glyphs[e - 1]
        width = gx + font.advance(g) * scale - glyphs[s][2]
        shifts[k] = -width if anchor == "end" else -width / 2.0
    path = PathData()
    for font, g, gx, gy, scale, k in glyphs:
        out = font.outline(g)
        if not out.ops:
            continue
        m = (scale, 0.0, 0.0, -scale, gx + shifts.get(k, 0.0), gy)
        path.extend(out.transformed(m))
    return path


# ---------------------------------------------------------------------------
# Picture compile


class CompileOptions:
    def __init__(self, fonts=None, text_font="nullptr", image_format="argb4444", dither="none"):
        self.fonts = fonts or FontSet()
        self.text_font = text_font
        self.image_format = image_format
        self.dither = dither


class Compiler:
    """Turns elements into CShapes. `animated_paint(node, prop)` tells (rig
    mode) whether the fill / stroke of a leaf comes from an animated
    property, so that the shape takes the current color of its slot."""

    def __init__(self, doc, options):
        self.doc = doc
        self.opt = options
        self.images = ImageLoader(doc)
        self.animated_paint = lambda node, prop: None
        self.animated_opacity = lambda node, prop: None
        self.gradient_cache = {}

    # --- paints ----------------------------------------------------------

    def brush(self, node, prop, bbox, opacity, what, force_current=False):
        """(CBrush or None, current-color flag) of a fill / stroke."""
        paint = parse_paint(node.style[prop])
        op_prop = "fill-opacity" if prop == "fill" else "stroke-opacity"
        pv = parse_numbers(node.style[op_prop])
        own = max(0.0, min(1.0, pv[0] if pv else 1.0))
        if self.animated_opacity(node, op_prop) is not None:
            own = 1.0  # in the slot's alpha timeline
        alpha = own * opacity
        if paint is None:
            warn(f"{what}: {prop} '{node.style[prop]}' is not understood, none used")
            return None, False
        if paint[0] == "none":
            return None, False
        if paint[0] == "url":
            target = self.doc.element("#" + paint[1])
            if target is None or target.tag not in ("linearGradient", "radialGradient"):
                if target is not None and target.tag == "pattern":
                    warn(f"{what}: {prop} pattern '{paint[1]}' is not supported, none used")
                    return None, False
                if paint[2] is not None:
                    paint = paint[2]
                else:
                    warn(f"{what}: {prop} url(#{paint[1]}) is not a gradient, none used")
                    return None, False
            else:
                g = resolve_gradient(self.doc, target, bbox, what)
                if g is None:
                    return None, False
                return CBrush((0, 0, 0, round(alpha * 255)), g), False
        if paint[0] == "current" or force_current:
            if force_current and paint[0] == "color":
                c = paint[1]
                return CBrush((round(c[0]), round(c[1]), round(c[2]), round(alpha * c[3] * 255))), True
            c = parse_color(node.style["color"]) or (0.0, 0.0, 0.0, 1.0)
            return CBrush((round(c[0]), round(c[1]), round(c[2]), round(alpha * c[3] * 255))), True
        c = paint[1]
        return CBrush((round(c[0]), round(c[1]), round(c[2]), round(alpha * c[3] * 255))), False

    # --- leaves ----------------------------------------------------------

    def leaf(self, node, transform, opacity, clip, shift=(0.0, 0.0), skip_own_opacity=False):
        """The CShapes of a drawable element (0, 1 or 2: a dashed stroke is a
        second shape). `transform` maps the element's user space (shifted by
        -shift) to the picture; `opacity` is the ancestors' product; `clip`
        a rect in the picture's space."""
        what = f"<{node.tag}{' id=' + node.id if node.id else ''}>"
        st = node.style
        if st["visibility"].strip().lower() in ("hidden", "collapse") and \
                self.animated_opacity(node, "visibility") is None:
            return []
        if not skip_own_opacity:
            ov = parse_numbers(st["opacity"])
            opacity *= max(0.0, min(1.0, ov[0] if ov else 1.0))
        for prop in ("mask", "filter", "marker-start", "marker-mid", "marker-end"):
            if st[prop].strip().lower() != "none":
                warn(f"{what}: {prop} is not supported, ignored")
        vw, vh = self.doc.viewbox[2], self.doc.viewbox[3]
        diag = math.hypot(vw, vh) / math.sqrt(2.0)
        shift_m = mat_translate(-shift[0], -shift[1])
        if node.tag == "image":
            return self._image(node, transform, opacity, clip, shift_m, what)
        if node.tag == "text":
            path = None
            if not self.opt.fonts.empty():
                path = layout_text(self.doc, node, self.opt.fonts, what)
            if path is None:
                return self._text_fallback(node, transform, opacity, clip, shift_m, what)
            rule = "nonzero"
        else:
            path, _, _ = element_geometry(self.doc, node)
            rule = "evenodd" if st["fill-rule"].strip().lower() == "evenodd" else "nonzero"
        if path.empty():
            note(f"{what}: empty geometry, dropped")
            return []
        path = path.transformed(shift_m)
        bbox = path.bounds()
        fill, fill_cur = self.brush(node, "fill", bbox, opacity, what,
                                    force_current=self.animated_paint(node, "fill") is not None)
        stroke, stroke_cur = self.brush(node, "stroke", bbox, opacity, what,
                                        force_current=self.animated_paint(node, "stroke") is not None)
        width = parse_length(st["stroke-width"], 1.0, diag)
        width_anim = stroke is not None and self.animated_opacity(node, "stroke-width") is not None
        if stroke is not None and width <= 0 and not width_anim:
            stroke = None
        if fill is not None and (node.tag == "line" or bbox[2] <= 0 or bbox[3] <= 0):
            fill = None  # nothing to fill
        if fill is None and stroke is None:
            note(f"{what}: neither filled nor stroked, dropped")
            return []
        cap = st["stroke-linecap"].strip().lower()
        join = st["stroke-linejoin"].strip().lower()
        if join in ("miter-clip", "arcs"):
            join = "miter"
        ml = parse_numbers(st["stroke-miterlimit"])
        miter = max(1.0, ml[0] if ml else 4.0)
        style = (width, cap if cap in ("butt", "round", "square") else "butt",
                 join if join in ("miter", "round", "bevel") else "miter", miter)
        dashes = parse_dasharray(st["stroke-dasharray"], diag) if stroke is not None else None
        shapes = []

        def make(p, f, fc, s, sc, r):
            sh = CShape("path", p, transform)
            sh.rule = r
            sh.fill, sh.stroke = f, s
            sh.flags = (1 if fc and f else 0) | (2 if sc and s else 0) | (4 if width_anim and s else 0)
            sh.stroke_style = style
            sh.clip = clip
            sh.source = node
            return sh
        if dashes is not None:
            offset = parse_length(st["stroke-dashoffset"], 0.0, diag)
            dashed = path_dashed(path, dashes, offset)
            if fill is not None:
                shapes.append(make(path, fill, fill_cur, None, False, rule))
            if dashed.ops:
                shapes.append(make(dashed, None, False, stroke, stroke_cur, "nonzero"))
        else:
            shapes.append(make(path, fill, fill_cur, stroke, stroke_cur, rule))
        return shapes

    def _image(self, node, transform, opacity, clip, shift_m, what):
        vw, vh = self.doc.viewbox[2], self.doc.viewbox[3]
        href = node.get("href") or node.get("xlink:href")
        cimg = self.images.load(href, what)
        if cimg is None:
            return []
        iw, ih = cimg.image.size
        x, y = _attr_len(node, "x", 0.0, vw), _attr_len(node, "y", 0.0, vh)
        w = _attr_len(node, "width", float(iw), vw) if node.get("width") not in (None, "auto") else float(iw)
        h = _attr_len(node, "height", float(ih), vh) if node.get("height") not in (None, "auto") else float(ih)
        if w <= 0 or h <= 0:
            return []
        m = mat_mul(transform, mat_mul(shift_m, image_matrix(x, y, w, h, iw, ih, node.get("preserveAspectRatio"))))
        sh = CShape("image", cimg, m)
        sh.fill = CBrush((0, 0, 0, round(max(0.0, min(1.0, opacity)) * 255)))
        sh.clip = clip
        sh.source = node
        return [sh]

    def _text_fallback(self, node, transform, opacity, clip, shift_m, what):
        vw, vh = self.doc.viewbox[2], self.doc.viewbox[3]
        s = text_string(node)
        if not s:
            return []
        xs = parse_numbers(node.get("x", ""))
        ys = parse_numbers(node.get("y", ""))
        x = parse_length(node.get("x", "0").split()[0].split(",")[0], 0.0, vw) if xs else 0.0
        y = parse_length(node.get("y", "0").split()[0].split(",")[0], 0.0, vh) if ys else 0.0
        x, y = mat_apply(shift_m, x, y)
        anchor = node.style["text-anchor"].strip().lower()
        warn(f"{what}: no font for '{node.style['font-family']}', emitted as a vg::Text drawn with the bitmap "
             f"font{' (text-anchor ' + anchor + ' cannot be applied)' if anchor != 'start' else ''}")
        bbox = None
        fill, fill_cur = self.brush(node, "fill", bbox, opacity, what,
                                    force_current=self.animated_paint(node, "fill") is not None)
        if fill is None:
            return []
        if fill.gradient is not None:
            warn(f"{what}: a gradient on a bitmap text is drawn in its color's alpha only")
            fill = CBrush((0, 0, 0, fill.color[3]))
        sh = CShape("text", CText(s, x, y), transform)
        sh.fill = fill
        sh.flags = 1 if fill_cur else 0
        sh.clip = clip
        sh.source = node
        return [sh]

    # --- clip paths --------------------------------------------------------

    def clip_rect(self, node, what):
        """The rect of the element's clip-path in its user space (after its
        own transform), or None. Only a clipPath holding one rect is
        supported."""
        ref = node.style["clip-path"]
        if ref.strip().lower() == "none":
            return None
        cp = self.doc.element(ref)
        if cp is None or cp.tag != "clipPath":
            warn(f"{what}: clip-path {ref} is not a clipPath, ignored")
            return None
        kids = [c for c in cp.children if c.tag not in IGNORED_TAGS and c.tag not in ANIM_TAGS]
        if len(kids) != 1 or kids[0].tag != "rect":
            warn(f"{what}: clipPath '{cp.id}' is not a single rect, ignored")
            return None
        r = kids[0]
        vw, vh = self.doc.viewbox[2], self.doc.viewbox[3]
        rect = (_attr_len(r, "x", 0.0, vw), _attr_len(r, "y", 0.0, vh),
                _attr_len(r, "width", 0.0, vw), _attr_len(r, "height", 0.0, vh))
        if r.get("rx") or r.get("ry"):
            warn(f"{what}: the rounded corners of clipPath '{cp.id}' are ignored")
        m = mat_mul(node_matrix(cp), node_matrix(r))
        if cp.get("clipPathUnits") == "objectBoundingBox":
            path, _, _ = element_geometry(self.doc, node) if node.tag in DRAWABLE_TAGS - {"image", "text"} \
                else (PathData(), None, None)
            b = path.bounds()
            if b is None:
                warn(f"{what}: clipPath '{cp.id}' in objectBoundingBox units needs a shape, ignored")
                return None
            m = mat_mul((b[2], 0.0, 0.0, b[3], b[0], b[1]), m)
        return self.map_clip(rect, m, what)

    @staticmethod
    def map_clip(rect, m, what):
        if not mat_is_axis_aligned(m, 1e-6):
            warn(f"{what}: the clip rectangle is turned, its bounding box is used")
        return rect_transform_bounds(rect, m)

    # --- subtrees ----------------------------------------------------------

    def subtree(self, node, transform, opacity, clip, out, include_own_transform=True):
        """Appends the CShapes of `node` and its descendants: `transform`
        maps the parent's user space to the picture."""
        if node.tag in ANIM_TAGS or node.tag in IGNORED_TAGS or node.tag in ("defs", "clipPath",
                                                                           "linearGradient", "radialGradient",
                                                                           "symbol"):
            return
        what = f"<{node.tag}{' id=' + node.id if node.id else ''}>"
        if node.tag in UNSUPPORTED_TAGS:
            warn(f"{what} is not supported, skipped")
            return
        if node.tag not in DRAWABLE_TAGS and node.tag not in CONTAINER_TAGS:
            warn(f"{what} is unknown, skipped")
            return
        if not is_displayed(node):
            return
        m = mat_mul(transform, node_matrix(node)) if include_own_transform else transform
        if node is not self.doc.root and node.tag == "svg":
            vb = parse_numbers(node.get("viewBox", ""))
            vw, vh = self.doc.viewbox[2], self.doc.viewbox[3]
            x, y = _attr_len(node, "x", 0.0, vw), _attr_len(node, "y", 0.0, vh)
            m = mat_mul(m, mat_translate(x, y))
            if len(vb) == 4 and node.get("width") and node.get("height"):
                m = mat_mul(m, viewport_matrix(vb, parse_length(node.get("width")), parse_length(node.get("height")),
                                               node.get("preserveAspectRatio")))
        c = self.clip_rect(node, what)
        if c is not None:
            c = rect_transform_bounds(c, m) if mat_is_axis_aligned(m, 1e-6) else self.map_clip(c, m, what)
            clip = c if clip is None else rect_intersect(clip, c)
        if node.tag in DRAWABLE_TAGS:
            out.extend(self.leaf(node, m, opacity, clip))
            return
        ov = parse_numbers(node.style["opacity"])
        opacity *= max(0.0, min(1.0, ov[0] if ov else 1.0))
        for prop in ("mask", "filter"):
            if node.style[prop].strip().lower() != "none":
                warn(f"{what}: {prop} is not supported, ignored")
        children = [switch_child(node)] if node.tag == "switch" else node.children
        for c in children:
            if c is not None:
                self.subtree(c, m, opacity, clip, out)


def compile_picture(doc, options):
    """Picture mode: the whole document as one CPicture in the picture space
    (the SVG's pixel size times the scale, origin top-left)."""
    comp = Compiler(doc, options)
    shapes = []
    comp.subtree(doc.root, doc.root_matrix, 1.0, None, shapes, include_own_transform=False)
    w, h = doc.picture_size
    return CPicture(shapes, (0.0, 0.0, w, h))


# ---------------------------------------------------------------------------
# SMIL animations: parsing
#
# An Anim is one animate / set / animateTransform / animateMotion element
# with its timing resolved: begin (the first offset of the list only), simple
# duration, active duration (repeatCount / repeatDur) and fill. Its values are
# tuples of floats (lengths, colors as r, g, b, transform parameters) or
# strings (visibility / display).

NUMERIC_ATTRS = {"opacity", "fill-opacity", "stroke-opacity", "stroke-width", "cx", "cy", "x", "y", "r",
                 "rx", "ry", "width", "height"}
COLOR_ATTRS = {"fill", "stroke", "color"}
DISCRETE_ATTRS = {"visibility", "display"}
POSITION_ATTRS = {"cx": 0, "cy": 1, "x": 0, "y": 1}
SIZE_ATTRS = {"r": (0, 1), "rx": (0,), "ry": (1,), "width": (0,), "height": (1,)}
TRANSFORM_TYPES = {"translate": 2, "scale": 2, "rotate": 3, "skewX": 1, "skewY": 1}
TRANSFORM_IDENTITY = {"translate": (0.0, 0.0), "scale": (1.0, 1.0), "rotate": (0.0, 0.0, 0.0),
                      "skewX": (0.0,), "skewY": (0.0,)}


class Anim:
    def __init__(self, el, target):
        self.el = el
        self.target = target
        self.order = el.index
        self.kind = "animate" if el.tag == "animateColor" else el.tag
        self.attr = el.get("attributeName")
        self.type = None  # animateTransform: translate / scale / rotate / skewX / skewY
        self.begin = 0.0
        self.dur = None  # simple duration in seconds (None: indefinite)
        self.active = math.inf  # active duration in seconds
        self.fill = "remove"
        self.additive = "replace"
        self.calc_mode = "linear"
        self.key_times = None
        self.key_splines = None
        self.values = []
        self.base = None
        # animateMotion
        self.motion_path = None
        self.key_points = None
        self.rotate = "0"

    @property
    def end(self):
        return self.begin + self.active

    def what(self):
        t = self.target
        tn = f"<{t.tag}{' id=' + t.id if t.id else ''}>"
        a = self.attr if self.kind != "animateTransform" else f"transform/{self.type}"
        return f"<{self.el.tag}> of {a} on {tn}"


def _split_list(s):
    return [v.strip() for v in (s or "").split(";") if v.strip()]


def _parse_value(anim, s, what):
    """One value of the animation to a tuple / string, or None (warned)."""
    s = s.strip()
    if anim.kind == "animateTransform":
        v = parse_numbers(s)
        n = TRANSFORM_TYPES[anim.type]
        if not v or len(v) > n:
            warn(f"{what}: value '{s}' is not understood, animation dropped")
            return None
        if anim.type == "scale" and len(v) == 1:
            v = [v[0], v[0]]
        while len(v) < n:
            v.append(0.0)
        return tuple(v)
    if anim.attr in NUMERIC_ATTRS:
        return (parse_length(s, 0.0),)
    if anim.attr in COLOR_ATTRS:
        c = parse_color(s) if s.lower() != "currentcolor" else parse_color(anim.target.style["color"])
        if c is None:
            warn(f"{what}: '{s}' is not a color (only colors can be animated), animation dropped")
            return None
        return (c[0], c[1], c[2])
    return s


def _add_values(a, b):
    if isinstance(a, str):
        return b
    return tuple(x + y for x, y in zip(a, b))


def base_value(anim):
    """The static value of the animated attribute of the target."""
    node = anim.target
    if anim.kind == "animateTransform":
        return TRANSFORM_IDENTITY[anim.type]
    attr = anim.attr
    if attr in ("opacity", "fill-opacity", "stroke-opacity"):
        v = parse_numbers(node.style[attr])
        return (max(0.0, min(1.0, v[0] if v else 1.0)),)
    if attr == "stroke-width":
        return (max(0.0, parse_length(node.style[attr], 1.0)),)
    if attr in COLOR_ATTRS:
        s = node.style[attr]
        if s.strip().lower() == "currentcolor":
            s = node.style["color"]
        c = parse_color(s)
        return (c[0], c[1], c[2]) if c else (0.0, 0.0, 0.0)
    if attr in DISCRETE_ATTRS:
        return node.style[attr].strip().lower()
    if attr in NUMERIC_ATTRS:
        if attr in ("width", "height") and node.tag == "image":
            return (parse_length(node.get(attr), 0.0),)
        return (parse_length(node.get(attr), 0.0),)
    return None


def parse_anim(el, target):
    """An Anim, or None when the element is not supported (warned)."""
    anim = Anim(el, target)
    what = anim.what()
    if anim.kind == "animateTransform":
        anim.type = el.get("type", "translate")
        if anim.type not in TRANSFORM_TYPES:
            warn(f"{what}: type '{anim.type}' is not supported, dropped")
            return None
        if anim.attr not in (None, "transform"):
            warn(f"{what}: animateTransform must animate 'transform', dropped")
            return None
    elif anim.kind == "animateMotion":
        anim.attr = "motion"
    else:
        if anim.attr is None:
            warn(f"{what}: no attributeName, dropped")
            return None
        if anim.attr not in NUMERIC_ATTRS | COLOR_ATTRS | DISCRETE_ATTRS:
            warn(f"{what}: animation of '{anim.attr}' is not supported, dropped")
            return None
        if anim.attr in POSITION_ATTRS or anim.attr in SIZE_ATTRS:
            if target.tag not in ("circle", "ellipse", "rect", "image", "text"):
                warn(f"{what}: '{anim.attr}' of <{target.tag}> cannot be animated, dropped")
                return None
            if anim.attr in ("rx", "ry") and target.tag == "rect":
                warn(f"{what}: the corner radii of a rect cannot be animated, dropped")
                return None
            if anim.attr in ("width", "height") and target.tag not in ("rect", "image"):
                warn(f"{what}: '{anim.attr}' of <{target.tag}> cannot be animated, dropped")
                return None
            if anim.attr == "r" and target.tag != "circle" or anim.attr in ("rx", "ry") and target.tag != "ellipse":
                warn(f"{what}: '{anim.attr}' of <{target.tag}> cannot be animated, dropped")
                return None
    # Timing
    begins = _split_list(el.get("begin"))
    if begins:
        b = begins[0]
        if b.lower() == "indefinite":
            warn(f"{what}: begin indefinite, dropped")
            return None
        t = parse_clock(b)
        if t is None:
            warn(f"{what}: event-based begin '{b}' is not supported, dropped")
            return None
        anim.begin = t
        if len(begins) > 1:
            warn(f"{what}: only the first begin time is used")
    dur = el.get("dur")
    if dur is not None and dur.strip().lower() not in ("indefinite", "media"):
        d = parse_clock(dur)
        if d is None or d <= 0:
            warn(f"{what}: dur '{dur}' is not understood, dropped")
            return None
        anim.dur = d
    elif dur is not None and dur.strip().lower() == "media":
        warn(f"{what}: dur media is not supported, dropped")
        return None
    if anim.dur is None and anim.kind != "set":
        warn(f"{what}: no dur (indefinite), dropped")
        return None
    rc = el.get("repeatCount")
    rd = el.get("repeatDur")
    active = anim.dur if anim.dur is not None else math.inf
    if rc is not None:
        if rc.strip().lower() == "indefinite":
            active = math.inf
        else:
            v = parse_numbers(rc)
            if not v or v[0] <= 0:
                warn(f"{what}: repeatCount '{rc}' is not understood, ignored")
            elif anim.dur is not None:
                active = anim.dur * v[0]
    if rd is not None:
        if rd.strip().lower() == "indefinite":
            if rc is None:
                active = math.inf
        else:
            v = parse_clock(rd)
            if v is None or v <= 0:
                warn(f"{what}: repeatDur '{rd}' is not understood, ignored")
            else:
                active = min(active, v) if rc is not None else v
    if el.get("end") is not None:
        warn(f"{what}: end is not supported, ignored")
    if el.get("min") is not None or el.get("max") is not None:
        warn(f"{what}: min / max are not supported, ignored")
    anim.active = active
    anim.fill = (el.get("fill") or "remove").strip().lower()
    if anim.fill not in ("freeze", "remove"):
        anim.fill = "remove"
    anim.additive = (el.get("additive") or "replace").strip().lower()
    if (el.get("accumulate") or "none").strip().lower() == "sum":
        warn(f"{what}: accumulate=sum is not supported, ignored")
    if (el.get("restart") or "").strip():
        note(f"{what}: restart is ignored")
    # Values
    anim.base = base_value(anim)
    if anim.kind == "animateMotion":
        return _parse_motion(anim, el, what)
    mode = (el.get("calcMode") or "linear").strip().lower()
    if anim.kind == "set":
        mode = "discrete"
    if mode not in ("discrete", "linear", "paced", "spline"):
        mode = "linear"
    if anim.attr in DISCRETE_ATTRS:
        mode = "discrete"
    anim.calc_mode = mode
    vals = _split_list(el.get("values"))
    if vals:
        parsed = [_parse_value(anim, v, what) for v in vals]
        if any(v is None for v in parsed):
            return None
        anim.values = parsed
    else:
        f, t, by = el.get("from"), el.get("to"), el.get("by")
        if anim.kind == "set":
            if t is None:
                warn(f"{what}: set without to, dropped")
                return None
            v = _parse_value(anim, t, what)
            if v is None:
                return None
            anim.values = [v]
        elif t is not None:
            tv = _parse_value(anim, t, what)
            if tv is None:
                return None
            if f is not None:
                fv = _parse_value(anim, f, what)
                if fv is None:
                    return None
            else:
                fv = anim.base  # to-animation: from the base value
            anim.values = [fv, tv]
        elif by is not None:
            bv = _parse_value(anim, by, what)
            if bv is None:
                return None
            if isinstance(bv, str):
                warn(f"{what}: by needs a numeric attribute, dropped")
                return None
            if f is not None:
                fv = _parse_value(anim, f, what)
                if fv is None:
                    return None
            else:
                fv = anim.base
            anim.values = [fv, _add_values(fv, bv)]
        else:
            warn(f"{what}: no values, to or by, dropped")
            return None
    if anim.values and isinstance(anim.values[0], tuple) and anim.base is not None and \
            isinstance(anim.base, tuple) and len(anim.values[0]) != len(anim.base):
        warn(f"{what}: values do not match the attribute, dropped")
        return None
    n = len(anim.values)
    kt = parse_numbers(el.get("keyTimes", "")) if el.get("keyTimes") else None
    if kt is not None and mode != "paced":
        ok = len(kt) == n and kt[0] == 0.0 and all(b >= a for a, b in zip(kt, kt[1:])) and \
            (mode == "discrete" or kt[-1] == 1.0)
        if not ok:
            warn(f"{what}: keyTimes do not match the values, ignored")
            kt = None
    anim.key_times = kt if mode != "paced" else None
    if mode == "spline":
        ks = _split_list(el.get("keySplines"))
        splines = [parse_numbers(s) for s in ks]
        if len(splines) != n - 1 or any(len(s) != 4 for s in splines):
            warn(f"{what}: keySplines do not match the values, linear used")
            anim.calc_mode = "linear"
        else:
            anim.key_splines = [("bezier", [max(0.0, min(1.0, s[0])), s[1], max(0.0, min(1.0, s[2])), s[3]])
                                for s in splines]
    return anim


def _parse_motion(anim, el, what):
    mode = (el.get("calcMode") or "paced").strip().lower()
    if mode not in ("discrete", "linear", "paced", "spline"):
        mode = "paced"
    anim.calc_mode = mode
    path = None
    if el.get("path"):
        path = parse_path_d(el.get("path"), what)
    else:
        mp = next((c for c in el.children if c.tag == "mpath"), None)
        if mp is not None:
            ref = mp.get("href") or mp.get("xlink:href") or ""
            target = anim.target
            doc_root = target
            while doc_root.parent is not None:
                doc_root = doc_root.parent
            found = next((n for n in doc_root.iter() if n.id == ref[1:]), None) if ref.startswith("#") else None
            if found is None or found.tag != "path":
                warn(f"{what}: mpath '{ref}' is not a path, dropped")
                return None
            path = parse_path_d(found.get("d", ""), what)
            path = path.transformed(node_matrix(found)) if found.get("transform") else path
        else:
            vals = _split_list(el.get("values"))
            pts = []
            if vals:
                pts = [parse_numbers(v) for v in vals]
            else:
                f, t, by = el.get("from"), el.get("to"), el.get("by")
                if t is not None:
                    pts = [parse_numbers(f) if f else [0.0, 0.0], parse_numbers(t)]
                elif by is not None:
                    fv = parse_numbers(f) if f else [0.0, 0.0]
                    bv = parse_numbers(by)
                    pts = [fv, [fv[0] + bv[0], fv[1] + bv[1]]]
            if not pts or any(len(p) != 2 for p in pts):
                warn(f"{what}: no path, mpath, values or to, dropped")
                return None
            path = PathData()
            path.move(*pts[0])
            for p in pts[1:]:
                path.line(*p)
    flat = []
    for pts, closed in path_flatten(path):
        if closed and pts and pts[0] != pts[-1]:
            pts = pts + [pts[0]]
        flat.extend(pts)
    if len(flat) < 2:
        if len(flat) == 1:
            flat = flat * 2
        else:
            warn(f"{what}: empty motion path, dropped")
            return None
    anim.motion_path = flat
    anim.rotate = (el.get("rotate") or "0").strip()
    kp = parse_numbers(el.get("keyPoints", "")) if el.get("keyPoints") else None
    kt = parse_numbers(el.get("keyTimes", "")) if el.get("keyTimes") else None
    if kp is not None:
        if kt is None or len(kt) != len(kp) or kt[0] != 0.0 or any(b < a for a, b in zip(kt, kt[1:])):
            warn(f"{what}: keyPoints need matching keyTimes, ignored")
            kp = kt = None
    anim.key_points = kp
    anim.key_times = kt if kp is not None else None
    if mode == "spline":
        ks = [parse_numbers(s) for s in _split_list(el.get("keySplines"))]
        n = len(kp) if kp else 2
        if len(ks) != n - 1 or any(len(s) != 4 for s in ks):
            warn(f"{what}: keySplines do not match the keyPoints, linear used")
            anim.calc_mode = "linear"
        else:
            anim.key_splines = [("bezier", s) for s in ks]
    anim.base = (0.0, 0.0, 0.0)
    return anim


# ---------------------------------------------------------------------------
# Timelines
#
# A timeline is a list of contiguous segments (f0, f1, v0, v1, curve) in
# frames over [0, D]: the value goes from v0 at f0 to v1 at f1 along the
# curve ("step": holds v0, so v1 == v0; "linear"; ("bezier", [x1, y1, x2,
# y2]) as a keySpline). None stands for "no contribution" (the animation is
# not active). Segments are combined at the union of their boundaries, which
# keeps the keys sparse; a curve cut in two stays a bezier (de Casteljau).

STEP = ("step",)
LINEAR = ("linear",)


def _bez(p0, p1, p2, p3, t):
    u = 1.0 - t
    return u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3


def ease(curve, t):
    if curve[0] == "step":
        return 0.0
    if curve[0] == "linear":
        return t
    return db.bezier_y_at_x(curve[1], t)


def _lerp_value(v0, v1, e):
    if v0 is None or v1 is None or isinstance(v0, str):
        return v0
    return tuple(a + (b - a) * e for a, b in zip(v0, v1))


def seg_value(seg, f):
    f0, f1, v0, v1, curve = seg
    if f1 <= f0:
        return v0
    return _lerp_value(v0, v1, ease(curve, (f - f0) / (f1 - f0)))


def split_bezier_easing(pts, x):
    """A keySpline cut at x: the two halves as keySplines (normalized), or
    (LINEAR, LINEAR) when the cut is degenerate."""
    x1, y1, x2, y2 = pts
    lo, hi = 0.0, 1.0
    for _ in range(40):
        t = (lo + hi) / 2
        if _bez(0.0, x1, x2, 1.0, t) < x:
            lo = t
        else:
            hi = t
    t = (lo + hi) / 2
    y = _bez(0.0, y1, y2, 1.0, t)

    def lerp(a, b):
        return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
    p0, p1, p2, p3 = (0.0, 0.0), (x1, y1), (x2, y2), (1.0, 1.0)
    q0, q1, q2 = lerp(p0, p1), lerp(p1, p2), lerp(p2, p3)
    r0, r1 = lerp(q0, q1), lerp(q1, q2)
    m = lerp(r0, r1)
    if x <= 1e-9 or x >= 1 - 1e-9 or y <= 1e-9 or y >= 1 - 1e-9 or abs(m[0]) < 1e-12:
        return LINEAR, LINEAR
    left = ("bezier", [q0[0] / m[0], q0[1] / m[1], r0[0] / m[0], r0[1] / m[1]])
    right = ("bezier", [(r1[0] - m[0]) / (1 - m[0]), (r1[1] - m[1]) / (1 - m[1]),
                        (q2[0] - m[0]) / (1 - m[0]), (q2[1] - m[1]) / (1 - m[1])])
    return left, right


def cut_segment(seg, f):
    """(left, right) parts of a segment cut at frame f (f0 < f < f1)."""
    f0, f1, v0, v1, curve = seg
    if curve[0] == "step":
        return (f0, f, v0, v0, STEP), (f, f1, v0, v0, STEP)
    t = (f - f0) / (f1 - f0)
    if curve[0] == "bezier":
        e = ease(curve, t)
        vm = _lerp_value(v0, v1, e)
        cl, cr = split_bezier_easing(curve[1], t)
        return (f0, f, v0, vm, cl), (f, f1, vm, v1, cr)
    vm = _lerp_value(v0, v1, t)
    return (f0, f, v0, vm, LINEAR), (f, f1, vm, v1, LINEAR)


def normalize(segs, D):
    """Contiguous segments over [0, D] from a sorted list that may have gaps
    (held) or overlaps (cut) from rounding."""
    out = []
    cur = 0
    last_v = None
    for seg in segs:
        f0, f1, v0, v1, curve = seg
        if f1 <= f0 or f1 <= cur:
            continue
        if f0 < cur:
            _, seg = cut_segment(seg, cur)
            f0, f1, v0, v1, curve = seg
        if f0 > cur:
            out.append((cur, f0, last_v, last_v, STEP))
        if f1 > D:
            if f0 >= D:
                break
            seg, _ = cut_segment(seg, D) if f0 < D < f1 else (seg, None)
            f0, f1, v0, v1, curve = seg
        out.append(seg)
        cur = f1
        last_v = v1
    if cur < D:
        out.append((cur, D, last_v, last_v, STEP))
    return out


def constant_timeline(v, D):
    return [(0, D, v, v, STEP)]


def refine(tl, frames):
    """The timeline cut at every frame of `frames` (sorted)."""
    out = []
    i = 0
    for seg in tl:
        f0, f1 = seg[0], seg[1]
        while i < len(frames) and frames[i] <= f0:
            i += 1
        j = i
        cur = seg
        while j < len(frames) and frames[j] < f1:
            left, cur = cut_segment(cur, frames[j])
            out.append(left)
            j += 1
        out.append(cur)
    return out


def combine(tls, fn, D, linear_ok=True, subdivide=4):
    """One timeline from several: fn(values) at every boundary. An interval
    where only one timeline moves keeps its curve; where several move and
    they are linear, the result is linear when `linear_ok` (sums), otherwise
    the interval is cut into `subdivide` linear pieces."""
    if not tls:
        return constant_timeline(fn([]), D)
    frames = sorted({f for tl in tls for seg in tl for f in (seg[0], seg[1])})
    refined = [refine(tl, frames) for tl in tls]
    out = []
    for j in range(len(frames) - 1):
        parts = [tl[j] for tl in refined]
        v0 = fn([p[2] for p in parts])
        v1 = fn([p[3] for p in parts])
        moving = [p for p in parts if p[2] is not None and p[3] is not None and p[2] != p[3]]
        f0, f1 = frames[j], frames[j + 1]
        if not moving or v0 == v1 and all(m[4][0] == "linear" for m in moving):
            out.append((f0, f1, v0, v0 if v0 == v1 else v1, STEP if v0 == v1 else LINEAR))
        elif len(moving) == 1:
            out.append((f0, f1, v0, v1, moving[0][4]))
        elif linear_ok and all(m[4] == moving[0][4] for m in moving):
            out.append((f0, f1, v0, v1, moving[0][4]))  # the same easing: the sum keeps it
        else:
            subs = sorted({f0, f1} | {round(f0 + (f1 - f0) * k / subdivide) for k in range(1, subdivide)})
            prev_f, prev_v = f0, v0
            for f in subs[1:]:
                v = fn([seg_value(p, f) for p in parts]) if f != f1 else v1
                out.append((prev_f, f, prev_v, v, LINEAR))
                prev_f, prev_v = f, v
    return out


def _simple_segments(anim):
    """The animation function over one simple duration, in [0, 1] time."""
    vals = anim.values
    n = len(vals)
    if n == 1:
        return [(0.0, 1.0, vals[0], vals[0], STEP)]
    mode = anim.calc_mode
    if mode == "discrete":
        kt = anim.key_times or [i / n for i in range(n)]
        out = []
        for i in range(n):
            t1 = kt[i + 1] if i + 1 < n else 1.0
            out.append((kt[i], t1, vals[i], vals[i], STEP))
        return out
    if mode == "paced":
        d = [0.0]
        for a, b in zip(vals, vals[1:]):
            d.append(d[-1] + (math.sqrt(sum((y - x) ** 2 for x, y in zip(a, b))) if not isinstance(a, str) else 1.0))
        kt = [x / d[-1] for x in d] if d[-1] > 0 else [i / (n - 1) for i in range(n)]
    else:
        kt = anim.key_times or [i / (n - 1) for i in range(n)]
    out = []
    for i in range(n - 1):
        curve = anim.key_splines[i] if anim.key_splines else LINEAR
        out.append((kt[i], kt[i + 1], vals[i], vals[i + 1], curve))
    return out


def anim_timeline(anim, D, fps):
    """The animation's contribution over the document as a timeline in
    frames (None where it is not active; the frozen value after its end
    with fill=freeze)."""
    if anim.kind == "animateMotion":
        return _motion_timeline(anim, D, fps)
    simple = _simple_segments(anim)
    bf = int(round(anim.begin * fps))
    end_t = anim.end
    end_f = D if math.isinf(end_t) else int(round(end_t * fps))
    end_f = min(end_f, D)
    segs = []
    if bf > 0:
        segs.append((0, bf, None, None, STEP))
    start_f = max(bf, 0)
    freeze = None
    if anim.dur is None:  # set with an indefinite duration
        v = anim.values[0]
        if end_f > start_f:
            segs.append((start_f, end_f, v, v, STEP))
        freeze = v
    else:
        d = anim.dur
        k = 0
        if anim.begin < 0:
            k = int(math.floor(-anim.begin / d))
        last_v = anim.values[-1] if anim.calc_mode != "discrete" else anim.values[-1]
        while True:
            ts = anim.begin + k * d
            if ts * fps >= end_f or ts >= anim.end - 1e-12:
                break
            for t0, t1, v0, v1, curve in simple:
                f0 = int(round((ts + t0 * d) * fps))
                f1 = int(round((ts + t1 * d) * fps))
                seg = (f0, f1, v0, v1, curve)
                if f1 <= start_f or f0 >= end_f or f1 <= f0:
                    if f0 < end_f <= f1 and f1 > f0:
                        freeze = seg_value(seg, end_f) if end_f < f1 else v1
                    continue
                if f0 < start_f:
                    _, seg = cut_segment(seg, start_f)
                if seg[1] > end_f:
                    seg, _ = cut_segment(seg, end_f)
                    freeze = seg[3]
                else:
                    freeze = seg[3]
                segs.append(seg)
            k += 1
            if k > 1000000:
                raise ConvertError(f"{anim.what()}: too many repeats")
        if freeze is None:
            freeze = last_v
    if end_f < D:
        if anim.fill == "freeze":
            segs.append((end_f, D, freeze, freeze, STEP))
        else:
            segs.append((end_f, D, None, None, STEP))
    return normalize(sorted(segs, key=lambda s: s[0]), D)


def _motion_point(pts, cum, s):
    """Point and tangent angle (degrees) at distance s along the polyline."""
    total = cum[-1]
    if total <= 0:
        return pts[0][0], pts[0][1], 0.0
    s = max(0.0, min(total, s))
    for k in range(len(pts) - 1):
        if cum[k + 1] >= s or k == len(pts) - 2:
            d = cum[k + 1] - cum[k]
            f = 0.0 if d <= 0 else (s - cum[k]) / d
            a, b = pts[k], pts[k + 1]
            ang = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0])) if d > 0 else 0.0
            return a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, ang
    return pts[-1][0], pts[-1][1], 0.0


def _motion_timeline(anim, D, fps):
    pts = anim.motion_path
    cum = [0.0]
    for a, b in zip(pts, pts[1:]):
        cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    total = cum[-1]
    kp, kt = anim.key_points, anim.key_times
    rot = anim.rotate.lower()
    fixed_angle = None
    if rot not in ("auto", "auto-reverse"):
        v = parse_numbers(rot)
        fixed_angle = v[0] if v else 0.0

    def fraction(u):
        """Distance fraction at simple time u in [0, 1]."""
        if kp:
            for i in range(len(kp) - 1):
                if u < kt[i + 1] or i == len(kp) - 2:
                    span = kt[i + 1] - kt[i]
                    t = 0.0 if span <= 0 else max(0.0, min(1.0, (u - kt[i]) / span))
                    if anim.calc_mode == "discrete":
                        return kp[i]
                    if anim.calc_mode == "spline" and anim.key_splines:
                        t = ease(anim.key_splines[i], t)
                    return kp[i] + (kp[i + 1] - kp[i]) * t
            return kp[-1]
        if anim.calc_mode == "spline" and anim.key_splines:
            return ease(anim.key_splines[0], u)
        return u

    def value_at_u(u):
        x, y, ang = _motion_point(pts, cum, fraction(u) * total)
        if fixed_angle is not None:
            ang = fixed_angle
        elif rot == "auto-reverse":
            ang += 180.0
        return (x, y, ang)
    d = anim.dur
    bf = int(round(anim.begin * fps))
    end_f = D if math.isinf(anim.end) else min(D, int(round(anim.end * fps)))
    segs = []
    if bf > 0:
        segs.append((0, bf, None, None, STEP))
    start_f = max(bf, 0)
    prev = None
    for f in range(start_f, end_f + 1):
        t = f / fps - anim.begin
        if f == end_f and not math.isinf(anim.end):
            t = anim.end - anim.begin
        u = (t / d) % 1.0
        if t > 0 and u == 0.0 and f == end_f:
            u = 1.0
        v = value_at_u(u)
        if prev is not None:
            segs.append((f - 1, f, prev, v, LINEAR))
        prev = v
    if end_f < D:
        segs.append((end_f, D, prev, prev, STEP) if anim.fill == "freeze" else (end_f, D, None, None, STEP))
    return normalize(segs, D)


def sandwich(anims, base, D, fps, discrete=False):
    """The SMIL sandwich of the animations of one attribute: in priority
    order (begin time, then document order) a replace animation sets the
    value and a sum one adds to it; the base value where none is active."""
    anims = sorted(anims, key=lambda a: (a.begin, a.order))
    tls = [anim_timeline(a, D, fps) for a in anims]

    def fn(vals):
        acc = base
        for a, v in zip(anims, vals):
            if v is None:
                continue
            if a.additive == "sum" and not discrete and not isinstance(v, str) and acc is not None:
                acc = _add_values(acc, v)
            else:
                acc = v
        return acc
    return combine(tls, fn, D, linear_ok=True)


def divide_by_scale(w_tl, s_tl, D):
    """width(t) / scale(t) (the mean of the two scale factors), sampled every
    frame where either moves and constant elsewhere; timeline_keys() then
    merges the straight runs."""
    frames = sorted({f for tl in (w_tl, s_tl) for seg in tl for f in (seg[0], seg[1])})
    w_r, s_r = refine(w_tl, frames), refine(s_tl, frames)

    def div(w, sc):
        k = (sc[0] + sc[1]) / 2.0
        return (w[0] / k if abs(k) > 1e-9 else w[0],)
    out = []
    for j in range(len(frames) - 1):
        w, sc = w_r[j], s_r[j]
        f0, f1 = frames[j], frames[j + 1]
        if w[2] == w[3] and sc[2] == sc[3]:
            v = div(w[2], sc[2])
            out.append((f0, f1, v, v, STEP))
            continue
        prev_f, prev_v = f0, div(w[2], sc[2])
        for f in range(f0 + 1, f1 + 1):
            v = div(seg_value(w, f), seg_value(sc, f)) if f < f1 else div(w[3], sc[3])
            out.append((prev_f, f, prev_v, v, LINEAR))
            prev_f, prev_v = f, v
    return out


def merge_linear(segs, eps=1e-6):
    """Joins consecutive linear segments with the same rate (per-frame
    samples of a straight, even motion become one key)."""
    out = []
    for seg in segs:
        if out:
            p = out[-1]
            if p[4][0] == "linear" and seg[4][0] == "linear" and p[3] == seg[2] and \
                    not isinstance(seg[2], str) and p[2] is not None and seg[3] is not None:
                r1 = [(b - a) / (p[1] - p[0]) for a, b in zip(p[2], p[3])]
                r2 = [(b - a) / (seg[1] - seg[0]) for a, b in zip(seg[2], seg[3])]
                if all(abs(x - y) <= eps * max(1.0, abs(x)) for x, y in zip(r1, r2)):
                    out[-1] = (p[0], seg[1], p[2], seg[3], LINEAR)
                    continue
        out.append(seg)
    return out


def split_rotations(segs, limit=90.0):
    """Cuts segments whose angle (the first component, degrees) changes by
    more than `limit`, so that the rig's shortest-way interpolation follows
    the intended direction."""
    out = []
    split_any = False
    for seg in segs:
        f0, f1, v0, v1, curve = seg
        if v0 is None or v1 is None or curve[0] == "step" or isinstance(v0, str):
            out.append(seg)
            continue
        delta = max(abs(b - a) for a, b in zip(v0[:2], v1[:2]))
        n = int(math.ceil(delta / limit - 1e-9))
        if n <= 1 or f1 - f0 < 2:
            out.append(seg)
            continue
        split_any = True
        prev_f, prev_v = f0, v0
        for k in range(1, n + 1):
            f = f1 if k == n else int(round(f0 + (f1 - f0) * k / n))
            if f <= prev_f:
                continue
            v = v1 if k == n else seg_value(seg, f)
            out.append((prev_f, f, prev_v, v, LINEAR))
            prev_f, prev_v = f, v
    if split_any:
        note("a rotation of more than 90 degrees between keys was split into several keys")
    return out


def timeline_keys(segs, convert, base, merge=True):
    """Keys [(frame, curve spec, converted value)] of a timeline, or None
    when it never leaves the base value. A discontinuity makes two keys on
    one frame (the rig takes the later one at that frame)."""
    if merge:
        segs = merge_linear(segs)
    keys = []
    for f0, f1, v0, v1, curve in segs:
        v0c = convert(base if v0 is None else v0)
        v1c = convert(base if v1 is None else v1)
        if keys and keys[-1][0] == f0 and keys[-1][2] == v0c:
            keys[-1] = (f0, curve, v0c)  # continuous: the start replaces the end key
        else:
            keys.append((f0, curve, v0c))  # a jump: the end key stays on the same frame
        keys.append((f1, STEP, v1c))
    # Drop keys that repeat the previous value when the next one has it too
    out = []
    for i, k in enumerate(keys):
        if out and out[-1][2] == k[2] and (i + 1 >= len(keys) or keys[i + 1][2] == k[2]):
            continue
        out.append(k)
    # A key followed by the same value is a hold
    final = []
    for i, k in enumerate(out):
        nxt = out[i + 1] if i + 1 < len(out) else None
        curve = STEP if nxt is None or nxt[2] == k[2] else k[1]
        final.append((k[0], curve, k[2]))
    basec = convert(base)
    if all(k[2] == basec for k in final):
        return None
    return final


# ---------------------------------------------------------------------------
# Rig compile
#
# Bones: the root bone (the picture root transform), then for every kept
# element (animated, --keep, --keep-all with an id) a chain: one bone for its
# static transform (the whole transform list, decomposed exactly, with the
# transforms of the non-kept ancestors folded in), one per animateTransform
# (identity bind pose, keys holding the animated values: a rotate about a
# center gets a static pivot bone before it and the -cx, -cy translation
# folded into its children), one per animateMotion (sampled every frame),
# and a "position" bone when x / y / cx / cy / r / rx / ry / width / height
# are animated. Slots: one per kept drawable, per direct <image>, and one per
# run of consecutive static siblings, whose shapes collapse into a VECTOR
# picture in the bone's space. Slot timelines: ALPHA from opacity /
# fill-opacity / stroke-opacity, COLOR from fill / stroke / color (the shapes
# take the slot's current color), ATTACHMENT from visibility / display.


class RBone:
    def __init__(self, name, matrix, parent):
        self.name = name
        self.matrix = matrix  # bind pose
        self.parent = parent  # index or None
        self.timelines = {}  # channel -> keys [(frame, curve, value)]


class RSlot:
    def __init__(self, name, bone):
        self.name = name
        self.bone = bone
        self.alpha = 255
        self.color = (0, 0, 0)
        self.clip = None  # ((x, y, w, h), bone)
        self.default = 0
        self.stroke_width = 0.0  # Slot::strokeWidth (FEATURE_STROKE_WIDTH when a timeline exists)
        self.kind = "vector"  # or "image"
        self.picture = None  # CPicture
        self.image = None  # CImage
        self.local = IDENTITY  # IMAGE: the texture's pixels to the bone's space
        self.timelines = {}


class Rig:
    def __init__(self):
        self.name = ""
        self.bones = []
        self.slots = []
        self.duration = 1
        self.fps = 30
        self.bounds = (0.0, 0.0, 0.0, 0.0)
        self.anim_name = "main"
        self.signature = 0


class _Ctx:
    def __init__(self):
        self.alpha_sources = []  # (node, prop)
        self.static_alpha = 1.0
        self.vis_sources = []  # nodes with visibility / display animations
        self.clip = None  # (rect, bone)

    def copy(self):
        c = _Ctx()
        c.alpha_sources = list(self.alpha_sources)
        c.static_alpha = self.static_alpha
        c.vis_sources = list(self.vis_sources)
        c.clip = self.clip
        return c


def _lcm(a, b):
    return a * b // math.gcd(a, b)


def _linear_part(m):
    return (m[0], m[1], m[2], m[3], 0.0, 0.0)


def map_timeline(tl, fn):
    return [(f0, f1, None if v0 is None else fn(v0), None if v1 is None else fn(v1), curve)
            for f0, f1, v0, v1, curve in tl]


class RigCompiler:
    def __init__(self, doc, options, keep=(), keep_all=False, fps=30, duration=None, anim_name="main"):
        self.doc = doc
        self.opt = options
        self.comp = Compiler(doc, options)
        self.comp.animated_paint = self.paint_source
        self.comp.animated_opacity = self.prop_source
        self.keep = set(keep)
        self.keep_all = keep_all
        self.fps = fps
        self.duration = duration
        self.anim_name = anim_name
        self.anims = {}  # node -> [Anim]
        self.rig = Rig()
        self.used_bone_names = set()
        self.used_slot_names = set()
        self._needs = {}
        self._run_parent = None
        self.D = 1

    # --- names and lookups -------------------------------------------------

    def node_name(self, node):
        if node.name is None:
            if node.id:
                node.name = node.id
            elif node is self.doc.root:
                node.name = "root"
            elif node.copy_of:
                node.name = self.doc.gen_name(node.copy_of + "_")
            else:
                node.name = self.doc.gen_name(node.tag)
        return node.name

    @staticmethod
    def _unique(name, used):
        cand = name
        i = 2
        while cand in used:
            cand = f"{name}_{i}"
            i += 1
        used.add(cand)
        return cand

    def _kept(self, node):
        if node is self.doc.root:
            return True
        if node in self.anims:
            return True
        if node.id and (self.keep_all or node.id in self.keep):
            return True
        return False

    def _needs_visit(self, node):
        if node in self._needs:
            return self._needs[node]
        r = self._kept(node) or any(self._needs_visit(c) for c in node.children if c.tag not in ANIM_TAGS)
        self._needs[node] = r
        return r

    def anims_of(self, node, attr):
        return [a for a in self.anims.get(node, []) if a.attr == attr]

    def prop_source(self, node, prop):
        """The element whose animation of `prop` reaches `node` through
        inheritance, or None."""
        n = node
        while n is not None:
            if self.anims_of(n, prop):
                return n
            if prop in n.specified and n.specified[prop].strip().lower() != "inherit":
                return None
            if prop not in INHERITED:
                return None
            n = n.parent
        return None

    def paint_source(self, node, prop):
        src = self.prop_source(node, prop)
        if src is not None:
            return src
        if node.style[prop].strip().lower() == "currentcolor":
            return self.prop_source(node, "color")
        return None

    def paint_anim_prop(self, node, prop):
        """(source node, animated property) of a fill / stroke, or None."""
        src = self.prop_source(node, prop)
        if src is not None:
            return src, prop
        if node.style[prop].strip().lower() == "currentcolor":
            src = self.prop_source(node, "color")
            if src is not None:
                return src, "color"
        return None

    # --- main ----------------------------------------------------------------

    def run(self):
        for node in self.doc.root.iter():
            for el in node.anims:
                a = parse_anim(el, node)
                if a is not None:
                    self.anims.setdefault(node, []).append(a)
        for i in self.keep:
            if i not in self.doc.by_id:
                warn(f"--keep: there is no element with id '{i}'")
        self.D = self._compute_duration()
        rig = self.rig
        rig.fps = self.fps
        rig.duration = self.D
        rig.anim_name = self.anim_name
        rig.name = self.doc.root.id or "root"
        w, h = self.doc.picture_size
        rig.bounds = (0.0, 0.0, w, h)
        self._visit(self.doc.root, None, IDENTITY, _Ctx())
        if len(rig.bones) > db.MAX_BONES:
            raise ConvertError(f"{len(rig.bones)} bones; the rig allows {db.MAX_BONES} (collapse more of the "
                               "document or drop --keep / --keep-all)")
        if len(rig.slots) > db.MAX_SLOTS:
            raise ConvertError(f"{len(rig.slots)} slots; the rig allows {db.MAX_SLOTS}")
        rig.signature = db.signature([b.name for b in rig.bones], [s.name for s in rig.slots])
        return rig

    def _compute_duration(self):
        fps = self.fps
        if self.duration is not None:
            D = int(round(self.duration * fps))
        else:
            periods = []
            ends = []
            for anims in self.anims.values():
                for a in anims:
                    if math.isinf(a.active):
                        if a.dur is not None:
                            periods.append(max(1, int(round(a.dur * fps))))
                    else:
                        ends.append(a.end)
            L = 0
            for p in periods:
                L = _lcm(L, p) if L else p
            E = int(math.ceil(max(ends) * fps - 1e-9)) if ends else 0
            D = L * max(1, int(math.ceil(E / L - 1e-9))) if L else E
            cap = int(round(DURATION_CAP * fps))
            if D > cap:
                warn(f"the animations repeat every {D / fps:g} s together; the duration is capped at "
                     f"{DURATION_CAP:g} s (give --duration)")
                D = cap
        if D < 1:
            D = 1
        if D > MAX_FRAMES:
            raise ConvertError(f"{D} frames; at most {MAX_FRAMES} (lower --fps or --duration)")
        return D

    # --- bones ---------------------------------------------------------------

    def _add_bone(self, name, matrix, parent, timelines=None, what=""):
        x, y, rot_x, rot_y, sx, sy = decompose(matrix)
        if abs(sx) > Q12_MAX or abs(sy) > Q12_MAX:
            warn(f"{what}: bone {name}: scale {sx:g} x {sy:g} exceeds the Q12 range, clamped")
        b = RBone(self._unique(name, self.used_bone_names), matrix, parent)
        b.timelines = timelines or {}
        self.rig.bones.append(b)
        return len(self.rig.bones) - 1

    def _bone_chain(self, node, parent_bone, pending, own, anims, what, skip_identity=False):
        """Bones of a kept element. Returns (its bone, the matrix from its
        user space to that bone's space). With `skip_identity`, an element
        whose only bone would be an identity one gets none (its position
        bone takes its name)."""
        name = self.node_name(node)
        tanims = [a for a in anims if a.kind == "animateTransform"]
        manims = [a for a in anims if a.kind == "animateMotion"]
        static = mat_mul(pending, own)
        replaces = [a for a in tanims if a.additive == "replace"]
        items = node_transform_items(node) if node is not self.doc.root else []
        bases = {}  # the value an animateTransform replaces: its static item when of its type
        if replaces:
            static = pending
            if len(replaces) == 1 and len(items) == 1 and items[0][0] == replaces[0].type:
                bases[replaces[0]] = tuple(items[0][1])  # exact: the static value outside the active time
            elif not mat_is_identity(own, 1e-9):
                warn(f"{what}: animateTransform additive=replace: the element's static transform is dropped")
            if len(replaces) > 1:
                warn(f"{what}: several animateTransform with additive=replace are summed")
        entries = []  # (suffix, matrix, timelines)
        fold = IDENTITY
        if node is self.doc.root:
            x, y, rx, ry, sx, sy = decompose(static)
            if abs(sx) > Q12_MAX or abs(sy) > Q12_MAX:
                note("the root scale exceeds the Q12 range, it is folded into the pictures")
                entries.append(("base", IDENTITY, {}))
                fold = static
            else:
                entries.append(("base", static, {}))
        elif not mat_is_identity(static, 1e-9) or not (tanims or manims):
            if skip_identity and mat_is_identity(static, 1e-9) and not (tanims or manims):
                node.lone_position = True
                return parent_bone, IDENTITY
            entries.append(("base", static, {}))
        pending_fold = IDENTITY  # a pivot's -cx, -cy waiting for the next bone
        for a in tanims:
            tl = sandwich([a], bases.get(a, TRANSFORM_IDENTITY[a.type]), self.D, self.fps)
            if not mat_is_identity(pending_fold):
                entries.append(("unpivot", pending_fold, {}))
                pending_fold = IDENTITY
            if a.type == "translate":
                entries.append(("translate", IDENTITY, {"TRANSLATE": tl}))
            elif a.type == "scale":
                entries.append(("scale", IDENTITY, {"SCALE": tl}))
            elif a.type == "rotate":
                centers = {(round(v[1], 6), round(v[2], 6)) for s in tl for v in (s[2], s[3]) if v is not None}
                rot_tl = map_timeline(tl, lambda v: (v[0], v[0]))
                if len(centers) <= 1:
                    cx, cy = centers.pop() if centers else (0.0, 0.0)
                    if (cx, cy) != (0.0, 0.0):
                        entries.append(("pivot", mat_translate(cx, cy), {}))
                    entries.append(("rotate", IDENTITY, {"ROTATE": rot_tl}))
                    pending_fold = mat_translate(-cx, -cy)
                else:
                    entries.append(("pivot", IDENTITY, {"TRANSLATE": map_timeline(tl, lambda v: (v[1], v[2]))}))
                    entries.append(("rotate", IDENTITY, {"ROTATE": rot_tl}))
                    entries.append(("unpivot", IDENTITY,
                                    {"TRANSLATE": map_timeline(tl, lambda v: (-v[1], -v[2]))}))
            elif a.type == "skewX":
                entries.append(("skewX", IDENTITY, {
                    "ROTATE": map_timeline(tl, lambda v: (-v[0], 0.0)),
                    "SCALE": map_timeline(tl, lambda v: (1.0, 1.0 / max(1e-6, abs(math.cos(math.radians(v[0]))))))}))
            else:
                entries.append(("skewY", IDENTITY, {
                    "ROTATE": map_timeline(tl, lambda v: (0.0, v[0])),
                    "SCALE": map_timeline(tl, lambda v: (1.0 / max(1e-6, abs(math.cos(math.radians(v[0])))), 1.0))}))
        for a in manims:
            tl = anim_timeline(a, self.D, self.fps)
            if not mat_is_identity(pending_fold):
                entries.append(("unpivot", pending_fold, {}))
                pending_fold = IDENTITY
            tls = {"TRANSLATE": map_timeline(tl, lambda v: (v[0], v[1]))}
            rot = map_timeline(tl, lambda v: (v[2], v[2]))
            if any(v is not None and v[0] != 0.0 for s in rot for v in (s[2], s[3])):
                tls["ROTATE"] = rot
            entries.append(("motion", IDENTITY, tls))
        fold = mat_mul(fold, pending_fold)
        parent = parent_bone
        counts = {}
        for i, (suffix, matrix, tls) in enumerate(entries):
            if i == len(entries) - 1:
                bname = name
            else:
                counts[suffix] = counts.get(suffix, 0) + 1
                bname = f"{name}_{suffix}" + (str(counts[suffix]) if counts[suffix] > 1 else "")
            parent = self._add_bone(bname, matrix, parent, self._bone_keys(tls, what), what)
        return parent, fold

    def _bone_keys(self, tls, what):
        """Timelines (segments of floats) to rig keys per channel."""
        out = {}
        for ch, tl in tls.items():
            if ch == "TRANSLATE":
                keys = timeline_keys(tl, lambda v: (v[0], v[1]), (0.0, 0.0))
            elif ch == "ROTATE":
                tl = split_rotations(merge_linear(tl))
                keys = timeline_keys(tl, lambda v: (db.angle16(v[0]), db.angle16(v[1])), (0.0, 0.0), merge=False)
            else:
                keys = timeline_keys(tl, lambda v: (self._q12(v[0], what), self._q12(v[1], what)), (1.0, 1.0))
            if keys:
                out[ch] = keys
        return out

    @staticmethod
    def _q12(s, what):
        v = int(round(s * db.SCALE_ONE))
        if not -32768 <= v <= 32767:
            warn(f"{what}: scale {s:g} is out of the Q12 range (-8..8), clamped")
            v = max(-32768, min(32767, v))
        return v

    def _position_bone(self, node, bone, content, pos_anims, size_anims, what):
        """A bone at the element's base position with TRANSLATE / SCALE keys
        from x / y / cx / cy and r / rx / ry / width / height animations.
        Returns (bone, content matrix, shift of the geometry)."""
        vw, vh = self.doc.viewbox[2], self.doc.viewbox[3]
        axis_attrs = {"circle": ("r", "r"), "ellipse": ("rx", "ry")}.get(node.tag, ("width", "height"))
        orig = {attr: _attr_len(node, attr, 0.0, vw if attr != axis_attrs[1] else vh) for attr in set(axis_attrs)}
        # The geometry is emitted at a reference size: the base size, or the
        # largest animated one when the base is 0 or the growth exceeds x8
        # (SCALE keys are Q12 factors of the bind pose)
        for attr in sorted({a.attr for a in size_anims}):
            anims = [a for a in size_anims if a.attr == attr]
            vmax = max([abs(v[0]) for a in anims for v in a.values] + [0.0])
            base = orig.get(attr, 0.0)
            if base <= 0 and vmax <= 0:
                warn(f"{what}: {attr} is 0 and never grows, its animation is dropped")
                size_anims = [a for a in size_anims if a.attr != attr]
            elif base <= 0 or vmax > base * Q12_MAX:
                node.attrs[attr] = f"{vmax:.9g}"
                note(f"{what}: {attr} is emitted at {vmax:g} (its base is {base:g})")
        if node.tag in ("image", "text"):
            base_pos = (_attr_len(node, "x", 0.0, vw), _attr_len(node, "y", 0.0, vh))
            base_size = (_attr_len(node, "width", 0.0, vw), _attr_len(node, "height", 0.0, vh))
            if node.tag == "text":
                xs = parse_numbers(node.get("x", ""))
                ys = parse_numbers(node.get("y", ""))
                base_pos = (xs[0] if xs else 0.0, ys[0] if ys else 0.0)
        else:
            _, base_pos, base_size = element_geometry(self.doc, node)
        tls = {}
        lin = _linear_part(content)
        if pos_anims:
            axis_tls = []
            for axis, attrs in ((0, ("x", "cx")), (1, ("y", "cy"))):
                anims = [a for a in pos_anims if a.attr in attrs]
                axis_tls.append(sandwich(anims, (base_pos[axis],), self.D, self.fps) if anims
                                else constant_timeline((base_pos[axis],), self.D))
            tls["TRANSLATE"] = combine(
                axis_tls, lambda vals: mat_apply(lin, vals[0][0] - base_pos[0], vals[1][0] - base_pos[1]),
                self.D, linear_ok=True)
        if size_anims:
            axis_tls = []
            for axis in (0, 1):
                anims = [a for a in size_anims if axis in SIZE_ATTRS[a.attr]]
                b = orig.get(axis_attrs[axis], base_size[axis])  # the static value
                axis_tls.append(sandwich(anims, (b,), self.D, self.fps) if anims else constant_timeline((b,), self.D))
            tls["SCALE"] = combine(axis_tls, lambda vals: (vals[0][0] / base_size[0] if base_size[0] else 1.0,
                                                           vals[1][0] / base_size[1] if base_size[1] else 1.0),
                                   self.D, linear_ok=True)
            if (node.style["stroke"].strip().lower() != "none"
                    and self.prop_source(node, "stroke-width") is None):
                # (an animated stroke width is divided by the scale instead)
                warn(f"{what}: the size animation scales the stroke width too")
        matrix = mat_mul(content, mat_translate(base_pos[0], base_pos[1]))
        node.scale_timeline = tls.get("SCALE")  # for an animated stroke width (divided by it)
        keys = self._bone_keys(tls, what)
        name = self.node_name(node) + ("" if getattr(node, "lone_position", False) else "_pos")
        b = self._add_bone(name, matrix, bone, keys, what)
        return b, IDENTITY, base_pos

    # --- tree ------------------------------------------------------------------

    def _own_matrix(self, node):
        if node is self.doc.root:
            return self.doc.root_matrix
        m = node_matrix(node)
        if node.tag == "svg":
            vw, vh = self.doc.viewbox[2], self.doc.viewbox[3]
            m = mat_mul(m, mat_translate(_attr_len(node, "x", 0.0, vw), _attr_len(node, "y", 0.0, vh)))
            vb = parse_numbers(node.get("viewBox", ""))
            if len(vb) == 4 and node.get("width") and node.get("height"):
                m = mat_mul(m, viewport_matrix(vb, parse_length(node.get("width")), parse_length(node.get("height")),
                                               node.get("preserveAspectRatio")))
        return m

    def _visit(self, node, parent_bone, pending, ctx):
        tag = node.tag
        if tag in ANIM_TAGS or tag in IGNORED_TAGS or tag in ("defs", "clipPath", "linearGradient",
                                                               "radialGradient", "symbol"):
            return
        what = f"<{tag}{' id=' + node.id if node.id else ''}>"
        if tag in UNSUPPORTED_TAGS:
            warn(f"{what} is not supported, skipped")
            return
        if tag not in DRAWABLE_TAGS and tag not in CONTAINER_TAGS:
            warn(f"{what} is unknown, skipped")
            return
        if not is_displayed(node) and not self.anims_of(node, "display"):
            return
        anims = self.anims.get(node, [])
        own = self._own_matrix(node)
        positional = tag in DRAWABLE_TAGS and any(a.attr in POSITION_ATTRS or a.attr in SIZE_ATTRS for a in anims)
        if self._kept(node):
            bone, content = self._bone_chain(node, parent_bone, pending, own, anims, what, skip_identity=positional)
        else:
            bone, content = parent_bone, mat_mul(pending, own)
        ctx2 = ctx.copy()
        if tag not in DRAWABLE_TAGS:
            if self.anims_of(node, "opacity"):
                ctx2.alpha_sources.append((node, "opacity"))
            else:
                ov = parse_numbers(node.style["opacity"])
                ctx2.static_alpha *= max(0.0, min(1.0, ov[0] if ov else 1.0))
            for prop in ("mask", "filter"):
                if node.style[prop].strip().lower() != "none":
                    warn(f"{what}: {prop} is not supported, ignored")
        if self.anims_of(node, "visibility") or self.anims_of(node, "display"):
            ctx2.vis_sources.append(node)
        if tag not in DRAWABLE_TAGS:
            c = self.comp.clip_rect(node, what)
            if c is not None:
                if ctx2.clip is not None:
                    warn(f"{what}: nested clip paths: only the inner one is applied")
                ctx2.clip = (self.comp.map_clip(c, content, what), bone)
        if tag in DRAWABLE_TAGS:
            self._leaf_slot(node, bone, content, ctx2, anims, what)
            return
        children = [switch_child(node)] if tag == "switch" else node.children
        run = []
        self._run_parent = node if self._kept(node) else None
        for c in children:
            if c is None or c.tag in ANIM_TAGS or c.tag in IGNORED_TAGS or c.tag in (
                    "defs", "clipPath", "linearGradient", "radialGradient", "symbol"):
                continue
            if self._needs_visit(c) or c.tag == "image":
                self._flush(run, bone, content, ctx2)
                run = []
                if c.tag == "image" and not self._needs_visit(c):
                    if is_displayed(c):
                        self._leaf_slot(c, bone, mat_mul(content, node_matrix(c)), ctx2, [], "<image>")
                else:
                    self._visit(c, bone, content, ctx2)
            else:
                run.append(c)
        self._flush(run, bone, content, ctx2)

    def _flush(self, run, bone, content, ctx):
        """One slot with a VECTOR picture of a run of static siblings (named
        after the kept parent for its first run, else after the first
        sibling)."""
        if not run:
            return
        shapes = []
        for c in run:
            self.comp.subtree(c, content, 1.0, None, shapes)
        if not shapes:
            return
        parent = self._run_parent
        self._run_parent = None
        name = self.node_name(parent) if parent is not None and parent is run[0].parent else self.node_name(run[0])
        pic = CPicture(shapes, union_bounds([sh.picture_bounds() for sh in shapes]))
        self._make_slot(name, bone, ctx, run[0], pic, None, shapes)

    def _leaf_slot(self, node, bone, content, ctx, anims, what):
        pos = [a for a in anims if a.attr in POSITION_ATTRS]
        size = [a for a in anims if a.attr in SIZE_ATTRS]
        shift = (0.0, 0.0)
        if pos or size:
            bone, content, shift = self._position_bone(node, bone, content, pos, size, what)
        clip = self.comp.clip_rect(node, what)
        if clip is not None:
            clip = self.comp.map_clip(clip, content, what)
        name = self.node_name(node)
        if node.tag == "image":
            href = node.get("href") or node.get("xlink:href")
            cimg = self.comp.images.load(href, what)
            if cimg is None:
                return
            vw, vh = self.doc.viewbox[2], self.doc.viewbox[3]
            iw, ih = cimg.image.size
            x, y = _attr_len(node, "x", 0.0, vw), _attr_len(node, "y", 0.0, vh)
            w = _attr_len(node, "width", float(iw), vw) if node.get("width") not in (None, "auto") else float(iw)
            h = _attr_len(node, "height", float(ih), vh) if node.get("height") not in (None, "auto") else float(ih)
            if w <= 0 or h <= 0:
                return
            local = mat_mul(content, mat_mul(mat_translate(-shift[0], -shift[1]),
                                             image_matrix(x, y, w, h, iw, ih, node.get("preserveAspectRatio"))))
            if clip is not None:
                note(f"{what}: the clip of an image attachment is applied to its slot")
            ctx = ctx.copy()
            if clip is not None:
                ctx.clip = (clip, bone)
            self._make_slot(name, bone, ctx, node, None, node, [], image=(cimg, local))
            return
        shapes = self.comp.leaf(node, content, 1.0, clip, shift=shift, skip_own_opacity=True)
        if not shapes:
            return
        pic = CPicture(shapes, union_bounds([sh.picture_bounds() for sh in shapes]))
        self._make_slot(name, bone, ctx, node, pic, node, shapes)

    # --- slots ---------------------------------------------------------------

    def _make_slot(self, name, bone, ctx, color_node, picture, own_node, shapes, image=None):
        slot = RSlot(self._unique(name, self.used_slot_names), bone)
        slot.clip = ctx.clip
        if image is not None:
            slot.kind = "image"
            slot.image, slot.local = image
        else:
            slot.picture = picture
        what = f"slot {slot.name}"
        # Alpha
        sources = list(ctx.alpha_sources)
        static = ctx.static_alpha
        if own_node is not None:
            if self.anims_of(own_node, "opacity"):
                sources.append((own_node, "opacity"))
            else:
                ov = parse_numbers(own_node.style["opacity"])
                static *= max(0.0, min(1.0, ov[0] if ov else 1.0))
        for sh in shapes:
            if sh.source is None:
                continue
            for prop, brush in (("fill-opacity", sh.fill), ("stroke-opacity", sh.stroke)):
                if brush is None:
                    continue
                src = self.prop_source(sh.source, prop)
                if src is not None and (src, prop) not in sources:
                    sources.append((src, prop))
                    if sh.fill is not None and sh.stroke is not None:
                        warn(f"{what}: {prop} is animated on a shape with both fill and stroke; "
                             "both fade (slot alpha)")
        tls = []
        bases = []
        for src, prop in sources:
            base = base_value(self.anims_of(src, prop)[0])
            bases.append(base[0])
            tls.append(sandwich(self.anims_of(src, prop), base, self.D, self.fps))
        base_alpha = static
        for b in bases:
            base_alpha *= b
        slot.alpha = max(0, min(255, int(round(base_alpha * 255))))
        if tls:
            def alpha_fn(vals):
                a = static
                for v in vals:
                    a *= v[0]
                return (a,)
            tl = combine(tls, alpha_fn, self.D, linear_ok=False)
            keys = timeline_keys(tl, lambda v: (max(0, min(255, int(round(v[0] * 255)))),), (base_alpha,))
            if keys:
                slot.timelines["ALPHA"] = keys
        # Color
        c = parse_color(color_node.style["color"]) or (0.0, 0.0, 0.0, 1.0)
        slot.color = (round(c[0]), round(c[1]), round(c[2]))
        color_src = None
        for sh in shapes:
            if sh.source is None:
                continue
            for flag, prop in ((1, "fill"), (2, "stroke")):
                if not sh.flags & flag:
                    continue
                sp = self.paint_anim_prop(sh.source, prop)
                if sp is None:
                    continue
                if color_src is None:
                    color_src = sp
                elif sp != color_src:
                    warn(f"{what}: several animated colors (fill / stroke / color) in one slot; "
                         f"{color_src[1]} of <{color_src[0].tag}> is used for all of them")
        if color_src is not None:
            src, prop = color_src
            anims = self.anims_of(src, prop)
            base = base_value(anims[0])
            slot.color = tuple(max(0, min(255, int(round(v)))) for v in base)
            tl = sandwich(anims, base, self.D, self.fps)
            keys = timeline_keys(tl, lambda v: tuple(max(0, min(255, int(round(x)))) for x in v), base)
            if keys:
                slot.timelines["COLOR"] = keys
        # Stroke width
        width_src = None
        for sh in shapes:
            if sh.source is None or not sh.flags & 4:
                continue
            src = self.prop_source(sh.source, "stroke-width")
            if src is None:
                continue
            if width_src is None:
                width_src = src
            elif src is not width_src:
                warn(f"{what}: several animated stroke widths in one slot; that of <{width_src.tag}> is used")
        if width_src is not None:
            anims = self.anims_of(width_src, "stroke-width")
            base = base_value(anims[0])
            slot.stroke_width = base[0]
            tl = map_timeline(sandwich(anims, base, self.D, self.fps), lambda v: (max(0.0, v[0]),))
            scale_tl = getattr(own_node, "scale_timeline", None) if own_node is not None else None
            if scale_tl is not None:
                tl = divide_by_scale(tl, scale_tl, self.D)
                note(f"{what}: the stroke width keys are divided by the size animation's scale, per frame")
            keys = timeline_keys(tl, lambda v: (v[0],), base)
            if keys:
                slot.timelines["STROKE_WIDTH"] = keys
        # Attachment (visibility / display)
        vis = list(ctx.vis_sources)
        if own_node is not None and own_node not in vis and (self.anims_of(own_node, "visibility") or
                                                           self.anims_of(own_node, "display")):
            vis.append(own_node)
        tls = []
        for src in vis:
            for prop in ("visibility", "display"):
                anims = self.anims_of(src, prop)
                if anims:
                    tls.append(sandwich(anims, base_value(anims[0]), self.D, self.fps, discrete=True))
        if tls:
            def vis_fn(vals):
                return (-1,) if any(v in ("hidden", "collapse", "none") for v in vals) else (0,)
            tl = combine(tls, vis_fn, self.D)
            keys = timeline_keys(tl, lambda v: v, (0,))
            if keys:
                # Not interpolated: only the changes matter
                pruned = []
                for f, _, v in keys:
                    if pruned and pruned[-1][2] == v:
                        continue
                    if pruned and pruned[-1][0] == f:
                        pruned[-1] = (f, STEP, v)
                    else:
                        pruned.append((f, STEP, v))
                slot.timelines["ATTACHMENT"] = pruned
                slot.default = pruned[0][2][0] if pruned[0][0] == 0 else 0
        self.rig.slots.append(slot)
        return slot


def compile_rig(doc, options, keep=(), keep_all=False, fps=30, duration=None, anim_name="main"):
    return RigCompiler(doc, options, keep, keep_all, fps, duration, anim_name).run()


# ---------------------------------------------------------------------------
# Emitter

VG = "shapoco::gfx2d::vg"
RIG = "shapoco::gfx2d::rig"
GFX = "shapoco::gfx2d"
VG_FORMAT_VERSION = 1  # the vg::FORMAT_VERSION the generated headers need
RIG_FORMAT_VERSION = 3  # rig: Slot::clip / VECTOR / COLOR (2), Slot::strokeWidth / STROKE_WIDTH (3)
fmt_float = db.fmt_float
c_string = db.c_string
CAP_ENUM = {"butt": "BUTT", "round": "ROUND", "square": "SQUARE"}
JOIN_ENUM = {"miter": "MITER", "round": "ROUND", "bevel": "BEVEL"}
BONE_CHANNELS = ("TRANSLATE", "ROTATE", "SCALE")
SLOT_CHANNELS = ("ATTACHMENT", "ALPHA", "COLOR", "STROKE_WIDTH")


def color_literal(c):
    r, g, b, a = c
    return f"0x{a:02X}{r:02X}{g:02X}{b:02X}u"


def _matrix_literal(m):
    return "{" + ", ".join(fmt_float(v) for v in m) + "}"


def _rect_literal(r):
    return "{" + ", ".join(fmt_float(v) for v in r) + "}"


def _wrap(values, per_line, indent="  "):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append(indent + ", ".join(values[i:i + per_line]) + ",")
    return lines


class Emitter:
    def __init__(self, namespace, options):
        self.namespace = namespace
        self.opt = options
        self.names = db.Names()
        self.names.used.update(("OP_MOVE", "OP_LINE", "OP_QUAD", "OP_CUBIC", "OP_CLOSE", "shapes", "picture",
                                "bones", "slots", "armature", "animations", "ANIMATION_COUNT"))
        self.body = []
        self.bytes = 0
        self.image_names = {}
        self.image_count = 0
        self.stops_names = {}
        self.has_paths = False
        self.stats = {"shapes": 0, "ops": 0, "images": 0, "gradients": 0, "texts": 0}

    # --- pictures ----------------------------------------------------------

    def image(self, cimg):
        """The Texture of an image (emitted once per distinct image)."""
        if cimg.key in self.image_names:
            return self.image_names[cimg.key]
        name = self.names.get(f"img{self.image_count}")
        self.image_count += 1
        w, h, stride, data, elem = conv.convert(cimg.image, self.opt.image_format, self.opt.dither)
        self.body += [""] + conv.format_array(name + "Data", data, elem)
        self.body += conv.format_texture(name, self.opt.image_format, w, h, stride, name + "Data")
        self.bytes += stride * h + 16
        self.stats["images"] += 1
        self.image_names[cimg.key] = (name, w, h)
        cimg.name = name
        return self.image_names[cimg.key]

    def gradient(self, g, name):
        key = tuple(g.stops)
        if key in self.stops_names:
            sn = self.stops_names[key]
        else:
            sn = self.names.get("stops" + name[len("gradient"):])
            self.stops_names[key] = sn
            self.body += ["", f"static const {VG}::GradientStop {sn}[] = {{"]
            for off, c in g.stops:
                self.body.append(f"  {{{fmt_float(off)}, {color_literal(c)}}},")
            self.body.append("};")
            self.bytes += 8 * len(g.stops)
        gn = self.names.get(name)
        self.body.append(f"static const {VG}::Gradient {gn} = {{{VG}::GradientKind::{g.kind.upper()}, "
                         f"{VG}::Spread::{g.spread.upper()}, {len(g.stops)}, 0, {sn}, {_matrix_literal(g.matrix)}}};")
        self.bytes += 32
        self.stats["gradients"] += 1
        return gn

    def brush(self, b):
        if b is None:
            return "{0x00000000u, nullptr}"
        return f"{{{color_literal(b.color)}, {'&' + b.gradient_name if b.gradient else 'nullptr'}}}"

    def picture(self, pic, infix, pic_name):
        """Emits a CPicture and returns its C++ name."""
        def nm(base, i):
            return f"{base}{infix}{i}"
        shape_lines = []
        for i, sh in enumerate(pic.shapes):
            self.stats["shapes"] += 1
            data = "nullptr"
            kind = sh.kind.upper()
            if sh.kind == "path":
                self.has_paths = True
                p = sh.data
                if len(p.ops) > 65535 or len(p.coords) > 65535:
                    raise ConvertError(f"a path has {len(p.ops)} ops / {len(p.coords)} coordinates; at most 65535")
                pn = self.names.get(nm("path", i))
                self.body += ["", f"static const uint8_t {pn}Ops[] = {{"]
                self.body += _wrap([OP_NAMES[o] for o in p.ops], 10)
                self.body += ["};", f"static const float {pn}Coords[] = {{"]
                self.body += _wrap([fmt_float(v) for v in p.coords], 8)
                self.body.append("};")
                b = p.bounds() or (0.0, 0.0, 0.0, 0.0)
                rule = "EVEN_ODD" if sh.rule == "evenodd" else "NONZERO"
                self.body.append(f"static const {VG}::Path {pn} = {{{pn}Ops, {pn}Coords, {len(p.ops)}, "
                                 f"{len(p.coords)}, {VG}::FillRule::{rule}, {{0, 0, 0}}, {_rect_literal(b)}}};")
                self.bytes += len(p.ops) + 4 * len(p.coords) + 32
                self.stats["ops"] += len(p.ops)
                data = "&" + pn
            elif sh.kind == "image":
                name, w, h = self.image(sh.data)
                data = "&" + name
            else:
                tn = self.names.get(nm("text", i))
                self.body += ["", f"static const {VG}::Text {tn} = {{{c_string(sh.data.text)}, "
                                  f"{self.opt.text_font}, {fmt_float(sh.data.x)}, {fmt_float(sh.data.y)}}};"]
                self.bytes += 16 + len(sh.data.text) + 1
                self.stats["texts"] += 1
                data = "&" + tn
            for b, suffix in ((sh.fill, ""), (sh.stroke, "s")):
                if b is not None and b.gradient is not None:
                    b.gradient_name = self.gradient(b.gradient, nm("gradient", i) + suffix)
            clip = "nullptr"
            if sh.clip is not None:
                cn = self.names.get(nm("clip", i))
                self.body.append(f"static const {GFX}::RectF {cn} = {_rect_literal(sh.clip)};")
                self.bytes += 16
                clip = "&" + cn
            w, cap, join, miter = sh.stroke_style
            style = (f"{{{fmt_float(w)}, {VG}::LineCap::{CAP_ENUM[cap]}, {VG}::LineJoin::{JOIN_ENUM[join]}, "
                     f"{{0, 0}}, {fmt_float(miter)}}}")
            shape_lines.append(f"  {{{VG}::ShapeKind::{kind}, {sh.flags}, {{0, 0}}, {data}, {self.brush(sh.fill)}, "
                               f"{self.brush(sh.stroke)}, {style}, {_matrix_literal(sh.transform)}, {clip}}},")
            self.bytes += 64
        sn = self.names.get("shapes" + infix.rstrip("_")) if infix else "shapes"
        if shape_lines:
            self.body += ["", f"static const {VG}::Shape {sn}[] = {{"] + shape_lines + ["};"]
        pn = self.names.get(pic_name) if pic_name != "picture" else "picture"
        self.body.append(f"static const {VG}::Picture {pn} = {{{sn if shape_lines else 'nullptr'}, "
                         f"{len(pic.shapes)}, 0, {_rect_literal(pic.bounds)}}};")
        self.bytes += 24
        return pn

    # --- rig -----------------------------------------------------------------

    def rig(self, rig):
        body = self.body
        att_names = []
        clip_names = []
        for s in rig.slots:
            if s.kind == "image":
                name, w, h = self.image(s.image)
                an = self.names.get("attachments_" + s.name)
                body += ["", f"static const {RIG}::Attachment {an}[] = {{",
                         f"  {{&{name}, {{0, 0, {w}, {h}}}, {_matrix_literal(s.local)}, nullptr, 0, "
                         f"{RIG}::AttachmentKind::IMAGE, {{0, 0}}, nullptr}},", "};"]
            else:
                pn = self.picture(s.picture, f"_{s.name}_", "picture_" + s.name)
                an = self.names.get("attachments_" + s.name)
                body += ["", f"static const {RIG}::Attachment {an}[] = {{",
                         f"  {{nullptr, {{0, 0, 0, 0}}, {_matrix_literal(IDENTITY)}, nullptr, 0, "
                         f"{RIG}::AttachmentKind::VECTOR, {{0, 0}}, &{pn}}},", "};"]
            self.bytes += 56
            att_names.append(an)
            if s.clip is not None:
                cn = self.names.get("clip_" + s.name)
                body.append(f"static const {GFX}::RectF {cn} = {_rect_literal(s.clip[0])};")
                clip_names.append((cn, s.clip[1]))
                self.bytes += 16
            else:
                clip_names.append(None)
        body += ["", f"static const {RIG}::Bone bones[] = {{"]
        for b in rig.bones:
            x, y, rx, ry, sx, sy = decompose(b.matrix)
            body.append(f"  {{{c_string(b.name)}, {fmt_float(x)}, {fmt_float(y)}, {db.angle16(rx)}, {db.angle16(ry)}, "
                        f"{RigCompiler._q12(sx, b.name)}, {RigCompiler._q12(sy, b.name)}, "
                        f"{'0xFF' if b.parent is None else b.parent}, 0}},")
        body.append("};")
        self.bytes += 24 * len(rig.bones)
        body += ["", f"static const {RIG}::Slot slots[] = {{"]
        for s, an, cn in zip(rig.slots, att_names, clip_names):
            clip = f"&{cn[0]}, {cn[1]}" if cn else "nullptr, 0"
            body.append(f"  {{{c_string(s.name)}, {an}, 1, {s.default}, {s.bone}, {s.alpha}, "
                        f"{GFX}::BlendMode::ALPHA, {s.color[0]}, {s.color[1]}, {s.color[2]}, {clip}, {{0, 0, 0}}, "
                        f"{fmt_float(s.stroke_width)}}},")
        body.append("};")
        self.bytes += 32 * len(rig.slots)
        has_width = any("STROKE_WIDTH" in s.timelines for s in rig.slots)
        arm_features = f"{RIG}::FEATURE_SLOT_COLOR" + (f" | {RIG}::FEATURE_STROKE_WIDTH" if has_width else "")
        body += ["", f"static const {RIG}::Armature armature = {{",
                 f"  {c_string(rig.name)}, bones, slots, {len(rig.bones)}, {len(rig.slots)},",
                 "  false, 0x00000000u,",
                 f"  {_rect_literal(rig.bounds)},",
                 f"  0x{rig.signature:08X}u,",
                 f"  {arm_features},",
                 "};"]
        # Animation
        an = self.names.get("anim_" + rig.anim_name)
        curves = []
        curve_ids = {}

        def curve_id(spec):
            if spec[0] == "step":
                return db.CURVE_STEP
            table = db.curve_table(spec)
            if table is None:
                return db.CURVE_LINEAR
            if table not in curve_ids:
                if len(curves) >= db.CURVE_MAX:
                    raise ConvertError(f"more than {db.CURVE_MAX} distinct easing curves")
                curve_ids[table] = len(curves)
                curves.append(table)
            return curve_ids[table]
        key_lines = []
        btl, stl = [], []
        for bi, b in enumerate(rig.bones):
            for ch in BONE_CHANNELS:
                if ch in b.timelines:
                    kn = self.names.get(f"{an}_bone{bi}_{ch.lower()}")
                    keys = b.timelines[ch]
                    typ = {"TRANSLATE": "TranslateKey", "ROTATE": "RotateKey", "SCALE": "ScaleKey"}[ch]
                    key_lines.append(f"static const {RIG}::{typ} {kn}[] = {{")
                    for f, cv, v in keys:
                        c = curve_id(cv)
                        if ch == "TRANSLATE":
                            key_lines.append(f"  {{{f}, 0x{c:02X}, 0, {fmt_float(v[0])}, {fmt_float(v[1])}}},")
                        else:
                            key_lines.append(f"  {{{f}, 0x{c:02X}, 0, {v[0]}, {v[1]}}},")
                    key_lines.append("};")
                    self.bytes += (12 if ch == "TRANSLATE" else 8) * len(keys)
                    btl.append((kn, len(keys), bi, ch))
        has_color = False
        anim_width = False
        for si, s in enumerate(rig.slots):
            for ch in SLOT_CHANNELS:
                if ch in s.timelines:
                    kn = self.names.get(f"{an}_slot{si}_{ch.lower()}")
                    keys = s.timelines[ch]
                    typ = {"ATTACHMENT": "AttachmentKey", "ALPHA": "AlphaKey", "COLOR": "ColorKey",
                           "STROKE_WIDTH": "StrokeWidthKey"}[ch]
                    key_lines.append(f"static const {RIG}::{typ} {kn}[] = {{")
                    for f, cv, v in keys:
                        if ch == "ATTACHMENT":
                            key_lines.append(f"  {{{f}, {v[0]}, 0}},")
                        elif ch == "ALPHA":
                            key_lines.append(f"  {{{f}, 0x{curve_id(cv):02X}, {v[0]}}},")
                        elif ch == "STROKE_WIDTH":
                            anim_width = True
                            key_lines.append(f"  {{{f}, 0x{curve_id(cv):02X}, 0, {fmt_float(v[0])}}},")
                        else:
                            has_color = True
                            key_lines.append(f"  {{{f}, 0x{curve_id(cv):02X}, {v[0]}, {v[1]}, {v[2]}, {{0, 0}}}},")
                    key_lines.append("};")
                    self.bytes += (8 if ch in ("COLOR", "STROKE_WIDTH") else 4) * len(keys)
                    stl.append((kn, len(keys), si, ch))
        body += ["", f"// animation {c_string(rig.anim_name)}: {rig.duration} frames at {rig.fps} fps"]
        if curves:
            body.append(f"static const {RIG}::Curve {an}_curves[] = {{")
            for t in curves:
                body.append("  {{" + ", ".join(str(v) for v in t) + "}},")
            body.append("};")
            self.bytes += 34 * len(curves)
        body += key_lines
        tl_names = {}
        for kind, tls in (("bone", btl), ("slot", stl)):
            if not tls:
                tl_names[kind] = "nullptr"
                continue
            tn = self.names.get(f"{an}_{kind}Timelines")
            tl_names[kind] = tn
            typ = "BoneTimeline" if kind == "bone" else "SlotTimeline"
            body.append(f"static const {RIG}::{typ} {tn}[] = {{")
            for kn, n, i, ch in tls:
                body.append(f"  {{{kn}, {n}, {i}, {RIG}::Channel::{ch}}},")
            body.append("};")
            self.bytes += 8 * len(tls)
        if len(btl) > 255 or len(stl) > 255:
            raise ConvertError("more than 255 bone or slot timelines")
        body += [f"static const {RIG}::Animation {an} = {{",
                 f"  {c_string(rig.anim_name)}, {rig.duration}, {rig.fps}, {len(btl)}, {len(stl)}, {len(curves)}, 0,",
                 f"  {tl_names['bone']}, {tl_names['slot']}, nullptr, {an + '_curves' if curves else 'nullptr'},",
                 f"  0x{rig.signature:08X}u,",
                 "  " + (" | ".join(([f"{RIG}::FEATURE_SLOT_COLOR"] if has_color else [])
                                    + ([f"{RIG}::FEATURE_STROKE_WIDTH"] if anim_width else [])) or "0") + ",",
                 "};",
                 "", f"static const {RIG}::Animation *const animations[] = {{&{an}}};",
                 "static constexpr int ANIMATION_COUNT = 1;"]
        self.bytes += 36
        self.anim_stats = {"boneTimelines": len(btl), "slotTimelines": len(stl), "curves": len(curves)}

    # --- the header ----------------------------------------------------------

    def header(self, guard, source, mode, summary, option_lines):
        head = [f"#ifndef {guard}", f"#define {guard}", "",
                f"// Generated by svg2cpp from {source}", f"// {summary}"]
        head += [f"// options: {o}" for o in option_lines]
        head.append("// Group opacity is multiplied into the brushes of the descendants (which differs")
        head.append("// from SVG where they overlap).")
        ws = warnings()
        if ws:
            head.append(f"// {len(ws)} warning(s):")
            for m in ws[:40]:
                head.append(f"//   {m}")
            if len(ws) > 40:
                head.append(f"//   ... {len(ws) - 40} more")
        head.append("")
        if mode == "rig":
            head.append('#include "shapoco/gfx2d/rig.hpp"')
        else:
            head.append('#include "shapoco/gfx2d/vg.hpp"')
        if self.opt.text_font != "nullptr" and self.stats["texts"]:
            head.append('#include "shapoco/gfx2d/fonts.hpp"')
        head += ["", f"static_assert({VG}::FORMAT_VERSION >= {VG_FORMAT_VERSION},",
                 '              "this header needs a newer ShapoGFX (vg.hpp FORMAT_VERSION)");']
        if mode == "rig":
            head += [f"static_assert({RIG}::FORMAT_VERSION >= {RIG_FORMAT_VERSION},",
                     '              "this header needs a newer ShapoGFX (rig.hpp FORMAT_VERSION)");']
        head += ["", f"namespace {self.namespace} {{"]
        if self.has_paths:
            head.append("")
            for v, n in sorted(OP_NAMES.items()):
                enum = ["MOVE", "LINE", "QUAD", "CUBIC", "CLOSE"][v]
                head.append(f"static constexpr uint8_t {n} = (uint8_t){VG}::PathOp::{enum};")
        tail = ["", f"}}  // namespace {self.namespace}", "", "#endif", ""]
        return "\n".join(head + self.body + tail)


def generate_picture_header(pic, namespace, guard, source, options, option_lines):
    e = Emitter(namespace, options)
    e.picture(pic, "", "picture")
    s = e.stats
    summary = (f"picture: {s['shapes']} shapes, {s['ops']} path ops, {s['gradients']} gradients, "
               f"{s['images']} images, {s['texts']} bitmap texts, about {e.bytes} bytes")
    return e.header(guard, source, "picture", summary, option_lines), e


def generate_rig_header(rig, namespace, guard, source, options, option_lines):
    e = Emitter(namespace, options)
    e.rig(rig)
    s = e.stats
    a = e.anim_stats
    summary = (f"rig: {len(rig.bones)} bones, {len(rig.slots)} slots, {s['shapes']} shapes, {s['ops']} path ops, "
               f"{s['images']} images; animation {rig.anim_name}: {rig.duration} frames at {rig.fps} fps, "
               f"{a['boneTimelines']} bone and {a['slotTimelines']} slot timelines, about {e.bytes} bytes")
    return e.header(guard, source, "rig", summary, option_lines), e


# ---------------------------------------------------------------------------
# Dump (JSON summary for the tests)


def _dump_brush(b):
    if b is None:
        return None
    out = {"color": color_literal(b.color)}
    if b.gradient is not None:
        g = b.gradient
        out["gradient"] = {"kind": g.kind, "spread": g.spread, "stops": [[o, color_literal(c)] for o, c in g.stops],
                           "toGradient": [round(v, 6) for v in g.matrix]}
    return out


def _dump_picture(pic, head_ops=6):
    shapes = []
    for sh in pic.shapes:
        d = {"kind": sh.kind, "flags": sh.flags, "transform": [round(v, 6) for v in sh.transform],
             "clip": [round(v, 6) for v in sh.clip] if sh.clip else None,
             "fill": _dump_brush(sh.fill), "stroke": _dump_brush(sh.stroke),
             "source": sh.source.id or sh.source.tag if sh.source else None}
        if sh.kind == "path":
            p = sh.data
            ops = []
            for op, c in p.segments()[:head_ops]:
                ops.append(["MLQCZ"[op]] + [round(v, 4) for v in c])
            b = p.bounds()
            d["path"] = {"opCount": len(p.ops), "coordCount": len(p.coords), "rule": sh.rule,
                         "bounds": [round(v, 6) for v in b] if b else None, "ops": ops}
            w, cap, join, miter = sh.stroke_style
            d["strokeStyle"] = {"width": round(w, 6), "cap": cap, "join": join, "miterLimit": miter}
        elif sh.kind == "image":
            d["image"] = {"width": sh.data.image.width, "height": sh.data.image.height, "name": sh.data.name}
        else:
            d["text"] = {"text": sh.data.text, "x": round(sh.data.x, 6), "y": round(sh.data.y, 6)}
        shapes.append(d)
    return {"bounds": [round(v, 6) for v in pic.bounds], "shapeCount": len(pic.shapes), "shapes": shapes}


def dump_picture(pic, namespace, source):
    return {"tool": "svg2cpp", "mode": "picture", "namespace": namespace, "source": source,
            "picture": _dump_picture(pic), "warnings": warnings()}


def dump_rig(rig, namespace, source, emitter=None):
    bones = []
    for b in rig.bones:
        x, y, rx, ry, sx, sy = decompose(b.matrix)
        bones.append({"name": b.name, "parent": b.parent, "x": round(x, 6), "y": round(y, 6),
                      "rotX": round(rx, 6), "rotY": round(ry, 6), "scaleX": round(sx, 6), "scaleY": round(sy, 6)})
    slots = []
    timelines = {}
    for b in rig.bones:
        for ch, keys in b.timelines.items():
            timelines[f"bone:{b.name}:{ch}"] = len(keys)
    for s in rig.slots:
        d = {"name": s.name, "bone": s.bone, "alpha": s.alpha, "color": list(s.color), "default": s.default,
             "strokeWidth": round(s.stroke_width, 6), "kind": s.kind, "clip": [round(v, 6) for v in s.clip[0]] if s.clip else None,
             "clipBone": s.clip[1] if s.clip else None}
        if s.kind == "vector":
            d["picture"] = _dump_picture(s.picture)
        else:
            d["image"] = {"width": s.image.image.width, "height": s.image.image.height,
                          "local": [round(v, 6) for v in s.local]}
        slots.append(d)
        for ch, keys in s.timelines.items():
            timelines[f"slot:{s.name}:{ch}"] = len(keys)
    out = {"tool": "svg2cpp", "mode": "rig", "namespace": namespace, "source": source,
           "rig": {"name": rig.name, "fps": rig.fps, "duration": rig.duration, "bounds": [round(v, 6) for v in rig.bounds],
                   "signature": f"0x{rig.signature:08X}", "bones": bones, "slots": slots, "timelines": timelines},
           "warnings": warnings()}
    if emitter is not None:
        out["rig"]["curves"] = emitter.anim_stats["curves"]
    return out
