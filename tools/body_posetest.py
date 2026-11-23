"""Blender (blender -b --factory-startup <file.blend> -P tools/body_posetest.py -- <object> <out.png> [subdiv] [textured]): poses
the hand rig through its range (PLAN 51 B rule 6) and reports per pose the edge stretch against the rest (worst and
99th percentile, longer and shorter) and triangle pairs that pass through each other (not neighbours), grouped by
the bones that carry them; writes a contact sheet of the poses (back-of-hand view, grey)."""
import bpy, bmesh, math, os, sys
from mathutils import Vector
from mathutils.bvhtree import BVHTree

args = sys.argv[sys.argv.index('--') + 1:]
OBJ, OUT = args[0], args[1]
SUBDIV = next((int(a) for a in args[2:] if a.isdigit()), 0)
scene = bpy.context.scene
obj = bpy.data.objects[OBJ]
rig = obj.parent
if SUBDIV:
    m = obj.modifiers.new('subd', 'SUBSURF')
    m.levels = SUBDIV
    bpy.ops.object.select_all(action='DESELECT')
    obj.modifiers.move(len(obj.modifiers) - 1, 0)

FINGERS = ('index', 'middle', 'ring', 'pinky')


def fist(curl=1.0):
    """Pose: bone -> (x, y, z) degrees; x bends (negative = flex), z swings sideways."""
    p = {}
    for f in FINGERS:
        p[f + '_1'] = (-80 * curl, 0, 0)
        p[f + '_2'] = (-95 * curl, 0, 0)
        p[f + '_3'] = (-60 * curl, 0, 0)
    p['thumb_1'] = (-20 * curl, 0, -30 * curl)  # z < 0 swings the thumb across the palm (measured on both rigs)
    p['thumb_2'] = (-25 * curl, 0, 0)
    p['thumb_3'] = (-35 * curl, 0, 0)
    return p


POSES = [
    ('rest', {}),
    ('open', {**{f + s: (12, 0, 0) for f in FINGERS for s in ('_1', '_2', '_3')}, 'thumb_2': (10, 0, 0)}),
    ('fist', fist()),
    # pointing: the index a little below the hand's line, slightly bent at each joint (not held at the rest pose,
    # which sat high against the closed fingers, idren2023)
    # straight past the rest curl, and swung at the knuckle toward the other fingers (z < 0: toward the pinky,
    # measured), so the finger lines up with the forearm (idren2023)
    ('point', {**fist(), 'index_1': (-14, 0, -10), 'index_2': (11, 0, 0), 'index_3': (6, 0, 0)}),
    ('thumbs up', {**fist(), 'thumb_1': (10, 0, 15), 'thumb_2': (10, 0, 0), 'thumb_3': (5, 0, 0)}),
    ('grip', fist(0.7)),
    ('wrist flex 70', {'hand': (-70, 0, 0)}),
    ('wrist ext 70', {'hand': (70, 0, 0)}),
    ('wrist side 25', {'hand': (0, 0, 25)}),
    ('roll +90', {'forearm_twist': (0, 54, 0), 'hand': (0, 36, 0)}),
    ('roll -90', {'forearm_twist': (0, -54, 0), 'hand': (0, -36, 0)}),
    ('grip flex 50 roll 60', {**fist(0.7), 'hand': (-50, 24, 0), 'forearm_twist': (0, 36, 0)}),
]


def set_pose(p):
    for pb in rig.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
    for name, deg in p.items():
        if name in rig.pose.bones:  # a rig without forearm bones (a hand alone) skips those
            pb = rig.pose.bones[name]
            pb.rotation_mode = 'ZXY' if name.endswith('_1') and not name.startswith('thumb') else 'XYZ'  # as body_poses
            pb.rotation_euler = [math.radians(d) for d in deg]
    bpy.context.view_layer.update()


def evaluated():
    dg = bpy.context.evaluated_depsgraph_get()
    e = obj.evaluated_get(dg)
    me = e.to_mesh()
    bm = bmesh.new()
    bm.from_mesh(me)
    e.to_mesh_clear()
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    return bm


def main_bone(v, deform, names):
    g, w = max(v[deform].items(), key=lambda gw: gw[1]) if v[deform] else (-1, 0)
    return names.get(g, '?')


rest_pairs = set()
set_pose({})
rest = evaluated()
rest_len = [e.calc_length() for e in rest.edges]
names = {g.index: g.name for g in obj.vertex_groups}
deform = rest.verts.layers.deform.active
print('POSETEST', OBJ, 'subdiv', SUBDIV, 'tris', len(rest.faces))
pictures = []
for label, p in POSES:
    set_pose(p)
    bm = evaluated()
    ratios = sorted(e.calc_length() / max(r, 1e-9) for e, r in zip(bm.edges, rest_len))
    n = len(ratios)
    stretch = (ratios[-1], ratios[int(n * 0.99)])
    squash = (ratios[0], ratios[int(n * 0.01)])
    bm.faces.ensure_lookup_table()
    tree = BVHTree.FromBMesh(bm)
    pairs = tree.overlap(tree)
    dl = bm.verts.layers.deform.active
    groups = {}
    for a, b in pairs:
        if a >= b or (a, b) in rest_pairs:
            continue
        fa, fb = bm.faces[a], bm.faces[b]
        if set(fa.verts) & set(fb.verts):
            continue
        ba = main_bone(fa.verts[0], dl, names) if dl else "?"
        bb = main_bone(fb.verts[0], dl, names) if dl else "?"
        key = ' x '.join(sorted((ba.split('_')[0], bb.split('_')[0])))
        groups[key] = groups.get(key, 0) + 1
    worst = {}
    for e, r in zip(bm.edges, rest_len):
        x = e.calc_length() / max(r, 1e-9)
        k = main_bone(e.verts[0], dl, names) if dl else '?'
        lo, hi = worst.get(k, (9.0, 0.0))
        worst[k] = (min(lo, x), max(hi, x))
    bad = sorted(worst.items(), key=lambda kv: -max(kv[1][1] - 1, 1 - kv[1][0]))[:3]
    if label == 'rest':
        rest_pairs = {(a, b) for a, b in pairs if a < b and not set(bm.faces[a].verts) & set(bm.faces[b].verts)}
        print('REST pairs that already cross (accessories sunk into the surface, left out below):', len(rest_pairs))
    print('POSE %-22s stretch max %.2f p99 %.2f | squash min %.2f p1 %.2f | worst at %s | new crossings: %s' % (
        label, stretch[0], stretch[1], squash[0], squash[1],
        ', '.join('%s %.2f-%.2f' % (k, lo, hi) for k, (lo, hi) in bad),
        ', '.join('%s %d' % kv for kv in sorted(groups.items(), key=lambda kv: -kv[1])) or 'none'))
    bm.free()
    pictures.append(label)

# contact sheet: one small render a pose, then tiled
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE' if 'textured' in args[2:] else 'SINGLE'
scene.display.shading.light = 'STUDIO'
scene.render.resolution_x, scene.render.resolution_y = 360, 300
scene.render.film_transparent = False
cam_data = bpy.data.cameras.new('posecam')
cam_data.type = 'ORTHO'
cam_data.ortho_scale = 0.30
cam = bpy.data.objects.new('posecam', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
d = Vector((0.35, -0.45, 0.82)).normalized()
bb = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
cam.location = sum(bb, Vector()) / 8 + d * 2
cam_data.ortho_scale = max((bb[6] - bb[0]).length * 1.1, 0.30)
cam.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()
tiles = []
tmp = os.path.splitext(os.path.abspath(OUT))[0]
ONLY = next((a[5:] for a in args[2:] if a.startswith('only=')), None)
if ONLY:
    POSES = [(l, p) for l, p in POSES if l == ONLY]
    scene.render.resolution_x, scene.render.resolution_y = 900, 750
    VIEWS = [('thumb side', Vector((-1, 0.15, 0.35)).normalized()), ('back', Vector((-0.3, -0.2, 1)).normalized())]
else:
    VIEWS = [(None, None)]
for i, (label, p) in enumerate(POSES):
    set_pose(p)
    for j, (vname, vd) in enumerate(VIEWS):
        if vd is not None:
            # the posed mesh framed from this side
            dg = bpy.context.evaluated_depsgraph_get()
            pts = [obj.matrix_world @ v.co for v in obj.evaluated_get(dg).to_mesh().vertices]
            c = sum(pts, Vector()) / len(pts)
            cam.location = c + vd * 2
            cam.rotation_euler = (-vd).to_track_quat('-Z', 'Z').to_euler()
            cam_data.ortho_scale = max((p_ - c).length for p_ in pts) * 2.4
        scene.render.filepath = '%s-%02d.png' % (tmp, len(tiles))
        bpy.ops.render.render(write_still=True)
        tiles.append(bpy.data.images.load(scene.render.filepath))
cols = 4 if not ONLY else len(VIEWS)
rows = (len(tiles) + cols - 1) // cols
tw, th = (360, 300) if not ONLY else (900, 750)
sheet = [0.2] * (tw * cols * th * rows * 4)
for i, im in enumerate(tiles):
    px = im.pixels[:]
    cx, cy = i % cols, rows - 1 - i // cols
    for y in range(th):
        row = ((cy * th + y) * tw * cols + cx * tw) * 4
        sheet[row:row + tw * 4] = px[y * tw * 4:(y + 1) * tw * 4]
out = bpy.data.images.new('sheet', tw * cols, th * rows)
out.pixels = sheet
out.filepath_raw = os.path.abspath(OUT)
out.file_format = 'PNG'
out.save()
for i in range(len(tiles)):
    os.remove('%s-%02d.png' % (tmp, i))
print('SHEET', OUT, 'order:', ' | '.join(l for l, _ in POSES))
