"""Blender (blender -b --factory-startup <hand.blend> -P tools/body_thumbview.py -- <out.png> [pose-label] [bone=x,y,z ..]): the thumb's
three bones alone, orthographic along the thumb base bone's own axes (rest-pose frame): looking along its x (the
bending axis: shows the bends), along its z (shows the sideways swing), and down its length (y); the rest pose in
grey, the posed bones coloured (base purple, middle magenta, tip lavender), joints as balls, a 1 cm scale bar."""
import bpy, math, os, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
LABEL = args[1] if len(args) > 1 else 'thumbs up'
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'body_poses.py')).read()
ns = {}
exec(src[src.index('FINGERS = '):src.index('scene = bpy.context.scene')], ns)
POSE = next(p for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(LABEL))
POSE = dict(POSE)
for a in args[2:]:  # overrides: bone=x,y,z (degrees), e.g. thumb_3=45,0,0
    n, v = a.split('=')
    POSE[n] = tuple(float(x) for x in v.split(','))

scene = bpy.context.scene
obj = bpy.data.objects['r_hand_stock']
rig = obj.parent
for o in scene.objects:
    o.hide_render = True
THUMB = ('thumb_1', 'thumb_2', 'thumb_3')
COL = {'thumb_1': (0.4, 0.0, 0.55, 1), 'thumb_2': (0.85, 0.15, 0.85, 1), 'thumb_3': (0.75, 0.65, 1.0, 1), 'rest': (0.5, 0.5, 0.5, 1)}
mats = {}
for k, c in COL.items():
    m = bpy.data.materials.new(k)
    m.diffuse_color = c
    mats[k] = m


def set_pose(p):
    for pb in rig.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
    for n, d in p.items():
        rig.pose.bones[n].rotation_euler = [math.radians(x) for x in d]
    bpy.context.view_layer.update()


def chain(mat_of):
    made = []
    for n in THUMB:
        pb = rig.pose.bones[n]
        h, t = rig.matrix_world @ pb.head, rig.matrix_world @ pb.tail
        bpy.ops.mesh.primitive_cylinder_add(radius=0.0016, depth=(t - h).length, location=(h + t) / 2, vertices=12)
        c = bpy.context.object
        c.rotation_euler = (t - h).to_track_quat('Z', 'Y').to_euler()
        c.data.materials.append(mats[mat_of(n)])
        made.append(c)
        for p in (h, t):
            bpy.ops.mesh.primitive_uv_sphere_add(radius=0.0028, location=p, segments=12, ring_count=8)
            bpy.context.object.data.materials.append(mats[mat_of(n)])
            made.append(bpy.context.object)
    return made


set_pose({})
base = rig.matrix_world @ rig.pose.bones['thumb_1'].matrix  # the base bone's rest frame
B = base.to_3x3().normalized()
origin = base.translation
chain(lambda n: 'rest')
set_pose(POSE)
chain(lambda n: n)
# 1 cm scale bar at the base joint, along the base bone's x
bpy.ops.mesh.primitive_cylinder_add(radius=0.0006, depth=0.01, location=origin + B.col[0] * 0.005 - B.col[2] * 0.012)
bar = bpy.context.object
bar.rotation_euler = B.col[0].to_track_quat('Z', 'Y').to_euler()
bar.data.materials.append(mats['rest'])

scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'MATERIAL'
scene.display.shading.light = 'FLAT'
TW, TH = 600, 600
scene.render.resolution_x, scene.render.resolution_y = TW, TH
scene.world = bpy.data.worlds.new('w') if not scene.world else scene.world
cd = bpy.data.cameras.new('t')
cd.type = 'ORTHO'
cd.ortho_scale = 0.13
cam = bpy.data.objects.new('t', cd)
scene.collection.objects.link(cam)
scene.camera = cam
tip_rest = None
centre = origin + B.col[1] * 0.045
VIEWS = (('along x: bends', B.col[0], B.col[1]), ('along z: sideways', B.col[2], B.col[1]), ('down the bone (y)', -B.col[1], B.col[2]))
tiles = []
tmp = os.path.abspath(OUT + '.tmp.png')
for name, d, up in VIEWS:
    d = d.normalized()
    up = (up - d * up.dot(d)).normalized()
    cam.matrix_world = Matrix.Translation(centre + d * 0.5) @ Matrix((d.cross(up) * -1, up, d)).transposed().to_4x4()
    scene.render.filepath = tmp
    bpy.ops.render.render(write_still=True)
    im = bpy.data.images.load(tmp, check_existing=False)
    tiles.append(im.pixels[:])
    bpy.data.images.remove(im)
os.remove(tmp)
sheet = [0.0] * (TW * 3 * TH * 4)
for i, px in enumerate(tiles):
    for y in range(TH):
        o = (y * TW * 3 + i * TW) * 4
        sheet[o:o + TW * 4] = px[y * TW * 4:(y + 1) * TW * 4]
im = bpy.data.images.new('thumb', TW * 3, TH)
im.pixels = sheet
im.filepath_raw = os.path.abspath(OUT)
im.file_format = 'PNG'
im.save()
print('POSE', LABEL, {n: POSE.get(n) for n in THUMB})
