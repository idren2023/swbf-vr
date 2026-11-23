"""Our arm and body textures as differences from the player's own game textures (docs/research/runtime-assets.md step
4): the DLL decodes the game's texture from the level files, resamples it to our size (integer bilinear,
lvlgeo.resize_bilinear = src/GameTextures.cpp) and adds the difference, per channel modulo 256, which gives our texture
exactly. The difference is useless without the game's texture.

    python tools/texpatch.py            from build/gen/BodyArm.inc and PlayerBody.inc (the textures as compiled today)

Writes tools/data/texpatch/<TABLE>.z (zlib of the differences, 0xAARRGGBB little-endian per pixel) and
tools/data/texpatch/texpatch.json (per table: level file, game texture, size, the game texture's fnv1a). For each table
the game texture giving the smallest difference is chosen among CANDIDATES.
"""
import json
import os
import re
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
# tools/ holds a dis.py, which would shadow the standard module numpy imports: tools/ goes last on the path
sys.path = [p for p in sys.path if os.path.abspath(p or '.') != HERE] + [HERE]
import numpy as np  # noqa: E402

import lvlgeo  # noqa: E402

LVL = 'C:/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront (Classic 2004)/GameData/Data/_lvl_pc/'
OUT = os.path.join(HERE, 'data', 'texpatch')
SOURCES = [('build/gen/BodyArm.inc', r'BODY_ARM_TEXTURE_\d+'), ('build/gen/PlayerBody.inc', r'PLAYER_BODY_TEXELS_\d+')]
CANDIDATES = [('FPM/ALL/alltroop.lvl', 'all_1st_inf_trooper_arms'), ('FPM/ALL/alltroop.lvl', 'all_1st_inf_trooper_hands'),
              ('FPM/ALL/allwook.lvl', 'all_1st_inf_wookie'), ('FPM/CIS/cissbdrd.lvl', 'cis_1st_sbdroid_hands'),
              ('SIDE/all.lvl', 'All_Inf_trooper'), ('SIDE/all.lvl', 'All_Inf_Trooper_packs')]


def arrays(path, pattern):
    """name -> list of ARGB from `constexpr unsigned NAME[] = {...};`"""
    text = open(path).read()
    out = {}
    for m in re.finditer(r'constexpr unsigned (%s)\[\] = \{(.*?)\};' % pattern, text, re.S):
        out[m.group(1)] = [int(v.rstrip('u'), 16) for v in re.findall(r'0x[0-9a-fA-F]+u?', m.group(2))]
    return out


def channels(argb):
    a = np.array(argb, dtype=np.uint32)
    return np.stack([(a >> s) & 255 for s in (24, 16, 8, 0)], axis=-1).astype(np.int32)


# Base recipes where one game texture resized is a poor base (texpatch.json carries them to the DLL): regions of the
# target, each from a source (a game texture: [lvl, name], or a table already built: "TABLE") cropped to `from`
# (source pixels x0, y0, x1, y1) and resized to `to` (target pixels), then `op`: copy, luma (grey by brightness: per
# channel (k * luminance >> 8) + m, fitted here; alpha kept) or fill (a flat colour, fitted here). Elsewhere 0.
SLEEVE_X = 169  # all_1st_inf_trooper_arms: the sleeve left of the cuff (u 0.662, tools/body_recolour.py)
RECIPES = {
    'BODY_ARM_TEXTURE_0': [
        {'source': ['FPM/ALL/alltroop.lvl', 'all_1st_inf_trooper_arms'], 'from': [0, 0, SLEEVE_X, 256],
         'to': [0, 0, SLEEVE_X, 256], 'op': 'luma'},
        {'source': ['FPM/ALL/alltroop.lvl', 'all_1st_inf_trooper_arms'], 'from': [SLEEVE_X, 0, 256, 256],
         'to': [SLEEVE_X, 0, 256, 256], 'op': 'copy'}],
    # the Wookiee's hands baked into our glove's layout (tools/body_unithands.py map=): per texel a pixel of the game's
    'BODY_ARM_TEXTURE_1': [
        {'source': ['FPM/ALL/allwook.lvl', 'all_1st_inf_wookie'], 'from': [0, 0, 256, 256], 'to': [0, 0, 256, 256],
         'op': 'remap', 'map': 'BODY_ARM_TEXTURE_1.map.z'}],
    'PLAYER_BODY_TEXELS_1': [
        {'source': 'BODY_ARM_TEXTURE_0', 'from': [0, 0, 256, 256], 'to': [0, 0, 1024, 512], 'op': 'copy'},
        {'source': None, 'to': [0, 512, 1024, 1024], 'op': 'fill'}],
}


def crop(px, w, x0, y0, x1, y1):
    return [px[y * w + x] for y in range(y0, y1) for x in range(x0, x1)]


def luminance(c):
    return (77 * ((c >> 16) & 255) + 150 * ((c >> 8) & 255) + 29 * (c & 255)) >> 8


def apply_op(px, op, params):
    if op == 'copy':
        return px
    if op == 'fill':
        return [params[0] << 24 | params[1] << 16 | params[2] << 8 | params[3]] * len(px)
    out = []  # luma: params k_r, m_r, k_g, m_g, k_b, m_b
    for c in px:
        lum = luminance(c)
        v = c & 0xFF000000
        for i, shift in enumerate((16, 8, 0)):
            v |= max(0, min(255, (params[2 * i] * lum >> 8) + params[2 * i + 1])) << shift
        out.append(v)
    return out


def fit(op, region_ours, region_src):
    """The op's parameters that bring the source region nearest ours (least squares, integers)."""
    if op == 'copy':
        return []
    o = channels(region_ours)
    if op == 'fill':
        return [int(round(v)) for v in o.mean(axis=0)]
    lum = np.array([luminance(c) for c in region_src], dtype=float)
    params = []
    for i in (1, 2, 3):
        k, m = np.polyfit(lum, o[:, i], 1)
        params += [int(round(k * 256)), int(round(m))]
    return params


def build_base(recipe, ours_px, side, built, games, sources_used):
    """The base image of a recipe (and its fitted parameters filled in); `built` holds the tables made so far."""
    base = [0] * (side * side)
    for region in recipe:
        x0, y0, x1, y1 = region['to']
        rw, rh = x1 - x0, y1 - y0
        if region['op'] == 'remap':
            import struct
            key = tuple(region['source'])
            w, h, px, fmt, hsh = games[key]
            sources_used.append({'lvl': key[0], 'texture': key[1], 'fnv1a': hsh})
            raw = zlib.decompress(open(os.path.join(OUT, region['map']), 'rb').read())
            out = []
            for sx, sy in struct.iter_unpack('<hh', raw):
                out.append(px[sy * w + sx] if sx >= 0 else 0)
            region['params'] = []
            for y in range(rh):
                base[(y0 + y) * side + x0:(y0 + y) * side + x1] = out[y * rw:(y + 1) * rw]
            continue
        if region['source'] is None:
            src_px = [0] * (rw * rh)
        else:
            if isinstance(region['source'], str):
                w, px = built[region['source']]
            else:
                key = tuple(region['source'])
                w, h, px, fmt, hsh = games[key]
                sources_used.append({'lvl': key[0], 'texture': key[1], 'fnv1a': hsh})
            a0, b0, a1, b1 = region['from']
            src_px = lvlgeo.resize_bilinear(crop(px, w, a0, b0, a1, b1), a1 - a0, b1 - b0, rw, rh)
        region_ours = crop(ours_px, side, x0, y0, x1, y1)
        if 'params' not in region:
            region['params'] = fit(region['op'], region_ours, src_px)
        out = apply_op(src_px, region['op'], region['params'])
        for y in range(rh):
            base[(y0 + y) * side + x0:(y0 + y) * side + x1] = out[y * rw:(y + 1) * rw]
    return base


def main():
    os.makedirs(OUT, exist_ok=True)
    games = {}
    for lvl, name in CANDIDATES:
        t = lvlgeo.load_texture(LVL + lvl, name)
        if t:
            games[(lvl, name)] = t
    meta = {}
    built = {}
    for path, pattern in SOURCES:
        for table, ours in arrays(path, pattern).items():
            side = int(round(len(ours) ** 0.5))
            assert side * side == len(ours), table
            o = channels(ours)
            sources_used = []
            if table in RECIPES:
                recipe = [dict(r) for r in RECIPES[table]]
                base_px = build_base(recipe, ours, side, built, games, sources_used)
                entry = {'recipe': recipe}
                label = 'recipe'
            else:
                best = None
                for key, (w, h, px, fmt, hsh) in games.items():
                    g = channels(px).reshape(h, w, 4)
                    yy = (np.arange(side) * h // side)[:, None]
                    xx = (np.arange(side) * w // side)[None, :]
                    score = np.abs(g[yy, xx].reshape(-1, 4) - o).mean()
                    if best is None or score < best[0]:
                        best = (score, key)
                key = best[1]
                w, h, px, fmt, hsh = games[key]
                sources_used.append({'lvl': key[0], 'texture': key[1], 'fnv1a': hsh})
                recipe = [{'source': list(key), 'from': [0, 0, w, h], 'to': [0, 0, side, side], 'op': 'copy',
                           'params': []}]
                base_px = build_base(recipe, ours, side, built, games, [])
                entry = {'recipe': recipe}
                label = key[1]
            base = channels(base_px)
            delta = ((o - base) % 256).astype(np.uint32)
            signed = ((o - base + 128) % 256) - 128
            entry.update({'width': side, 'height': side, 'sources': sources_used,
                          'unchanged': bool((delta == 0).all())})
            if not entry['unchanged']:
                packed = (delta[:, 0] << 24) | (delta[:, 1] << 16) | (delta[:, 2] << 8) | delta[:, 3]
                open(os.path.join(OUT, table + '.z'), 'wb').write(zlib.compress(packed.astype('<u4').tobytes(), 9))
            elif os.path.exists(os.path.join(OUT, table + '.z')):
                os.remove(os.path.join(OUT, table + '.z'))
            meta[table] = entry
            built[table] = (side, ours)
            size = os.path.getsize(os.path.join(OUT, table + '.z')) // 1024 if not entry['unchanged'] else 0
            print('%-22s %-26s -> %4d  mean |difference| %5.1f  %4d kB' % (
                table, label, side, np.abs(signed).mean(), size))
    json.dump(meta, open(os.path.join(OUT, 'texpatch.json'), 'w'), indent=1, sort_keys=True)


if __name__ == '__main__':
    main()
