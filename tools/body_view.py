"""Blender (blender -b <file.blend> -P tools/body_view.py -- <object> <out-prefix> <bone,bone,..|all> [scale] [subdiv]
[curl]):
close-up pictures of the part of a skinned body touching the given bones (orthographic, six sides, textured, everything
else hidden; 'all' = the whole object; subdiv = Catmull-Clark levels, to see the surface a cage makes), and the texture
region those faces use with their uv wireframe drawn in (<out-prefix>-uv.png, 4x)."""
import bpy, bmesh, math, os, sys
from mathutils import Vector

args = sys.argv[sys.argv.index('--') + 1:]
OBJ, OUT, BONES = args[0], args[1], args[2].split(',')
SCALE = float(args[3]) if len(args) > 3 else 0.0
SUBDIV = int(args[4]) if len(args) > 4 else 0
CURL = float(args[5]) if len(args) > 5 else 0.0  # fingers bent by this share of 70 / 80 / 50 deg (a hand rig)
scene = bpy.context.scene
src = bpy.data.objects[OBJ]
for o in scene.objects:
    o.hide_render = True

# copy of the part: faces with a vertex on the bones
groups = {src.vertex_groups[b].index for b in BONES if b in src.vertex_groups}
bm = bmesh.new()
if CURL and src.parent and src.parent.type == 'ARMATURE':
    for pb in src.parent.pose.bones:
        pb.rotation_mode = 'XYZ'
        for suffix, deg in (('_1', 70), ('_2', 80), ('_3', 50)):
            if pb.name.endswith(suffix) and not pb.name.startswith('thumb'):
                pb.rotation_euler.x = -math.radians(deg * CURL)
    bpy.context.view_layer.update()
    bm.from_mesh(src.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh())
else:
    bm.from_mesh(src.data)
deform = bm.verts.layers.deform.active
keep = {v.index for v in bm.verts if BONES == ['all'] or any(g in groups and w > 0.5 for g, w in v[deform].items())}
bmesh.ops.delete(bm, geom=[f for f in bm.faces if not any(v.index in keep for v in f.verts)], context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
part_me = bpy.data.meshes.new('part')
bm.to_mesh(part_me)
bm.free()
part = bpy.data.objects.new('part', part_me)
for m in src.data.materials:
    part_me.materials.append(m)
scene.collection.objects.link(part)
part.matrix_world = src.matrix_world.copy()
if SUBDIV:
    part.modifiers.new('subd', 'SUBSURF').levels = SUBDIV
pts = [part.matrix_world @ v.co for v in part_me.vertices]
lo = Vector([min(p[i] for p in pts) for i in range(3)])
hi = Vector([max(p[i] for p in pts) for i in range(3)])
mid = (lo + hi) / 2
size = max(hi - lo)
print('PART', OBJ, BONES, 'tris', sum(len(p.vertices) - 2 for p in part_me.polygons), 'verts', len(part_me.vertices),
      'bounds', tuple(round(x, 3) for x in lo), tuple(round(x, 3) for x in hi))

scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
scene.display.shading.show_cavity = True
scene.display.shading.show_object_outline = False
scene.render.resolution_x, scene.render.resolution_y = 900, 600
cam_data = bpy.data.cameras.new('closeup')
cam_data.type = 'ORTHO'
cam_data.ortho_scale = SCALE or size * 1.25
cam = bpy.data.objects.new('closeup', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
views = {'top': (0, 0, 1), 'bottom': (0, 0, -1), 'front': (0, -1, 0), 'back': (0, 1, 0), 'out': (-1, 0, 0) if mid.x < 0 else (1, 0, 0),
         'wire': (0.5, -0.6, 0.6)}
for label, d in views.items():
    d = Vector(d).normalized()
    cam.location = mid + d * 3
    cam.rotation_euler = (-d).to_track_quat('-Z', 'Y' if abs(d.z) > 0.9 else 'Z').to_euler()
    scene.display.shading.show_xray = False
    if label == 'wire':
        part.show_wire = True
        scene.display.shading.color_type = 'SINGLE'
    scene.render.filepath = os.path.abspath(OUT + '-' + label + '.png')
    bpy.ops.render.render(write_still=True)
scene.display.shading.color_type = 'TEXTURE'

# texture region with the part's uv wireframe
uv = part_me.uv_layers.active.data
for mi, mat in enumerate(part_me.materials):
    polys = [p for p in part_me.polygons if p.material_index == mi]
    img = next((n.image for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image), None)
    if not polys or not img:
        continue
    w, h = img.size
    us = [uv[li].uv for p in polys for li in p.loop_indices]
    x0, x1 = max(0, int(min(u.x for u in us) * w) - 4), min(w, int(math.ceil(max(u.x for u in us) * w)) + 4)
    y0, y1 = max(0, int(min(u.y for u in us) * h) - 4), min(h, int(math.ceil(max(u.y for u in us) * h)) + 4)
    px = list(img.pixels)
    k = 4
    cw, ch = (x1 - x0) * k, (y1 - y0) * k
    out = [0.0] * (cw * ch * 4)
    for y in range(ch):
        for x in range(cw):
            s = ((y0 + y // k) * w + x0 + x // k) * 4
            o = (y * cw + x) * 4
            out[o:o + 4] = px[s:s + 4]
    for p in polys:
        loop = [uv[li].uv for li in p.loop_indices]
        for a, b in zip(loop, loop[1:] + loop[:1]):
            for t in range(64):
                u = a.lerp(b, t / 63)
                x, y = int((u.x * w - x0) * k), int((u.y * h - y0) * k)
                if 0 <= x < cw and 0 <= y < ch:
                    out[(y * cw + x) * 4:(y * cw + x) * 4 + 4] = [1.0, 0.1, 0.9, 1.0]
    im = bpy.data.images.new('uv%d' % mi, cw, ch, alpha=True)
    im.pixels = out
    im.filepath_raw = os.path.abspath('%s-uv%d.png' % (OUT, mi))
    im.file_format = 'PNG'
    im.save()
    print('UV', mi, img.name, 'texels', (x0, y0, x1, y1), 'of', (w, h))
