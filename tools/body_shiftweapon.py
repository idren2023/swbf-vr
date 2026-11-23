"""Blender (blender -b --factory-startup re/body/r_arm_weapons.blend -P tools/body_shiftweapon.py -- <weapon> <x> <y> <z>):
moves a weapon in the hand by x, y, z mm in the hand frame (x toward the little finger, y toward the knuckles, z the back
of the hand; a hand moved forward on the gun is the gun moved by -y), keeps re/body/grip-weap_<weapon>.json's place and
grip axis with it, and saves the file. For hand adjustments idren2023 asks for (2026-10-24)."""
import bpy, json, os, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
weap = args[0]
shift = Vector([float(a) / 1000 for a in args[1:4]])
w = bpy.data.objects['weap_' + weap]
rig = bpy.data.objects['r_hand_retopo'].parent
w.matrix_world = Matrix.Translation(shift) @ w.matrix_world
bpy.context.view_layer.update()
jp = os.path.join('re', 'body', 'grip-weap_%s.json' % weap)
g = json.load(open(jp))
hb = rig.matrix_world @ rig.data.bones['hand'].matrix_local
g['in_hand_bone'] = [list(r) for r in hb.inverted() @ w.matrix_world]
g['axis_point'] = list(Vector(g['axis_point']) + shift)
json.dump(g, open(jp, 'w'), indent=1)
bpy.ops.wm.save_mainfile()
print('SHIFT %s by %s mm' % (weap, ' '.join(args[1:4])))
