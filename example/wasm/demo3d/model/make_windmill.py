#!/usr/bin/env python3
"""Generate windmill.glb, the small model shown in demo3d, and its C++ header:

  python3 example/wasm/demo3d/model/make_windmill.py
  bin/gltf2cpp --namespace windmill example/wasm/demo3d/model/windmill.glb example/wasm/demo3d/model/windmill.hpp

The model is a tapered tower (vertex-colored), a roof and a hub node named
"Blades" with four blades; demo3d rotates the "Blades" node with a NodeVisitor.
"""

import math
import os

import numpy as np
import pygltflib

OUT = os.path.join(os.path.dirname(__file__), "windmill.glb")


class MeshBuilder:
    def __init__(self):
        self.pos, self.nrm, self.col, self.idx = [], [], [], []

    def quad(self, a, b, c, d, color):
        """CCW quad a-b-c-d with a flat normal and one color."""
        a, b, c, d = (np.array(v, np.float32) for v in (a, b, c, d))
        n = np.cross(b - a, d - a)
        n = n / (np.linalg.norm(n) or 1.0)
        base = len(self.pos)
        for v in (a, b, c, d):
            self.pos.append(v)
            self.nrm.append(n)
            self.col.append(color)
        self.idx += [base, base + 1, base + 2, base, base + 2, base + 3]

    def box(self, center, size, color):
        cx, cy, cz = center
        hx, hy, hz = (s / 2 for s in size)
        p = lambda sx, sy, sz: (cx + sx * hx, cy + sy * hy, cz + sz * hz)  # noqa: E731
        self.quad(p(1, -1, 1), p(1, -1, -1), p(1, 1, -1), p(1, 1, 1), color)
        self.quad(p(-1, -1, -1), p(-1, -1, 1), p(-1, 1, 1), p(-1, 1, -1), color)
        self.quad(p(-1, 1, 1), p(1, 1, 1), p(1, 1, -1), p(-1, 1, -1), color)
        self.quad(p(-1, -1, -1), p(1, -1, -1), p(1, -1, 1), p(-1, -1, 1), color)
        self.quad(p(-1, -1, 1), p(1, -1, 1), p(1, 1, 1), p(-1, 1, 1), color)
        self.quad(p(1, -1, -1), p(-1, -1, -1), p(-1, 1, -1), p(1, 1, -1), color)

    def arrays(self):
        return (np.array(self.pos, np.float32), np.array(self.nrm, np.float32),
                np.array(self.col, np.uint8), np.array(self.idx, np.uint16))


def tower():
    b = MeshBuilder()
    # Tapered octagonal tower with a color gradient (dark base, light top)
    n = 8
    h = 2.2
    for i in range(n):
        a0 = 2 * math.pi * i / n
        a1 = 2 * math.pi * (i + 1) / n
        rb, rt = 0.55, 0.35
        p0 = (rb * math.cos(a0), 0.0, rb * math.sin(a0))
        p1 = (rb * math.cos(a1), 0.0, rb * math.sin(a1))
        p2 = (rt * math.cos(a1), h, rt * math.sin(a1))
        p3 = (rt * math.cos(a0), h, rt * math.sin(a0))
        # Reverse for outward winding (angle increases clockwise seen from +Y)
        a, bb, c, d = (np.array(v, np.float32) for v in (p1, p0, p3, p2))
        nrm = np.cross(bb - a, d - a)
        nrm = nrm / np.linalg.norm(nrm)
        base = len(b.pos)
        for v, col in ((a, (120, 80, 50, 255)), (bb, (120, 80, 50, 255)), (c, (235, 220, 190, 255)), (d, (235, 220, 190, 255))):
            b.pos.append(v)
            b.nrm.append(nrm)
            b.col.append(col)
        b.idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    # Door
    b.box((0.0, 0.3, 0.5), (0.25, 0.6, 0.12), (60, 40, 30, 255))
    return b.arrays()


def roof():
    b = MeshBuilder()
    n = 8
    for i in range(n):
        a0 = 2 * math.pi * i / n
        a1 = 2 * math.pi * (i + 1) / n
        r = 0.45
        p0 = np.array((r * math.cos(a0), 0.0, r * math.sin(a0)), np.float32)
        p1 = np.array((r * math.cos(a1), 0.0, r * math.sin(a1)), np.float32)
        top = np.array((0.0, 0.45, 0.0), np.float32)
        nrm = np.cross(p0 - p1, top - p1)
        nrm = nrm / np.linalg.norm(nrm)
        base = len(b.pos)
        for v in (p1, p0, top):
            b.pos.append(v)
            b.nrm.append(nrm)
            b.col.append((180, 50, 40, 255))
        b.idx += [base, base + 1, base + 2]
    return b.arrays()


def blades():
    b = MeshBuilder()
    b.box((0, 0, 0), (0.18, 0.18, 0.18), (90, 90, 100, 255))  # hub
    for k in range(4):
        ang = k * math.pi / 2
        # Blade along +Y, then rotated around Z by `ang`: bake the rotation into the vertices
        tmp = MeshBuilder()
        tmp.box((0.0, 0.55, 0.0), (0.16, 0.95, 0.03), (240, 240, 250, 255))
        tmp.box((0.0, 0.25, 0.0), (0.04, 0.5, 0.05), (110, 80, 60, 255))
        c, s = math.cos(ang), math.sin(ang)
        rot = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], np.float32)
        base = len(b.pos)
        for p, nn, col in zip(tmp.pos, tmp.nrm, tmp.col):
            b.pos.append(rot @ p)
            b.nrm.append(rot @ nn)
            b.col.append(col)
        b.idx += [base + i for i in tmp.idx]
    return b.arrays()


def main():
    blob = bytearray()
    views, accessors, meshes = [], [], []

    def add_accessor(arr, ctype, atype, normalized=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        data = arr.tobytes()
        blob.extend(data)
        views.append(pygltflib.BufferView(buffer=0, byteOffset=off, byteLength=len(data)))
        acc = pygltflib.Accessor(bufferView=len(views) - 1, componentType=ctype, count=len(arr), type=atype)
        if normalized:
            acc.normalized = True
        if atype == "SCALAR":
            acc.min, acc.max = [int(arr.min())], [int(arr.max())]
        else:
            acc.min, acc.max = [float(v) for v in arr.min(axis=0)], [float(v) for v in arr.max(axis=0)]
        accessors.append(acc)
        return len(accessors) - 1

    for name, (pos, nrm, col, idx) in (("Tower", tower()), ("Roof", roof()), ("Blades", blades())):
        a_pos = add_accessor(pos, pygltflib.FLOAT, "VEC3")
        a_nrm = add_accessor(nrm, pygltflib.FLOAT, "VEC3")
        a_col = add_accessor(col, pygltflib.UNSIGNED_BYTE, "VEC4", normalized=True)
        a_idx = add_accessor(idx, pygltflib.UNSIGNED_SHORT, "SCALAR")
        meshes.append(pygltflib.Mesh(name=name, primitives=[pygltflib.Primitive(
            attributes=pygltflib.Attributes(POSITION=a_pos, NORMAL=a_nrm, COLOR_0=a_col), indices=a_idx, material=0)]))

    gltf = pygltflib.GLTF2(
        asset=pygltflib.Asset(version="2.0", generator="make_windmill.py"),
        scene=0,
        scenes=[pygltflib.Scene(name="Windmill", nodes=[0])],
        nodes=[
            pygltflib.Node(name="Windmill", children=[1, 2, 3]),
            pygltflib.Node(name="Tower", mesh=0),
            pygltflib.Node(name="Roof", mesh=1, translation=[0.0, 2.2, 0.0]),
            pygltflib.Node(name="Blades", mesh=2, translation=[0.0, 2.1, 0.5]),
        ],
        meshes=meshes,
        materials=[pygltflib.Material(name="VertexColors", pbrMetallicRoughness=pygltflib.PbrMetallicRoughness(
            baseColorFactor=[1.0, 1.0, 1.0, 1.0]))],
        accessors=accessors,
        bufferViews=views,
        buffers=[pygltflib.Buffer(byteLength=len(blob))],
    )
    gltf.set_binary_blob(bytes(blob))
    gltf.save(OUT)
    print(f"wrote {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
