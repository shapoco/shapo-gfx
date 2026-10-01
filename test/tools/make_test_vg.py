#!/usr/bin/env python3
"""Generate the SVG test data and its headers for the vg / svg2cpp tests.

Writes test/data/test_vg.svg (a static document exercising every shape
element, every path command, gradients, strokes and dashes, nested
transforms, a style sheet, currentColor, group opacity, a clip path, a use,
an embedded PNG, a text, fill-rule evenodd and a hidden element) and
test/data/test_vg_anim.svg (SMIL: animateTransform rotate about a center and
translate additive sum, animate opacity with keySplines, fill color, cx, r, stroke-width,
animateMotion with rotate auto, set visibility, a static subtree to collapse
and an element kept by --keep), then runs:

  bin/svg2cpp --namespace test_vg --picture --dump test/data/test_vg.json \\
      test/data/test_vg.svg test/data/test_vg.hpp
  bin/svg2cpp --namespace test_vg_anim --rig --keep kept --fps 30 --duration 2 \\
      --dump test/data/test_vg_anim.json test/data/test_vg_anim.svg test/data/test_vg_anim.hpp
  bin/svg2cpp --namespace test_vg_text --picture --font 'sans-serif=<DejaVuSans.ttf>' \\
      --dump test/data/test_vg_text.json test/data/test_vg.svg test/data/test_vg_text.hpp
      (only when DejaVuSans.ttf is found under /usr/share/fonts)

and compile-checks the headers when g++ is available.

  python3 test/tools/make_test_vg.py
"""

import base64
import glob
import io
import os
import shutil
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(ROOT, "test", "data")
TOOL = os.path.join(ROOT, "bin", "svg2cpp")


def tiny_png():
    """A 4 x 3 RGBA image with distinct pixels, as a base64 PNG."""
    img = Image.new("RGBA", (4, 3))
    for y in range(3):
        for x in range(4):
            img.putpixel((x, y), (40 + 50 * x, 200 - 60 * y, (x * 70 + y * 90) % 256, 255 if (x + y) % 2 == 0 else 128))
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


STATIC_SVG = """<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="160" height="120" viewBox="0 0 160 120">
  <title>svg2cpp test</title>
  <style>
    .boxed { fill: #ffcc00; stroke: #333333; stroke-width: 2; }
    #ring { stroke: navy; }
    g.faded rect { fill: teal; }
  </style>
  <defs>
    <linearGradient id="lg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="red"/>
      <stop offset="50%" stop-color="#00ff00" stop-opacity="0.5"/>
      <stop offset="1" stop-color="blue"/>
    </linearGradient>
    <radialGradient id="rg" gradientUnits="userSpaceOnUse" cx="120" cy="30" r="20"
                    gradientTransform="translate(120 30) scale(1 0.5) translate(-120 -30)" spreadMethod="reflect">
      <stop offset="0" stop-color="white"/>
      <stop offset="1" stop-color="black"/>
    </radialGradient>
    <clipPath id="clip"><rect x="8" y="64" width="40" height="30"/></clipPath>
    <circle id="dot" r="3" fill="magenta"/>
  </defs>
  <rect id="rounded" class="boxed" x="8" y="8" width="40" height="24" rx="6" ry="4"/>
  <circle id="ring" cx="80" cy="20" r="12" fill="url(#lg)" stroke-width="3"/>
  <ellipse id="egg" cx="120" cy="30" rx="24" ry="14" fill="url(#rg)"/>
  <line id="dashed" x1="8" y1="40" x2="60" y2="40" stroke="black" stroke-width="2" stroke-dasharray="6 3 2 3"
        stroke-dashoffset="2" stroke-linecap="round"/>
  <polyline id="zig" points="64,40 72,48 80,40 88,48 96,40" fill="none" stroke="darkgreen" stroke-width="3"
            stroke-linejoin="bevel" stroke-linecap="square"/>
  <polygon id="tri" points="104,40 120,56 136,40" fill="rgb(255, 128, 0)" stroke="brown" stroke-linejoin="miter"
           stroke-miterlimit="2"/>
  <path id="allcmds" fill="#c0c0ff" stroke="#000080" stroke-width="1.5" stroke-opacity="0.75"
        d="M 8 100 h 20 v -10 H 40 V 100 l 4 -8 L 50 100 c 2 -6 6 -6 8 0 C 60 110 66 110 68 100
           s 6 -6 8 0 S 86 110 88 100 q 4 -8 8 0 Q 100 110 104 100 t 8 0 T 120 100
           a 6 6 0 0 1 12 0 A 6 6 0 1 0 144 100 z m 2 -30 l 6 0 l -3 6 z"/>
  <g id="nested" transform="translate(100 60) rotate(30 10 10) scale(1.25 0.8)" opacity="0.5">
    <g transform="skewX(15) matrix(1 0 0 1 2 3)">
      <rect class="boxed" width="20" height="12" fill="currentColor" color="olive"/>
      <circle cx="26" cy="6" r="5" fill="currentColor" stroke="currentColor" stroke-width="1" color="purple"/>
    </g>
  </g>
  <g id="clipped" clip-path="url(#clip)" class="faded">
    <rect x="0" y="60" width="60" height="40"/>
    <circle cx="30" cy="80" r="14" fill="orange" fill-opacity="0.5"/>
  </g>
  <use xlink:href="#dot" x="70" y="80"/>
  <use href="#dot" x="78" y="80" fill="blue"/>
  <image id="pic" x="60" y="90" width="24" height="18" preserveAspectRatio="none"
         xlink:href="data:image/png;base64,{png}"/>
  <image id="pic2" x="88" y="90" width="24" height="18" opacity="0.5" href="data:image/png;base64,{png}"/>
  <text id="label" x="120" y="110" font-size="10" font-family="sans-serif" fill="#202020">Hi, vg!</text>
  <text id="anchored" x="150" y="80" font-size="8" text-anchor="end" letter-spacing="1">end</text>
  <path id="evenodd" fill-rule="evenodd" fill="#008080"
        d="M 120 60 l 30 0 l 0 20 l -30 0 z M 128 66 l 14 0 l 0 8 l -14 0 z"/>
  <rect id="ghost" x="140" y="100" width="10" height="10" fill="red" visibility="hidden"/>
  <rect id="gone" x="140" y="100" width="10" height="10" fill="red" display="none"/>
  <switch><rect x="0" y="0" width="4" height="4" fill="#123456"/><rect x="4" y="0" width="4" height="4"/></switch>
</svg>
"""

ANIM_SVG = """<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120" viewBox="0 0 200 120">
  <defs>
    <path id="track" d="M 0 0 L 40 0 A 20 20 0 0 1 40 40 L 0 40 Z"/>
    <linearGradient id="lg"><stop offset="0" stop-color="gold"/><stop offset="1" stop-color="crimson"/></linearGradient>
  </defs>
  <g id="backdrop">
    <rect x="0" y="0" width="200" height="120" fill="#eeeeee"/>
    <g transform="translate(4 4)"><rect width="30" height="10" fill="#bbbbbb"/><circle cx="36" cy="5" r="5" fill="#999999"/></g>
  </g>
  <g id="wheel" transform="translate(40 60)">
    <circle r="24" fill="url(#lg)" stroke="#333333" stroke-width="2"/>
    <rect x="-2" y="-24" width="4" height="48" fill="#333333"/>
    <animateTransform attributeName="transform" type="rotate" from="0 0 0" to="360 0 0" dur="2s"
                      repeatCount="indefinite" additive="sum"/>
  </g>
  <g id="slider" transform="translate(80 10)">
    <rect id="kept" width="20" height="12" fill="#3366cc"/>
    <animateTransform attributeName="transform" type="translate" values="0 0; 60 0; 0 0" dur="1s"
                      repeatCount="indefinite" additive="sum"/>
  </g>
  <rect id="fader" x="80" y="30" width="30" height="12" fill="#cc3366">
    <animate attributeName="opacity" values="1; 0.1; 1" keyTimes="0; 0.4; 1" calcMode="spline"
             keySplines="0.42 0 1 1; 0 0 0.58 1" dur="2s" repeatCount="indefinite"/>
    <animate attributeName="fill" from="#cc3366" to="#33cc66" dur="2s" repeatCount="indefinite"/>
  </rect>
  <circle id="blob" cx="100" cy="80" r="8" fill="green" stroke="black" stroke-width="1">
    <animate attributeName="cx" from="100" to="160" dur="1s" repeatCount="indefinite"/>
    <animate attributeName="r" from="8" to="16" begin="0.5s" dur="1s" fill="freeze"/>
    <animate attributeName="stroke-width" from="1" to="3" dur="1s" repeatCount="indefinite"/>
  </circle>
  <g id="arrow">
    <polygon points="0,-4 8,0 0,4" fill="#ff6600"/>
    <animateMotion dur="2s" repeatCount="indefinite" rotate="auto"><mpath href="#track"/></animateMotion>
  </g>
  <rect id="blinker" x="170" y="100" width="20" height="12" fill="#660066">
    <set attributeName="visibility" to="hidden" begin="0.5s" dur="0.5s"/>
  </rect>
  <g id="spin" transform="translate(150 60)">
    <g id="inner"><rect x="-10" y="-5" width="20" height="10" fill="#006699"/>
      <circle cx="12" cy="0" r="3" fill="#0099cc"/></g>
    <animateTransform attributeName="transform" type="rotate" values="0 5 0; 90 5 0; 0 5 0" dur="2s"
                      repeatCount="indefinite" additive="sum"/>
  </g>
</svg>
"""


def run(*args):
    subprocess.check_call([sys.executable, TOOL] + list(args))


def compile_check(path, includes):
    gxx = shutil.which("g++")
    if gxx is None:
        print(f"g++ not found, {os.path.basename(path)} not compile-checked")
        return
    cmd = [gxx, "-std=c++17", "-fsyntax-only", "-Wall", "-Wextra", "-I", os.path.join(ROOT, "include")]
    for inc in includes:
        cmd += ["-include", inc]
    cmd += ["-x", "c++", path]
    subprocess.check_call(cmd)
    print(f"{os.path.basename(path)}: compiles")


def find_dejavu():
    for pattern in ("/usr/share/fonts/**/DejaVuSans.ttf", "/usr/share/fonts/**/DejaVuSans-Bold.ttf"):
        hits = glob.glob(pattern, recursive=True)
        if hits:
            return hits[0]
    return None


def main():
    os.makedirs(DATA, exist_ok=True)
    png = tiny_png()
    static_svg = os.path.join(DATA, "test_vg.svg")
    anim_svg = os.path.join(DATA, "test_vg_anim.svg")
    with open(static_svg, "w") as f:
        f.write(STATIC_SVG.replace("{png}", png))
    with open(anim_svg, "w") as f:
        f.write(ANIM_SVG)

    static_hpp = os.path.join(DATA, "test_vg.hpp")
    run("--namespace", "test_vg", "--picture", "--dump", os.path.join(DATA, "test_vg.json"), static_svg, static_hpp)
    compile_check(static_hpp, ["shapoco/gfx2d/vg.hpp", "shapoco/gfx2d/fonts.hpp"])

    anim_hpp = os.path.join(DATA, "test_vg_anim.hpp")
    run("--namespace", "test_vg_anim", "--rig", "--keep", "kept", "--fps", "30", "--duration", "2",
        "--dump", os.path.join(DATA, "test_vg_anim.json"), anim_svg, anim_hpp)
    compile_check(anim_hpp, ["shapoco/gfx2d/rig.hpp"])

    font = find_dejavu()
    if font is None:
        print("DejaVuSans.ttf not found under /usr/share/fonts: test_vg_text.hpp not regenerated")
    else:
        text_hpp = os.path.join(DATA, "test_vg_text.hpp")
        run("--namespace", "test_vg_text", "--picture", "--font", f"sans-serif={font}",
            "--dump", os.path.join(DATA, "test_vg_text.json"), static_svg, text_hpp)
        compile_check(text_hpp, ["shapoco/gfx2d/vg.hpp"])


if __name__ == "__main__":
    main()
