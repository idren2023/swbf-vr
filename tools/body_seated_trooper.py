"""Blender (blender -b re/body/seated/rep_inf_trooper.blend -P tools/body_seated_trooper.py -- <out.json>): poses the
stock clone trooper (tools/body_import.py's armature of rep_inf_trooper.msh, T pose, facing -y, z up) seated: thighs
level ahead, calves down, arms down with the forearms on the thighs, head a little bowed; writes the posed stock
mesh's triangles (positions in Blender space, uvs, texture) as JSON for tools/seated_trooper.py (the AT-TE troop bay,
idren2023 2026-11-08). Generated from the game's model: the output stays out of git."""
import bpy, json, math, sys
from mathutils import Matrix, Vector

OUT = sys.argv[sys.argv.index('--') + 1]
arm = bpy.data.objects['rep_inf_trooper_skeleton']
mesh = bpy.data.objects['rep_inf_trooper']
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='POSE')


def turn(name, axis, degrees):
    """Turns a pose bone (and so its children) about an armature-space axis through its head."""
    bpy.context.view_layer.update()
    pb = arm.pose.bones[name]
    head = pb.matrix.to_translation()
    rot = Matrix.Rotation(math.radians(degrees), 4, axis)
    pb.matrix = Matrix.Translation(head) @ rot @ Matrix.Translation(-head) @ pb.matrix


def aim(name, target):
    """Turns a pose bone about its head so it points at an armature-space target."""
    bpy.context.view_layer.update()
    pb = arm.pose.bones[name]
    head = pb.head.copy()
    rot = (pb.tail - head).rotation_difference(Vector(target) - head).to_matrix().to_4x4()
    pb.matrix = Matrix.Translation(head) @ rot @ Matrix.Translation(-head) @ pb.matrix


for side in ('l', 'r'):
    turn('bone_%s_thigh' % side, 'X', -88)
    turn('bone_%s_calf' % side, 'X', 92)
    turn('bone_%s_foot' % side, 'X', -4)
turn('bone_l_upperarm', 'Y', 72)
turn('bone_r_upperarm', 'Y', -72)
turn('bone_l_upperarm', 'X', -18)
turn('bone_r_upperarm', 'X', -18)
for side, x in (('l', 1), ('r', -1)):
    aim('bone_%s_upperarm' % side, (x * 0.27, 0.02, 1.36))  # elbows by the sides, a little back
    aim('bone_%s_forearm' % side, (x * 0.20, -0.32, 1.10))  # hands on the thighs
turn('bone_a_spine', 'X', -6)
turn('bone_head', 'X', -12)
bpy.context.view_layer.update()
bpy.ops.object.mode_set(mode='OBJECT')

dg = bpy.context.evaluated_depsgraph_get()
ev = mesh.evaluated_get(dg)
m = ev.to_mesh()
m.calc_loop_triangles()
uv = m.uv_layers.active.data
world = mesh.matrix_world
textures = []
for mat in mesh.data.materials:
    name = mat.name if mat else ''
    if mat and mat.use_nodes:
        for node in mat.node_tree.nodes:
            if node.type == 'TEX_IMAGE' and node.image:
                name = node.image.name
    textures.append(name)
tris = []
for t in m.loop_triangles:
    ps = [list(world @ m.vertices[v].co) for v in t.vertices]
    uvs = [list(uv[l].uv) for l in t.loops]
    tris.append({'p': ps, 'uv': uvs, 't': textures[t.material_index] if textures else ''})
json.dump({'tris': tris}, open(OUT, 'w'))
zs = [p[2] for tr in tris for p in tr['p']]
ys = [p[1] for tr in tris for p in tr['p']]
print('SEATED tris', len(tris), 'z %.2f..%.2f y %.2f..%.2f' % (min(zs), max(zs), min(ys), max(ys)), set(textures))
