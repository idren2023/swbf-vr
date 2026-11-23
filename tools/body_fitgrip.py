"""Blender (blender -b --factory-startup <hand+weapons.blend> -P tools/body_fitgrip.py -- <weapon-object> <pose-label>
[free=index] [gap=mm] [hand=<object>] [frompose]): closes a pose onto a weapon: each finger (and the thumb) is curled along the pose's own angles
(scaled by s from 0 up to 1.8) until its glove first touches the weapon's surface (its outer two segments within `gap` of
the surface, default 1.5 mm, or crossing it; the weapons are open shells: no inside / outside test); fingers named in free= keep their angles (a finger off the trigger).
Prints the fitted pose as a line for tools/body_poses.py and how close each finger came."""
import bpy, bmesh, math, os, sys
from mathutils import Vector
from mathutils.bvhtree import BVHTree

args = sys.argv[sys.argv.index('--') + 1:]
WEAPON, LABEL = args[0], args[1]
FREE = next((a[5:].split(',') for a in args[2:] if a.startswith('free=')), [])
FROM_POSE = 'frompose' in args[2:]  # start at the pose's own angles (open until clear, or close until touching)
GAP = next((float(a[4:]) for a in args[2:] if a.startswith('gap=')), 1.5) / 1000
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'body_poses.py')).read()
ns = {}
exec(src[src.index('FINGERS = '):src.index('scene = bpy.context.scene')], ns)
POSE = dict(next(p for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(LABEL)))
obj = bpy.data.objects[next((a[5:] for a in args[2:] if a.startswith('hand=')), 'r_hand_stock')]
rig = obj.parent
P = rig.pose.bones
names = {g.index: g.name for g in obj.vertex_groups}
w = bpy.data.objects[WEAPON]
wb = bmesh.new()
wb.from_mesh(w.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh())
wb.transform(w.matrix_world)
wtree = BVHTree.FromBMesh(wb)
DIGITS = {f: [n for n in ('%s_1' % f, '%s_2' % f, '%s_3' % f) if n in P] for f in ('index', 'middle', 'ring', 'pinky', 'thumb')}
# the digit's outer part: its two outer segments (the thumb: its tip), where it meets the weapon first
own = {f: [v.index for v in obj.data.vertices
           if sum(g.weight for g in v.groups if names[g.group] in DIGITS[f][-2 if f != 'thumb' else -1:]) > 0.6]
       for f in DIGITS}
obj.data.calc_loop_triangles()
own_faces = {f: [t.index for t in obj.data.loop_triangles if all(i in set(own[f]) for i in t.vertices)] for f in DIGITS}


def apply(p):
    for pb in P:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
    for n, d in p.items():
        if n in P:
            P[n].rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
            P[n].rotation_euler = [math.radians(x) for x in d]
    bpy.context.view_layer.update()
    return obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()


def closest(me, f):
    """The digit's outer part's distance to the weapon; 0 when their surfaces cross (the weapons are open shells,
    so no inside / outside test)."""
    verts = [obj.matrix_world @ v.co for v in me.vertices]
    me.calc_loop_triangles()
    tris = [list(me.loop_triangles[i].vertices) for i in own_faces[f]]
    if tris and BVHTree.FromPolygons(verts, tris).overlap(wtree):
        return 0.0
    return min(wtree.find_nearest(verts[i])[3] for i in own[f])


fitted = dict(POSE)
report = []
for f, bones in DIGITS.items():
    if f in FREE:
        continue
    base = {n: POSE.get(n, (0, 0, 0)) for n in bones}
    if not any(any(v) for v in base.values()):
        continue
    if FROM_POSE:
        # from the pose itself (idren2023 2026-10-24: the stock grip as the start): opened step by step until the digit
        # no longer crosses the weapon, or closed further until it touches; the scale kept
        s_fit = 1.0
        if closest(apply(dict(fitted, **{n: d for n, d in base.items()})), f) <= 0.0:
            while s_fit > 0.0:
                s_fit = round(s_fit - 0.05, 2)
                trial = dict(fitted, **{n: tuple(x * s_fit for x in d) for n, d in base.items()})
                if closest(apply(trial), f) > 0.0:
                    break
            how = 'opened'
        else:
            while s_fit < 1.5:
                trial = dict(fitted, **{n: tuple(x * (s_fit + 0.05) for x in d) for n, d in base.items()})
                if closest(apply(trial), f) <= GAP:
                    break
                s_fit = round(s_fit + 0.05, 2)
            how = 'closed'
        for n, d in base.items():
            fitted[n] = tuple(round(x * s_fit) for x in d)
        report.append('%s x%.2f (%s)' % (f, s_fit, how))
        continue
    lo, hi = 0.0, None
    # step out until contact, then halve between
    s, last_free = 0.0, 0.0
    for s in [k * 0.1 for k in range(0, 19)]:
        trial = dict(fitted)
        for n, d in base.items():
            trial[n] = tuple(x * s for x in d)
        if closest(apply(trial), f) <= GAP:
            hi = s
            break
        last_free = s
    if hi is None:
        s_fit = 1.8
    else:
        lo = last_free
        for _ in range(6):
            mid = (lo + hi) / 2
            trial = dict(fitted)
            for n, d in base.items():
                trial[n] = tuple(x * mid for x in d)
            if closest(apply(trial), f) <= GAP:
                hi = mid
            else:
                lo = mid
        s_fit = lo
    for n, d in base.items():
        fitted[n] = tuple(round(x * s_fit) for x in d)
    report.append('%s x%.2f (%s)' % (f, s_fit, 'touches' if hi is not None else 'no contact'))
me = apply(fitted)
gaps = ' '.join('%s %.1f' % (f, closest(me, f) * 1000) for f in DIGITS)
print('FIT %s on %s: %s | closest (mm): %s' % (LABEL, WEAPON, ', '.join(report), gaps))
print('POSE ' + repr({k: v for k, v in fitted.items() if any(v)}))
