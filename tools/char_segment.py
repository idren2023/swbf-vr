"""Step 2 of the per-character pipeline: the stock texture's paint cut into flat-colour regions, numbered, for labelling
by fabric. The cut is made on the 'plastic' upscale (Upscayl digital-art-4x: flat areas, clean edges; idren2023
2026-10-25), never across a texture island's edge. Writes, into the output folder:
  regions.npy      region id per texel (-1 unused)
  regions.json     per region: area, mean colour, island, where on the body (mean position and normal), neighbours
  sheet-<n>.png    the regions outlined and numbered over the texture, island by island, at full resolution
Run with the project's image environment (re/pyenv, see docs/WORKFLOW.md):
  re/pyenv/Scripts/python.exe tools/char_segment.py <plastic.png> <texmaps.npz> <out dir>
"""
import json, os, sys
import numpy as np
import cv2
from PIL import Image
from skimage import color, graph, segmentation

plastic, maps, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
rgb = np.asarray(Image.open(plastic).convert('RGB'), np.float32) / 255
m = np.load(maps)
island, pos, nrm = m['island'], m['pos'], m['nrm']
N = rgb.shape[0]
lab = color.rgb2lab(rgb)
used = island >= 0

# 1. the paint flattened with its edges kept (mean-shift filter: each texel moves to the mode of its colour
# neighbourhood), then every used texel put in one of K paint classes (k-means in Lab): edges exact to the texel
K = 8
flat = cv2.pyrMeanShiftFiltering((rgb * 255).astype(np.uint8), 8, 14)
flab = color.rgb2lab(flat.astype(np.float32) / 255)
# lightness weighted down: shading changes lightness, a fabric keeps its hue (a*, b*) in shadow
WL = 0.25
samples = (flab[used] * np.array([WL, 1, 1])).astype(np.float32)
crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 50, 0.2)
cv2.setRNGSeed(1)
_, cls, centres = cv2.kmeans(samples, K, None, crit, 4, cv2.KMEANS_PP_CENTERS)
klass = np.full((N, N), -1, np.int32)
klass[used] = cls.ravel()
# 2. a majority filter (radius 3) takes out single-texel noise without moving edges
votes = np.stack([cv2.blur((klass == c).astype(np.float32), (7, 7)) for c in range(K)], -1)
klass = np.where(used, votes.argmax(-1), -1)
# 3. regions: connected pieces of one class inside one island; specks (under MIN texels) joined to the neighbour of
# closest colour
MIN = 300
labels = np.full((N, N), -1, np.int64)
nxt = 0
for isl in np.unique(island[used]):
    for c in range(K):
        n_c, comp = cv2.connectedComponents(((island == isl) & (klass == c)).astype(np.uint8), connectivity=4)
        for j in range(1, n_c):
            labels[comp == j] = nxt
            nxt += 1
merged = labels.copy()
k3 = np.ones((3, 3), np.uint8)
for _ in range(4):
    ids, counts = np.unique(merged[merged >= 0], return_counts=True)
    small = ids[counts < MIN]
    if not len(small):
        break
    means = {}
    for sid in small:
        msk = merged == sid
        if not msk.any():
            continue
        ys_, xs_ = np.nonzero(msk)
        y0, y1, x0, x1 = max(ys_.min() - 1, 0), ys_.max() + 2, max(xs_.min() - 1, 0), xs_.max() + 2
        sm = msk[y0:y1, x0:x1]
        ring = cv2.dilate(sm.astype(np.uint8), k3).astype(bool) & ~sm
        sub = merged[y0:y1, x0:x1]
        isl_ = island[y0:y1, x0:x1]
        nb = np.unique(sub[ring & (sub >= 0) & (isl_ == island[ys_[0], xs_[0]])])
        if not len(nb):
            continue
        mc = flab[msk].mean(0)
        best = min(nb, key=lambda n_: np.linalg.norm(means.setdefault(n_, flab[merged == n_].mean(0)) - mc))
        merged[msk] = best
CUT = K
# renumbered 0..n-1 in reading order (top to bottom, left to right) so the numbers are easy to find
ids = [i for i in np.unique(merged) if i >= 0]
cy = {i: np.argwhere(merged == i).mean(0) for i in ids}
order = sorted(ids, key=lambda i: (int(cy[i][0] // 128), cy[i][1]))
remap = np.full(merged.max() + 2, -1)
for new, old in enumerate(order):
    remap[old] = new
regions = remap[merged]
np.save(os.path.join(out, 'regions.npy'), regions)

# the table
table = []
k3 = np.ones((3, 3), np.uint8)
for r in range(len(order)):
    msk = regions == r
    ring = cv2.dilate(msk.astype(np.uint8), k3).astype(bool) & ~msk
    nb = sorted(int(n_) for n_ in np.unique(regions[ring]) if n_ >= 0)
    table.append({'id': r, 'area': int(msk.sum()), 'island': int(np.bincount(island[msk]).argmax()),
                  'rgb': [round(float(c), 3) for c in rgb[msk].mean(0)],
                  'lab': [round(float(c), 1) for c in lab[msk].mean(0)],
                  'pos': [round(float(c), 3) for c in pos[msk].mean(0)],
                  'nrm': [round(float(c), 2) for c in nrm[msk].mean(0)],
                  'uv_box': [int(v) for v in (*np.argwhere(msk).min(0), *np.argwhere(msk).max(0))],
                  'neighbours': nb})
json.dump(table, open(os.path.join(out, 'regions.json'), 'w'), indent=0)

# 4. numbered sheets: tiles of TILE texels over the used texture, region edges in red, each number at the point
# deepest inside its region (where it cannot be mistaken for a neighbour's)
TILE = 512
edge = segmentation.find_boundaries(regions, mode='inner') & used
vis = (rgb * 255).astype(np.uint8).copy()
vis[~used] = (vis[~used] * 0.25).astype(np.uint8)
vis[edge] = (255, 0, 0)
for r in range(len(order)):
    msk = (regions == r).astype(np.uint8)
    dist = cv2.distanceTransform(np.pad(msk, 1), cv2.DIST_L2, 3)[1:-1, 1:-1]
    y, x = np.unravel_index(dist.argmax(), dist.shape)
    fs = 0.45 if dist.max() > 9 else 0.32
    for col, th in (((0, 0, 0), 3), ((255, 255, 0), 1)):
        cv2.putText(vis, str(r), (int(x) - 8, int(y) + 5), cv2.FONT_HERSHEY_SIMPLEX, fs, col, th, cv2.LINE_AA)
n_ = 0
for ty in range(0, N, TILE):
    for tx in range(0, N, TILE):
        if used[ty:ty + TILE, tx:tx + TILE].mean() < 0.05:
            continue
        Image.fromarray(vis[ty:ty + TILE, tx:tx + TILE]).save(os.path.join(out, 'sheet-%d-%d.png' % (ty // TILE, tx // TILE)))
        n_ += 1
Image.fromarray(vis).resize((1024, 1024), Image.LANCZOS).save(os.path.join(out, 'overview.png'))
print('SEGMENT %d regions over %d islands (cells %d, merged at %.0f Lab), %d sheets -> %s' % (
    len(order), len(np.unique(island[used])), nxt, CUT, n_, out))

# the paint classes: their map, their colours, and the texture painted in each class's colour (to name them by)
np.save(os.path.join(out, 'classes.npy'), klass)
centres = centres / np.array([WL, 1, 1], np.float32)
cl = [{'class': c, 'rgb': [round(float(v), 3) for v in color.lab2rgb(centres[c][None, None])[0, 0]],
       'share': round(float((klass == c).mean() / used.mean()), 3)} for c in range(K)]
json.dump(cl, open(os.path.join(out, 'classes.json'), 'w'), indent=0)
PAL = np.array([(230, 25, 75), (60, 180, 75), (255, 225, 25), (0, 130, 200), (245, 130, 48), (145, 30, 180),
                (70, 240, 240), (240, 50, 230), (210, 245, 60), (250, 190, 212), (0, 128, 128), (170, 110, 40)],
               np.uint8)
fc = np.where(used[..., None], PAL[np.maximum(klass, 0) % len(PAL)], 0).astype(np.uint8)
for c in range(K):
    y, x = 40 + 40 * c, 20
    fc[y - 30:y + 6, x:x + 36] = PAL[c]
    fc[y - 30:y + 6, x + 40:x + 76] = (np.array(cl[c]['rgb']) * 255).astype(np.uint8)
    cv2.putText(fc, str(c), (x + 84, y), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2, cv2.LINE_AA)
Image.fromarray(fc).resize((1024, 1024), Image.NEAREST).save(os.path.join(out, 'classes.png'))
for c in cl:
    print('CLASS %(class)d rgb %(rgb)s share %(share).3f' % c)
