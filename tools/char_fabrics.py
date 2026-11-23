"""Step 3 of the per-character pipeline: fabric maps from the paint, with no hand outlines. Each texel of the
'plastic' upscale (flattened by a mean-shift filter, edges kept) takes the nearest paint of the character's config
(Lab, lightness weighted down: a fabric keeps its hue in shadow); a fabric is its paints on its islands (and texture
rows), less other fabrics, its enclosed holes filled, its specks dropped. Writes one mask per fabric (white = fabric).
Run: re/pyenv/Scripts/python.exe -I tools/char_fabrics.py tools/chars/<character>.json [reference masks: name=png ...]
"""
import json, sys
import numpy as np
import cv2
from PIL import Image
from scipy import ndimage
from skimage import color

cfg = json.load(open(sys.argv[1]))
refs = dict(a.split('=', 1) for a in sys.argv[2:])
rgb = np.asarray(Image.open(cfg['plastic']).convert('RGB'))
N = rgb.shape[0]
_tm = np.load(cfg['texmaps'])
island, curv, nrm_ = _tm['island'], _tm['curv'], _tm['nrm']
used = island >= 0
WL = 0.25
flab_u = color.rgb2lab(cv2.pyrMeanShiftFiltering(rgb, 8, 14).astype(np.float32) / 255)
flat = flab_u * np.array([WL, 1, 1])
names = list(cfg['paints'])
protos = color.rgb2lab(np.array([cfg['paints'][n] for n in names], np.float32)[None])[0] * np.array([WL, 1, 1])
paint = np.argmin(((flat[..., None, :] - protos) ** 2).sum(-1), -1)
# majority over a 7-texel window: single-texel noise out, edges kept
votes = np.stack([cv2.blur((paint == c).astype(np.float32), (7, 7)) for c in range(len(names))], -1)
paint = votes.argmax(-1)
rows = np.arange(N)[:, None]
masks = {}
for fname, f in cfg['fabrics'].items():
    m = np.isin(paint, [names.index(p) for p in f['paints']]) & np.isin(island, f['islands'])
    if 'rows' in f:
        m &= (rows >= f['rows'][0]) & (rows < f['rows'][1])
    for other in f.get('not', []):
        m &= ~masks[other]
    lab_, n_ = ndimage.label(m)
    if n_:
        sizes = ndimage.sum(m, lab_, range(1, n_ + 1))
        m = np.isin(lab_, 1 + np.nonzero(sizes >= f.get('min_area', 0))[0])
    if f.get('fill_holes'):
        holes = ndimage.binary_fill_holes(m) & ~m
        hl, hn = ndimage.label(holes)
        if hn:
            hs = ndimage.sum(holes, hl, range(1, hn + 1))
            m |= np.isin(hl, 1 + np.nonzero(hs <= f['fill_holes'])[0])
    if f.get('grow'):
        # from the fabric's clean paint outward into touching texels of its hue (its own shading toward edges and seams:
        # darker, but the same hue), up to grow['steps'] texels away
        g = f['grow']
        hue = np.degrees(np.arctan2(flab_u[..., 2], flab_u[..., 1]))
        chroma = np.hypot(flab_u[..., 1], flab_u[..., 2])
        ok = (hue >= g.get('hue_min', -180)) & (hue <= g.get('hue_max', 180)) & (chroma >= g.get('chroma_min', 0)) &              (flab_u[..., 0] >= g.get('L_min', 0)) & np.isin(island, f['islands'])
        for other in f.get('not', []):
            ok &= ~masks[other]
        if 'crease' in g:
            ok &= curv < g['crease']           # the geometry's creases stop it (where it meets the layer under it)
        m = ndimage.binary_dilation(m, iterations=g['steps'], mask=ok | m)
    if f.get('fill_rims'):
        # thin strips of other paint along the fabric (its edges darkened toward a border or a seam: still this
        # fabric); a strip is what an opening of radius fill_rims removes, so wide areas (a hood, a lapel) stay out
        rr = f['fill_rims']
        gaps = ~m & np.isin(island, f['islands'])
        disk = np.hypot(*np.mgrid[-rr:rr + 1, -rr:rr + 1]) <= rr
        thin = gaps & ~ndimage.binary_opening(gaps, disk)
        # only strips that touch the fabric
        tl, tn = ndimage.label(thin)
        touch = np.unique(tl[ndimage.binary_dilation(m) & thin])
        m |= np.isin(tl, touch[touch > 0])
    if f.get('close_px'):
        # notches cut into the fabric's outline (narrower than 2 x close_px) filled: a closing by a disk, kept to its islands
        rr = f['close_px']
        disk = np.hypot(*np.mgrid[-rr:rr + 1, -rr:rr + 1]) <= rr
        m |= ndimage.binary_closing(m, disk) & np.isin(island, f['islands'])
    if f.get('crease_snap'):
        # the edge onto a crease of the stock's geometry running beside it: in a band of +-px texels round the edge, the
        # creases (curvature over min) cut the band into pieces; each piece goes wholly to the side most of it was on
        cs = f['crease_snap']
        band = (ndimage.binary_dilation(m, iterations=cs['px']) & ~ndimage.binary_erosion(m, iterations=cs['px'])) & used
        crease = curv > cs['min']
        lab_b, nb_ = ndimage.label(band & ~crease)
        if nb_:
            share = ndimage.mean(m, lab_b, range(1, nb_ + 1))
            win = np.zeros(nb_ + 1, bool)
            win[1:] = share > 0.5
            m = np.where(band & ~crease, win[lab_b], m)
            m &= ~(band & crease & ~ndimage.binary_dilation(m))   # crease lines themselves: with the side they touch
    if f.get('slivers_to'):
        # thin slivers of this fabric (what an opening of radius px removes) touching another fabric go to it
        st = f['slivers_to']
        rr = st['px']
        disk = np.hypot(*np.mgrid[-rr:rr + 1, -rr:rr + 1]) <= rr
        thin = m & ~ndimage.binary_opening(m, disk)
        tl, tn = ndimage.label(thin)
        other_m = masks[st['fabric']]
        touch = np.unique(tl[thin & ndimage.binary_dilation(other_m, iterations=2)])
        give = np.isin(tl, touch[touch > 0])
        m &= ~give
        masks[st['fabric']] = other_m | give
        Image.fromarray((masks[st['fabric']] * 255).astype(np.uint8)).save(cfg['out'][st['fabric']])
        print('FABRIC %-7s %d slivers (%d texels) to the %s' % (fname, len(touch[touch > 0]), give.sum(), st['fabric']))
    if f.get('underside_to'):
        # texels near another fabric on a surface facing down (a flap's underside: the stock stretches the paint below
        # onto it) go to that fabric
        ut = f['underside_to']
        give = m & (nrm_[..., 2] < ut['nz_max']) & ndimage.binary_dilation(masks[ut['fabric']], iterations=ut['px'])
        m &= ~give
        masks[ut['fabric']] |= give
        Image.fromarray((masks[ut['fabric']] * 255).astype(np.uint8)).save(cfg['out'][ut['fabric']])
        print('FABRIC %-7s %d texels facing down to the %s' % (fname, give.sum(), ut['fabric']))
    masks[fname] = m
    # the mask in Blender's orientation for its image loader is the same PNG: row 0 the texture's top
    Image.fromarray((m * 255).astype(np.uint8)).save(cfg['out'][fname])
    line = 'FABRIC %-7s %.1f%% of the texture' % (fname, m.mean() * 100)
    if fname in refs:
        r = (np.asarray(Image.open(refs[fname]).convert('L')) > 127) & (island >= 0)   # texels the model uses
        line += ', against %s: overlap %.3f (%.2f%% only here, %.2f%% only there)' % (
            refs[fname], (m & r).sum() / max((m | r).sum(), 1), (m & ~r).mean() * 100, (r & ~m).mean() * 100)
    print(line)
# the label texture: each fabric in its colour over the 4x paint, its edges bright (drawn on the stock in 3D by
# tools/char_maps_view.py, to check the maps where they are worn)
COLS = {n: np.array(c, np.float32) for n, c in cfg.get('colours', {}).items()}
hdp = np.asarray(Image.open(cfg['hd']).convert('RGB'), np.float32) / 255
lbl = hdp * 0.45
for fname, m in masks.items():
    c = COLS.get(fname, np.array([1, 0, 1], np.float32))
    lbl[m] = hdp[m] * 0.45 + c * 0.55
    e = m ^ ndimage.binary_erosion(m, iterations=2)
    lbl[e] = c * 0.6 + 0.4
Image.fromarray((np.clip(lbl, 0, 1) * 255).astype(np.uint8)).save(cfg['label_texture'])
# a preview of the torso and pads: each fabric's edge in its own colour over the texture
vis = np.asarray(Image.open(cfg['plastic']).convert('RGB')).copy()
for (fname, m), col in zip(masks.items(), [(255, 140, 0), (0, 255, 255), (255, 0, 255), (255, 255, 0)]):
    e = m ^ ndimage.binary_erosion(m)
    vis[ndimage.binary_dilation(e)] = col
Image.fromarray(vis).save(cfg['out'][next(iter(masks))].rsplit('/', 1)[0] + '/fabrics_preview.png')
