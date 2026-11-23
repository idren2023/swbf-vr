"""Relief for a stock character (alternative to rebuilding its garments; idren2023 2026-10-26): a height map in the
stock's own texture layout from its fabric maps (each layer raised by its thickness, edges softened: cloth, not
shelves) plus fine relief from the paint's shading. Read by tools/char_relief_view.py (displace + 4x texture).
Run: re/pyenv/Scripts/python.exe -I tools/char_relief.py tools/chars/<character>.json <out height.png> [config key]
"""
import json, sys
import numpy as np
from PIL import Image
from scipy import ndimage

cfg = json.load(open(sys.argv[1]))
out = sys.argv[2]
rel = cfg[sys.argv[3] if len(sys.argv) > 3 else 'relief']     # another key: e.g. relief_base (folds only)
island = np.load(cfg['texmaps'])['island']
N = island.shape[0]
h = np.zeros((N, N), np.float32)
rows, cols = np.mgrid[0:N, 0:N]
for layer in rel['layers']:            # in order: later layers lie over earlier ones
    m = np.asarray(Image.open(layer['mask']).convert('L')) > 127 if 'mask' in layer else np.ones((N, N), bool)
    if 'rows' in layer:
        m = m & (rows >= layer['rows'][0]) & (rows < layer['rows'][1])
    if 'cols' in layer:
        m = m & (cols >= layer['cols'][0]) & (cols < layer['cols'][1])
    if 'islands' in layer:
        m = m & np.isin(island, layer['islands'])
    soft = ndimage.gaussian_filter(m.astype(np.float32), layer.get('soft_px', 6))
    h = np.where(soft > 0.02, h * (1 - soft) + layer['mm'] * soft, h) if layer.get('replace') else h + layer['mm'] * soft
    if 'edge' in layer:
        # the edge as cloth (idren2023 2026-10-26: seams and folds from the maps, no separate pieces): a rolled lip just
        # inside it, a groove just outside it (the shadow of an edge lying over the layer under it); gaussian bumps at
        # lip_px inside and groove_px outside the edge, by the distance to it in texels
        e = layer['edge']
        d_in = ndimage.distance_transform_edt(m)
        d_out = ndimage.distance_transform_edt(~m)
        near = np.isin(island, layer['islands']) if 'islands' in layer else np.ones_like(m)
        lip = e['lip_mm'] * np.exp(-((d_in - e['lip_px']) / (0.6 * e['lip_px'])) ** 2) * m
        groove = e['groove_mm'] * np.exp(-((d_out - e['groove_px']) / (0.6 * e['groove_px'])) ** 2) * ~m
        h += (lip + groove) * near
# fine relief: the painted shading, high-passed (lighter = raised)
hd = np.asarray(Image.open(cfg['hd']).convert('L'), np.float32) / 255
hd = ndimage.gaussian_filter(hd, rel.get('detail_smooth_px', 0))     # the paint's grain out first (else dimples)
detail = hd - ndimage.gaussian_filter(hd, rel['detail_px'])
h += rel['detail_mm'] * np.clip(detail / (detail.std() * 3), -1, 1)
# only the body's fabrics carry relief (the packs, gloves and head keep the stock's shape)
if 'islands' in rel:
    h = np.where(np.isin(island, rel['islands']), h, 0.0)
# the unused texels: the nearest used texel's height (no dark rims where the mesh samples at an island's edge)
used = island >= 0
idx = ndimage.distance_transform_edt(~used, return_distances=False, return_indices=True)
h = h[tuple(idx)]
lo, hi_ = float(h.min()), float(h.max())
Image.fromarray(((h - lo) / (hi_ - lo) * 65535).astype(np.uint16)).save(out)
json.dump({'min_mm': lo, 'max_mm': hi_}, open(out.rsplit('.', 1)[0] + '.json', 'w'))
print('RELIEF height %.2f .. %.2f mm -> %s' % (lo, hi_, out))
