"""The stock character subdivided, displaced by its relief map (tools/char_relief.py), drawn with the 4x texture,
beside the plain stock, from the same cameras. Saves the relief model to <out dir>/relief.blend.
Run: blender -b --factory-startup <stock.blend> -P tools/char_relief_view.py -- <object> <height.png> <hd.png> <out dir>
"""
import bpy, bmesh, json, os, sys
import numpy as np
from mathutils import Vector, Matrix

obj_name, height, hd, out = sys.argv[sys.argv.index('--') + 1:][:4]
out = os.path.abspath(out)
rng = json.load(open(height.rsplit('.', 1)[0] + '.json'))
sc = bpy.context.scene
stock = bpy.data.objects[obj_name]
stock.parent = None
stock.matrix_world = Matrix.Identity(4)
for m in list(stock.modifiers):
    stock.modifiers.remove(m)
hd_img = bpy.data.images.load(os.path.abspath(hd))
for m in stock.data.materials:
    if m and m.use_nodes:
        for n in m.node_tree.nodes:
            if n.type == 'TEX_IMAGE' and n.image and n.image.name == 'All_Inf_trooper.tga':
                n.image = hd_img
rel = stock.copy()
rel.data = stock.data.copy()
rel.name = 'relief'
sc.collection.objects.link(rel)
# welded at the texture seams (a game mesh splits its points there: displaced apart they would crack)
bm = bmesh.new()
bm.from_mesh(rel.data)
n0 = len(bm.verts)
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=0.0002)
bm.to_mesh(rel.data)
print('RELIEF welded %d -> %d points' % (n0, len(rel.data.vertices)))
sub = rel.modifiers.new('sub', 'SUBSURF')
sub.levels = sub.render_levels = int(os.environ.get('RELIEF_SUBD', '2'))
sub.uv_smooth = 'PRESERVE_BOUNDARIES'
himg = bpy.data.images.load(os.path.abspath(height))
himg.colorspace_settings.name = 'Non-Color'
tex = bpy.data.textures.new('height', 'IMAGE')
tex.image = himg
tex.extension = 'EXTEND'
# displaced: the main material's faces only (the packs use their own texture and keep their shape)
vg = rel.vertex_groups.new(name='relief')
vg.add(sorted({v for p in rel.data.polygons if p.material_index == 0 for v in p.vertices}), 1.0, 'REPLACE')
disp = rel.modifiers.new('disp', 'DISPLACE')
disp.texture = tex
disp.texture_coords = 'UV'
disp.vertex_group = 'relief'
disp.uv_layer = rel.data.uv_layers.active.name
span = (rng['max_mm'] - rng['min_mm']) / 1000
disp.strength = span
disp.mid_level = -rng['min_mm'] / 1000 / span
for p in rel.data.polygons:
    p.use_smooth = True
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'
sc.display.shading.show_cavity = True
sc.display.shading.color_type = 'TEXTURE'
sc.render.resolution_x = sc.render.resolution_y = 640
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.type = 'ORTHO'
VIEWS = [('front', (0, 0, 1.3), (0, -1, 0.1), 1.2), ('3q', (0, 0, 1.3), (-0.7, -0.7, 0.25), 1.2),
         ('chest', (0, -0.1, 1.55), (0, -1, 0.2), 0.55), ('backtop', (0, 0.1, 1.6), (0, 1, 0.6), 0.6),
         ('side', (0, 0, 1.45), (-1, 0, 0.1), 0.7), ('belt', (0, -0.1, 1.15), (-0.3, -1, 0.1), 0.4)]
os.makedirs(out, exist_ok=True)
for who, ob in (('relief', rel), ('stock', stock)):
    for o in sc.objects:
        if o.type == 'MESH':
            o.hide_render = o is not ob
    for name, t, d, s_ in VIEWS:
        dv = Vector(d).normalized()
        cam.data.ortho_scale = s_
        cam.location = Vector(t) + dv * 3
        cam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
        sc.render.filepath = os.path.join(out, '%s-%s.png' % (who, name))
        bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, 'relief.blend'))
print('RELIEF views -> %s' % out)
