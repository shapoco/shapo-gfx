#!/usr/bin/env python3
"""Generate test/data/test_image.png (13x7, procedural RGBA) and its headers in
every pixel format with bin/img2cpp. test/tools_test.cpp recomputes the same
pixels and compares them with the generated textures.

  python3 test/tools/make_test_image.py
"""

import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
BIN = os.path.join(HERE, "..", "..", "bin", "img2cpp")


def pixel(x, y):
    # Keep in sync with testImagePixel() in test/tools_test.cpp
    return ((x * 19) % 256, (y * 36) % 256, (x * y * 7) % 256, 0 if (x + y) % 5 == 0 else 255)


def main():
    img = Image.new("RGBA", (13, 7))
    for y in range(7):
        for x in range(13):
            img.putpixel((x, y), pixel(x, y))
    png = os.path.join(DATA, "test_image.png")
    img.save(png)
    for fmt in ("rgb565_swapped", "rgb565", "argb4444", "rgb444", "gray1"):
        out = os.path.join(DATA, f"test_image_{fmt}.hpp")
        subprocess.check_call([sys.executable, BIN, "-f", fmt, "--namespace", f"test_image_{fmt}",
                               "--name", "texture", png, out])


if __name__ == "__main__":
    main()
