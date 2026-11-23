# python tools/sbd_shell_warp.py [sigma mm]: writes tools/data/sbd_shell_warp.json (read by body_sbd.py). Inputs in tools/data:
# the third droid's camera in Hot Toys photo h08 (solved by hand-point PnP, wrist angles folded in) and the stock shell's
# vertices in the forearm frame (body_sbd.py forearm_place); the vertex numbers below index that list.
"""Edge correspondences (model vertex chains <-> photo polylines, one fitted camera) -> per-vertex displacements of
the stock shell (mm), the chains moved onto the photo edges in the image plane at their depth, the rest following."""
import json, numpy as np, sys
import os; DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
c = json.load(open(os.path.join(DATA, 'sbd_cam_h08_droid3.json'))); R, t, f = np.array(c['R']), np.array(c['t']), c['f']; W, H = c['size']
sh = json.load(open(os.path.join(DATA, 'sbd_shell_stock.json'))); P = np.array(sh['verts'])
def proj(p):
    q = R @ (p / 1000) + t; return np.array([W / 2, H / 2]) + f * q[:2] / q[2], q[2]
pairs = {  # chain of model vertices: photo polyline (h08 pixels)
 'top_left': ([48, 50, 52, 61], [(947.2, 161.5), (932.5, 175.2), (915.7, 193), (903.1, 205.6)]),
 'thumb':    ([61, 77, 82, 90, 94], [(903.1, 205.6), (900, 214), (901, 239.2), (908.4, 272.8)]),
 'lower':    ([94, 95, 104, 105], [(908.4, 272.8), (922, 274.9), (936.7, 283.3), (949.3, 295.9), (958.8, 319), (961.9, 337.9)]),
 'finger':   ([47, 59, 89, 100, 105], [(1009.2, 182.5), (1006, 203.5), (999.7, 228.7), (989.2, 258.1), (976.6, 293.8), (964, 331.6)])}
def along(poly, s):
    poly = np.array(poly, float); seg = np.linalg.norm(np.diff(poly, axis=0), axis=1); L = np.concatenate([[0], np.cumsum(seg)])
    s = s * L[-1]; k = min(np.searchsorted(L, s, side='right') - 1, len(seg) - 1)
    return poly[k] + (poly[k + 1] - poly[k]) * (s - L[k]) / max(seg[k], 1e-9)
disp = {}
for name, (chain, poly) in pairs.items():
    uv = np.array([proj(P[i])[0] for i in chain]); seg = np.linalg.norm(np.diff(uv, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)]) / max(seg.sum(), 1e-9)
    for i, si in zip(chain, s):
        tgt = along(poly, si); cur, z = proj(P[i])
        d = (tgt - cur) * z / f * 1000              # mm in the camera's x, y
        disp.setdefault(i, []).append(R[0] * d[0] + R[1] * d[1])
        print('%-8s v%-3d %5.1f px' % (name, i, np.linalg.norm(tgt - cur)))
fixed = {i: np.mean(v, axis=0) for i, v in disp.items()}
sigma = float(sys.argv[1]) if len(sys.argv) > 1 else 20
out = []
for i, p in enumerate(P):
    if i in fixed: d = fixed[i]
    else:
        w = np.array([np.exp(-np.sum((p - P[j]) ** 2) / sigma ** 2) for j in fixed]); D = np.array(list(fixed.values()))
        d = (w[:, None] * D).sum(0) / (w.sum() + 1.0)   # the +1: points far from every chain stay put
    if np.linalg.norm(d) > 0.05: out.append([[round(x, 1) for x in p], [round(x, 2) for x in d]])
json.dump({'note': 'SBD stock shell displacements (mm, forearm frame after forearm_place): chains of its edges moved '
           'onto the Hot Toys figure\'s edges in photo h08 (third droid) through its fitted camera, the rest following '
           '(gaussian %g mm); made by tools/sbd_shell_warp.py from idren2023\'s colour-matched edges 2026-11-03' % sigma,
           'moves': out}, open(os.path.join(DATA, 'sbd_shell_warp.json'), 'w'), indent=0)
print('moved', len(out), 'of', len(P))
