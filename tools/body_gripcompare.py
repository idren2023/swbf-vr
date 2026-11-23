"""Blender (blender -b --factory-startup <hand+weapons.blend> -P tools/body_gripcompare.py -- <weapon-object> <out.png>
[pose-label]): the stock first-person hand gripping a weapon next to ours in a pose (default: the stock grip), at the
same scale and from the same cameras: left the stock hand (its own mesh, in the hand frame: re/body/r_hand_fit.blend),
right ours, each with the weapon where the game puts it; rows: thumb side, palm 3/4, back 3/4, from below."""
import bpy, math, os, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
WEAPON, OUT = args[0], args[1]
LABEL = args[2] if len(args) > 2 else 'stock grip'
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'body_poses.py')).read()
ns = {}
exec(src[src.index('FINGERS = '):src.index('scene = bpy.context.scene')], ns)
POSE = next(p for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(LABEL))
scene = bpy.context.scene
ours = bpy.data.objects['r_hand_stock']
rig = ours.parent
weap = bpy.data.objects[WEAPON]
with bpy.data.libraries.load(os.path.abspath('re/body/r_hand_fit.blend')) as (s_, d_):
    d_.meshes = ['r_hand_stock']
stock = bpy.data.objects.new('stock_hand', d_.meshes[0])
scene.collection.objects.link(stock)
stock.data.materials.clear()
stock.data.materials.append(ours.data.materials[0])
for pb in rig.pose.bones:
    pb.rotation_mode = 'XYZ'
    pb.rotation_euler = (0, 0, 0)
for n, d in POSE.items():
    if n in rig.pose.bones:
        pb = rig.pose.bones[n]
        pb.rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
        pb.rotation_euler = [math.radians(x) for x in d]
bpy.context.view_layer.update()

scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
TW, TH = 700, 600
scene.render.resolution_x, scene.render.resolution_y = TW, TH
cd = bpy.data.cameras.new('cmp')
cd.type = 'ORTHO'
cd.ortho_scale = 0.22
cam = bpy.data.objects.new('cmp', cd)
scene.collection.objects.link(cam)
scene.camera = cam
centre = Vector((0.0, 0.07, -0.03))
VIEWS = (('thumb side', Vector((-1, 0.1, 0.25)), Vector((0, 0, 1))), ('palm 3/4', Vector((-0.5, 0.3, -0.8)), Vector((0, 1, 0))),
         ('back 3/4', Vector((-0.4, -0.3, 0.85)), Vector((0, 1, 0))), ('below', Vector((0.1, 0.05, -1)), Vector((0, 1, 0))))
tmp = os.path.abspath(OUT + '.tmp.png')
tiles = []
for name, d, up in VIEWS:
    d = d.normalized()
    up = (up - d * up.dot(d)).normalized()
    cam.matrix_world = Matrix.Translation(centre + d * 2) @ Matrix((d.cross(up) * -1, up, d)).transposed().to_4x4()
    for hand in (stock, ours):
        for o in scene.objects:
            o.hide_render = o not in (hand, weap)
        scene.render.filepath = tmp
        bpy.ops.render.render(write_still=True)
        im = bpy.data.images.load(tmp, check_existing=False)
        tiles.append(im.pixels[:])
        bpy.data.images.remove(im)
os.remove(tmp)
cols, rows = 2, len(VIEWS)
sheet = [0.0] * (TW * cols * TH * rows * 4)
for i, px in enumerate(tiles):
    cx, cy = i % cols, rows - 1 - i // cols
    for y in range(TH):
        o = ((cy * TH + y) * TW * cols + cx * TW) * 4
        sheet[o:o + TW * 4] = px[y * TW * 4:(y + 1) * TW * 4]
im = bpy.data.images.new('cmp', TW * cols, TH * rows)
im.pixels = sheet
im.filepath_raw = os.path.abspath(OUT)
im.file_format = 'PNG'
im.save()
print('COMPARE', WEAPON, LABEL, '->', OUT)
