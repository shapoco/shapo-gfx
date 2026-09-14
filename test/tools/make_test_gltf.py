#!/usr/bin/env python3
"""Generate test/data/test_model.glb, a small glTF exercising gltf2cpp:
an indexed textured cube with vertex colors, a pyramid without normals, a LINES
primitive with vertex colors and a three-level node hierarchy with TRS transforms. Regenerate the header with:

  python3 test/tools/make_test_gltf.py
  bin/gltf2cpp test/data/test_model.glb test/data/test_model.hpp
"""

import io
import os
import struct

import numpy as np
import pygltflib
from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "test_model.glb")


def cube():
    # 24 vertices (4 per face), CCW outward
    faces = [
        ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
        ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),
        ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
        ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
        ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
        ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
    ]
    pos, nrm, uv, col, idx = [], [], [], [], []
    uvs = [(0, 1), (1, 1), (1, 0), (0, 0)]
    for fi, (n, corners) in enumerate(faces):
        base = len(pos)
        for k, c in enumerate(corners):
            pos.append([c[0] * 0.5, c[1] * 0.5, c[2] * 0.5])
            nrm.append(list(n))
            uv.append(list(uvs[k]))
            col.append([255 if fi % 3 == 0 else 128, 255 if fi % 3 == 1 else 128, 255 if fi % 3 == 2 else 128, 255])
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return (np.array(pos, np.float32), np.array(nrm, np.float32), np.array(uv, np.float32),
            np.array(col, np.uint8), np.array(idx, np.uint16))


def pyramid():
    pos = np.array([[-0.5, 0, -0.5], [0.5, 0, -0.5], [0.5, 0, 0.5], [-0.5, 0, 0.5], [0, 0.8, 0]], np.float32)
    # Sides and base, counter-clockwise seen from outside
    idx = np.array([0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0, 0, 1, 2, 0, 2, 3], np.uint16)
    return pos, idx


def axes():
    # Three colored line segments from the origin (LINES mode), vertex colors only
    pos = np.array([[0, 0, 0], [1, 0, 0], [0, 0, 0], [0, 1, 0], [0, 0, 0], [0, 0, 1]], np.float32)
    col = np.array([[255, 0, 0, 255], [255, 0, 0, 255], [0, 255, 0, 255], [0, 255, 0, 255],
                    [0, 0, 255, 255], [0, 0, 255, 255]], np.uint8)
    idx = np.array([0, 1, 2, 3, 4, 5], np.uint16)
    return pos, col, idx


def checker_png():
    img = Image.new("RGB", (16, 16))
    for y in range(16):
        for x in range(16):
            c = ((x >> 2) ^ (y >> 2)) & 1
            img.putpixel((x, y), (230, 230, 240) if c else (60, 90, 160))
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def main():
    blob = bytearray()
    views = []
    accessors = []

    def add_view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        views.append(pygltflib.BufferView(buffer=0, byteOffset=off, byteLength=len(data), target=target))
        return len(views) - 1

    def add_accessor(arr, ctype, atype, normalized=None, target=None):
        view = add_view(arr.tobytes(), target)
        acc = pygltflib.Accessor(bufferView=view, componentType=ctype, count=len(arr), type=atype)
        if normalized:
            acc.normalized = True
        if atype != "SCALAR":
            acc.min = [float(v) for v in arr.min(axis=0)]
            acc.max = [float(v) for v in arr.max(axis=0)]
        else:
            acc.min = [int(arr.min())]
            acc.max = [int(arr.max())]
        accessors.append(acc)
        return len(accessors) - 1

    cpos, cnrm, cuv, ccol, cidx = cube()
    a_pos = add_accessor(cpos, pygltflib.FLOAT, "VEC3", target=pygltflib.ARRAY_BUFFER)
    a_nrm = add_accessor(cnrm, pygltflib.FLOAT, "VEC3", target=pygltflib.ARRAY_BUFFER)
    a_uv = add_accessor(cuv, pygltflib.FLOAT, "VEC2", target=pygltflib.ARRAY_BUFFER)
    a_col = add_accessor(ccol, pygltflib.UNSIGNED_BYTE, "VEC4", normalized=True, target=pygltflib.ARRAY_BUFFER)
    a_idx = add_accessor(cidx, pygltflib.UNSIGNED_SHORT, "SCALAR", target=pygltflib.ELEMENT_ARRAY_BUFFER)
    ppos, pidx = pyramid()
    a_ppos = add_accessor(ppos, pygltflib.FLOAT, "VEC3", target=pygltflib.ARRAY_BUFFER)
    a_pidx = add_accessor(pidx, pygltflib.UNSIGNED_SHORT, "SCALAR", target=pygltflib.ELEMENT_ARRAY_BUFFER)
    apos, acol, aidx = axes()
    a_apos = add_accessor(apos, pygltflib.FLOAT, "VEC3", target=pygltflib.ARRAY_BUFFER)
    a_acol = add_accessor(acol, pygltflib.UNSIGNED_BYTE, "VEC4", normalized=True, target=pygltflib.ARRAY_BUFFER)
    a_aidx = add_accessor(aidx, pygltflib.UNSIGNED_SHORT, "SCALAR", target=pygltflib.ELEMENT_ARRAY_BUFFER)
    img_view = add_view(checker_png())

    gltf = pygltflib.GLTF2(
        asset=pygltflib.Asset(version="2.0", generator="make_test_gltf.py"),
        scene=0,
        scenes=[pygltflib.Scene(name="Scene", nodes=[0])],
        nodes=[
            pygltflib.Node(name="Root", translation=[0.0, 0.2, 0.0], children=[1, 2, 4]),
            pygltflib.Node(name="Cube", mesh=0, rotation=[0.0, 0.38268343, 0.0, 0.92387953], translation=[-0.8, 0.0, 0.0]),
            pygltflib.Node(name="Pyramid", mesh=1, scale=[1.5, 1.0, 1.5], translation=[0.8, -0.4, 0.0], children=[3]),
            pygltflib.Node(name="Tip Pyramid", mesh=1, translation=[0.0, 0.9, 0.0], scale=[0.4, 0.4, 0.4]),
            pygltflib.Node(name="Axes", mesh=2, scale=[1.5, 1.5, 1.5]),
        ],
        meshes=[
            pygltflib.Mesh(name="CubeMesh", primitives=[pygltflib.Primitive(
                attributes=pygltflib.Attributes(POSITION=a_pos, NORMAL=a_nrm, TEXCOORD_0=a_uv, COLOR_0=a_col),
                indices=a_idx, material=0)]),
            pygltflib.Mesh(name="PyramidMesh", primitives=[pygltflib.Primitive(
                attributes=pygltflib.Attributes(POSITION=a_ppos), indices=a_pidx, material=1)]),
            pygltflib.Mesh(name="AxesMesh", primitives=[pygltflib.Primitive(
                attributes=pygltflib.Attributes(POSITION=a_apos, COLOR_0=a_acol), indices=a_aidx, mode=1)]),
        ],
        materials=[
            pygltflib.Material(name="Checker", pbrMetallicRoughness=pygltflib.PbrMetallicRoughness(
                baseColorTexture=pygltflib.TextureInfo(index=0), baseColorFactor=[1.0, 1.0, 1.0, 1.0])),
            pygltflib.Material(name="Red", doubleSided=True, pbrMetallicRoughness=pygltflib.PbrMetallicRoughness(
                baseColorFactor=[0.9, 0.2, 0.1, 1.0])),
        ],
        textures=[pygltflib.Texture(source=0, sampler=0)],
        samplers=[pygltflib.Sampler()],
        images=[pygltflib.Image(bufferView=img_view, mimeType="image/png")],
        accessors=accessors,
        bufferViews=views,
        buffers=[pygltflib.Buffer(byteLength=len(blob))],
    )
    gltf.set_binary_blob(bytes(blob))
    gltf.save(OUT)
    print(f"wrote {os.path.normpath(OUT)} ({len(blob)} bytes of buffer data)")


if __name__ == "__main__":
    main()
