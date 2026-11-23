"""Blender (blender -b --factory-startup re/body/r_arm_sleeve.blend -P tools/body_armour.py -- <out.blend> <unit> ..
[levels=2] [crease=35]): the armoured units' forearms on our hand (2026-10-31, PLAN 60 step 2: stormtrooper, dark
trooper, clones). For each unit (its first-person msh, e.g. imp_1st_stormtrooper):

- its stock right forearm (armour plates over the black undersuit, on bone_r_forearm) and hand pad (the plate on the
  back of the hand, on bone_r_wrist), carried from that unit's wrist into the Rebel trooper's (the units' arms models
  stand in different places: bone_r_wrist to bone_r_wrist) and from there into our hand frame as
  re/body/hand-frame-all_1st_trooper.json does for every stock part (scale 0.712);
- vertices welded where the msh split them, edges sharper than `crease` degrees creased, `levels` Catmull-Clark levels
  applied (the uvs carried): the stock's shapes, smooth, plates' edges crisp (as tools/body_forearm.py does the sleeve);
- rigged to our arm: the hand pad wholly on the hand bone; the forearm on forearm_twist (the plates rigid, half the
  hand's roll, as the sleeve's middle), the elbow end blending into forearm, the wrist end into wrist_twist.

Saved as objects forearm_<unit> with a custom property `texture` (the unit's arms texture), which tools/body_export.py
exports as parts of their own; BodyArm draws one in place of our sleeve for the units that name it (UNIT_SKINS)."""
import bpy, bmesh, json, math, os, sys
from mathutils import Matrix, Vector
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
opt = dict(a.split('=', 1) for a in args[1:] if '=' in a)
UNITS = [a for a in args[1:] if '=' not in a]
LEVELS = int(opt.get('levels', 2))
CREASE = math.radians(float(opt.get('crease', 35)))
A = 'C:/LucasArts/BFBuilder/Assets/Sides/'
F = json.load(open('re/body/hand-frame-all_1st_trooper.json'))
R, W, S = Matrix(F['to_hand']), Vector(F['wrist']), F['scale']
rig = bpy.data.objects['r_hand_retopo'].parent


def msh_path(unit):
    return next(p for p in (A + d + '/MSH/' + unit + '.msh' for d in ('ALL', 'IMP', 'REP', 'CIS')) if os.path.exists(p))


def frame(model, name):
    node = next(n for n in model['nodes'] if n['name'].lower() == name)
    rows, t = node['world']
    return Matrix([list(rows[0]) + [0], list(rows[1]) + [0], list(rows[2]) + [0], list(t) + [1]])  # row vectors


def to_blender(p):
    return Vector((p[0], -p[2], p[1]))  # as tools/body_import.py


def bone_of(model, node):
    """The bone a node hangs from (the Wookiee's hair skirts hang from each other: \"l_hair\" in both arms' names)."""
    nodes = {n['name']: n for n in model['nodes']}
    while node and not node['name'].lower().startswith('bone_'):
        node = nodes.get(node.get('parent'))
    return node['name'].lower() if node else ''


def is_kind(node, kind):
    """forearm: the right forearm (armour plates, the Wookiee's fur arm) and hand pad; hair: the skirts of hair strands
    round the right forearm (the Wookiee: alpha-tested, BodyArm draws hair_ parts so)."""
    name = node['name'].lower()
    if kind == 'hair':
        return 'hair' in name and bone_of(model, node) == 'bone_r_forearm'
    return '_l_' not in name and ('r_forearm' in name or 'r_handpad' in name or name.endswith('_r_arm'))


rebel = mshgeo.load(msh_path('all_1st_trooper'))
rebel_wrist = frame(rebel, 'bone_r_wrist')
for unit in UNITS:
    model = mshgeo.load(msh_path(unit))
    unit_wrist = frame(model, 'bone_r_wrist')
    carry = unit_wrist.inverted() @ rebel_wrist  # row vectors: p' = p x inv(unit wrist) x rebel wrist

    def H(p):
        q = Vector((p[0], p[1], p[2], 1.0)) @ carry
        return (R @ (to_blender(q) - W)) * S

    for kind in ('forearm', 'hair'):
        verts, faces, uvs, pads, textures = [], [], [], [], []
        for node in model['nodes']:
            name = node['name'].lower()
            if not mshgeo.is_drawn(node) or not is_kind(node, kind):
                continue
            for seg in node['segments']:
                if not seg['wpos']:
                    continue
                textures.append(seg.get('texture') or '')
                base = len(verts)
                verts += [H(p) for p in seg['wpos']]
                pads += ['handpad' in name] * len(seg['wpos'])
                uv = seg.get('uv') or [(0, 0)] * len(seg['wpos'])
                for a, b, c in seg['tris']:
                    faces.append((base + a, base + b, base + c))
                    uvs.append((uv[a], uv[b], uv[c]))
        if not verts:
            continue
        me = bpy.data.meshes.new(kind + '_' + unit)
        me.from_pydata(verts, [], faces)
        layer = me.uv_layers.new(name='UV')
        for poly, tuv in zip(me.polygons, uvs):
            for j, li in enumerate(poly.loop_indices):
                layer.data[li].uv = tuv[j]  # msh v runs up the image, as Blender's (2026-11-02: was 1 - v, upside down)
        # the hand pad's vertices remembered through the weld (a layer of their own)
        bm = bmesh.new()
        bm.from_mesh(me)
        pad = bm.verts.layers.int.new('pad')
        for v in bm.verts:
            v[pad] = 1 if pads[v.index] else 0
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0002)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(me)
        pad_of = [v[pad] for v in bm.verts]
        bm.free()
        ob = bpy.data.objects.new(kind + '_' + unit, me)
        bpy.context.scene.collection.objects.link(ob)
        ob['texture'] = os.path.splitext(max(set(textures), key=textures.count))[0]
        if kind == 'hair':  # our copy with its alpha (tools/body_export.py OVERRIDES): the game's has none usable (run539)
            ob['texture'] += '_hair'
        # creases on the hard edges, then the subdivision applied
        me.edges.foreach_set('use_edge_sharp', [False] * len(me.edges))
        crease = me.attributes.get('crease_edge') or me.attributes.new('crease_edge', 'FLOAT', 'EDGE')
        bm = bmesh.new()
        bm.from_mesh(me)
        sharp = []
        for e in bm.edges:
            if kind == 'forearm' and (len(e.link_faces) != 2 or e.calc_face_angle(0) > CREASE):
                sharp.append(e.index)
        bm.free()
        for i in sharp:
            crease.data[i].value = 1.0
        bpy.context.view_layer.objects.active = ob
        if LEVELS and kind == 'forearm':
            mod = ob.modifiers.new('subd', 'SUBSURF')
            mod.levels = mod.render_levels = LEVELS
            mod.uv_smooth = 'PRESERVE_CORNERS'
            # the pad flag as a vertex group, so it survives the subdivision
            g = ob.vertex_groups.new(name='pad_flag')
            for i, f_ in enumerate(pad_of):
                if f_:
                    g.add([i], 1.0, 'REPLACE')
            bpy.ops.object.modifier_apply(modifier='subd')
        for p in me.polygons:
            p.use_smooth = True
        # rig: weights by place (the pad on the hand; the forearm on forearm_twist, its ends blending)
        groups = {n: ob.vertex_groups.new(name=n) for n in ('forearm', 'forearm_twist', 'wrist_twist', 'hand')}
        flag = ob.vertex_groups.get('pad_flag')
        ys = [v.co.y for v in me.vertices]
        y0, y1 = min(ys), max(ys)
        for v in me.vertices:
            is_pad = flag and any(g.group == flag.index and g.weight > 0.5 for g in v.groups)
            if is_pad:
                groups['hand'].add([v.index], 1.0, 'REPLACE')
                continue
            t = (v.co.y - y0) / max(y1 - y0, 1e-6)  # 0 at the elbow end, 1 at the wrist end
            elbow = max(0.0, min(1.0, (0.20 - t) / 0.20))
            wrist = max(0.0, min(1.0, (t - 0.85) / 0.15))
            twist = 1.0 - elbow - wrist
            for n, w in (('forearm', elbow), ('forearm_twist', twist), ('wrist_twist', wrist)):
                if w > 0:
                    groups[n].add([v.index], w, 'REPLACE')
        if flag:
            ob.vertex_groups.remove(flag)
        ob.parent = rig
        ob.matrix_world = Matrix.Identity(4)
        arm = ob.modifiers.new('rig', 'ARMATURE')
        arm.object = rig
        # a material with the unit's arms texture, for the render sheets
        m = bpy.data.materials.new(ob['texture'])
        m.use_nodes = True
        img = m.node_tree.nodes.new('ShaderNodeTexImage')
        m.node_tree.nodes.active = img  # (textured views draw the active image node)
        folder = os.path.dirname(msh_path(unit))
        path = os.path.join(folder, ob['texture'] + '.tga')
        if os.path.exists(path):
            img.image = bpy.data.images.load(path)
        m.node_tree.links.new(img.outputs['Color'], m.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
        me.materials.append(m)
        print('ARMOUR %s %s: %d stock tris -> %d faces, %d vertices, y %.0f..%.0f mm, texture %s, %d creased edges' % (
            kind, unit, len(faces), len(me.polygons), len(me.vertices), y0 * 1000, y1 * 1000, ob['texture'], len(sharp)))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
