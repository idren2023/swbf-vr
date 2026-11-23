"""Blender (blender -b --factory-startup -P tools/body_recolour.py [-- <arms texture> <out.png>]): our first-person sleeve in the body's grey (idren2023
2026-10-25: "grey all the way": the third-person soldier's shirt sleeve is grey, the stock first-person sleeve beige).

all_1st_inf_trooper_arms.tga's sleeve part (u below the cuff, 0.662) is mapped onto All_Inf_trooper.tga's grey sleeve
(its island u 0.09-0.33, v 0.28-0.55): each texel's luminance moved from the beige's median to the grey's, its
distance from it scaled by CONTRAST (the grey's own spread is about half the beige's; 0.6 keeps the painted folds), coloured by the grey's mean hue; the cuff,
tab and stud untouched, a FEATHER-texel blend at the cuff's edge. Writes re/body/all_1st_inf_trooper_arms_grey.png
(tools/body_export.py embeds it; BodyArm draws it in place of the game's texture). Prints both sleeves' statistics."""
import bpy, os, sys

MSH = 'C:/LucasArts/BFBuilder/Assets/Sides/ALL/MSH/'
ARGS = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
ARMS = ARGS[0] if ARGS else MSH + 'all_1st_inf_trooper_arms.tga'   # or its 2x copy (re/body/hd/*-hd.png)
OUT = os.path.abspath(ARGS[1] if len(ARGS) > 1 else 're/body/all_1st_inf_trooper_arms_grey.png')
CUFF_U, FEATHER, CONTRAST = 0.662, 3, 0.6


def lum(r, g, b):
    return 0.3 * r + 0.59 * g + 0.11 * b


def stats(im, box):
    w, h = im.size
    px = im.pixels[:]
    u0, v0, u1, v1 = box
    acc, ls = [0.0, 0.0, 0.0], []
    for y in range(int(v0 * h), int(v1 * h)):
        for x in range(int(u0 * w), int(u1 * w)):
            i = 4 * (y * w + x)
            for c in range(3):
                acc[c] += px[i + c]
            ls.append(lum(*px[i:i + 3]))
    ls.sort()
    n = len(ls)
    return [a / n for a in acc], ls[n // 10], ls[n // 2], ls[9 * n // 10]


arms = bpy.data.images.load(os.path.abspath(ARMS))
body = bpy.data.images.load(MSH + 'All_Inf_trooper.tga')
b_rgb, b10, b50, b90 = stats(arms, (0.02, 0.05, 0.64, 0.95))
g_rgb, g10, g50, g90 = stats(body, (0.09, 0.28, 0.33, 0.55))
print('RECOLOUR beige median %.3f (p10-p90 %.3f-%.3f) -> grey median %.3f (%.3f-%.3f), grey rgb %.3f %.3f %.3f' % (
    b50, b10, b90, g50, g10, g90, *g_rgb))
spread = CONTRAST  # of the beige's luminance spread (the grey's own is about half: a little more keeps the folds)
g_lum = lum(*g_rgb)
tint = [c / g_lum for c in g_rgb]
w, h = arms.size
px = list(arms.pixels[:])
cuff_x = CUFF_U * w
FEATHER = FEATHER * w / 256   # texels of the 256 original
for y in range(h):
    for x in range(w):
        t = min(1.0, max(0.0, (cuff_x - x) / FEATHER))  # 1 on the sleeve, 0 on the cuff
        if t <= 0:
            continue
        i = 4 * (y * w + x)
        r, g, b = px[i:i + 3]
        l2 = max(0.0, min(1.0, g50 + (lum(r, g, b) - b50) * spread))
        for c in range(3):
            px[i + c] = (1 - t) * px[i + c] + t * max(0.0, min(1.0, l2 * tint[c]))
out = bpy.data.images.new('arms_grey', w, h, alpha=True)
out.pixels[:] = px
out.filepath_raw = OUT
out.file_format = 'PNG'
out.save()
print('RECOLOUR spread x%.2f, wrote %s (%d x %d)' % (spread, OUT, w, h))
