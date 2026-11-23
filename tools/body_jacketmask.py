"""The green jacket's and hood's paint on the stock texture -> re/body/jacket_mask.png (2048).

idren2023 2026-10-25 outlined them (roughly) on phone screenshots of the texture: the hood on the back panel, the band
between the vest's hem and the belt (back and front; it stops at the belt), the lapels at the front between the vest's
edges. The screenshots were matched to the texture by correlation (scale and offset, NCC 0.94 / 0.98 / 0.83); the
outlines below are those strokes read off re/body/jacket_strokes.png (half the pixels of the crop x >= 0.33, y < 0.75
of the texture, y down). The outlines are tightened by colour: not the vest (its map), not the grey T-shirt and neck,
not the plain background.
Run: blender -b --factory-startup -P tools/body_jacketmask.py
"""
import bpy, numpy as np, os

X0 = 0.33 * 2048


def tex_uv(px, py):
    return ((X0 + 2 * px) / 2048, 1 - 2 * py / 2048)


POLYS = {
    'hood': [(88, 238), (130, 226), (165, 238), (240, 240), (282, 224), (308, 232), (306, 265), (282, 298), (240, 330),
             (195, 352), (155, 340), (110, 300), (88, 268)],
    'band': [(25, 544), (686, 544), (686, 590), (25, 590)],
    'lapels': [(398, 205), (445, 210), (470, 240), (560, 240), (612, 238), (606, 262), (570, 320), (556, 360),
               (556, 552), (452, 552), (452, 372), (425, 300), (400, 250)],
}
im = bpy.data.images.load(os.path.abspath('re/body/hd/All_Inf_trooper-hd.png'))
W, H = im.size
px = np.array(im.pixels[:], dtype=np.float32).reshape(H, W, 4)
vm = bpy.data.images.load(os.path.abspath('re/body/vest_mask.png'))
vest = np.array(vm.pixels[:], dtype=np.float32).reshape(H, W, 4)[..., 0] > 0.5
ys, xs = np.mgrid[0:H, 0:W]
u = (xs + 0.5) / W
v = (ys + 0.5) / H


def inside(poly):
    pts = [tex_uv(*p) for p in poly]
    res = np.zeros((H, W), bool)
    for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1]):
        cond = (y1 > v) != (y2 > v)
        xint = (x2 - x1) * (v - y1) / ((y2 - y1) if y2 != y1 else 1e-9) + x1
        res ^= cond & (u < xint)
    return res


region = np.zeros((H, W), bool)
for p in POLYS.values():
    region |= inside(p)
# the hood lies on top of the vest (idren2023): inside its outline the vest map does not cut it
vest &= ~inside(POLYS['hood'])
r, g, b = px[..., 0], px[..., 1], px[..., 2]
lum = 0.3 * r + 0.59 * g + 0.11 * b
sat = px[..., :3].max(-1) - px[..., :3].min(-1)
# the background: the flat dark teal (blue about level with green, low red); the jacket's green has blue well under green
bg_ref = np.median(px[int(0.787 * H):int(0.80 * H), int(0.608 * W):int(0.622 * W), :3].reshape(-1, 3), 0)  # above the hood
print('JACKET background sample', np.round(bg_ref, 3))
background = (np.abs(px[..., :3] - bg_ref).max(-1) < 0.035)
grey = (sat < 0.10) & (lum > 0.30)
chroma = r - b
beige = (chroma > 0.035) & ((g - b) < 0.86 * chroma) & (lum > 0.14)      # the vest's hue (as tools/body_vestmask.py)
mask = region & ~vest & ~grey & ~background & ~beige


def grow(m, rad):
    out = m.copy()
    for dy in range(-rad, rad + 1):
        for dx in range(-rad, rad + 1):
            if dx * dx + dy * dy <= rad * rad:
                out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


mask = grow(~grow(~mask, 3), 3)            # specks dropped
# holes the green encloses (its own highlights) filled, as in the vest map
reach = ~region
for _ in range(400):
    nxt = reach.copy()
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nxt |= np.roll(np.roll(reach, dy, 0), dx, 1)
    nxt &= ~mask
    if (nxt == reach).all():
        break
    reach = nxt
mask = ~reach & region & ~vest
out = np.zeros((H, W, 4), np.float32)
out[..., :3] = mask[..., None]
out[..., 3] = 1
mi = bpy.data.images.new('jacketmask', W, H)
mi.pixels[:] = out.ravel()
mi.filepath_raw = os.path.abspath('re/body/jacket_mask.png')
mi.file_format = 'PNG'
mi.save()
# preview: the torso crop, the jacket bright with a red edge, the vest's edge in orange
c0, c1, r0, r1 = int(0.33 * W), W, int(0.20 * H), int(0.64 * H)
c = px[::-1][r0:r1, c0:c1].copy()
m = mask[::-1][r0:r1, c0:c1]
vv = vest[::-1][r0:r1, c0:c1]
c[..., :3] = np.where(m[..., None], c[..., :3], c[..., :3] * 0.3)
c[(vv ^ np.roll(vv, 1, 0)) | (vv ^ np.roll(vv, 1, 1)), :3] = (1, 0.55, 0)
c[(m ^ np.roll(m, 1, 0)) | (m ^ np.roll(m, 1, 1)), :3] = (1, 0, 0)
ci = bpy.data.images.new('crop', c1 - c0, r1 - r0)
ci.pixels[:] = c[::-1].ravel()
ci.scale((c1 - c0) // 2, (r1 - r0) // 2)
ci.filepath_raw = os.path.abspath('re/body/jacket_mask_preview.png')
ci.file_format = 'PNG'
ci.save()
print('JACKET region %.1f%%, jacket %.1f%% of the texture' % (region.mean() * 100, mask.mean() * 100))
