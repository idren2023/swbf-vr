"""The player body's two baked textures (tools/body_assemble.py, idren2023 2026-11-04: rebuildable from git): the image
work tools/body_assemble.py does, apart from its geometry, so that it can run again from a recorded recipe
(tools/data/models/player/bake.json: the elbow strips' uv rings and cells, the glove's uvs by the wrist and the glove
faces by the seam; ours, no pixels) on the textures tools/body_textures.sh makes from the game's.

    blender -b --factory-startup -P tools/body_bake.py -- [bake=tools/data/models/player/bake.json] [out_dir=re/body]

writes <out_dir>/player_arms.png (the grey sleeve 4x with its cuff made the glove's leather, above; the elbow strips
below) and <out_dir>/player_hands.png (the glove texture with the painted highlights by the seam cleaned).
"""
import json
import math
import os
import sys

import bpy
from mathutils import Vector

HIGHLIGHT_OVER = 0.08      # a texel this much lighter than the leather round it is a highlight


def loader(path):
    im = bpy.data.images.load(path)
    return im.size[0], im.size[1], im.pixels[:]


def sample(tex, u, v):
    w, h, px = tex
    x, y = (u % 1.0) * w - 0.5, (v % 1.0) * h - 0.5
    x0, y0 = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - x0, y - y0
    out = [0.0, 0.0, 0.0, 0.0]
    for dy, wy in ((0, 1 - fy), (1, fy)):
        for dx, wx in ((0, 1 - fx), (1, fx)):
            i = 4 * (((y0 + dy) % h) * w + (x0 + dx) % w)
            for c in range(4):
                out[c] += px[i + c] * wx * wy
    return out


def leather_as_glove(tex, hands_tex, glove_uvs, CUFF_U=0.662):
    """The cuff's leather (the sleeve texture past CUFF_U) made the glove's (idren2023 2026-10-25: one leather glove):
    its painted dark band at the wrist end painted over with its plain leather from the elbow side, then each channel moved
    from the cuff's mean and spread to the glove's (sampled near the wrist), the brass stud left as it is."""
    w, h, px = tex
    px = list(px)
    x0, xb = int(CUFF_U * w) + 2, int(0.925 * w)
    shift = int(0.205 * w)          # from the cuff's plain leather by the elbow side (u 0.72..0.795: past its rim,
    for y in range(h):              # before the tab)
        # on the tab's rows the plain leather is taken from further round the cuff (the copy took in the tab's
        # painted edge there: a scar beside the flap)
        ys = (y + int(0.30 * h)) % h if 0.20 * h <= y <= 0.45 * h else y
        for x in range(xb, w):
            xs = x - shift
            px[4 * (y * w + x):4 * (y * w + x) + 4] = px[4 * (ys * w + xs):4 * (ys * w + xs) + 4]

    def is_stud(x, y):
        return math.hypot(x / w - 0.848, y / h - 0.328) < 0.04

    cuff = [px[4 * (y * w + x):4 * (y * w + x) + 3] for y in range(h) for x in range(x0, w) if not is_stud(x, y)]
    glove = [sample(hands_tex, q.x, q.y)[:3] for q in glove_uvs]
    stats = []
    for c in range(3):
        cm = sum(p_[c] for p_ in cuff) / len(cuff)
        gm = sum(p_[c] for p_ in glove) / len(glove)
        cs = (sum((p_[c] - cm) ** 2 for p_ in cuff) / len(cuff)) ** 0.5 or 1e-3
        gs = (sum((p_[c] - gm) ** 2 for p_ in glove) / len(glove)) ** 0.5
        stats.append((cm, cs, gm, gs))
    for y in range(h):
        for x in range(x0 - 4, w):
            if is_stud(x, y):
                continue
            k = min(1.0, (x - (x0 - 4)) / 4)          # eased in over the cuff's first texels
            i = 4 * (y * w + x)
            for c in range(3):
                cm, cs, gm, gs = stats[c]
                v = gm + (px[i + c] - cm) * gs / cs
                px[i + c] = max(0.0, min(1.0, px[i + c] * (1 - k) + v * k))
    print('ASSEMBLE leather: cuff rgb %.2f %.2f %.2f -> glove %.2f %.2f %.2f (%d glove samples)' % (
        stats[0][0], stats[1][0], stats[2][0], stats[0][2], stats[1][2], stats[2][2], len(glove)))
    return w, h, tuple(px)


def seam_blend(tex, v_lo=0.034, v_hi=0.987, band=6):
    """The sleeve texture's two lengthwise edges (where the grid's seam columns meet round the arm) cross-faded over
    `band` texels each side, so the seam's two sides carry the same colour."""
    w, h, px = tex
    px = list(px)
    y_lo, y_hi = int(v_lo * h), int(v_hi * h)
    for i in range(band):
        f = 0.5 * (1 - i / band)
        ya, yb = y_lo + i, y_hi - i
        for x in range(w):
            ia, ib = 4 * (ya * w + x), 4 * (yb * w + x)
            for c in range(3):
                pa, pb = px[ia + c], px[ib + c]
                px[ia + c], px[ib + c] = pa * (1 - f) + pb * f, pb * (1 - f) + pa * f
    return w, h, tuple(px)


def clean_highlights(tex, polys):
    """The glove texture's light patches under the glove's last stretch before the seam (they were hidden in the
    stock's cuff): texels much lighter than the leather round them (a 12-texel blur) take that leather."""
    w, h, px = tex
    px = list(px)
    mask = set()
    for poly in polys:
        xs = [q.x * w for q in poly]
        ys = [q.y * h for q in poly]
        for y in range(max(0, int(min(ys))), min(h, int(max(ys)) + 1)):
            for x in range(max(0, int(min(xs))), min(w, int(max(xs)) + 1)):
                mask.add((x, y))
    lum = [0.3 * px[4 * i] + 0.59 * px[4 * i + 1] + 0.11 * px[4 * i + 2] for i in range(w * h)]
    r = 12
    cleaned = 0
    for (x, y) in mask:
        acc, al, n_ = [0.0, 0.0, 0.0], 0.0, 0
        for yy in range(max(0, y - r), min(h, y + r + 1), 3):
            for xx in range(max(0, x - r), min(w, x + r + 1), 3):
                j = yy * w + xx
                if lum[j] < 0.75:
                    for c in range(3):
                        acc[c] += px[4 * j + c]
                    al += lum[j]
                    n_ += 1
        if not n_:
            continue
        i = y * w + x
        if lum[i] > al / n_ + HIGHLIGHT_OVER:
            k = min(1.0, (lum[i] - al / n_ - HIGHLIGHT_OVER) / 0.08)
            for c in range(3):
                px[4 * i + c] = px[4 * i + c] * (1 - k) + acc[c] / n_ * k
            cleaned += 1
    print('ASSEMBLE glove: %d texels of %d under the glove by the seam cleaned of painted highlights' % (cleaned, len(mask)))
    return w, h, tuple(px)


def side_uv(rings_, sv, k, fr):
    """A side's texture place at sv along the arm, column k (fraction fr): between its rings, or carried on past
    them from the two nearest."""
    i = 0
    while i < len(rings_) - 2 and rings_[i + 1][0] < sv:
        i += 1
    (s0, r0), (s1, r1) = rings_[i], rings_[i + 1]
    t = (sv - s0) / (s1 - s0) if abs(s1 - s0) > 1e-6 else 0.0
    p0 = r0[k][0].lerp(r0[k][1], fr)
    p1 = r1[k][0].lerp(r1[k][1], fr)
    return p0 + (p1 - p0) * t


def paint_strips(canvas, strips, TEX, AW, STRIP_H, STRIP_W):
    """The elbow strips into the lower half of canvas (Blender rows from the bottom), as tools/body_assemble.py made them."""
    for si, strip_ in enumerate(strips):
        if strip_[-1] == 'elbowx':
            side, sa_, sb_, s_lo, s_hi, nn, ta, tb, kind = strip_
            # each side read where it lies and carried on past its end (our sleeve's u reflected at 0: below it the
            # texture holds the cuff); the cross-fade over the whole stretch
            for y in range(STRIP_H):
                t = (y + 0.5) / STRIP_H
                sv = s_lo + t * (s_hi - s_lo)
                blend = t * t * (3 - 2 * t)
                for x in range(STRIP_W):
                    kf = x / STRIP_W * nn
                    k = min(int(kf), nn - 1)
                    fr = kf - k
                    qa = side_uv(sa_, sv, k, fr)
                    qb = side_uv(sb_, sv, k, fr)
                    qb = Vector((abs(qb.x), qb.y))
                    a_ = sample(TEX[ta], qa.x, qa.y)
                    b_ = sample(TEX[tb], qb.x, qb.y)
                    i = 4 * ((si * STRIP_H + y) * AW + x)
                    for c in range(3):
                        canvas[i + c] = a_[c] * (1 - blend) + b_[c] * blend
                    canvas[i + 3] = 1.0
            continue
        (side, ca, cb, gap, na, nb, ta, tb, kind) = strip_
        for y in range(STRIP_H):
            t = (y + 0.5) / STRIP_H
            if kind == 'seam':
                # a narrow change at the middle, a little shadow in the seam's line
                q_ = min(1.0, max(0.0, (t - 0.2) / 0.6))
                blend = q_ * q_ * (3 - 2 * q_)
                shade = 1 - 0.12 * math.exp(-((t - 0.62) / 0.04) ** 2)
            else:
                blend = t * t * (3 - 2 * t)
                shade = 1.0
            for x in range(STRIP_W):
                col = []
                for cells_, nn, tex, is_a in ((ca, na, TEX[ta], True), (cb, nb, TEX[tb], False)):
                    kf = x / STRIP_W * nn
                    k = min(int(kf), nn - 1)
                    fr = kf - k
                    cell = cells_[k]
                    if cell is None:
                        col.append(None)
                        continue
                    uv0, du0, uv1, du1, row = cell
                    # the seam's glove side at the glove's own edge (carried on into the glove its uvs left the
                    # glove's island: a light patch by the thumb)
                    st = 0.0 if (kind == 'seam' and not is_a) else (t if is_a else 1 - t) * gap / row
                    p0, p1 = uv0 + du0 * st, uv1 + du1 * st
                    q = p0.lerp(p1, fr)
                    col.append(sample(tex, q.x, q.y))
                a_, b_ = col
                a_ = a_ or b_
                b_ = b_ or a_
                i = 4 * ((si * STRIP_H + y) * AW + x)
                for c in range(3):
                    canvas[i + c] = (a_[c] * (1 - blend) + b_[c] * blend) * shade
                canvas[i + 3] = 1.0


def to_json(x):
    if isinstance(x, Vector):
        return {'v': [float(c) for c in x]}
    if isinstance(x, (list, tuple)):
        return [to_json(y) for y in x]
    return x


def from_json(x):
    if isinstance(x, dict) and 'v' in x:
        return Vector(x['v'])
    if isinstance(x, list):
        return [from_json(y) for y in x]
    return x


def bake(arms_path, body_path, hands_path, strips, glove_uvs, near_glove_tris, strip_h):
    """Both textures from the three source textures and the recipe: (player_arms (w, h, pixels), player_hands or None)."""
    arms_tex = loader(arms_path)
    body_tex = loader(body_path)
    hands_tex = loader(hands_path)
    arms_tex = seam_blend(arms_tex)
    if glove_uvs:
        arms_tex = leather_as_glove(arms_tex, hands_tex, glove_uvs)
    hands_out = None
    if near_glove_tris:
        hands_tex = hands_out = clean_highlights(hands_tex, near_glove_tris)
    TEX = {'arms': arms_tex, 'body': body_tex, 'hands': hands_tex}
    AW, AH = arms_tex[0], arms_tex[1]
    canvas = [0.0] * (AW * 2 * AH * 4)
    for y in range(AH):      # the sleeve's texture: the upper half
        canvas[(AH + y) * AW * 4:(AH + 1 + y) * AW * 4] = arms_tex[2][y * AW * 4:(y + 1) * AW * 4]
    paint_strips(canvas, strips, TEX, AW, strip_h, AW)
    return (AW, 2 * AH, canvas), hands_out


def save_png(name, tex, path):
    img = bpy.data.images.new(name, tex[0], tex[1], alpha=True)
    img.pixels[:] = tex[2]
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()


if __name__ == '__main__':
    args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    opt = dict(a.split('=', 1) for a in args if '=' in a)
    d = json.load(open(opt.get('bake', os.path.join('tools', 'data', 'models', 'player', 'bake.json')), encoding='utf-8'))
    out = opt.get('out_dir', os.path.join('re', 'body'))
    arms, hands = bake(*(os.path.abspath(d['textures'][k]) for k in ('arms', 'body', 'hands')), from_json(d['strips']),
                       from_json(d['glove_uvs']), from_json(d['near_glove_tris']), d['strip_h'])
    save_png('player_arms', arms, os.path.abspath(os.path.join(out, 'player_arms.png')))
    if hands:
        save_png('player_hands', hands, os.path.abspath(os.path.join(out, 'player_hands.png')))
    print('BAKE player_arms.png %d x %d%s -> %s' % (arms[0], arms[1], ', player_hands.png' if hands else '', out))
