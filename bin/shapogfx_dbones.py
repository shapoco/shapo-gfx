"""DragonBones armature conversion shared by dbones2cpp and its tests.

Reads DragonBones 5.x data (*_ske.json, and *.dbani which has the same schema),
turns it into the quantized structures of shapoco::gfx2d::rig (rig.hpp):
angles in 1/65536 turns, scales in Q12, easing curves as 17-entry Q14 tables,
and evaluates poses with the same arithmetic as src/gfx2d/rig.cpp, which makes
the evaluator the reference the C++ tests are checked against.

Conventions (DragonBones): y points down, angles are degrees clockwise, the
matrix of a transform is Flash's (x' = a x + c y + tx):
  a = cos(skY) scX, b = sin(skY) scX, c = -sin(skX) scY, d = cos(skX) scY.
Keyframe values are offsets added to the bind pose (scales multiply it).
"""

import json
import math
import os
import statistics
import sys

import numpy as np
from PIL import Image, ImageDraw

import shapogfx_imgconv as conv

SCALE_ONE = 4096
Q14_ONE = 16384
NO_PARENT = 0xFF
CURVE_LINEAR = 0xFF
CURVE_STEP = 0xFE
CURVE_MAX = 0xFD  # tables 0..252
MAX_BONES = 255
MAX_SLOTS = 255
MAX_ATTACHMENTS = 127
CURVE_SAMPLES = 17
CHANNELS = ("TRANSLATE", "ROTATE", "SCALE", "ATTACHMENT", "ALPHA")
ATLAS_WIDTHS = [1 << k for k in range(6, 12)]  # 64..2048
ATLAS_HEIGHT_MAX = 16384

f32 = np.float32

_warnings = []


def warn(msg):
    _warnings.append(msg)
    print(f"warning: {msg}", file=sys.stderr)


def warnings():
    return list(_warnings)


class ConvertError(Exception):
    pass


# ---------------------------------------------------------------------------
# Scalars


def wrap16(v):
    """int16 two's complement wrap."""
    return ((int(v) + 32768) & 0xFFFF) - 32768


def angle16(deg):
    """Degrees to 1/65536 turns, wrapped to int16 (a turn is a full wrap)."""
    return wrap16(round(deg * 65536.0 / 360.0))


def q12(s, what):
    v = round(s * SCALE_ONE)
    if not -32768 <= v <= 32767:
        warn(f"{what}: scale {s} is out of the Q12 range (-8..8), clamped")
        v = max(-32768, min(32767, v))
    return v


def fnv1a(data, h=0x811C9DC5):
    for byte in data:
        h ^= byte
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def signature(bone_names, slot_names):
    """FNV-1a 32 of the bone names, then the slot names in draw order, each
    terminated by a zero byte, the two lists separated by 0x01."""
    h = 0x811C9DC5
    for n in bone_names:
        h = fnv1a(n.encode("utf-8") + b"\0", h)
    h = fnv1a(b"\x01", h)
    for n in slot_names:
        h = fnv1a(n.encode("utf-8") + b"\0", h)
    return h


# ---------------------------------------------------------------------------
# Affine helpers (tuples a, b, c, d, tx, ty; m * n applies n first)


def mat_from(x, y, skx_rad, sky_rad, scx, scy):
    return (math.cos(sky_rad) * scx, math.sin(sky_rad) * scx,
            -math.sin(skx_rad) * scy, math.cos(skx_rad) * scy, x, y)


def mat_mul(m, n):
    a, b, c, d, tx, ty = m
    na, nb, nc, nd, ntx, nty = n
    return (a * na + c * nb, b * na + d * nb, a * nc + c * nd, b * nc + d * nd,
            a * ntx + c * nty + tx, b * ntx + d * nty + ty)


def mat_apply(m, x, y):
    a, b, c, d, tx, ty = m
    return a * x + c * y + tx, b * x + d * y + ty


def mat_inv(m):
    a, b, c, d, tx, ty = m
    det = a * d - b * c
    r = 1.0 / det
    ia, ib, ic, id_ = d * r, -b * r, -c * r, a * r
    return (ia, ib, ic, id_, -(ia * tx + ic * ty), -(ib * tx + id_ * ty))


def transform_matrix(t, scale=1.0):
    """DragonBones transform dict (degrees) to a matrix, positions scaled."""
    return mat_from(t.get("x", 0.0) * scale, t.get("y", 0.0) * scale,
                    math.radians(t.get("skX", 0.0)), math.radians(t.get("skY", 0.0)),
                    t.get("scX", 1.0), t.get("scY", 1.0))


# ---------------------------------------------------------------------------
# Easing curves


def _bezier(p0, p1, p2, p3, t):
    u = 1.0 - t
    return u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3


def bezier_y_at_x(points, x):
    """Piecewise cubic bezier from (0, 0) to (1, 1). `points` is the
    DragonBones curve array: c1x, c1y, c2x, c2y, then per further segment
    px, py, c1x, c1y, c2x, c2y (length 4 + 6 k); other lengths are taken as
    the points of a polyline. x(t) is assumed monotonic within a segment and
    solved by bisection."""
    n = len(points)
    if n < 4 or (n - 4) % 6 != 0:
        pts = [(0.0, 0.0)] + [(points[i], points[i + 1]) for i in range(0, n - 1, 2)] + [(1.0, 1.0)]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            if x <= x1:
                return y0 if x1 <= x0 else y0 + (y1 - y0) * (x - x0) / (x1 - x0)
        return 1.0
    segs = []
    start = (0.0, 0.0)
    for i in range(0, n, 6):
        c1 = (points[i], points[i + 1])
        c2 = (points[i + 2], points[i + 3])
        end = (points[i + 4], points[i + 5]) if i + 6 <= n - 4 else (1.0, 1.0)
        segs.append((start, c1, c2, end))
        start = end
    for j, (s0, c1, c2, e) in enumerate(segs):
        if x <= e[0] or j == len(segs) - 1:
            lo, hi = 0.0, 1.0
            for _ in range(40):
                t = (lo + hi) / 2
                if _bezier(s0[0], c1[0], c2[0], e[0], t) < x:
                    lo = t
                else:
                    hi = t
            return _bezier(s0[1], c1[1], c2[1], e[1], (lo + hi) / 2)
    return x


def easing_value(k, p):
    """DragonBones tweenEasing k (-2..2, not 0) at progress p."""
    if k < 0:
        f = p * p
    elif k <= 1:
        f = 1.0 - (1.0 - p) * (1.0 - p)
    else:
        f = (1.0 - math.cos(math.pi * p)) / 2.0
    return p + (f - p) * abs(k)


def curve_table(spec):
    """17 Q14 samples of a curve spec, or None for LINEAR / STEP."""
    kind = spec[0]
    if kind in ("linear", "step"):
        return None
    ys = []
    for i in range(CURVE_SAMPLES):
        x = i / (CURVE_SAMPLES - 1)
        y = bezier_y_at_x(spec[1], x) if kind == "bezier" else easing_value(spec[1], x)
        ys.append(max(-32768, min(32767, round(y * Q14_ONE))))
    ys[0] = 0
    ys[-1] = Q14_ONE
    if ys == [i * (Q14_ONE // 16) for i in range(CURVE_SAMPLES)]:
        return None  # same as linear
    return tuple(ys)


def curve_spec(frame):
    """Easing of a DragonBones frame toward the next one."""
    if frame.get("curve"):
        return ("bezier", [float(v) for v in frame["curve"]])
    te = frame.get("tweenEasing")
    if te is None:
        return ("step",)
    if te == 0:
        return ("linear",)
    return ("easing", float(te))


# ---------------------------------------------------------------------------
# Source model (floats and degrees, as read)


class Key:
    def __init__(self, frame, value, curve):
        self.frame = frame
        self.value = value  # tuple
        self.curve = curve  # spec


class Bone:
    def __init__(self, name, t, parent, length):
        self.name = name
        self.x = t.get("x", 0.0)
        self.y = t.get("y", 0.0)
        self.skx = t.get("skX", 0.0)
        self.sky = t.get("skY", 0.0)
        self.scx = t.get("scX", 1.0)
        self.scy = t.get("scY", 1.0)
        self.parent = parent  # name or None
        self.length = length


class Display:
    def __init__(self, kind, path, image, transform, pivot, frame):
        self.kind = kind
        self.path = path
        self.image = image  # RGBA PIL image of the whole frame content, or None
        self.transform = transform
        self.pivot = pivot
        self.frame = frame  # (frameX, frameY, frameWidth, frameHeight) or None


class Slot:
    def __init__(self, name, bone, z, display_index, alpha, blend, json_index):
        self.name = name
        self.bone = bone  # index
        self.z = z
        self.display_index = display_index
        self.alpha = alpha
        self.blend = blend
        self.json_index = json_index
        self.displays = []


class Animation:
    def __init__(self, name, duration):
        self.name = name
        self.duration = duration
        self.bone = {}  # bone index -> {channel: [Key]}
        self.slot = {}  # slot index -> {channel: [Key]}
        self.zorder = []  # [(frame, order tuple or None)]


class Armature:
    def __init__(self):
        self.name = ""
        self.frame_rate = 24
        self.bones = []
        self.slots = []
        self.aabb = None
        self.animations = []


def _num(d, key, default):
    v = d.get(key, default)
    return default if v is None else v


def _frames_with_starts(frames):
    t = 0
    out = []
    for fr in frames:
        out.append((t, fr))
        t += int(_num(fr, "duration", 1))
    return out


def _drop_if_identity(keys, identity):
    return None if all(k.value == identity for k in keys) else keys


def _check_unsupported(anim, what):
    for key in ("ffd", "ik", "frame", "event"):
        if anim.get(key):
            warn(f"animation {anim.get('name')}: {key} timelines are not supported, ignored{what}")


def _parse_bone_timelines(an, arm, bone_index, pos_scale):
    for tl in an.get("bone", []):
        name = tl.get("name")
        if name not in bone_index:
            warn(f"animation {an.get('name')}: unknown bone {name}, timeline ignored")
            continue
        bi = bone_index[name]
        chans = {}
        if "frame" in tl:  # 5.0: one timeline with every channel
            tr, ro, sc = [], [], []
            for start, fr in _frames_with_starts(tl["frame"]):
                t = fr.get("transform", {})
                cv = curve_spec(fr)
                if fr.get("tweenRotate"):
                    warn(f"bone {name}: tweenRotate (extra turns) is not supported, ignored")
                tr.append(Key(start, (_num(t, "x", 0.0) * pos_scale, _num(t, "y", 0.0) * pos_scale), cv))
                ro.append(Key(start, (_num(t, "skX", 0.0), _num(t, "skY", 0.0)), cv))
                sc.append(Key(start, (_num(t, "scX", 1.0), _num(t, "scY", 1.0)), cv))
            chans["TRANSLATE"] = _drop_if_identity(tr, (0.0, 0.0))
            chans["ROTATE"] = _drop_if_identity(ro, (0.0, 0.0))
            chans["SCALE"] = _drop_if_identity(sc, (1.0, 1.0))
        else:  # 5.5: one timeline per channel
            if tl.get("translateFrame"):
                keys = [Key(s, (_num(f, "x", 0.0) * pos_scale, _num(f, "y", 0.0) * pos_scale), curve_spec(f))
                        for s, f in _frames_with_starts(tl["translateFrame"])]
                chans["TRANSLATE"] = _drop_if_identity(keys, (0.0, 0.0))
            if tl.get("rotateFrame"):
                keys = []
                for s, f in _frames_with_starts(tl["rotateFrame"]):
                    if f.get("clockwise"):
                        warn(f"bone {name}: clockwise (extra turns) is not supported, ignored")
                    r = _num(f, "rotate", 0.0)
                    # Transform.rotation = skY, skew = skX - skY
                    keys.append(Key(s, (r + _num(f, "skew", 0.0), r), curve_spec(f)))
                chans["ROTATE"] = _drop_if_identity(keys, (0.0, 0.0))
            if tl.get("scaleFrame"):
                keys = [Key(s, (_num(f, "x", 1.0), _num(f, "y", 1.0)), curve_spec(f))
                        for s, f in _frames_with_starts(tl["scaleFrame"])]
                chans["SCALE"] = _drop_if_identity(keys, (1.0, 1.0))
        chans = {c: k for c, k in chans.items() if k}
        if chans:
            arm_tl = arm.setdefault(bi, {})
            arm_tl.update(chans)


def _alpha_of(color, what):
    if color:
        for k, dflt in (("rM", 100), ("gM", 100), ("bM", 100), ("rO", 0), ("gO", 0), ("bO", 0), ("aO", 0)):
            if _num(color, k, dflt) != dflt:
                warn(f"{what}: color tint ({k}) is not supported, only the alpha is used")
                break
    return max(0, min(255, round(_num(color or {}, "aM", 100) * 255 / 100)))


def _parse_slot_timelines(an, out, slots, slot_index):
    for tl in an.get("slot", []):
        name = tl.get("name")
        if name not in slot_index:
            warn(f"animation {an.get('name')}: unknown slot {name}, timeline ignored")
            continue
        si = slot_index[name]
        slot = slots[si]
        what = f"animation {an.get('name')} slot {name}"
        chans = {}
        if "frame" in tl:  # 5.0
            att, alp = [], []
            for s, f in _frames_with_starts(tl["frame"]):
                att.append(Key(s, (int(_num(f, "displayIndex", 0)),), ("step",)))
                alp.append(Key(s, (_alpha_of(f.get("color"), what),), curve_spec(f)))
            chans["ATTACHMENT"] = att
            chans["ALPHA"] = alp
        else:  # 5.5
            if tl.get("displayFrame"):
                chans["ATTACHMENT"] = [Key(s, (int(_num(f, "value", 0)),), ("step",))
                                       for s, f in _frames_with_starts(tl["displayFrame"])]
            if tl.get("colorFrame"):
                chans["ALPHA"] = [Key(s, (_alpha_of(f.get("value"), what),), curve_spec(f))
                                  for s, f in _frames_with_starts(tl["colorFrame"])]
        if "ATTACHMENT" in chans:
            for k in chans["ATTACHMENT"]:
                if k.value[0] >= len(slot.displays):
                    k.value = (-1,)
            chans["ATTACHMENT"] = _drop_if_identity(chans["ATTACHMENT"], (slot.display_index,))
        if "ALPHA" in chans:
            chans["ALPHA"] = _drop_if_identity(chans["ALPHA"], (slot.alpha,))
        chans = {c: k for c, k in chans.items() if k}
        if chans:
            out.setdefault(si, {}).update(chans)


def _zorder(pairs, json_to_new, n):
    """DragonBones zOrder array (slot, offset, ...) to draw position -> slot."""
    if not pairs:
        return None
    moves = sorted((json_to_new[pairs[i]], pairs[i + 1]) for i in range(0, len(pairs) - 1, 2)
                   if pairs[i] in json_to_new)
    order = [-1] * n
    unchanged = []
    orig = 0
    for slot, offset in moves:
        while orig < slot:
            unchanged.append(orig)
            orig += 1
        pos = orig + offset
        if not 0 <= pos < n or order[pos] != -1:
            raise ConvertError(f"zOrder moves slot {slot} out of range")
        order[pos] = orig
        orig += 1
    while orig < n:
        unchanged.append(orig)
        orig += 1
    for i in range(n - 1, -1, -1):
        if order[i] == -1:
            order[i] = unchanged.pop()
    return tuple(order)


def _parse_animation(an, arm, pos_scale, bone_index, slot_index, json_to_new):
    anim = Animation(an.get("name", "animation"), int(_num(an, "duration", 0)))
    _check_unsupported(an, "")
    _parse_bone_timelines(an, anim.bone, bone_index, pos_scale)
    _parse_slot_timelines(an, anim.slot, arm.slots, slot_index)
    zo = an.get("zOrder")
    if zo and zo.get("frame"):
        for s, f in _frames_with_starts(zo["frame"]):
            anim.zorder.append((s, _zorder(f.get("zOrder") or [], json_to_new, len(arm.slots))))
        # Leading base-order keys change nothing
        while anim.zorder and anim.zorder[0][1] is None:
            anim.zorder.pop(0)
    return anim


def _select_armature(data, name, path):
    arms = []
    for a in data.get("armature", []):
        if a.get("type", "Armature") != "Armature":
            warn(f"{path}: armature {a.get('name')} of type {a.get('type')} ignored")
            continue
        arms.append(a)
    if not arms:
        raise ConvertError(f"{path}: no armature")
    if name is None:
        return arms[0]
    for a in arms:
        if a.get("name") == name:
            return a
    raise ConvertError(f"{path}: no armature named {name}")


def _stem(path):
    base = os.path.basename(path)
    for ext in (".json", ".dbani"):
        if base.endswith(ext):
            base = base[: -len(ext)]
    if base.endswith("_ske"):
        base = base[:-4]
    return base


def _load_atlas(ske_path):
    d = os.path.dirname(os.path.abspath(ske_path))
    tex_json = os.path.join(d, _stem(ske_path) + "_tex.json")
    if not os.path.exists(tex_json):
        return None
    with open(tex_json, encoding="utf-8") as f:
        atlas = json.load(f)
    img = Image.open(os.path.join(d, atlas.get("imagePath", _stem(ske_path) + "_tex.png"))).convert("RGBA")
    subs = {s["name"]: s for s in atlas.get("SubTexture", [])}
    return img, subs


def _sub_image(atlas, sub):
    x, y, w, h = int(sub["x"]), int(sub["y"]), int(sub["width"]), int(sub["height"])
    if sub.get("rotated"):
        # Stored turned a quarter clockwise: w x h in the frame, h x w in the atlas
        return atlas.crop((x, y, x + h, y + w)).transpose(Image.ROTATE_90)
    return atlas.crop((x, y, x + w, y + h))


def load(ske_path, armature_name=None, skin_name=None, texture_dir=None, in_key=None):
    """Read a *_ske.json with its images. Positions are as in the file."""
    with open(ske_path, encoding="utf-8") as f:
        data = json.load(f)
    raw = _select_armature(data, armature_name, ske_path)
    arm = Armature()
    arm.name = raw.get("name", "Armature")
    arm.frame_rate = int(_num(raw, "frameRate", _num(data, "frameRate", 24)))
    arm.aabb = raw.get("aabb")
    if raw.get("ik"):
        warn("IK constraints are not supported, ignored")

    # Bones: parents first (by depth, keeping the file order within a depth)
    raw_bones = raw.get("bone", [])
    by_name = {}
    for b in raw_bones:
        if b.get("name") in by_name:
            raise ConvertError(f"duplicate bone name {b.get('name')}")
        by_name[b.get("name")] = b
    depth = {}

    def depth_of(name, seen):
        if name in depth:
            return depth[name]
        if name in seen:
            raise ConvertError(f"bone {name} is part of a cycle")
        seen.add(name)
        p = by_name[name].get("parent")
        if p is not None and p not in by_name:
            raise ConvertError(f"bone {name}: undefined parent {p}")
        depth[name] = 0 if p is None else depth_of(p, seen) + 1
        return depth[name]

    for b in raw_bones:
        depth_of(b["name"], set())
    order = sorted(range(len(raw_bones)), key=lambda i: depth[raw_bones[i]["name"]])
    for i in order:
        b = raw_bones[i]
        for k in ("inheritTranslation", "inheritRotation", "inheritScale", "inheritReflection"):
            if b.get(k) is False or b.get(k) == 0:
                warn(f"bone {b['name']}: {k} = false is not supported, inherits anyway")
        arm.bones.append(Bone(b["name"], b.get("transform", {}), b.get("parent"), b.get("length", 0.0)))
    if len(arm.bones) > MAX_BONES:
        raise ConvertError(f"{len(arm.bones)} bones: at most {MAX_BONES}")
    bone_index = {b.name: i for i, b in enumerate(arm.bones)}

    # Slots in draw order (stable sort by z)
    raw_slots = raw.get("slot", [])
    slots = []
    for ji, s in enumerate(raw_slots):
        if s.get("parent") not in bone_index:
            raise ConvertError(f"slot {s.get('name')}: undefined bone {s.get('parent')}")
        blend = s.get("blendMode", "normal")
        if blend not in ("normal", "add"):
            warn(f"slot {s.get('name')}: blend mode {blend} is not supported, drawn as normal")
        slots.append(Slot(s["name"], bone_index[s["parent"]], _num(s, "z", 0), int(_num(s, "displayIndex", 0)),
                          _alpha_of(s.get("color"), f"slot {s['name']}"), "add" if blend == "add" else "normal", ji))
    slots.sort(key=lambda s: s.z)
    if len(slots) > MAX_SLOTS:
        raise ConvertError(f"{len(slots)} slots: at most {MAX_SLOTS}")
    arm.slots = slots
    slot_index = {s.name: i for i, s in enumerate(slots)}
    json_to_new = {s.json_index: i for i, s in enumerate(slots)}

    # Skin
    skins = raw.get("skin", [])
    skin = None
    if skins:
        skin = skins[0]
        if skin_name is not None:
            found = [s for s in skins if s.get("name", "") == skin_name]
            if not found:
                raise ConvertError(f"no skin named {skin_name}")
            skin = found[0]
        if len(skins) > 1:
            warn(f"{len(skins)} skins: only {skin.get('name', '(default)')} is converted")
    atlas = _load_atlas(ske_path)
    tex_dir = texture_dir or os.path.join(os.path.dirname(os.path.abspath(ske_path)), _stem(ske_path) + "_texture")
    images = {}

    def image_of(path):
        if path in images:
            return images[path]
        frame = None
        if atlas is not None:
            aimg, subs = atlas
            if path not in subs:
                raise ConvertError(f"image {path} not found in the texture atlas")
            sub = subs[path]
            img = _sub_image(aimg, sub)
            if "frameWidth" in sub:
                frame = (float(_num(sub, "frameX", 0)), float(_num(sub, "frameY", 0)),
                         float(sub["frameWidth"]), float(sub["frameHeight"]))
        else:
            file = os.path.join(tex_dir, path + ".png")
            if not os.path.exists(file):
                raise ConvertError(f"image {file} not found (no {_stem(ske_path)}_tex.json either)")
            img = Image.open(file).convert("RGBA")
        if in_key is not None:
            img = conv.apply_key_color(img, in_key)
        images[path] = (img, frame)
        return images[path]

    for ss in (skin or {}).get("slot", []):
        si = slot_index.get(ss.get("name"))
        if si is None:
            warn(f"skin: unknown slot {ss.get('name')}, ignored")
            continue
        for d in ss.get("display", []):
            kind = d.get("type", "image")
            path = d.get("path", d.get("name", ""))
            pv = d.get("pivot", {})
            pivot = (_num(pv, "x", 0.5), _num(pv, "y", 0.5))
            if kind != "image":
                warn(f"slot {slots[si].name}: display {d.get('name')} of type {kind} is not supported, not drawn")
                slots[si].displays.append(Display(kind, path, None, d.get("transform", {}), pivot, None))
                continue
            img, frame = image_of(path)
            slots[si].displays.append(Display(kind, path, img, d.get("transform", {}), pivot, frame))
        if len(slots[si].displays) > MAX_ATTACHMENTS:
            raise ConvertError(f"slot {slots[si].name}: {len(slots[si].displays)} displays, at most {MAX_ATTACHMENTS}")
    for s in slots:
        if s.display_index >= len(s.displays):
            s.display_index = -1

    for an in raw.get("animation", []):
        arm.animations.append(_parse_animation(an, arm, 1.0, bone_index, slot_index, json_to_new))
    arm._bone_index = bone_index
    arm._slot_index = slot_index
    arm._json_to_new = json_to_new
    arm._raw_bones = raw_bones
    return arm


def load_extra_animations(arm, path, anim_scale="auto"):
    """Animations of a .dbani (or another _ske.json of the same armature).
    Returns the position scale applied."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    raw = _select_armature(data, arm.name, path)
    names = {b.get("name") for b in raw.get("bone", [])}
    if names != {b.name for b in arm.bones}:
        raise ConvertError(f"{path}: the bones differ from those of the armature")
    snames = {s.get("name") for s in raw.get("slot", [])}
    if snames != {s.name for s in arm.slots}:
        raise ConvertError(f"{path}: the slots differ from those of the armature")
    if anim_scale == "auto":
        lengths = {b.name: b.length for b in arm.bones}
        ratios = [lengths[b["name"]] / b["length"] for b in raw.get("bone", [])
                  if b.get("length") and lengths.get(b["name"])]
        scale = statistics.median(ratios) if ratios else 1.0
        if abs(scale - 1.0) < 0.01:
            scale = 1.0
        print(f"{os.path.basename(path)}: positions are {1.0 / scale:.4g}x those of the armature "
              f"(bone lengths); scaled by {scale:.4g}", file=sys.stderr)
    else:
        scale = float(anim_scale)
    # Slot indices of zOrder refer to the order of the slots in that file
    json_to_new = {}
    for ji, s in enumerate(raw.get("slot", [])):
        json_to_new[ji] = arm._slot_index[s["name"]]
    for an in raw.get("animation", []):
        anim = _parse_animation(an, arm, scale, arm._bone_index, arm._slot_index, json_to_new)
        for i, a in enumerate(arm.animations):
            if a.name == anim.name:
                warn(f"animation {anim.name} of {os.path.basename(path)} replaces the one of the armature")
                arm.animations[i] = anim
                break
        else:
            arm.animations.append(anim)
    return scale


# ---------------------------------------------------------------------------
# Quantized model (what the header holds)


# ---------------------------------------------------------------------------
# Fitting the images: the transparent margin is trimmed, an image may be
# turned so that its opaque pixels fill the rectangle (the smallest-area
# bounding rectangle of their convex hull is made upright, which is what
# takes the empty corners off a limb drawn diagonally), and a convex polygon
# around the opaque pixels is kept for Graphics2D::drawImage() to clip to.
# The drawn area of a transformed image is what its rectangle covers, so both
# cut the pixels that are read only to be found transparent.

HULL_MAX = 16  # Graphics2D::IMAGE_POLYGON_MAX
# Alpha from which a pixel is opaque in ARGB4444 (rounding to 4 bits: 8
# becomes 0, 9 becomes 1)
ARGB4444_OPAQUE = 9


def fit_options(rotate=False, min_gain=0.03, hull=8, alpha=ARGB4444_OPAQUE, resample=Image.BICUBIC):
    """rotate: turn images to their smallest bounding rectangle when that saves
    at least min_gain of the area; hull: at most this many vertices (0: none);
    alpha: the smallest alpha that counts as opaque (a keyed image uses the
    key threshold instead, see format_options)."""
    return {"rotate": rotate, "min_gain": min_gain, "hull": min(hull, HULL_MAX), "alpha": alpha,
            "resample": resample}


def format_options(out_format="argb4444", threshold=128, inner_max=0.05):
    """out_format: argb4444, rgb565_swapped, rgb565, or auto (argb4444 for the
    images whose translucent pixels are not just an antialiased edge,
    rgb565_swapped with the key for the rest); threshold: alpha from which a
    pixel of a keyed image is opaque; inner_max: with auto, the share of
    translucent pixels away from any transparent one above which an image keeps
    its alpha."""
    return {"out_format": out_format, "threshold": threshold, "inner_max": inner_max}


def opaque_threshold(fmt, threshold):
    """The smallest alpha the format keeps."""
    return ARGB4444_OPAQUE if fmt == "argb4444" else threshold


def choose_format(img, fmts):
    """The format of one image under format_options() `fmts`. With auto, the
    translucent pixels (alpha 1..14 after 4-bit quantization) next to a
    transparent one -- the antialiased edge, which a key threshold turns hard
    -- do not count; those away from any are meant to be seen through, and
    an image with more than inner_max of them among its visible pixels keeps
    its alpha."""
    if fmts["out_format"] != "auto":
        return fmts["out_format"]
    q = np.rint(np.asarray(img)[:, :, 3] / 17.0).astype(int)
    transparent = q == 0
    translucent = (q > 0) & (q < 15)
    if not translucent.any():
        return "rgb565_swapped"
    pad = np.pad(transparent, 1, constant_values=True)
    h, w = transparent.shape
    near = np.zeros_like(transparent)
    for dy in (0, 1, 2):
        for dx in (0, 1, 2):
            near |= pad[dy:dy + h, dx:dx + w]
    inner = int((translucent & ~near).sum())
    visible = int((~transparent).sum())
    return "argb4444" if inner > fmts["inner_max"] * visible else "rgb565_swapped"


def _opaque_points(img, threshold):
    """Corners of the outermost pixels of every row with alpha >= threshold
    (every vertex of the convex hull of the opaque pixels' areas is one of
    them); None when there is no such pixel."""
    alpha = np.asarray(img)[:, :, 3] >= threshold
    rows = np.nonzero(alpha.any(axis=1))[0]
    if len(rows) == 0:
        return None
    pts = []
    for y in rows:
        xs = np.nonzero(alpha[y])[0]
        x0, x1 = int(xs[0]), int(xs[-1]) + 1
        pts += [(x0, int(y)), (x0, int(y) + 1), (x1, int(y)), (x1, int(y) + 1)]
    return pts


def convex_hull(points):
    """Vertices of the convex hull of integer points (Andrew's monotone chain),
    without collinear points, in a consistent winding."""
    pts = sorted(set(points))
    if len(pts) <= 2:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower = []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper = []
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def _area2(poly):
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]))


def min_rect_angle(hull):
    """The angle (radians, within +-pi/4) that turns the hull so that its
    smallest-area bounding rectangle is upright, and that rectangle's area.
    A side of that rectangle lies along a side of the hull (rotating
    calipers), so only the sides' angles are tried."""
    pts = np.array(hull, dtype=np.float64)

    def area(t):
        c, s = math.cos(t), math.sin(t)
        u = pts[:, 0] * c + pts[:, 1] * s
        v = -pts[:, 0] * s + pts[:, 1] * c
        return (u.max() - u.min()) * (v.max() - v.min())

    best_t, best = 0.0, area(0.0)
    for (x0, y0), (x1, y1) in zip(hull, hull[1:] + hull[:1]):
        if x0 == x1 and y0 == y1:
            continue
        # The same rectangle comes back every quarter turn: the turn nearest
        # to upright
        t = (math.atan2(y1 - y0, x1 - x0) + math.pi / 4) % (math.pi / 2) - math.pi / 4
        a = area(t)
        if a < best - 1e-6:
            best_t, best = t, a
    return best_t, best


def _intersection(a, b, c, d):
    """Intersection of the lines a-b and c-d, or None if parallel."""
    r = (b[0] - a[0], b[1] - a[1])
    q = (d[0] - c[0], d[1] - c[1])
    den = r[0] * q[1] - r[1] * q[0]
    if abs(den) < 1e-12:
        return None
    t = ((c[0] - a[0]) * q[1] - (c[1] - a[1]) * q[0]) / den
    return (a[0] + t * r[0], a[1] + t * r[1])


def simplify_hull(hull, n_max, far):
    """Outer approximation of a convex polygon by at most n_max vertices:
    an edge is replaced by the point where its neighbors meet, the edge whose
    removal adds the least area first. None when it cannot be done (a meeting
    point farther than `far` from the origin, or none at all)."""
    poly = [(float(x), float(y)) for x, y in hull]
    while len(poly) > n_max:
        k = len(poly)
        best = None
        for i in range(k):
            a, b = poly[i - 1], poly[i]
            c, d = poly[(i + 1) % k], poly[(i + 2) % k]
            p = _intersection(a, b, c, d)
            if p is None:
                continue
            # Beyond b along a-b and beyond c along d-c, or the polygon would
            # not stay convex
            if ((p[0] - b[0]) * (b[0] - a[0]) + (p[1] - b[1]) * (b[1] - a[1]) <= 0 or
                    (p[0] - c[0]) * (c[0] - d[0]) + (p[1] - c[1]) * (c[1] - d[1]) <= 0):
                continue
            if abs(p[0]) > far or abs(p[1]) > far:
                continue
            added = abs((p[0] - b[0]) * (c[1] - b[1]) - (p[1] - b[1]) * (c[0] - b[0]))
            if best is None or added < best[0]:
                best = (added, i, p)
        if best is None:
            return None
        _, i, p = best
        j = (i + 1) % k
        poly = [p] + poly[1:i] if j == 0 else poly[:i] + [p] + poly[j + 1:]
    return poly


def _contains(poly, p):
    """Whether the convex polygon holds p (edges included)."""
    sign = 1.0 if _area2(poly) >= 0 else -1.0
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        if sign * ((x1 - x0) * (p[1] - y0) - (y1 - y0) * (p[0] - x0)) < -1e-9:
            return False
    return True


def integer_hull(poly, inside):
    """The polygon's vertices rounded to integers away from its centroid, which
    keeps it a superset; verified to hold every point of `inside`, the
    polygon being pushed outward a little and rounded again if rounding cut
    one. None if that fails or a coordinate leaves int16."""
    n = len(poly)
    cx = sum(x for x, _ in poly) / n
    cy = sum(y for _, y in poly) / n
    for push in (0.0, 0.5, 1.0, 2.0):
        r = []
        for x, y in poly:
            dx, dy = x - cx, y - cy
            d = math.hypot(dx, dy)
            if d > 0 and push > 0:
                x += dx / d * push
                y += dy / d * push
            r.append((math.floor(x) if x < cx else math.ceil(x), math.floor(y) if y < cy else math.ceil(y)))
        r = [p for i, p in enumerate(r) if p != r[i - 1]]  # merged vertices
        if len(r) < 3:
            return None
        if all(abs(v) <= 32000 for p in r for v in p) and all(_contains(r, p) for p in inside):
            return r
    return None


def _strip_ringing(img):
    """Bicubic resampling overshoots: a hard edge leaves faint pixels a pixel
    or two out. Everything farther than one pixel from a pixel at least a
    quarter opaque is made transparent, which keeps the antialiased edge."""
    arr = np.array(img)
    core = arr[:, :, 3] >= 64
    near = core.copy()
    near[1:, :] |= core[:-1, :]
    near[:-1, :] |= core[1:, :]
    near[:, 1:] |= near[:, :-1].copy()
    near[:, :-1] |= near[:, 1:].copy()
    arr[~near] = 0
    return Image.fromarray(arr, "RGBA")


def fit_image(orig, scaled, opts):
    """Trim, maybe turn, and hull one image. `scaled` is `orig` resized (the
    scale may differ per axis by the rounding of the size). Returns (image,
    M, hull, turned): M maps the new image's coordinates to those of
    `scaled`, hull the polygon as (x, y) vertices in the new image (None for
    none), turned whether the image was resampled."""
    identity = (1, 0, 0, 1, 0, 0)
    pts = _opaque_points(scaled, opts["alpha"])
    if pts is None:
        return scaled, identity, None, False
    hull = convex_hull(pts)
    xs = [x for x, _ in hull]
    ys = [y for _, y in hull]
    x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
    t = 0.0
    if opts["rotate"] and len(hull) >= 3:
        t, area = min_rect_angle(hull)
        if area > (x1 - x0) * (y1 - y0) * (1.0 - opts["min_gain"]):
            t = 0.0
    if t == 0.0:
        img = scaled.crop((x0, y0, x1, y1))
        m = (1, 0, 0, 1, x0, y0)
    else:
        c, s = math.cos(t), math.sin(t)
        u = [x * c + y * s for x, y in hull]
        v = [-x * s + y * c for x, y in hull]
        u0, v0 = math.floor(min(u)), math.floor(min(v))
        nw, nh = math.ceil(max(u)) - u0, math.ceil(max(v)) - v0
        # New coordinates -> scaled image (Flash matrix) -> original image,
        # in one resampling from the original (premultiplied, so that the
        # transparent pixels do not bleed their color)
        m = (c, s, -s, c, c * u0 - s * v0, s * u0 + c * v0)
        sx, sy = orig.width / scaled.width, orig.height / scaled.height
        pil = (m[0] * sx, m[2] * sx, m[4] * sx, m[1] * sy, m[3] * sy, m[5] * sy)
        img = orig.convert("RGBa").transform((nw, nh), Image.AFFINE, pil, resample=opts["resample"]).convert("RGBA")
        img = _strip_ringing(img)
    poly = None
    if opts["hull"] >= 3:
        pts = _opaque_points(img, opts["alpha"])
        h2 = convex_hull(pts) if pts else None
        if h2 and len(h2) >= 3:
            simple = simplify_hull(h2, opts["hull"], 4 * max(img.width, img.height) + 64)
            poly = integer_hull(simple, h2) if simple else None
            # Not worth a clip when it leaves nearly the whole rectangle
            if poly and abs(_area2(poly)) >= 2 * 0.98 * img.width * img.height:
                poly = None
    return img, m, poly, t != 0.0


class CAttachment:
    def __init__(self, image_key, image, local, w, h, hull=None, fmt="argb4444"):
        self.image_key = image_key  # dedup key of the scaled image, or None (not drawn)
        self.image = image  # scaled (and fitted) RGBA image
        self.local = local
        self.w, self.h = w, h
        self.hull = hull  # convex polygon around the opaque pixels, or None
        self.fmt = fmt  # pixel format of the image (argb4444, rgb565_swapped, rgb565)
        self.texture = None  # name of the Texture, after packing
        self.src = (0, 0, w, h)


class CAnimation:
    pass


class Compiled:
    pass


def _scaled_image(img, scale):
    if scale == 1.0:
        return img
    w = max(1, round(img.width * scale))
    h = max(1, round(img.height * scale))
    # Premultiplied, so that transparent pixels do not bleed their color
    return img.convert("RGBa").resize((w, h), Image.LANCZOS).convert("RGBA")


def compile_armature(arm, scale=1.0, fit=None, fmts=None):
    """fit: fit_options(), or None to keep the images as they are; fmts:
    format_options() (default: argb4444)."""
    c = Compiled()
    c.name = arm.name
    c.frame_rate = arm.frame_rate
    c.scale = scale
    c.fit = {"images": 0, "turned": 0, "hulls": 0, "texels_before": 0, "texels_after": 0, "hull_area": 0.0,
             "keyed": 0}
    fmts = fmts or format_options()
    c.threshold = fmts["threshold"]
    c.bones = []
    for b in arm.bones:
        parent = NO_PARENT if b.parent is None else arm._bone_index[b.parent]
        c.bones.append({
            "name": b.name, "x": b.x * scale, "y": b.y * scale,
            "rotX": angle16(b.skx), "rotY": angle16(b.sky),
            "scaleX": q12(b.scx, f"bone {b.name}"), "scaleY": q12(b.scy, f"bone {b.name}"),
            "parent": parent,
        })
    scaled = {}
    c.slots = []
    for s in arm.slots:
        atts = []
        for d in s.displays:
            if d.image is None:
                atts.append(CAttachment(None, None, (1, 0, 0, 1, 0, 0), 0, 0))
                continue
            key = (d.path, id(d.image))
            if key not in scaled:
                simg = _scaled_image(d.image, scale)
                fmt = choose_format(simg, fmts)
                if fit:
                    fitted, fm, hull, turned = fit_image(
                        d.image, simg, dict(fit, alpha=opaque_threshold(fmt, fmts["threshold"])))
                else:
                    fitted, fm, hull, turned = simg, None, None, False
                scaled[key] = (simg, fitted, fm, hull, fmt)
                c.fit["images"] += 1
                c.fit["keyed"] += fmt != "argb4444"
                c.fit["turned"] += turned
                c.fit["hulls"] += hull is not None
                c.fit["texels_before"] += simg.width * simg.height
                c.fit["texels_after"] += fitted.width * fitted.height
                c.fit["hull_area"] += abs(_area2(hull)) / 2 if hull else fitted.width * fitted.height
            simg, fitted, fm, hull, fmt = scaled[key]
            ow, oh = d.image.size
            if d.frame is not None:
                fx, fy, fw, fh = d.frame
            else:
                fx, fy, fw, fh = 0.0, 0.0, float(ow), float(oh)
            m = transform_matrix(d.transform, scale)
            # The frame's pivot to the origin; the trimmed content sits at
            # (-frameX, -frameY) in the frame
            m = mat_mul(m, (1, 0, 0, 1, (-d.pivot[0] * fw - fx) * scale, (-d.pivot[1] * fh - fy) * scale))
            sx, sy = ow * scale / simg.width, oh * scale / simg.height
            if sx != 1.0 or sy != 1.0:
                m = mat_mul(m, (sx, 0, 0, sy, 0, 0))
            # The fitted image's coordinates to the scaled image's
            if fm is not None:
                m = mat_mul(m, fm)
            atts.append(CAttachment(key, fitted, m, fitted.width, fitted.height, hull, fmt))
        c.slots.append({
            "name": s.name, "attachments": atts, "default": s.display_index, "bone": s.bone,
            "alpha": s.alpha, "blend": "ADD" if s.blend == "add" else "ALPHA",
        })
    c.signature = signature([b.name for b in arm.bones], [s.name for s in arm.slots])
    c.animations = [_compile_animation(a, c, scale) for a in arm.animations]
    if arm.aabb:
        a = arm.aabb
        c.bounds = (a.get("x", 0.0) * scale, a.get("y", 0.0) * scale,
                    a.get("width", 0.0) * scale, a.get("height", 0.0) * scale)
    else:
        c.bounds = None
    return c


def _compile_animation(a, c, scale):
    ca = CAnimation()
    ca.name = a.name
    if a.duration > 65535:
        raise ConvertError(f"animation {a.name}: {a.duration} frames, at most 65535")
    ca.duration = a.duration
    ca.frame_rate = c.frame_rate
    ca.curves = []
    curve_ids = {}

    def curve_id(spec):
        if spec[0] == "step":
            return CURVE_STEP
        table = curve_table(spec)
        if table is None:
            return CURVE_LINEAR
        if table not in curve_ids:
            if len(ca.curves) >= CURVE_MAX:
                raise ConvertError(f"animation {a.name}: more than {CURVE_MAX} distinct curves")
            curve_ids[table] = len(ca.curves)
            ca.curves.append(table)
        return curve_ids[table]

    def keys_of(keys, conv_value):
        out = []
        if keys[0].frame != 0:
            out.append((0, CURVE_STEP, conv_value(keys[0].value)))
        for k in keys:
            if k.frame > 65535:
                raise ConvertError(f"animation {a.name}: key at frame {k.frame}")
            out.append((k.frame, curve_id(k.curve), conv_value(k.value)))
        # The last key's curve is never used
        out[-1] = (out[-1][0], CURVE_STEP, out[-1][2])
        return out

    conv_fns = {
        "TRANSLATE": lambda v: (v[0] * scale, v[1] * scale),
        "ROTATE": lambda v: (angle16(v[0]), angle16(v[1])),
        "SCALE": lambda v: (q12(v[0], f"animation {a.name}"), q12(v[1], f"animation {a.name}")),
        "ATTACHMENT": lambda v: (v[0],),
        "ALPHA": lambda v: (v[0],),
    }
    ca.bone_timelines = []
    for bi in sorted(a.bone):
        for ch in CHANNELS:
            if ch in a.bone[bi]:
                ca.bone_timelines.append((bi, ch, keys_of(a.bone[bi][ch], conv_fns[ch])))
    ca.slot_timelines = []
    for si in sorted(a.slot):
        for ch in CHANNELS:
            if ch in a.slot[si]:
                ca.slot_timelines.append((si, ch, keys_of(a.slot[si][ch], conv_fns[ch])))
    if len(ca.bone_timelines) > 255 or len(ca.slot_timelines) > 255:
        raise ConvertError(f"animation {a.name}: more than 255 bone or slot timelines")
    ca.draw_order_keys = list(a.zorder)
    ca.signature = c.signature
    return ca


# ---------------------------------------------------------------------------
# Evaluation, with the arithmetic of src/gfx2d/rig.cpp


def _ease(curves, curve, t):
    """Q14 easing of float32 progress t."""
    if curve == CURVE_STEP:
        return 0
    if curve == CURVE_LINEAR:
        return int(t * f32(16384.0))
    y = curves[curve]
    u = t * f32(16.0)
    k = min(int(u), 15)
    return y[k] + int(f32(y[k + 1] - y[k]) * (u - f32(k)))


def _sample(keys, frame, curves):
    """(value a, value b, e) with the interpolation a + (b - a) e / 16384."""
    n = len(keys)
    i = 0
    while i + 1 < n and keys[i + 1][0] <= frame:
        i += 1
    if i + 1 >= n or frame < keys[i][0]:
        return keys[i][2], keys[i][2], 0
    k0, k1 = keys[i], keys[i + 1]
    t = (f32(frame) - f32(k0[0])) / f32(k1[0] - k0[0])
    return k0[2], k1[2], _ease(curves, k0[1], t)


def _lerp_f(a, b, e):
    return float(f32(a) + (f32(b) - f32(a)) * (f32(e) * f32(1.0 / 16384.0)))


def _lerp_i(a, b, e):
    return a + (((b - a) * e) >> 14)


def _lerp_angle(a, b, e):
    return wrap16(a + ((wrap16(b - a) * e) >> 14))


# sin over a quarter turn in 256 steps, Q15 (SIN_Q15 of rig.cpp)
SIN_Q15 = [round(32767 * math.sin(i * math.pi / 512)) for i in range(257)]


def sin_q15(angle):
    """sin of an angle in 1/65536 turns, Q15, like sinQ15() of rig.cpp."""
    angle &= 0xFFFF
    quadrant, r = angle >> 14, angle & 0x3FFF
    pos = 16384 - r if quadrant & 1 else r
    i, f = pos >> 6, pos & 63
    v = SIN_Q15[i]
    if f:
        v += ((SIN_Q15[i + 1] - v) * f) >> 6
    return -v if quadrant & 2 else v


def local_matrix(p):
    k = f32(1.0 / (32767.0 * SCALE_ONE))
    sx, sy = f32(p["scaleX"]) * k, f32(p["scaleY"]) * k
    cy, sny = sin_q15(p["rotY"] + 16384), sin_q15(p["rotY"])
    cx, snx = sin_q15(p["rotX"] + 16384), sin_q15(p["rotX"])
    return (float(f32(cy) * sx), float(f32(sny) * sx), float(-f32(snx) * sy), float(f32(cx) * sy),
            float(f32(p["x"])), float(f32(p["y"])))


def evaluate(c, anim, frame, visitor=None):
    """Pose of the compiled armature `c` at `frame` (anim None: bind pose).
    Returns dict(world, attachment, alpha, order, slot_aabb, bounds)."""
    if anim is not None:
        frame = float(f32(min(max(frame, 0.0), float(anim.duration))))
    btl = {}
    stl = {}
    curves = []
    if anim is not None:
        curves = anim.curves
        for bi, ch, keys in anim.bone_timelines:
            btl.setdefault(bi, []).append((ch, keys))
        for si, ch, keys in anim.slot_timelines:
            stl.setdefault(si, []).append((ch, keys))
    world = []
    for i, b in enumerate(c.bones):
        p = {k: b[k] for k in ("x", "y", "rotX", "rotY", "scaleX", "scaleY")}
        for ch, keys in btl.get(i, []):
            va, vb, e = _sample(keys, frame, curves)
            if ch == "TRANSLATE":
                p["x"] = float(f32(p["x"]) + f32(_lerp_f(va[0], vb[0], e)))
                p["y"] = float(f32(p["y"]) + f32(_lerp_f(va[1], vb[1], e)))
            elif ch == "ROTATE":
                p["rotX"] = wrap16(p["rotX"] + _lerp_angle(va[0], vb[0], e))
                p["rotY"] = wrap16(p["rotY"] + _lerp_angle(va[1], vb[1], e))
            elif ch == "SCALE":
                p["scaleX"] = wrap16((p["scaleX"] * _lerp_i(va[0], vb[0], e)) >> 12)
                p["scaleY"] = wrap16((p["scaleY"] * _lerp_i(va[1], vb[1], e)) >> 12)
        if visitor:
            visitor(i, p)
        m = local_matrix(p)
        world.append(m if b["parent"] == NO_PARENT else mat_mul(world[b["parent"]], m))
    attachment, alpha = [], []
    for si, s in enumerate(c.slots):
        att, alp = s["default"], s["alpha"]
        for ch, keys in stl.get(si, []):
            va, vb, e = _sample(keys, frame, curves)
            if ch == "ATTACHMENT":
                att = va[0]
            elif ch == "ALPHA":
                alp = max(0, min(255, _lerp_i(va[0], vb[0], e)))
        attachment.append(att)
        alpha.append(alp)
    order = list(range(len(c.slots)))
    if anim is not None:
        for kf, ko in anim.draw_order_keys:
            if kf <= frame:
                order = list(ko) if ko is not None else list(range(len(c.slots)))
    aabbs = []
    bx0 = by0 = bx1 = by1 = None
    for si, s in enumerate(c.slots):
        att = attachment[si]
        if att < 0 or att >= len(s["attachments"]) or s["attachments"][att].image_key is None:
            aabbs.append(None)
            continue
        a = s["attachments"][att]
        m = mat_mul(world[s["bone"]], a.local)
        pts = [mat_apply(m, x, y) for x, y in ((0, 0), (a.w, 0), (0, a.h), (a.w, a.h))]
        x0 = math.floor(min(p[0] for p in pts))
        y0 = math.floor(min(p[1] for p in pts))
        x1 = math.ceil(max(p[0] for p in pts))
        y1 = math.ceil(max(p[1] for p in pts))
        aabbs.append((x0, y0, x1, y1))
        bx0 = x0 if bx0 is None else min(bx0, x0)
        by0 = y0 if by0 is None else min(by0, y0)
        bx1 = x1 if bx1 is None else max(bx1, x1)
        by1 = y1 if by1 is None else max(by1, y1)
    bounds = (0, 0, 0, 0) if bx0 is None else (bx0, by0, bx1 - bx0, by1 - by0)
    return {"world": world, "attachment": attachment, "alpha": alpha, "order": order,
            "slot_aabb": aabbs, "bounds": bounds}


def render_preview(c, anim, frame, margin=8, background=(40, 40, 48, 255)):
    """PIL rendering of a pose (bilinear, not the pixel formats)."""
    pose = evaluate(c, anim, frame)
    x, y, w, h = pose["bounds"]
    if c.bounds is not None:
        bx, by, bw, bh = c.bounds
        x0, y0 = min(x, math.floor(bx)), min(y, math.floor(by))
        x1, y1 = max(x + w, math.ceil(bx + bw)), max(y + h, math.ceil(by + bh))
        x, y, w, h = x0, y0, x1 - x0, y1 - y0
    W, H = int(w) + 2 * margin, int(h) + 2 * margin
    canvas = Image.new("RGBA", (W, H), background)
    view = (1, 0, 0, 1, -x + margin, -y + margin)
    for si in pose["order"]:
        s = c.slots[si]
        att = pose["attachment"][si]
        if att < 0 or att >= len(s["attachments"]):
            continue
        a = s["attachments"][att]
        if a.image_key is None or pose["alpha"][si] == 0:
            continue
        m = mat_mul(view, mat_mul(pose["world"][s["bone"]], a.local))
        ia, ib, ic, id_, itx, ity = mat_inv(m)
        src = a.image
        if a.fmt != "argb4444":
            # What the key threshold leaves: opaque or nothing
            r, g, b, al = src.split()
            src = Image.merge("RGBA", (r, g, b, al.point(lambda v: 255 if v >= c.threshold else 0)))
        if a.hull:
            # What the runtime clips to
            mask = Image.new("L", src.size, 0)
            ImageDraw.Draw(mask).polygon([(x, y) for x, y in a.hull], fill=255)
            r, g, b, al = src.split()
            src = Image.merge("RGBA", (r, g, b, Image.fromarray(np.minimum(np.asarray(al), np.asarray(mask)))))
        layer = src.transform((W, H), Image.AFFINE, (ia, ic, itx, ib, id_, ity), resample=Image.BILINEAR)
        if pose["alpha"][si] < 255:
            r, g, b, al = layer.split()
            al = al.point(lambda v, k=pose["alpha"][si]: v * k // 255)
            layer = Image.merge("RGBA", (r, g, b, al))
        if s["blend"] == "ADD":
            lay = np.asarray(layer, dtype=np.float64)
            base = np.asarray(canvas, dtype=np.float64).copy()
            base[:, :, :3] = np.minimum(255.0, base[:, :, :3] + lay[:, :, :3] * lay[:, :, 3:4] / 255.0)
            canvas = Image.fromarray(base.round().astype(np.uint8), "RGBA")
        else:
            canvas.alpha_composite(layer)
    return canvas


def dump_pose(c, frames):
    out = {"bones": [b["name"] for b in c.bones], "slots": [s["name"] for s in c.slots], "animations": {}}
    for anim in c.animations:
        rows = []
        for f in frames:
            p = evaluate(c, anim, f)
            rows.append({"frame": f, "world": [list(m) for m in p["world"]], "attachment": p["attachment"],
                         "alpha": p["alpha"], "order": p["order"], "bounds": list(p["bounds"])})
        out["animations"][anim.name] = rows
    return out


# ---------------------------------------------------------------------------
# Atlas and pixels


def shelf_pack(sizes, width):
    """Positions of (w, h) boxes packed in shelves of `width`, tallest first,
    and the height used; None if a box is wider."""
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0], i))
    pos = [None] * len(sizes)
    x = y = shelf_h = 0
    for i in order:
        w, h = sizes[i]
        if w > width:
            return None, 0
        if x + w > width:
            y += shelf_h
            x = shelf_h = 0
        if shelf_h == 0:
            shelf_h = h
        pos[i] = (x, y)
        x += w
    return pos, y + shelf_h


def choose_atlas(sizes, width="auto"):
    candidates = ATLAS_WIDTHS if width == "auto" else [int(width)]
    best = None
    for w in candidates:
        pos, h = shelf_pack(sizes, w)
        if pos is None:
            continue
        if best is None or w * h < best[0] * best[2]:
            best = (w, pos, h)
    if best is None:
        raise ConvertError("an image is wider than the atlas")
    if best[2] > ATLAS_HEIGHT_MAX:
        raise ConvertError(f"atlas {best[0]}x{best[2]}: taller than {ATLAS_HEIGHT_MAX} rows")
    return best


def convert_pixels(img, fmt, dither, out_key, threshold):
    """Pixels of an RGBA image in `fmt` (argb4444, or rgb565(_swapped) with
    transparent pixels in the key color). Returns (data, stride, escaped)."""
    if fmt == "argb4444":
        w, h, stride, data, _ = conv.convert(img, fmt, dither)
        return data, stride, 0
    arr = np.array(img.convert("RGBA"))
    opaque = arr[:, :, 3] >= threshold
    rgb = arr[:, :, :3].copy()
    rgb[~opaque] = out_key
    w, h, stride, data, _ = conv.convert(Image.fromarray(rgb, "RGB"), fmt, dither)
    kr, kg, kb = out_key
    key = ((kr >> 3) << 11) | ((kg >> 2) << 5) | (kb >> 3)
    swapped = fmt == "rgb565_swapped"

    def native(v):
        return ((v & 0xFF) << 8) | (v >> 8) if swapped else v

    escaped = 0
    flat = opaque.flatten()
    for i, v in enumerate(data):
        if not flat[i]:
            data[i] = native(key)
        elif native(v) == key:
            data[i] = native(key ^ 1)  # blue LSB
            escaped += 1
    return data, stride, escaped


# ---------------------------------------------------------------------------
# C++ output


def fmt_float(f):
    f = float(f32(f))
    if f == 0.0:
        return "0.0f"
    s = f"{f:.7g}"
    if "e" in s or "E" in s:
        mant, exp = s.split("e")
        if "." not in mant:
            mant += ".0"
        return f"{mant}e{exp}f"
    if "." not in s:
        s += ".0"
    return s + "f"


def c_string(s):
    out = []
    for byte in s.encode("utf-8"):
        ch = chr(byte)
        if ch == "\\" or ch == '"':
            out.append("\\" + ch)
        elif 32 <= byte < 127:
            out.append(ch)
        else:
            out.append(f"\\{byte:03o}")
    return '"' + "".join(out) + '"'


class Names:
    def __init__(self):
        self.used = set()

    def get(self, base):
        name = conv.sanitize_identifier(base)
        cand = name
        i = 2
        while cand in self.used:
            cand = f"{name}_{i}"
            i += 1
        self.used.add(cand)
        return cand


RIG = "shapoco::gfx2d::rig"
# The rig::FORMAT_VERSION the generated headers need (the members they initialize)
RIG_FORMAT_VERSION = 3


def generate_header(c, namespace, guard, opts, sources):
    """C++ header text. opts: dict(out_format, out_key, dither, threshold,
    atlas_width). Returns (text, stats)."""
    names = Names()
    lines = []
    w = lines.append
    stats = {"bytes": 0, "escaped": 0}

    # Images: one per distinct scaled image, with its format (one per image
    # with --out-format auto)
    images = {}
    formats = {}
    for s in c.slots:
        for a in s["attachments"]:
            if a.image_key is not None and a.image_key not in images:
                images[a.image_key] = a.image
                formats[a.image_key] = a.fmt
    keys = list(images)
    fmt_list = [f for f in ("argb4444", "rgb565_swapped", "rgb565") if f in formats.values()]
    keyed = any(f != "argb4444" for f in fmt_list)
    tex_lines = []
    atlas_desc = ""
    if opts["atlas_width"] in (0, "0"):
        tex_names = {}
        for k in keys:
            img = images[k]
            base = names.get("tex_" + os.path.basename(k[0]))
            data, stride, esc = convert_pixels(img, formats[k], opts["dither"], opts["out_key"], opts["threshold"])
            stats["escaped"] += esc
            tex_lines += [""] + conv.format_array(base + "Data", data, "uint16_t")
            tex_lines += conv.format_texture(base, formats[k], img.width, img.height, stride, base + "Data")
            stats["bytes"] += stride * img.height
            tex_names[k] = (base, (0, 0))
        atlas_desc = f"{len(keys)} textures"
    else:
        # One atlas per format: `atlas`, or `atlas` (ARGB4444) and
        # `atlasKeyed` when the formats are mixed
        tex_names = {}
        descs = []
        for fmt in fmt_list:
            fkeys = [k for k in keys if formats[k] == fmt]
            sizes = [images[k].size for k in fkeys]
            aw, pos, ah = choose_atlas(sizes, opts["atlas_width"])
            ah = max(ah, 1)
            atlas = Image.new("RGBA", (aw, ah), (0, 0, 0, 0))
            for k, p in zip(fkeys, pos):
                atlas.paste(images[k], p)
            data, stride, esc = convert_pixels(atlas, fmt, opts["dither"], opts["out_key"], opts["threshold"])
            stats["escaped"] += esc
            name = "atlas" if len(fmt_list) == 1 or fmt == "argb4444" else "atlasKeyed"
            names.used.update((name, name + "Data"))
            tex_lines += [""] + conv.format_array(name + "Data", data, "uint16_t")
            tex_lines += conv.format_texture(name, fmt, aw, ah, stride, name + "Data")
            stats["bytes"] += stride * ah
            tex_names.update({k: (name, p) for k, p in zip(fkeys, pos)})
            used = sum(sz[0] * sz[1] for sz in sizes)
            descs.append(f"{name} {aw}x{ah} ({used} of {aw * ah} pixels used)")
        atlas_desc = ", ".join(descs)
    for s in c.slots:
        for a in s["attachments"]:
            if a.image_key is not None:
                a.texture, (ox, oy) = tex_names[a.image_key]
                a.src = (ox, oy, a.w, a.h)
    if stats["escaped"]:
        warn(f"{stats['escaped']} opaque pixels had the key color: blue LSB flipped")

    body = tex_lines
    # Hulls: one per image that has one
    hull_names = {}
    for s in c.slots:
        for a in s["attachments"]:
            if a.hull is None or a.image_key in hull_names:
                continue
            hn = names.get("hull_" + os.path.basename(a.image_key[0]))
            hull_names[a.image_key] = (hn, len(a.hull))
            body += ["", f"static const int16_t {hn}[] = {{"
                     + ", ".join(f"{x}, {y}" for x, y in a.hull) + "};"]
            stats["bytes"] += 4 * len(a.hull)
    # Attachments
    att_names = []
    for s in c.slots:
        if not s["attachments"]:
            att_names.append("nullptr")
            continue
        n = names.get("attachments_" + s["name"])
        att_names.append(n)
        body += ["", f"static const {RIG}::Attachment {n}[] = {{"]
        for a in s["attachments"]:
            tex = f"&{a.texture}" if a.image_key is not None else "nullptr"
            loc = ", ".join(fmt_float(v) for v in a.local)
            hn, hc = hull_names.get(a.image_key, ("nullptr", 0))
            # kind IMAGE, pad, ext: reserved for later kinds of attachments
            body.append(f"  {{{tex}, {{{a.src[0]}, {a.src[1]}, {a.src[2]}, {a.src[3]}}}, {{{loc}}}, {hn}, {hc}, "
                        f"{RIG}::AttachmentKind::IMAGE, {{0, 0}}, nullptr}},")
        body.append("};")
        stats["bytes"] += 56 * len(s["attachments"])
    names.used.update(("bones", "slots", "armature", "animations", "ANIMATION_COUNT"))
    body += ["", f"static const {RIG}::Bone bones[] = {{"]
    for b in c.bones:
        body.append(f"  {{{c_string(b['name'])}, {fmt_float(b['x'])}, {fmt_float(b['y'])}, "
                    f"{b['rotX']}, {b['rotY']}, {b['scaleX']}, {b['scaleY']}, "
                    f"{'0xFF' if b['parent'] == NO_PARENT else b['parent']}, 0}},")  # 0: reserved flags
    body.append("};")
    stats["bytes"] += 24 * len(c.bones)
    body += ["", f"static const {RIG}::Slot slots[] = {{"]
    for s, an in zip(c.slots, att_names):
        body.append(f"  {{{c_string(s['name'])}, {an}, {len(s['attachments'])}, {s['default']}, {s['bone']}, "
                    f"{s['alpha']}, shapoco::gfx2d::BlendMode::{s['blend']}, 255, 255, 255, "
                    "nullptr, 0, {0, 0, 0}, 0.0f},")  # white (no tint), no clip, no stroke width
    body.append("};")
    stats["bytes"] += 28 * len(c.slots)
    if c.bounds is not None:
        bounds = c.bounds
    else:
        bounds = evaluate(c, None, 0)["bounds"]
    key = opts["out_key"]
    key_hex = f"0xFF{key[0]:02X}{key[1]:02X}{key[2]:02X}u" if keyed else "0x00000000u"
    body += ["", f"static const {RIG}::Armature armature = {{",
             f"  {c_string(c.name)}, bones, slots, {len(c.bones)}, {len(c.slots)},",
             f"  {'true' if keyed else 'false'}, {key_hex},",
             "  {" + ", ".join(fmt_float(v) for v in bounds) + "},",
             f"  0x{c.signature:08X}u,",
             "  0,  // features (reserved)",
             "};"]

    anim_names = []
    for a in c.animations:
        an = names.get("anim_" + a.name)
        anim_names.append(an)
        body.append("")
        body.append(f"// animation {c_string(a.name)}: {a.duration} frames at {a.frame_rate} fps")
        if a.curves:
            body.append(f"static const {RIG}::Curve {an}_curves[] = {{")
            for t in a.curves:
                body.append("  {{" + ", ".join(str(v) for v in t) + "}},")
            body.append("};")
            stats["bytes"] += 34 * len(a.curves)

        def key_array(prefix, index, ch, keys):
            kn = names.get(f"{an}_{prefix}{index}_{ch.lower()}")
            typ = {"TRANSLATE": "TranslateKey", "ROTATE": "RotateKey", "SCALE": "ScaleKey",
                   "ATTACHMENT": "AttachmentKey", "ALPHA": "AlphaKey"}[ch]
            body.append(f"static const {RIG}::{typ} {kn}[] = {{")
            for f, cv, v in keys:
                if ch == "TRANSLATE":
                    body.append(f"  {{{f}, 0x{cv:02X}, 0, {fmt_float(v[0])}, {fmt_float(v[1])}}},")
                elif ch in ("ROTATE", "SCALE"):
                    # the 0 is RotateKey::turns (reserved) / ScaleKey::pad
                    body.append(f"  {{{f}, 0x{cv:02X}, 0, {v[0]}, {v[1]}}},")
                elif ch == "ATTACHMENT":
                    body.append(f"  {{{f}, {v[0]}, 0}},")
                else:
                    body.append(f"  {{{f}, 0x{cv:02X}, {v[0]}}},")
            body.append("};")
            stats["bytes"] += {"TRANSLATE": 12, "ROTATE": 8, "SCALE": 8}.get(ch, 4) * len(keys)
            return kn

        btl = [(bi, ch, key_array("bone", bi, ch, keys)) for bi, ch, keys in a.bone_timelines]
        stl = [(si, ch, key_array("slot", si, ch, keys)) for si, ch, keys in a.slot_timelines]
        timelines = {}
        for kind, tls, src in (("bone", btl, a.bone_timelines), ("slot", stl, a.slot_timelines)):
            if not tls:
                timelines[kind] = "nullptr"
                continue
            tn = names.get(f"{an}_{kind}Timelines")
            timelines[kind] = tn
            typ = "BoneTimeline" if kind == "bone" else "SlotTimeline"
            body.append(f"static const {RIG}::{typ} {tn}[] = {{")
            for (i, ch, kn), (_, _, keys) in zip(tls, src):
                body.append(f"  {{{kn}, {len(keys)}, {i}, {RIG}::Channel::{ch}}},")
            body.append("};")
            stats["bytes"] += 8 * len(tls)
        dok = "nullptr"
        if a.draw_order_keys:
            order_names = []
            for k, (f, order) in enumerate(a.draw_order_keys):
                if order is None:
                    order_names.append("nullptr")
                    continue
                on = names.get(f"{an}_order{k}")
                order_names.append(on)
                body.append(f"static const uint8_t {on}[] = {{" + ", ".join(str(v) for v in order) + "};")
                stats["bytes"] += len(order)
            dok = names.get(f"{an}_drawOrderKeys")
            body.append(f"static const {RIG}::DrawOrderKey {dok}[] = {{")
            for (f, _), on in zip(a.draw_order_keys, order_names):
                body.append(f"  {{{f}, {on}}},")
            body.append("};")
            stats["bytes"] += 8 * len(a.draw_order_keys)
        body += [f"static const {RIG}::Animation {an} = {{",
                 f"  {c_string(a.name)}, {a.duration}, {a.frame_rate}, "
                 f"{len(a.bone_timelines)}, {len(a.slot_timelines)}, {len(a.curves)}, {len(a.draw_order_keys)},",
                 f"  {timelines['bone']}, {timelines['slot']}, {dok}, {an + '_curves' if a.curves else 'nullptr'},",
                 f"  0x{a.signature:08X}u,",
                 "  0,  // features (reserved)",
                 "};"]
        stats["bytes"] += 36
    body += ["", f"static const {RIG}::Animation *const animations[] = {{"
             + (", ".join("&" + n for n in anim_names) if anim_names else "nullptr") + "};",
             f"static constexpr int ANIMATION_COUNT = {len(anim_names)};"]

    head = [f"#ifndef {guard}", f"#define {guard}", "",
            f"// Generated by dbones2cpp from {', '.join(sources)}",
            f"// armature {c_string(c.name)}: {len(c.bones)} bones, {len(c.slots)} slots, "
            f"{sum(len(s['attachments']) for s in c.slots)} attachments, {len(c.animations)} animation(s)",
            f"// scale {c.scale:g}, " + ", ".join(
                f"{f}" + (f" (key #{key[0]:02X}{key[1]:02X}{key[2]:02X})" if f != "argb4444" else "")
                + (f" for {sum(1 for v in formats.values() if v == f)} images" if len(fmt_list) > 1 else "")
                for f in fmt_list)
            + f", {atlas_desc}, about {stats['bytes']} bytes",
            ]
    if c.fit["images"]:
        f = c.fit
        head.append(f"// images: {f['texels_before']} pixels trimmed to {f['texels_after']}"
                    f" ({f['turned']} of {f['images']} turned), {f['hulls']} hulls"
                    f" enclosing {round(f['hull_area'])} pixels")
    ws = warnings()
    if ws:
        head.append(f"// {len(ws)} warning(s):")
        for m in ws[:20]:
            head.append(f"//   {m}")
        if len(ws) > 20:
            head.append(f"//   ... {len(ws) - 20} more")
    # The members this output initializes exist from rig format 1 on
    head += ["", '#include "shapoco/gfx2d/rig.hpp"', "",
             f"static_assert({RIG}::FORMAT_VERSION >= {RIG_FORMAT_VERSION},",
             '              "this header needs a newer ShapoGFX (rig.hpp FORMAT_VERSION)");',
             "", f"namespace {namespace} {{"]
    tail = ["", f"}}  // namespace {namespace}", "", "#endif", ""]
    return "\n".join(head + body + tail), stats
