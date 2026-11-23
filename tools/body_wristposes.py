"""Blender (blender -b --factory-startup re/body/r_arm_sleeve.blend -P tools/body_wristposes.py -- <out-prefix> [edges] [close] [show=<objects>]): our
first-person arm and hand (glove, sleeve, stud) through the wrist's range: bent toward the palm and back, sideways
toward the thumb and the little finger, twisted both ways (the twist bones share the roll: forearm_twist 0.5,
wrist_twist 1.0), and mixes, open and in a fist. Two views per pose (the back three-quarter and the thumb side),
textured, <out-prefix>-NN-<view>.png, for tools/sheet.ps1. Angles about the hand bone: x bends (positive toward the
back of the hand), z sideways, y twists. With `edges`: a light grey surface with its edges as thin dark lines (a
wireframe copy of each part, deformed by the same rig) instead of the textures."""
import bpy, math, os, sys
from mathutils import Vector

OUT = sys.argv[sys.argv.index('--') + 1:][0]
EDGES = 'edges' in sys.argv[sys.argv.index('--') + 1:]
CLOSE = 'close' in sys.argv[sys.argv.index('--') + 1:]  # framed on the wrist (0.20 m across)
POSES = [  # name, (x, y, z) degrees on the hand bone, fist
    ('rest', (0, 0, 0), False),
    ('bent to the palm 60', (-60, 0, 0), False),
    ('bent back 50', (50, 0, 0), False),
    ('sideways +25', (0, 0, 25), False),
    ('sideways -30', (0, 0, -30), False),
    ('twist +80', (0, 80, 0), False),
    ('twist -80', (0, -80, 0), False),
    ('twist +80, bent to the palm 45', (-45, 80, 0), False),
    ('twist -80, bent back 40', (40, -80, 0), False),
    ('fist', (0, 0, 0), True),
    ('fist, bent to the palm 50', (-50, 0, 0), True),
    ('fist, twist +80, sideways -25', (0, 80, -25), True),
]
scene = bpy.context.scene
hand = bpy.data.objects['r_hand_retopo']
rig = hand.parent
shown = next((tuple(a[5:].split(',')) for a in sys.argv[sys.argv.index('--') + 1:] if a.startswith('show=')),
             ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud'))  # show=<objects>: other parts (an armoured forearm)
for o in scene.objects:
    o.hide_render = o.name not in shown
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
scene.display.shading.show_cavity = True
scene.render.resolution_x, scene.render.resolution_y = 600, 600
if EDGES:
    surface = bpy.data.materials.new('surface')
    surface.diffuse_color = (0.78, 0.78, 0.76, 1)
    wire = bpy.data.materials.new('wire')
    wire.diffuse_color = (0.05, 0.05, 0.08, 1)
    scene.display.shading.color_type = 'MATERIAL'
    for n in shown:
        o = bpy.data.objects[n]
        dup = o.copy()
        dup.data = o.data.copy()
        scene.collection.objects.link(dup)
        o.data.materials.clear()
        o.data.materials.append(surface)
        dup.data.materials.clear()
        dup.data.materials.append(wire)
        mod = dup.modifiers.new('wf', 'WIREFRAME')
        mod.thickness = 0.0004
        mod.use_replace = True
        dup.hide_render = False
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
scene.collection.objects.link(cam)
scene.camera = cam
cam.data.type = 'ORTHO'
cam.data.ortho_scale = 0.20 if CLOSE else 0.48
for pb in rig.pose.bones:
    pb.rotation_mode = 'XYZ'
target = rig.matrix_world @ rig.data.bones['hand'].head_local + (Vector((0, 0.01, 0)) if CLOSE else Vector((0, -0.04, 0)))
views = (('back3q', Vector((0.45, -0.25, 1.0))), ('thumb', Vector((1.0, -0.15, 0.1))))
paths = []
for k, (name, deg, fist) in enumerate(POSES):
    for pb in rig.pose.bones:
        pb.rotation_euler = (0, 0, 0)
    rig.pose.bones['hand'].rotation_euler = tuple(math.radians(a) for a in deg)
    if fist:
        for pb in rig.pose.bones:
            for suffix, a in (('_1', -70), ('_2', -80), ('_3', -50)):
                if pb.name.endswith(suffix) and not pb.name.startswith('thumb'):
                    pb.rotation_euler = (math.radians(a), 0, 0)
            if pb.name.startswith('thumb'):
                pb.rotation_euler = (math.radians(-25), 0, 0)
    bpy.context.view_layer.update()
    for vname, d in views:
        dv = d.normalized()
        cam.location = target + dv
        cam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
        path = os.path.abspath('%s-%02d-%s.png' % (OUT, k, vname))
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        paths.append(path)
    print('POSE %02d %s' % (k, name))
print('DONE', len(paths))
