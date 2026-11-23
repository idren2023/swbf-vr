"""Blender (blender -b --factory-startup re/body/r_arm_grips.blend -P tools/body_hold.py -- <item> [place=x,y,z]
[rot=x:deg,z:deg ..] [thumbto=x,y,z] [press=mm] [open=<finger>,..] [minmid=k] [curl=k] [save] [write]): an item without a handle (a
grenade, a mine, a canister, the recon droid; 2026-11-01) held in the hand: tools/body_grab.py's way for handles, made
simple for round and flat things:

1. the item placed: its bounding box's middle at `place` (mm, the hand frame: x toward the little finger, y toward the
   knuckles, z the back of the hand; the palm's plane is z = -24), turned by `rot` (degrees about the hand's axes, in
   order, from the place tools/body_weapons.py put it: kept in the object's `import_matrix`);
2. each finger closed by its bones until it touches: each bone a capsule of the finger's own thickness (measured from
   the hand mesh) kept off the item (`press` mm into it allowed, default 1.5) and out of its inside; of the clear poses
   (knuckle, middle joint, end joint 0.7 times the middle one) the one lying closest along the surface; `minmid=k`:
   the middle joint bent at least k times the knuckle (a fist round a ball, not a finger hinged straight at the
   knuckle); `curl=k`: one curl instead, the knuckle k times the middle joint, the most closed clear one; `open=` fingers keep the pose line's;
3. the thumb (its base bent and swung, its two outer joints bent and turned sideways toward the fingers, as b465) to the
   clear pose whose tip is nearest `thumbto` (mm, the hand frame).

The controller's grip point in the hand (axis_point, axis_direction) is the rifle's (re/body/grip-weap_rifle.json): the
hand sits on the controller as with the rifle. save: the item's place in the .blend and re/body/grip-weap_<item>.json;
write: the pose's line in tools/body_poses.py (its label from WEAPON_POSES)."""
import bpy, json, math, os, re, sys
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

args = sys.argv[sys.argv.index('--') + 1:]
ITEM = args[0]
opt = dict(a.split('=', 1) for a in args[1:] if '=' in a)
vec = lambda s: Vector([float(x) / 1000 for x in s.split(',')])
TOOLS = os.path.dirname(os.path.abspath(__file__))
POSES_PY = os.path.join(TOOLS, 'body_poses.py')
src = open(POSES_PY, encoding='utf-8').read()
LABEL = re.search(r"'%s': '([^']+)'" % ITEM, src).group(1)
line = next(l for l in src.split('\n') if l.strip().startswith("('%s'" % LABEL))
start = {b: tuple(float(x) for x in v.split(',')) for b, v in re.findall(r'(\w+_\d)=\(([^)]*)\)', line)}

hand = bpy.data.objects['r_hand_retopo']
rig = hand.parent
P = rig.pose.bones
for pb in P:
    for c in pb.constraints:
        c.mute = True
w = bpy.data.objects['weap_' + ITEM]
HB = rig.matrix_world @ rig.data.bones['hand'].matrix_local  # the hand frame (the hand bone's rest)
FINGERS = ('index', 'middle', 'ring', 'pinky')
PRESS = float(opt.get('press', 1.5)) / 1000
MIN_MID = float(opt.get('minmid', 0))
CURL = float(opt['curl']) if 'curl' in opt else None


def apply(p):
    for pb in P:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
    for n, d in p.items():
        if n in P:
            P[n].rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
            P[n].rotation_euler = [math.radians(x) for x in d]
    bpy.context.view_layer.update()


def seg(b):
    return rig.matrix_world @ P[b].head, rig.matrix_world @ P[b].tail


def in_hand(q):
    return HB.inverted() @ q


# ---- each bone's thickness: the median distance of its own vertices (weighted mostly to it) from the bone --------
apply({})
names = {g.index: g.name for g in hand.vertex_groups}
RADIUS = {}
for f in FINGERS + ('thumb',):
    for k in (1, 2, 3):
        b = '%s_%d' % (f, k)
        h, t = seg(b)
        ds = []
        for v in hand.data.vertices:
            if sum(g.weight for g in v.groups if names.get(g.group) == b) < 0.5:
                continue
            q = hand.matrix_world @ v.co
            s_ = max(0.0, min(1.0, (q - h).dot(t - h) / (t - h).length_squared))
            ds.append((q - (h + (t - h) * s_)).length)
        RADIUS[b] = sorted(ds)[len(ds) // 2] if ds else 0.008
print('HOLD radii mm: ' + ' '.join('%s %.1f' % (b, r * 1000) for b, r in RADIUS.items()))
print('HOLD hand points mm: ' + '; '.join('%s %s' % (b, tuple(round(c * 1000) for c in in_hand(seg(b)[0])))
                                          for b in ('index_1', 'middle_1', 'pinky_1', 'thumb_1', 'thumb_2', 'thumb_3')))

# ---- 1. the item placed ------------------------------------------------------------------------------------------
if 'import_matrix' not in w:
    w['import_matrix'] = [list(r) for r in w.matrix_world]
M0 = Matrix(w['import_matrix'])
w.matrix_world = M0
bpy.context.view_layer.update()
V = [HB.inverted() @ (w.matrix_world @ v.co) for v in w.data.vertices]
centre = Vector([(min(v[i] for v in V) + max(v[i] for v in V)) / 2 for i in range(3)])
R = Matrix()
for part in [p for p in opt.get('rot', '').split(',') if p]:
    ax, deg = part.split(':')
    R = Matrix.Rotation(math.radians(float(deg)), 4, ax.upper()) @ R
target = vec(opt['place']) if 'place' in opt else centre
in_frame = Matrix.Translation(target) @ R @ Matrix.Translation(-centre)  # in the hand frame
w.matrix_world = HB @ in_frame @ HB.inverted() @ M0
bpy.context.view_layer.update()
V = [w.matrix_world @ v.co for v in w.data.vertices]
VH = [in_hand(v) for v in V]
print('HOLD item %s: in the hand frame x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f mm' % (
    ITEM, *(c * 1000 for a in range(3) for c in (min(v[a] for v in VH), max(v[a] for v in VH)))))
w.data.calc_loop_triangles()
TREE = BVHTree.FromPolygons(V, [tuple(t.vertices) for t in w.data.loop_triangles])


def inside(q):
    """Odd number of crossings along +z (the mesh may be open: the nearest face's normal as a tie-break)."""
    n, o = 0, q.copy()
    for _ in range(64):
        hit = TREE.ray_cast(o, Vector((0, 0, 1)))
        if hit[0] is None:
            break
        n += 1
        o = hit[0] + Vector((0, 0, 1e-5))
    if n % 2:
        return True
    loc, nrm, _, d = TREE.find_nearest(q)
    return loc is not None and d < 0.004 and (q - loc).dot(nrm) < 0


def clear(b):
    h, t = seg(b)
    for k in range(6):
        q = h.lerp(t, k / 5)
        loc, nrm, _, d = TREE.find_nearest(q)
        if loc is None:
            continue
        if d < RADIUS[b] - PRESS or inside(q):
            return False
    return True


def palm_clear(b):  # off the palm's plane (a finger folded through the palm)
    return in_hand(seg(b)[1]).z < -0.012 or in_hand(seg(b)[1]).y > 0.06


def hug(f):
    """How far a finger's bones lie off the item: the gaps (beyond its thickness) at each bone's middle and tip."""
    total = 0.0
    for k in (1, 2, 3):
        h, t = seg(f + '_%d' % k)
        for q in (h.lerp(t, 0.5), t):
            d = TREE.find_nearest(q)[3]
            total += max(0.0, d - RADIUS[f + '_%d' % k]) * (2 if k > 1 else 1)  # the outer bones count double
    return total


# ---- 2. the fingers --------------------------------------------------------------------------------------------
pose = dict(start)
OPEN = [f for f in opt.get('open', '').split(',') if f]
for f in FINGERS:
    if f in OPEN:
        continue
    swing = pose.get(f + '_1', (0, 0, 0))[2]
    best = None
    for a1 in range(0, -91, -5):
        for a2 in range(0, -101, -5):
            if a2 > MIN_MID * a1 or (CURL is not None and abs(a1 - round(CURL * a2 / 5) * 5) > 0):
                continue
            t = dict(pose, **{f + '_1': (a1, 0, swing), f + '_2': (a2, 0, 0), f + '_3': (round(0.7 * a2), 0, 0)})
            apply(t)
            if not (all(clear(f + '_%d' % k) for k in (1, 2, 3)) and palm_clear(f + '_3')):
                continue
            sc = -(a1 + a2) if CURL is not None else -hug(f) + 1e-6 * (a1 + a2)  # the most closed / lying closest along the surface
            if best is None or sc > best[0]:
                best = (sc, t)
    if best:
        pose = best[1]
    gaps = []
    apply(pose)
    for k in (1, 2, 3):
        loc, nrm, _, d = TREE.find_nearest(seg(f + '_%d' % k)[1])
        gaps.append('%.0f' % ((d - RADIUS[f + '_%d' % k]) * 1000))
    print('HOLD %s: %s; tip gaps %s mm' % (f, ' '.join('%s=%s' % (b, pose[b]) for b in (f + '_1', f + '_2', f + '_3')),
                                          '/'.join(gaps)))

# ---- 3. the thumb ------------------------------------------------------------------------------------------------
HAND_X = HB.to_3x3().col[0].normalized()
TO = HB @ vec(opt['thumbto']) if 'thumbto' in opt else None
best = None
for x1 in range(-40, 21, 5):
    for z1 in range(-10, 61, 5):
        for a2 in range(0, 61, 10):
            for sz in (0, -15, -25, -35):
                t = dict(pose, thumb_1=(x1, 0, z1), thumb_2=(-a2, 0, sz), thumb_3=(-round(0.8 * a2), 0, sz))
                apply(t)
                base, tip = seg('thumb_3')
                if (tip - base).normalized().dot(HAND_X) > 0.2:
                    continue
                sc = -(tip - TO).length if TO is not None else -(a2 + abs(sz))
                if best is not None and sc <= best[0]:
                    continue
                if all(clear(b) for b in ('thumb_2', 'thumb_3')):
                    best = (sc, t)
if best:
    pose = best[1]
    print('HOLD thumb: %s%s' % (' '.join('%s=%s' % (b, pose[b]) for b in ('thumb_1', 'thumb_2', 'thumb_3')),
                                '; tip %.0f mm from the point' % (-best[0] * 1000) if TO is not None else ''))
else:
    print('HOLD thumb: no clear pose (kept)')
apply(pose)

ORDER = ['%s_%d' % (f, k) for f in FINGERS + ('thumb',) for k in (1, 2, 3)]
if 'write' in args:
    body = ', '.join('%s=(%s)' % (b, ', '.join('%d' % round(x) for x in pose[b])) for b in ORDER if b in pose)
    new = re.sub(r"pose\(.*\)\),\s*$", 'pose(%s)),' % body, line)
    open(POSES_PY, 'w', encoding='utf-8', newline='').write(src.replace(line, new))
    print('HOLD wrote the pose line of %r' % LABEL)
if 'save' in args:
    g = json.load(open(os.path.join('re', 'body', 'grip-weap_rifle.json')))
    out = {'axis_point': g['axis_point'], 'axis_direction': g['axis_direction'],
           'placement': 'held (tools/body_hold.py %s)' % ' '.join(args[1:]),
           'in_hand_bone': [list(r) for r in HB.inverted() @ w.matrix_world]}
    json.dump(out, open(os.path.join('re', 'body', 'grip-weap_%s.json' % ITEM), 'w'), indent=1)
    bpy.ops.wm.save_mainfile()
    print('HOLD saved the item place and re/body/grip-weap_%s.json' % ITEM)
