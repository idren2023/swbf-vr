#!/usr/bin/env python3
"""Texel density and stretch of a cockpit shell (idren2023 2026-10-19: texels consistently sized and unstretched).

    python tools/texelcheck.py re/shells/<design>.obj [--worst N] [--finish F]

Reads the OBJ that tools/shellpreview.sh writes (usemtl = texture, `# finish N` = the design's finish index) and the
textures' sizes from the mod tools' TGAs. Per triangle: texels per metre along its two principal directions (the
singular values of the UV map in texels); density = their geometric mean, stretch = their ratio. Prints, area
weighted, per texture and per finish: the median density, the share of area outside 0.5x..2x of the texture's median
(off), the share stretched more than 1.25 (str), and the worst triangles' places.
"""
import glob
import math
import os
import struct
import sys

SIDES = 'C:/LucasArts/BFBuilder/Assets/Sides'


def texture_sizes():
    sizes = {}
    for path in glob.glob(SIDES + '/*/MSH/*.tga') + glob.glob(SIDES + '/*/msh/*.tga'):
        with open(path, 'rb') as f:
            h = f.read(18)
        if len(h) == 18:
            w, hh = struct.unpack('<HH', h[12:16])
            sizes[os.path.basename(path)[:-4].lower()] = (w, hh)
    return sizes


def load(path):
    vs, vts, tris, tex, finish = [], [], [], '', -1
    for line in open(path):
        if line.startswith('v '):
            vs.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith('vt '):
            vts.append(tuple(map(float, line.split()[1:3])))
        elif line.startswith('usemtl'):
            tex = line.split()[1].lower()
        elif line.startswith('# finish'):
            finish = int(line.split()[2])
        elif line.startswith('f '):
            idx = [p.split('/') for p in line.split()[1:4]]
            tris.append(([vs[int(i[0]) - 1] for i in idx], [vts[int(i[1]) - 1] for i in idx], tex, finish))
    return tris


def measure(p, t, size):
    e1 = [p[1][i] - p[0][i] for i in range(3)]
    e2 = [p[2][i] - p[0][i] for i in range(3)]
    l1 = math.sqrt(sum(c * c for c in e1))
    if l1 < 1e-7:
        return None
    x = [c / l1 for c in e1]
    d = sum(e2[i] * x[i] for i in range(3))
    yv = [e2[i] - d * x[i] for i in range(3)]
    h = math.sqrt(sum(c * c for c in yv))
    if h < 1e-7:
        return None
    area = 0.5 * l1 * h
    w, hh = size
    du1, dv1 = (t[1][0] - t[0][0]) * w, (t[1][1] - t[0][1]) * hh
    du2, dv2 = (t[2][0] - t[0][0]) * w, (t[2][1] - t[0][1]) * hh
    # Jacobian columns: d(uv)/dx = (du1, dv1)/l1; d(uv)/dy = ((du2, dv2) - d * d(uv)/dx) / h
    a, c = du1 / l1, dv1 / l1
    b, dd = (du2 - d * a) / h, (dv2 - d * c) / h
    # singular values of [[a, b], [c, dd]]
    s = a * a + b * b + c * c + dd * dd
    det = abs(a * dd - b * c)
    disc = math.sqrt(max(s * s / 4 - det * det, 0))
    s1 = math.sqrt(max(s / 2 + disc, 0))
    s2 = math.sqrt(max(s / 2 - disc, 0))
    if s2 < 1e-6:
        return area, 0.0, float('inf')
    return area, math.sqrt(s1 * s2), s1 / s2


def weighted_median(pairs):
    pairs = sorted(pairs)
    total = sum(w for _, w in pairs)
    acc = 0
    for v, w in pairs:
        acc += w
        if acc >= total / 2:
            return v
    return 0


def main():
    args = sys.argv[1:]
    path = args.pop(0)
    worst, only = 8, None
    while args:
        a = args.pop(0)
        if a == '--worst':
            worst = int(args.pop(0))
        elif a == '--finish':
            only = int(args.pop(0))
    sizes = texture_sizes()
    rows = []
    missing = set()
    for p, t, tex, fin in load(path):
        if tex.startswith('#'):
            continue
        size = sizes.get(tex)
        if not size:
            missing.add(tex)
            continue
        m = measure(p, t, size)
        if m and m[0] > 1e-8:
            rows.append((tex, fin, m[0], m[1], m[2], p))
    if missing:
        print('no size for', ', '.join(sorted(missing)))
    allmed = weighted_median([(r[3], r[2]) for r in rows])
    total = sum(r[2] for r in rows)
    print(f'{os.path.basename(path)}: textured area {total:.2f} m2, median {allmed:.0f} texels/m')

    def summary(sel):
        area = sum(r[2] for r in sel)
        med = weighted_median([(r[3], r[2]) for r in sel])
        off = sum(r[2] for r in sel if not (0.5 * allmed <= r[3] <= 2 * allmed)) / area
        stretched = sum(r[2] for r in sel if r[4] > 1.25) / area
        return area, med, off, stretched

    print(f'{"texture / finish":34s} {"m2":>7s} {"tex/m":>6s} {"off":>5s} {"str":>5s}')
    for tex in sorted({r[0] for r in rows}):
        sel = [r for r in rows if r[0] == tex]
        a, med, off, st = summary(sel)
        print(f'{tex:34s} {a:7.2f} {med:6.0f} {off:5.0%} {st:5.0%}')
        for fin in sorted({r[1] for r in sel}):
            fs = [r for r in sel if r[1] == fin]
            a, med, off, st = summary(fs)
            if only is None or fin == only:
                print(f'  finish {fin:<3d}{"":23s} {a:7.2f} {med:6.0f} {off:5.0%} {st:5.0%}')
    a, med, off, st = summary(rows)
    print(f'{"ALL":34s} {a:7.2f} {med:6.0f} {off:5.0%} {st:5.0%}')
    bad = sorted(
        (r for r in rows if (only is None or r[1] == only)),
        key=lambda r: -r[2] * max(abs(math.log(max(r[3], 1e-3) / allmed)) - math.log(2), math.log(min(r[4], 1e3)) -
                                  math.log(1.25), 0))
    print('worst (finish, area cm2, tex/m, stretch, centre):')
    for r in bad[:worst]:
        c = [sum(v[i] for v in r[5]) / 3 for i in range(3)]
        print(f'  {r[1]:3d} {r[2] * 1e4:8.1f} {r[3]:7.0f} {r[4]:6.2f}  ({c[0]:.2f}, {c[1]:.2f}, {c[2]:.2f})')


if __name__ == '__main__':
    main()
