"""The game mesh with its edges, each face coloured by the fabric under it (to see whether its loops follow the fabric
borders), from five sides. Run: blender -b <lowpoly.blend> -P tools/char_lowpoly_view.py -- <config.json> <out prefix>
"""
import bpy, json, os, sys
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

cfg_path, prefix = sys.argv[sys.argv.index('--') + 1:][:2]
cfg = json.load(open(cfg_path))
sc = bpy.context.scene
low, src = bpy.data.objects['body_low'], bpy.data.objects['lp_source']
N = 2048


def load_mask(path):
    im = bpy.data.images.load(os.path.abspath(path))
    W, H = im.size
    a = np.array(im.pixels[:], np.float32).reshape(H, W, 4)[::-1, :, 0] > 0.5
    bpy.data.images.remove(im)
    return a


fabrics = list(cfg['fabrics'])
label = np.zeros((N, N), np.int32)
for i, f in enumerate(fabrics):
    label[load_mask(cfg['out'][f])] = i + 1
dg = bpy.context.evaluated_depsgraph_get()
bvh = BVHTree.FromObject(src, dg)
suv = src.data.uv_layers.active.data
low.data.materials.clear()
mats = []
for i, f in enumerate(['none'] + fabrics):
    m = bpy.data.materials.new('f_' + f)
    m.diffuse_color = tuple(cfg['colours'].get(f, [0.5, 0.5, 0.5])) + (1,)
    low.data.materials.append(m)
fab_attr = low.data.attributes.get('fabric')
fab_vals = [d.value for d in fab_attr.data] if fab_attr is not None else None    # read first: writing material
for p in low.data.polygons:                                                          # indices moves the attributes
    if fab_vals is not None:
        p.material_index = fab_vals[p.index]
        continue
    loc, _, fi, _ = bvh.find_nearest(p.center)
    sp = src.data.polygons[fi]
    uv = sum((suv[li].uv for li in sp.loop_indices), Vector((0, 0))) / len(sp.loop_indices)
    p.material_index = int(label[min(int((1 - uv.y % 1) * N), N - 1), min(int(uv.x % 1 * N), N - 1)])
wire = bpy.data.objects.new('wire', low.data.copy())
sc.collection.objects.link(wire)
wm = wire.modifiers.new('w', 'WIREFRAME')
wm.thickness = 0.0012
wm.use_replace = True
blk = bpy.data.materials.new('blk')
blk.diffuse_color = (0, 0, 0, 1)
wire.data.materials.clear()
wire.data.materials.append(blk)
src.hide_render = True
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'
sc.display.shading.color_type = 'MATERIAL'
sc.render.resolution_x = sc.render.resolution_y = 700
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.type = 'ORTHO'
for name, t, d, s_ in [('front', (0, 0, 1.0), (0, -1, 0.1), 2.0), ('back', (0, 0, 1.0), (0, 1, 0.1), 2.0),
                       ('chest', (0, -0.1, 1.5), (0.3, -1, 0.3), 0.6), ('backtop', (0, 0.1, 1.6), (0, 1, 0.6), 0.6),
                       ('shoulder', (-0.25, 0, 1.55), (-0.7, -0.7, 0.3), 0.5),
                       ('elbow', (-0.5, 0, 1.6), (0, -1, 0.3), 0.45), ('knee', (-0.12, 0, 0.5), (-0.3, -1, 0.1), 0.6)]:
    dv = Vector(d).normalized()
    cam.data.ortho_scale = s_
    cam.location = Vector(t) + dv * 3
    cam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
    sc.render.filepath = os.path.abspath('%s-%s.png' % (prefix, name))
    bpy.ops.render.render(write_still=True)
