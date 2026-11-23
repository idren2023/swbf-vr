"""Blender (blender -b --factory-startup re/body/r_hand_stock.blend -P tools/body_retopo.py -- <out.blend>): a clean quad
mesh for the hand (idren2023 2026-10-22: a retopology after character-animation practice), built around idren2023's
skeleton in the rest pose and projected onto the stock glove's surface, so the shape stays the artist's and only the
edge flow changes.

Topology: the palm as rings across the hand (26 vertices: 12 columns over the back, the pinky side, 12 under the palm,
the thumb side; three columns a finger) from the wrist to a cap row just short of the knuckles; each finger a tube of
8 from its cap columns and the web points between fingers, with three rings at every joint (just before, on, just
after the pivot) and one between; the thumb a tube of 8 out of an opening on the thumb side by the knuckle row, sharing edges with the back, the palm and
the index's base, three rings at its
knuckle and tip joint. Every vertex is placed by a ray from inside (the bone axis, or the palm's middle) to the stock
surface. Saved as object r_hand_retopo (parented to the skeleton, no weights yet) into <out.blend>."""
import bpy, bmesh, math, os, sys
from mathutils import Vector, geometry
from mathutils.bvhtree import BVHTree

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
stock = bpy.data.objects['r_hand_stock']
rig = stock.parent
B = {b.name: b for b in rig.data.bones}
sb = bmesh.new()
sb.from_mesh(stock.data)            # the rest pose (frozen), in the rig's frame
tree = BVHTree.FromBMesh(sb)
FINGERS = ('index', 'middle', 'ring', 'pinky')


def hit(origin, d, reach=0.08):
    """The stock surface from origin along d (first hit); the nearest surface point if the ray misses."""
    loc, nrm, i, dist = tree.ray_cast(origin, d.normalized(), reach)
    if loc is None:
        loc = tree.find_nearest(origin)[0]
    return loc


# hand frame: the hand bone (y along the palm to the middle knuckle, z the back of the hand)
H = B['hand'].matrix_local.to_3x3().normalized()
hx, hy, hz = H.col[0], H.col[1], H.col[2]
O = B['hand'].head_local.copy()


def hand_pt(x, y, z=0.0):
    return O + hx * x + hy * y + hz * z


def to_hand(p):
    q = p - O
    return Vector((q.dot(hx), q.dot(hy), q.dot(hz)))


bm = bmesh.new()


def quad(a, b, c, d):
    bm.faces.new((a, b, c, d))


# ---- palm ----------------------------------------------------------------------------------------------------------
mcp = {f: to_hand(B[f + '_1'].head_local) for f in FINGERS}
xs = [mcp[f].x for f in FINGERS]
gaps = [xs[1] - xs[0], xs[2] - xs[1], xs[3] - xs[2]]
width = [gaps[0], (gaps[0] + gaps[1]) / 2, (gaps[1] + gaps[2]) / 2, gaps[2]]
COLS = []
for k, f in enumerate(FINGERS):
    for o in (-0.33, 0.0, 0.33):
        COLS.append(mcp[f].x + o * width[k])
# the stock's wrist edge and the thumb's root along the palm
ys = [to_hand(v.co).y for v in sb.verts]
y_wrist = max(min(ys) + 0.002, -0.004)   # the wrist joint (the stock glove narrows to a point below it)
t1h, t1t = to_hand(B['thumb_1'].head_local), to_hand(B['thumb_1'].tail_local)
y_cap = min(mcp[f].y for f in FINGERS) - 0.008
root0 = max(y_wrist + 0.012, t1h.y + (t1t.y - t1h.y) * 0.35)
root1 = min(y_cap - 0.012, t1h.y + (t1t.y - t1h.y) * 0.85)
# rings along the palm about as far apart as the columns (idren2023: even quads; five rings left tall thin quads), the last
# two (the thumb's opening) where they were
PALM_STEP = float(os.environ.get('PALM_STEP', '0.0075'))
y_open = (root0 + root1) / 2
n_ = max(1, round((y_open - y_wrist) / PALM_STEP))
STATIONS = [y_wrist + (y_open - y_wrist) * j / n_ for j in range(n_)] + [y_open, root1]


def palm_ring(y, cap=False):
    """26 vertices across the palm at y: 12 over the back (thumb side first), the pinky side, 12 under (pinky side
    first), the thumb side; columns squeezed inside the palm's own width there."""
    centre = hand_pt(0.0, y)
    side_p = hit(centre, hx)        # pinky side
    side_t = hit(centre, -hx)       # thumb side
    xp, xt = to_hand(side_p).x, to_hand(side_t).x
    lo, hi = min(COLS), max(COLS)
    # the columns spread over the palm's width here (92 %), keeping their spacing pattern
    a0, a1 = xt + (xp - xt) * 0.04, xp - (xp - xt) * 0.04
    cx = [a0 + (c - lo) / (hi - lo) * (a1 - a0) for c in COLS]
    ring = []
    for c, x in enumerate(cx):
        yy = (mcp[FINGERS[c // 3]].y - 0.008) if cap else y
        ring.append(bm.verts.new(hit(hand_pt(x, yy), hz)))
    ring.append(bm.verts.new(side_p))
    for c in reversed(range(12)):
        yy = (mcp[FINGERS[c // 3]].y - 0.008) if cap else y
        ring.append(bm.verts.new(hit(hand_pt(cx[c], yy), -hz)))
    ring.append(bm.verts.new(side_t))
    return ring


T = lambda c: c          # top (back) column c
Bo = lambda c: 24 - c    # bottom (palm) column c
R, L = 12, 25            # pinky side, thumb side
rings = [palm_ring(y) for y in STATIONS]
cap = palm_ring(y_cap, cap=True)
rings.append(cap)
# the thumb's opening: the thumb side between stations 2 and 4 (its middle vertex removed)
# the thumb's opening: on the hand's thumb side between the last two palm rings and the knuckle row, so its edge ring
# runs over the back (column 0), down the side, under the palm (column 0) and along the index's base (idren2023: the
# thumb must join the palm, the back and the index cleanly)
io = len(STATIONS) - 2
opening_cells = {(io, 24), (io, 25), (io + 1, 24), (io + 1, 25)}
for i in range(len(rings) - 1):
    a, b = rings[i], rings[i + 1]
    for s_ in range(26):
        if (i, s_) in opening_cells:
            continue
        quad(a[s_], a[(s_ + 1) % 26], b[(s_ + 1) % 26], b[s_])
# webs between fingers: a point in each cleft (from the palm's middle forward), cap faces between fingers
mid_row = [cap[L]] + [None] * 10 + [cap[R]]
for c in (2, 5, 8):
    x = (COLS[c] + COLS[c + 1]) / 2
    y = (mcp[FINGERS[c // 3]].y + mcp[FINGERS[c // 3 + 1]].y) / 2 - 0.012
    mid_row[c] = mid_row[c + 1] = bm.verts.new(hit(hand_pt(x, y), hy))
for c in (2, 5, 8):
    w = mid_row[c]
    bm.faces.new((cap[T(c)], w, cap[T(c + 1)]))
    bm.faces.new((cap[Bo(c + 1)], w, cap[Bo(c)]))
for c in range(12):
    if mid_row[c] is None:
        mid_row[c] = cap[L] if c < 6 else cap[R]


# ---- fingers and the thumb: tubes of 8 along a bone chain -------------------------------------------------------
def chain_frame(bones, s):
    """Point, direction and back (z) at arc length s along a chain of bones (rest)."""
    acc = 0.0
    for k, n in enumerate(bones):
        b = B[n]
        h, t = b.head_local, b.tail_local
        L_ = (t - h).length
        if s <= acc + L_ or k == len(bones) - 1:
            f = (s - acc) / L_
            d = (t - h).normalized()
            z = b.matrix_local.to_3x3().col[2].normalized()
            return h + (t - h) * f, d, z
        acc += L_


def chain_len(bones):
    return sum((B[n].tail_local - B[n].head_local).length for n in bones)


def stations(bones, start, d=0.004):
    """Arc lengths: three rings at every joint between the chain's bones, one between, up to the tip."""
    out, acc = [start], 0.0
    for k, n in enumerate(bones):
        L_ = (B[n].tail_local - B[n].head_local).length
        if k > 0:
            out += [acc - d, acc, acc + d]
        mid = acc + L_ / 2
        if mid > out[-1] + d:
            out.append(mid)
        acc += L_
    out.append(acc - 0.010)
    return [s for s in out if s >= start and s <= acc - 0.010]


def ring8(bones, s):
    p, d, z = chain_frame(bones, s)
    z = (z - d * z.dot(d)).normalized()
    x = d.cross(z).normalized()
    out = []
    for ang in (135, 90, 45, 0, -45, -90, -135, 180):
        a = math.radians(ang)
        out.append(bm.verts.new(hit(p, x * math.cos(a) + z * math.sin(a), 0.04)))
    return out


def cap_tip(prev, bones):
    """Close a digit (idren2023: fingertips, best practice): the loops wrap over the end: three smaller rings whose rays
    tilt forward from a point inside the tip (30 / 55 / 75 deg), then a quad cap of four small faces round the end;
    the shape comes from the stock surface (its fuller pad included)."""
    L_ = chain_len(bones)
    p, d, z = chain_frame(bones, L_ - 0.008)
    z = (z - d * z.dot(d)).normalized()
    x = d.cross(z).normalized()
    for tilt in (30, 55, 75):
        t_ = math.radians(tilt)
        ring = []
        for ang in (135, 90, 45, 0, -45, -90, -135, 180):
            a = math.radians(ang)
            radial = x * math.cos(a) + z * math.sin(a)
            ring.append(bm.verts.new(hit(p, radial * math.cos(t_) + d * math.sin(t_), 0.04)))
        for k in range(8):
            quad(prev[k], prev[(k + 1) % 8], ring[(k + 1) % 8], ring[k])
        prev = ring
    tip = bm.verts.new(hit(p, d, 0.04))
    r = prev
    quad(r[0], r[1], tip, r[7])
    quad(r[1], r[2], r[3], tip)
    quad(tip, r[3], r[4], r[5])
    quad(r[7], tip, r[5], r[6])


def tube(border, bones, sts):
    prev = border
    for s in sts:
        ring = ring8(bones, s)
        for k in range(8):
            quad(prev[k], prev[(k + 1) % 8], ring[(k + 1) % 8], ring[k])
        prev = ring
    cap_tip(prev, bones)


for k, f in enumerate(FINGERS):
    c0 = 3 * k
    border = [cap[T(c0)], cap[T(c0 + 1)], cap[T(c0 + 2)], mid_row[c0 + 2], cap[Bo(c0 + 2)], cap[Bo(c0 + 1)], cap[Bo(c0)],
              mid_row[c0]]
    bones = [f + '_1', f + '_2', f + '_3']
    tube(border, bones, [s for s in stations(bones, 0.0) if s > 0.002])

# the thumb: out of the opening; its tube starts where the thumb leaves the palm (half way along its first bone)
tb = [rings[io][T(0)], rings[io + 1][T(0)], cap[T(0)], cap[L], cap[Bo(0)], rings[io + 1][Bo(0)], rings[io][Bo(0)],
      rings[io][L]]
bm.verts.remove(rings[io + 1][L])
tbones = ['thumb_1', 'thumb_2', 'thumb_3']
L1 = (B['thumb_1'].tail_local - B['thumb_1'].head_local).length
# (idren2023: the base too dense and folding, aim for even quads): the tube starts at the thumb's knuckle loops, spaced
# TD either side of each joint, and joins the opening in one band (no rings between: they bunched on the bend's inside)
TD = float(os.environ.get('TD', '0.006'))
THUMB_START = L1 - TD if os.environ.get('TSTART', 'knuckle') == 'knuckle' else L1 * 0.7
tst = [s for s in stations(tbones, THUMB_START, TD) if s >= THUMB_START - 1e-6]
first = ring8(tbones, tst[0])
best = None
for flip in (False, True):
    rr = list(reversed(first)) if flip else first
    for off in range(8):
        cost = sum((tb[k].co - rr[(k + off) % 8].co).length for k in range(8))
        if best is None or cost < best[0]:
            best = (cost, [rr[(k + off) % 8] for k in range(8)])
fr = best[1]
# two rings between the opening and the thumb's first ring, snapped to the stock surface (one long band of faces
# there was flat and faceted)
prev = tb
NBR = int(os.environ.get('NBR', '0'))
for t_ in [j / (NBR + 1) for j in range(1, NBR + 1)]:
    mid = [bm.verts.new(tree.find_nearest(a.co.lerp(b.co, t_))[0]) for a, b in zip(tb, fr)]
    for k in range(8):
        quad(prev[k], prev[(k + 1) % 8], mid[(k + 1) % 8], mid[k])
    prev = mid
for k in range(8):
    quad(prev[k], prev[(k + 1) % 8], fr[(k + 1) % 8], fr[k])
prev = first
for s in tst[1:]:
    ring = ring8(tbones, s)
    for k in range(8):
        quad(prev[k], prev[(k + 1) % 8], ring[(k + 1) % 8], ring[k])
    prev = ring
cap_tip(prev, tbones)

bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=0.0002)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
# clean-up (idren2023: rough): relax on the surface: each vertex half way to its neighbours' mean, then back onto the stock
# surface (nearest point); the open wrist edge stays; RELAX passes
RELAX = 8
for _ in range(RELAX):
    new = {}
    for v in bm.verts:
        if any(not e.is_manifold for e in v.link_edges):
            continue
        nb = [e.other_vert(v).co for e in v.link_edges]
        m = sum(nb, Vector()) / len(nb)
        new[v] = tree.find_nearest(v.co.lerp(m, 0.5))[0]
    for v, co in new.items():
        v.co = co
# less blocky (idren2023): one Catmull-Clark level (the same edge flow, 16-sided fingers), every vertex back onto the stock
# surface, two more relax passes
tmp_me = bpy.data.meshes.new('tmp')
bm.to_mesh(tmp_me)
tmp = bpy.data.objects.new('tmp', tmp_me)
bpy.context.scene.collection.objects.link(tmp)
sub = tmp.modifiers.new('subd', 'SUBSURF')
sub.levels = 1
sub.boundary_smooth = 'PRESERVE_CORNERS'
bpy.context.view_layer.objects.active = tmp
bpy.ops.object.modifier_apply(modifier='subd')
bm = bmesh.new()
bm.from_mesh(tmp_me)
bpy.data.objects.remove(tmp)
for v in bm.verts:
    v.co = tree.find_nearest(v.co)[0]
for _ in range(2):
    new = {}
    for v in bm.verts:
        if any(not e.is_manifold for e in v.link_edges):
            continue
        nb = [e.other_vert(v).co for e in v.link_edges]
        new[v] = tree.find_nearest(v.co.lerp(sum(nb, Vector()) / len(nb), 0.5))[0]
    for v, co in new.items():
        v.co = co
# the stock's facets and hard creases (390 tris; the thumb-index web, the thumb's knuckle) came through the projection
# (idren2023: the thumb's join rough): Taubin smoothing off the surface (shrink then inflate: keeps the volume), the open
# wrist edge fixed; TAUBIN passes, the largest move printed
TAUBIN = int(os.environ.get('TAUBIN', '30'))
start = {v: v.co.copy() for v in bm.verts}
for _ in range(TAUBIN):
    for lam in (0.5, -0.53):
        new = {}
        for v in bm.verts:
            if any(not e.is_manifold for e in v.link_edges):
                continue
            nb = [e.other_vert(v).co for e in v.link_edges]
            new[v] = v.co + (sum(nb, Vector()) / len(nb) - v.co) * lam
        for v, co in new.items():
            v.co = co
dev = sorted((v.co - tree.find_nearest(v.co)[0]).length for v in bm.verts)
print('TAUBIN %d: off the stock surface max %.1f mm, p99 %.1f mm' % (TAUBIN, dev[-1] * 1000, dev[int(len(dev) * .99)] * 1000))
me = bpy.data.meshes.new('r_hand_retopo')
bm.to_mesh(me)
for p_ in me.polygons:
    p_.use_smooth = True
ob = bpy.data.objects.new('r_hand_retopo', me)
bpy.context.scene.collection.objects.link(ob)
ob.parent = rig
for b in rig.data.bones:
    ob.vertex_groups.new(name=b.name)
mod = ob.modifiers.new('skeleton', 'ARMATURE')
mod.object = rig
non_manifold = sum(1 for e in bm.edges if not e.is_manifold)
quads = sum(1 for f in me.polygons if len(f.vertices) == 4)
print('RETOPO faces %d (quads %d), tris %d, verts %d, non-manifold edges %d (the open wrist ring: 26)' % (
    len(me.polygons), quads, sum(len(f.vertices) - 2 for f in me.polygons), len(me.vertices), non_manifold))
bm.normal_update()
# roughness at the thumb's join: edges within 25 mm of the thumb's first bone whose faces meet at more than 30 deg
th0, th1 = B['thumb_1'].head_local, B['thumb_1'].tail_local
near = lambda p: (p - geometry.intersect_point_line(p, th0, th1)[0]).length < 0.025 and \
    0 <= geometry.intersect_point_line(p, th0, th1)[1] <= 1
ang = [math.degrees(e.calc_face_angle()) for e in bm.edges if e.is_manifold and near((e.verts[0].co + e.verts[1].co) / 2)]
print('THUMBJOIN edges %d, >30 deg %d, >45 deg %d, max %.0f deg' % (
    len(ang), sum(a > 30 for a in ang), sum(a > 45 for a in ang), max(ang)))
# quad size (square root of the area; idren2023: even density): near the thumb's first bone vs the whole hand
sz = sorted(math.sqrt(f.calc_area()) * 1000 for f in bm.faces)
tz = sorted(math.sqrt(f.calc_area()) * 1000 for f in bm.faces if near(f.calc_center_median()))
asp = [max(e.calc_length() for e in f.edges) / max(min(e.calc_length() for e in f.edges), 1e-9) for f in bm.faces
       if near(f.calc_center_median())]
print('ASPECT thumb join: faces %d, longest / shortest edge > 2: %d, > 3: %d, max %.1f' % (
    len(asp), sum(a > 2 for a in asp), sum(a > 3 for a in asp), max(asp)))
ga = [max(e.calc_length() for e in f.edges) / max(min(e.calc_length() for e in f.edges), 1e-9) for f in bm.faces]
print('ASPECT whole hand: faces %d, > 2: %d, > 3: %d; palm stations %d' % (len(ga), sum(a > 2 for a in ga),
      sum(a > 3 for a in ga), len(STATIONS)))
print('QUADSIZE hand median %.1f mm; thumb join p10 %.1f median %.1f p90 %.1f mm' % (
    sz[len(sz) // 2], tz[len(tz) // 10], tz[len(tz) // 2], tz[len(tz) * 9 // 10]))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
