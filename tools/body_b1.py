"""Blender (blender -b --factory-startup re/body/r_arm_grips.blend -P tools/body_b1.py -- <out.blend> [levels=0] [round=1] [segments=1]
[bevel=0.8] [angle=50] [stock=1] [render=<png prefix>] [pose=<bone>:x,y,z;..]): the B1 battle droid's first-person right arm on a skeleton
of its own (PLAN 60 step 4b-c, idren2023 2026-11-02: two segments a digit as the stock, the stock's cross piece between
the fingers' middle joints dropped, the stock widths kept).

- skeleton b1 (armature b1_skeleton, custom property `skeleton` = "b1": tools/body_export.py writes it after the human
  one): forearm (the rod, elbow to wrist), hand (the human hand bone's rest, so arm space and the grips' frame are the
  same), trigger_1-2, grip_1-2, thumb_1-2, each from a stock joint's centre to the next (the stock hand is posed
  closed round a grip: that is the rest); every bone's x axis along the arm's x, so x < 0 curls toward the palm as on
  the human hand.
- parts: b1_hand (the stock hand's rigid pieces, each machined (idren2023 2026-11-02): welded, its flat triangles merged
  into faces, every edge sharper than `angle` degrees bevelled `bevel` mm (two segments), Catmull-Clark `levels`
  applied, its uvs kept, wholly on one bone; the joint cylinders' open inner ends capped; the wrist ball made whole (the stock's is an open band); normals turned outward (the bars are open boxes); the texture's round plate
  (a dark groove with a lit lip, painted across the back and the palm of the hand) cut as a groove `groove` mm deep; the wrist ball on the hand) and b1_arm (the rod, on forearm),
  texture cis_1st_bdroid_hands. The stock arms model (cis_1st_bdroid) has the Rebel trooper's wrist and hp_weapons, so
  its pieces come into our hand frame as re/body/hand-frame-all_1st_trooper.json carries every stock part.
- weapons: the CIS first-person weapons, objects weap_cis_<name> on the b1 hand bone, where their grip json
  (re/body/grip-weap_cis_<name>.json, `in_hand_bone`: tools/body_b1grab.py) puts them, else at the stock arms'
  hp_weapons (as the game holds them); the common models and items as copies of the human hand's weap_<grip>
  (weap_b1_<grip>, property `grip`: exported under that name on the b1 skeleton), where re/body/grip-weap_b1_<grip>.json
  puts them, else where the human hand holds them (the b1 hand bone's rest is the human one's).
- render=: a sheet per object set (hand alone, with each weapon), three sides, workbench textured; pose= turns bones
  (degrees, the bone's own axes) for the render only."""
import bpy, bmesh, json, math, os, sys
from mathutils import Matrix, Vector
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
opt = dict(a.split('=', 1) for a in args[1:] if '=' in a)
LEVELS = int(opt.get('levels', 0))  # the flat bars: bevelled only (idren2023 2026-11-02: Quest-native later, no wasted geometry)
ROUND_LEVELS = int(opt.get('round', 1))  # the round pieces (joint cylinders, the rod): subdivided once
SEGMENTS = int(opt.get('segments', 1))  # bevel segments on the flat bars (the round pieces keep 2)
ROUND_PIECES = (1, 2, 3, 4)  # the knuckle and middle joint cylinders
BEVEL = float(opt.get('bevel', 0.8)) / 1000
ANGLE = math.radians(float(opt.get('angle', 50)))
GROOVE = float(opt.get('groove', 0.5)) / 1000
# the painted ring in cis_1st_bdroid_hands.tga (256 px, fitted to its dark pixels): centre, radius, the dark band's
# inner and outer radius (px)
RING = ((251.0, 199.0), 44.4, (-2.5, 0.5))
RENDER = opt.get('render')
STOCK = opt.get('stock') == '1'  # every weapon as the game holds it (stock hp_weapons, the stock models), saved grips ignored
POSE = {b: [float(x) for x in v.split(',')] for b, v in (p.split(':') for p in opt.get('pose', '').split(';') if p)}
A = 'C:/LucasArts/BFBuilder/Assets/Sides/'
UNIT_MSH = A + 'CIS/MSH/cis_1st_bdroid.msh'
TEXTURE = 'cis_1st_bdroid_hands'
# the B1 classes' held weapons (idren2023 2026-11-02: every droid weapon; the classes' ODFs): the CIS models, and the common
# models and Holster's items the human hand holds already (their grips under the same names, on the b1 skeleton)
WEAPONS = ('rifle', 'pistol', 'sniperrifle', 'launcher', 'fusioncutter')
COMMON = ('com_torpedo', 'com_grenadethermal', 'item_landmine', 'item_probedroid', 'item_powerup')
# stock=1: idren2023's changes to the stock hold per weapon (2026-11-02 sheets): move (mm, arm space), turn (degrees about
# the arm's x, y, z in that order, round pivot, mm) and the fingers' pose (degrees, the bone's own axes, BodyArm's orders)
TWEAKS = json.load(open('re/body/b1-stock-tweaks.json')) if os.path.exists('re/body/b1-stock-tweaks.json') else {}
ONLY = [x for x in opt.get('only', '').split(',') if x]
F = json.load(open('re/body/hand-frame-all_1st_trooper.json'))
R, W, S = Matrix(F['to_hand']), Vector(F['wrist']), F['scale']
human = bpy.data.objects['r_hand_retopo'].parent

# the stock pieces of the right hand (numbered largest first, as on re/body/b1-rig-proposal.png) and their bones
PIECE_BONES = {0: 'hand', 1: 'grip_2', 2: 'trigger_1', 3: 'grip_1', 4: 'trigger_2', 5: 'hand', 6: 'thumb_1',
               7: 'grip_2', 8: 'trigger_2', 9: 'thumb_1', 10: 'trigger_1', 11: 'grip_1', 12: 'thumb_2', 13: None}


def to_blender(p):
    return Vector((p[0], -p[2], p[1]))  # as tools/body_import.py


def H(p):
    return (R @ (to_blender(p) - W)) * S  # the arms msh -> our hand frame (the B1's wrist is the trooper's)


model = mshgeo.load(UNIT_MSH)


def node_mesh(name):
    """A node's triangles in our hand frame: positions, triangles, uvs per corner, textures."""
    node = next(n for n in model['nodes'] if n['name'] == name)
    verts, tris, uvs = [], [], []
    for seg in node['segments']:
        base = len(verts)
        verts += [H(p) for p in seg['wpos']]
        uv = seg.get('uv') or [(0, 0)] * len(seg['wpos'])
        for a, b, c in seg['tris']:
            tris.append((base + a, base + b, base + c))
            uvs.append((uv[a], uv[b], uv[c]))
    return verts, tris, uvs


def pieces(verts, tris):
    """Connected pieces (positions welded), largest first: lists of triangle indices."""
    key = {}
    rep = [key.setdefault(tuple(round(c, 5) for c in v), i) for i, v in enumerate(verts)]
    parent = list(range(len(verts)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for t in tris:
        for a in t[1:]:
            parent[find(rep[a])] = find(rep[t[0]])
    groups = {}
    for ti, t in enumerate(tris):
        groups.setdefault(find(rep[t[0]]), set()).add(ti)
    count = {r: len({rep[i] for ti in g for i in tris[ti]}) for r, g in groups.items()}
    return [sorted(groups[r]) for r in sorted(groups, key=lambda r: -count[r])]


# ---- the skeleton ----
hand_v, hand_t, hand_uv = node_mesh('bdroid_1st_r_hand')
arm_v, arm_t, arm_uv = node_mesh('bdroid_1st_r_arm')
# each piece by its centre (mm, as numbered on the proposal sheet; equal sizes make the size order ambiguous)
PIECE_CENTRES = ((-3, 30, -5), (13, 111, -16), (-28, 50, -8), (18, 56, -8), (-33, 106, -17), (-17, 23, -14),
                 (-15, 49, -66), (12, 97, -43), (-31, 101, -46), (-16, 34, -36), (-28, 74, -12), (14, 78, -11),
                 (-15, 72, -58), (-10, 108, -17))
found = pieces(hand_v, hand_t)
assert len(found) == 14, len(found)


def piece_centre(tri_ids):
    vs = [hand_v[i] for ti in tri_ids for i in hand_t[ti]]
    return sum(vs, Vector()) / len(vs) * 1000


P = [min(found, key=lambda g: (piece_centre(g) - Vector(c)).length) for c in PIECE_CENTRES]
assert len({id(g) for g in P}) == 14, 'pieces not told apart'


def centre(k):
    vs = {i for ti in P[k] for i in hand_t[ti]}
    return sum((hand_v[i] for i in vs), Vector()) / len(vs)


def far_end(k, frm):
    vs = [hand_v[i] for i in {i for ti in P[k] for i in hand_t[ti]}]
    d = max((v - frm).length for v in vs)
    cap = [v for v in vs if (v - frm).length > d - 0.006]
    return sum(cap, Vector()) / len(cap)


elbow = [v for v in arm_v if v.y < min(u.y for u in arm_v) + 0.008]
JOINTS = {
    'forearm': (sum(elbow, Vector()) / len(elbow), Vector()),
    'trigger_1': (centre(2), centre(4)), 'trigger_2': (centre(4), far_end(8, centre(4))),
    'grip_1': (centre(3), centre(1)), 'grip_2': (centre(1), far_end(7, centre(1))),
    'thumb_1': (centre(5), centre(6)), 'thumb_2': (centre(6), far_end(12, centre(6)))}
PARENTS = {'forearm': None, 'hand': 'forearm', 'trigger_1': 'hand', 'trigger_2': 'trigger_1', 'grip_1': 'hand',
           'grip_2': 'grip_1', 'thumb_1': 'hand', 'thumb_2': 'thumb_1'}

old = bpy.data.objects.get('b1_skeleton')
for o in [o for o in bpy.data.objects if o.name.startswith(('b1_', 'weap_cis_', 'weap_b1_')) or o is old]:
    bpy.data.objects.remove(o, do_unlink=True)
arm_data = bpy.data.armatures.new('b1_skeleton')
rig = bpy.data.objects.new('b1_skeleton', arm_data)
rig['skeleton'] = 'b1'
bpy.context.scene.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
human_hand = human.data.bones['hand']
for name, parent in PARENTS.items():
    b = arm_data.edit_bones.new(name)
    if name == 'hand':  # the human hand bone's rest: arm space and the grip frames the same
        b.head, b.tail = (0, 0, 0), (0, 0.05, 0)
        b.matrix = human.matrix_world @ human_hand.matrix_local
        b.length = human_hand.length
    else:
        b.head, b.tail = JOINTS[name]
        b.align_roll(Vector((1, 0, 0)).cross(b.tail - b.head).normalized())  # x along the arm's x
    if parent:
        b.parent = arm_data.edit_bones[parent]
bpy.ops.object.mode_set(mode='OBJECT')

# ---- the parts ----
material = bpy.data.materials.get(TEXTURE) or bpy.data.materials.new(TEXTURE)
if not material.node_tree or not any(n.type == 'TEX_IMAGE' for n in material.node_tree.nodes):
    material.use_nodes = True
    img = material.node_tree.nodes.new('ShaderNodeTexImage')
    material.node_tree.nodes.active = img  # (textured views draw the active image node)
    img.image = bpy.data.images.load(A + 'CIS/MSH/' + TEXTURE + '.tga')
    material.node_tree.links.new(img.outputs['Color'], material.node_tree.nodes['Principled BSDF'].inputs['Base Color'])


def ring_distance(uv):
    """A uv's distance from the painted ring's line, in texels (negative inside)."""
    (cx, cy), r, _ = RING
    return math.hypot(uv.x * 256 - cx, (1 - uv.y) * 256 - cy) - r


def cut_ring(bm):
    """The ring as a groove: the flat faces it crosses subdivided to about a millimetre, their vertices in the dark
    band pushed in along the normal (a smooth profile, deepest mid-band)."""
    layer = bm.loops.layers.uv.active
    inner, outer = RING[2]
    for _ in range(3):
        faces = [f for f in bm.faces if abs(f.normal.z) > 0.8 and
                 any(inner - 4 < ring_distance(l[layer].uv) < outer + 4 for l in f.loops) and
                 max(e.calc_length() for e in f.edges) > 0.0012]
        if not faces:
            break
        edges = list({e for f in faces for e in f.edges})
        bmesh.ops.subdivide_edges(bm, edges=edges, cuts=1, use_grid_fill=True)
        bm.normal_update()
    moved = 0
    for v in bm.verts:
        if not v.link_loops or abs(v.normal.z) < 0.8:
            continue
        d = ring_distance(v.link_loops[0][layer].uv)
        if inner < d < outer:
            t = (d - inner) / (outer - inner)
            v.co -= v.normal * GROOVE * math.sin(math.pi * t)
            moved += 1
    print('B1 ring groove: %d vertices pushed in' % moved)


def whole_ball():
    """The wrist ball whole (idren2023 2026-11-02: the stock sphere1 is an open band, its caps hidden in the palm and the
    rod): a sphere at its centre and radius, latitude and longitude spread over the stock's uv rectangle (the grey
    metal)."""
    v, t, uv = node_mesh('sphere1')
    centre = sum(v, Vector()) / len(v)
    radius = sum((p - centre).length for p in v) / len(v)
    us = [c for tri in uv for c in tri]
    u0, u1 = min(c[0] for c in us), max(c[0] for c in us)
    v0, v1 = min(c[1] for c in us), max(c[1] for c in us)
    rings, segments = 10, 20  # (b482: 12 x 24 subdivided once was 2 300 triangles)
    verts, tris, uvs = [], [], []
    for i in range(rings + 1):
        lat = math.pi * i / rings
        for j in range(segments + 1):
            lon = 2 * math.pi * j / segments
            verts.append(centre + radius * Vector((math.sin(lat) * math.cos(lon), math.cos(lat),
                                                   math.sin(lat) * math.sin(lon))))
    corner = lambda i, j: ((u0 + (u1 - u0) * j / segments), (v0 + (v1 - v0) * i / rings))
    for i in range(rings):
        for j in range(segments):
            a, b = i * (segments + 1) + j, i * (segments + 1) + j + 1
            c, d = a + segments + 1, b + segments + 1
            for tri, ij in (((a, c, b), ((i, j), (i + 1, j), (i, j + 1))),
                            ((b, c, d), ((i, j + 1), (i + 1, j), (i + 1, j + 1)))):
                tris.append(tri)
                uvs.append(tuple(corner(*x) for x in ij))
    return verts, tris, uvs, list(range(len(tris)))


def cap_open_end(bm):
    """A joint cylinder's open end closed (idren2023 2026-11-02: the middle joints' inner ends were capped by the dropped
    cross piece; the knuckles' inner ends are open in the stock too): the hole filled with one face, its uvs those of
    the closed end's vertices mirrored across the cylinder's middle (the axis runs along x)."""
    layer = bm.loops.layers.uv.active
    open_edges = [e for e in bm.edges if len(e.link_faces) == 1]
    if not open_edges:
        return
    middle = sum((v.co.x for v in bm.verts), 0.0) / len(bm.verts)
    old = list(bm.faces)
    bmesh.ops.holes_fill(bm, edges=open_edges, sides=0)
    ends = [f for f in old if abs(f.normal.x) > 0.9]
    for f in set(bm.faces) - set(old):
        for loop in f.loops:
            mirrored = loop.vert.co.copy()
            mirrored.x = 2 * middle - mirrored.x
            near = min((l for e in ends for l in e.loops), key=lambda l: (l.vert.co - mirrored).length)
            loop[layer].uv = near[layer].uv
    print('B1 capped %d open edges' % len(open_edges))


def smooth_piece(name, verts, tris, uvs, tri_ids, bone, groove=False, levels=LEVELS, cap=False, bevel=True, segments=2):
    """One rigid piece: welded, flat triangles merged, hard edges bevelled, subdivided, on one bone."""
    used = sorted({i for ti in tri_ids for i in tris[ti]})
    index = {i: k for k, i in enumerate(used)}
    me = bpy.data.meshes.new(name)
    me.from_pydata([verts[i] for i in used], [], [tuple(index[i] for i in tris[ti]) for ti in tri_ids])
    layer = me.uv_layers.new(name='UV')
    for poly, ti in zip(me.polygons, tri_ids):
        for j, li in enumerate(poly.loop_indices):
            layer.data[li].uv = uvs[ti][j]  # msh v runs up the image, as Blender's (body_export writes 1 - v)
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0002)
    bmesh.ops.dissolve_limit(bm, angle_limit=math.radians(2), verts=bm.verts, edges=bm.edges, delimit={'UV'})
    if cap:
        cap_open_end(bm)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    if BEVEL and bevel:
        hard = [e for e in bm.edges if len(e.link_faces) == 2 and e.calc_face_angle(0) > ANGLE]
        bmesh.ops.bevel(bm, geom=hard, offset=BEVEL, segments=segments, profile=0.5, affect='EDGES', clamp_overlap=True)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    if levels:
        bpy.context.view_layer.objects.active = ob
        mod = ob.modifiers.new('subd', 'SUBSURF')
        mod.levels = mod.render_levels = levels
        mod.uv_smooth = 'PRESERVE_CORNERS'
        bpy.ops.object.modifier_apply(modifier='subd')
    bm = bmesh.new()
    bm.from_mesh(me)
    centroid = sum((v.co for v in bm.verts), Vector()) / len(bm.verts)
    bm.normal_update()
    if sum((f.calc_center_median() - centroid).dot(f.normal) * f.calc_area() for f in bm.faces) < 0:
        bmesh.ops.reverse_faces(bm, faces=bm.faces)
    if GROOVE and groove:
        cut_ring(bm)
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = True
    ob.vertex_groups.new(name=bone).add(list(range(len(me.vertices))), 1.0, 'REPLACE')
    me.materials.append(material)
    return ob


def part(name, obs):
    bpy.ops.object.select_all(action='DESELECT')
    for o in obs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = obs[0]
    if len(obs) > 1:
        bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    ob.name = ob.data.name = name
    ob['texture'] = TEXTURE
    ob.parent = rig
    ob.matrix_world = Matrix.Identity(4)
    ob.modifiers.new('rig', 'ARMATURE').object = rig
    print('B1 part %s: %d faces, %d vertices, bones %s' % (
        name, len(ob.data.polygons), len(ob.data.vertices), sorted(g.name for g in ob.vertex_groups)))
    return ob


def raw_piece(name, verts, tris, uvs, tri_ids, bone):
    """A stock piece as it is (rawmesh=1: for side-by-side renders against the stock hand)."""
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], [tris[ti] for ti in tri_ids])
    layer = me.uv_layers.new(name='UV')
    for poly, ti in zip(me.polygons, tri_ids):
        for j, li in enumerate(poly.loop_indices):
            layer.data[li].uv = uvs[ti][j]
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    ob.vertex_groups.new(name=bone).add(list(range(len(me.vertices))), 1.0, 'REPLACE')
    me.materials.append(material)
    return ob


if opt.get('rawmesh') == '1':
    smooth_piece = lambda name, verts, tris, uvs, tri_ids, bone, **kw: raw_piece(name, verts, tris, uvs, tri_ids, bone)
    whole_ball = None
hand_obs = [smooth_piece('b1_piece_%d' % k, hand_v, hand_t, hand_uv, P[k], bone or 'hand', groove=k == 0,
                         cap=k in (1, 2, 3, 4), levels=ROUND_LEVELS if k in ROUND_PIECES else LEVELS,
                         segments=2 if k in ROUND_PIECES else SEGMENTS)
            for k, bone in PIECE_BONES.items() if bone or not whole_ball]
if whole_ball:
    hand_obs.append(smooth_piece('b1_piece_ball', *whole_ball(), 'hand', levels=0, bevel=False))
hand = part('b1_hand', hand_obs)
arm = part('b1_arm', [smooth_piece('b1_piece_arm', arm_v, arm_t, arm_uv, list(range(len(arm_t))), 'forearm',
                                   levels=ROUND_LEVELS)])

# ---- the weapons, as the game holds them ----
def hand_rest():
    return rig.matrix_world @ rig.data.bones['hand'].matrix_local


hp_rows, hp_t = next(n for n in model['nodes'] if n['name'].lower() == 'hp_weapons')['world']
weapons = []
COMMON_MSH = 'C:/LucasArts/BFBuilder/Assets/Shipped Add On/DataTAT3/Common/MSHs/'
STOCK_MSH = {'com_torpedo': 'com_1st_weap_inf_torpedo', 'com_grenadethermal': 'com_1st_weap_inf_grenadethermal',
             'item_landmine': 'com_1st_weap_inf_landmine'}
# the model and size Holster draws in the hand where it is not the stock one (Holster ItemScale, DispensedModel)
DRAWN = {'com_grenadethermal': ('com_1st_weap_inf_grenadethermal', 0.8),
         'item_landmine': ('com_weap_inf_landmine_placed', 0.6)}


def stock_weapon(path, obname):
    wm = mshgeo.load(path)
    verts, faces, uvs, textures = [], [], [], []
    for node in wm['nodes']:  # the vertices tools/body_export.py fits the grip over, in its order
        if not mshgeo.is_drawn(node) or 'lens' in node['name'].lower():
            continue
        for seg in node['segments']:
            if not seg.get('uv') or not seg.get('texture') or seg['texture'].endswith('.pic'):
                continue
            base = len(verts)
            for p in seg['wpos']:
                q = [sum(p[r] * hp_rows[r][c] for r in range(3)) + hp_t[c] for c in range(3)]  # row vectors
                verts.append(H(q))
            for a, b, c in seg['tris']:
                faces.append((base + a, base + b, base + c))
                uvs.append((seg['uv'][a], seg['uv'][b], seg['uv'][c]))
            textures.append(seg['texture'])
    me = bpy.data.meshes.new(obname)
    me.from_pydata(verts, [], faces)
    layer = me.uv_layers.new(name='UV')
    for poly, tuv in zip(me.polygons, uvs):
        for j, li in enumerate(poly.loop_indices):
            layer.data[li].uv = tuv[j]
    tex = os.path.splitext(max(set(textures), key=textures.count))[0]
    m = bpy.data.materials.get(tex) or bpy.data.materials.new(tex)
    if not m.use_nodes:
        m.use_nodes = True
        img = m.node_tree.nodes.new('ShaderNodeTexImage')
        m.node_tree.nodes.active = img  # (textured views draw the active image node)
        tga = os.path.join(os.path.dirname(path), tex + '.tga')
        if os.path.exists(tga):
            img.image = bpy.data.images.load(tga)
        m.node_tree.links.new(img.outputs['Color'], m.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
    me.materials.append(m)
    ob = bpy.data.objects.new(obname, me)
    bpy.context.scene.collection.objects.link(ob)
    ob.parent, ob.parent_type, ob.parent_bone = rig, 'BONE', 'hand'
    ob.matrix_world = Matrix.Identity(4)
    print('B1 weapon %s: %d vertices, texture %s' % (ob.name, len(verts), tex))
    return ob


for name in WEAPONS:
    path = A + 'CIS/MSH/cis_1st_weap_inf_%s.msh' % name
    if not os.path.exists(path):
        continue
    ob = stock_weapon(path, 'weap_cis_' + name)
    jp = os.path.join('re', 'body', 'grip-weap_cis_%s.json' % name)
    saved = json.load(open(jp)) if os.path.exists(jp) else {}
    if 'in_hand_bone' in saved and not STOCK:
        ob.matrix_world = hand_rest() @ Matrix(saved['in_hand_bone'])
    weapons.append(ob)
    if not saved:
        # the controller's handle through the loop of the closed fingers and thumb (the stock rest), across the hand
        loop = (centre(4) + centre(1) + centre(6) + JOINTS['thumb_2'][1]) / 4
        json.dump({'weapon': ob.name, 'pose': 'b1 rest (the stock closed hand)', 'axis_point': list(loop),
                   'axis_direction': [1.0, 0.0, 0.0], 'placement': 'stock hp_weapons (tools/body_b1.py)'},
                  open(jp, 'w'), indent=1)
for name in COMMON:
    if STOCK and name in STOCK_MSH:  # the stock first-person model at the stock hp_weapons
        ob = stock_weapon(COMMON_MSH + STOCK_MSH[name] + '.msh', 'weap_b1_' + name)
        ob['grip'], ob['msh'] = name, COMMON_MSH + STOCK_MSH[name] + '.msh'  # (tools/body_export.py: the model fitted)
        weapons.append(ob)
        continue
    human_weapon = bpy.data.objects.get('weap_' + name)
    if not human_weapon:
        print('B1 no human weap_%s to copy' % name)
        continue
    ob = human_weapon.copy()
    ob.name = 'weap_b1_' + name
    ob['grip'] = name
    bpy.context.scene.collection.objects.link(ob)
    world = human_weapon.matrix_world.copy()
    ob.parent, ob.parent_type, ob.parent_bone = rig, 'BONE', 'hand'
    jp = os.path.join('re', 'body', 'grip-weap_b1_%s.json' % name)
    if not os.path.exists(jp):  # the human grip's json as the start (its axis; the place is the human's)
        human_json = os.path.join('re', 'body', 'grip-weap_%s.json' % name)
        saved = json.load(open(human_json)) if os.path.exists(human_json) else {}
        saved = {k: v for k, v in saved.items() if k in ('axis_point', 'axis_direction')}
        saved.update(weapon=ob.name, placement='as the human hand holds it (tools/body_b1.py)')
        json.dump(saved, open(jp, 'w'), indent=1)
    saved = json.load(open(jp))
    ob.matrix_world = hand_rest() @ Matrix(saved['in_hand_bone']) if 'in_hand_bone' in saved else world
    weapons.append(ob)
    print('B1 weapon %s: a copy of weap_%s' % (ob.name, name))
if STOCK:
    def tweak_steps(t):  # a tweak, then its 'after' step (another pivot, turn and move on the result)
        return [t] + (tweak_steps(t['after']) if 'after' in t else [])
    def apply_tweak(ob, tweak):
        for t in tweak_steps(tweak):
            pivot = Vector(t.get('pivot', (0, 0, 0))) / 1000
            turn = Matrix.Identity(4)
            for axis, a in zip('XYZ', t.get('turn', (0, 0, 0))):
                turn = Matrix.Rotation(math.radians(a), 4, axis) @ turn
            if 'align' in t:  # a direction of the stock model turned onto another (before move, round pivot)
                turn = Vector(t['align'][0]).rotation_difference(Vector(t['align'][1])).to_matrix().to_4x4() @ turn
            ob.matrix_world = (Matrix.Translation(Vector(t.get('move', (0, 0, 0))) / 1000 + pivot) @ turn @
                               Matrix.Translation(-pivot) @ ob.matrix_world)
    for ob in weapons:
        apply_tweak(ob, TWEAKS.get(ob.get('grip', ob.name[9:]), {}))
    # Holster draws the grenade and the mine smaller (ItemScale), the mine as the placed one: the drawn model put where
    # the posed one is (its centre, its flat side's normal: the axis of least spread), at the drawn size
    import numpy as np

    def spread(ob):
        pts = np.array([list(ob.matrix_world @ v.co) for v in ob.data.vertices])
        c = pts.mean(axis=0)
        return Vector(c), Vector(np.linalg.eigh(np.cov((pts - c).T))[1][:, 0])

    for i, ob in enumerate(weapons):
        drawn = DRAWN.get(ob.get('grip'))
        if not drawn:
            continue
        name, scale = drawn
        c, n = spread(ob)
        new = stock_weapon(COMMON_MSH + name + '.msh', ob.name + '_drawn')
        c2, n2 = spread(new)
        turn = n2.rotation_difference(n if n.dot(n2) >= 0 else -n).to_matrix().to_4x4()
        new.matrix_world = (Matrix.Translation(c) @ turn @ Matrix.Scale(scale, 4) @ Matrix.Translation(-c2) @
                            new.matrix_world)
        new['grip'], new['msh'], new['item_scale'] = ob['grip'], COMMON_MSH + name + '.msh', scale
        bpy.data.objects.remove(ob)
        new.name = 'weap_b1_' + new['grip']
        weapons[i] = new
        apply_tweak(new, TWEAKS.get(new['grip'], {}).get('drawn', {}))  # then its own steps on the drawn model
        print('B1 drawn %s: %s at %.2f' % (new.name, name, scale))
for b in rig.data.bones:
    print('B1 bone %-10s head %s tail %s' % (b.name, tuple(round(x * 1000) for x in b.head_local),
                                           tuple(round(x * 1000) for x in b.tail_local)))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))

# ---- render sheets ----
if RENDER:
    scene = bpy.context.scene
    from mathutils.bvhtree import BVHTree

    def set_pose(pose):
        for pb in rig.pose.bones:
            pb.rotation_mode = 'ZXY' if pb.name.endswith('_1') and not pb.name.startswith('thumb') else 'XYZ'
            pb.rotation_euler = [math.radians(x) for x in pose.get(pb.name, (0, 0, 0))]
        bpy.context.view_layer.update()

    def clipping(w):
        """The posed hand's vertices inside the weapon (ray parity, 3 rays), counted per bone."""
        bm = bmesh.new()
        bm.from_mesh(w.data)
        bm.transform(w.matrix_world)
        tree = BVHTree.FromBMesh(bm)
        lo = Vector([min(v.co[i] for v in bm.verts) for i in range(3)])
        hi = Vector([max(v.co[i] for v in bm.verts) for i in range(3)])
        dg = bpy.context.evaluated_depsgraph_get()
        ev = hand.evaluated_get(dg)
        me = ev.to_mesh()
        names = {g.index: g.name for g in hand.vertex_groups}
        bone_of = [names[max(v.groups, key=lambda g: g.weight).group] if v.groups else '?' for v in hand.data.vertices]
        rays = (Vector((0, 0, 1)), Vector((0, 0, -1)), Vector((0.3, 0.9, 0.1)).normalized())
        out = {}
        for v in me.vertices:
            p = ev.matrix_world @ v.co
            if not all(lo[i] - 0.001 < p[i] < hi[i] + 0.001 for i in range(3)):
                continue
            votes = 0
            for d in rays:
                q, n = p.copy(), 0
                for _ in range(24):
                    hit = tree.ray_cast(q, d, 1.0)
                    if hit[0] is None:
                        break
                    n += 1
                    q = hit[0] + d * 1e-5
                votes += n % 2
            if votes >= 2:
                b = bone_of[v.index] if v.index < len(bone_of) else '?'
                out[b] = out.get(b, 0) + 1
        ev.to_mesh_clear()
        return out

    set_pose(POSE)
    shown = {hand, arm}
    for o in scene.objects:
        o.hide_render = o not in shown and o.type == 'MESH'
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.color_type = 'TEXTURE'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.show_cavity = True
    scene.render.resolution_x, scene.render.resolution_y = 600, 600
    scene.render.film_transparent = False
    scene.world = scene.world or bpy.data.worlds.new('b1_world')
    scene.world.color = (0.55, 0.57, 0.6)
    cam = bpy.data.objects.new('b1_cam', bpy.data.cameras.new('b1_cam'))
    cam.data.type = 'ORTHO'
    scene.collection.objects.link(cam)
    scene.camera = cam
    VIEWS = (('thumb side', (-1, 0, 0)), ('back three-quarter', (0.6, -0.5, 0.6)), ('palm three-quarter', (-0.5, 0.3, -0.8)),
             ('fingertips', (0, 1, 0)))
    GUN_VIEWS = (('gun right side', (0, 0, 1), (-1, 0, 0)), ('gun left side', (0, 0, -1), (-1, 0, 0)),
                 ('gun top', (-1, 0, 0), (0, 1, 0)), ('from behind (eye)', (-0.35, -1, 0.15), (-1, 0, 0)),
                 ('from behind left', (-0.2, -1, -0.9), (-1, 0, 0)))
    files = []
    for label, extra in [('hand', None)] + [(w.get('grip', w.name[9:]), w) for w in weapons]:
        if ONLY and label not in ONLY:
            continue
        for o in weapons:
            o.hide_render = o is not extra
        set_pose(dict(POSE, **TWEAKS.get(label, {}).get('pose', {})) if extra else POSE)
        if extra:
            print('B1 clipping %s: %s' % (label, clipping(extra) or 'none'))
        mid, size = (Vector((0, 0.12, -0.02)), 0.45) if extra else (Vector((-0.005, 0.055, -0.03)), float(opt.get('handsize', 0.17)))
        # the hand alone also close round the wrist ball (its caps were open in the stock)
        # a held weapon's views upright as the gun is: its top toward the thumb side (-x), barrel +y, right side +z
        views = [(v, d, (0, 0, 1)) for v, d in VIEWS] if not extra else GUN_VIEWS
        wrist = [('wrist ' + v, d, u, Vector(), 0.08) for v, d, u in views[:3]] if not extra else [
            ('close ' + v, d, u, Vector((-0.005, 0.06, -0.04)), 0.22) for v, d, u in views]  # the grip in the hand
        for view, d, up, at, scale in [(v, d, u, mid + (Vector((0, 0.04, -0.02)) if 'handsize' in opt and not extra else Vector()), size) for v, d, u in views] + wrist:
            cam.data.ortho_scale = scale
            d = Vector(d).normalized()
            cam.location = at + d * 2
            right = Vector(up).cross(d).normalized()
            cam.rotation_euler = Matrix((right, d.cross(right), d)).transposed().to_euler()
            path = os.path.abspath('%s-%s-%s.png' % (RENDER, label, view.replace(' ', '')))
            scene.render.filepath = path
            bpy.ops.render.render(write_still=True)
            files.append((label, view, path))
    json.dump(files, open(os.path.abspath(RENDER + '-files.json'), 'w'))
    print('B1 rendered %d views' % len(files))
