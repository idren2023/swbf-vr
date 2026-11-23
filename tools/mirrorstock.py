#!/usr/bin/env python3
"""The combat speeder's passenger half as the mirror of the game's pilot half (idren2023 2026-10-19: the front right window
framed as the left, the left dashboard's displays mirrored to the right, the cabin symmetric about its middle).

    python tools/mirrorstock.py [all_1st_cockpit_combatspeeder.msh] [out.inc]

Reads the mod tools' first-person model (not in the repository: the generated file goes to build/gen/, never into
git) and writes a C++ include for src/ShellDesigns.cpp: the triangles of its window, canopy, dashboard, floorboard and
light nodes left of the cabin's middle line (x 0.58), clipped at it and mirrored across it, with their texture
coordinates and texture (1 cockpit, 2 effects, 4 window glass); and the parts right of the line, pushed 5 mm toward
the cabin, as the depth mask that hides the game's own right half (CockpitShell draws it depth only). Without the msh
it writes empty tables (the speeder then has no right front).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

MIDDLE = 0.58
NODES = ('window', 'canopy', 'dashboard', 'floorboard', 'lightfx')
CABIN = (MIDDLE, 1.5, 1.2)  # the mask is pushed toward this point
DEFAULT_MSH = 'C:/LucasArts/BFBuilder/Assets/Sides/ALL/MSH/all_1st_cockpit_combatspeeder.msh'


def clip(poly, keep_left):
    """Sutherland-Hodgman against x = MIDDLE; poly: [(pos, uv, normal)]."""
    out = []
    for i, (p, t, n) in enumerate(poly):
        q, s, m = poly[(i + 1) % len(poly)]
        inside_p = p[0] <= MIDDLE if keep_left else p[0] >= MIDDLE
        inside_q = q[0] <= MIDDLE if keep_left else q[0] >= MIDDLE
        if inside_p:
            out.append((p, t, n))
        if inside_p != inside_q:
            k = (MIDDLE - p[0]) / (q[0] - p[0])
            out.append((tuple(p[j] + (q[j] - p[j]) * k for j in range(3)), tuple(t[j] + (s[j] - t[j]) * k for j in range(2)),
                        tuple(n[j] + (m[j] - n[j]) * k for j in range(3))))
    return out


def unit(v):
    length = sum(c * c for c in v) ** 0.5 or 1
    return tuple(c / length for c in v)


def normal(a, b, c):
    e1 = [b[i] - a[i] for i in range(3)]
    e2 = [c[i] - a[i] for i in range(3)]
    n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
    length = sum(v * v for v in n) ** 0.5 or 1
    n = [v / length for v in n]
    centre = [(a[i] + b[i] + c[i]) / 3 for i in range(3)]
    if sum(n[i] * (CABIN[i] - centre[i]) for i in range(3)) < 0:
        n = [-v for v in n]
    return n


def main():
    msh = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MSH
    out = sys.argv[2] if len(sys.argv) > 2 else 'build/gen/SpeederStockMirror.inc'
    mirror, mask = [], []
    import modelsource
    if modelsource.exists(msh):
        import mshgeo
        model = modelsource.load(msh)
        for node in model['nodes']:
            name = node.get('name', '').lower()
            if not mshgeo.is_drawn(node) or not name.startswith(NODES):
                continue
            for segment in node['segments']:
                texture = segment['texture'].lower()
                kind = 4 if 'xwingwin' in texture else 2 if '_fx' in texture else 1
                uv = segment.get('uv') or [(0, 0)] * len(segment['wpos'])
                normals = segment.get('wnrm') or []
                for a, b, c in segment['tris']:
                    face = normal(*[segment['wpos'][i] for i in (a, b, c)])
                    # The model's own vertex normals (smooth shading, as the game lights it; b333's face normals
                    # showed every facet under the map's sun), else the face's.
                    tri = [(tuple(segment['wpos'][i]), (uv[i][0], 1 - uv[i][1]),
                            unit(normals[i]) if i < len(normals) else tuple(face)) for i in (a, b, c)]
                    left = clip(tri, True)
                    for k in range(1, len(left) - 1):
                        for p, t, n in (left[0], left[k], left[k + 1]):
                            # Mirrored, except on the middle line itself, where the game's own half goes on with its
                            # normal unmirrored: no shading seam there.
                            on_line = abs(p[0] - MIDDLE) < 1e-4
                            mirror.append(((2 * MIDDLE - p[0], p[1], p[2]), n if on_line else (-n[0], n[1], n[2]), t, kind))
                    if kind == 4:
                        continue  # the game's glass is see-through: nothing to hide behind it
                    right = clip(tri, False)
                    for k in range(1, len(right) - 1):
                        corners = [right[0][0], right[k][0], right[k + 1][0]]  # pushed along the face toward the cabin
                        n = normal(*corners)
                        for p in corners:
                            mask.append(tuple(p[i] + n[i] * 0.005 for i in range(3)))
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'w') as f:
        f.write('// Generated by tools/mirrorstock.py from %s: not in git, do not edit.\n' % os.path.basename(msh))
        f.write('constexpr int SPEEDER_MIRROR_COUNT = %d;\n' % len(mirror))
        f.write('constexpr StockVertex SPEEDER_MIRROR[%d] = {\n' % max(len(mirror), 1))
        for p, n, t, kind in mirror:
            f.write('    {{%.4ff, %.4ff, %.4ff}, {%.4ff, %.4ff, %.4ff}, %.5ff, %.5ff, %d},\n' % (*p, *n, *t, kind))
        if not mirror:
            f.write('    {{0, 0, 0}, {0, 1, 0}, 0, 0, 1},\n')
        f.write('};\n')
        f.write('constexpr int SPEEDER_MASK_COUNT = %d;\n' % len(mask))
        f.write('constexpr ShellKit::Vec3 SPEEDER_MASK[%d] = {\n' % max(len(mask), 1))
        for p in mask:
            f.write('    {%.4ff, %.4ff, %.4ff},\n' % p)
        if not mask:
            f.write('    {0, 0, 0},\n')
        f.write('};\n')
    print('%s: %d mirrored triangles, %d mask triangles' % (out, len(mirror) // 3, len(mask) // 3))


if __name__ == '__main__':
    main()
