"""Pictures of a level as it is being built, without the game: the ground's
grid in perspective from a camera, flat-shaded with the generator's
colours, fogged, with rock formations, boulders, boxes and trees roughed
in. For judging shapes and routes while editing a generator; the game
itself is the last word on how it looks.
"""

from __future__ import annotations

import math

import numpy as np
from PIL import Image, ImageDraw

from levelkit import D, N, ORG

W, H = 640, 480


def _camera(pos, target, fov=1.2):
    pos = np.asarray(pos, dtype=float)
    f = np.asarray(target, dtype=float) - pos
    f /= np.linalg.norm(f)
    r = np.cross([0.0, 1.0, 0.0], f)
    r /= np.linalg.norm(r)
    u = np.cross(f, r)
    k = (H / 2) / math.tan(fov / 2 * H / W * 1.15)
    return pos, r, u, f, k


def loft_mesh(rings, sides, seed=0, rough=0.12):
    """Triangles (n, 3, 3) and vertex normals for a rock lofted over rings
    [(x, y, z, ra, rb)]: about what world.c builds."""
    rings = np.asarray(rings, dtype=float)
    c = rings[:, :3]
    n = len(c)
    tang = np.gradient(c, axis=0)
    tang /= np.linalg.norm(tang, axis=1, keepdims=True) + 1e-9
    pts = np.zeros((n, sides, 3))
    ph = (seed % 97) * 0.37
    for i in range(n):
        t = tang[i]
        ref = np.array([0.0, 1.0, 0.0]) if abs(t[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
        side = np.cross(ref, t)
        side /= np.linalg.norm(side)
        up = np.cross(t, side)
        for s in range(sides):
            a = 2 * math.pi * s / sides
            b = 1 + rough * (0.5 * math.sin(2 * a + ph + i * 0.7) + 0.3 * math.sin(3 * a + ph * 2 + i * 1.3))
            pts[i, s] = c[i] + side * math.cos(a) * rings[i, 3] * b + up * math.sin(a) * rings[i, 4] * b
    tris = []
    for i in range(n - 1):
        for s in range(sides):
            s1 = (s + 1) % sides
            tris.append([pts[i, s], pts[i + 1, s], pts[i, s1]])
            tris.append([pts[i, s1], pts[i + 1, s], pts[i + 1, s1]])
    return np.array(tris)


def render(h, rgb, pos, target, path=None, fov=1.2, haze=(222, 204, 190), fog=(160.0, 820.0), sky=((86, 140, 208), (214, 214, 212)),
           rocks=(), rock_rgb=(196, 104, 68), spheres=(), boxes=(), trees=(), tree_rgb=((70, 92, 56),), light=None, far=760.0,
           water=None, water_rgb=(96, 120, 96), extra=()):
    """h, rgb: the grid's heights and lit colours (N x N, N x N x 3)."""
    pos, r, u, f, k = _camera(pos, target, fov)
    img = Image.new("RGB", (W, H))
    px = np.asarray(img).copy()
    top, bot = np.array(sky[0], float), np.array(sky[1], float)
    # The horizon's row for this pitch.
    hy = H / 2 + f[1] / max(1e-6, math.hypot(f[0], f[2])) * k
    t = np.clip((np.arange(H) - (hy - 260)) / 260.0, 0, 1) ** 1.4
    px[:] = (top[None, :] * (1 - t[:, None]) + bot[None, :] * t[:, None])[:, None, :]
    px[int(max(0, min(H, hy))):] = haze
    img = Image.fromarray(px)
    d = ImageDraw.Draw(img)
    L = np.array([-0.5, 0.62, -0.6]) if light is None else np.asarray(light, float)
    L = L / np.linalg.norm(L)

    polys = []   # (depth, [(x, y)...], colour)

    def add(tris, cols):
        """tris (n, 3, 3) world, cols (n, 3) rgb."""
        rel = tris - pos
        z = rel @ f
        x = rel @ r
        y = rel @ u
        zmin = z.min(axis=1)
        zmax = z.max(axis=1)
        ok = (zmin > 0.6) & (zmax < far)
        sx = W / 2 + x / np.maximum(z, 0.6) * k
        sy = H / 2 - y / np.maximum(z, 0.6) * k
        ok &= (sx.max(axis=1) > 0) & (sx.min(axis=1) < W) & (sy.max(axis=1) > 0) & (sy.min(axis=1) < H)
        # Facing the camera.
        area = (sx[:, 1] - sx[:, 0]) * (sy[:, 2] - sy[:, 0]) - (sx[:, 2] - sx[:, 0]) * (sy[:, 1] - sy[:, 0])
        ok &= area > 0
        zm = z.mean(axis=1)
        fg = np.clip((zm - fog[0]) / (fog[1] - fog[0]), 0, 1)[:, None]
        cc = cols * (1 - fg) + np.array(haze, float)[None, :] * fg
        for i in np.nonzero(ok)[0]:
            polys.append((zm[i], [(sx[i, 0], sy[i, 0]), (sx[i, 1], sy[i, 1]), (sx[i, 2], sy[i, 2])],
                          tuple(int(v) for v in np.clip(cc[i], 0, 255))))

    # The ground: two triangles a cell, split as the game draws them,
    # every other vertex past 250 m.
    xs = ORG + D * np.arange(N)
    P = np.dstack([np.tile(xs, (N, 1)), h, np.tile(xs[:, None], (1, N))])
    hw = None
    if water is not None:
        hw = np.where(water > -999, np.maximum(h, water), h)
        wet = water > h + 0.05
        rgb = np.where(wet[..., None], np.array(water_rgb, float)[None, None, :] * 0.75 + rgb * 0.25, rgb)
        P = np.dstack([np.tile(xs, (N, 1)), hw, np.tile(xs[:, None], (1, N))])
    for step, lo, hi in ((1, 0.0, 250.0), (2, 250.0, far)):
        Ps = P[::step, ::step]
        Cs = rgb[::step, ::step]
        a, b, c, e = Ps[:-1, :-1], Ps[:-1, 1:], Ps[1:, :-1], Ps[1:, 1:]
        mid = (a + e) * 0.5
        dist = np.linalg.norm(mid - pos, axis=2)
        m = (dist >= lo) & (dist < hi)
        t1 = np.stack([a[m], e[m], b[m]], axis=1)     # (i,j) (i+1,j+1) (i+1,j): up-facing winding
        t2 = np.stack([a[m], c[m], e[m]], axis=1)
        ca, cb, cc_, ce = Cs[:-1, :-1][m], Cs[:-1, 1:][m], Cs[1:, :-1][m], Cs[1:, 1:][m]
        add(t1, (ca + ce + cb) / 3.0)
        add(t2, (ca + cc_ + ce) / 3.0)

    # Anything else, as triangles with their colours (the land beyond the map).
    for tris, cols in extra:
        add(np.asarray(tris, float), np.asarray(cols, float))

    def lit(tris, base):
        nrm = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        nrm /= np.linalg.norm(nrm, axis=1, keepdims=True) + 1e-9
        dd = np.clip(nrm @ L, 0, 1)
        up = 0.5 + 0.5 * nrm[:, 1]
        return np.asarray(base, float)[None, :] * (0.42 + 0.3 * up + 0.6 * dd)[:, None]

    for rk in rocks:
        tris = loft_mesh(rk["rings"], rk.get("sides", 8), rk.get("seed", 0))
        tris = tris[:, [0, 2, 1]]
        add(tris, lit(tris, rock_rgb))
    for s_ in spheres:
        c = np.array(s_[:3])
        rr = s_[3]
        ring = [(c + np.array([math.cos(a) * rr, 0.0, math.sin(a) * rr])) for a in np.linspace(0, 2 * math.pi, 7)[:-1]]
        topp = c + np.array([0, rr, 0])
        tris = np.array([[ring[i], topp, ring[(i + 1) % 6]] for i in range(6)])
        add(tris, lit(tris, rock_rgb))
    for b_ in boxes:
        cx, cy, cz, hx, hy_, hz, yaw = b_
        co, si = math.cos(yaw), math.sin(yaw)
        ax = np.array([co * hx, 0, -si * hx])
        az = np.array([si * hz, 0, co * hz])
        c0 = np.array([cx, cy, cz])
        cs = [c0 + ax + az, c0 + ax - az, c0 - ax - az, c0 - ax + az]
        up = np.array([0, hy_, 0])
        tris = []
        for i in range(4):
            p0, p1 = cs[i], cs[(i + 1) % 4]
            tris += [[p0 + up, p1 + up, p0 - up], [p1 + up, p1 - up, p0 - up]]
        tris += [[cs[0] + up, cs[2] + up, cs[1] + up], [cs[0] + up, cs[3] + up, cs[2] + up]]
        tris = np.array(tris)
        add(tris, lit(tris, rock_rgb))
        tris = tris[:, [0, 2, 1]]
        add(tris, lit(tris, rock_rgb))
    for t_ in trees:
        x, y, z, kind, scale = t_[:5]
        rr, hh = 1.6 * scale, 3.2 * scale
        c = np.array([x, y, z])
        col = tree_rgb[kind % len(tree_rgb)]
        ring = [(c + np.array([math.cos(a) * rr, hh * 0.45, math.sin(a) * rr])) for a in np.linspace(0, 2 * math.pi, 6)[:-1]]
        topp = c + np.array([0, hh, 0])
        tris = np.array([[ring[i], topp, ring[(i + 1) % 5]] for i in range(5)] + [[ring[(i + 1) % 5], c, ring[i]] for i in range(5)])
        add(tris, lit(tris, col))

    polys.sort(key=lambda p: -p[0])
    for _, pts, col in polys:
        d.polygon(pts, fill=col)
    if path is not None:
        img.save(path)
    return img


def sheet(images, cols, path):
    """Pictures side by side in a grid."""
    rows = (len(images) + cols - 1) // cols
    out = Image.new("RGB", (cols * W, rows * H))
    for i, im in enumerate(images):
        out.paste(im, ((i % cols) * W, (i // cols) * H))
    out.save(path)
