"""python tools/meshpatch.py: our game-derived meshes (the units' forearms, the droid arms, the player body, the seated
trooper) as places on the player's own game model plus our offsets, so the mod carries none of the game's geometry
(docs/research/runtime-assets.md, the meshes step). Writes tools/data/meshpatch/<TABLE>.z and meshpatch.json; the
build turns them into build/gen/MeshPatches.inc (tools/meshpatch_inc.py) and src/GameMeshes.cpp rebuilds the tables
at game start.

Per table: the game model (level file, name, FNV-1a of its modl chunk) and a similarity T (ours: fitted here, game
model space -> the table's space). Per vertex: a triangle of the game model (index over its segments' triangles in
order), the barycentric place on it (b1, b2) of the base point, and in the triangle's frame (e1 along its first edge,
n its normal, e2 = n x e1, all after T) the offset of our position from the base point and our normal; our uv as
the difference from the game's uv at the base point. The reconstruction (double, then float) is replayed here and
must give the source table within 1e-5.

Sources (developer's PC only): re/body/BodyArm.inc, re/body/PlayerBody.inc (tools/body_rebuild_all.sh) and
re/body/SeatedTrooper.inc (tools/build.sh: tools/seated_trooper.py from re/body/seated)."""
import json
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path = [p for p in sys.path if os.path.abspath(p or '.') != HERE] + [HERE]  # tools/dis.py shadows `dis`
import numpy as np  # noqa: E402
from scipy.spatial import cKDTree  # noqa: E402
import lvlgeo  # noqa: E402

LVL = 'C:/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront (Classic 2004)/GameData/Data/_lvl_pc/'
OUT = os.path.join(HERE, 'data', 'meshpatch')
ARM, BODY, SEATED = 're/body/BodyArm.inc', 're/body/PlayerBody.inc', 're/body/SeatedTrooper.inc'
# table -> (source include, level file, game model)
TABLES = {
    'ALL_1ST_WOOKIE_VERTICES': (ARM, 'FPM/ALL/allwook.lvl', 'all_1st_wookie'),
    'HAIR_ALL_1ST_WOOKIE_VERTICES': (ARM, 'FPM/ALL/allwook.lvl', 'all_1st_wookie'),
    'HAIR_ALL_1ST_WOOKIE_HAND_VERTICES': (ARM, 'FPM/ALL/allwook.lvl', 'all_1st_wookie'),
    'IMP_1ST_DARKTROOPER_VERTICES': (ARM, 'FPM/IMP/impdark.lvl', 'imp_1st_darktrooper'),
    'IMP_1ST_STORMTROOPER_VERTICES': (ARM, 'FPM/IMP/impstrp.lvl', 'imp_1st_stormtrooper'),
    'REP_1ST_TROOPER_VERTICES': (ARM, 'FPM/REP/reptroop.lvl', 'rep_1st_trooper'),
    'B1_ARM_VERTICES': (ARM, 'FPM/CIS/cisbdrd.lvl', 'cis_1st_bdroid'),
    'B1_HAND_VERTICES': (ARM, 'FPM/CIS/cisbdrd.lvl', 'cis_1st_bdroid'),
    'SBD_ARM_VERTICES': (ARM, 'FPM/CIS/cissbdrd.lvl', 'cis_1st_sbdroid'),
    'SBD_ARM_L_VERTICES': (ARM, 'FPM/CIS/cissbdrd.lvl', 'cis_1st_sbdroid'),
    'SBD_HAND_VERTICES': (ARM, 'FPM/CIS/cissbdrd.lvl', 'cis_1st_sbdroid'),
    'PLAYER_BODY_VERTICES_0': (BODY, 'SIDE/all.lvl', 'all_inf_soldier'),
    'PLAYER_BODY_VERTICES_1': (BODY, 'SIDE/all.lvl', 'all_inf_soldier'),
    'PLAYER_BODY_VERTICES_2': (BODY, 'SIDE/all.lvl', 'all_inf_soldier'),
    'PLAYER_BODY_VERTICES_3': (BODY, 'SIDE/all.lvl', 'all_inf_soldier'),
    'SEATED_TROOPER': (SEATED, 'SIDE/rep.lvl', 'rep_inf_trooper'),
}
NUM = r'(-?[\d.]+(?:e-?\d+)?)f?'


def source_table(path, name):
    """(n, 8) float32: position, normal, uv of each vertex (the first eight numbers of each row)."""
    text = open(path).read()
    m = re.search(r'%s\[\d*\] = \{[^\n]*\n(.*?)\n\};' % name, text, re.S)
    rows = []
    for line in m.group(1).split('\n'):
        nums = re.findall(r'-?\d+(?:\.\d*)?(?:e-?\d+)?', line)
        rows.append([float(v) for v in nums[:8]])
    return np.array(rows, dtype=np.float32)


def game(lvl, name):
    model = lvlgeo.load(LVL + lvl, name)
    pos, uv, tris = [], [], []
    for s in model['segments']:
        base = len(pos)
        pos += s['wpos']
        uv += [list(t) for t in s['uv']]
        tris += [[base + a, base + b, base + c] for a, b, c in s['tris']]
    return np.array(pos, dtype=np.float64), np.array(uv, dtype=np.float64), np.array(tris), model


def umeyama(a, b):
    """s, R, t with b ~ s R a + t (row vectors: b = s a R^T + t), no reflection."""
    ma, mb = a.mean(0), b.mean(0)
    A, B = a - ma, b - mb
    U, S, Vt = np.linalg.svd(B.T @ A / len(a))
    D = np.eye(3)
    D[2, 2] = np.sign(np.linalg.det(U @ Vt))
    R = U @ D @ Vt
    s = (S * np.diag(D)).sum() / (A ** 2).sum() * len(a)
    return s, R, mb - s * ma @ R.T


def fit(ours, g, tris, bones):
    """Our similarity game -> table space (only keeps the offsets small; any T reconstructs exactly): from every bone
    frame as a start, rounds of our points' nearest places on the game surface (dense samples of its triangles), the
    nearest 80 % fitted; the start with the best median kept."""
    w = [(i / 4, j / 4) for i in range(5) for j in range(5 - i)]
    a0, a1, a2 = g[tris[:, 0]], g[tris[:, 1]], g[tris[:, 2]]
    samples = np.concatenate([a0 + u * (a1 - a0) + v * (a2 - a0) for u, v in w])
    best = None
    starts = [(np.eye(3), np.zeros(3))]
    for name, frame in bones.items():
        if name != '__order__':
            inv = np.linalg.inv(np.array(frame[0]))
            starts.append((inv.T, -np.array(frame[1]) @ inv))
    for R, t in starts:
        s = 1.0
        for _ in range(30):
            cur = s * samples @ R.T + t
            d, i = cKDTree(cur).query(ours)
            keep = d <= max(0.002, np.percentile(d, 80))
            if keep.sum() < 10:
                break
            s, R, t = umeyama(samples[i[keep]], ours[keep])
            if not np.isfinite(s) or s <= 0:
                break
        if not np.isfinite(s) or s <= 0:
            continue
        d, _ = cKDTree(s * samples @ R.T + t).query(ours)
        if best is None or np.median(d) < best[0]:
            best = (np.median(d), s, R, t)
    return best


def frames(a, tris):
    a0, a1, a2 = a[tris[:, 0]], a[tris[:, 1]], a[tris[:, 2]]
    e1, f2 = a1 - a0, a2 - a0
    n = np.cross(e1, f2)
    e1n = e1 / np.linalg.norm(e1, axis=1, keepdims=True)
    nn = n / np.linalg.norm(n, axis=1, keepdims=True)
    return a0, e1, f2, e1n, nn, np.cross(nn, e1n)


def encode(name):
    src, lvl, model_name = TABLES[name]
    ours = source_table(src, name).astype(np.float64)
    g, guv, tris, model = game(lvl, model_name)
    score, s, R, t = fit(ours[:, :3], g, tris, model['bones'])
    M = (s * R).astype(np.float32).astype(np.float64)  # x' = M x + t, stored as floats
    t = t.astype(np.float32).astype(np.float64)
    a = g @ M.T + t
    area = np.linalg.norm(np.cross(a[tris[:, 1]] - a[tris[:, 0]], a[tris[:, 2]] - a[tris[:, 0]]), axis=1)
    good = np.nonzero(area > 1e-10)[0]
    a0, e1, f2, e1n, nn, e2n = frames(a, tris)
    centres = (a[tris[good, 0]] + a[tris[good, 1]] + a[tris[good, 2]]) / 3
    _, cand = cKDTree(centres).query(ours[:, :3], k=min(16, len(good)))
    cand = good[cand]  # (n, k) triangle indices
    p = ours[:, None, :3]
    # place on each candidate's plane, barycentrics clamped to the triangle: the nearest of them is the base
    d = p - a0[cand]
    E1, F2 = e1[cand], f2[cand]
    d11, d12, d22 = (E1 * E1).sum(-1), (E1 * F2).sum(-1), (F2 * F2).sum(-1)
    q1, q2 = (d * E1).sum(-1), (d * F2).sum(-1)
    det = d11 * d22 - d12 * d12
    b1 = (d22 * q1 - d12 * q2) / det
    b2 = (d11 * q2 - d12 * q1) / det
    b1, b2 = np.clip(b1, 0, 1), np.clip(b2, 0, 1)
    over = b1 + b2 > 1
    b1[over], b2[over] = b1[over] / (b1 + b2)[over], b2[over] / (b1 + b2)[over]
    base = a0[cand] + b1[..., None] * E1 + b2[..., None] * F2
    pick = np.linalg.norm(p - base, axis=-1).argmin(1)
    rows = np.arange(len(ours))
    tri = cand[rows, pick]
    b1 = b1[rows, pick].astype(np.float32).astype(np.float64)  # stored as float: the base from the stored values
    b2 = b2[rows, pick].astype(np.float32).astype(np.float64)
    base = a0[tri] + b1[:, None] * e1[tri] + b2[:, None] * f2[tri]
    off = ours[:, :3] - base
    ax = np.stack([e1n[tri], e2n[tri], nn[tri]], 1)  # (n, 3 axes, 3)
    o = np.einsum('nij,nj->ni', ax, off)
    nl = np.einsum('nij,nj->ni', ax, ours[:, 3:6])
    # uv: the table's v runs down or up against the level file's; whichever gives the smaller differences
    gu = guv[tris[tri, 0]] + b1[:, None] * (guv[tris[tri, 1]] - guv[tris[tri, 0]]) + b2[:, None] * (
        guv[tris[tri, 2]] - guv[tris[tri, 0]])
    flips = []
    for flip in (0, 1):
        gv = gu.copy()
        if flip:
            gv[:, 1] = 1 - gv[:, 1]
        flips.append((np.abs(ours[:, 6:8] - gv).mean(), flip, gv))
    _, flip, gv = min(flips, key=lambda f: f[0])
    duv = ours[:, 6:8] - gv
    values = np.concatenate([b1[:, None], b2[:, None], o, nl, duv], 1).astype(np.float32)
    meta = {'lvl': lvl, 'model': model_name, 'hash': model['hash'], 'flip_v': flip, 'count': len(ours),
            'transform': [float(x) for x in np.concatenate([M.reshape(-1), t])]}
    return meta, tri.astype(np.int32), values, ours, score, np.linalg.norm(off, axis=1)


def decode(meta, tri, values, g, guv, tris):
    """The reconstruction as src/GameMeshes.cpp does it (double, then float)."""
    T = np.array(meta['transform'], dtype=np.float32).astype(np.float64)
    M, t = T[:9].reshape(3, 3), T[9:]
    a = g @ M.T + t
    a0, e1, f2, e1n, nn, e2n = frames(a, tris)
    v = values.astype(np.float64)
    b1, b2 = v[:, 0:1], v[:, 1:2]
    base = a0[tri] + b1 * e1[tri] + b2 * f2[tri]
    ax = (e1n[tri], e2n[tri], nn[tri])
    pos = base + v[:, 2:3] * ax[0] + v[:, 3:4] * ax[1] + v[:, 4:5] * ax[2]
    nrm = v[:, 5:6] * ax[0] + v[:, 6:7] * ax[1] + v[:, 7:8] * ax[2]
    g0, g1, g2 = guv[tris[tri, 0]], guv[tris[tri, 1]], guv[tris[tri, 2]]
    gu = g0 + b1 * (g1 - g0) + b2 * (g2 - g0)
    if meta['flip_v']:
        gu[:, 1] = 1 - gu[:, 1]
    return np.concatenate([pos, nrm, gu + v[:, 8:10]], 1).astype(np.float32)


def main():
    os.makedirs(OUT, exist_ok=True)
    index = {}
    worst = 0
    for name in TABLES:
        meta, tri, values, ours, score, dist = encode(name)
        src, lvl, model_name = TABLES[name]
        g, guv, tris, _ = game(lvl, model_name)
        back = decode(meta, tri, values, g, guv, tris)
        err = np.abs(back - ours.astype(np.float32)).max()
        worst = max(worst, err)
        blob = struct.pack('<I', meta['count']) + tri.tobytes() + values.tobytes()
        open(os.path.join(OUT, name + '.z'), 'wb').write(zlib.compress(blob, 9))
        index[name] = meta
        print('%-34s %5d vertices on %-22s fit median %.4f m; offsets median %.4f max %.4f m; uv flip %d; '
              'replay max error %.2g' % (name, len(ours), model_name, score, np.median(dist), dist.max(),
                                         meta['flip_v'], err))
    json.dump(index, open(os.path.join(OUT, 'meshpatch.json'), 'w'), indent=1)
    print('meshpatch: %d tables, worst replay error %.2g' % (len(index), worst))
    sys.exit(0 if worst < 1e-5 else 1)


if __name__ == '__main__':
    main()
