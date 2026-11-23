"""Blender (blender -b --factory-startup re/body/r_arm_weapons.blend -P tools/body_stockcompare.py -- <weapon> <out.png> [grip]):
the stock first-person arm holding a weapon next to ours, at the same scale and from the same cameras (idren2023
2026-10-23: where do we go wrong). Left: the stock hand in its own grip (r_hand_stock of re/body/r_hand_fit.blend, the
stock mesh in our hand frame), the stock forearm and stud, the weapon where the stock game hangs it (hp_weapons on the
wrist: tools/body_weapons.py's placement, kept in re/body/wbase.blend). Right: ours as the game holds it now
(tools/body_poses.py weapon=: fitted pose, wrist straightened, the weapon on our grip). Rows: thumb side, palm 3/4,
back 3/4, from below, then behind left and behind right (where the player looks from); with `grip` closer on the grip: from the muzzle, the far side (thumb, fingertips), the far
side low. <weapon>: pistol, rifle or thermaldetonator."""
import bpy, math, os, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
WEAPON, OUT = args[0], args[1]
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
# ours: the pose renderer's own setup (rig, poses, weapon= wrist step), without its rendering
sys.argv = ['blender', '--', OUT + '-unused', 'r_hand_retopo', 'weapon=' + WEAPON]
src = open(os.path.join(HERE, 'body_poses.py')).read()
ns = {'__file__': os.path.join(HERE, 'body_poses.py')}
exec(compile(src[:src.index('for group, poses in GROUPS:')], 'body_poses', 'exec'), ns)
label = ns['WEAPON_POSES'][WEAPON]
ns['set_pose'](dict(next(p for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(label))))
scene = bpy.context.scene
ours = [bpy.data.objects[n] for n in ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud', 'weap_' + WEAPON)]
# the stock side: hand in its grip, forearm, stud, and the weapon at the stock place
with bpy.data.libraries.load(os.path.abspath('re/body/r_hand_fit.blend')) as (src_, dst_):
    dst_.meshes = ['r_hand_stock']
stock_hand = bpy.data.objects.new('stock_hand', dst_.meshes[0])
with bpy.data.libraries.load(os.path.abspath('re/body/wbase.blend')) as (src_, dst_):
    dst_.objects = ['weap_' + WEAPON]
stock_weapon = dst_.objects[0]
stock_weapon.name = 'stock_weapon'
stock_weapon.parent = None
stock = [stock_hand, stock_weapon]
for o in stock:
    scene.collection.objects.link(o)
stock_hand.data.materials.clear()
stock_hand.data.materials.append(bpy.data.objects['r_hand_retopo'].data.materials[0])
for n in ('r_forearm_stock', 'r_forearm_stud'):
    if n in bpy.data.objects:
        stock.append(bpy.data.objects[n])
# the stock weapon in wbase was parented to the hand bone at rest: its world matrix there is its place
stock_weapon.matrix_world = Matrix.Identity(4)

scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
TW, TH = 700, 600
scene.render.resolution_x, scene.render.resolution_y = TW, TH
cd = bpy.data.cameras.new('cmp')
cd.type = 'ORTHO'
cd.ortho_scale = 0.42 if 'grip' not in args[2:] else 0.2
cam = bpy.data.objects.new('cmp', cd)
scene.collection.objects.link(cam)
scene.camera = cam
centre = Vector((0.0, 0.03, -0.04)) if 'grip' not in args[2:] else Vector((-0.01, 0.075, -0.06))
VIEWS = (('thumb side', Vector((-1, 0.1, 0.25)), Vector((0, 0, 1))),
         ('palm 3/4', Vector((-0.5, 0.3, -0.8)), Vector((0, 1, 0))),
         ('back 3/4', Vector((-0.4, -0.3, 0.85)), Vector((0, 1, 0))),
         ('below', Vector((0.1, 0.05, -1)), Vector((0, 1, 0))))
# from behind the weapon where the player's eyes are (idren2023 2026-10-24): the hand frame's -y is back toward the
# player, -x up (the thumb side up when the gun is held level), -z the player's left
BEHIND_VIEWS = (('behind, left (player view)', Vector((-0.15, -1, -0.35)), Vector((-1, 0, 0))),
                ('behind, right (player view)', Vector((-0.15, -1, 0.8)), Vector((-1, 0, 0))),
                ('behind, top down', Vector((-1, -0.45, 0.1)), Vector((0, 1, 0))))
if 'grip' in args[2:]:
    # the grip's far side and front (where the thumb and the fingertips lie), closer
    VIEWS = (('from the muzzle', Vector((-0.15, 1, -0.35)), Vector((0, 0, 1))),
             ('far side', Vector((-0.35, 0.25, -1)), Vector((0, 1, 0))),
             ('far side, low', Vector((0.35, 0.25, -1)), Vector((0, 1, 0))))
    cd_scale = 0.2
VIEWS = VIEWS + BEHIND_VIEWS
tmp = os.path.abspath(OUT + '.tmp.png')
tiles = []
base_scale = cd.ortho_scale
for name, d, up in VIEWS:
    d = d.normalized()
    up = (up - d * up.dot(d)).normalized()
    # the player views closer, on the hand round the grip
    is_behind = name.startswith('behind')
    cd.ortho_scale = 0.24 if is_behind else base_scale
    view_centre = Vector((-0.005, 0.07, -0.05)) if is_behind else centre
    cam.matrix_world = Matrix.Translation(view_centre + d * 2) @ Matrix((d.cross(up) * -1, up, d)).transposed().to_4x4()
    for side in (stock, ours):
        for o in scene.objects:
            o.hide_render = o not in side
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
print('STOCKCOMPARE', WEAPON, '->', OUT, '(left stock, right ours; rows: %s)' % ', '.join(v[0] for v in VIEWS))
