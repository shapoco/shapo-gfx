#!/usr/bin/env python3
"""Generate the DragonBones test armature and its headers for test/rig_test.cpp.

Writes test/data/test_rig_ske.json (5.0 animation format, images in
test_rig_texture/), test/data/test_rig_anim.dbani (the same armature at twice
the size, with the animation "extra") and test/data/test_rig55/ (the same
armature in the 5.5 animation format with a texture atlas), then runs:

  bin/dbones2cpp --atlas-width 64 --dump-pose 0,3.5,7,9.25,12 \\
      test/data/test_rig_ske.json test/data/test_rig_anim.dbani test/data/test_rig.hpp
  bin/dbones2cpp --atlas-width 0 --namespace test_rig_sep \\
      test/data/test_rig_ske.json test/data/test_rig_sep.hpp
  bin/dbones2cpp --out-format rgb565_swapped --out-key '#FF00FF' --namespace test_rig_keyed \\
      test/data/test_rig_ske.json test/data/test_rig_keyed.hpp

checks that the 5.5 / atlas variant converts to the same header, and turns
test_rig_pose.json (the tool's reference poses) into test_rig_expected.hpp.

  python3 test/tools/make_test_rig.py
"""

import json
import os
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, "..", "data"))
TOOL = os.path.normpath(os.path.join(HERE, "..", "..", "bin", "dbones2cpp"))

IMAGES = [("a", 8, 6), ("b", 5, 9), ("c", 7, 7), ("d", 4, 4)]


def pixel(index, x, y, w, h):
    # Keep in sync with rigImagePixel() in test/rig_test.cpp
    r = (40 + 60 * index + 13 * x) % 256
    g = (200 + 70 * index + 17 * y) % 256
    b = (90 * index + 7 * x * y) % 256
    corner = (x == 0 or x == w - 1) and (y == 0 or y == h - 1)
    edge = x == 0 or y == 0 or x == w - 1 or y == h - 1
    return (r, g, b, 0 if corner else (136 if edge else 255))


def make_image(index, w, h):
    img = Image.new("RGBA", (w, h))
    for y in range(h):
        for x in range(w):
            img.putpixel((x, y), pixel(index, x, y, w, h))
    return img


def armature(k, animations, skin=True):
    """The armature with positions multiplied by k."""
    def t(**v):
        return {n: (x * k if n in ("x", "y") else x) for n, x in v.items()}
    arm = {
        "type": "Armature", "name": "Armature", "frameRate": 24,
        "bone": [
            {"name": "root", "transform": t(x=20, y=16)},
            {"name": "a", "parent": "root", "length": 12 * k, "transform": t(x=6, y=-2, skX=30, skY=30)},
            {"name": "b", "parent": "a", "length": 8 * k, "transform": t(x=12, skX=40, skY=25)},
            {"name": "c", "parent": "root", "length": 6 * k, "transform": t(x=-6, y=8, scX=1.5, scY=0.75)},
        ],
        # Definition order differs from the draw order (z)
        "slot": [
            {"name": "sa", "parent": "a", "z": 2},
            {"name": "sb", "parent": "b", "z": 0},
            {"name": "sc", "parent": "c", "z": 3, "color": {"aM": 50}},
            {"name": "sroot", "parent": "root", "z": 1, "blendMode": "add"},
        ],
        "animation": animations,
    }
    if skin:
        arm["skin"] = [{"name": "", "slot": [
            {"name": "sa", "display": [{"name": "a", "path": "a", "transform": t(x=4, skX=10, skY=10)}]},
            {"name": "sb", "display": [{"name": "b", "path": "b", "pivot": {"x": 0, "y": 0}},
                                       {"name": "d", "path": "d", "transform": t(x=2)}]},
            {"name": "sc", "display": [{"name": "c", "path": "c"}]},
            {"name": "sroot", "display": [{"name": "d", "path": "d", "transform": t(y=-3)}]},
        ]}]
    return arm


ZORDER = {"frame": [{"duration": 7}, {"duration": 5, "zOrder": [0, 1]}]}  # sa after sc

MOVE_50 = {
    "name": "move", "duration": 12, "playTimes": 0,
    "bone": [
        {"name": "a", "frame": [
            {"duration": 4, "curve": [0.5, 0, 0.5, 1], "transform": {}},
            {"duration": 4, "tweenEasing": 0, "transform": {"x": 3, "y": -2, "skX": 30, "skY": 30}},
            {"duration": 4, "transform": {"x": 1, "y": 1, "skX": -20, "skY": -20}},  # no tween
            {"duration": 0, "transform": {}},
        ]},
        {"name": "b", "frame": [
            {"duration": 6, "tweenEasing": 0, "transform": {}},
            {"duration": 6, "tweenEasing": 0, "transform": {"skX": -350, "skY": -350}},  # = +10
            {"duration": 0, "transform": {}},
        ]},
        {"name": "c", "frame": [
            {"duration": 6, "tweenEasing": 0, "transform": {}},
            {"duration": 6, "tweenEasing": 0, "transform": {"scX": 1.5, "scY": 0.5}},
            {"duration": 0, "transform": {}},
        ]},
    ],
    "slot": [
        {"name": "sb", "frame": [{"duration": 6, "displayIndex": 0}, {"duration": 6, "displayIndex": 1},
                                 {"duration": 0, "displayIndex": 1}]},
        {"name": "sa", "frame": [{"duration": 8, "tweenEasing": 0, "color": {}},
                                 {"duration": 4, "color": {"aM": 30}}]},
    ],
    "zOrder": ZORDER,
}

MOVE_55 = {
    "name": "move", "duration": 12, "playTimes": 0,
    "bone": [
        {"name": "a",
         "translateFrame": [
             {"duration": 4, "curve": [0.5, 0, 0.5, 1]},
             {"duration": 4, "tweenEasing": 0, "x": 3, "y": -2},
             {"duration": 4, "x": 1, "y": 1},
             {"duration": 0}],
         "rotateFrame": [
             {"duration": 4, "curve": [0.5, 0, 0.5, 1]},
             {"duration": 4, "tweenEasing": 0, "rotate": 30},
             {"duration": 4, "rotate": -20},
             {"duration": 0}]},
        {"name": "b", "rotateFrame": [
            {"duration": 6, "tweenEasing": 0},
            {"duration": 6, "tweenEasing": 0, "rotate": -350},
            {"duration": 0}]},
        {"name": "c", "scaleFrame": [
            {"duration": 6, "tweenEasing": 0},
            {"duration": 6, "tweenEasing": 0, "x": 1.5, "y": 0.5},
            {"duration": 0}]},
    ],
    "slot": [
        {"name": "sb", "displayFrame": [{"duration": 6, "value": 0}, {"duration": 6, "value": 1},
                                        {"duration": 0, "value": 1}]},
        {"name": "sa", "colorFrame": [{"duration": 8, "tweenEasing": 0, "value": {}},
                                      {"duration": 4, "value": {"aM": 30}}]},
    ],
    "zOrder": ZORDER,
}

# In the .dbani, at twice the size: the tool scales the positions by 0.5
EXTRA = {
    "name": "extra", "duration": 8, "playTimes": 0,
    "bone": [
        {"name": "root", "frame": [
            {"duration": 4, "tweenEasing": 0, "transform": {}},
            {"duration": 4, "tweenEasing": 0, "transform": {"x": 8, "y": -4}},
            {"duration": 0, "transform": {}},
        ]},
        {"name": "a", "frame": [
            {"duration": 4, "tweenEasing": 0, "transform": {}},
            {"duration": 4, "tweenEasing": 0, "transform": {"skX": 90, "skY": 90}},
            {"duration": 0, "transform": {}},
        ]},
    ],
}


def write_json(path, arm, version="5.0"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump({"frameRate": 24, "name": "test_rig", "version": version, "armature": [arm]}, f, indent=1)
        f.write("\n")


def run(*args):
    subprocess.check_call([sys.executable, TOOL] + list(args))


def body(path):
    with open(path) as f:
        return f.read()


def fmt_f(v):
    s = f"{v:.7g}"
    return (s if ("." in s or "e" in s) else s + ".0") + "f"


def write_expected(pose_path, out_path):
    with open(pose_path) as f:
        pose = json.load(f)
    lines = [
        "#ifndef TEST_RIG_EXPECTED_HPP",
        "#define TEST_RIG_EXPECTED_HPP",
        "",
        "// Generated by test/tools/make_test_rig.py from test_rig_pose.json: the",
        "// poses of test_rig.hpp as evaluated by bin/shapogfx_dbones.py",
        "",
        "#include <cstdint>",
        "",
        "namespace test_rig_expected {",
        "",
        f"constexpr int BONES = {len(pose['bones'])};",
        f"constexpr int SLOTS = {len(pose['slots'])};",
        "",
        "struct Pose {",
        "  float frame;",
        "  float world[BONES][6];",
        "  int8_t attachment[SLOTS];",
        "  uint8_t alpha[SLOTS];",
        "  uint8_t order[SLOTS];",
        "  int16_t bounds[4];  // x, y, width, height",
        "};",
    ]
    for name, rows in pose["animations"].items():
        lines += ["", f"static const Pose {name}[] = {{"]
        for r in rows:
            lines.append(f"  {{{fmt_f(r['frame'])},")
            lines.append("   {" + ", ".join("{" + ", ".join(fmt_f(v) for v in m) + "}" for m in r["world"]) + "},")
            lines.append("   {" + ", ".join(str(v) for v in r["attachment"]) + "},")
            lines.append("   {" + ", ".join(str(v) for v in r["alpha"]) + "},")
            lines.append("   {" + ", ".join(str(v) for v in r["order"]) + "},")
            lines.append("   {" + ", ".join(str(v) for v in r["bounds"]) + "}},")
        lines.append("};")
        lines.append(f"constexpr int {name.upper()}_COUNT = {len(rows)};")
    lines += ["", "}  // namespace test_rig_expected", "", "#endif", ""]
    with open(out_path, "w") as f:
        f.write("\n".join(lines))


def main():
    images = [make_image(i, w, h) for i, (_, w, h) in enumerate(IMAGES)]
    tex_dir = os.path.join(DATA, "test_rig_texture")
    os.makedirs(tex_dir, exist_ok=True)
    for (name, _, _), img in zip(IMAGES, images):
        img.save(os.path.join(tex_dir, name + ".png"))
    write_json(os.path.join(DATA, "test_rig_ske.json"), armature(1, [MOVE_50]))
    with open(os.path.join(DATA, "test_rig_anim.dbani"), "w") as f:
        json.dump({"frameRate": 24, "name": "test_rig", "version": "5.0",
                   "armature": [armature(2, [EXTRA], skin=False)]}, f, indent=1)
        f.write("\n")

    # 5.5 format with an atlas (one row, 1 pixel apart)
    d55 = os.path.join(DATA, "test_rig55")
    write_json(os.path.join(d55, "test_rig_ske.json"), armature(1, [MOVE_55]), "5.5")
    aw = sum(w + 1 for _, w, _ in IMAGES)
    ah = max(h for _, _, h in IMAGES)
    atlas = Image.new("RGBA", (aw, ah), (0, 0, 0, 0))
    subs = []
    x = 0
    for (name, w, h), img in zip(IMAGES, images):
        atlas.paste(img, (x, 0))
        subs.append({"name": name, "x": x, "y": 0, "width": w, "height": h})
        x += w + 1
    atlas.save(os.path.join(d55, "test_rig_tex.png"))
    with open(os.path.join(d55, "test_rig_tex.json"), "w") as f:
        json.dump({"name": "test_rig", "imagePath": "test_rig_tex.png", "width": aw, "height": ah,
                   "SubTexture": subs}, f, indent=1)
        f.write("\n")

    ske = os.path.join(DATA, "test_rig_ske.json")
    run("--atlas-width", "64", "--dump-pose", "0,3.5,7,9.25,12", ske,
        os.path.join(DATA, "test_rig_anim.dbani"), os.path.join(DATA, "test_rig.hpp"))
    run("--atlas-width", "0", "--namespace", "test_rig_sep", ske, os.path.join(DATA, "test_rig_sep.hpp"))
    run("--out-format", "rgb565_swapped", "--out-key", "#FF00FF", "--namespace", "test_rig_keyed", ske,
        os.path.join(DATA, "test_rig_keyed.hpp"))

    # The 5.0 and 5.5 formats, folder and atlas images: the same header
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "a"))
        os.makedirs(os.path.join(tmp, "b"))
        a = os.path.join(tmp, "a", "test_rig.hpp")
        b = os.path.join(tmp, "b", "test_rig.hpp")
        run("--atlas-width", "64", ske, a)
        run("--atlas-width", "64", os.path.join(d55, "test_rig_ske.json"), b)
        assert body(a) == body(b), "the 5.5 / atlas variant converts differently"
    print("5.0 folder and 5.5 atlas variants: same header")

    pose = os.path.join(DATA, "test_rig_pose.json")
    write_expected(pose, os.path.join(DATA, "test_rig_expected.hpp"))
    os.remove(pose)
    print("wrote test_rig_expected.hpp")


if __name__ == "__main__":
    main()
