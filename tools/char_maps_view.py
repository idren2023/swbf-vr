"""The fabric maps painted on the stock character in 3D (tools/char_fabrics.py's label texture in place of its own),
from six sides: to check each map where it is worn, not on the unfolded texture.
Run: blender -b --factory-startup <stock.blend> -P tools/char_maps_view.py -- <object> <label.png> <out prefix>
"""
import bpy, os, sys
from mathutils import Vector, Matrix

obj_name, label, prefix = sys.argv[sys.argv.index('--') + 1:][:3]
sc = bpy.context.scene
ob = bpy.data.objects[obj_name]
ob.parent = None
ob.matrix_world = Matrix.Identity(4)
for m in list(ob.modifiers):
    ob.modifiers.remove(m)
img = bpy.data.images.load(os.path.abspath(label))
for m in ob.data.materials:
    if m and m.use_nodes:
        for n in m.node_tree.nodes:
            if n.type == 'TEX_IMAGE' and n.image and m is ob.data.materials[0]:
                n.image = img
for o in sc.objects:
    if o.type == 'MESH':
        o.hide_render = o is not ob
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'FLAT'
sc.display.shading.color_type = 'TEXTURE'
sc.render.resolution_x = sc.render.resolution_y = 560
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.type = 'ORTHO'
VIEWS = [('hemfront', (0, -0.1, 1.27), (0, -1, 0.05), 0.35), ('hem3q', (0.05, -0.1, 1.27), (0.5, -1, 0.05), 0.35), ('front', (0, 0, 1.0), (0, -1, 0.1), 2.1), ('back', (0, 0, 1.0), (0, 1, 0.1), 2.1),
         ('side', (0, 0, 1.0), (-1, 0, 0.1), 2.1), ('chest', (0, -0.1, 1.5), (0, -1, 0.2), 0.6),
         ('backtop', (0, 0.1, 1.6), (0, 1, 0.6), 0.6), ('waist', (0, 0, 1.15), (-0.6, -1, 0.1), 0.6)]
for name, t, d, s_ in VIEWS:
    dv = Vector(d).normalized()
    cam.data.ortho_scale = s_
    cam.location = Vector(t) + dv * 3
    cam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
    sc.render.filepath = os.path.abspath('%s-%s.png' % (prefix, name))
    bpy.ops.render.render(write_still=True)
print('MAPSVIEW -> %s-*.png' % prefix)
