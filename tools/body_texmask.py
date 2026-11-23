"""Blender (blender -b --factory-startup -P tools/body_texmask.py -- <texture> <out-prefix>): the painted parts of a
texture against its flat background: background colour from the border, mask = colour distance over a threshold,
cleaned (small specks and holes); prints each connected part's bounds and, per texel column, the mask's top edge
(finger tips and valleys); writes <out-prefix>-mask.png (texture 4x, outline cyan) and <out-prefix>-mask.txt (the
mask, one row of 0/1 per texel row, top row first)."""
import bpy, os, sys

args = sys.argv[sys.argv.index('--') + 1:]
TEX, OUT = args[0], args[1]
img = bpy.data.images.load(TEX)
w, h = img.size
px = img.pixels[:]


def rgb(x, y):
    i = (y * w + x) * 4
    return px[i], px[i + 1], px[i + 2]


border = [rgb(x, h - 1) for x in range(w)] + [rgb(x, h - 2) for x in range(w)]
bg = tuple(sorted(c[k] for c in border)[len(border) // 2] for k in range(3))


def dist(c):
    return sum((a - b) ** 2 for a, b in zip(c, bg)) ** 0.5


THRESH = float(args[2]) if len(args) > 2 else 0.10
mask = [[1 if dist(rgb(x, y)) > THRESH else 0 for x in range(w)] for y in range(h)]


def morph(m, keep):
    """keep=1: erode then dilate (open); keep=0: dilate then erode (close)."""
    def step(m, val):
        out = [row[:] for row in m]
        for y in range(h):
            for x in range(w):
                if m[y][x] != val:
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < w and 0 <= yy < h and m[yy][xx] == val:
                            out[y][x] = val
                            break
        return out
    a, b = (0, 1) if keep else (1, 0)
    return step(step(m, a), b)


mask = morph(morph(mask, 1), 0)
# connected parts
label = [[0] * w for _ in range(h)]
parts = []
for y in range(h):
    for x in range(w):
        if mask[y][x] and not label[y][x]:
            n = len(parts) + 1
            stack, box, count = [(x, y)], [x, y, x, y], 0
            label[y][x] = n
            while stack:
                cx, cy = stack.pop()
                count += 1
                box = [min(box[0], cx), min(box[1], cy), max(box[2], cx), max(box[3], cy)]
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = cx + dx, cy + dy
                    if 0 <= xx < w and 0 <= yy < h and mask[yy][xx] and not label[yy][xx]:
                        label[yy][xx] = n
                        stack.append((xx, yy))
            parts.append((count, box))
print('BACKGROUND', tuple(round(c, 3) for c in bg), 'threshold', THRESH)
for n, (count, box) in enumerate(parts):
    if count > 30:
        # image rows run bottom-up in Blender; report top-down texel rows like the -uv pictures
        print('PART %d texels %d x %d-%d rows %d-%d' % (n + 1, count, box[0], box[2], h - 1 - box[3], h - 1 - box[1]))
tops = []
for x in range(w):
    ys = [y for y in range(h) if mask[y][x]]
    tops.append(h - 1 - max(ys) if ys else -1)
print('TOP EDGE (texel row of the highest painted texel, every 4th column):')
print(' '.join('%d:%d' % (x, tops[x]) for x in range(0, w, 4)))
with open(OUT + '-mask.txt', 'w') as f:
    for y in reversed(range(h)):
        f.write(''.join(str(v) for v in mask[y]) + '\n')
k = 4
out = [0.0] * (w * k * h * k * 4)
for y in range(h * k):
    for x in range(w * k):
        sx, sy = x // k, y // k
        c = rgb(sx, sy)
        edge = mask[sy][sx] and any(not (0 <= sx + dx < w and 0 <= sy + dy < h) or not mask[sy + dy][sx + dx]
                                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        o = (y * w * k + x) * 4
        out[o:o + 4] = (0.1, 0.95, 1.0, 1.0) if edge else (c[0] * 0.8, c[1] * 0.8, c[2] * 0.8, 1.0)
im = bpy.data.images.new('mask', w * k, h * k)
im.pixels = out
im.filepath_raw = os.path.abspath(OUT + '-mask.png')
im.file_format = 'PNG'
im.save()
