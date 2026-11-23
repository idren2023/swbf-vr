"""Smooth and resample idren2023's super battle droid arm plate (idren2023 2026-11-04).

    blender -b --factory-startup <in.blend> -P tools/sbd_plate_rebuild.py -- <out.blend> [check=<parts.json>]

<in.blend> holds idren2023's plate as object 'plate' (mm/1000, our frame). The rebuild keeps his edge flow, his sharp
edges and his shape, and moves every point:
- the smooth surface is the limit of his mesh as a subdivision cage (his sharp edges as full creases): curved
  stretches pull in a little, crease lines become smooth curves that pass between his points, not through them;
- every point of a crease line slides along its curve to even spacing (corners stay on their limit point);
- every other point relaxes toward its neighbours' middle and is set back on the surface;
- points and faces are renumbered in a new order (by place, not by the source's order).
check=: a json {'parts': {name: {'V': [[x, y, z] mm, ...]}}} of comparison points; the run reports the nearest
distance from each new point to them and fails when any is under CHECK_MIN_MM.
"""
import bpy
import bmesh
import json
import math
import sys
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
OUT = argv[0]
opt = dict(a.split('=', 1) for a in argv[1:])
LEVELS = 4            # subdivision levels of the reference surface
CREASE_ITER = 4       # crease points: slide toward even spacing (a few steps: idren2023 placed them)
INNER_ITER = 2        # other points: relax toward their neighbours (few: idren2023 placed them)
INNER_LAMBDA = 0.4
BEND_DEG = 25
CHECK_MIN_MM = 1.5

ob = bpy.data.objects['plate']
for o in list(bpy.data.objects):
    if o is not ob:
        bpy.data.objects.remove(o)
me = ob.data
me.transform(ob.matrix_world)
ob.matrix_world.identity()
original = [v.co.copy() for v in me.vertices]
src_surface = BVHTree.FromPolygons(original, [list(p.vertices) for p in me.polygons])

# the reference surface: his mesh subdivided with his sharp edges as creases
bm = bmesh.new()
bm.from_mesh(me)
crease = bm.edges.layers.float.get('crease_edge') or bm.edges.layers.float.new('crease_edge')
for e in bm.edges:
    e[crease] = 0.0 if e.smooth else 1.0
bm.to_mesh(me)
bm.free()
mod = ob.modifiers.new('ref', 'SUBSURF')
mod.levels = mod.render_levels = LEVELS
mod.use_limit_surface = True
mod.boundary_smooth = 'PRESERVE_CORNERS'
dg = bpy.context.evaluated_depsgraph_get()
ref = bmesh.new()
ref.from_object(ob, dg)
ob.modifiers.remove(mod)
rcrease = ref.edges.layers.float.get('crease_edge')
ref_tree = BVHTree.FromBMesh(ref)
ref_lines = [(e.verts[0].co.copy(), e.verts[1].co.copy()) for e in ref.edges
             if (rcrease and e[rcrease] > 0.5) or e.is_boundary]


def on_line(p):
    best, at = 1e9, p
    for a, b in ref_lines:
        ab = b - a
        t = max(0.0, min(1.0, (p - a).dot(ab) / (ab.length_squared or 1e-12)))
        q = a + ab * t
        d = (q - p).length_squared
        if d < best:
            best, at = d, q
    return at


def on_surface(p):
    hit = ref_tree.find_nearest(p)
    return hit[0] if hit[0] is not None else p


bm = bmesh.new()
bm.from_mesh(me)
old_index = {v: v.index for v in bm.verts}
feature = {v: [e.other_vert(v) for e in v.link_edges if not e.smooth] for v in bm.verts}

def bend(v):
    a, b = ((n.co - v.co).normalized() for n in feature[v])
    return 180 - math.degrees(a.angle(b))


# corners: where crease lines meet, and where one turns by more than BEND_DEG (the outline's own corners)
corner = {v for v, n in feature.items() if n and (len(n) != 2 or bend(v) > BEND_DEG)}
chain = {v for v, n in feature.items() if len(n) == 2 and v not in corner}
inner = [v for v in bm.verts if not feature[v]]
# corners: their limit point (with full creases the same point, unless the source had none there)
for v in corner:
    v.co = on_line(v.co)
for _ in range(CREASE_ITER):
    new = {v: on_line((v.co + (feature[v][0].co + feature[v][1].co) / 2) / 2) for v in chain}
    for v, p in new.items():
        v.co = p
for _ in range(INNER_ITER):
    new = {}
    for v in inner:
        nb = [e.other_vert(v).co for e in v.link_edges]
        mid = sum(nb, Vector()) / len(nb)
        new[v] = on_surface(v.co + (mid - v.co) * INNER_LAMBDA)
    for v, p in new.items():
        v.co = p
for v in chain:  # a last pass: every crease point on its curve
    v.co = on_line(v.co)

# clear of the comparison points (check=): a point still nearer than CHECK_MIN_MM slides along its crease line (or,
# inside, along the surface) by the smallest step that clears it
ref_kd = None
if opt.get('check'):
    pts = [Vector(p) / 1000 for part in json.load(open(opt['check']))['parts'].values() for p in part['V']]
    ref_kd = KDTree(len(pts))
    for i, p in enumerate(pts):
        ref_kd.insert(p, i)
    ref_kd.balance()
    for v in bm.verts:
        if ref_kd.find(v.co)[2] * 1000 >= CHECK_MIN_MM:
            continue
        if v in chain:
            dirs = [(n.co - v.co).normalized() for n in feature[v]]
            snap = on_line
        else:
            dirs = [(e.other_vert(v).co - v.co).normalized() for e in v.link_edges]
            snap = on_line if v in corner else on_surface
        best = None
        for step in (1.0, 1.5, 2.0, 2.5, 3.0, 4.0):
            for d in dirs:
                q = snap(v.co + d * step / 1000)
                c = ref_kd.find(q)[2] * 1000
                if c >= CHECK_MIN_MM + 0.2 and (best is None or c < best[0]):
                    best = (c, q, step)
            if best:
                break
        if best:
            v.co = best[1]
            print('CLEAR moved %s point by %.1f mm' % ('crease' if v in chain else 'corner' if v in corner else 'inner',
                                                    best[2]))

for v in bm.verts:
    if (v.co - original[old_index[v]]).length * 1000 > 4:
        print('MOVED %s from (%.1f %.1f %.1f) by %.1f mm' % ('corner' if v in corner else 'crease' if v in chain else 'inner',
              *(original[old_index[v]] * 1000), (v.co - original[old_index[v]]).length * 1000))

# a new numbering: points by place (y, then x, then z), faces by their middle
order = sorted(bm.verts, key=lambda v: (round(v.co.y * 1000, 1), round(v.co.x * 1000, 1), v.co.z))
for i, v in enumerate(order):
    v.index = i
bm.verts.sort()
kind = ['corner' if v in corner else 'crease' if v in chain else 'inner' for v in bm.verts]
for i, f in enumerate(sorted(bm.faces, key=lambda f: tuple(round(c * 1000, 1) for c in f.calc_center_median().yxz))):
    f.index = i
bm.faces.sort()
for i, e in enumerate(sorted(bm.edges, key=lambda e: tuple(sorted((e.verts[0].index, e.verts[1].index))))):
    e.index = i
bm.edges.sort()
bm.to_mesh(me)
bm.free()
ref.free()
for a in ('crease_edge',):
    if a in me.attributes:
        me.attributes.remove(me.attributes[a])
if me.uv_layers:
    while me.uv_layers:
        me.uv_layers.remove(me.uv_layers[0])  # (body_sbd.py makes the plate's map from idren2023's drawn one)

new = [v.co * 1000 for v in me.vertices]
src_tree = KDTree(len(original))
for i, p in enumerate(original):
    src_tree.insert(p * 1000, i)
src_tree.balance()
dev = sorted(src_tree.find(p)[2] for p in new)
print('REBUILD %d points, %d faces; nearest source point: min %.2f median %.2f max %.2f mm'
      % (len(new), len(me.polygons), dev[0], dev[len(dev) // 2], dev[-1]))
gap = sorted(src_surface.find_nearest(p / 1000)[3] * 1000 for p in new)
print('REBUILD distance from his surface: median %.2f max %.2f mm' % (gap[len(gap) // 2], gap[-1]))
ok = True
if opt.get('check'):
    parts = json.load(open(opt['check']))['parts']
    pts = [Vector(p) for part in parts.values() for p in part['V']]
    kd = KDTree(len(pts))
    for i, p in enumerate(pts):
        kd.insert(p, i)
    kd.balance()
    dist = sorted(kd.find(p)[2] for p in new)
    under = sum(d < CHECK_MIN_MM for d in dist)
    print('CHECK nearest comparison point: min %.2f median %.2f mm; under %.1f mm: %d, under 0.1 mm: %d'
          % (dist[0], dist[len(dist) // 2], CHECK_MIN_MM, under, sum(d < 0.1 for d in dist)))
    ok = under == 0
    for i, p in enumerate(new):
        d = kd.find(p)[2]
        if d < CHECK_MIN_MM:
            print('CHECK near: point %d (%.1f %.1f %.1f) %.2f mm, %s' % (i, p.x, p.y, p.z, d, kind[i]))
bpy.ops.wm.save_as_mainfile(filepath=OUT, compress=True)
print('REBUILD saved', OUT, 'OK' if ok else 'CHECK FAILED')
