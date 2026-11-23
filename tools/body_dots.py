"""Blender (blender -b --factory-startup <file.blend> -P tools/body_dots.py [-- point]): the rest pose of r_hand_stock, textured,
every vertex a red dot, the ones facing the camera labelled with their number, from seven sides -> re/body/dots-*.png."""
import bpy, bmesh, os
from mathutils import Vector, Matrix
from bpy_extras.object_utils import world_to_camera_view
scene = bpy.context.scene
import math, sys
o = bpy.data.objects['r_hand_stock']
# optional pose (-- point): the pose test's pointing pose (tools/body_posetest.py)
POSE_NAME = (sys.argv[sys.argv.index('--') + 1:] or [''])[0] if '--' in sys.argv else ''
if POSE_NAME == 'point':
    pose = {}
    for f in ('middle', 'ring', 'pinky'):
        pose.update({f + '_1': (-80, 0, 0), f + '_2': (-95, 0, 0), f + '_3': (-60, 0, 0)})
    pose.update({'thumb_1': (-20, 0, -30), 'thumb_2': (-25, 0, 0), 'thumb_3': (-35, 0, 0),
                 'index_1': (-14, 0, -10), 'index_2': (11, 0, 0), 'index_3': (6, 0, 0)})
    for n, d in pose.items():
        pb = o.parent.pose.bones[n]
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = [math.radians(x) for x in d]
    bpy.context.view_layer.update()
for ob in scene.objects: ob.hide_render = ob is not o
me = o.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
pts = [o.matrix_world @ v.co for v in me.vertices]
c = sum(pts, Vector()) / len(pts)
red = bpy.data.materials.new('dot'); red.diffuse_color = (1, 0, 0, 1)
white = bpy.data.materials.new('lbl'); white.diffuse_color = (1, 1, 0.3, 1)
# small red spheres on every vertex; drawn through the glove only where the vertex faces the camera
bm = bmesh.new()
for p in pts:
    bmesh.ops.create_icosphere(bm, subdivisions=1, radius=0.0011, matrix=Matrix.Translation(p))
dm = bpy.data.meshes.new('dots'); bm.to_mesh(dm)
dots = bpy.data.objects.new('dots', dm); dm.materials.append(red); scene.collection.objects.link(dots)
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.show_object_outline = True
scene.display.shading.light = 'STUDIO'
scene.render.resolution_x, scene.render.resolution_y = 1400, 1400
cd = bpy.data.cameras.new('c'); cd.type = 'ORTHO'
cam = bpy.data.objects.new('c', cd); scene.collection.objects.link(cam); scene.camera = cam
cd.ortho_scale = max((p - c).length for p in pts) * 2.2
dots.data.materials[0].use_backface_culling = False
views = {'back': ((0,0,1),(0,1,0)), 'palm': ((0,0,-1),(0,1,0)), 'thumbside': ((-1,0,0),(0,0,1)),
         'pinkyside': ((1,0,0),(0,0,1)), 'front': ((0,1,0.2),(0,0,1)), 'back34': ((-0.6,-0.3,0.75),(0,1,0)),
         'palm34': ((-0.6,0.2,-0.75),(0,1,0))}
for name, (d, up) in views.items():
    d = Vector(d).normalized(); up = Vector(up); up = (up - d * up.dot(d)).normalized()
    cam.matrix_world = Matrix.Translation(c + d * 2) @ Matrix((d.cross(up) * -1, up, d)).transposed().to_4x4()
    for t in [ob for ob in scene.objects if ob.name.startswith('lbl')]:
        bpy.data.objects.remove(t)
    rot = cam.matrix_world.to_euler()
    for v in me.vertices:
        n = (o.matrix_world.to_3x3() @ v.normal).normalized()  # (me is the posed mesh)
        if n.dot(d) < 0.15:
            continue  # faces away: no label
        p = o.matrix_world @ v.co
        cu = bpy.data.curves.new('lbl%d' % v.index, 'FONT'); cu.body = str(v.index); cu.size = cd.ortho_scale * 0.018
        t = bpy.data.objects.new('lbl%d' % v.index, cu); t.location = p + d * 0.02 + up * 0.0012
        t.rotation_euler = rot; cu.materials.append(white); scene.collection.objects.link(t)
    scene.render.filepath = os.path.abspath('re/body/dots-%s%s.png' % (POSE_NAME + '-' if POSE_NAME else '', name))
    bpy.ops.render.render(write_still=True)
