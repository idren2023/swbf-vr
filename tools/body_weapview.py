"""Blender (blender -b --factory-startup <hand+weapons.blend> -P tools/body_weapview.py -- <out-prefix> <weapon-object>
[axis=x|y|z] [step=mm]): a weapon alone, orthographic, along one axis of the hand frame (default x: side view), with
a grid every `step` mm (default 20) and its coordinates written on the grid lines, for reading off points on the model
(the grip's axis, the trigger) by eye."""
import bpy, math, os, sys
from mathutils import Vector

args = sys.argv[sys.argv.index('--') + 1:]
OUT, WEAP = args[0], args[1]
AX = next((a[5:] for a in args[2:] if a.startswith('axis=')), 'x')
STEP = next((float(a[5:]) for a in args[2:] if a.startswith('step=')), 20.0) / 1000
scene = bpy.context.scene
w = bpy.data.objects[WEAP]
for o in scene.objects:
    o.hide_render = o is not w
w.hide_render = False
pts = [w.matrix_world @ v.co for v in w.data.vertices]
i = 'xyz'.index(AX)
u, v_ = [k for k in range(3) if k != i]          # the image's horizontal, vertical axes
lo = [min(p[k] for p in pts) for k in range(3)]
hi = [max(p[k] for p in pts) for k in range(3)]
c = Vector([(lo[k] + hi[k]) / 2 for k in range(3)])
size = max(hi[u] - lo[u], hi[v_] - lo[v_]) * 1.15
cam_d = Vector((0, 0, 0))
cam_d[i] = 1.0
cd = bpy.data.cameras.new('cam')
cd.type = 'ORTHO'
cd.ortho_scale = size
cd.clip_start, cd.clip_end = 0.001, 10
cam = bpy.data.objects.new('cam', cd)
scene.collection.objects.link(cam)
cam.location = c + cam_d * 2
up = Vector((0, 0, 0))
up[v_] = 1.0
cam.rotation_euler = (-cam_d).to_track_quat('-Z', 'Y' if v_ == 1 else 'Z').to_euler()
scene.camera = cam
mat = bpy.data.materials.new('grid')
mat.diffuse_color = (1, 0.2, 0.2, 1)
tmat = bpy.data.materials.new('label')
tmat.diffuse_color = (1, 1, 0.3, 1)
front = c + cam_d * 0.5


def line(a, b):
    me = bpy.data.meshes.new('l')
    d = (b - a)
    n = d.cross(cam_d).normalized() * size * 0.0012
    me.from_pydata([a - n, a + n, b + n, b - n], [], [(0, 1, 2, 3)])
    me.materials.append(mat)
    o = bpy.data.objects.new('l', me)
    scene.collection.objects.link(o)


def label(text, p):
    cu = bpy.data.curves.new('t', 'FONT')
    cu.body = text
    cu.size = size * 0.022
    o = bpy.data.objects.new('t', cu)
    o.data.materials.append(tmat)
    scene.collection.objects.link(o)
    o.location = p
    o.rotation_euler = cam.rotation_euler


g0 = [math.floor(lo[k] / STEP) * STEP for k in range(3)]
for axis_k, other in ((u, v_), (v_, u)):
    x = g0[axis_k]
    while x <= hi[axis_k] + STEP:
        a = c.copy()
        a[i] = front[i]
        b = a.copy()
        a[axis_k] = b[axis_k] = x
        a[other] = lo[other] - STEP
        b[other] = hi[other] + STEP
        line(a, b)
        label('%s%.0f' % ('xyz'[axis_k], x * 1000), a)
        x += STEP
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'FLAT'
scene.display.shading.color_type = 'MATERIAL'
scene.render.resolution_x = scene.render.resolution_y = 1400
scene.render.filepath = os.path.abspath('%s-%s-%s.png' % (OUT, WEAP, AX))
bpy.ops.render.render(write_still=True)
print('WEAPVIEW', scene.render.filepath)
