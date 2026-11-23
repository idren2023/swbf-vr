"""Blender (blender -b --factory-startup re/body/r_arm_grips.blend -P tools/body_grasp.py -- <weapon> [pose=<label>]
[close=middle,ring,pinky] [thumb] [trigger=x,y,z | trigger=hole] [move=x,y,z] [turn=x,y,z] [set=<bone>:x,y,z ..]
[render=<out.png>] [xray] [write] [save]): a hand closed round one weapon's own grip as a real hand grabs it
(idren2023 2026-10-31: every weapon its own grip pose, the weapon models are all different).

- move= / turn=: the weapon moved (mm) and turned (deg, about the grip's measured axis point) in the hand frame first
  (x toward the little finger, y toward the knuckles, z out of the back of the hand).
- close=: each finger closed as a hand closes on a handle: its three joints turn together (knuckle : middle : end joint
  1 : 1.1 : 0.8, 1 degree steps, from nearly straight); a joint stops when the segment it moves (or one beyond it)
  touches the weapon's surface or the neighbouring finger, the joints beyond it keep turning, so the finger wraps the
  grip's own shape. Its sideways swing (the knuckle's z) is searched (-20..20 deg round the start pose's) for the
  finger that wraps furthest without lying on its neighbour.
- trigger=: the index's last segment onto the trigger: x,y,z mm in the weapon's mesh coordinates (its place in the
  stock hold), or `hole`: the trigger guard's opening found in the weapon's side view, the rear of it (the trigger's
  face) at the gun's middle. The index's knuckle (curl and swing) and two outer joints are searched for the end
  segment nearest that point with the first two segments crossing nothing of the weapon.
- thumb: the thumb's base turned (searched) and its two segments closed the same way onto the weapon or the fingers;
  the thumb that wraps furthest round the grip toward the middle finger, crossing nothing.
- set=: a bone's angles set by hand (deg), after the fits.
- render=: 8 views of the hand on the weapon (back of the hand, gun's left side, front, from below, from above, the
  player's view from behind, two three-quarter views), close up, 2 rows; xray: the weapon see-through.
- write: the pose's line in tools/body_poses.py replaced; save: the weapon's place saved in the .blend (the grip json's
  measured axis moved with it; its axis_point, where the controller sits in the hand, is kept).
Prints each digit's crossings (the hand's triangles crossing the weapon's) and the pose line."""
import bpy, json, math, os, re, sys
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree
import bmesh

args = sys.argv[sys.argv.index('--') + 1:]
WEAP = args[0]
opt = {}
for a in args[1:]:
    if '=' in a:
        k, v = a.split('=', 1)
        opt.setdefault(k, []).append(v)
one = lambda k, d=None: opt.get(k, [d])[-1]
TOOLS = os.path.dirname(os.path.abspath(__file__))
POSES_PY = os.path.join(TOOLS, 'body_poses.py')
src = open(POSES_PY, encoding='utf-8').read()
ns = {}
exec(src[src.index('def wrap('):src.index('scene = bpy.context.scene')], dict(ns, FINGERS=('index', 'middle', 'ring', 'pinky')), ns)
LABEL = one('pose') or re.search(r"'%s': '([^']+)'" % WEAP, src).group(1)
POSE = {k: tuple(v) for k, v in next(p for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(LABEL)).items()}
LABEL = next(l for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(LABEL))

hand = bpy.data.objects['r_hand_retopo']
rig = hand.parent
P = rig.pose.bones
for pb in P:
    for c in pb.constraints:
        c.mute = True
w = bpy.data.objects.get('weap_' + WEAP) or bpy.data.objects['wip_' + WEAP]  # wip_: not exported yet
JSON_PATH = os.path.join('re', 'body', 'grip-weap_%s.json' % WEAP)
GJ = json.load(open(JSON_PATH)) if os.path.exists(JSON_PATH) else None
AXIS0 = Vector(GJ.get('measured_axis_point', GJ['axis_point'])) if GJ else w.matrix_world.translation.copy()
AXIS_DIR = Vector(GJ.get('measured_axis_direction', GJ['axis_direction'])).normalized() if GJ else Vector((1, 0, 0))
W_LOAD = w.matrix_world.copy()

# ---- the weapon moved / turned in the hand ---------------------------------------------------------------------------
mv = Vector([float(x) / 1000 for x in one('move', '0,0,0').split(',')])
tx, ty, tz = (math.radians(float(x)) for x in one('turn', '0,0,0').split(','))
R = (Matrix.Rotation(tz, 4, 'Z') @ Matrix.Rotation(ty, 4, 'Y') @ Matrix.Rotation(tx, 4, 'X'))
w.matrix_world = Matrix.Translation(AXIS0 + mv) @ R @ Matrix.Translation(-AXIS0) @ W_LOAD
AXIS_NOW = AXIS0 + mv
AXIS_DIR_NOW = (R.to_3x3() @ AXIS_DIR).normalized()
bpy.context.view_layer.update()

# ---- the hand's segments ---------------------------------------------------------------------------------------------
names = {g.index: g.name for g in hand.vertex_groups}
DIGITS = {f: ['%s_%d' % (f, k) for k in (1, 2, 3)] for f in ('index', 'middle', 'ring', 'pinky', 'thumb')}
BONE_OF = {}
for v in hand.data.vertices:
    best = max(v.groups, key=lambda g: g.weight, default=None)
    BONE_OF[v.index] = names[best.group] if best else 'hand'
hand.data.calc_loop_triangles()
TRIS = [tuple(t.vertices) for t in hand.data.loop_triangles]
# a triangle belongs to the bone most of its corners follow
from collections import Counter
TRI_BONE = [Counter(BONE_OF[i] for i in t).most_common(1)[0][0] for t in TRIS]
TRIS_OF = {}
for t, b in zip(TRIS, TRI_BONE):
    TRIS_OF.setdefault(b, []).append(t)
# a finger's root (the first third of its first segment, the knuckle in the palm) is left out of the closing's touch
# test: the grip pressed into the palm reaches it at any curl
ROOT_FREE = {}
_rest = [hand.matrix_world @ v.co for v in hand.data.vertices]
for b, ts in TRIS_OF.items():
    if b in rig.data.bones and ((b.endswith('_1') and not b.startswith('thumb')) or b == 'thumb_2'):
        bn = rig.data.bones[b]
        h_, t_ = rig.matrix_world @ bn.head_local, rig.matrix_world @ bn.tail_local
        ax_ = (t_ - h_).normalized()
        ROOT_FREE[b] = [t for t in ts if (sum((_rest[i] for i in t), Vector()) / 3 - h_).dot(ax_) > 0.35 * (t_ - h_).length]
    else:
        ROOT_FREE[b] = ts


def weapon_tree():
    b = bmesh.new()
    b.from_mesh(w.data)
    b.transform(w.matrix_world)
    return BVHTree.FromBMesh(b)


WT = weapon_tree()


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
    me = hand.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
    mw = hand.matrix_world
    return [mw @ v.co for v in me.vertices]


def tree_of(pts, bones):
    tris = [t for b in bones for t in TRIS_OF.get(b, [])]
    return BVHTree.FromPolygons(pts, tris) if tris else None


def hits(pts, bone, others=(), free=False):
    """How many of the bone's triangles cross the weapon's (and the other trees given: neighbouring fingers); free:
    without a finger's root."""
    tris = (ROOT_FREE if free else TRIS_OF).get(bone, [])
    t = BVHTree.FromPolygons(pts, tris) if tris else None
    if t is None:
        return 0
    n = len({a for a, _ in t.overlap(WT)})
    for o in others:
        if o is not None:
            n += len({a for a, _ in t.overlap(o)})
    return n


def tip(pts, bone):
    pb = P[bone]
    return rig.matrix_world @ pb.tail


def wrap_angle(f):
    """How far round the grip's axis the finger reaches (radians, the knuckle to the tip, signed so a curl counts up)."""
    chain = [rig.matrix_world @ P[DIGITS[f][0]].head] + [rig.matrix_world @ P[b].tail for b in DIGITS[f]]
    flat = []
    for q in chain:
        q = q - AXIS_NOW
        flat.append(q - AXIS_DIR_NOW * q.dot(AXIS_DIR_NOW))
    total = 0.0
    for u_, v_ in zip(flat, flat[1:]):
        if u_.length < 1e-6 or v_.length < 1e-6:
            continue
        a_ = u_.angle(v_, 0.0)
        total += a_ if AXIS_DIR_NOW.dot(u_.cross(v_)) >= 0 else -a_
    return total


pose = dict(POSE)
LIMITS = {'1': 95, '2': 110, '3': 85}
RATIO = tuple(float(x) for x in one('ratio', '0.6,1.2,0.9').split(','))
SQUEEZE = float(one('squeeze', 4))  # degrees past the touch (the pads press in)


def close_finger(f, swing, others, start=(-10, 0, 0), fixed=0):
    """GraspIt-style closing of finger f (joints from `fixed` on), returns (angles, touched segments, start hits)."""
    bones = DIGITS[f]
    ang = [start[0], start[1], start[2]]
    base = {b: pose.get(b, (0, 0, 0)) for b in bones}
    for k in range(fixed):
        ang[k] = -base[bones[k]][0]

    def trial():
        t = dict(pose)
        for k, b in enumerate(bones):
            x_, y_, z_ = base[b]
            t[b] = (-ang[k], y_ if k else 0, swing if k == 0 else 0)
            if k == 0 and f == 'thumb':
                t[b] = base[b]
        return t
    pts = apply(trial())
    start_hits = [hits(pts, b, others, True) for b in bones]
    # a segment already crossing at the start (the grip pressed into the palm reaches the finger's root) stops when it
    # crosses more than that
    active = [k >= fixed for k in range(3)]
    touched = [False, False, False]
    for _ in range(400):
        if not any(active):
            break
        for k in range(3):
            if not active[k]:
                continue
            ang[k] += RATIO[k]
            if ang[k] > LIMITS[str(k + 1)]:
                ang[k] -= RATIO[k]
                active[k] = False
                continue
            pts = apply(trial())
            hit = [hits(pts, b, others, True) > start_hits[k + i] + 2 for i, b in enumerate(bones[k:])]
            if any(hit):
                ang[k] -= RATIO[k]
                active[k] = False
                touched[k + next(i for i, h in enumerate(hit) if h)] = True
    # the squeeze: the joints that stopped on the weapon turned a little further, the pads pressing in
    for k in range(fixed, 3):
        if touched[k] or (k < 2 and any(touched[k + 1:])):
            ang[k] = min(ang[k] + SQUEEZE * RATIO[k], LIMITS[str(k + 1)])
    pts = apply(trial())
    return trial(), touched, start_hits


def finger_pose(f, a1, a2, a3, swing):
    t = dict(pose)
    t[f + '_1'] = (-a1, 0, swing)
    t[f + '_2'] = (-a2, pose.get(f + '_2', (0, 0, 0))[1], 0)
    t[f + '_3'] = (-a3, 0, 0)
    return t


def clean(pts, f, others, allow=(2, 2, 2)):
    return all(hits(pts, b, others, True) <= allow[k] for k, b in enumerate(DIGITS[f]))


def close_static(f, swing, others):
    """Finger f (knuckle a1, middle joint a2, end joint k * a2 as a finger bends) as far round the grip's axis as it
    reaches with its segments crossing nothing (2 triangles of press allowed each): wrapped round the grip, not
    hooked through a guard in front of it."""
    best = None

    def score(a1, a2, a3):
        pts = apply(finger_pose(f, a1, a2, a3, swing))
        if not clean(pts, f, others):
            return None
        return (round(-math.degrees(wrap_angle(f)) / 3), a1 + a2 + a3)
    for k in (0.55, 0.7, 0.85):
        for a2 in range(0, 106, 6):
            for a1 in range(-10, 91, 6):
                a3 = min(round(k * a2), 80)
                sc = score(a1, a2, a3)
                if sc is not None and (best is None or sc > best[0]):
                    best = (sc, a1, a2, a3)
    if best is None:
        return None
    _, a1, a2, a3 = best
    for _ in range(2):  # refined in 2 degree steps
        for d1 in (-4, -2, 0, 2, 4):
            for d2 in (-4, -2, 0, 2, 4):
                for d3 in (-4, 0, 4):
                    b1, b2, b3 = a1 + d1, min(a2 + d2, 110), min(max(a3 + d3, 0), 85)
                    sc = score(b1, b2, b3)
                    if sc is not None and sc > best[0]:
                        best = (sc, b1, b2, b3)
        _, a1, a2, a3 = best
    return finger_pose(f, a1, a2, a3, swing)


# the palm proper (the hand's triangles short of the knuckles): a closing finger must not sink into it
PALM_TRIS = [t for t in TRIS_OF.get('hand', []) if (sum((_rest[i] for i in t), Vector()) / 3).y < 0.075]


def neighbours(f, pts):
    """What finger f must not cross besides the weapon: the palm, and the finger closed before it (the index before
    the middle finger, and so on; the thumb: the index and middle finger)."""
    palm = BVHTree.FromPolygons(pts, PALM_TRIS)
    if 'nonb' in args:
        return [palm]
    order = ['index', 'middle', 'ring', 'pinky']
    if f == 'thumb':
        return [palm, tree_of(pts, DIGITS['index'] + DIGITS['middle'])]
    i = order.index(f)
    return [palm] + ([tree_of(pts, DIGITS[order[i - 1]])] if i else [])


def depth(pts, bones):
    """How deep the weapon reaches into the given bones' vertices (mm; the nearest face's normal tells inside)."""
    deep = 0.0
    for i, b in BONE_OF.items():
        if b in bones:
            loc, nrm, _, dist = WT.find_nearest(pts[i], 0.03)
            if loc is not None and (pts[i] - loc).dot(nrm) < 0:
                deep = max(deep, dist)
    return deep * 1000


def report(tag):
    pts = apply(pose)
    out = []
    for f in ('index', 'middle', 'ring', 'pinky', 'thumb'):
        out.append('%s %s' % (f, '/'.join(str(hits(pts, b)) for b in DIGITS[f] if b in P)))
    out.append('palm %d' % hits(pts, 'hand'))
    print('GRASP %-6s crossings (segment 1/2/3): %s' % (tag, ', '.join(out)))


report('start')

# ---- the index on the trigger ---------------------------------------------------------------------------------------
def guard_hole():
    import numpy as np
    S = 1.0
    V = np.array([w.matrix_world @ v.co for v in w.data.vertices]) * 1000  # the hand frame (meshes differ in units)
    lo = V[:, :2].min(0) - 3
    hi = V[:, :2].max(0) + 3
    nx, ny = int((hi - lo)[0] / S) + 1, int((hi - lo)[1] / S) + 1
    occ = np.zeros((nx, ny), bool)
    w.data.calc_loop_triangles()
    for t in w.data.loop_triangles:
        a, b, c = V[list(t.vertices)][:, :2]
        n = int(max(np.linalg.norm(b - a), np.linalg.norm(c - a)) / S * 1.5) + 2
        for i in range(n + 1):
            for j in range(n + 1 - i):
                p = a + (b - a) * i / n + (c - a) * j / n
                occ[int((p[0] - lo[0]) / S), int((p[1] - lo[1]) / S)] = True
    out = np.zeros_like(occ)
    stack = [(i, j) for i in range(nx) for j in (0, ny - 1)] + [(i, j) for j in range(ny) for i in (0, nx - 1)]
    while stack:
        i, j = stack.pop()
        if 0 <= i < nx and 0 <= j < ny and not occ[i, j] and not out[i, j]:
            out[i, j] = True
            stack += [(i + 1, j), (i - 1, j), (i, j + 1), (i, j - 1)]
    hole = ~occ & ~out
    seen = np.zeros_like(hole)
    best = None
    for i in range(nx):
        for j in range(ny):
            if hole[i, j] and not seen[i, j]:
                comp, st = [], [(i, j)]
                seen[i, j] = True
                while st:
                    a_, b_ = st.pop()
                    comp.append((a_, b_))
                    for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        q = (a_ + d[0], b_ + d[1])
                        if 0 <= q[0] < nx and 0 <= q[1] < ny and hole[q] and not seen[q]:
                            seen[q] = True
                            st.append(q)
                # the opening nearest the index's knuckle (a scope ring or a stock's hole elsewhere is not it)
                Cc = np.array(comp) * S + lo
                dist = np.hypot(Cc[:, 0].mean() + 22, Cc[:, 1].mean() - 110)
                if 30 <= len(comp) * S * S <= 2500 and dist < 45 and (best is None or dist < best[1]):
                    best = (comp, dist)
    best = best[0] if best else None
    if not best or len(best) * S * S < 30:
        return None
    C = np.array(best) * S + lo
    # the trigger hangs from the frame: near the top of the opening (toward -x), at its rear (toward -y, the trigger's
    # face), a finger's half thickness in from both
    cx = C[:, 0].min() + 8
    rows = C[abs(C[:, 0] - cx) < 3]
    cy = rows[:, 1].min() + 6
    near = V[np.argsort(np.hypot(V[:, 0] - cx, V[:, 1] - cy))[:30]]
    cz = (near[:, 2].min() + near[:, 2].max()) / 2
    print('GRASP guard opening %.0f mm2, x %.0f..%.0f y %.0f..%.0f; trigger point %.0f %.0f %.0f (hand frame mm)' % (
        len(best) * S * S, C[:, 0].min(), C[:, 0].max(), C[:, 1].min(), C[:, 1].max(), cx, cy, cz))
    return Vector((cx, cy, cz)) / 1000


TRIGGER = None
if one('trigger'):
    if one('trigger') == 'hole':
        TRIGGER = guard_hole()
    else:
        TRIGGER = w.matrix_world @ Vector([float(x) / 1000 for x in one('trigger').split(',')])
# the fingers start straight (their old pose would stand in the way of the one closed before them)
for f in ('index', 'middle', 'ring', 'pinky'):
    if f in (one('close') or '') or (f == 'index' and TRIGGER is not None):
        for b in DIGITS[f]:
            pose[b] = (0, 0, 0)
if TRIGGER is not None:
    # the index's end segment as near the trigger point as it gets, its first two segments crossing nothing (the end
    # segment may press on the trigger), the middle finger's old pose not counted (it closes after)
    best = None
    for k in (0.5, 0.7, 0.9):
        for a2 in range(0, 101, 5):
            for c1 in range(-6, 51, 4):
                for s1 in range(-16, 13, 4):
                    t = finger_pose('index', c1, a2, min(round(k * a2), 80), s1)
                    apply(t)
                    pb = P['index_3']
                    d = ((rig.matrix_world @ ((pb.head + pb.tail) / 2)) - TRIGGER).length
                    if best is not None and d >= best[0]:
                        continue
                    pts = apply(t)
                    if clean(pts, 'index', [BVHTree.FromPolygons(pts, PALM_TRIS)], allow=(2, 2, 8)):
                        best = (d, t)
    if best:
        pose.update({b: best[1][b] for b in DIGITS['index']})
        pts = apply(pose)
        print('GRASP index on the trigger: end segment %.1f mm from it, crossings %s; %s' % (
            best[0] * 1000, [hits(pts, b) for b in DIGITS['index']], ' '.join('%s=%s' % (b, pose[b]) for b in DIGITS['index'])))
    else:
        print('GRASP index: no pose reaches the trigger without crossing')

# ---- the fingers closed round the grip ------------------------------------------------------------------------------
for f in (one('close').split(',') if one('close') else []):
    # the knuckle's sideways swing: as a fist closes (middle straight, ring and little finger toward it; the Rebel
    # grips' poses), searched a little round it; swing=<finger>:<deg> sets it
    s0 = {'index': -4, 'middle': 0, 'ring': 8, 'pinky': 15}[f]
    for sv in opt.get('swing', []):
        if sv.split(':')[0] == f:
            s0 = float(sv.split(':')[1])
    results = []
    for ds in ((0,) if any(sv.split(':')[0] == f for sv in opt.get('swing', [])) else range(-int(one('swingrange', 8)), int(one('swingrange', 8)) + 1, 4)):
        pts = apply(pose)
        res = close_static(f, s0 + ds, neighbours(f, pts))
        if res is None:
            continue
        apply(res)
        touched, start_hits = [], [0]
        results.append((True, -wrap_angle(f), -abs(ds), ds, res, touched, start_hits))
    if not results:
        print('GRASP %s: crosses the weapon at every curl (left as posed)' % f)
        continue
    results.sort(key=lambda r: (r[0] >= 2, r[1], r[2]), reverse=True)
    _, wr, _, ds, res, touched, start_hits = results[0]
    pose.update({b: res[b] for b in DIGITS[f]})
    print('GRASP %s closed: swing %+d, wraps %.0f deg round the grip, touching %s%s; %s' % (
        f, ds, math.degrees(wr), touched, (' (crossing at the start: %s)' % start_hits) if any(start_hits) else '',
        ' '.join('%s=%s' % (b, res[b]) for b in DIGITS[f])))

# ---- the thumb -------------------------------------------------------------------------------------------------------
if 'thumb' in args:
    # the thumb's base (thumb_1: the ball of the thumb) turned and its two joints bent (the end joint k times the
    # middle one), crossing nothing of the weapon or the fingers (thumb_1 belongs to the palm: let be), the tip as near
    # the index finger's end joint as it reaches (wrapped round the grip's other side, over the fingers)
    t1 = pose.get('thumb_1', (0, 0, 0))
    pts = apply(pose)
    obst = [tree_of(pts, DIGITS['index'] + DIGITS['middle'])]
    # thumbto=far: the tip as far round to the gun's left side as it reaches (a grip too wide to reach a finger)
    FAR = one('thumbto') == 'far'
    # thumbto=x,y,z: a point in the hand frame (mm), e.g. on a wide grip's left side
    if not FAR and ',' in one('thumbto', ''):
        target = Vector([float(v) / 1000 for v in one('thumbto').split(',')])
    else:
        target = None if FAR else rig.matrix_world @ P[one('thumbto', 'index_3')].head
    best = None
    for x1 in range(-40, 21, 5):
        for z1 in range(-10, 61, 5):
            for a2 in range(0, 61, 10):
                for k in (0.6, 1.0, 1.4):
                    t = dict(pose, thumb_1=(x1, t1[1], z1), thumb_2=(-a2, 0, 0), thumb_3=(-min(round(k * a2), 80), 0, 0))
                    apply(t)
                    tip_ = rig.matrix_world @ P['thumb_3'].tail
                    d = tip_.z + 0.1 if FAR else (tip_ - target).length
                    if best is not None and d >= best[0]:
                        continue
                    pts = apply(t)
                    if all(hits(pts, b, obst, True) <= 2 for b in ('thumb_2', 'thumb_3')):
                        best = (d, t)
    if best:
        pose.update({b: best[1][b] for b in DIGITS['thumb']})
        print('GRASP thumb: %s %.0f mm; %s' % ('tip at z' if FAR else 'tip from the %s joint' % one('thumbto', 'index_3'), (best[0] - (0.1 if FAR else 0)) * 1000,
              ' '.join('%s=%s' % (b, pose[b]) for b in DIGITS['thumb'])))
    else:
        print('GRASP thumb: crosses at every pose (left as posed)')

for s in opt.get('set', []):
    b, v = s.split(':')
    pose[b] = tuple(float(x) for x in v.split(','))

pose = {k: tuple(int(round(x)) for x in v) for k, v in pose.items()}
report('fitted')
ORDER = ['index_1', 'index_2', 'index_3', 'middle_1', 'middle_2', 'middle_3', 'ring_1', 'ring_2', 'ring_3',
         'pinky_1', 'pinky_2', 'pinky_3', 'thumb_1', 'thumb_2', 'thumb_3']
line = "        ('%s', pose(%s))," % (LABEL, ', '.join('%s=(%d, %d, %d)' % ((k,) + pose[k]) for k in ORDER if k in pose))
print('GRASP line:', line.strip())
if 'write' in args:
    lines = src.split('\n')
    k = next(i for i, l in enumerate(lines) if l.lstrip().startswith("('%s', pose(" % LABEL))
    if lines[k].rstrip().endswith(')),'):
        lines[k] = line
        open(POSES_PY, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
        print('GRASP wrote the pose to tools/body_poses.py')
    else:
        print('GRASP the pose spans lines in tools/body_poses.py: not written')

# ---- render ----------------------------------------------------------------------------------------------------------
if one('render'):
    OUT = os.path.abspath(one('render'))
    apply(pose)
    scene = bpy.context.scene
    for o in scene.objects:
        o.hide_render = o not in (hand, w)
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.color_type = 'TEXTURE'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.show_cavity = True
    scene.display.shading.show_object_outline = True
    if 'xray' in args:  # the weapon as its edges: the fingers and thumb behind it seen
        w.display_type = 'WIRE'
    TS = int(one('size', 560))
    scene.render.resolution_x = scene.render.resolution_y = TS
    cd = bpy.data.cameras.new('grasp')
    cd.type = 'ORTHO'
    cam = bpy.data.objects.new('grasp', cd)
    scene.collection.objects.link(cam)
    scene.camera = cam
    pts = apply(pose)
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    centre = (lo + hi) / 2 + Vector((0, 0.01, -0.015))
    cd.ortho_scale = float(one('scale', 0.15))
    # the hand frame: x toward the little finger (down the grip), y toward the knuckles (the gun's front), z out of the
    # back of the hand (the gun's right side); the weapon's barrel about +y, its top -x
    VIEWS = (('back of the hand (gun right side)', (0.15, 0.1, 1), (-1, 0, 0)),
             ('gun left side (thumb, fingertips)', (0.15, 0.1, -1), (-1, 0, 0)),
             ('front (fingers round the grip)', (0.1, 1, -0.25), (-1, 0, 0)),
             ('player view (behind, above)', (-0.5, -1, 0.35), (-1, 0, 0)),
             ('from above (index, thumb)', (-1, 0.15, -0.3), (0, 1, 0)),
             ('from below (little finger)', (1, 0.2, -0.3), (0, 1, 0)),
             ('left front 3/4', (-0.35, 0.8, -0.8), (-1, 0, 0)),
             ('right rear 3/4', (-0.3, -0.6, 0.9), (-1, 0, 0)))
    if TRIGGER is not None:
        bpy.ops.mesh.primitive_uv_sphere_add(radius=0.002, location=TRIGGER)
        mk = bpy.context.object
        mk.color = (1, 0, 0, 1)
        scene.display.shading.color_type = 'TEXTURE'
    only = one('views')
    if only:
        VIEWS = [VIEWS[int(i)] for i in only.split(',')]
    tiles = []
    for k, (name, d, up) in enumerate(VIEWS):
        d = Vector(d).normalized()
        up = Vector(up)
        up = (up - d * up.dot(d)).normalized()
        cam.matrix_world = Matrix.Translation(centre + d * 1.0) @ Matrix((d.cross(up) * -1, up, d)).transposed().to_4x4()
        cd.clip_end = 3
        scene.render.filepath = OUT + '-%d.png' % k
        bpy.ops.render.render(write_still=True)
        tiles.append(scene.render.filepath)
    # the sheet: 4 a row
    import numpy as np
    imgs = []
    for p_ in tiles:
        im = bpy.data.images.load(p_)
        a = np.array(im.pixels[:]).reshape(TS, TS, 4)
        imgs.append(a)
        bpy.data.images.remove(im)
    cols = min(4, len(imgs))
    rows = (len(imgs) + cols - 1) // cols
    sheet = np.zeros((rows * TS, cols * TS, 4))
    sheet[..., 3] = 1
    for i, a in enumerate(imgs):
        r_, c_ = divmod(i, cols)
        sheet[(rows - 1 - r_) * TS:(rows - r_) * TS, c_ * TS:(c_ + 1) * TS] = a
    out_im = bpy.data.images.new('sheet', cols * TS, rows * TS, alpha=True)
    out_im.pixels = sheet.ravel()
    out_im.filepath_raw = OUT
    out_im.file_format = 'PNG'
    out_im.save()
    for p_ in tiles:
        os.remove(p_)
    print('GRASP render %s (%s)' % (OUT, ', '.join('%d %s' % (i, v[0]) for i, v in enumerate(VIEWS))))

if 'save' in args:
    for o in bpy.data.objects:
        o.hide_render = False
    if GJ:
        # the grip's own axis follows the weapon; axis_point / axis_direction (where the controller sits in the hand)
        # stay
        GJ['measured_axis_point'] = list(AXIS_NOW)
        GJ['measured_axis_direction'] = list(AXIS_DIR_NOW)
        hb = rig.matrix_world @ rig.data.bones['hand'].matrix_local
        GJ['in_hand_bone'] = [list(r) for r in hb.inverted() @ w.matrix_world]
        json.dump(GJ, open(JSON_PATH, 'w'), indent=1)
    for pb in P:
        pb.rotation_euler = (0, 0, 0)
        for c in pb.constraints:
            c.mute = False
    bpy.ops.wm.save_mainfile()
    print('GRASP saved the weapon\'s place (moved %s mm, turned %s deg)' % (one('move', '0,0,0'), one('turn', '0,0,0')))
