"""Blender (blender -b --factory-startup -P tools/body_hand.py -- <out.blend> <textures-dir>): the rebel trooper's right
first-person hand and forearm, built to PLAN 51 B's rigging rules.

Frame (metres): wrist joint at the origin, fingers toward +y, back of the hand +z (palm down), thumb toward -x, elbow
at y = -0.27. Pieces: glove (palm tube, four fingers and the thumb as quad tubes with three loops at every joint, a
wrist tube that runs on inside the cuff), cuff (a stiff shell with rolled rims, its tab and brass stud), sleeve (tube
from the elbow into the cuff). Skeleton: forearm, forearm_twist, hand, three bones a finger, thumb metacarpal +
two. Weights: smooth blends over each joint's loops, at most two bones per vertex. Uvs: the glove built flat, the
back projected onto the painted back of the hand (left half of the hands texture), the palm onto the painted palm
(right half), finger by finger; sleeve and cuff by position along and angle around the arm as the stock model has
them. Then the fingers are curled into a relaxed rest pose and that pose becomes the skeleton's rest."""
import bpy, bmesh, math, os, sys
from mathutils import Vector, Matrix

args = sys.argv[sys.argv.index('--') + 1:]
OUT, TEX = args[0], args[1]
UNWRAP = 'projected' not in args[2:]  # 'projected': the older per-part projection instead of the pattern unwrap


def smooth(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


# ---- glove dimensions --------------------------------------------------------------------------------------------
# Proportions after the stock first-person hand (tools/body_measure.py) scaled to a real gloved hand: 195 mm from
# the wrist to the middle finger's tip (the stock is 274 mm, drawn large for a flat screen), knuckles 96 / 98 / 93 /
# 91 mm from the wrist, the middle finger as long as the palm; index, ring and pinky at the usual 0.93 / 0.93 /
# 0.79 of the middle (the stock's grip pose blurs those). Joints at the stock's edge loops (BACK_MARKS below).
# name, mcp (x, y), length from the knuckle to the tip, width, splay (deg, + toward the pinky), back uv (centre u, half width u,
# v at the knuckle, v at the tip), palm uv (same). The hands texture: back of the hand = left half (pinky at u 0,
# index next to the thumb), palm = right half (index next to the thumb, pinky at u 1), thumb between them; checked
# by tinting the halves on the stock fist (back and the curled fingers' outsides left, the palm right)
FINGERS = [
    ('index', (-0.034, 0.096), (0.090,), 0.0210, -5, (0.393, 0.048, 0.51, 0.976), (0.688, 0.040, 0.53, 0.951)),
    ('middle', (-0.011, 0.098), (0.097,), 0.0220, -1, (0.271, 0.052, 0.53, 0.995), (0.776, 0.044, 0.55, 0.995)),
    ('ring', (0.0115, 0.093), (0.090,), 0.0210, 3, (0.161, 0.045, 0.52, 0.932), (0.874, 0.044, 0.54, 0.907)),
    ('pinky', (0.033, 0.091), (0.077,), 0.0190, 8, (0.051, 0.040, 0.51, 0.834), (0.962, 0.035, 0.53, 0.829)),
]
# painted joint markers (texel rows, y down), read off the texture at 5x (idren2023 2026-10-21: the creases belong on
# the joints, the palm's main crease (y 148) on the palm below the fingers, the subtle finger-base crease where the
# finger starts). Palm half: finger base crease (where our finger leaves the knuckle cap), PIP crease, DIP crease,
# top of the painted fingertip. Back half: the brighter knuckle bands, MCP, PIP, DIP. Bone lengths put the PIP and
# DIP at the palm creases' proportions of the finger from its base (total length kept).
# (idren2023 2026-10-21: follow the stock's texture layout) the stock's palm-side edge loops: where the finger leaves
# the palm, PIP, DIP, and the painted fingertip's top; 3-8 texels above the painted creases read earlier
PALM_MARKS = {'index': (123, 80, 47, 18), 'middle': (113, 74, 40, 8), 'ring': (116, 91, 57, 24), 'pinky': (118, 94, 71, 45)}
# back half: the stock model's edge loops across each finger strip (its joints, tools/body_islands.py' stock blend;
# idren2023: the knuckles are marked by those near-horizontal edges), MCP / PIP / DIP
BACK_MARKS = {'index': (125, 80, 46), 'middle': (126, 74, 41), 'ring': (125, 91, 56), 'pinky': (126, 95, 73)}
CAP_S = -0.010  # the knuckle cap (where a finger leaves the palm), from the MCP pivot
BACK_APEX_Y = {'index': 6, 'middle': 1, 'ring': 17, 'pinky': 42}
for i, (name, mcp, lens, w, splay, uvb, uvp) in enumerate(FINGERS):
    # phalanges in the proportions of the stock's joint loops along its back strip, knuckle row to the apex
    ym, yp, yd = BACK_MARKS[name]
    total, span = lens[0], ym - BACK_APEX_Y[name]
    l1, l2 = total * (ym - yp) / span, total * (yp - yd) / span
    FINGERS[i] = (name, mcp, (l1, l2, total - l1 - l2), w, splay, uvb, uvp)
THUMB_CMC = Vector((-0.022, 0.022, -0.010))
THUMB_MCP = Vector((-0.072, 0.058, -0.018))
THUMB_IP = THUMB_MCP + Vector((-0.45, 0.89, -0.05)).normalized() * 0.034
THUMB_TIP = THUMB_IP + Vector((-0.30, 0.95, -0.05)).normalized() * 0.030
THUMB_W, THUMB_T = 0.0220, 0.0195
THUMB_UP = Vector((-0.55, 0.0, 0.83))
THUMB_UV_BACK, THUMB_UV_PALM, THUMB_UV_V = (0.520, 0.040), (0.600, 0.040), (0.375, 0.630)

PALM_STATIONS = [0.0, 0.024, 0.048, 0.070]          # rings from the wrist; the knuckle cap follows
PALM_WIDTH = [0.72, 0.92, 1.00, 1.01]                # scale of the knuckle row's x per station
PALM_THICK = [0.032, 0.036, 0.036, 0.032]
CAP_THICK = 0.019
WRIST_TUBE = [(-0.012, 0.97), (-0.024, 0.95), (-0.042, 0.93)]  # y, scale of the wrist ring
UV_BACK_PALM = (0.23, -0.225, 0.03, 0.51)            # centre u, half width u (+x = pinky), v at wrist, knuckles
UV_PALM_PALM = (0.82, 0.175, 0.03, 0.53)

# ---- sleeve and cuff -----------------------------------------------------------------------------------------------
ELBOW_Y = -0.27
SLEEVE = [(-0.270, 0.046), (-0.245, 0.0455), (-0.215, 0.0445), (-0.185, 0.043), (-0.155, 0.0415), (-0.130, 0.040),
          (-0.112, 0.0388), (-0.100, 0.0375), (-0.088, 0.0365)]
CUFF_Y = (-0.100, 0.002)                              # the gauntlet: its bottom edge to the glove's wrist ring
SEGMENTS = 26
ARM_UV_ALONG = [(-0.328, 0.0), (-0.247, 0.20), (-0.163, 0.40), (-0.081, 0.662)]  # y -> u (stock, sleeve)


def arm_v(phi):
    """v of the arm texture at angle phi (deg, from the thumb side toward the back of the hand), as the stock model."""
    return 0.03 + 0.96 * (((97 - phi) / 360) % 1.0)


def sleeve_u(y):
    pts = ARM_UV_ALONG
    for (y0, u0), (y1, u1) in zip(pts, pts[1:]):
        if y <= y1:
            return u0 + (u1 - u0) * (y - y0) / (y1 - y0)
    return pts[-1][1] + (y - pts[-1][0]) * 3.2


def cuff_u(y):
    return 0.662 + (y - CUFF_Y[0]) / (CUFF_Y[1] - CUFF_Y[0]) * (0.998 - 0.662)


def ellipse(r, phi_deg, sx=1.12, sz=0.90):
    """Point on the arm's cross-section at angle phi from the thumb side (-x) toward the back (+z)."""
    p = math.radians(phi_deg)
    return Vector((-math.cos(p) * r * sx, 0.0, math.sin(p) * r * sz))


bm = bmesh.new()
uvl = bm.loops.layers.uv.new('UV')
tag = {}  # vertex -> (part, data)


def vert(co, part, data=None):
    v = bm.verts.new(co)
    tag[v] = (part, data)
    return v


def quad(a, b, c, d, mat=0):
    f = bm.faces.new((a, b, c, d))
    f.material_index = mat
    return f


# ---- palm: rings of 26 (top row 12 columns, right middle, bottom row back, left middle) ------------------------
col_x = []
for name, (mx, my), lens, w, splay, uvb, uvp in FINGERS:
    col_x += [mx - 0.42 * w, mx, mx + 0.42 * w]
cap_y = []
for name, (mx, my), lens, w, splay, uvb, uvp in FINGERS:
    cap_y += [my - 0.010] * 3
half = (col_x[-1] - col_x[0]) / 2 + 0.003
mid_x = (col_x[-1] + col_x[0]) / 2


def palm_ring(y, sw, th, part='palm'):
    """26 vertices; the palm side padded (cushion, and the pinky side's pad toward the wrist)."""
    ring = []
    for c in range(12):
        x = mid_x + (col_x[c] - mid_x) * sw
        xn = (x - mid_x) / (half * sw)
        ring.append(vert((x, y, th / 2 + 0.004 * (1 - xn * xn)), part, y))
    ring.append(vert((mid_x + (half + 0.005) * sw, y, -0.001), part, y))
    pad = smooth(0.083, 0.053, y)
    for c in reversed(range(12)):
        x = mid_x + (col_x[c] - mid_x) * sw
        xn = (x - mid_x) / (half * sw)
        thenar = smooth(-0.2, -0.85, xn) * smooth(0.0, 0.026, y) * smooth(0.080, 0.053, y)
        z = -th / 2 - 0.005 * (1 - xn * xn) - 0.004 * pad * smooth(0.1, 0.8, xn) - 0.006 * thenar
        ring.append(vert((x, y, z), part, y))
    ring.append(vert((mid_x - (half + 0.004) * sw, y, 0.0), part, y))
    return ring


T = lambda c: c
B = lambda c: 24 - c
R, L = 12, 25
rings = [palm_ring(y, sw, th) for y, sw, th in zip(PALM_STATIONS, PALM_WIDTH, PALM_THICK)]
# the cap: top and bottom rows at each column's knuckle, the middle row inside
cap = [None] * 26
for c in range(12):
    cap[T(c)] = vert((col_x[c], cap_y[c], CAP_THICK / 2 + 0.001), 'palm', cap_y[c])
    cap[B(c)] = vert((col_x[c], cap_y[c], -CAP_THICK / 2), 'palm', cap_y[c])
cap[R] = vert((mid_x + half + 0.003, cap_y[11] - 0.002, -0.001), 'palm', cap_y[11])
cap[L] = vert((mid_x - half - 0.002, cap_y[0] - 0.002, 0.0), 'palm', cap_y[0])
mid_row = [cap[L]] + [vert((col_x[c], cap_y[c] + 0.004, 0.0), 'palm', cap_y[c]) for c in range(1, 11)] + [cap[R]]
rings.append(cap)

ring_of = {}  # palm / wrist vertex -> (its ring, index in it)
for ring in rings:
    for i, v in enumerate(ring):
        ring_of[v] = (ring, i)
thumb_cells = {(1, 24), (1, 25), (2, 24), (2, 25)}   # (ring interval, segment start) left out for the thumb
for i in range(len(rings) - 1):
    a, b = rings[i], rings[i + 1]
    for s in range(26):
        if (i, s) in thumb_cells:
            continue
        quad(a[s], a[(s + 1) % 26], b[(s + 1) % 26], b[s])
# cap faces between the fingers (the webbing); finger blocks stay open
for c in range(11):
    if c in (2, 5, 8):
        quad(cap[T(c)], mid_row[c], mid_row[c + 1], cap[T(c + 1)])
        quad(mid_row[c], cap[B(c)], cap[B(c + 1)], mid_row[c + 1])

wrist_rings = []  # (the glove runs on into the gauntlet, built with the sleeve below)


# ---- fingers ---------------------------------------------------------------------------------------------------
def tube(border, stations, frame, part, data_of):
    """Rings of 8 (top-left, top, top-right, right, bottom-right, bottom, bottom-left, left) along stations
    (s, width, thickness, dorsal bulge) in frame (origin, axis, lateral, up), bridged from border, closed by a cap.
    Returns the rings (border first) and the tip."""
    origin, axis, lat, up = frame
    prev = border
    last = None
    all_rings = [border]
    for s, w, th, bulge in stations:
        ring = []
        for k, ang in enumerate((135, 90, 45, 0, -45, -90, -135, 180)):
            a = math.radians(ang)
            z = math.sin(a) * th / 2 + (bulge if math.sin(a) > 0.5 else 0.0)
            ring.append(vert(origin + axis * s + lat * (math.cos(a) * w / 2) + up * z, part, data_of(s)))
        for k in range(8):
            quad(prev[k], prev[(k + 1) % 8], ring[(k + 1) % 8], ring[k])
        prev = ring
        all_rings.append(ring)
        last = (s, w, th)
    s, w, th = last
    tip = vert(origin + axis * (s + 0.0065) + up * 0.0005, part, data_of(s + 0.0065))
    r = prev
    quad(r[0], r[1], tip, r[7])
    quad(r[1], r[2], r[3], tip)
    quad(tip, r[3], r[4], r[5])
    quad(r[7], tip, r[5], r[6])
    return all_rings, tip


finger_frames = []
finger_tubes = []
for k, (name, (mx, my), (l1, l2, l3), w, splay, uvb, uvp) in enumerate(FINGERS):
    a = math.radians(splay)
    axis = Vector((math.sin(a), math.cos(a), 0.0))
    lat = Vector((math.cos(a), -math.sin(a), 0.0))
    up = Vector((0, 0, 1))
    origin = Vector((mx, my, 0.0))
    finger_frames.append((origin, axis, lat, up))
    c0 = 3 * k
    border = [cap[T(c0)], cap[T(c0 + 1)], cap[T(c0 + 2)], mid_row[c0 + 2], cap[B(c0 + 2)], cap[B(c0 + 1)], cap[B(c0)], mid_row[c0]]
    if c0 == 0:
        border[7] = cap[L]
    if c0 + 2 == 11:
        border[3] = cap[R]
    th = 0.86 * w
    L1, L2, L3 = l1, l1 + l2, l1 + l2 + l3
    st = [(0.000, w * 1.02, th * 1.05, 0.0012), (0.010, w, th, 0.0004), (L1 * 0.55, w * 0.97, th * 0.95, 0.0),
          (L1 - 0.006, w * 0.96, th * 0.93, 0.0), (L1, w * 0.98, th * 0.95, 0.0010), (L1 + 0.006, w * 0.94, th * 0.9, 0.0),
          (L2 - 0.005, w * 0.92, th * 0.88, 0.0), (L2, w * 0.93, th * 0.89, 0.0007), (L2 + 0.005, w * 0.9, th * 0.86, 0.0),
          (L3 - 0.0075, w * 0.86, th * 0.80, 0.0)]
    finger_tubes.append(tube(border, st, (origin, axis, lat, up), 'finger', lambda s, k=k: (k, s)))

# ---- thumb: from the palm's side between stations 1 and 3 -----------------------------------------------------
tb = [rings[1][T(0)], rings[2][T(0)], rings[3][T(0)], rings[3][L], rings[3][B(0)], rings[2][B(0)], rings[1][B(0)], rings[1][L]]
bm.verts.remove(rings[2][L])
t_axis = (THUMB_TIP - THUMB_MCP).normalized()
seg = [THUMB_CMC, THUMB_MCP, THUMB_IP, THUMB_TIP]


def thumb_point(s):
    """Point and direction along the thumb's bent axis at arc length s from the metacarpal joint."""
    d = 0.0
    for a, b in zip(seg, seg[1:]):
        l = (b - a).length
        if s <= d + l:
            return a + (b - a) * ((s - d) / l), (b - a).normalized()
        d += l
    return seg[-1], (seg[-1] - seg[-2]).normalized()


L_META = (THUMB_MCP - THUMB_CMC).length
L_PROX = (THUMB_IP - THUMB_MCP).length
L_DIST = (THUMB_TIP - THUMB_IP).length
th_st = [(L_META - 0.009, 1.06, 1.06, 0.0), (L_META, 1.0, 1.0, 0.0010), (L_META + 0.008, 0.97, 0.96, 0.0),
         (L_META + L_PROX - 0.006, 0.95, 0.94, 0.0), (L_META + L_PROX, 0.96, 0.95, 0.0008),
         (L_META + L_PROX + 0.006, 0.92, 0.9, 0.0), (L_META + L_PROX + L_DIST - 0.0075, 0.86, 0.82, 0.0)]
# the thenar: two full rings between the palm's side opening and the first metacarpal ring, set toward the palm
opening = sum((v.co for v in tb), Vector()) / 8
p_first, _ = thumb_point(th_st[0][0])
meta_dir = (THUMB_MCP - THUMB_CMC).normalized()
specs = []
# each ring faces out of the side opening at first and turns toward the thumb's metacarpal (a ring tilted along
# the thumb from the start tucks its palm side under the opening's edge: a fold, idren2023 2026-10-21)
out_n = Vector((-1.0, 0.0, 0.0))
for f, turn, ws, ts, drop in ((0.28, 0.22, 1.32, 1.42, 0.0025), (0.68, 0.55, 1.14, 1.20, 0.0015)):
    c = opening + (p_first - opening) * f + Vector((0, 0, -drop))
    axis = (out_n * (1 - turn) + meta_dir * turn).normalized()
    specs.append((c, axis, ws, ts, 0.0, (c - THUMB_CMC).dot(meta_dir)))
for s, ws, ts, bulge in th_st:
    p, d = thumb_point(s)
    specs.append((p, d, ws, ts, bulge, s))
prev = None
first = None
thumb_rings = []
for p, d, ws, ts, bulge, s in specs:
    up = (THUMB_UP - d * THUMB_UP.dot(d)).normalized()
    lat = d.cross(up)
    ring = []
    for ang in (135, 90, 45, 0, -45, -90, -135, 180):
        a = math.radians(ang)
        z = math.sin(a) * THUMB_T * ts / 2 + (bulge if math.sin(a) > 0.5 else 0.0)
        ring.append(vert(p + lat * (math.cos(a) * THUMB_W * ws / 2) + up * z, 'thumb', s))
    if prev:
        for k in range(8):
            quad(prev[k], prev[(k + 1) % 8], ring[(k + 1) % 8], ring[k])
    else:
        first = ring
    thumb_rings.append(ring)
    prev = ring
p, d = thumb_point(L_META + L_PROX + L_DIST)
tip = vert(p - d * 0.001, 'thumb', L_META + L_PROX + L_DIST)
thumb_tip = tip
r = prev
quad(r[0], r[1], tip, r[7])
quad(r[1], r[2], r[3], tip)
quad(tip, r[3], r[4], r[5])
quad(r[7], tip, r[5], r[6])
# bridge the palm's side opening to the thumb's first ring: best cyclic offset and direction
best = None
for flip in (False, True):
    rr = list(reversed(first)) if flip else first
    for off in range(8):
        cost = sum((tb[k].co - rr[(k + off) % 8].co).length for k in range(8))
        if best is None or cost < best[0]:
            best = (cost, [rr[(k + off) % 8] for k in range(8)])
fr = best[1]
for k in range(8):
    quad(tb[k], tb[(k + 1) % 8], fr[(k + 1) % 8], fr[k])

# ---- the glove's seams and uv pins, after the stock pattern (tools/body_islands.py, re/body/stock-hand-islands.png):
# one piece, back of the hand (left half of the texture) and palm (right half) joined through the column under the
# thumb; cut along the pinky side, both sides of each finger and the webs between them, the slit between thumb and
# index, and one line along the thumb (the side facing the index). Pins in texels (x right, y down, 256 square).
def seam(a, b):
    e = bm.edges.get((a, b))
    if e:
        e.seam = True


def seam_chain(vs):
    for a, b in zip(vs, vs[1:]):
        seam(a, b)


palm_rings = list(rings)  # stations and the knuckle cap
seam_chain([r[R] for r in reversed(wrist_rings)] + [r[R] for r in palm_rings])
for tube_rings, ftip in finger_tubes:
    seam_chain([r[3] for r in tube_rings] + [ftip])
    seam_chain([r[7] for r in tube_rings] + [ftip])
for c in (2, 5, 8):
    seam(mid_row[c], mid_row[c + 1])
seam(rings[3][L], cap[L])
# the thumb's line: the ring vertex facing the index finger's base, the same index on every ring
to_index = (Vector((FINGERS[0][1][0], FINGERS[0][1][1], 0.0)) - THUMB_MCP).normalized()
centre1 = sum((v.co for v in thumb_rings[1]), Vector()) / 8
ti = max(range(8), key=lambda k: (thumb_rings[1][k].co - centre1).dot(to_index))
seam_chain([r[ti] for r in thumb_rings] + [thumb_tip])
j = fr.index(thumb_rings[0][ti])
seam(fr[j], tb[j])
step = 1 if (3 - j) % 8 <= 4 else -1
while j != 3:
    seam(tb[j], tb[(j + step) % 8])
    j = (j + step) % 8

PINS = []  # (vertex, side: 'back' / 'palm' / None for every corner, texel x, texel y)
BACK_APEX = [(100, 6), (70, 1), (41, 17), (15, 42)]
PALM_APEX = [(178, 12), (198, 0), (224, 22), (246, 43)]
BACK_COLS = [112, 101, 88, 84, 70, 56, 52, 41, 31, 25, 13, 2]
PALM_COLS = [168, 176, 186, 189, 199, 209, 213, 223, 234, 238, 246, 254]
for k, (tube_rings, ftip) in enumerate(finger_tubes):
    PINS += [(ftip, 'back') + BACK_APEX[k], (ftip, 'palm') + PALM_APEX[k]]
# the palm piece's rows: back of the hand from the wrist (y 247) to the knuckle row; palm from the wrist (y 250) to
# the palm line over the knuckles (about 0.78 mm a texel); the knuckle cap 10 mm short of the knuckles
CAP_BACK_Y = [BACK_MARKS[f[0]][0] for f in FINGERS]   # the back's knuckle band on the cap, where the finger leaves the hand
CAP_PALM_Y = [PALM_MARKS[f[0]][0] for f in FINGERS]       # the cap on the finger base crease
for c in range(12):
    PINS += [(cap[T(c)], None, BACK_COLS[c], CAP_BACK_Y[c // 3]), (cap[B(c)], None, PALM_COLS[c], CAP_PALM_Y[c // 3])]
# palm rows: the main crease (y ~148) about 16 mm below the fingers, between the last two rows
ROWS = [(rings[0], 247, 250), (rings[1], 215, 214), (rings[2], 185, 174), (rings[3], 158, 140)]


def outline_x(pts, y):
    if y >= pts[0][1]:
        return pts[0][0]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if min(y0, y1) <= y <= max(y0, y1):
            return x0 + (x1 - x0) * (y - y0) / ((y1 - y0) or 1)
    return pts[-1][0]


BACK_EDGE = [(51, 247), (46, 236), (41, 216), (14, 163), (1, 130), (1, 120)]
PALM_EDGE = [(218, 244), (225, 215), (254, 146), (255, 112)]
for ring, (yb, yp) in zip(palm_rings, [(247, 250), (215, 214), (185, 174), (158, 140), (CAP_BACK_Y[3], CAP_PALM_Y[3])]):
    # at the knuckle row the palm's pinky strip reaches the texture's edge (wider than the outline below it)
    xp = 255.5 if ring is cap else outline_x(PALM_EDGE, yp)
    PINS += [(ring[R], 'back', outline_x(BACK_EDGE, yb), yb), (ring[R], 'palm', xp, yp)]
# each row's 12 columns spread from the thumb side to the pinky edge (back: x 120 -> back outline; palm: x 164 ->
# palm outline), so the rows run straight across as painted and the palm line stays over the knuckles
# the glove's wrist section (inside the cuff) squeezed onto the bottom edge
ROWS += [(r, 252 + 1.5 * i, 252 + 1.5 * i) for i, r in enumerate(wrist_rings)]
for ring, yb, yp in ROWS:
    for c in range(12):
        if c == 0:
            # the painted seam from the index finger straight down the back to the wrist (texel column 116); at the
            # rows beside the thumb-index slit (also at x 116) a little inside it, so the side wall there keeps width
            PINS += [(ring[T(c)], None, 112 if ring is rings[3] else 116, yb)]
            continue
        t = (c + 0.5) / 12
        PINS += [(ring[T(c)], None, 120 + (outline_x(BACK_EDGE, yb) + 3 - 120) * t, yb),
                 (ring[B(c)], None, 164 + (outline_x(PALM_EDGE, yp) - 3 - 164) * t, yp)]
PINS += [(rings[0][L], None, 136, 252), (rings[1][L], None, 143, 205), (thumb_tip, None, 143, 97),
         (rings[3][L], 'back', 116, 146), (rings[3][L], 'palm', 166, 146), (cap[L], 'back', 116, 126),
         (cap[L], 'palm', 166, 125)]
# finger side seams along the stock strips' edges, from the knuckle row (y 124) to where the pointed tip starts
BACK_STRIPS = [(87, 115, 20), (55, 85, 10), (30, 53, 28), (1, 26, 51)]    # (left x, right x, tip shoulder y)
PALM_STRIPS = [(166, 187, 20), (188, 210, 10), (212, 235, 30), (237, 255, 47)]


def along(points, s):
    for (s0, y0), (s1, y1) in zip(points, points[1:]):
        if s <= s1:
            return y0 + (y1 - y0) * (s - s0) / (s1 - s0)
    return points[-1][1]


for k, (tube_rings, ftip) in enumerate(finger_tubes):
    name, mcp, (l1, l2, l3), w, splay, uvb, uvp = FINGERS[k]
    origin, axis, lat, up = finger_frames[k]
    (bl, br, bs), (pl, pr, ps) = BACK_STRIPS[k], PALM_STRIPS[k]
    yb_, yp_, yd_, yt_ = PALM_MARKS[name]
    bm_, bp_, bd_ = BACK_MARKS[name]
    palm_y = [(CAP_S, yb_), (l1, yp_), (l1 + l2, yd_), (l1 + l2 + l3 - 0.001, PALM_APEX[k][1])]
    back_y = [(CAP_S, bm_), (l1, bp_), (l1 + l2, bd_), (l1 + l2 + l3 - 0.001, BACK_APEX[k][1])]
    for ring in tube_rings[1:]:
        sv = (ring[1].co - origin).dot(axis)
        yb, yq = along(back_y, sv), along(palm_y, sv)
        # the strips narrow into their pointed tips above the shoulder; below it they keep their width
        # ring vertex 3 is the pinky side: left on the back half, right on the palm half; 7 the thumb side
        PINS += [(ring[3], 'back', bl, yb), (ring[3], 'palm', pr, yq), (ring[7], 'back', br, yb), (ring[7], 'palm', pl, yq),
                 (ring[1], None, (bl + br) / 2, yb), (ring[5], None, (pl + pr) / 2, yq)]
# the thumb's seam: its back side on the strip's left edge (x 123), its palm side on the right (x 163), from the
# first metacarpal ring at the strip's base (y 150) to where the tip starts (y 107); the two pad rings' seam points
# just under the base (the stock seam runs down the thumb to y 150, then up the slit to the knuckle row; the pad's
# other vertices spread into the column below)
n = len(thumb_rings) - 3
for i, ring in enumerate(thumb_rings):
    if i < 2:
        y, xl, xr = (156, 123, 165) if i == 0 else (153, 123, 164)
    else:
        y, xl, xr = 150 + (107 - 150) * (i - 2) / n, 123, 163
    PINS += [(ring[ti], 'thumb back', xl, y), (ring[ti], 'thumb palm', xr, y)]
# targets from the comparison with the stock hand (tools/body_match.py): the row / column / knuckle-row / finger
# centre pins off the seams move to the stock's placement there (the seams keep theirs: they are the stock's cut;
# the unwrap fills in between, so the measuring noise does not fold faces)
TARGETS = {}
if 'targets' in args[2:]:
    import json
    TARGETS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(OUT)), 'uv-targets.json')))
for v, side, x, y in PINS:
    for l in v.link_loops:
        if side in ('back', 'palm'):
            above = l.face.calc_center_median().z > v.co.z
            if (side == 'back') != above:
                continue
        elif side is not None:
            p_, d_ = thumb_point(max(0.0, min((v.co - THUMB_CMC).dot(t_axis), L_META + L_PROX + L_DIST)))
            up_ = (THUMB_UP - d_ * THUMB_UP.dot(d_)).normalized()
            if ((l.face.calc_center_median() - v.co).dot(up_) > 0) != (side == 'thumb back'):
                continue
        l[uvl].uv = (x / 256, 1 - y / 256)
        l[uvl].pin_uv = True

GLOVE_FACES = len(bm.faces)

# ---- gauntlet: the glove's leather runs on from its wrist ring and flares into the cuff (one surface, as the stock
# glove; idren2023 2026-10-21), ending in a rolled edge with the shirt's sleeve tucked under it
def cuff_r(y):
    """Outer radius: 42 mm along the gauntlet, 43.5 at its bottom edge."""
    return 0.0420 + 0.0015 * smooth(-0.070, -0.100, y)


def phi_of(co):
    """Angle around the arm (deg) from the thumb side toward the back of the hand, as `ellipse` measures it."""
    return math.degrees(math.atan2(co.z / 0.90, -co.x / 1.12)) % 360


wrist_phis = [phi_of(v.co) for v in rings[0]]
# (y, radius offset, share of the ellipse against the glove's wrist shape): flare, band, rolled bottom edge, lip
profile = [(-0.007, 0.0, 0.55), (-0.016, 0.0, 0.9), (-0.028, 0.0, 1.0), (-0.046, 0.0, 1.0), (-0.066, 0.0, 1.0),
           (-0.086, 0.0, 1.0), (-0.0900, 0.0012, 1.0), (-0.0945, 0.0034, 1.0), (-0.0990, 0.0030, 1.0),
           (-0.1012, 0.0005, 1.0), (-0.1005, -0.0025, 1.0), (-0.0960, -0.0050, 1.0)]
cuff_rings = [rings[0]]
for y, dr, share in profile:
    ring = []
    for v, phi in zip(rings[0], wrist_phis):
        e = ellipse(cuff_r(y) + dr, phi)
        g = Vector((v.co.x, 0.0, v.co.z))
        ring.append(vert(g.lerp(e, share) + Vector((0, y, 0)), 'cuff', (y, phi)))
    cuff_rings.append(ring)
for a, b in zip(cuff_rings, cuff_rings[1:]):
    for k in range(26):
        quad(a[(k + 1) % 26], a[k], b[k], b[(k + 1) % 26], 1)

# tab with the stud (stock texture: tab u 0.782-0.908, v 0.232-0.418; stud u 0.848, v 0.328)
def phi_of_v(v):
    return 97 - 360 * ((v - 0.03) / 0.96)


def y_of_cuff_u(u):
    return CUFF_Y[0] + (u - 0.662) / (0.998 - 0.662) * (CUFF_Y[1] - CUFF_Y[0])


def on_cuff(y, phi, lift):
    return ellipse(cuff_r(y) + lift, phi) + Vector((0, y, 0))


tab_y = (y_of_cuff_u(0.782), y_of_cuff_u(0.908))
tab_phi = (phi_of_v(0.418), phi_of_v(0.232))
tab_grid = []
NY, NP = 6, 6
for i in range(NY + 1):
    row = []
    for j in range(NP + 1):
        y = tab_y[0] + (tab_y[1] - tab_y[0]) * i / NY
        phi = tab_phi[0] + (tab_phi[1] - tab_phi[0]) * j / NP
        edge = min(i, NY - i, j, NP - j)
        lift = 0.0006 if edge == 0 else 0.0022
        row.append(vert(on_cuff(y, phi, lift), 'tab', (y, phi)))
    tab_grid.append(row)
for i in range(NY):
    for j in range(NP):
        quad(tab_grid[i][j], tab_grid[i][j + 1], tab_grid[i + 1][j + 1], tab_grid[i + 1][j], 1)
stud_y, stud_phi, stud_r = y_of_cuff_u(0.848), phi_of_v(0.328), 0.0058
stud_rings = []
for lvl, (rf, lift) in enumerate(((1.0, 0.0018), (1.0, 0.0034), (0.78, 0.0050), (0.42, 0.0058))):
    ring = []
    for k in range(10):
        a = 2 * math.pi * k / 10
        dy = math.cos(a) * stud_r * rf
        dphi = math.degrees(math.sin(a) * stud_r * rf / cuff_r(stud_y))
        ring.append(vert(on_cuff(stud_y + dy, stud_phi + dphi, lift), 'stud', (stud_y + dy, stud_phi + dphi)))
    stud_rings.append(ring)
for a, b in zip(stud_rings, stud_rings[1:]):
    for k in range(10):
        quad(a[k], a[(k + 1) % 10], b[(k + 1) % 10], b[k], 1)
top = vert(on_cuff(stud_y, stud_phi, 0.0061), 'stud', (stud_y, stud_phi))
for k in range(10):
    f = bm.faces.new((stud_rings[-1][k], stud_rings[-1][(k + 1) % 10], top))
    f.material_index = 1

# ---- sleeve ------------------------------------------------------------------------------------------------------
sl = []
for y, r in SLEEVE:
    sl.append([vert(ellipse(r, 360 * k / SEGMENTS) + Vector((0, y, 0)), 'sleeve', (y, 360 * k / SEGMENTS)) for k in range(SEGMENTS)])
for a, b in zip(sl, sl[1:]):
    for k in range(SEGMENTS):
        quad(a[k], b[k], b[(k + 1) % SEGMENTS], a[(k + 1) % SEGMENTS], 1)

bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])


# ---- uvs ---------------------------------------------------------------------------------------------------------
def glove_uv(v, back, face_part):
    part, data = tag[v]
    co = v.co
    if face_part[0] == 'finger':
        k = face_part[1]
        origin, axis, lat, up = finger_frames[k]
        name, (mx, my), (l1, l2, l3), w, splay, uvb, uvp = FINGERS[k]
        cu, hu, v0, v1 = uvb if back else uvp
        s = (co - origin).dot(axis)
        x = (co - origin).dot(lat) / (w / 2)
        u = cu + x * hu * (-1 if back else 1)
        return u, v0 + max(s, -0.012) / (l1 + l2 + l3) * (v1 - v0)
    if face_part[0] == 'thumb':
        s = (co - THUMB_CMC).dot(t_axis)
        p, d = thumb_point(max(0.0, min(s, L_META + L_PROX + L_DIST)))
        upv = (THUMB_UP - d * THUMB_UP.dot(d)).normalized()
        x = (co - p).dot(d.cross(upv)) / (THUMB_W / 2)
        cu, hu = THUMB_UV_BACK if back else THUMB_UV_PALM
        t = (s - (L_META - 0.012)) / (L_PROX + L_DIST + 0.012)
        return cu + x * hu * (-1 if back else 1), max(0.22, THUMB_UV_V[0] + t * (THUMB_UV_V[1] - THUMB_UV_V[0]))
    return None


# palm and wrist: unwrapped around the hand as the texture paints it: back (pinky edge -> thumb edge), the thumb
# side's column under the painted thumb, palm (thumb edge -> pinky edge); narrower toward the wrist
PALM_U = {'knuckles': ((0.00, 0.47), (0.47, 0.64), (0.64, 1.00)), 'wrist': ((0.17, 0.48), (0.48, 0.58), (0.58, 0.87))}
SECTIONS = {'back': [12] + list(range(11, -1, -1)), 'side': [0, 25, 24], 'palm': list(range(24, 11, -1))}


def palm_section(face):
    idx = {ring_of[v][1] for v in face.verts if v in ring_of}
    if idx & set(range(0, 12)) and not idx & set(range(13, 25)) and 25 not in idx:
        return 'back' if not (idx <= {0, 25}) else 'side'
    if idx & set(range(13, 25)) and not idx & set(range(0, 12)) and 25 not in idx:
        return 'palm'
    return 'side'


def palm_uv(v, section):
    ring, i = ring_of[v]
    order = SECTIONS[section]
    if i not in order:
        i = order[0] if (section == 'back' and i == 25) else order[-1]
    pts = [ring[k].co for k in order]
    lens = [0.0]
    for a, b in zip(pts, pts[1:]):
        lens.append(lens[-1] + (b - a).length)
    frac = lens[order.index(i)] / lens[-1]
    y = v.co.y
    t = smooth(0.0, 0.070, y)
    k = ('back', 'side', 'palm').index(section)
    (a0, a1), (b0, b1) = PALM_U['wrist'][k], PALM_U['knuckles'][k]
    u0, u1 = a0 + (b0 - a0) * t, a1 + (b1 - a1) * t
    vv0, vv1 = {'back': (0.03, 0.51), 'side': (0.03, 0.40), 'palm': (0.03, 0.53)}[section]
    return u0 + (u1 - u0) * frac, vv0 + min(max(y, 0.0), 0.075) / 0.075 * (vv1 - vv0)


for f in bm.faces:
    parts = [tag[v][0] for v in f.verts]
    if f.material_index == 0 and UNWRAP:
        continue  # the glove: unwrapped on the stock pattern once the mesh exists (seams and pins above)
    if f.material_index == 0:
        fp = next(((tag[v][0], tag[v][1][0]) for v in f.verts if tag[v][0] == 'finger'), None) or \
             (('thumb', 0) if 'thumb' in parts else ('palm', 0))
        if fp[0] == 'thumb':
            p, d = thumb_point(min(max((f.calc_center_median() - THUMB_CMC).dot(t_axis), 0), 0.1))
            back = f.normal.dot((THUMB_UP - d * THUMB_UP.dot(d)).normalized()) >= 0
        else:
            back = f.normal.z >= 0
        if fp[0] == 'palm' and all(v in ring_of for v in f.verts):
            sec = palm_section(f)
            uvs = [list(palm_uv(l.vert, sec)) for l in f.loops]
            us = [q[0] for q in uvs]
            if max(us) - min(us) > 0.5:  # the pinky side's wall across the texture's wrap
                for q in uvs:
                    if q[0] < 0.5:
                        q[0] += 1.0
            for l, q in zip(f.loops, uvs):
                l[uvl].uv = q
            continue
        for l in f.loops:
            uv = glove_uv(l.vert, back, fp)
            if uv is None:  # webbing between the fingers: by x as before
                cu, hu, v0, v1 = UV_BACK_PALM if back else UV_PALM_PALM
                xn = max(-1.05, min(1.05, (l.vert.co.x - mid_x) / half))
                uv = (cu + xn * hu, v0 + min(max(l.vert.co.y, 0.0), 0.075) / 0.075 * (v1 - v0))
            l[uvl].uv = uv
    else:
        uvs = []
        for l in f.loops:
            part, data = tag[l.vert]
            y, phi = data if isinstance(data, tuple) else (l.vert.co.y, phi_of(l.vert.co))
            if part == 'sleeve':
                uvs.append([sleeve_u(y), arm_v(phi)])
            elif part == 'cuff' or part == 'palm':
                # the gauntlet; its top kept inside the painted rim (the texture's last columns are pale)
                uvs.append([min(cuff_u(max(CUFF_Y[0], min(CUFF_Y[1], y))), 0.985), arm_v(phi)])
            else:
                uvs.append([cuff_u(y), arm_v(phi)])
        vs = [q[1] for q in uvs]
        if max(vs) - min(vs) > 0.5:  # across the seam: keep the face on one side
            for q in uvs:
                if q[1] < 0.5:
                    q[1] += 0.96
        for l, q in zip(f.loops, uvs):
            l[uvl].uv = q

# ---- skeleton ------------------------------------------------------------------------------------------------------
scene = bpy.context.scene
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
arm_data = bpy.data.armatures.new('r_arm_skeleton')
rig = bpy.data.objects.new('r_arm_skeleton', arm_data)
scene.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')


def bone(name, head, tail, parent=None, up=Vector((0, 0, 1))):
    b = arm_data.edit_bones.new(name)
    b.head, b.tail = Vector(head), Vector(tail)
    b.align_roll(up)
    if parent:
        b.parent = arm_data.edit_bones[parent]
    return b


bone('forearm', (0, ELBOW_Y, 0), (0, ELBOW_Y / 2, 0))
bone('forearm_twist', (0, ELBOW_Y / 2, 0), (0, 0, 0), 'forearm')
bone('hand', (0, 0, 0), (FINGERS[1][1][0], FINGERS[1][1][1], 0), 'forearm_twist')
for k, (name, (mx, my), lens, w, splay, uvb, uvp) in enumerate(FINGERS):
    origin, axis, lat, up = finger_frames[k]
    p = origin.copy()
    parent = 'hand'
    for j, l in enumerate(lens):
        bone('%s_%d' % (name, j + 1), p, p + axis * l, parent)
        parent = '%s_%d' % (name, j + 1)
        p = p + axis * l
parent = 'hand'
for j, (a, b) in enumerate(zip(seg, seg[1:])):
    d = (b - a).normalized()
    bone('thumb_%d' % (j + 1), a, b, parent, (THUMB_UP - d * THUMB_UP.dot(d)).normalized())
    parent = 'thumb_%d' % (j + 1)
bpy.ops.object.mode_set(mode='OBJECT')


# ---- weights -------------------------------------------------------------------------------------------------------
def chain(s, bounds, names, halves):
    """Weights along a chain: names[i] before bounds[i] blends into names[i + 1] over +-halves[i]."""
    w = {names[0]: 1.0}
    for b, h, n0, n1 in zip(bounds, halves, names, names[1:]):
        t = smooth(b - h, b + h, s)
        if t <= 0:
            break
        carry = w.pop(n0)
        w[n0] = carry * (1 - t)
        w[n1] = carry * t
    return {k: x for k, x in w.items() if x > 0.001}


def weights(v):
    part, data = tag[v]
    co = v.co
    if part == 'finger':
        k, s = data
        name, (mx, my), (l1, l2, l3), w, splay, uvb, uvp = FINGERS[k]
        return chain(s, [0.0, l1, l1 + l2], ['hand', name + '_1', name + '_2', name + '_3'], [0.011, 0.0065, 0.0055])
    if part == 'thumb':
        return chain(data, [0.004, L_META, L_META + L_PROX], ['hand', 'thumb_1', 'thumb_2', 'thumb_3'], [0.012, 0.0085, 0.006])
    if part in ('palm', 'wrist'):
        y = co.y
        w = {'hand': smooth(-0.020, 0.012, y)}
        if w['hand'] < 1:
            w['forearm_twist'] = 1 - w['hand']
        # palm edge at the knuckles follows the fingers a little; the thenar follows the thumb's metacarpal
        cap_k = min(range(4), key=lambda k: abs(co.x - FINGERS[k][1][0]))
        reach = smooth(FINGERS[cap_k][1][1] - 0.022, FINGERS[cap_k][1][1] - 0.008, y) * 0.22
        if reach > 0 and 'hand' in w:
            w[FINGERS[cap_k][0] + '_1'] = w['hand'] * reach
            w['hand'] *= 1 - reach
        a, b = THUMB_CMC, THUMB_MCP
        t = max(0.0, min(1.0, (co - a).dot(b - a) / (b - a).length_squared))
        dist = (co - (a + (b - a) * t)).length
        m = (1 - smooth(0.012, 0.030, dist)) * smooth(0.15, 0.7, t) * 0.8
        if m > 0 and co.x < -0.012 and 'hand' in w:
            w['thumb_1'] = w['hand'] * m
            w['hand'] *= 1 - m
        return {k: x for k, x in w.items() if x > 0.001}
    if part in ('cuff', 'tab', 'stud'):
        y = data[0]
        h = 0.65 * smooth(-0.034, -0.002, y)  # the gauntlet's top bends with the hand
        return {'forearm_twist': 1 - h, 'hand': h} if h > 0.001 else {'forearm_twist': 1.0}
    if part == 'sleeve':
        y = data[0]
        t = smooth(-0.24, -0.07, y)
        return {'forearm': 1 - t, 'forearm_twist': t} if 0.001 < t < 0.999 else ({'forearm_twist': 1.0} if t >= 0.999 else {'forearm': 1.0})
    return {'hand': 1.0}


if UNWRAP and TARGETS:
    bm.faces.index_update()
    bm.verts.index_update()
    cap_set = set(cap) | set(mid_row)
    placed = 0
    for f in bm.faces:
        if f.material_index != 0:
            continue
        for l in f.loops:
            t = TARGETS.get('%d:%d' % (f.index, l.vert.index))
            fixed = (any(e.seam for e in l.vert.link_edges)  # the seams keep the stock's cut
                     or l.vert in cap_set                      # the knuckle row: on the stock's loops already, held
                     or l.vert.co.y < 0.002)                   # the wrist ring, where the gauntlet starts
            if t and l[uvl].pin_uv and not fixed:
                l[uvl].uv = t
                l[uvl].pin_uv = True
                placed += 1
    print('TARGETS pinned', placed, 'face corners')
me = bpy.data.meshes.new('r_arm')
vweights = [weights(v) for v in bm.verts]
bm.verts.index_update()
bm.to_mesh(me)
bm.free()
for p in me.polygons:
    p.use_smooth = True
obj = bpy.data.objects.new('r_arm', me)
scene.collection.objects.link(obj)
for name, path in (('glove', 'all_1st_inf_trooper_hands.tga'), ('sleeve', 'all_1st_inf_trooper_arms.tga')):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    img = m.node_tree.nodes.new('ShaderNodeTexImage')
    m.node_tree.nodes.active = img  # (textured views draw the active image node)
    img.image = bpy.data.images.load(os.path.join(TEX, path))
    m.node_tree.links.new(img.outputs['Color'], m.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
    me.materials.append(m)
for b in arm_data.bones:
    obj.vertex_groups.new(name=b.name)
for i, w in enumerate(vweights):
    total = sum(w.values())
    for name, x in w.items():
        obj.vertex_groups[name].add([i], x / total, 'REPLACE')
if UNWRAP:
    # the glove's uvs: conformal unwrap of the glove faces along the seams, the pins held
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='DESELECT')
    obj.active_material_index = 0
    bpy.ops.object.material_slot_select()
    bpy.ops.uv.unwrap(method='CONFORMAL', margin=0.0)
    bpy.ops.object.mode_set(mode='OBJECT')
    uvd = me.uv_layers.active.data
    pinned = [(uvd[i].uv.x * 256, (1 - uvd[i].uv.y) * 256) for i in range(len(uvd)) if uvd[i].pin_uv]
    print('UNWRAP pinned corners', len(pinned), 'glove uv bounds x %.0f-%.0f y %.0f-%.0f texels' % (
        min(u for p in me.polygons if p.material_index == 0 for u in [uvd[i].uv.x * 256 for i in p.loop_indices]),
        max(u for p in me.polygons if p.material_index == 0 for u in [uvd[i].uv.x * 256 for i in p.loop_indices]),
        min(v for p in me.polygons if p.material_index == 0 for v in [(1 - uvd[i].uv.y) * 256 for i in p.loop_indices]),
        max(v for p in me.polygons if p.material_index == 0 for v in [(1 - uvd[i].uv.y) * 256 for i in p.loop_indices])))
obj.parent = rig
mod = obj.modifiers.new('skeleton', 'ARMATURE')
mod.object = rig

# ---- relaxed rest pose: curl, then make it the rest ------------------------------------------------------------
REST_CURL = {'_1': 14, '_2': 18, '_3': 10}
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='POSE')
for pb in rig.pose.bones:
    pb.rotation_mode = 'XYZ'
    for suffix, deg in REST_CURL.items():
        if pb.name.endswith(suffix) and not pb.name.startswith('thumb'):
            pb.rotation_euler.x = -math.radians(deg)
    if pb.name in ('thumb_2', 'thumb_3'):
        pb.rotation_euler.x = -math.radians(10)
bpy.ops.object.mode_set(mode='OBJECT')
bpy.context.view_layer.objects.active = obj
bpy.ops.object.modifier_apply(modifier='skeleton')
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='POSE')
bpy.ops.pose.armature_apply(selected=False)
bpy.ops.object.mode_set(mode='OBJECT')
mod = obj.modifiers.new('skeleton', 'ARMATURE')
mod.object = rig

maxw = max(len([g for g in v.groups if g.weight > 0.001]) for v in me.vertices)
print('BUILT r_arm quads', len(me.polygons), 'tris', sum(len(p.vertices) - 2 for p in me.polygons), 'glove faces', GLOVE_FACES,
      'verts', len(me.vertices), 'bones', len(arm_data.bones), 'max bones per vertex', maxw,
      'non-manifold edges', sum(1 for e in me.edges if False))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
