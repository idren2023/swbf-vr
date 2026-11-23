"""Blender (blender -b --factory-startup re/body/r_hand_retopo.blend -P tools/body_forearm.py -- <out.blend>): the
forearm for the retopology hand.

- r_forearm_stock: the stock first-person right forearm (all_1st_trooper.msh: the sleeve with its cuff on
  bone_r_forearm, 226 tris, arms texture, and the cuff's stud on imp_inf_trooper_r_forearm) brought into the hand frame
  (re/body/hand-frame-all_1st_trooper.json: wrist at the origin, +y to the middle knuckle, +z the back of the hand,
  the stock scaled 0.712): the reference, as r_hand_stock is for the hand. Its stud stays as the stock's.
- r_forearm_retopo: the stock sleeve's own edge loops (a simple ringed tube: triangles joined into quads), its hard
  edges creased (the cuff's rims), two Catmull-Clark levels with the uvs interpolated, back onto the stock surface:
  the stock's shape and texture exactly, smooth, cuff edges crisp (a new tube of even columns crossed the stock's
  uv seams and zigzagged the cuff's edges). A separate surface over the glove's wrist, as the stock's: the cuff, then
  a cone into the glove's open wrist (the cuff hides the glove's edge when the wrist bends).
- bones: forearm (the elbow end's centre to the wrist; the hand its child), forearm_twist (half way to the wrist) and
  wrist_twist (at the cuff), both children of the forearm copying the hand's roll about its length (0.5 and 1.0;
  Blender constraints here, the DLL sets them alike).
- weights: the sleeve shares the roll evenly from the elbow (forearm) through forearm_twist to wrist_twist; the cuff
  and the stud wholly on wrist_twist (rigid leather: it rolls with the hand, it does not bend with the wrist); the
  cone from wrist_twist to the hand bone at the glove (it bends with the wrist and stays in the glove's edge).
"""
import bpy, bmesh, json, math, os, sys
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from body_uvlib import transfer_uv

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
COLS = 16                 # rays round the arm for the stock's profile (where the cuff begins and ends)
F = json.load(open('re/body/hand-frame-all_1st_trooper.json'))
R = Matrix(F['to_hand'])
W = Vector(F['wrist'])
S = F['scale']
src = bpy.data.objects['all_1st_trooper']


def stock_part(groups, name):
    gi = {g.index for g in src.vertex_groups if g.name in groups}
    b = bmesh.new()
    b.from_mesh(src.data)
    dl = b.verts.layers.deform.active
    bmesh.ops.delete(b, geom=[v for v in b.verts if not any(k in gi for k in v[dl].keys())], context='VERTS')
    for v in b.verts:
        v.co = R @ (v.co - W) * S
    b.verts.layers.deform.remove(dl)
    m = bpy.data.meshes.new(name)
    b.to_mesh(m)
    for mt in src.data.materials:
        m.materials.append(mt)
    o = bpy.data.objects.new(name, m)
    bpy.context.scene.collection.objects.link(o)
    return o


for n in ('r_forearm_stock', 'r_forearm_stud', 'r_forearm_retopo'):
    if n in bpy.data.objects:
        bpy.data.objects.remove(bpy.data.objects[n])
fs = stock_part({'bone_r_forearm'}, 'r_forearm_stock')
stud = stock_part({'imp_inf_trooper_r_forearm'}, 'r_forearm_stud')
me = fs.data
ys = [v.co.y for v in me.vertices]
print('FOREARM stock sleeve: verts %d, tris %d, y %.3f..%.3f m (wrist at 0)' % (
    len(me.vertices), sum(len(p.vertices) - 2 for p in me.polygons), min(ys), max(ys)))

# ---- the forearm's tube ---------------------------------------------------------------------------------------------
sb = bmesh.new()
sb.from_mesh(me)
tree = BVHTree.FromBMesh(sb)
bnd = [v.co.copy() for v in sb.verts if any(e.is_boundary for e in v.link_edges)]
E = sum(bnd, Vector()) / len(bnd)                       # the elbow end's centre
front = [v.co.copy() for v in sb.verts if v.co.y > max(ys) - 0.015]
C = sum(front, Vector()) / len(front)                   # the cuff's front's centre
ax = (C - E).normalized()
L_ = (C - E).length
zr = Vector((0, 0, 1))
zr = (zr - ax * zr.dot(ax)).normalized()
xr = ax.cross(zr).normalized()


def ray(s_, k):
    """The stock surface at s_ from the elbow end along the axis, column k (0 = the back of the arm)."""
    a = 2 * math.pi * k / COLS
    d = zr * math.cos(a) + xr * math.sin(a)
    o = E + ax * s_
    loc = tree.ray_cast(o, d, 0.2)[0]
    return loc if loc is not None else tree.find_nearest(o)[0]


def radius(s_):
    o = E + ax * s_
    tot = 0.0
    for k in range(COLS):
        q = ray(s_, k) - o
        tot += (q - ax * q.dot(ax)).length
    return tot / COLS


# the front of the cuff: where rays from the axis stop finding the sleeve (its closed front)
s_end = L_
while s_end > 0 and tree.ray_cast(E + ax * s_end, zr, 0.2)[0] is None:
    s_end -= 0.001
s_end -= 0.001
prof = [(i * 0.002, radius(i * 0.002)) for i in range(int(s_end / 0.002) + 1)]
# the cuff: from the largest rise in radius (toward the wrist) to its front, where the radius falls below 75 % of
# the cuff's; past it the stock narrows in a cone into the glove's open wrist (the glove's own wrist, dark leather)
s_cuff = max((prof[i + 2][1] - prof[i][1], prof[i][0]) for i in range(len(prof) - 2) if prof[i][0] < s_end - 0.03)[1]
r_cuff = max(r for a, r in prof if s_cuff <= a <= s_cuff + 0.02)
s_front = next(a for a, r in prof if a > s_cuff + 0.01 and r < 0.75 * r_cuff)
# the stock's own edge loops, subdivided (idren2023: even columns crossed the stock's uv seams: the cuff's edges zigzagged):
# triangles joined into quads, the hard edges (the cuff's rims, faces meeting at more than CREASE_DEG) creased, SUBD
# Catmull-Clark levels with the uvs interpolated (seams kept), every vertex back onto the stock surface
CREASE_DEG, SUBD = 40, 2
bm = sb.copy()
bmesh.ops.join_triangles(bm, faces=bm.faces[:], angle_face_threshold=math.radians(40), angle_shape_threshold=math.radians(40))
bm.normal_update()
fme = bpy.data.meshes.new('r_forearm_retopo')
bm.to_mesh(fme)
for mt in me.materials:
    fme.materials.append(mt)
fo = bpy.data.objects.new('r_forearm_retopo', fme)
bpy.context.scene.collection.objects.link(fo)
bm.edges.ensure_lookup_table()
creased = 0
cr = fme.attributes.new('crease_edge', 'FLOAT', 'EDGE')
for e in bm.edges:
    if len(e.link_faces) == 2 and math.degrees(e.calc_face_angle()) > CREASE_DEG:
        cr.data[e.index].value = 1.0
        creased += 1
sub = fo.modifiers.new('subd', 'SUBSURF')
sub.levels = SUBD
sub.uv_smooth = 'PRESERVE_BOUNDARIES'
bpy.context.view_layer.objects.active = fo
bpy.ops.object.modifier_apply(modifier='subd')
for v in fme.vertices:
    v.co = tree.find_nearest(v.co)[0]
for p_ in fme.polygons:
    p_.use_smooth = True
quads = sum(1 for p_ in bm.faces if len(p_.verts) == 4)
print('FOREARM retopo: the stock loops (%d faces, %d quads), %d hard edges creased, %d levels: faces %d (tris %d); '
      'cuff %.0f-%.0f mm, to %.0f (wrist at %.0f)' % (len(bm.faces), quads, creased, SUBD, len(fme.polygons),
                                                      sum(len(p_.vertices) - 2 for p_ in fme.polygons),
                                                      s_cuff * 1000, s_front * 1000, s_end * 1000, L_ * 1000))

# ---- bones -----------------------------------------------------------------------------------------------------------
hand = bpy.data.objects['r_hand_retopo']
rig = hand.parent
bpy.context.view_layer.objects.active = rig
for o_ in bpy.context.scene.objects:
    o_.select_set(o_ is rig)
bpy.ops.object.mode_set(mode='EDIT')
eb = rig.data.edit_bones
W0 = eb['hand'].head.copy()
fb = eb.get('forearm') or eb.new('forearm')
fb.head, fb.tail = E.copy(), W0
fb.align_roll(Vector((0, 0, 1)))
mid = eb.get('forearm_twist') or eb.new('forearm_twist')
mid.head, mid.tail = E.lerp(W0, 0.5), W0
wt = eb.get('wrist_twist') or eb.new('wrist_twist')
wt.head, wt.tail = E.lerp(W0, 0.92), W0 + (W0 - E).normalized() * 0.01
for b_ in (mid, wt):
    b_.align_roll(Vector((0, 0, 1)))
    b_.parent = fb
eb['hand'].parent = fb
bpy.ops.object.mode_set(mode='POSE')
for n, inf in (('forearm_twist', 0.5), ('wrist_twist', 1.0)):
    pb = rig.pose.bones[n]
    for c in list(pb.constraints):
        pb.constraints.remove(c)
    c = pb.constraints.new('COPY_ROTATION')
    c.target, c.subtarget = rig, 'hand'
    c.use_x = c.use_z = False
    c.target_space = c.owner_space = 'LOCAL'
    c.influence = inf
bpy.ops.object.mode_set(mode='OBJECT')

# ---- weights ---------------------------------------------------------------------------------------------------------
for o_, groups in ((fo, ('forearm', 'forearm_twist', 'wrist_twist', 'hand')), (stud, ('wrist_twist',))):
    o_.parent = rig
    for n in groups:
        o_.vertex_groups.new(name=n)
    m_ = o_.modifiers.new('skeleton', 'ARMATURE')
    m_.object = rig
stud.vertex_groups['wrist_twist'].add([v.index for v in stud.data.vertices], 1.0, 'REPLACE')
G = {g.name: g for g in fo.vertex_groups}
back = s_cuff - 0.002
for v in fme.vertices:
    s_ = (v.co - E).dot(ax)
    if s_ >= s_front:
        # the cone into the glove: from the cuff's roll to the hand (bends with the wrist, meets the glove's edge)
        h = min(1.0, (s_ - s_front) / max(s_end - s_front, 1e-6))
        w = {'wrist_twist': 1 - h, 'hand': h}
    elif s_ >= back:
        w = {'wrist_twist': 1.0}
    else:
        r = max(s_, 0.0) / back                   # 0 at the elbow, 1 at the cuff's back
        w = {'forearm': max(0.0, 1 - 2 * r), 'forearm_twist': 1 - abs(2 * r - 1), 'wrist_twist': max(0.0, 2 * r - 1)}
    for n, x in w.items():
        if x > 0.001:
            G[n].add([v.index], x, 'REPLACE')
fs.hide_render = True
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
