"""Blender (blender -b --factory-startup re/body/r_arm_grips.blend -P tools/body_grab.py -- <weapon> [palm=mm] [top=mm]
[back=mm] [heel=x,y,z] [turn=deg] [swing=<finger>:<deg> ..] [trigger=hole | trigger=x,y,z] [thumbto=x,y,z]
[keep up=mm] [set=<bone>:x,y,z ..] [render=<out>] [save] [write]): a weapon grabbed as a real hand grabs it (idren2023 2026-10-31), by the bones:

1. the hand opened, about to grab: fingers straight, the thumb out (a wide J);
2. the weapon's grip laid on the line from the web between thumb and index (between the thumb's base joint and the
   index knuckle) down to the heel of the palm: the grip's axis along that line, the top of the grip (where the frame
   starts) `top` mm (default 0) above the web point (the web at the grip's top), its back `back` mm (default 0) behind it, its palm-side face on
   the palm (`palm`: the palm's plane, z = -24 mm by default; the heel default 25,35 mm); `turn` turns it further about the hand's z;
3. each finger closed round the grip by its bones: each bone a capsule of the finger's own thickness (measured from
   the hand mesh), kept off the weapon's surface (`press` mm into it allowed, default 1.5) and out of its inside, off
   the palm and off the finger closed before it; of the poses that allow (knuckle, middle joint, end joint k times
   the middle one as a finger bends), the one reaching furthest round the grip's axis (a finger in a trigger guard
   goes round the grip inside it); trigger=: the index's end segment onto the trigger instead (`hole`: the guard's
   opening found in the side view, its rear at the gun's middle);
4. the thumb closed the other way round the grip the same way (its base turned, its two joints bent).

The controller's grip frame in the hand (axis_point, axis_direction) is turned and moved with the weapon, so the
weapon sits on the controller exactly as before and only the hand turns on it.
keep: the weapon stays where it was saved (no new placement), slid `up` mm along its grip (the hand up the grip);
set=: that bone's angles after the search (a finger set by hand, e.g. set=middle_2:-40,0,0);
thumbto=a/b: the thumb's tip toward the middle of those bones' heads (e.g. index_3/middle_3, the gap between them).
render=: <out>-bones.png (the weapon's edges and the bones, from the side and four more angles) and <out>-hand.png
(the textured hand, 8 views). write: the pose's line in tools/body_poses.py; save: the weapon's place in the .blend
and the grip json."""
import bpy, bmesh, json, math, os, re, sys
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

args = sys.argv[sys.argv.index('--') + 1:]
WEAP = args[0]
opt = {}
for a in args[1:]:
    if '=' in a:
        k, v = a.split('=', 1)
        opt.setdefault(k, []).append(v)
one = lambda k, d=None: opt.get(k, [d])[-1]
vec = lambda s: Vector([float(x) / 1000 for x in s.split(',')])
TOOLS = os.path.dirname(os.path.abspath(__file__))
POSES_PY = os.path.join(TOOLS, 'body_poses.py')
src = open(POSES_PY, encoding='utf-8').read()
LABEL = re.search(r"'%s': '([^']+)'" % WEAP, src).group(1)

hand = bpy.data.objects['r_hand_retopo']
rig = hand.parent
P = rig.pose.bones
for pb in P:
    for c in pb.constraints:
        c.mute = True
w = bpy.data.objects.get('weap_' + WEAP) or bpy.data.objects['wip_' + WEAP]
JSON_PATH = os.path.join('re', 'body', 'grip-weap_%s.json' % WEAP)
GJ = json.load(open(JSON_PATH)) if os.path.exists(JSON_PATH) else None
W_LOAD = w.matrix_world.copy()
FINGERS = ('index', 'middle', 'ring', 'pinky')
DIGITS = {f: ['%s_%d' % (f, k) for k in (1, 2, 3)] for f in FINGERS + ('thumb',)}
PALM_Z = float(one('palm', -24)) / 1000
PRESS = float(one('press', 1.5)) / 1000


def apply(p):
    for pb in P:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
        pb.location = (0, 0, 0)
    for n, d in p.items():
        if n in P:
            P[n].rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
            P[n].rotation_euler = [math.radians(x) for x in d]
    bpy.context.view_layer.update()


def seg(b):
    return rig.matrix_world @ P[b].head, rig.matrix_world @ P[b].tail


# ---- each bone's thickness: the median distance of its own vertices (weighted mostly to it) from the bone --------
names = {g.index: g.name for g in hand.vertex_groups}
RADIUS = {}
apply({})
for b in [b for f in DIGITS.values() for b in f]:
    h, t = seg(b)
    ds = []
    for v in hand.data.vertices:
        if any(names[g.group] == b and g.weight > 0.7 for g in v.groups):
            q = hand.matrix_world @ v.co
            ab = t - h
            s = max(0.0, min(1.0, (q - h).dot(ab) / ab.length_squared))
            ds.append((q - (h + ab * s)).length)
    ds.sort()
    RADIUS[b] = ds[len(ds) // 2] if ds else 0.007
print('GRAB finger thickness (radius mm): %s' % ' '.join('%s %.1f' % (b, r * 1000) for b, r in RADIUS.items()))

# ---- 1. the open hand, about to grab -----------------------------------------------------------------------------
OPEN = {'index_1': (0, 0, 0), 'middle_1': (0, 0, 0), 'ring_1': (0, 0, 0), 'pinky_1': (0, 0, 0),
        'thumb_1': (-5, 0, 35), 'thumb_2': (-10, 0, 0), 'thumb_3': (-20, 0, 0)}
apply(OPEN)
# the web: where the backstrap's top sits in the crease between thumb and index (idren2023 2026-11-01: the web at the
# top of the grip; the point the accepted Rebel pistol and rifle grips of b456 put it, 26 mm toward the wrist from the
# index knuckle); webk= the old point between the index knuckle (0) and the thumb's base joint (1)
WEB = (seg('index_1')[0].lerp(seg('thumb_2')[0], float(one('webk'))) if one('webk') else
       vec(one('web', '-36,69,%d' % round(PALM_Z * 1000))))
HEEL = vec(one('heel', '40,42,%d' % round(PALM_Z * 1000)))  # the heel on the little finger's side (hypothenar)
AXIS = (HEEL - WEB).normalized()  # from the web (the grip's top) toward the heel (its butt)
AXIS_TURN = Matrix.Rotation(math.radians(float(one('turn', 0))), 3, 'Z')
AXIS = (AXIS_TURN @ AXIS).normalized()
print('GRAB web %s, heel %s mm, grip line %s' % (tuple(round(c * 1000) for c in WEB), tuple(round(c * 1000) for c in HEEL),
                                                 tuple(round(c, 2) for c in AXIS)))


# ---- the weapon's grip: slices across an axis ----------------------------------------------------------------------
def frame_of(a):
    """(a, u, v): a the grip's axis, u forward (the hand's y square to it), v out of the back of the hand."""
    u = Vector((0, 1, 0))
    u = (u - a * u.dot(a)).normalized()
    return a, u, a.cross(u) * -1 if a.cross(u).z < 0 else a.cross(u)


def mesh_world():
    me = w.data
    me.calc_loop_triangles()
    V = [w.matrix_world @ v.co for v in me.vertices]
    return V, [tuple(t.vertices) for t in me.loop_triangles]


def cut(a, c, V, T, S, UV):
    """The pieces of the weapon cut by the plane s = c across axis a: lists of (u, v) points."""
    segs = []
    for t in T:
        pts = []
        for i, j in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            if (S[i] < c) != (S[j] < c):
                f = (c - S[i]) / (S[j] - S[i])
                pts.append((UV[i][0] + (UV[j][0] - UV[i][0]) * f, UV[i][1] + (UV[j][1] - UV[i][1]) * f))
        if len(pts) == 2:
            segs.append(pts)
    key = lambda q: (round(q[0] * 2000), round(q[1] * 2000))
    par = {}

    def find(k):
        while par.setdefault(k, k) != k:
            k = par[k]
        return k
    for p_, q_ in segs:
        par[find(key(p_))] = find(key(q_))
    groups = {}
    for p_, q_ in segs:
        groups.setdefault(find(key(p_)), []).extend([p_, q_])
    return list(groups.values())


# a point in the grip where the hand holds it now (the hand frame): the grip json's own axis point, or seed=x,y,z mm
GRIP_SEED = vec(one('seed')) if one('seed') else (
    Vector(GJ.get('measured_axis_point', GJ['axis_point'])) if GJ else Vector((0, 0.075, -0.04)))


def slices(a, step=0.002):
    """The grip's cross-sections along axis a: the piece nearest GRIP_SEED at the palm's height, followed up and down
    while it stays one piece of about its size (a frame, a guard joining or a block below ends it):
    [(s, umin, umax, vmin, vmax, ucentre, vcentre)] from the butt up."""
    a, u, v = frame_of(a)
    V, T = mesh_world()
    S = [p.dot(a) for p in V]
    UV = [(p.dot(u), p.dot(v)) for p in V]

    def piece(c, cur, depth0=None):
        best = None
        for g in cut(a, c, V, T, S, UV):
            cu = sum(q[0] for q in g) / len(g)
            cv = sum(q[1] for q in g) / len(g)
            d = math.hypot(cu - cur[0], cv - cur[1])
            if d < (0.02 if depth0 is None else 0.006) and (best is None or d < best[0]):
                best = (d, g, cu, cv)
        if best is None:
            return None
        g = best[1]
        row = (c, min(q[0] for q in g), max(q[0] for q in g), min(q[1] for q in g), max(q[1] for q in g), best[2], best[3])
        if depth0 is not None and (row[2] - row[1] > 1.35 * depth0 or row[4] - row[3] > 1.6 * depth0 + 0.01):
            return 'wide', row
        return row
    c0 = GRIP_SEED.dot(a)
    start = piece(c0, (GRIP_SEED.dot(u), GRIP_SEED.dot(v)))
    if start is None:
        raise SystemExit('GRAB no grip piece near the seed %s mm (seed=x,y,z)' % str(tuple(round(c * 1000) for c in GRIP_SEED)))
    depth0 = start[2] - start[1]
    rows, frame_row = [start], None
    for direction in (-1, 1):  # up (toward the frame), then down (toward the butt)
        cur = (start[5], start[6])
        c = c0 + direction * step
        while abs(c - c0) < 0.07:  # a hand's width each way at most
            r = piece(c, cur, depth0)
            if r is None:
                break
            if r[0] == 'wide':
                if direction < 0:
                    frame_row = r[1]
                break
            rows.append(r)
            cur = (r[5], r[6])
            c += direction * step
    rows.sort(key=lambda r: -r[0])
    return rows, frame_row


def grip_shape(a):
    """The grip along axis a: the top (s where the frame starts), the back (u) and palm-side face (v) just under it,
    its centre line's direction."""
    grip, frame_row = slices(a)
    s_top = frame_row[0] if frame_row else grip[-1][0]
    med = sorted(r[2] - r[1] for r in grip)[len(grip) // 2]
    under = [r for r in grip if s_top + 0.004 < r[0] < s_top + 0.03] or grip[-3:]
    back = sum(r[1] for r in under) / len(under)
    face = max(r[4] for r in under)
    n = len(grip)
    ms = sum(r[0] for r in grip) / n
    mu = sum(r[5] for r in grip) / n
    mv = sum(r[6] for r in grip) / n
    ss = sum((r[0] - ms) ** 2 for r in grip) or 1e-9
    du = sum((r[0] - ms) * (r[5] - mu) for r in grip) / ss
    dv = sum((r[0] - ms) * (r[6] - mv) for r in grip) / ss
    a_, u_, v_ = frame_of(a)
    true_axis = (a_ + u_ * du + v_ * dv).normalized()
    return dict(top=s_top, back=back, face=face, axis=true_axis, length=grip[0][0] - s_top, depth=med,
                width=sum(r[4] - r[3] for r in grip) / n, rows=grip)


# ---- 2. the grip on the line from the web to the heel ------------------------------------------------------------
try:
    g0 = grip_shape(Vector((1, 0, 0)))
except SystemExit:  # no piece across the hand's x at the seed (a steep grip): across the grip's last measured axis
    if not (GJ and 'measured_axis_direction' in GJ):
        raise
    g0 = grip_shape(Vector(GJ['measured_axis_direction']))
g1 = grip_shape(g0['axis'])  # refined along its own axis
print('GRAB before: grip axis %s' % (tuple(round(c, 2) for c in g0['axis']),))
if 'measure' in args:  # where the grip sits now relative to the open hand's web, palm and heel line
    a_, u_, v_ = frame_of(g1['axis'])
    print('GRAB measure: top %.0f mm above the web, back %.0f mm behind it, face at z %.0f mm; grip axis %s, %.1f deg from '
          'the web-heel line; %.0f mm long' % ((WEB.dot(a_) - g1['top']) * 1000, (WEB.dot(u_) - g1['back']) * 1000,
                                             g1['face'] * 1000, tuple(round(c, 2) for c in g1['axis']),
                                             math.degrees(g1['axis'].angle(AXIS)), g1['length'] * 1000))
    corner = a_ * g1['top'] + u_ * g1['back'] + v_ * g1['face']
    print('GRAB measure: the backstrap top at %s mm; index knuckle %s, thumb base %s, thumb_2 %s' % tuple(
        str(tuple(round(c * 1000) for c in q)) for q in (corner, seg('index_1')[0], seg('thumb_1')[0], seg('thumb_2')[0])))
    raise SystemExit(0)
KEEP = 'keep' in args  # the weapon left at its saved place, only slid `up` mm along its grip (the hand up the grip)
turn = Matrix() if KEEP else g1['axis'].rotation_difference(AXIS).to_matrix().to_4x4()
pivot = w.matrix_world.translation.copy()
w.matrix_world = Matrix.Translation(pivot) @ turn @ Matrix.Translation(-pivot) @ W_LOAD
bpy.context.view_layer.update()
GRIP_SEED = pivot + turn.to_3x3() @ (GRIP_SEED - pivot)  # the seed turned with the weapon
g = grip_shape(AXIS)
a, u, v = frame_of(AXIS)
TOP = float(one('top', 0)) / 1000
BACK = float(one('back', 0)) / 1000
want_s = WEB.dot(a) - TOP
want_u = WEB.dot(u) - BACK
want_v = PALM_Z  # the palm's plane (v is about the hand's z)
move = a * (want_s - g['top']) + u * (want_u - g['back']) + v * (want_v - g['face'])
if KEEP:  # a points from the web toward the heel: the weapon moved that way is the hand moved up its grip
    ga = g1['axis'] if g1['axis'].dot(a) > 0 else -g1['axis']
    move = ga * float(one('up', 0)) / 1000
w.matrix_world = Matrix.Translation(move) @ w.matrix_world
bpy.context.view_layer.update()
DELTA = w.matrix_world @ W_LOAD.inverted()  # the weapon's move in the hand frame (applied to the controller's frame too)
def long_axis():
    import numpy as np
    Vv = np.array([[q.x, q.y, q.z] for q in mesh_world()[0]])
    Vv -= Vv.mean(0)
    e = np.linalg.eigh(Vv.T @ Vv)[1][:, -1]
    return tuple(round(float(c), 2) for c in (e if e[1] > 0 else -e))
print('GRAB axes: grip measured %s, wanted %s; the gun long axis now %s' % (
    tuple(round(c, 2) for c in g1['axis']), tuple(round(c, 2) for c in AXIS), long_axis()))
print('GRAB grip: %.0f mm long, %.0f deep, %.0f wide; turned %.1f deg; moved %s mm' % (
    g['length'] * 1000, g['depth'] * 1000, g['width'] * 1000, math.degrees(g1['axis'].angle(AXIS)),
    tuple(round(c * 1000) for c in DELTA.translation)))
AXIS_POINT = Vector((0, 0, 0))
# the grip's centre line (for the wrap measure): through the middle of the slices
rows = g['rows']
mid = rows[len(rows) // 2]
AXIS_POINT = a * mid[0] + u * mid[5] + v * mid[6] + move  # the rows were cut before the move

# ---- the weapon as a surface and a volume --------------------------------------------------------------------------
bm = bmesh.new()
bm.from_mesh(w.data)
bm.transform(w.matrix_world)
WT = BVHTree.FromBMesh(bm)


def inside(p):
    """Inside the weapon: a ray along +z crosses its surface an odd number of times (and so does one along -z)."""
    votes = 0
    for d in (Vector((0, 0, 1)), Vector((0, 0, -1)), Vector((0.3, 0.9, 0.1)).normalized()):
        q, n = p.copy(), 0
        for _ in range(24):
            hit = WT.ray_cast(q, d, 1.0)
            if hit[0] is None:
                break
            n += 1
            q = hit[0] + d * 1e-5
        votes += n % 2
    return votes >= 2


def samples(b, n=5):
    h, t = seg(b)
    return [h + (t - h) * (k / (n - 1)) for k in range(n)]


def clear_of_weapon(b):
    r = RADIUS[b] - PRESS
    for q in samples(b):
        loc, _, _, d = WT.find_nearest(q, 0.05)
        if loc is not None and d < r:
            return False
        if inside(q):
            return False
    return True


def clear_of_palm(b):
    for q in samples(b):
        if q.y < 0.085 and q.z > PALM_Z - RADIUS[b] + 0.002 and -0.035 < q.x < 0.045:
            return False
    return True


def seg_dist(p1, q1, p2, q2):
    best = 1e9
    for k in range(6):
        a_ = p1 + (q1 - p1) * (k / 5)
        ab = q2 - p2
        s = max(0.0, min(1.0, (a_ - p2).dot(ab) / ab.length_squared))
        best = min(best, (a_ - (p2 + ab * s)).length)
    return best


def clear_of(bones_a, bones_b, slack=float(one('slack', 3.5)) / 1000):  # neighbouring fingers press a little
    for x in bones_a:
        for y in bones_b:
            if seg_dist(*seg(x), *seg(y)) < RADIUS[x] + RADIUS[y] - slack:
                return False
    return True


def wrap(chain_bones, sign=1.0):
    """How far round the grip's centre line the chain reaches (degrees), from its first joint to its tip."""
    pts = [seg(chain_bones[0])[0]] + [seg(b)[1] for b in chain_bones]
    flat = []
    for q in pts:
        q = q - AXIS_POINT
        flat.append(q - a * q.dot(a))
    total = 0.0
    for p_, q_ in zip(flat, flat[1:]):
        if p_.length < 1e-6 or q_.length < 1e-6:
            continue
        ang = p_.angle(q_, 0.0)
        total += ang if a.dot(p_.cross(q_)) >= 0 else -ang
    return math.degrees(total) * sign


pose = dict(OPEN)
apply(pose)
# the sense a curl turns round the grip's axis: a bent middle finger against a straight one
w0 = wrap(DIGITS['middle'])
apply(dict(pose, middle_1=(-40, 0, 0), middle_2=(-40, 0, 0)))
SENSE = 1.0 if wrap(DIGITS['middle']) > w0 else -1.0


def finger_pose(f, a1, a2, a3, swing):
    return dict(pose, **{f + '_1': (-a1, 0, swing), f + '_2': (-a2, 0, 0), f + '_3': (-a3, 0, 0)})


def ok_finger(f, before):
    return all(clear_of_weapon(b) and clear_of_palm(b) for b in DIGITS[f]) and (
        not before or clear_of(DIGITS[f], DIGITS[before]))


SWING0 = {'index': -2, 'middle': 0, 'ring': 5, 'pinky': 10}
for sv in opt.get('swing', []):
    SWING0[sv.split(':')[0]] = float(sv.split(':')[1])


def close(f, before, score_fn, swings):
    best = None
    for sw in swings:
        for k in (0.55, 0.7, 0.85):
            for a2 in range(0, 106, 6):
                for a1 in range(-6, 91, 6):
                    a3 = min(round(k * a2), 80)
                    apply(finger_pose(f, a1, a2, a3, sw))
                    sc = score_fn()
                    if best is not None and sc <= best[0]:
                        continue
                    if ok_finger(f, before):
                        best = (sc, a1, a2, a3, sw)
    if best is None:
        return None
    sc, a1, a2, a3, sw = best
    for _ in range(2):
        for d1 in (-3, 0, 3):
            for d2 in (-3, 0, 3):
                for d3 in (-4, 0, 4):
                    for ds in (-2, 0, 2):
                        t = finger_pose(f, a1 + d1, min(a2 + d2, 110), max(0, min(a3 + d3, 85)), sw + ds)
                        apply(t)
                        s2 = score_fn()
                        if s2 > best[0] and ok_finger(f, before):
                            best = (s2, a1 + d1, min(a2 + d2, 110), max(0, min(a3 + d3, 85)), sw + ds)
        sc, a1, a2, a3, sw = best
    return finger_pose(f, a1, a2, a3, sw), sc


# ---- 3. the fingers --------------------------------------------------------------------------------------------------
TRIGGER = None
if one('trigger') and one('trigger') != 'hole':
    TRIGGER = vec(one('trigger'))
elif one('trigger') == 'hole':
    import numpy as np
    Vw = np.array([[p.x, p.y, p.z] for p in mesh_world()[0]]) * 1000
    Fw = mesh_world()[1]
    S_ = 1.0
    lo = Vw[:, :2].min(0) - 3
    hi = Vw[:, :2].max(0) + 3
    nx, ny = int((hi - lo)[0] / S_) + 1, int((hi - lo)[1] / S_) + 1
    occ = np.zeros((nx, ny), bool)
    for t in Fw:
        A_, B_, C_ = Vw[list(t)][:, :2]
        n_ = int(max(np.linalg.norm(B_ - A_), np.linalg.norm(C_ - A_)) / S_ * 1.5) + 2
        for i in range(n_ + 1):
            for j in range(n_ + 1 - i):
                q = A_ + (B_ - A_) * i / n_ + (C_ - A_) * j / n_
                occ[int((q[0] - lo[0]) / S_), int((q[1] - lo[1]) / S_)] = True
    outside = np.zeros_like(occ)
    st = [(i, j) for i in range(nx) for j in (0, ny - 1)] + [(i, j) for j in range(ny) for i in (0, nx - 1)]
    while st:
        i, j = st.pop()
        if 0 <= i < nx and 0 <= j < ny and not occ[i, j] and not outside[i, j]:
            outside[i, j] = True
            st += [(i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)]
    hole = ~occ & ~outside
    seen = np.zeros_like(hole)
    knuckle = seg('index_1')[0] * 1000
    found = None
    for i in range(nx):
        for j in range(ny):
            if hole[i, j] and not seen[i, j]:
                comp, st2 = [], [(i, j)]
                seen[i, j] = True
                while st2:
                    p_, q_ = st2.pop()
                    comp.append((p_, q_))
                    for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        r_ = (p_ + d[0], q_ + d[1])
                        if 0 <= r_[0] < nx and 0 <= r_[1] < ny and hole[r_] and not seen[r_]:
                            seen[r_] = True
                            st2.append(r_)
                C = np.array(comp) * S_ + lo
                dist = math.hypot(C[:, 0].mean() - knuckle.x, C[:, 1].mean() - knuckle.y - 20)
                if 30 <= len(comp) <= 2500 and dist < float(one("holedist", 70)) and (found is None or dist < found[1]):
                    found = (C, dist)
    if found:
        C = found[0]
        cx = C[:, 0].min() + 8
        rows_ = C[abs(C[:, 0] - cx) < 3]
        cy = rows_[:, 1].min() + 6
        near_ = Vw[np.argsort(np.hypot(Vw[:, 0] - cx, Vw[:, 1] - cy))[:30]]
        TRIGGER = Vector((cx, cy, (near_[:, 2].min() + near_[:, 2].max()) / 2)) / 1000
        print('GRAB guard opening %.0f mm2; trigger point %s mm' % (len(C), tuple(round(c * 1000) for c in TRIGGER)))
    else:
        # no closed guard (a rifle's open one, a launcher's): the trigger in front of the grip's top, where the index
        # pad meets it
        top_rows = [r for r in g['rows'] if g['top'] + 0.004 < r[0] < g['top'] + 0.016] or g['rows'][-2:]
        r = max(top_rows, key=lambda r: r[2])
        TRIGGER = a * r[0] + u * (r[2] + 0.007) + v * r[6] + move
        print('GRAB no trigger guard opening near the index: the trigger point in front of the grip top %s mm' % str(
            tuple(round(c * 1000) for c in TRIGGER)))

# the hand slides along the grip until the index lines up with the trigger (a real hand does: the trigger's place on
# the gun is fixed, the hand's size is not): the trigger trigx mm (default 6) toward the little finger from the index
# knuckle's line
if TRIGGER is not None and 'slide' in args:  # off by default: the web stays at the grip's top
    knuckle_x = seg('index_1')[0].x
    ds_ = (knuckle_x + float(one('trigx', 6)) / 1000 - TRIGGER.x) / a.x
    ds_ = max(-0.03, min(0.03, ds_))
    w.matrix_world = Matrix.Translation(a * ds_) @ w.matrix_world
    bpy.context.view_layer.update()
    TRIGGER = TRIGGER + a * ds_
    AXIS_POINT = AXIS_POINT + a * ds_
    DELTA = w.matrix_world @ W_LOAD.inverted()
    bm = bmesh.new()
    bm.from_mesh(w.data)
    bm.transform(w.matrix_world)
    WT = BVHTree.FromBMesh(bm)
    print('GRAB slid %.0f mm along the grip (the trigger level with the index); trigger now %s mm' % (
        ds_ * 1000, str(tuple(round(c * 1000) for c in TRIGGER))))

before = None
for f in FINGERS:
    if f == 'index' and TRIGGER is not None:
        def score_fn():
            h, t = seg('index_3')
            return -((h + t) / 2 - TRIGGER).length
    else:
        def score_fn(f=f):
            return round(wrap(DIGITS[f], SENSE) / 3)
    s0 = SWING0[f]
    # the index, beside the frame, may have to swing well down to reach a guard's opening
    res = close(f, before, score_fn, list(range(int(s0) - 30, int(s0) + 13, 4)) if f == 'index' else [s0 - 6, s0, s0 + 6])
    if res is None:
        print('GRAB %s: no pose clear of the weapon (left straight)' % f)
    else:
        pose.update({b: res[0][b] for b in DIGITS[f]})
        apply(pose)
        print('GRAB %s: %s; %s' % (f, ('end segment %.0f mm from the trigger' % (-res[1] * 1000)) if (f == 'index' and TRIGGER is not None)
                                   else 'round the grip %.0f deg' % wrap(DIGITS[f], SENSE),
                                   ' '.join('%s=%s' % (b, pose[b]) for b in DIGITS[f])))
    before = f

# ---- 4. the thumb ----------------------------------------------------------------------------------------------------
# thumbto=x,y,z (mm, the hand frame) or a bone (its head once the fingers are closed): the thumb's tip as near it as it
# reaches, instead of as far round the grip
# thumbto=face: the middle of the grip's left side (a grip too wide for the thumb to reach the fingers)
if one('thumbto') == 'face':
    mid_ = g['rows'][len(g['rows']) // 2]
    THUMB_TO = a * mid_[0] + u * mid_[5] + v * (mid_[3] - 0.006) + move + (DELTA.translation - move) * 0
    THUMB_TO = THUMB_TO + (AXIS_POINT - (a * mid[0] + u * mid[5] + v * mid[6] + move))  # any slide since
elif one('thumbto') and '/' in one('thumbto'):  # thumbto=index_3/middle_3: between those bones' heads (the gap between two fingers)
    THUMB_TO = sum((seg(b)[0] for b in one('thumbto').split('/')), Vector()) / len(one('thumbto').split('/'))
else:
    THUMB_TO = (seg(one('thumbto'))[0] if one('thumbto') in P else vec(one('thumbto'))) if one('thumbto') else None
# the outer joints' sideways bend (z) turns the curl toward the fingers: with plain bends every grip's thumb curled down
# toward the pinky (idren2023 2026-11-01: always toward the index / middle; rolling the base bone instead twisted it);
# poses whose end bone leans more than 0.2 toward the pinky (the hand's x) are left out
HAND_X = (rig.matrix_world.to_3x3() @ rig.data.bones['hand'].matrix_local.to_3x3()).col[0].normalized()
best = None
for x1 in range(-40, 21, 5):
    for sz in (0, -15, -25, -35):
        for z1 in range(-10, 61, 5):
            for a2 in range(0, 61, 10):
                for k in (0.6, 1.0, 1.4):
                    t = dict(pose, thumb_1=(x1, 0, z1), thumb_2=(-a2, 0, sz), thumb_3=(-min(round(k * a2), 80), 0, sz))
                    apply(t)
                    base, tip = seg('thumb_3')
                    if (tip - base).normalized().dot(HAND_X) > 0.2:
                        continue
                    sc = -(tip - THUMB_TO).length if THUMB_TO is not None else round(wrap(['thumb_2', 'thumb_3'], -SENSE) / 3)
                    if best is not None and sc <= best[0]:
                        continue
                    if all(clear_of_weapon(b) for b in ('thumb_2', 'thumb_3')) and clear_of(['thumb_2', 'thumb_3'], DIGITS['index'] + DIGITS['middle']):
                        best = (sc, t)
if best:
    pose.update({b: best[1][b] for b in DIGITS['thumb']})
    apply(pose)
    print('GRAB thumb: %s; %s' % ('round the grip %.0f deg' % wrap(['thumb_2', 'thumb_3'], -SENSE) if THUMB_TO is None else
                                  'tip %.0f mm from the point' % (-best[0] * 1000), ' '.join('%s=%s' % (b, pose[b]) for b in DIGITS['thumb'])))
else:
    print('GRAB thumb: no pose clear of the weapon (left open)')

for sv in opt.get('set', []):  # bones set by hand on top of the search
    b, v = sv.split(':')
    pose[b] = tuple(float(x) for x in v.split(','))
pose = {k: tuple(int(round(x)) for x in v) for k, v in pose.items()}
ORDER = [b for f in FINGERS + ('thumb',) for b in DIGITS[f]]
line = "        ('%s', pose(%s))," % (LABEL, ', '.join('%s=(%d, %d, %d)' % ((k,) + pose.get(k, (0, 0, 0))) for k in ORDER))
print('GRAB line:', line.strip())
if 'write' in args:
    lines = src.split('\n')
    k = next(i for i, l in enumerate(lines) if l.lstrip().startswith("('%s', pose(" % LABEL))
    if lines[k].rstrip().endswith(')),'):
        lines[k] = line
        open(POSES_PY, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
        print('GRAB wrote the pose to tools/body_poses.py')
    else:
        print('GRAB the pose spans lines in tools/body_poses.py: not written')

if 'save' in args:
    for o in bpy.data.objects:
        o.hide_render = False
    if GJ:
        R3 = DELTA.to_3x3()
        GJ['axis_point'] = list(DELTA @ Vector(GJ['axis_point']))
        GJ['axis_direction'] = list((R3 @ Vector(GJ['axis_direction'])).normalized())
        GJ['measured_axis_point'] = list(AXIS_POINT)
        GJ['measured_axis_direction'] = list(AXIS)
        GJ['placement'] = 'grabbed (tools/body_grab.py %s)' % ' '.join(args[1:])
        hb_ = rig.matrix_world @ rig.data.bones['hand'].matrix_local
        GJ['in_hand_bone'] = [list(r) for r in hb_.inverted() @ w.matrix_world]
        json.dump(GJ, open(JSON_PATH, 'w'), indent=1)
    if w.name.startswith('wip_'):
        w.name = 'weap_' + WEAP  # fitted: exported from now on
    for pb in P:
        pb.rotation_euler = (0, 0, 0)
        for c in pb.constraints:
            c.mute = False
    bpy.ops.wm.save_mainfile()
    print('GRAB saved the weapon\'s place')

# ---- render ----------------------------------------------------------------------------------------------------------
if one('render'):
    import numpy as np
    OUT = os.path.abspath(one('render'))
    apply(pose)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'STUDIO'
    TS = int(one('size', 520))
    scene.render.resolution_x = scene.render.resolution_y = TS
    cd = bpy.data.cameras.new('grab')
    cd.type = 'ORTHO'
    cd.ortho_scale = float(one('scale', 0.2))
    cd.clip_end = 3
    cam = bpy.data.objects.new('grab', cd)
    scene.collection.objects.link(cam)
    scene.camera = cam
    centre = Vector((0.0, 0.07, -0.03))

    def shoot(views, out):
        tiles = []
        for k, (name, d, up) in enumerate(views):
            d = Vector(d).normalized()
            up = Vector(up)
            up = (up - d * up.dot(d)).normalized()
            cam.matrix_world = Matrix.Translation(centre + d * 1.0) @ Matrix((d.cross(up) * -1, up, d)).transposed().to_4x4()
            scene.render.filepath = out + '-%d.png' % k
            bpy.ops.render.render(write_still=True)
            im = bpy.data.images.load(scene.render.filepath)
            tiles.append(np.array(im.pixels[:]).reshape(TS, TS, 4))
            bpy.data.images.remove(im)
            os.remove(scene.render.filepath)
        cols = min(4, len(tiles))
        rows_ = (len(tiles) + cols - 1) // cols
        sheet = np.zeros((rows_ * TS, cols * TS, 4))
        sheet[..., 3] = 1
        for i, a_ in enumerate(tiles):
            r_, c_ = divmod(i, cols)
            sheet[(rows_ - 1 - r_) * TS:(rows_ - r_) * TS, c_ * TS:(c_ + 1) * TS] = a_
        im = bpy.data.images.new('sheet', cols * TS, rows_ * TS, alpha=True)
        im.pixels = sheet.ravel()
        im.filepath_raw = out
        im.file_format = 'PNG'
        im.save()
    # the bones and the weapon's edges
    mats = {}
    for nm, col in (('bone', (1, 0.55, 0.1, 1)), ('web', (0.1, 0.9, 1, 1)), ('trig', (1, 0.1, 0.1, 1)),
                    ('wire', (0.85, 0.85, 0.85, 1))):
        m = bpy.data.materials.new(nm)
        m.diffuse_color = col
        mats[nm] = m
    extras = []
    for f in FINGERS + ('thumb',):
        for b in DIGITS[f]:
            h, t = seg(b)
            bpy.ops.mesh.primitive_cylinder_add(radius=0.0016, depth=(t - h).length, location=(h + t) / 2, vertices=8)
            o = bpy.context.object
            o.rotation_euler = (t - h).to_track_quat('Z', 'Y').to_euler()
            o.data.materials.append(mats['bone'])
            extras.append(o)
            bpy.ops.mesh.primitive_uv_sphere_add(radius=0.0028, location=h, segments=10, ring_count=6)
            bpy.context.object.data.materials.append(mats['bone'])
            extras.append(bpy.context.object)
    hb = seg('hand')
    for p_ in (WEB, HEEL) + ((TRIGGER,) if TRIGGER is not None else ()):
        bpy.ops.mesh.primitive_uv_sphere_add(radius=0.0035, location=p_, segments=10, ring_count=6)
        bpy.context.object.data.materials.append(mats['trig' if p_ is TRIGGER else 'web'])
        extras.append(bpy.context.object)
    bpy.ops.mesh.primitive_cylinder_add(radius=0.0012, depth=(HEEL - WEB).length, location=(HEEL + WEB) / 2, vertices=6)
    o = bpy.context.object
    o.rotation_euler = (HEEL - WEB).to_track_quat('Z', 'Y').to_euler()
    o.data.materials.append(mats['web'])
    extras.append(o)
    wire = bpy.data.objects.new('wire', w.data.copy())
    wire.matrix_world = w.matrix_world.copy()
    scene.collection.objects.link(wire)
    mod = wire.modifiers.new('w', 'WIREFRAME')
    mod.thickness = 0.0006 / max(w.matrix_world.to_scale())
    mod.use_replace = True
    wire.data.materials.clear()
    wire.data.materials.append(mats['wire'])
    for o in scene.objects:
        o.hide_render = o not in extras and o is not wire and o is not cam
    scene.display.shading.color_type = 'MATERIAL'
    BONE_VIEWS = (('side (gun right side)', (0, 0, 1), (-1, 0, 0)),
                  ('side (gun left side)', (0, 0, -1), (-1, 0, 0)),
                  ('front', (0, 1, -0.15), (-1, 0, 0)),
                  ('from above', (-1, 0.1, -0.2), (0, 1, 0)),
                  ('from below', (1, 0.1, -0.2), (0, 1, 0)),
                  ('behind', (-0.3, -1, 0.3), (-1, 0, 0)))
    if one('bviews'):
        BONE_VIEWS = [BONE_VIEWS[int(i)] for i in one('bviews').split(',')]
    shoot(BONE_VIEWS, OUT + '-bones.png')
    if 'boneonly' in args:
        sys.exit(0)
    for o in extras + [wire]:
        o.hide_render = True
    hand.hide_render = False
    w.hide_render = False
    scene.display.shading.color_type = 'TEXTURE'
    shoot((('back of the hand', (0.15, 0.1, 1), (-1, 0, 0)),
           ('gun left side', (0.15, 0.1, -1), (-1, 0, 0)),
           ('front', (0.1, 1, -0.25), (-1, 0, 0)),
           ('player view', (-0.5, -1, 0.35), (-1, 0, 0)),
           ('from above', (-1, 0.15, -0.3), (0, 1, 0)),
           ('from below', (1, 0.2, -0.3), (0, 1, 0)),
           ('left front 3/4', (-0.35, 0.8, -0.8), (-1, 0, 0)),
           ('right rear 3/4', (-0.3, -0.6, 0.9), (-1, 0, 0))), OUT + '-hand.png')
    print('GRAB render %s-bones.png, %s-hand.png' % (OUT, OUT))

