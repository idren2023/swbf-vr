"""Step 3 of the bake plan: the detailed model's surface baked as a tangent-space normal map onto a target mesh in the
stock's texture layout (the game mesh, or the stock itself for comparison), then the target drawn with the 4x texture
and that normal map in Cycles beside the others under the same light.
Run: blender -b <highpoly.blend> -P tools/char_bake.py -- <target.blend> <target object> <name> <out dir> [size]
  (the stock: re/body/all_inf_soldier.blend all_inf_soldier_hi stock; the game mesh: re/body/lowpoly.blend body_low game)
"""
import bpy, bmesh, os, sys
from mathutils import Matrix

tgt_blend, tgt_name, name, out = sys.argv[sys.argv.index('--') + 1:][:4]
size = int((sys.argv[sys.argv.index('--') + 1:] + ['2048'])[4])
out = os.path.abspath(out)
sc = bpy.context.scene
high = [o for o in sc.objects if o.name.startswith('hp_')]
with bpy.data.libraries.load(os.path.abspath(tgt_blend)) as (src, dst):
    dst.objects = [tgt_name]
tgt = dst.objects[0]
sc.collection.objects.link(tgt)
tgt.parent = None
tgt.matrix_world = Matrix.Identity(4)
for m in list(tgt.modifiers):
    tgt.modifiers.remove(m)
tgt.data = tgt.data.copy()
# the stock's main material only (its packs and face use other textures)
bm = bmesh.new()
bm.from_mesh(tgt.data)
bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != 0], context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
bm.to_mesh(tgt.data)
for p in tgt.data.polygons:
    p.use_smooth = True
    p.material_index = 0
img = bpy.data.images.new('nrm_' + name, size, size, alpha=False, float_buffer=False)
img.colorspace_settings.name = 'Non-Color'
mat = bpy.data.materials.new('bake_' + name)
mat.use_nodes = True
nt = mat.node_tree
tn = nt.nodes.new('ShaderNodeTexImage')
nt.nodes.active = tn  # (textured views draw the active image node)
tn.image = img
nt.nodes.active = tn
tgt.data.materials.clear()
tgt.data.materials.append(mat)
sc.render.engine = 'CYCLES'
sc.cycles.device = 'CPU'
sc.cycles.samples = 1
bk = sc.render.bake
bk.use_selected_to_active = True
bk.cage_extrusion = 0.02
bk.max_ray_distance = 0.05
bk.margin = 8
bk.normal_space = 'TANGENT'
for o in sc.objects:
    o.select_set(o in high or o is tgt)
bpy.context.view_layer.objects.active = tgt
bpy.ops.object.bake(type='NORMAL', use_clear=True, target='IMAGE_TEXTURES')
# texels the rays missed (left black), and normals pointing into the surface (a ray that met the inside of a garment
# shell: blue under one half), filled with the flat normal: no black spots
import numpy as np  # noqa: E402
px = np.array(img.pixels[:], np.float32).reshape(size, size, 4)
miss = (px[..., :3].max(-1) < 0.05) | (px[..., 2] < 0.5)
px[miss, :3] = (0.5, 0.5, 1.0)
img.pixels[:] = px.ravel()
print('BAKE missed texels filled: %d' % miss.sum())
img.filepath_raw = os.path.join(out, 'normal-%s.png' % name)
img.file_format = 'PNG'
img.save()
print('BAKE normal map -> %s' % img.filepath_raw)
# saved with the target alone, ready to draw
for o in high:
    bpy.data.objects.remove(o)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, 'baked-%s.blend' % name))
