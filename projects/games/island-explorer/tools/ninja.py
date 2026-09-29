"""Ninja BASIC stage reader. Layouts cross-checked against X-Hax SA Tools.

Only reads data. Does not execute disc code. Coordinates remain SA1 Y-up.
"""
import struct
import math
import numpy as np

BASE = 0x0C900000


def transform(position=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1), zyx=False):
    x, y, z = [v * math.tau / 65536 for v in rotation]
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    m = np.eye(4)
    # Ninja's alternate order is Z, X, Y when applied to a column vector.
    m[:3, :3] = (ry @ rx @ rz if zyx else rz @ ry @ rx) @ np.diag(scale)
    m[:3, 3] = position
    return m


class Ninja:
    def __init__(self, data):
        self.data = data

    def unpack(self, fmt, p):
        if p < 0 or p + struct.calcsize('<' + fmt) > len(self.data):
            raise ValueError(f'Out-of-range Ninja data pointer: {p:x}')
        return struct.unpack_from('<' + fmt, self.data, p)

    def ptr(self, p):
        v = self.unpack('I', p)[0]
        return v - BASE if v else None

    def string(self, p):
        return self.data[p:self.data.index(0, p)].decode('ascii').strip()

    def model(self, p, matrix=None, bank=0, surface=0x80000000, label='mesh', depth=0):
        if depth > 64:
            raise ValueError('Cyclic or excessively deep Ninja object tree')
        if matrix is None:
            matrix = np.eye(4)
        flags = self.unpack('I', p)[0]
        pos = (0, 0, 0) if flags & 1 else self.unpack('3f', p + 8)
        rot = (0, 0, 0) if flags & 2 else self.unpack('3i', p + 20)
        scale = (1, 1, 1) if flags & 4 else self.unpack('3f', p + 32)
        world = matrix @ transform(pos, rot, scale, bool(flags & 0x20))
        attach = self.ptr(p + 4)
        if attach is not None and not flags & 8:
            yield from self.attach(attach, world, bank, surface, label)
        child, sibling = self.ptr(p + 44), self.ptr(p + 48)
        if child is not None and not flags & 16:
            yield from self.model(child, world, bank, surface, label, depth + 1)
        if sibling is not None:
            yield from self.model(sibling, matrix, bank, surface, label, depth + 1)

    def attach(self, p, world, bank, surface, label):
        vp, nptr, nv, mp, matp, nm, nmat = self.unpack('IIIIIHH', p)
        assert 0 < nv < 100000 and nm < 2048
        positions = np.array(self.unpack(f'{nv * 3}f', vp - BASE)).reshape((-1, 3))
        positions = positions @ world[:3, :3].T + world[:3, 3]
        normals = np.array(self.unpack(f'{nv * 3}f', nptr - BASE)).reshape((-1, 3)) if nptr else np.tile((0, 1, 0), (nv, 1))
        normals = normals @ np.linalg.inv(world[:3, :3])
        normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-8)
        for im in range(nm):
            m = mp - BASE + im * 24
            mid, count, pp, _, _, cp, up = self.unpack('HHIIIII', m)
            kind, mid = mid >> 14, mid & 0x3fff
            diffuse, _, _, tex, flags = self.unpack('IIfII', matp - BASE + mid * 20) if matp else (0xffffffff, 0, 0, 0, 0)
            pp -= BASE
            vertices, triangles, weld = [], [], {}
            corner = 0
            for _ in range(count):
                reverse = False
                if kind == 0:
                    n = 3
                elif kind == 1:
                    n = 4
                else:
                    n = self.unpack('H', pp)[0]
                    pp += 2
                    reverse, n = bool(n & 0x8000), n & 0x7fff
                ids = self.unpack(f'{n}H', pp)
                pp += n * 2
                polygon = []
                for idx in ids:
                    uv = self.unpack('2h', up - BASE + corner * 4) if up else (0, 0)
                    color = self.unpack('I', cp - BASE + corner * 4)[0] if cp else diffuse
                    # Soft sun and sky fill, retain the original baked vertex colours.
                    light = 1.0 if cp or flags & 0x2000000 else 0.78 + 0.22 * max(0, np.dot(normals[idx], (-0.35, 0.85, -0.39)))
                    rgb = [min(255, round(((color >> s) & 255) * light)) for s in (16, 8, 0)]
                    color = (color & 0xff000000) | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]
                    vert = (*[round(float(x), 5) for x in positions[idx]], uv[0] / 255., uv[1] / 255., color)
                    if vert not in weld:
                        weld[vert] = len(vertices)
                        vertices.append(vert)
                    polygon.append(weld[vert])
                    corner += 1
                if kind == 1:
                    local = [(0, 1, 2), (2, 1, 3)]
                elif kind == 0:
                    local = [(0, 1, 2)]
                else:
                    local = [(i + 1, i, i + 2) if (i % 2 == 1) != reverse else (i, i + 1, i + 2) for i in range(n - 2)]
                for a, b, c in local:
                    tri = (polygon[a], polygon[b], polygon[c])
                    if len(set(tri)) == 3:
                        triangles.append(tri)
            if triangles:
                yield dict(name=f'{label}_{im}', vertices=vertices, triangles=triangles,
                           texture=(bank + (tex & 0xffff)) if flags & 0x200000 else -1,
                           material_flags=flags, surface=surface)

    def stage(self, offset, bank):
        count = self.unpack('H', offset)[0]
        p = self.ptr(offset + 12)
        for i in range(count):
            c = p + i * 36
            surface = self.unpack('I', c + 32)[0]
            yield from self.model(self.ptr(c + 24), bank=bank, surface=surface, label=f'land_{i:03d}')

    def object_names(self):
        count = self.unpack('I', 0x1a8f1c)[0]
        p = self.ptr(0x1a8f20)
        return [self.string(self.ptr(p + i * 20 + 16)) for i in range(count)]
