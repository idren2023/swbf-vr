"""Blender (blender -b --factory-startup re/body/r_hand_fit.blend -P tools/body_skeledit.py -- <out.blend>): the
skeleton fitted inside the stock hand in the game's own grip (tools/body_fromstock.py ... fit: no weights, no opening),
set up for editing bones by hand in Blender's window: glove textured and see-through, bones drawn in front with their
names, the view framed on the hand, the armature selected. idren2023 edits bones in Edit Mode (Tab) and saves (Ctrl+S);
tools/body_fromstock.py ... skeleton=<out.blend> then builds from those bones (the frame is the hand frame: wrist at
the origin, +y to the middle knuckle, +z the back of the hand, metres)."""
import bpy, os, sys

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
hand = bpy.data.objects['r_hand_stock']
rig = hand.parent
scene = bpy.context.scene
if not scene.objects.get(hand.name):
    scene.collection.objects.link(hand)
if not scene.objects.get(rig.name):
    scene.collection.objects.link(rig)
arm = rig.data
# each finger's bones joined at their shared joints (moving a joint moves both bones' ends; the fit leaves them
# unconnected: idren2023, the other bones did not follow)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
for eb in arm.edit_bones:
    if eb.parent and eb.parent.name != 'hand' and (eb.head - eb.parent.tail).length < 1e-5:
        eb.use_connect = True
bpy.ops.object.mode_set(mode='OBJECT')
# the bones: octahedral, in front of the glove, named, axes shown (x = the bending axis)
arm.display_type = 'OCTAHEDRAL'
arm.show_names = True
arm.show_axes = True
rig.show_in_front = True
# the glove: textured, see-through
for area in [a for s in bpy.data.screens for a in s.areas if a.type == 'VIEW_3D']:
    sp = area.spaces[0]
    sp.shading.type = 'SOLID'
    sp.shading.color_type = 'TEXTURE'
    sp.shading.show_xray = True
    sp.shading.xray_alpha = 0.6
    sp.overlay.show_floor = False
    sp.overlay.show_axis_x = sp.overlay.show_axis_y = False
    sp.clip_start = 0.001
    r3d = sp.region_3d
    r3d.view_location = (0.0, 0.07, -0.02)
    r3d.view_distance = 0.35
for o in scene.objects:
    o.select_set(o is rig)
bpy.context.view_layer.objects.active = rig
scene.unit_settings.length_unit = 'MILLIMETERS'
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
print('SKELEDIT saved', OUT, 'bones:', ', '.join(b.name for b in arm.bones))
