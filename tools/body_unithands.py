"""python tools/body_unithands.py <arms msh> <out.png> [size=256] [map=<out.z>]: a unit's hand texture in our glove's layout (PLAN 60
step 3, 2026-11-01: the Wookiee).

The Wookiee's first-person hands (all_1st_wookie.msh, imp_inf_trooper_r_hand / _l_hand) are the Rebel trooper's glove
mesh point for point, but unwrapped into a quarter of its one atlas (all_1st_inf_wookie.tga) with uvs of their own. Our
hand wears the trooper glove's layout (all_1st_inf_trooper_hands), so the texture is baked across: each texel of the
glove layout is found in a triangle of the trooper's stock hand (by its uv), the same triangle of the unit's hand (same
corners by position) gives the unit's uv at the same barycentric point, and the unit's texture is sampled there
(bilinear). The right hand first, then the left for texels only it covers; texels in no triangle are filled from their
neighbours (8 passes), so the mip chain does not bleed grey. tools/body_export.py embeds the result under the name
UNIT_SKINS gives the unit's hands (BodyArm takes it in place of a game texture of that name). map= also writes, per texel,
the unit texture's pixel it was sampled nearest (int16 x, y; grown texels take their first neighbour's; -1 none), zlib:
tools/texpatch.py's remap base (the DLL makes it again from the player's own texture)."""
import os, sys
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

MSH = 'C:/LucasArts/BFBuilder/Assets/Sides/ALL/MSH/'
args = sys.argv[1:]
UNIT, OUT = args[0], args[1]
opt = dict(a.split('=', 1) for a in args[2:] if '=' in a)
SIZE = int(opt.get('size', 256))


def segment(path, node_name):
    model = mshgeo.load(path)
    node = next(n for n in model['nodes'] if n['name'].lower() == node_name)
    return node['segments'][0]


def key(p):
    return tuple(round(x, 4) for x in p)


def triangles(seg):
    """Each triangle as (corner position keys, corner uvs)."""
    return [((key(seg['pos'][a]), key(seg['pos'][b]), key(seg['pos'][c])), (seg['uv'][a], seg['uv'][b], seg['uv'][c]))
            for a, b, c in seg['tris']]


trooper_path = MSH + 'all_1st_trooper.msh'
unit_path = UNIT if os.path.exists(UNIT) else MSH + UNIT + '.msh'
texture_name = segment(unit_path, 'imp_inf_trooper_r_hand').get('texture')
source = Image.open(os.path.join(os.path.dirname(unit_path), texture_name)).convert('RGBA')
sw, sh = source.size
src = source.load()


def sample(u, v):
    x, y = u * sw - 0.5, (1.0 - v) * sh - 0.5  # msh v runs up the image
    x0, y0 = int(x // 1), int(y // 1)
    fx, fy = x - x0, y - y0
    out = [0.0] * 4
    for dx, dy, w in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)), (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
        c = src[max(0, min(sw - 1, x0 + dx)), max(0, min(sh - 1, y0 + dy))]
        for i in range(4):
            out[i] += c[i] * w
    return tuple(int(round(c)) for c in out)


image = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
px = image.load()
filled = [[False] * SIZE for _ in range(SIZE)]
nearest = [[(-1, -1)] * SIZE for _ in range(SIZE)]
matched = missing = 0
for side in ('r', 'l'):
    stock = triangles(segment(trooper_path, 'imp_inf_trooper_%s_hand' % side))
    unit, uvs_at = {}, {}
    seg = segment(unit_path, 'imp_inf_trooper_%s_hand' % side)
    for p, uv in zip(seg['pos'], seg['uv']):
        uvs_at.setdefault(key(p), []).append(uv)
    for corners, uvs in triangles(seg):
        unit[frozenset(corners)] = (corners, uvs)
    for corners, uvs in stock:
        found = unit.get(frozenset(corners))
        if found:
            matched += 1
            order = [found[0].index(c) for c in corners]  # the unit triangle's corners in the stock one's order
            target = [found[1][i] for i in order]
        elif all(c in uvs_at for c in corners):
            # triangulated differently: each corner's uv from the unit's points there, the combination with the
            # shortest uv edges (not across a seam)
            missing += 1
            target = min(([a, b, c] for a in uvs_at[corners[0]] for b in uvs_at[corners[1]] for c in uvs_at[corners[2]]),
                         key=lambda t: sum(abs(t[i][0] - t[i - 1][0]) + abs(t[i][1] - t[i - 1][1]) for i in range(3)))
        else:
            continue
        (u0, v0), (u1, v1), (u2, v2) = uvs
        det = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)
        if abs(det) < 1e-12:
            continue
        xs = [u * SIZE for u in (u0, u1, u2)]
        ys = [(1 - v) * SIZE for v in (v0, v1, v2)]
        for y in range(max(0, int(min(ys)) - 1), min(SIZE, int(max(ys)) + 2)):
            for x in range(max(0, int(min(xs)) - 1), min(SIZE, int(max(xs)) + 2)):
                if filled[y][x]:
                    continue
                u, v = (x + 0.5) / SIZE, 1.0 - (y + 0.5) / SIZE
                b1 = ((u - u0) * (v2 - v0) - (u2 - u0) * (v - v0)) / det
                b2 = ((u1 - u0) * (v - v0) - (u - u0) * (v1 - v0)) / det
                b0 = 1 - b1 - b2
                if min(b0, b1, b2) < -0.02:
                    continue
                tu = b0 * target[0][0] + b1 * target[1][0] + b2 * target[2][0]
                tv = b0 * target[0][1] + b1 * target[1][1] + b2 * target[2][1]
                px[x, y] = sample(tu, tv)[:3] + (255,)
                filled[y][x] = True
                nearest[y][x] = (max(0, min(sw - 1, int(tu * sw))), max(0, min(sh - 1, int((1.0 - tv) * sh))))
covered = sum(map(sum, filled))
for _ in range(8):  # grow the islands' edges outward
    grown = []
    for y in range(SIZE):
        for x in range(SIZE):
            if filled[y][x]:
                continue
            ns = [(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
                  if 0 <= x + dx < SIZE and 0 <= y + dy < SIZE and filled[y + dy][x + dx]]
            near = [px[a, b] for a, b in ns]
            if near:
                grown.append((x, y, tuple(sum(c[i] for c in near) // len(near) for i in range(3)) + (255,),
                              nearest[ns[0][1]][ns[0][0]]))
    for x, y, c, n in grown:
        px[x, y] = c
        filled[y][x] = True
        nearest[y][x] = n
image.save(OUT)
if 'map' in opt:
    import struct
    import zlib
    with open(opt['map'], 'wb') as f:
        f.write(zlib.compress(b''.join(struct.pack('<hh', *nearest[y][x]) for y in range(SIZE) for x in range(SIZE)), 9))
print('UNITHANDS %s: %s from %s, triangles matched %d, by corners %d, texels covered %.1f %%' % (
    OUT, UNIT, texture_name, matched, missing, 100.0 * covered / SIZE / SIZE))
