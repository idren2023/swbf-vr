"""Draws baked targets (tools/char_bake.py) with the 4x texture, with and without their normal maps, in Cycles under one
raking light, from the same cameras: <out dir>/look-<name>-<nm|flat>-<view>.png.
Run: blender -b --factory-startup -P tools/char_bake_view.py -- <hd.png> <out dir> <name> [<name> ...]
"""
import bpy, os, sys
from mathutils import Vector, Euler
import math

args = sys.argv[sys.argv.index('--') + 1:]
hd, out, names = os.path.abspath(args[0]), os.path.abspath(args[1]), args[2:]
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc.render.engine = 'CYCLES'
sc.cycles.device = 'CPU'
sc.cycles.samples = 24
sc.cycles.use_denoising = True
sc.render.resolution_x = sc.render.resolution_y = 560
sc.view_settings.view_transform = 'Standard'
world = bpy.data.worlds.new('w')
world.use_nodes = True
world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.25, 0.25, 0.27, 1)
world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.35
sc.world = world
sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
sun.data.energy = 4.0
sun.data.angle = math.radians(2)
# low and from the side: a raking light, so relief of a millimetre or two casts light and shade
sun.rotation_euler = Euler((math.radians(72), 0, math.radians(-60)))
sc.collection.objects.link(sun)
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.type = 'ORTHO'
hd_img = bpy.data.images.load(hd)
objs = {}
for n in names:
    with bpy.data.libraries.load(os.path.join(out, 'baked-%s.blend' % n)) as (src, dst):
        dst.objects = [o for o in src.objects]
    o = next(o for o in dst.objects if o and o.type == 'MESH')
    sc.collection.objects.link(o)
    nimg = bpy.data.images.load(os.path.join(out, 'normal-%s.png' % n))
    nimg.colorspace_settings.name = 'Non-Color'
    for mode in ('nm', 'flat'):
        m = bpy.data.materials.new('%s_%s' % (n, mode))
        m.use_nodes = True
        nt = m.node_tree
        bsdf = nt.nodes['Principled BSDF']
        bsdf.inputs['Roughness'].default_value = 0.8
        t = nt.nodes.new('ShaderNodeTexImage')
        nt.nodes.active = t  # (textured views draw the active image node)
        t.image = hd_img
        nt.links.new(t.outputs['Color'], bsdf.inputs['Base Color'])
        if mode == 'nm':
            nn = nt.nodes.new('ShaderNodeTexImage')
            nn.image = nimg
            nm = nt.nodes.new('ShaderNodeNormalMap')
            nt.links.new(nn.outputs['Color'], nm.inputs['Color'])
            nt.links.new(nm.outputs['Normal'], bsdf.inputs['Normal'])
        objs[n, mode] = m
    objs[n] = o
VIEWS = [('chest', (0, -0.1, 1.5), (0.3, -1, 0.3), 0.55), ('backtop', (0, 0.1, 1.6), (0, 1, 0.6), 0.6),
         ('belt', (0, -0.1, 1.17), (-0.3, -1, 0.1), 0.4)]
for n in names:
    for mode in ('nm', 'flat'):
        if mode == 'flat' and n != names[0]:
            continue
        for o in sc.objects:
            if o.type == 'MESH':
                o.hide_render = o is not objs[n]
        objs[n].data.materials.clear()
        objs[n].data.materials.append(objs[n, mode])
        for vn, t, d, s_ in VIEWS:
            dv = Vector(d).normalized()
            cam.data.ortho_scale = s_
            cam.location = Vector(t) + dv * 3
            cam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
            sc.render.filepath = os.path.join(out, 'look-%s-%s-%s.png' % (n, mode, vn))
            bpy.ops.render.render(write_still=True)
print('LOOK done')
