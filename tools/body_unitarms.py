"""Blender (blender -b --factory-startup <our arm .blend> -P tools/body_unitarms.py -- <out-dir> [unit ..]): every unit's
stock first-person right arm and hand (the msh's right forearm / hand nodes and the parts beside them) next to ours
(r_hand_retopo, r_forearm_retopo, r_forearm_stud of the .blend, in its rest pose), each from two sides, textured and
as edges (2026-10-31, PLAN 60: compare the units' arms with ours). Writes <out-dir>/ua-<name>-<view>.png per object and
view (view: a / b = the two sides, t = textured, e = edges), orthographic, framed on the object alone."""
import bpy, glob, math, os, sys
from mathutils import Matrix, Vector
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
ASSETS = 'C:/LucasArts/BFBuilder/Assets/Sides/'
UNITS = args[1:] or sorted({os.path.basename(p)[:-4] for p in glob.glob(ASSETS + '*/MSH/*_1st_*.msh')
                            if not any(k in p for k in ('weap', 'cockpit', 'droideka'))})
os.makedirs(OUT, exist_ok=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.render.resolution_x, scene.render.resolution_y = 800, 320
scene.render.film_transparent = False
scene.world = scene.world or bpy.data.worlds.new('w')
scene.display.shading.background_type = 'VIEWPORT'
scene.display.shading.background_color = (0.06, 0.06, 0.07)
cam_data = bpy.data.cameras.new('cam')
cam_data.type = 'ORTHO'
cam = bpy.data.objects.new('cam', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
surface = bpy.data.materials.new('surface')
surface.diffuse_color = (0.78, 0.78, 0.76, 1)
wire = bpy.data.materials.new('wire')
wire.diffuse_color = (0.05, 0.05, 0.08, 1)


def to_blender(p):
    return Vector((p[0], -p[2], p[1]))  # as tools/body_import.py


def is_right(name):
    n = name.lower()
    return ('_r_' in n or n.startswith('r_') or 'r_hand' in n or 'r_arm' in n or 'r_forearm' in n) and '_l_' not in n


def import_unit(unit):
    path = next(iter(glob.glob(ASSETS + '*/MSH/%s.msh' % unit)), None)
    model = mshgeo.load(path)
    folder = os.path.dirname(path)
    nodes = [n for n in model['nodes'] if mshgeo.is_drawn(n) and any(s['wpos'] for s in n['segments'])]
    right = [n for n in nodes if is_right(n['name'])]
    if not right:
        return None
    centre = sum((Vector(p) for n in right for s in n['segments'] for p in s['wpos']), Vector()) / \
        sum(len(s['wpos']) for n in right for s in n['segments'])
    # parts without a side in their name (spheres, cylinders: the battle droids' joints) belong to the right arm when
    # they lie on its side (x has the right hand's sign) and near it
    for n in nodes:
        if n in right or '_l_' in n['name'].lower() or is_right(n['name']):
            continue
        pts = [Vector(p) for s in n['segments'] for p in s['wpos']]
        c = sum(pts, Vector()) / len(pts)
        if c.x * centre.x > 0 and (c - centre).length < 0.35:
            right.append(n)
    verts, faces, uvs, mats = [], [], [], []
    for n in right:
        for s in n['segments']:
            if not s['wpos']:
                continue
            tex = s.get('texture') or ''
            if tex not in mats:
                mats.append(tex)
            base = len(verts)
            verts += [to_blender(p) for p in s['wpos']]
            uv = s.get('uv') or [(0, 0)] * len(s['wpos'])
            for a, b, c in s['tris']:
                faces.append((base + a, base + b, base + c))
                uvs.append(([uv[a], uv[b], uv[c]], mats.index(tex)))
    me = bpy.data.meshes.new(unit)
    me.from_pydata(verts, [], faces)
    layer = me.uv_layers.new(name='UV')
    for poly, (tuv, mi) in zip(me.polygons, uvs):
        for j, li in enumerate(poly.loop_indices):
            layer.data[li].uv = (tuv[j][0], 1 - tuv[j][1]) if False else tuv[j]
        poly.material_index = mi
        poly.use_smooth = True
    for t in mats:
        m = bpy.data.materials.new(unit + ':' + t)
        m.use_nodes = True
        img = m.node_tree.nodes.new('ShaderNodeTexImage')
        m.node_tree.nodes.active = img  # (textured views draw the active image node)
        p = os.path.join(folder, t) if t else ''
        if p and not os.path.exists(p) and os.path.exists(p + '.tga'):
            p += '.tga'
        if p and os.path.exists(p):
            img.image = bpy.data.images.load(p)
        m.node_tree.links.new(img.outputs['Color'], m.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
        me.materials.append(m)
    ob = bpy.data.objects.new('stock_' + unit, me)
    scene.collection.objects.link(ob)
    return ob


def ours():
    parts = [bpy.data.objects[n] for n in ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud') if n in bpy.data.objects]
    rig = parts[0].parent
    for pb in rig.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
    bpy.context.view_layer.update()
    # one evaluated mesh (rest pose, the modifiers applied), the parts' materials kept
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new('ours')
    import bmesh
    bm = bmesh.new()
    for o in parts:
        e = o.evaluated_get(dg)
        m = e.to_mesh()
        m.transform(o.matrix_world)
        offset = len(me.materials)
        for mat in o.data.materials:
            me.materials.append(mat)
        for poly in m.polygons:
            poly.material_index += offset
        bm.from_mesh(m)
        e.to_mesh_clear()
    bm.to_mesh(me)
    ob = bpy.data.objects.new('ours_rebel_soldier', me)
    scene.collection.objects.link(ob)
    return ob


def long_axis(pts):
    c = sum(pts, Vector()) / len(pts)
    # power iteration on the covariance
    cov = [[sum((p[i] - c[i]) * (p[j] - c[j]) for p in pts) for j in range(3)] for i in range(3)]
    v = Vector((1, 0.3, 0.2))
    for _ in range(40):
        v = Vector([sum(cov[i][j] * v[j] for j in range(3)) for i in range(3)]).normalized()
    return c, v


def render(ob, name):
    for o in scene.objects:
        if o.type == 'MESH':
            o.hide_render = o is not ob
    pts = [ob.matrix_world @ v.co for v in ob.data.vertices]
    c, axis = long_axis(pts)
    up = Vector((0, 0, 1))
    side = axis.cross(up)
    if side.length < 0.1:
        side = axis.cross(Vector((0, 1, 0)))
    side.normalize()
    up2 = side.cross(axis).normalized()
    for view, direction in (('a', side), ('b', -side)):
        # the camera looks along -direction, its x along the arm, its y up2
        z = direction
        x = axis if view == 'a' else -axis
        y = z.cross(x).normalized()
        R = Matrix((x, y, z)).transposed()
        cam.matrix_world = Matrix.Translation(c + z * 2.0) @ R.to_4x4()
        xs = [(p - c).dot(x) for p in pts]
        ys = [(p - c).dot(y) for p in pts]
        cam_data.ortho_scale = max(max(xs) - min(xs), (max(ys) - min(ys)) * 800 / 320) * 1.08
        cam_data.shift_x = (max(xs) + min(xs)) / 2 / cam_data.ortho_scale
        cam_data.shift_y = (max(ys) + min(ys)) / 2 / cam_data.ortho_scale
        # textured
        scene.display.shading.color_type = 'TEXTURE'
        scene.render.filepath = os.path.join(OUT, 'ua-%s-%st.png' % (name, view))
        bpy.ops.render.render(write_still=True)
        # edges: a light grey surface and its edges as thin dark tubes (a wireframe copy)
        saved = list(ob.data.materials)
        ob.data.materials.clear()
        ob.data.materials.append(surface)
        dup = ob.copy()
        dup.data = ob.data.copy()
        dup.data.materials.clear()
        dup.data.materials.append(wire)
        scene.collection.objects.link(dup)
        mod = dup.modifiers.new('wf', 'WIREFRAME')
        size = max(max(xs) - min(xs), max(ys) - min(ys))
        mod.thickness = size * 0.0022
        mod.use_replace = True
        dup.hide_render = False
        scene.display.shading.color_type = 'MATERIAL'
        scene.render.filepath = os.path.join(OUT, 'ua-%s-%se.png' % (name, view))
        bpy.ops.render.render(write_still=True)
        bpy.data.objects.remove(dup, do_unlink=True)
        ob.data.materials.clear()
        for m in saved:
            ob.data.materials.append(m)


render(ours(), 'ours')
for unit in UNITS:
    ob = import_unit(unit)
    if ob is None:
        print('UNIT %s: no right arm nodes' % unit)
        continue
    print('UNIT %s: %d tris' % (unit, len(ob.data.polygons)))
    render(ob, unit)
