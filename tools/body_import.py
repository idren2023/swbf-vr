"""Blender (blender -b --factory-startup -P tools/body_import.py -- <file.msh> <out-dir> [target-tris]): builds a SWBF
soldier msh as an armature and a skinned mesh (one bone per vertex as in the msh; a part without weights follows its
parent node), then a copy subdivided to about
target-tris (Catmull-Clark, hard edges creased, weights interpolated by Blender), saves <name>.blend and front /
three-quarter / torso preview pictures of both side by side (stock left, subdivided right)."""
import bpy, bmesh, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

args = sys.argv[sys.argv.index('--') + 1:]
MSH, OUT = args[0], args[1]
TARGET = int(args[2]) if len(args) > 2 else 15000
NAME = os.path.splitext(os.path.basename(MSH))[0]


def to_blender(p):
    # msh: y up, +z forward; Blender: z up, the model facing -y (front view)
    return (p[0], -p[2], p[1])


def raw_nodes(path):
    """Per MODL in file order: MNDX, ENVL and per segment the heaviest bone (envelope index) of each vertex."""
    d = open(path, 'rb').read()
    msh2 = dict(mshgeo._chunks(dict(mshgeo._chunks(d))[b'HEDR']))[b'MSH2']
    out = []
    for tag, c in mshgeo._chunks(msh2):
        if tag != b'MODL':
            continue
        f = dict(mshgeo._chunks(c))
        info = {'mndx': struct.unpack_from('<I', f[b'MNDX'])[0] if b'MNDX' in f else None, 'envl': [], 'wght': []}
        for t3, g in mshgeo._chunks(f.get(b'GEOM', b'')):
            if t3 == b'ENVL':
                n = struct.unpack_from('<I', g)[0]
                info['envl'] = list(struct.unpack_from('<%dI' % n, g, 4))
            elif t3 == b'SEGM':
                w = None
                for t4, s in mshgeo._chunks(g):
                    if t4 == b'WGHT':
                        n = struct.unpack_from('<I', s)[0]
                        w = [max((struct.unpack_from('<If', s, 4 + 32 * i + 8 * k) for k in range(4)),
                                 key=lambda e: e[1])[0] for i in range(n)]
                info['wght'].append(w)
        out.append(info)
    return out


model = mshgeo.load(MSH)
raw = raw_nodes(MSH)
nodes = model['nodes']
by_mndx = {r['mndx']: n for n, r in zip(nodes, raw) if r['mndx'] is not None}
by_name = {n['name']: n for n in nodes}

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

# armature: every node an envelope names, plus its ancestors
used = set()
for r in raw:
    for e in r['envl']:
        node = by_mndx[e]
        while node:
            used.add(node['name'])
            node = by_name.get(node.get('parent'))
# rigid parts (first-person arms): a drawn, textured node without weights moves with its parent node
for n, r in zip(nodes, raw):
    if mshgeo.is_drawn(n) and not r['envl'] and any(s.get('uv') and s.get('texture') for s in n['segments']):
        node = by_name.get(n.get('parent'))
        while node:
            used.add(node['name'])
            node = by_name.get(node.get('parent'))
arm_data = bpy.data.armatures.new(NAME + '_skeleton')
arm = bpy.data.objects.new(NAME + '_skeleton', arm_data)
scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='EDIT')
edit = {}
for node in nodes:
    if node['name'] not in used:
        continue
    b = arm_data.edit_bones.new(node['name'])
    head = to_blender(node['world'][1])
    kids = [k for k in nodes if k.get('parent') == node['name'] and k['name'] in used]
    tail = to_blender(kids[0]['world'][1]) if kids else None
    if not tail or math.dist(head, tail) < 0.01:
        tail = (head[0], head[1], head[2] + 0.05)
    b.head, b.tail = head, tail
    edit[node['name']] = b
for node in nodes:
    if node['name'] in edit and node.get('parent') in edit:
        edit[node['name']].parent = edit[node['parent']]
bpy.ops.object.mode_set(mode='OBJECT')

# mesh: every drawn, skinned, textured segment; vertices welded by position and bone, uvs per corner
verts, vbone, faces, face_uv, face_mat, mats = [], [], [], [], [], []
key_of = {}
for node, r in zip(nodes, raw):
    if not mshgeo.is_drawn(node) or node['name'].lower().startswith('sv_'):
        continue
    for seg, w in zip(node['segments'], r['wght']):
        if not seg.get('uv') or not seg.get('texture') or (w is None and r['envl']):
            continue
        if seg['texture'] not in mats:
            mats.append(seg['texture'])
        remap = []
        for i, p in enumerate(seg['wpos']):
            bone = by_mndx[r['envl'][w[i]]]['name'] if w else node['parent']
            k = (round(p[0], 4), round(p[1], 4), round(p[2], 4), bone)
            if k not in key_of:
                key_of[k] = len(verts)
                verts.append(to_blender(p))
                vbone.append(bone)
            remap.append(key_of[k])
        for a, b, c in seg['tris']:
            tri = (remap[a], remap[b], remap[c])
            if len(set(tri)) < 3:
                continue
            faces.append(tri)
            face_uv.append([seg['uv'][a], seg['uv'][b], seg['uv'][c]])
            face_mat.append(mats.index(seg['texture']))

me = bpy.data.meshes.new(NAME)
me.from_pydata(verts, [], faces)
uv = me.uv_layers.new(name='UV')
for f, poly in enumerate(me.polygons):
    for j, li in enumerate(poly.loop_indices):
        uv.data[li].uv = face_uv[f][j]
    poly.material_index = face_mat[f]
    poly.use_smooth = True
tex_dir = os.path.dirname(MSH)
for t in mats:
    m = bpy.data.materials.new(t)
    m.use_nodes = True
    bsdf = m.node_tree.nodes['Principled BSDF']
    img = m.node_tree.nodes.new('ShaderNodeTexImage')
    m.node_tree.nodes.active = img  # (textured views draw the active image node)
    p = os.path.join(tex_dir, t)
    if os.path.exists(p):
        img.image = bpy.data.images.load(p)
    m.node_tree.links.new(img.outputs['Color'], bsdf.inputs['Base Color'])
    me.materials.append(m)
body = bpy.data.objects.new(NAME, me)
scene.collection.objects.link(body)
for i, bone in enumerate(vbone):
    g = body.vertex_groups.get(bone) or body.vertex_groups.new(name=bone)
    g.add([i], 1.0, 'REPLACE')
src_tris = len(me.polygons)

# hard edges (dihedral over 50 deg) creased so the subdivision keeps them
bm = bmesh.new()
bm.from_mesh(me)
crease = bm.edges.layers.float.new('crease_edge')
hard = 0
for e in bm.edges:
    if len(e.link_faces) == 2 and e.calc_face_angle(0) > math.radians(50):
        e[crease] = 1.0
        hard += 1
bm.to_mesh(me)
bm.free()

# subdivided copy: one or two levels, decimated down to the target
hi = body.copy()
hi.data = me.copy()
hi.name = NAME + '_hi'
scene.collection.objects.link(hi)
level = 1 if src_tris * 6 >= TARGET * 0.8 else 2
mod = hi.modifiers.new('subd', 'SUBSURF')
mod.levels = mod.render_levels = level
mod.uv_smooth = 'PRESERVE_CORNERS'
bpy.context.view_layer.objects.active = hi
bpy.ops.object.modifier_apply(modifier='subd')
tris_now = sum(len(p.vertices) - 2 for p in hi.data.polygons)
if tris_now > TARGET * 1.05:
    dec = hi.modifiers.new('dec', 'DECIMATE')
    dec.ratio = TARGET / tris_now
    bpy.ops.object.modifier_apply(modifier='dec')
bm = bmesh.new()
bm.from_mesh(hi.data)
bmesh.ops.triangulate(bm, faces=bm.faces[:])
bm.to_mesh(hi.data)
bm.free()
hi_tris = len(hi.data.polygons)
hi_maxw = max(sum(1 for g in v.groups if g.weight > 0.01) for v in hi.data.vertices)
for o in (body, hi):
    o.parent = arm
    a = o.modifiers.new('skeleton', 'ARMATURE')
    a.object = arm
hi.location.x = 1.3
print('RESULT', NAME, 'bones', len(arm_data.bones), 'source tris', src_tris, 'verts', len(verts), 'hard edges', hard,
      '| hi: level', level, 'tris', hi_tris, 'verts', len(hi.data.vertices), 'max bones per vertex', hi_maxw)

os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(os.path.join(OUT, NAME + '.blend')))

# previews: workbench, textures, orthographic
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
scene.display.shading.show_cavity = True
scene.render.resolution_x, scene.render.resolution_y = 1600, 1000
cam_data = bpy.data.cameras.new('cam')
cam_data.type = 'ORTHO'
cam = bpy.data.objects.new('cam', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
for label, yaw, z, scale in (('front', 0, 1.0, 3.8), ('threequarter', 35, 1.0, 3.8), ('torso', 20, 1.5, 2.3)):
    cam_data.ortho_scale = scale
    r = math.radians(yaw)
    cam.location = (0.65 + 6 * math.sin(r), -6 * math.cos(r), z)
    cam.rotation_euler = (math.radians(90), 0, r)
    scene.render.filepath = os.path.abspath(os.path.join(OUT, NAME + '-' + label + '.png'))
    bpy.ops.render.render(write_still=True)
