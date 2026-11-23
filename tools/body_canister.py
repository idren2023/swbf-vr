"""Blender (blender -b --factory-startup re/body/r_arm_weapons.blend -P tools/body_canister.py): the manual reload's
ammo canister (item_powerup_ammo.msh) put in our hand as the game holds it (Holster: the canister's middle on the
controller's grip pose, its length along the handle toward the thumb, drawn at 0.27): here on the grip axis our hand
closes round (the rifle's grip axis point and direction, re/body/grip-weap_rifle.json, the same handle line BodyArm puts
on the grip pose), scaled into the hand frame (0.27 x 0.712). Saves object weap_ammocanister and
re/body/grip-weap_ammocanister.json (its grip axis: the canister's middle and length) for tools/body_tunegrip.py."""
import bpy, json, os, sys
from mathutils import Matrix, Vector
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

MSH = 'C:/LucasArts/BFBuilder/Assets/Shipped Add On/DataTAT3/Common/MSHs/item_powerup_ammo.msh'
SCALE = 0.27 * 0.7116788
CENTRE = Vector((0.0135, 0.239, 0.0))      # the model's middle (Holster AMMO_MODEL_CENTRE)
rig = bpy.data.objects['r_hand_retopo'].parent
g = json.load(open('re/body/grip-weap_rifle.json'))
axis_point = Vector(g['axis_point'])
along = -Vector(g['axis_direction']).normalized()   # toward the thumb (the canister's +y)
# the canister's frame in the hand: y along the handle toward the thumb, z the hand's palm normal side
z_ = Vector((0, 0, 1))
z_ = (z_ - along * z_.dot(along)).normalized()
x_ = along.cross(z_).normalized()
R = Matrix((x_, along, z_)).transposed()
model = mshgeo.load(MSH)
verts, faces, uvs = [], [], []
for node in model['nodes']:
    if not mshgeo.is_drawn(node):
        continue
    for seg in node['segments']:
        base = len(verts)
        for p in seg['wpos']:
            q = Vector((p[0], p[1], p[2])) - CENTRE    # msh space as the game draws it (the axes as they are)
            verts.append(axis_point + R @ (q * SCALE))
        for a, b, c in seg['tris']:
            faces.append((base + a, base + b, base + c))
            uvs.append([seg['uv'][a], seg['uv'][b], seg['uv'][c]] if seg.get('uv') else None)
for n in ('weap_ammocanister',):
    if n in bpy.data.objects:
        bpy.data.objects.remove(bpy.data.objects[n])
me = bpy.data.meshes.new('weap_ammocanister')
me.from_pydata(verts, [], faces)
if all(uvs):
    uvl = me.uv_layers.new(name='UV')
    for poly, tuv in zip(me.polygons, uvs):
        for j, li in enumerate(poly.loop_indices):
            uvl.data[li].uv = tuv[j]
mat = bpy.data.materials.new('item_powerup_ammo')
mat.use_nodes = True
img = mat.node_tree.nodes.new('ShaderNodeTexImage')
mat.node_tree.nodes.active = img  # (textured views draw the active image node)
tga = MSH.replace('.msh', '.tga')
if os.path.exists(tga):
    img.image = bpy.data.images.load(tga)
mat.node_tree.links.new(img.outputs['Color'], mat.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
me.materials.append(mat)
ob = bpy.data.objects.new('weap_ammocanister', me)
bpy.context.scene.collection.objects.link(ob)
ob.parent = rig
ob.parent_type = 'BONE'
ob.parent_bone = 'hand'
ob.matrix_world = Matrix.Identity(4)
json.dump({'weapon': 'weap_ammocanister', 'pose': 'ammo canister', 'in_hand_bone': [list(r) for r in Matrix.Identity(4)],
           'axis_point': list(axis_point), 'axis_direction': list(-along), 'placement': 'the game\'s (Holster)'},
          open('re/body/grip-weap_ammocanister.json', 'w'), indent=1)
lo = [min(v[k] for v in verts) for k in range(3)]
hi = [max(v[k] for v in verts) for k in range(3)]
print('CANISTER %d tris, in the hand frame %s .. %s mm' % (len(faces), [round(x * 1000) for x in lo],
                                                          [round(x * 1000) for x in hi]))
bpy.ops.wm.save_mainfile()
