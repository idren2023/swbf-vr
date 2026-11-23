"""Blender (blender -b --factory-startup re/body/all_inf_soldier.blend -P tools/body_soldier.py -- <out.blend> [sheet=<prefix>]):
the player's first-person body (PLAN 51 B), first pass, on the Rebel soldier (all_inf_soldier.msh, 1 678 tris, rigid
weights on 22 bones, T-pose):

- surface: the stock's own loops (triangles joined into quads), the head removed (first person: no head, the neck
  left open and hollow), hard edges creased (CREASE_DEG), SUBD Catmull-Clark levels with the uvs interpolated, every
  vertex back onto the stock surface: the stock shape and textures exactly, smooth (as the forearm, tools/body_forearm.py).
- weights: each vertex's bone from the stock (the nearest stock face's majority), then a smooth blend across every
  joint it touches: smoothstep over +-band (JOINTS, metres) of the plane through the joint halving the angle between
  the two bones (the stock's rigid weights fold every joint).
- object r_body (parented to the stock skeleton, armature modifier).

With sheet=: a pose test (POSES: walking, crouching, arms lowered and bent, the chest twisted), edge stretch against
the rest per pose (99th percentile and worst, longer and shorter), and a picture sheet <prefix>.png (front, side,
three-quarter per pose, textured)."""
import bpy, bmesh, math, os, sys
from mathutils import Matrix, Quaternion, Vector
from mathutils.bvhtree import BVHTree

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
SHEET = next((a[6:] for a in args[1:] if a.startswith('sheet=')), None)
SUBD = int(next((a[5:] for a in args[1:] if a.startswith('subd=')), '1'))
CREASE_DEG = 40
stock = bpy.data.objects['all_inf_soldier']
rig = stock.parent
B = rig.data.bones
for o in list(bpy.data.objects):
    if o.name in ('r_body', 'all_inf_soldier_hi'):
        bpy.data.objects.remove(o)

# ---- the surface -------------------------------------------------------------------------------------------------------
gname = {g.index: g.name for g in stock.vertex_groups}


def stock_bone(v):
    return gname[max(v.groups, key=lambda g: g.weight).group] if v.groups else 'bone_root'


sb = bmesh.new()
sb.from_mesh(stock.data)
sb.verts.ensure_lookup_table()
vbone = [stock_bone(v) for v in stock.data.vertices]
BONE_NAMES = sorted(set(vbone))
# each vertex's stock bone kept on the vertex itself (indices shift when the head is deleted)
bone_layer = sb.verts.layers.int.new('stock_bone')
for v in sb.verts:
    v[bone_layer] = BONE_NAMES.index(vbone[v.index])
head = [v for v in sb.verts if vbone[v.index] == 'bone_head']
nohead = sb.copy()
nohead.verts.ensure_lookup_table()
bmesh.ops.delete(nohead, geom=[nohead.verts[v.index] for v in head], context='VERTS')
tree = BVHTree.FromBMesh(nohead)
# the stock face under a point -> its majority bone (kept from before the deletion: face index in nohead)
nohead.faces.ensure_lookup_table()
bm = nohead.copy()
bmesh.ops.join_triangles(bm, faces=bm.faces[:], angle_face_threshold=math.radians(40),
                         angle_shape_threshold=math.radians(40))
# the stock's own weight layer would come along (and the new groups would land on its indices): dropped
for layer in list(bm.verts.layers.deform.values()):
    bm.verts.layers.deform.remove(layer)
me = bpy.data.meshes.new('r_body')
bm.to_mesh(me)
for m in stock.data.materials:
    me.materials.append(m)
ob = bpy.data.objects.new('r_body', me)
bpy.context.scene.collection.objects.link(ob)
bm.normal_update()
cr = me.attributes.new('crease_edge', 'FLOAT', 'EDGE')
bm.edges.ensure_lookup_table()
for e in bm.edges:
    if len(e.link_faces) == 2 and math.degrees(e.calc_face_angle()) > CREASE_DEG:
        cr.data[e.index].value = 1.0
for vg in list(ob.vertex_groups):
    ob.vertex_groups.remove(vg)
sub = ob.modifiers.new('subd', 'SUBSURF')
sub.levels = SUBD
sub.uv_smooth = 'PRESERVE_BOUNDARIES'
bpy.context.view_layer.objects.active = ob
bpy.ops.object.modifier_apply(modifier='subd')
for v in me.vertices:
    v.co = tree.find_nearest(v.co)[0]
for p in me.polygons:
    p.use_smooth = True
nbl = nohead.verts.layers.int['stock_bone']

# ---- weights -----------------------------------------------------------------------------------------------------------
def H(n):
    return B[n].head_local.copy()


SIDE = ('l', 'r')
DIRS = {}
JOINTS = []   # (parent, child, joint point, normal (toward the child), band metres, reach metres)


def joint(p, c, point, dp, dc, band, reach=0.25):
    n = dp.normalized() + dc.normalized()
    n = n.normalized() if n.length > 0.3 else dc.normalized()
    JOINTS.append((p, c, point, n, band, reach))


up = Vector((0, 0, 1))
joint('bone_pelvis', 'bone_a_spine', H('bone_a_spine') + Vector((0, 0, 0.03)), -up, up, 0.05, 0.35)
joint('bone_a_spine', 'bone_b_spine', H('bone_b_spine'), up, up, 0.05, 0.35)
joint('bone_b_spine', 'bone_ribcage', H('bone_ribcage'), up, up, 0.05, 0.35)
joint('bone_ribcage', 'bone_neck', H('bone_neck'), up, up, 0.03, 0.15)
for s in SIDE:
    sg = 1 if s == 'l' else -1
    thigh, calf, foot, toe = ('bone_%s_%s' % (s, n) for n in ('thigh', 'calf', 'foot', 'toe'))
    clav, uarm, farm, hand = ('bone_%s_%s' % (s, n) for n in ('clavicle', 'upperarm', 'forearm', 'hand'))
    joint('bone_pelvis', thigh, H(thigh), -up, H(calf) - H(thigh), 0.06, 0.25)
    joint(thigh, calf, H(calf), H(calf) - H(thigh), H(foot) - H(calf), 0.04, 0.18)
    joint(calf, foot, H(foot), H(foot) - H(calf), H(toe) - H(foot), 0.03, 0.14)
    joint(foot, toe, H(toe), H(toe) - H(foot), Vector((0, -1, 0)), 0.02, 0.10)
    joint('bone_ribcage', clav, H(clav) + (H(uarm) - H(clav)) * 0.35, up, H(uarm) - H(clav), 0.06, 0.25)
    joint(clav, uarm, H(uarm), H(uarm) - H(clav), H(farm) - H(uarm), 0.05, 0.18)
    joint(uarm, farm, H(farm), H(farm) - H(uarm), H(hand) - H(farm), 0.04, 0.14)
    joint(farm, hand, H(hand), H(hand) - H(farm), Vector((sg, 0, 0)), 0.03, 0.12)


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


groups = {}


def group(n):
    if n not in groups:
        groups[n] = ob.vertex_groups.new(name=n)
    return groups[n]


nohead_tree = BVHTree.FromBMesh(nohead)
for v in me.vertices:
    loc, _, fi, _ = nohead_tree.find_nearest(v.co)
    fb = [BONE_NAMES[x[nbl]] for x in nohead.faces[fi].verts]
    b = max(set(fb), key=fb.count)
    w = {b: 1.0}
    for p, c, point, n, band, reach in JOINTS:
        if b not in (p, c) or (v.co - point).length > reach:
            continue
        t = smooth(((v.co - point).dot(n) + band) / (2 * band))   # 1 well on the child's side
        if b == c and t < 1:
            w[c] = w.get(c, 0) - (1 - t)
            w[p] = w.get(p, 0) + (1 - t)
        elif b == p and t > 0:
            w[p] = w.get(p, 0) - t
            w[c] = w.get(c, 0) + t
    w = {k: x for k, x in w.items() if x > 0.005}
    tot = sum(w.values())
    for k, x in w.items():
        group(k).add([v.index], x / tot, 'REPLACE')
ob.parent = rig
mod = ob.modifiers.new('skeleton', 'ARMATURE')
mod.object = rig
stock.hide_render = True
blended = sum(1 for v in me.vertices if len(v.groups) > 1)
print('BODY r_body: %d faces (%d quads), %d tris, %d vertices (%d on 2+ bones), head removed (%d stock vertices), '
      'subd %d' % (len(me.polygons), sum(1 for p in me.polygons if len(p.vertices) == 4),
                   sum(len(p.vertices) - 2 for p in me.polygons), len(me.vertices), blended, len(head), SUBD))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))

if not SHEET:
    sys.exit(0)

# ---- pose test ---------------------------------------------------------------------------------------------------------
# rotations about the rig's axes (x the soldier's left, -y ahead, z up), degrees, applied to each bone in its rest frame
POSES = [
    ('rest', {}),
    ('walking stride', {'bone_r_thigh': ('x', -35), 'bone_r_calf': ('x', 25), 'bone_l_thigh': ('x', 25),
                        'bone_l_calf': ('x', 45), 'bone_l_foot': ('x', -15)}),
    ('crouch', {'bone_r_thigh': ('x', -95), 'bone_l_thigh': ('x', -95), 'bone_r_calf': ('x', 120),
                'bone_l_calf': ('x', 120), 'bone_r_foot': ('x', -25), 'bone_l_foot': ('x', -25),
                'bone_a_spine': ('x', -15), 'bone_b_spine': ('x', -10)}),
    ('arms lowered, elbows bent', {'bone_r_upperarm': ('y', -70), 'bone_l_upperarm': ('y', 70),
                                   'bone_r_forearm': ('z', 80), 'bone_l_forearm': ('z', -80)}),
    ('arms forward (aiming)', {'bone_r_upperarm': ('z', 80), 'bone_l_upperarm': ('z', -70),
                               'bone_l_forearm': ('z', -60), 'bone_r_forearm': ('z', 20)}),
    ('chest twisted, bent forward', {'bone_a_spine': ('z', 15), 'bone_b_spine': ('z', 15), 'bone_ribcage': ('x', -20)}),
]


def set_pose(spec):
    for pb in rig.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.rotation_quaternion = (1, 0, 0, 0)
    for n, (axis, deg) in spec.items():
        pb = rig.pose.bones[n]
        R = pb.bone.matrix_local.to_3x3().normalized().to_quaternion()
        qw = Quaternion({'x': (1, 0, 0), 'y': (0, 1, 0), 'z': (0, 0, 1)}[axis], math.radians(deg))
        pb.rotation_quaternion = R.inverted() @ qw @ R
    bpy.context.view_layer.update()


gnames = {g.index: g.name for g in ob.vertex_groups}


def groups_of(vi):
    gs = sorted(me.vertices[vi].groups, key=lambda g: -g.weight)[:2]
    return '/'.join(gnames[g.group][5:] for g in gs)


edges = [tuple(e.vertices) for e in me.edges]
rest = [v.co.copy() for v in me.vertices]
rest_len = [(rest[a] - rest[b]).length for a, b in edges]
for label, spec in POSES:
    set_pose(spec)
    ev = ob.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
    ratio = [((ev.vertices[a].co - ev.vertices[b].co).length / max(l, 1e-6), a) for (a, b), l in zip(edges, rest_len)]
    r = sorted(x for x, _ in ratio)
    # where the worst stretch is: the dominant bones of the ten worst edges' first vertices
    worst = sorted(ratio, reverse=True)[:10]
    where = {}
    for _, vi in worst:
        n = groups_of(vi)
        where[n] = where.get(n, 0) + 1
    print('POSE %-30s stretch p99 %.2f max %.2f | squash p1 %.2f min %.2f | worst at %s' % (
        label, r[int(len(r) * 0.99)], r[-1], r[int(len(r) * 0.01)], r[0],
        ', '.join('%s %d' % kv for kv in sorted(where.items(), key=lambda kv: -kv[1]))))

# picture sheet: one row per pose, three views
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'TEXTURE'
scene.render.resolution_x, scene.render.resolution_y = 600, 700
scene.render.film_transparent = False
scene.world = scene.world or bpy.data.worlds.new('w')
cd = bpy.data.cameras.new('cam')
cd.type = 'ORTHO'
cd.ortho_scale = 2.3
cam = bpy.data.objects.new('cam', cd)
scene.collection.objects.link(cam)
scene.camera = cam
VIEWS = (('front', Vector((0, -1, 0))), ('side', Vector((-1, 0, 0))), ('3/4 back', Vector((0.6, 0.8, 0.25))))
for o in scene.objects:
    if o.type == 'MESH':
        o.hide_render = o is not ob
tiles = []
for label, spec in POSES:
    set_pose(spec)
    for vname, d in VIEWS:
        target = Vector((0, 0, 1.0))
        cam.location = target + d.normalized() * 5
        cam.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()
        path = os.path.abspath('%s-%d-%s.png' % (SHEET, len(tiles), vname.replace('/', '').replace(' ', '')))
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        tiles.append(path)
# one sheet: rows = poses, columns = views (Blender's image API, no PIL)
cols, rows = len(VIEWS), len(POSES)
W, Hh = scene.render.resolution_x, scene.render.resolution_y
sheet = bpy.data.images.new('sheet', W * cols, Hh * rows)
px = [0.0] * (W * cols * Hh * rows * 4)
for k, path in enumerate(tiles):
    img = bpy.data.images.load(path)
    src = list(img.pixels)
    r_, c_ = rows - 1 - k // cols, k % cols
    for y in range(Hh):
        o = ((r_ * Hh + y) * W * cols + c_ * W) * 4
        px[o:o + W * 4] = src[y * W * 4:(y + 1) * W * 4]
    os.remove(path)
sheet.pixels = px
sheet.filepath_raw = os.path.abspath(SHEET + '.png')
sheet.file_format = 'PNG'
sheet.save()
print('SHEET %s.png rows: %s' % (SHEET, ' | '.join(l for l, _ in POSES)))
