"""Seal check of a cockpit shell from the pilot's eye (PLAN 54): six cube faces rendered as ID buffers, each pixel
classed by what the eye sees first along it:
    cockpit   the game's cockpit model (opaque parts)      fine; the shell must not stand in front of it
    window    the cockpit's glass (nodes named *window*)    fine only if no shell triangle lies behind it on that ray
              (a shell part before the glass, such as the front console's brow over the window's lower edge, is
              counted apart as `before glass`: it hides the glass, nothing is seen through it)
    glass     the shell's own glass (usemtl #glass)         fine whatever lies behind it (the hull outside, or sky)
    shell     the shell                                     fine
    hole      nothing at all                                a gap: the outside would show
Prints the shares and writes a picture: cockpit grey, window blue, glass light blue, shell green, hole red, shell in
front of the cockpit yellow, shell behind the window magenta, shell before the window cyan.

    python tools/shellcheck.py <cockpit.msh> <shell.obj> <out.png> [--window-from other.msh dz] [--size N]
                               [--eye X Y Z]
--eye checks from another head position (a VR head moves: eye +-15 cm shows gaps the resting eye cannot).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import meshview  # noqa: E402
import mshgeo  # noqa: E402

EYE = (0.0, 1.5, 1.5)
FACES = [  # forward, up for each cube face
    ((0, 0, -1), (0, 1, 0)), ((1, 0, 0), (0, 1, 0)), ((0, 0, 1), (0, 1, 0)),
    ((-1, 0, 0), (0, 1, 0)), ((0, 1, 0), (0, 0, 1)), ((0, -1, 0), (0, 0, -1)),
]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def raster(tris, forward, up, n, near=0.03):
    """Nearest distance per pixel (along forward) for the triangles, inf where none."""
    right = cross(forward, up)
    depth = [[math.inf] * n for _ in range(n)]
    half = n / 2.0
    for p0, p1, p2 in tris:
        v = []
        for p in (p0, p1, p2):
            d = (p[0] - EYE[0], p[1] - EYE[1], p[2] - EYE[2])
            v.append((sum(d[i] * right[i] for i in range(3)), sum(d[i] * up[i] for i in range(3)),
                      sum(d[i] * forward[i] for i in range(3))))
        if all(q[2] < near for q in v):
            continue
        if any(q[2] < near for q in v):
            # clip crudely: push the near vertices to the near plane (enough for a coverage check)
            v = [(q[0], q[1], max(q[2], near)) for q in v]
        s = [(half + q[0] / q[2] * half, half - q[1] / q[2] * half, q[2]) for q in v]
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = s
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-12:
            continue
        minx, maxx = max(0, int(min(x0, x1, x2))), min(n - 1, int(max(x0, x1, x2)) + 1)
        miny, maxy = max(0, int(min(y0, y1, y2))), min(n - 1, int(max(y0, y1, y2)) + 1)
        iz = (1 / z0, 1 / z1, 1 / z2)
        for py in range(miny, maxy + 1):
            fy = py + 0.5
            row = depth[py]
            for px in range(minx, maxx + 1):
                fx = px + 0.5
                a = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area
                b = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area
                c = 1 - a - b
                if a < -1e-6 or b < -1e-6 or c < -1e-6:
                    continue
                z = 1 / (a * iz[0] + b * iz[1] + c * iz[2])
                if z < row[px]:
                    row[px] = z
    return depth


def main():
    args = sys.argv[1:]
    msh, obj, out = args[0], args[1], args[2]
    n = 96
    extra_window = None
    i = 3
    while i < len(args):
        if args[i] == '--size':
            n = int(args[i + 1])
            i += 2
        elif args[i] == '--eye':
            global EYE
            EYE = tuple(float(v) for v in args[i + 1:i + 4])
            i += 4
        elif args[i] == '--window-from':
            extra_window = (args[i + 1], float(args[i + 2]))
            i += 3
        else:
            i += 1
    model = mshgeo.load(msh)
    opaque, window = [], []
    for node in model['nodes']:
        is_window = 'window' in node.get('name', '').lower()
        for s in node['segments']:
            for a, b, c in s['tris']:
                (window if is_window else opaque).append((s['wpos'][a], s['wpos'][b], s['wpos'][c]))
    if extra_window:
        other = mshgeo.load(extra_window[0])
        dz = extra_window[1]
        for node in other['nodes']:
            if 'window' in node.get('name', '').lower():
                for s in node['segments']:
                    for a, b, c in s['tris']:
                        window.append(tuple((p[0], p[1], p[2] + dz) for p in (s['wpos'][a], s['wpos'][b], s['wpos'][c])))
    shellTris = meshview.load_obj(obj)
    shell = [t[0] for t in shellTris if t[2] != '#glass']
    glass = [t[0] for t in shellTris if t[2] == '#glass']
    counts = {'cockpit': 0, 'window': 0, 'glass': 0, 'shell': 0, 'hole': 0, 'front': 0, 'through': 0, 'before glass': 0}
    images = []
    for forward, up in FACES:
        dc, dw, ds = raster(opaque, forward, up, n), raster(window, forward, up, n), raster(shell, forward, up, n)
        dg = raster(glass, forward, up, n)
        img = []
        for y in range(n):
            row = []
            for x in range(n):
                c, w, s, g = dc[y][x], dw[y][x], ds[y][x], dg[y][x]
                if g < math.inf and g < c and g < w:
                    if s < g - 0.01:
                        counts['shell'] += 1
                        row.append((40, 170, 60))
                    else:
                        counts['glass'] += 1
                        row.append((160, 220, 255))
                elif c < math.inf and c <= w:
                    if s < c - 0.03:
                        counts['front'] += 1
                        row.append((255, 230, 0))
                    else:
                        counts['cockpit'] += 1
                        row.append((110, 110, 110))
                elif w < math.inf:
                    if s < w - 0.01:
                        counts['before glass'] += 1
                        row.append((0, 230, 230))
                    elif s < math.inf:
                        counts['through'] += 1
                        row.append((255, 0, 255))
                    else:
                        counts['window'] += 1
                        row.append((60, 120, 255))
                elif s < math.inf:
                    counts['shell'] += 1
                    row.append((40, 170, 60))
                else:
                    counts['hole'] += 1
                    row.append((230, 20, 20))
            img.append(row)
        images.append(img)
    total = sum(counts.values())
    print(' '.join('%s %.2f%%' % (k, 100.0 * v / total) for k, v in counts.items()))
    # Cross layout: front, right, back, left in a row; up above front, down below front.
    gap = [(255, 255, 255)]
    grid = []
    blank = [(0, 0, 0)] * n
    for y in range(n):
        grid.append(images[4][y] + gap + blank + gap + blank + gap + blank)
    for y in range(n):
        grid.append(images[0][y] + gap + images[1][y] + gap + images[2][y] + gap + images[3][y])
    for y in range(n):
        grid.append(images[5][y] + gap + blank + gap + blank + gap + blank)
    meshview.write_png(out, len(grid[0]), len(grid), grid)


if __name__ == '__main__':
    main()
