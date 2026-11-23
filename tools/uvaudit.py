"""Texturing audit of the cockpit shells (idren2023 2026-11-09: a perfectionist's UV / texturing pass over every seat).

Reads re/shells/<design>.obj (tools/shellpreview.sh writes them) and reports, per design, how much of the surface is
plain colour (no texture: "#flat") and how much is textured but stretched: for each textured triangle the mapping's
two singular values (texture units per metre along its two principal directions); their ratio over 1.6 counts as
stretched (a patch pulled out of proportion), and the area-weighted share is printed per finish, worst first.
Displays (HUD rectangles), glow, glass and the vehicle's own exterior (the game's hull) are left out.

    python tools/uvaudit.py [design ...]        (default: every re/shells/*.obj)
    python tools/uvaudit.py --finishes design   (every finish of one design: area, flat or stretch)
    python tools/uvaudit.py --where design finish  (where that finish is stretched, by area)
"""
import glob
import math
import os
import sys

STRETCH = 1.6


def load(path):
    vs, vts, tris = [], [], []
    material, finish, color = None, None, None
    exterior = '-'
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('# exterior '):
            exterior = line.split()[2]
        elif line.startswith('v '):
            vs.append(tuple(float(x) for x in line.split()[1:4]))
        elif line.startswith('vt '):
            vts.append(tuple(float(x) for x in line.split()[1:3]))
        elif line.startswith('usemtl '):
            material, finish, color = line.split(None, 1)[1].strip(), None, None
        elif line.startswith('# color '):
            color = line.split()[2]
        elif line.startswith('# finish '):
            finish = int(line.split()[2])
        elif line.startswith('f '):
            idx = [tuple(int(k) - 1 for k in c.split('/')) for c in line.split()[1:4]]
            if material != exterior:  # the game's own hull: its mapping is the game's
                tris.append((material, finish, color, [vs[i] for i, _ in idx], [vts[j] for _, j in idx]))
    return tris


def sub(a, b):
    return [a[k] - b[k] for k in range(len(a))]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(a[k] * b[k] for k in range(len(a)))


def stretch(p, t):
    """Area (m^2) and the ratio of the mapping's singular values (1: in proportion)."""
    e1, e2 = sub(p[1], p[0]), sub(p[2], p[0])
    n = cross(e1, e2)
    area = math.sqrt(dot(n, n)) / 2
    if area < 1e-8:
        return 0, 1
    # a 2D frame in the triangle's plane
    x = [c / math.sqrt(dot(e1, e1)) for c in e1]
    y = cross([c / (2 * area) for c in n], x)
    q1, q2 = (dot(e1, x), dot(e1, y)), (dot(e2, x), dot(e2, y))
    d1, d2 = sub(t[1], t[0]), sub(t[2], t[0])
    det = q1[0] * q2[1] - q1[1] * q2[0]
    if abs(det) < 1e-12:
        return area, 1
    # J maps metres (in-plane) to texture units: J = [d1 d2] [q1 q2]^-1
    inv = ((q2[1] / det, -q2[0] / det), (-q1[1] / det, q1[0] / det))
    J = [[d1[r] * inv[0][c] + d2[r] * inv[1][c] for c in range(2)] for r in range(2)]
    a, b, c, d = J[0][0], J[0][1], J[1][0], J[1][1]
    s = a * a + b * b + c * c + d * d
    disc = math.sqrt(max(0.0, (a * a + b * b - c * c - d * d) ** 2 + 4 * (a * c + b * d) ** 2))
    big, small = math.sqrt((s + disc) / 2), math.sqrt(max(0.0, (s - disc) / 2))
    if small < 1e-9:
        return area, 99.0  # degenerate: the whole face on a line of texels
    return area, big / small


def audit(path, perFinish=False):
    tris = load(path)
    flat = tex = bad = 0.0
    finishes = {}
    for material, finish, color, p, t in tris:
        if material is None or material.startswith('#glow') or material.startswith('#glass'):
            continue
        area, ratio = stretch(p, t) if not material.startswith('#flat') else (stretch(p, [(0, 0), (1, 0), (0, 1)])[0], 1)
        key = (finish, material, color)
        f = finishes.setdefault(key, [0.0, 0.0, 0.0])
        f[0] += area
        if material.startswith('#flat'):
            flat += area
            f[1] += area
        else:
            tex += area
            if ratio > STRETCH:
                bad += area
                f[2] += area
    name = os.path.splitext(os.path.basename(path))[0]
    total = flat + tex
    if total <= 0:
        return
    print('%-28s %6.2f m2  flat %3.0f%%  stretched %3.0f%%' % (name, total, 100 * flat / total, 100 * bad / total))
    if perFinish:
        for (finish, material, color), (area, f, b) in sorted(finishes.items(), key=lambda kv: -(kv[1][1] + kv[1][2])):
            if f + b > 0.001:
                print('   finish %-3s %-34s %s  %.3f m2  %s' % (finish, material[:34], color, area,
                                                             'FLAT' if f else 'stretched %.0f%%' % (100 * b / area)))


def main():
    args = sys.argv[1:]
    if args and args[0] == '--where':
        where(args[1], int(args[2]))
        return
    perFinish = '--finishes' in args
    args = [a for a in args if a != '--finishes']
    paths = ['re/shells/%s.obj' % a for a in args] if args else sorted(glob.glob('re/shells/*.obj'))
    for path in paths:
        audit(path, perFinish)



def where(design, finish):
    """Where a finish's stretched triangles are: their centres, grouped to 5 cm, the largest areas first."""
    spots = {}
    for material, f, color, p, t in load('re/shells/%s.obj' % design):
        if f != finish or material.startswith('#'):
            continue
        area, ratio = stretch(p, t)
        if ratio > STRETCH:
            c = tuple(round(sum(q[k] for q in p) / 3 / 0.05) * 0.05 for k in range(3))
            spots[c] = spots.get(c, 0) + area
    for c, a in sorted(spots.items(), key=lambda kv: -kv[1])[:15]:
        print('   %6.2f %6.2f %6.2f  %.3f m2' % (c[0], c[1], c[2], a))


if __name__ == '__main__':
    main()
