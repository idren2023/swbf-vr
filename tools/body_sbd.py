"""Blender (blender -b --factory-startup re/body/r_arm_grips.blend -P tools/body_sbd.py -- <out.blend>
[bevel=3] [angle=50] [rawmesh=1] [render=<png prefix>] [size=0.26] [centre=-10,40,-15 (mm)] [views=0123 (of 01234567)] [only=hand|arm] [shade=solid|pieces] [poses=<file: label|bone:x,y,z;.. per line>]): the super battle
droid's first-person right arm on a skeleton of its own (PLAN 60 step 4e; idren2023 2026-11-02: the whole arm rebuilt,
one block finger as the grip finger and the thumb, no trigger finger).

- frame: the stock arm (cis_1st_sbdroid) has its own wrist and hand box, so it is not carried through the trooper's
  hand frame (as the B1's is): msh -> Blender axes, centred on the stock hand box's back face (the wrist), turned half
  round z so +y runs to the fingers, +z is the back of the hand and -x the thumb side, at our 0.712; then turned so the
  fist block (the hand) is level: its top face up, its front edge along x (the stock arm is rolled about 21 degrees).
- stock hand pieces (cis_superbattledroid_r_hand, five): the fist block (the hand, whole), the finger's middle plate
  (grip_1, hinged on the block's front top edge) and tip plate (grip_2, hinged on the middle plate's bottom edge), the
  thumb's two bars; sphere1 is the thumb's base ball (on the hand). Open boundaries are closed.
  Every hinge runs along the hand's x (idren2023 2026-11-03: a hinge in the plane of the hand).
- skeleton sbd (armature sbd_skeleton, property `skeleton` = "sbd"): forearm, hand (the human hand bone's rest),
  grip_1-2, thumb_1-3 (machined; the stock thumb has two); the stock fist is the rest; every bone's x along the arm's x (x < 0 curls toward the palm).
- parts: sbd_hand (the hand pieces, flat faces bevelled once, not subdivided) and sbd_arm (forearm shell, its cap and
  the barrels, as the stock), texture cis_1st_sbdroid_hands.
- render= with poses=: a sheet's pictures per pose (four sides), <prefix>-files.json."""
import bpy, bmesh, json, math, os, sys
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
opt = dict(a.split('=', 1) for a in args[1:] if '=' in a)
BEVEL = float(opt.get('bevel', 3)) / 1000
LEFT_GAP = -260                 # the left arm's skeleton this far along x from the right one (mm; for viewing only)
SMOOTH_SHARP = math.radians(40)  # smooth-shaded pieces: an edge between faces more than this apart stays sharp
ANGLE = math.radians(float(opt.get('angle', 50)))
RENDER = opt.get('render')
A = 'C:/LucasArts/BFBuilder/Assets/Sides/CIS/MSH/'
TEXTURE = 'cis_1st_sbdroid_hands'
SCALE = 0.7116788
human = bpy.data.objects['r_hand_retopo'].parent
model = mshgeo.load(A + 'cis_1st_sbdroid.msh')


def node(name):
    return next(n for n in model['nodes'] if n['name'] == name)


def to_blender(p):
    return Vector((p[0], -p[2], p[1]))


ball_pts = [to_blender(p) for s in node('sphere1')['segments'] for p in s['wpos']]
BALL = sum(ball_pts, Vector()) / len(ball_pts)
WRIST = Vector((0.045, -0.010, 0.0))  # the hand box's back face, in the ball-centred frame (metres, our scale)


def H(p):
    q = to_blender(p) - BALL
    return Vector((-q.x, -q.y, q.z)) * SCALE - WRIST


def node_mesh(name):
    verts, tris, uvs = [], [], []
    for seg in node(name)['segments']:
        if not seg.get('tris'):
            continue
        base = len(verts)
        verts += [H(p) for p in seg['wpos']]
        uv = seg.get('uv') or [(0, 0)] * len(seg['wpos'])
        for a, b, c in seg['tris']:
            tris.append((base + a, base + b, base + c))
            uvs.append((uv[a], uv[b], uv[c]))
    return verts, tris, uvs


def pieces(verts, tris):
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
        groups.setdefault(find(rep[t[0]]), []).append(ti)
    return list(groups.values())


hand_v, hand_t, hand_uv = node_mesh('cis_superbattledroid_r_hand')
found = pieces(hand_v, hand_t)
assert len(found) == 5, len(found)


def piece_centre(tri_ids):
    vs = [hand_v[i] for ti in tri_ids for i in hand_t[ti]]
    return sum(vs, Vector()) / len(vs)


# each stock piece by its centre (mm in this frame; the ball-centred numbers less WRIST)
CENTRES = {'block': (-13, 76, -2), 'middle': (-17, 89, -13), 'thumb_2': (-68, 59, -43), 'thumb_1': (-58, 30, -21),
           'tip': (-21, 68, -37)}
P = {k: min(found, key=lambda g: (piece_centre(g) * 1000 - Vector(c)).length) for k, c in CENTRES.items()}
assert len({id(g) for g in P.values()}) == 5, 'pieces not told apart'


def ends(tri_ids):
    """A bar's two ends along its longest spread (for the thumb's joints)."""
    vs = [hand_v[i] for i in {i for ti in tri_ids for i in hand_t[ti]}]
    c = sum(vs, Vector()) / len(vs)
    import numpy as np
    w, ev = np.linalg.eigh(np.cov(np.array([list(v - c) for v in vs]).T))
    a = Vector(ev[:, 2])
    pr = [(v - c).dot(a) for v in vs]
    return c + a * min(pr), c + a * max(pr)




def level_frame(tri_ids):
    """The fist block's axes: z its top face's normal, y its front face's, x = y x z (area-weighted face normals)."""
    up, front = Vector(), Vector()
    for ti in tri_ids:
        a, b, c = (hand_v[i] for i in hand_t[ti])
        n = (b - a).cross(c - a) / 2
        if n.length and abs(n.normalized().z) > 0.6:
            up += n if n.z > 0 else -n
        elif n.length and abs(n.normalized().y) > 0.6:
            front += n if n.y > 0 else -n
    z = up.normalized()
    y = (front - z * front.dot(z)).normalized()
    return Matrix((y.cross(z), y, z))  # rows: the hand's axes, so R @ v is v in the hand's frame


R = level_frame(P['block'])
print('SBD hand frame rows', [tuple(round(c, 3) for c in r) for r in R])
hand_v = [R @ v for v in hand_v]
H_stock = H




def front_edge_yaw():
    """The block's front edge (its verts within 10 mm of the front, end to end) against x: the front face's normal is
    a few degrees off it, and the finger plates run along the edge, so the hinges must too."""
    vs = sorted((hand_v[i] for i in {i for ti in P['block'] for i in hand_t[ti]}), key=lambda v: v.x)
    front = max(v.y for v in vs)
    sel = [v for v in vs if front - v.y < 0.010]
    d = sel[-1] - sel[0]
    return math.atan2(d.y, d.x)


yaw = front_edge_yaw()
print('SBD front edge yaw %.1f degrees, turned out' % math.degrees(yaw))
R = Matrix.Rotation(-yaw, 3, 'Z') @ R
hand_v = [Matrix.Rotation(-yaw, 3, 'Z') @ v for v in hand_v]


def verts_of(key):
    return [hand_v[i] for i in {i for ti in P[key] for i in hand_t[ti]}]


def edge_mid(vs, axis, far, band=0.008):
    """The mean of the verts within `band` of the far (far=1) or near end along `axis` (an edge line's middle)."""
    pr = [v[axis] for v in vs]
    end = max(pr) if far else min(pr)
    sel = [v for v in vs if abs(v[axis] - end) < band]
    return sum(sel, Vector()) / len(sel)


ball = [H(p) for s in node('sphere1')['segments'] for p in s['wpos']]
ball_centre = sum(ball, Vector()) / len(ball)
t1a, t1b = ends(P['thumb_1'])
t1_tip = t1a if (t1a - ball_centre).length > (t1b - ball_centre).length else t1b
t2a, t2b = ends(P['thumb_2'])
t2_tip = t2a if (t2a - t1_tip).length > (t2b - t1_tip).length else t2b
block = verts_of('block')
FINGER_X = (min(v.x for v in block) + max(v.x for v in block)) / 2
mid_top, mid_bottom = edge_mid(verts_of('middle'), 2, 1), edge_mid(verts_of('middle'), 2, 0)
tip_back = edge_mid(verts_of('tip'), 1, 0)


def on_x(v):
    return Vector((FINGER_X, v.y, v.z))


JOINTS = {'grip_1': (on_x(mid_top), on_x(mid_bottom)), 'grip_2': (on_x(mid_bottom), on_x(tip_back)),
          'thumb_1': (ball_centre, t1_tip), 'thumb_2': (t1_tip, t2_tip)}
HAND_CENTRE = Vector((-1, 45, -15)) / 1000
ROLL_Z = {}                   # bones whose z axis is set here rather than from the hand's x
THUMB_BALL = 20               # the thumb's base ball's radius (mm): the base segment's width (idren2023 2026-11-03)
THUMB_BALL_SQUASH = 0.35      # its height off the palm, as a share of the radius: a squashed sphere, like the B1's
STOCK = opt.get('stock') == '1' or opt.get('rawmesh') == '1'
# The machined hand (mm, the level frame), designed straight and folded into the stock fist: the film's hand is a
# wide block with the finger's two plates, each seam with two slanted tabs (re/body/ref/b2-hands.png).
HAND_X = (-55, 53)            # the block's width (the stock block's)
HAND_Z = (-9, 9)              # its thickness (film: thin plates, big bevels)
FINGER_Z = (-7.5, 7.5)        # the finger's first plate's thickness (centred on the hinges)
TIP_Z = (-6.5, 6.5)           # the tip plate's, a little thinner (idren2023 2026-11-03: the plates taper)
TAPER = 0.28                  # the hand's width lost from the wrist to the fingertip
TAB = 10                      # how far each hinge's tabs reach back
TAB_AT = (0.24, 0.76)         # the two tabs' centres across the hand (film, re/body/ref/b2-hands.png)
TAB_BASE, TAB_TIP = 7.5, 5    # their half widths at the seam and at their ends (film: 14 % and 9 % of the hand)
WRIST_TONGUE = (-37, 37, 24)  # the hand's back end between the wrist pins: x range, how far it runs
HINGE_1 = 88                  # y of the block-to-finger hinge (the stock block's front is at 87-93)
HINGE_2 = HINGE_1 + 46        # the plates' hinge (the stock middle plate is 45 long)
TIP_END = HINGE_2 + 34        # the tip plate's end (the stock tip plate is about 31 long)
CURL = (-90, -90)             # the stock fist: each hinge folded a right angle toward the palm
def across(f, y):
    """x (mm) at fraction f across the hand at y: the hand narrows steadily toward the fingertip (TAPER)."""
    w = (HAND_X[1] - HAND_X[0]) * (1 - TAPER * y / TIP_END)
    return (HAND_X[0] + HAND_X[1]) / 2 + (f - 0.5) * w


if not STOCK:
    def fold_about(y, deg):
        """A turn of `deg` about the x line at (y, z=0) (mm): negative curls toward the palm."""
        return (Matrix.Translation((0, y / 1000, 0)) @ Matrix.Rotation(math.radians(deg), 4, 'X') @
                Matrix.Translation((0, -y / 1000, 0)))
    FOLD = {'hand': Matrix.Identity(4), 'grip_1': fold_about(HINGE_1, CURL[0])}
    FOLD['grip_2'] = FOLD['grip_1'] @ fold_about(HINGE_2, CURL[1])
    h1 = Vector((FINGER_X, HINGE_1 / 1000, 0))
    h2 = FOLD['grip_1'] @ Vector((FINGER_X, HINGE_2 / 1000, 0))
    end = FOLD['grip_2'] @ Vector((FINGER_X, TIP_END / 1000, 0))
    JOINTS['grip_1'], JOINTS['grip_2'] = (h1, h2), (h2, end)
    THUMB_SPLIT = 0.62  # the middle block's share of the stock second thumb bar; the rest is the tip
    # in the palm where a human thumb's base joint is: the squashed ball's centre on the palm (idren2023 2026-11-03)
    # ... and THUMB_EDGE mm in from the hand's edge (idren2023: a few millimetres between the edge and the ball)
    THUMB_EDGE = 4
    THUMB_BASE = Vector((across(0, 30) + THUMB_EDGE + THUMB_BALL, 30, HAND_Z[0])) / 1000
    JOINTS['thumb_1'] = (THUMB_BASE, t1_tip)
    # One hinge axis for the whole thumb (idren2023: the tip must stay in line with the base segment): the middle and tip
    # are laid into the base segment's plane (the stock second bar leans out of it), and the axis is signed so that,
    # as for the finger, a negative turn curls the thumb closed (toward the hand).
    d1 = (t1_tip - THUMB_BASE).normalized()
    THUMB_AXIS = (Vector((1, 0, 0)) - d1 * d1.x).normalized()
    d2 = t2_tip - t1_tip
    flat = (d2 - THUMB_AXIS * d2.dot(THUMB_AXIS)).normalized() * d2.length
    split, t2_flat = t1_tip + flat * THUMB_SPLIT, t1_tip + flat
    JOINTS['thumb_2'], JOINTS['thumb_3'] = (t1_tip, split), (split, t2_flat)
    toward_hand = HAND_CENTRE - t1_tip
    if THUMB_AXIS.cross(flat).dot(toward_hand) > 0:
        THUMB_AXIS = -THUMB_AXIS  # -x turns y toward -z = toward the hand
    for name in ('thumb_1', 'thumb_2', 'thumb_3'):
        h, t = JOINTS[name]
        ROLL_Z[name] = THUMB_AXIS.cross((t - h).normalized())
arm_parts = [node_mesh(n) for n in ('cis_superbattledroid_r_forearm', 'cis_superbattledroid_r_handcap', 'cylinder2',
                                    'cylinder3', 'cylinder4', 'cylinder5')]
arm_y = min(v.y for v, _, _ in [(v, 0, 0) for part in arm_parts for v in part[0]])
elbow = [v for part in arm_parts[:1] for v in part[0] if v.y < arm_y + 0.010]
JOINTS['forearm'] = (sum(elbow, Vector()) / len(elbow), Vector())
# The wrist on the hand's hinge (idren2023 2026-11-03: a rotation, not a move): the stock forearm's two wrist bosses (its
# side pins) are not opposite each other in this frame; the line through them, the stock wrist's hinge, is rolled
# about 22 degrees and turned about 9 from the hand's (the hand bone's x through its origin). The forearm is turned so
# that line lies on the hand's hinge, the bosses' midpoint on its origin; the hand stays where it is (its frame is the
# controller's and every grip's). (mm)
WRIST_BOSSES = (Vector((-49, -24, 23)), Vector((47, -8, -16)))  # the stock bosses' centres (thumb side, other side)
_mid = (WRIST_BOSSES[0] + WRIST_BOSSES[1]) / 2000
FOREARM_TURN = ((WRIST_BOSSES[1] - WRIST_BOSSES[0]).rotation_difference(Vector((1, 0, 0))).to_matrix().to_4x4() @
                Matrix.Translation(-_mid))


def forearm_place(p):
    return FOREARM_TURN @ p if not (opt.get('stock') == '1' or opt.get('rawmesh') == '1') else p


JOINTS['forearm'] = (forearm_place(JOINTS['forearm'][0]), Vector())
print('SBD forearm turned %.1f degrees' % math.degrees(
    (WRIST_BOSSES[1] - WRIST_BOSSES[0]).angle(Vector((1, 0, 0)))))
PARENTS = {'forearm': None, 'hand': 'forearm', 'grip_1': 'hand', 'grip_2': 'grip_1', 'thumb_1': 'hand',
           'thumb_2': 'thumb_1'}
if 'thumb_3' in JOINTS:
    PARENTS['thumb_3'] = 'thumb_2'
JOINTS_PARENT = PARENTS

old = bpy.data.objects.get('sbd_skeleton')
for o in [o for o in bpy.data.objects if o.name.startswith(('sbd_', 'weap_sbd_')) or o is old]:
    bpy.data.objects.remove(o, do_unlink=True)
arm_data = bpy.data.armatures.new('sbd_skeleton')
rig = bpy.data.objects.new('sbd_skeleton', arm_data)
rig['skeleton'] = 'sbd'
bpy.context.scene.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
human_hand = human.data.bones['hand']
for name, parent in PARENTS.items():
    b = arm_data.edit_bones.new(name)
    if name == 'hand':
        b.head, b.tail = (0, 0, 0), (0, 0.05, 0)
        b.matrix = human.matrix_world @ human_hand.matrix_local
        b.length = human_hand.length
    else:
        b.head, b.tail = JOINTS[name]
        b.align_roll(ROLL_Z.get(name) or Vector((1, 0, 0)).cross(b.tail - b.head).normalized())
    if parent:
        b.parent = arm_data.edit_bones[parent]
bpy.ops.object.mode_set(mode='OBJECT')
for b in rig.data.bones:
    print('SBD bone %-8s head %s tail %s' % (b.name, tuple(round(x * 1000) for x in b.head_local),
                                           tuple(round(x * 1000) for x in b.tail_local)))

# idren2023 2026-11-03: the stock 128 px texture upscaled 4x (Upscayl ultrasharp-4x, re/body/hd/; the game's file untouched)
TEXTURE_HD = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're', 'body', 'hd', TEXTURE + '-ultrasharp-4x.png')
material = bpy.data.materials.get(TEXTURE) or bpy.data.materials.new(TEXTURE)
if not material.node_tree or not any(n.type == 'TEX_IMAGE' for n in material.node_tree.nodes):
    material.use_nodes = True
    img = material.node_tree.nodes.new('ShaderNodeTexImage')
    material.node_tree.nodes.active = img  # (textured views draw the active image node)
    img.image = bpy.data.images.load(TEXTURE_HD if os.path.exists(TEXTURE_HD) else A + TEXTURE + '.tga')
    img.image.alpha_mode = 'NONE'  # its alpha is not see-through in the game (BodyArm draws it opaque)
    material.node_tree.links.new(img.outputs['Color'], material.node_tree.nodes['Principled BSDF'].inputs['Base Color'])


def bm_from(verts, tris, uvs, tri_ids):
    bm = bmesh.new()
    layer = bm.loops.layers.uv.new('UV')
    index = {}
    for ti in tri_ids:
        vs = []
        for i in tris[ti]:
            if i not in index:
                index[i] = bm.verts.new(verts[i])
            vs.append(index[i])
        try:
            f = bm.faces.new(vs)
        except ValueError:
            continue
        for loop, uv in zip(f.loops, uvs[ti]):
            loop[layer].uv = uv
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0002)
    return bm


def close_holes(bm):
    """Every open boundary filled (a cut, the block's open underside), the new faces' uvs from the nearest old corner."""
    layer = bm.loops.layers.uv.active
    old = set(bm.faces)
    edges = [e for e in bm.edges if len(e.link_faces) == 1]
    if not edges:
        return 0
    bmesh.ops.holes_fill(bm, edges=edges, sides=0)
    corners = [l for f in old for l in f.loops]
    new = [f for f in bm.faces if f not in old]
    for f in new:
        for loop in f.loops:
            near = min(corners, key=lambda l: (l.vert.co - loop.vert.co).length)
            loop[layer].uv = near[layer].uv
    bmesh.ops.triangulate(bm, faces=new)
    return len(new)


def cut(bm, co, no, keep_positive):
    geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
    bmesh.ops.bisect_plane(bm, geom=geom, plane_co=co, plane_no=no, clear_inner=keep_positive,
                           clear_outer=not keep_positive, dist=0.00001)


def finish(name, bm, bone, raw=False, bevel=True, grid=False, diag02=False, shading=False):
    if not raw and not grid:
        keep = bm.edges.layers.int.get('cut')  # knife_project's edges stay
        bmesh.ops.dissolve_limit(bm, angle_limit=math.radians(2), verts=[v for v in bm.verts if not keep or not any(
            e[keep] for e in v.link_edges)], edges=[e for e in bm.edges if not keep or not e[keep]], delimit={'UV'})
        closed = close_holes(bm)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        if BEVEL and bevel:
            hard = [e for e in bm.edges if len(e.link_faces) == 2 and e.calc_face_angle(0) > ANGLE]
            bmesh.ops.bevel(bm, geom=hard, offset=BEVEL, segments=1, profile=0.5, affect='EDGES', clamp_overlap=True)
        centroid = sum((v.co for v in bm.verts), Vector()) / len(bm.verts)
        bm.normal_update()
        if sum((f.calc_center_median() - centroid).dot(f.normal) * f.calc_area() for f in bm.faces) < 0:
            bmesh.ops.reverse_faces(bm, faces=bm.faces)
        print('SBD piece %s: %d holes closed' % (name, closed))
    if opt.get('tris') == '1':  # the modelling source keeps its quads; triangulated on export
        bmesh.ops.triangulate(bm, faces=bm.faces, quad_method='FIXED' if diag02 else 'BEAUTY')
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    import zlib
    h = zlib.crc32(name.encode())  # shade=pieces: each piece its own colour
    col = me.color_attributes.new('piece', 'FLOAT_COLOR', 'POINT')
    import colorsys
    rgb = colorsys.hsv_to_rgb((h & 1023) / 1023, 0.75, 0.95)
    for c in col.data:
        c.color = (*rgb, 1)
    if not shading:  # shading: the mesh's own smooth faces and sharp edges kept (idren2023's plate)
        smooth = bone == 'forearm'  # the forearm's pieces shaded smooth, edges over SMOOTH_SHARP sharp (idren2023 2026-11-03)
        for p in me.polygons:
            p.use_smooth = smooth
        if smooth:
            me.set_sharp_from_angle(angle=SMOOTH_SHARP)
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
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
    tris = sum(len(p.vertices) - 2 for p in ob.data.polygons)
    print('SBD part %s: %d triangles, bones %s' % (name, tris, sorted(g.name for g in ob.vertex_groups)))
    return ob


RAW = opt.get('rawmesh') == '1'


def new_bm():
    bm = bmesh.new()
    bm.loops.layers.uv.new('UV')
    return bm


def prism(poly, z):
    """A plate: the outline poly ((x, y) mm, counter-clockwise from above, may be concave) over the z range."""
    bm = new_bm()
    bottom = [bm.verts.new(Vector((x, y, z[0])) / 1000) for x, y in poly]
    top = [bm.verts.new(Vector((x, y, z[1])) / 1000) for x, y in poly]
    bm.faces.new(top)
    bm.faces.new(bottom[::-1])
    for k in range(len(poly)):
        a, b = k, (k + 1) % len(poly)
        bm.faces.new((bottom[a], bottom[b], top[b], top[a]))
    return bm


def tab_edge(y, sign):
    """A hinge seam across the hand, left to right (sign 1) or right to left (-1), at y (mm) with the film's two tabs:
    the plate ahead of the seam reaches back TAB mm into the plate behind it, each tab's sides slanted (wider ahead)."""
    ya, yb = y + TAB / 2, y - TAB / 2
    pts = [(across(0, ya), ya)]
    for f in TAB_AT:
        xa, xb = across(f, ya), across(f, yb)
        pts += [(xa - TAB_BASE, ya), (xb - TAB_TIP, yb), (xb + TAB_TIP, yb), (xa + TAB_BASE, ya)]
    pts.append((across(1, ya), ya))
    return pts if sign > 0 else pts[::-1]


def machined_finger():
    """The block and the finger's two plates, each hinge a straight seam with the film's two tabs (the plate ahead
    holds them), the hinge axis through the seam's middle at mid-thickness; folded into the fist on their bones."""
    obs = []

    def add(name, bm, bone):
        # the outer edges bevelled, the hinge seams' edges (the tabs and the plates' ends at a seam) left crisp, as the
        # film's (idren2023 2026-11-03)
        def at_seam(e):
            return any(all(abs(v.co.y * 1000 - h) < TAB / 2 + 0.1 for v in e.verts) for h in (HINGE_1, HINGE_2))
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        hard = [e for e in bm.edges if len(e.link_faces) == 2 and e.calc_face_angle(0) > ANGLE and not at_seam(e)]
        bmesh.ops.bevel(bm, geom=hard, offset=BEVEL, segments=1, profile=0.5, affect='EDGES', clamp_overlap=True)
        for v in bm.verts:
            v.co = FOLD[bone] @ v.co
        obs.append(finish('sbd_piece_' + name, bm, bone, bevel=False))
    t0, t1, tr = WRIST_TONGUE  # the back end narrowed to a tongue between the wrist pins
    add('block', prism([(t0, 0), (t1, 0), (t1, tr), (across(1, tr), tr)] + tab_edge(HINGE_1, -1) +
                       [(across(0, tr), tr), (t0, tr)], HAND_Z), 'hand')
    add('finger1', prism(tab_edge(HINGE_1, 1) + tab_edge(HINGE_2, -1), FINGER_Z), 'grip_1')
    add('finger2', prism(tab_edge(HINGE_2, 1) + [(across(1, TIP_END), TIP_END), (across(0, TIP_END), TIP_END)],
                         TIP_Z), 'grip_2')
    bpy.ops.object.select_all(action='DESELECT')
    for o in obs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = obs[0]
    bpy.ops.object.join()
    uv_from_stock(obs[0], P['block'] + P['middle'] + P['tip'])
    return [obs[0]]


# The thumb as the Hot Toys figure's (re/body/ref/ht-hands*.png; idren2023 2026-11-03): three solid blocks on the stock
# thumb's path and length, a ball joint on the palm at its base, the tip's long plate lying over the middle block (on its
# outer face) when the thumb is straight (mm).
THUMB_BLOCK = ((18, 11), (16, 10), (16, 10))  # each block's width (along the hinge axis) and thickness
THUMB_PIN = (4.5, 1.5)        # the middle and tip joints' pins: radius, how far they stand out of the blocks
TIP_PLATE = (16, 4, 4)        # the tip's plate: width, thickness, the gap it leaves to the middle joint's pin


def bone_frame(name):
    """A bone's rest axes: y along it, z as set above (ROLL_Z, or the hand's x cross y), x = y cross z."""
    return tuple(v.copy() for v in rig.data.bones[name].matrix_local.to_3x3().transposed())


def local_box(origin, axes, ranges, bm=None):
    """A box over three (lo, hi) mm ranges along the given axes from origin."""
    bm = bm or new_bm()
    for v in bmesh.ops.create_cube(bm, size=2.0)['verts']:
        k = [((r[0] + r[1]) / 2 + (r[1] - r[0]) / 2 * c) / 1000 for r, c in zip(ranges, v.co)]
        v.co = origin + sum((a * kk for a, kk in zip(axes, k)), Vector())
    return bm


def local_pin(origin, axis, r, half, segments=10):
    """A cylinder along axis through origin, r and half its length in mm."""
    bm = new_bm()
    turn = Vector((0, 0, 1)).rotation_difference(axis).to_matrix().to_4x4()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=segments, radius1=r / 1000, radius2=r / 1000,
                          depth=2 * half / 1000, matrix=Matrix.Translation(origin) @ turn)
    return bm


def machined_thumb():
    obs = []
    for k, bone in enumerate(('thumb_1', 'thumb_2', 'thumb_3')):
        head, tail = JOINTS[bone]
        x, y, z = bone_frame(bone)  # z away from the hand: the blocks' outer face
        length = (tail - head).length * 1000
        w, t = THUMB_BLOCK[k]
        obs.append(finish('sbd_piece_%s' % bone, local_box(head, (x, y, z), ((-w / 2, w / 2), (0, length),
                                                                              (-t / 2, t / 2))), bone))
        if k:
            r, out = THUMB_PIN
            obs.append(finish('sbd_piece_%s_pin' % bone, local_pin(head, x, r, w / 2 + out), JOINTS_PARENT[bone]))
        if k == 2:
            pw, pt, gap = TIP_PLATE
            back = (JOINTS['thumb_2'][1] - JOINTS['thumb_2'][0]).length * 1000 - THUMB_PIN[0] - gap
            obs.append(finish('sbd_piece_thumb_plate', local_box(head, (x, y, z), (
                (-pw / 2, pw / 2), (-back, length), (t / 2, t / 2 + pt))), bone))
    head = JOINTS['thumb_1'][0]
    bm = new_bm()
    bmesh.ops.create_uvsphere(bm, u_segments=20, v_segments=10, radius=THUMB_BALL / 1000,
                              matrix=Matrix.Translation(head) @ Matrix.Scale(THUMB_BALL_SQUASH, 4, (0, 0, 1)))
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if v.co.z > head.z + 0.0005], context='VERTS')  # the half in the hand
    ob = finish('sbd_piece_thumb_ball', bm, 'hand', raw=True)
    for p in ob.data.polygons:
        p.use_smooth = True
    obs.append(ob)
    return obs


# The blaster arm (idren2023 2026-11-03: the stock first-person arm has the right shape; the third-person model, the
# Hot Toys figure and our own traces of it did not do better): the stock forearm shell machined like the hand (holes
# closed, hard edges bevelled, flat; its wrist cap dropped, it lay askew over the back of the hand); the wrist blaster's barrels and their housings rebuilt as clean
# cylinders on the stock pieces' own axes and sizes. No elbow of its own (idren2023 2026-11-03): the forearm bone starts
# at the stock shell's elbow end, as the stock arm's did.
WRIST_CUT = -2                 # the shell ends here, short of the hand's hinge
WRIST_PIN = (11, (39, 59))     # the wrist pins: radius (Hot Toys photo h12 through its fitted camera: 17-25 mm across),
                               # |x| range either side of the hand's tongue
# The forearm's height from the side (photo h12 through its fitted camera, idren2023 2026-11-03: ours was too thick, with no
# flat bottom): its top at z = 53 at the wrist falling 8 per 100 mm back, its flat underside at z = 9 falling 6 per 100;
# a capsule along the middle of the underside. (mm; y negative toward the elbow)
FOREARM_TOP = (29, -0.015)    # z at the wrist, change per mm back (y < 0): straight, a little deeper toward the elbow
FOREARM_BOTTOM = (-15, 0.026) # the left arm from the side (idren2023 2026-11-03): one straight plate centred on the wrist
                              # pin at the wrist (about 38 deep), deepest at the elbow (about 45, plus the capsule)
CAPSULE = ((-5, -96), (-5, -160), 21)  # (x, y) at its front and back ends, its radius (idren2023 2026-11-03: bigger): under the base plate on the
                                        # bone line (wrist hinge to the elbow end), its rounded end toward the wrist
CAPSULE_DROP = 18              # how far it hangs below the base plate (idren2023 2026-11-03: taller)
ELBOW_DISC = ((-6, -223), (-5, 13), 40)   # centre (x, y), z bottom and top, radius
ELBOW_PIN = ((-19, 27), 22, 5)            # z of its straight part, radius, chamfer (radius 16 at both ends)
ELBOW_NECK = (-188, -140, 6, -0.5)       # y from the disc into the capsule (embedded in the forearm), radius, z of
                                         # its axis: below the plate's underside (z 6), not through it
MUZZLE_Y = 37                  # both muzzles here (mm ahead of the wrist hinge): the Hot Toys photo h08 through its
                               # fitted camera puts the openings at 32 and 42 on our barrel axes
BARREL_STEPS = ((0.0, 1.0), (0.45, 0.85), (0.78, 0.7), (0.86, 0.85), (0.93, 0.7))  # (share of length, share of radius)
GUN_BACK = {'cylinder2': -120, 'cylinder5': -110}  # y of each barrel's back end, its housing moved with it (idren2023
                               # 2026-11-03, film right arm from above: about 1.25 and 1.0 hand widths behind the muzzles)
GUN_FLASH_AHEAD = 25            # mm: the game's flash point this far ahead of the muzzles (3.5 cm at game scale)
GUN_BRIDGE = (12, 2.5)         # the block joining the two barrels between their housings: height above the lower
                               # barrel axis's bottom.. (mm), how far into each housing (idren2023 2026-11-03)
GUN_SCALE = 0.8                # the guns' radii against the stock pieces' (idren2023 2026-11-03: a bit too big)
STOCK_CYLINDERS = {'cylinder2': 'barrel', 'cylinder5': 'barrel', 'cylinder3': 'housing', 'cylinder4': 'housing'}


def along_y(x, z, y0, y1, r0, r1=None, segments=12, caps=True):
    bm = new_bm()
    place = Matrix.Translation(Vector((x, (y0 + y1) / 2, z)) / 1000) @ Matrix.Rotation(-math.pi / 2, 4, 'X')
    bmesh.ops.create_cone(bm, cap_ends=caps, segments=segments, radius1=r0 / 1000,
                          radius2=(r1 if r1 is not None else r0) / 1000, depth=(y1 - y0) / 1000, matrix=place)
    return bm


def along_x(x0, x1, y, z, r, segments=16, matrix=Matrix.Identity(4)):
    """A cylinder along x (then placed by matrix: about the y axis a quarter turn makes it along z)."""
    bm = new_bm()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=segments, radius1=r / 1000, radius2=r / 1000,
                          depth=(x1 - x0) / 1000, matrix=matrix @ Matrix.Translation(Vector(((x0 + x1) / 2, y, z)) / 1000) @
                          Matrix.Rotation(math.pi / 2, 4, 'Y'))
    return bm


def knife_project(bm, cuts_file, margin=4.0):
    """Edges cut into a mesh along lines traced in a photo, through that photo's fitted camera (tools/data): each line
    segment's two pixel rays span a plane through the camera; the faces whose image lies along the segment (within
    `margin` px) are bisected by it, so the new edges sit on the surface where the line is in the photo."""
    import numpy as np
    data = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
    cuts = json.load(open(os.path.join(data, cuts_file)))
    cam = json.load(open(os.path.join(data, cuts['camera'])))
    R, t, f = np.array(cam['R']), np.array(cam['t']), cam['f']
    W, H = cam['size']
    centre = -R.T @ t

    def image(p):
        q = R @ np.array(p) + t
        return np.array([W / 2, H / 2]) + f * q[:2] / q[2]

    def ray(uv):
        return R.T @ np.array([(uv[0] - W / 2) / f, (uv[1] - H / 2) / f, 1.0])
    made = 0
    for name, line in cuts['lines'].items():
        for a, b in zip(line, line[1:]):
            a, b = np.array(a), np.array(b)
            ab = b - a
            n = np.cross(ray(a), ray(b))
            n /= np.linalg.norm(n)
            faces = []
            for fc in bm.faces:
                s = image(fc.calc_center_median())
                k = np.dot(s - a, ab) / np.dot(ab, ab)
                span = max(np.linalg.norm(image(v.co) - s) for v in fc.verts)
                if -0.1 < k < 1.1 and abs(np.cross(ab, s - a)) / np.linalg.norm(ab) < span + margin:
                    faces.append(fc)
            if not faces:
                continue
            geom = list({e for fc in faces for e in fc.edges}) + faces + list({v for fc in faces for v in fc.verts})
            res = bmesh.ops.bisect_plane(bm, geom=geom, plane_co=Vector(centre), plane_no=Vector(n), dist=0.00002)
            # keep only the part of the cut that lies along the segment
            layer = bm.edges.layers.int.get('cut') or bm.edges.layers.int.new('cut')
            for e in res['geom_cut']:
                if isinstance(e, bmesh.types.BMEdge) and e.is_valid:
                    m = image((e.verts[0].co + e.verts[1].co) / 2)
                    if -0.02 < np.dot(m - a, ab) / np.dot(ab, ab) < 1.02:
                        e[layer] = 1
            made += 1
    print('SBD knife: %d segments cut' % made)
    for name, reg in cuts.get('regions', {}).items():
        poly = np.array([pt for line in reg['outline'] for pt in cuts['lines'][line]])

        def inside(uv):
            x, y = uv
            hit = False
            for (x0, y0), (x1, y1) in zip(poly, np.roll(poly, -1, axis=0)):
                if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
                    hit = not hit
            return hit
        region = [fc for fc in bm.faces if inside(image(fc.calc_center_median())) and fc.normal.z > 0.2]
        if not region:
            continue
        layer = bm.edges.layers.int.get('cut') or bm.edges.layers.int.new('cut')
        inner = [e for e in {e for fc in region for e in fc.edges} if not e[layer]]
        bmesh.ops.subdivide_edges(bm, edges=inner, cuts=1, use_grid_fill=True)
        region = [fc for fc in bm.faces if inside(image(fc.calc_center_median())) and fc.normal.z > 0.2]
        rim = {v for fc in region for e in fc.edges if e[layer] for v in e.verts}
        verts = {v for fc in region for v in fc.verts} - rim
        front = max(v.co.y for v in verts | rim)
        rim_pts = [v.co.copy() for v in rim]
        for v in verts:
            near = min(rim_pts, key=lambda r: (r.xy - v.co.xy).length)
            d_rim = (near.xy - v.co.xy).length
            k = d_rim / max(d_rim + (front - v.co.y), 1e-6)  # 0 on the arc, 1 at the wrist end
            v.co.z = near.z + (reg['to_z'] / 1000 - near.z) * k
        print('SBD knife region %s: %d faces sloped to z %g at the wrist' % (name, len(region), reg['to_z']))


SHELL_WARP = None


def shell_warp(p):
    """A stock shell point moved by tools/data/sbd_shell_warp.json (its edges brought onto the Hot Toys figure's in a
    fitted photo; idren2023's colour-matched edges 2026-11-03), if it is listed there."""
    global SHELL_WARP
    if SHELL_WARP is None:
        moves = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'sbd_shell_warp.json')))
        SHELL_WARP = [(Vector(a), Vector(d)) for a, d in moves['moves']]
    q = p * 1000
    for a, d in SHELL_WARP:
        if (q - a).length < 0.3:
            return (q + d) / 1000
    return p


# Our own forearm shell (idren2023 2026-11-03: "build your own arm" in clean quads): a cross-section swept along the arm.
# Its outline from above is the Hot Toys figure's (photo h08 through its fitted camera), its top and flat underside the
# heights from photo h12 (FOREARM_TOP / _BOTTOM); inside the arc at the wrist end its top slopes down to the wrist
# (idren2023: clear on the left arm). (y, x mm; y negative toward the elbow)
# 2026-11-03: outline, tip, elbow arc and capsule reworked (elbow-wrist bone line along -y, our 289 mm)
SWEEP_THUMB = ((16, -52), (-12, -52), (-42, -55), (-72, -61), (-102, -67), (-129, -69), (-147, -58), (-162, -45),
               (-172, -20), (-179, 5), (-192, 28), (-207, 40), (-222, 50))
SWEEP_FINGER = ((16, 40), (-12, 42), (-42, 46), (-72, 52), (-102, 57), (-132, 59), (-162, 60), (-192, 59), (-212, 58),
                (-227, 57))
SWEEP_TIP = (57, -232)       # the point (x, y) the stations close to
SWEEP_STEP = 15               # mm between stations
# the cross-section: (share across from the thumb edge, height above the flat bottom as a share of the thickness,
# extra mm): flat bottom with chamfered corners, two side faces, a top with rounded edges and a slight crown
SWEEP_SECTION = ((0.0, 0.18, 0), (0.07, 0.0, 0), (0.3, 0.0, 0), (0.5, 0.0, 0), (0.7, 0.0, 0), (0.93, 0.0, 0),
                 (1.0, 0.18, 0), (1.0, 0.8, 0), (0.95, 1.0, 0), (0.75, 1.0, 1.5), (0.5, 1.0, 2.5), (0.25, 1.0, 1.5),
                 (0.05, 1.0, 0), (0.0, 0.8, 0))
SLOPE_ARC = ((-45, 40), (-44, 3), (-43, -33), (-39, -69), (-33, -87), (-29, -92), (-15, -100), (1, -94), (15, -80),
             (17, -72), (21, -54), (21, -24), (18, 12), (16, 40))  # the arc (x, y), traced on h08 at the top (z 45)
SLOPE_TO = None               # no slope at the wrist end (idren2023 2026-11-03: the left arm shows one straight plate)


def ramp(table, y):
    """Piecewise linear in y over (y, value) pairs given from high y to low."""
    for (y0, v0), (y1, v1) in zip(table, table[1:]):
        if y1 <= y <= y0:
            return v0 + (v1 - v0) * (y - y0) / (y1 - y0)
    return table[0][1] if y > table[0][0] else table[-1][1]


def inside_poly(x, y, poly):
    hit = False
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            hit = not hit
    return hit


def arc_back_at(x):
    """How far back (y) the slope's arc reaches at this x (its lowest crossing)."""
    ys = [ya + (yb - ya) * (x - xa) / (xb - xa) for (xa, ya), (xb, yb) in zip(SLOPE_ARC, SLOPE_ARC[1:])
          if xa != xb and min(xa, xb) <= x <= max(xa, xb)]
    return min(ys) if ys else min(p[1] for p in SLOPE_ARC)


# idren2023's description of the forearm (2026-11-03, the Hot Toys figure's left arm, photo h05): a 3 mm base plate with the
# forearm's outline, its flat underside the arm's underside; on it the arm plate, same outline, about 30 thick at its
# centre, thinner toward its edges; on the arm plate's top two very slightly concave smooth scoops: one inside the arc
# that runs from one side of the wrist up the arm and back to the other side, falling to the wrist cylinder, one inside
# the arc from the elbow tip up and back toward the opposite edge, falling to the tip; the wrist cylinder (about 15
# thick, its axis square to the plate, bevelled round its front) on the base plate at the wrist end, the arm plate
# meeting the elbow-side half of its top edge; a capsule under the base plate along the bone line. (mm)
BASE_Z = 3                   # the underside: just above the back of the hand (z 9), the wrist pins below it
BASE_T = 3                    # the base plate's thickness
ARM_T = (20, 30)              # the arm plate's thumb-side height at its front and back, and at the middle of its arch
                              # (idren2023 2026-11-03): the thickest part runs along the thumb side as a smooth arch from
                              # the wrist end to the elbow end; across, the top falls to nothing at the pinky edge
WRIST_CYL = (-5, -15, 33, 9)  # the wrist cylinder: centre x, y, radius, thickness: ahead of the wrist pins, its back
                              # edge on the hinge, the barrels' muzzles at its front (idren2023's drawing on the side photo)
WRIST_ARC = 100               # how far back (from the forearm's wrist end) the wrist arc reaches at its middle
ELBOW_ARC = ((57, -232), (15, -122), (-69, -132))  # the elbow arc: from the tip, up, and back to the thumb-side edge
ELBOW_TAPER = None            # (the pinky side already runs to nothing at the point)
SCOOP_POWER = 1.35
PLATE_FRONT = -8              # y where the plates start (the wrist bar, r 10 at the hinge, is ahead of it)
WRIST_CYL_Z = 11              # the wrist disc's underside: just over the hand's back (z 9)
WRIST_BAR = (10, (-52, 44))   # the hinge bar across the wrist: radius, x range
WRIST_DOME = (-5, 40, 11)     # under the plate behind the bar: centre x, plan radius, depth (centred on the hinge)


EDIT_PLATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're', 'body', 'sbd_plate.blend')
EDIT_PLATE = os.path.exists(EDIT_PLATE_FILE) and opt.get('plate') != 'ours'
# idren2023's plate (object 'plate' in that file, mm/1000, our frame; built by tools/sbd_plate_rebuild.py); plate=ours:
# the generated plate. The base strips under it are cut from its underside's outline:
BASE_STRIPS = ((-999, -30), (10, 999))  # x ranges of the two strips (the gap between them: the capsule)
BASE_STRIP_END = -165                   # y where the strips end toward the elbow
BASE_STRIP_INSET = 3                    # how far inside the plate's underside outline
BASE_STRIP_T = 5                        # thickness, down from the plate's underside (idren2023: thicker than BASE_T)


def edit_plate():
    """idren2023's plate from EDIT_PLATE_FILE, as a bmesh in metres."""
    with bpy.data.libraries.load(EDIT_PLATE_FILE) as (src, dst):
        dst.objects = ['plate']
    ob = dst.objects[0]
    bm = new_bm()
    bm.from_mesh(ob.data)
    bm.transform(ob.matrix_world)
    me = ob.data
    bpy.data.objects.remove(ob)
    bpy.data.meshes.remove(me)
    return bm


def underside_outline(bm):
    """The outline (mm, x y) of a plate's flat underside: the border of its faces lying at its lowest level."""
    zmin = min(v.co.z for v in bm.verts)
    under = {f for f in bm.faces if all(v.co.z < zmin + 0.0005 for v in f.verts)}
    nxt = {}
    for f in under:
        for l in f.loops:
            if not any(g in under for g in l.edge.link_faces if g is not f):
                nxt[l.vert] = l.link_loop_next.vert
    loops = []
    while nxt:
        v0 = next(iter(nxt))
        loop, v = [], v0
        while v in nxt:
            loop.append((v.co.x * 1000, v.co.y * 1000))
            v = nxt.pop(v)
        loops.append(loop)
    return max(loops, key=len), zmin * 1000


def _area(poly):
    return sum(poly[i - 1][0] * poly[i][1] - poly[i][0] * poly[i - 1][1] for i in range(len(poly))) / 2


def inset(poly, d):
    """Each vertex moved d mm inward along its corner's bisector (enough for a gently curved outline)."""
    if _area(poly) < 0:
        poly = poly[::-1]
    out = []
    for i, (x, y) in enumerate(poly):
        (ax, ay), (bx, by) = poly[i - 1], poly[(i + 1) % len(poly)]
        n = []
        for (px, py), (qx, qy) in (((ax, ay), (x, y)), ((x, y), (bx, by))):
            l = math.hypot(qx - px, qy - py) or 1
            n.append((-(qy - py) / l, (qx - px) / l))  # inward normal of a counter-clockwise edge
        nx, ny = n[0][0] + n[1][0], n[0][1] + n[1][1]
        l = math.hypot(nx, ny) or 1
        k = d / max(0.3, (nx * n[0][0] + ny * n[0][1]) / l)
        out.append((x + nx / l * k, y + ny / l * k))
    return out


def clip(poly, axis, value, keep_above):
    """Sutherland-Hodgman: poly cut by the line coordinate[axis] = value."""
    inside = lambda p: (p[axis] >= value) == keep_above
    out = []
    for i, p in enumerate(poly):
        q = poly[i - 1]
        if inside(p) != inside(q):
            t = (value - q[axis]) / (p[axis] - q[axis])
            out.append((q[0] + (p[0] - q[0]) * t, q[1] + (p[1] - q[1]) * t))
        if inside(p):
            out.append(p)
    return out


def base_strips(outline):
    """Our two base strips: the plate's underside outline inset, cut to each strip's x range and to the strips' end,
    BASE_T thick from BASE_Z up to the plate; the caps paired into quads."""
    out = []
    poly = inset(outline, BASE_STRIP_INSET)
    poly = clip(poly, 1, BASE_STRIP_END, True)
    for x0, x1 in BASE_STRIPS:
        q = clip(clip(poly, 0, x0, True), 0, x1, False)
        q = [p for i, p in enumerate(q) if math.dist(p, q[i - 1]) > 0.5]
        bm = new_bm()
        tri = new_bm()
        face = tri.faces.new([tri.verts.new((x, y, 0)) for x, y in q])
        bmesh.ops.triangulate(tri, faces=[face], ngon_method='BEAUTY')
        tri.verts.index_update()
        V, F = quadify([v.co for v in tri.verts], [[v.index for v in f.verts] for f in tri.faces])
        tri.free()
        n = len(V)
        vs = [bm.verts.new(Vector((v.x, v.y, z)) / 1000) for z in (BASE_Z + BASE_T - BASE_STRIP_T, BASE_Z + BASE_T)
              for v in V]
        top = []
        for f in F:
            bm.faces.new([vs[i] for i in reversed(f)])
            top.append(bm.faces.new([vs[n + i] for i in f]))
        bm.verts.index_update()
        for f in top:  # the side walls under the top cap's open edges
            for l in f.loops:
                if l.edge.is_boundary:
                    i, j = l.vert.index - n, l.link_loop_next.vert.index - n
                    bm.faces.new([vs[j], vs[i], vs[n + i], vs[n + j]])
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        out.append(bm)
    return out


def quadify(V, F):
    """Pair the triangles F (indices into V, mm) into quads: a maximum matching on the triangle adjacency graph, the
    flattest and squarest pairs first, then augmenting paths so that no triangle stays single where a pairing exists.
    A triangle still single (an odd count) with an edge on the open border gets a vertex in that edge's middle, which
    makes it a quad. Each quad keeps the two triangles' shared edge as its diagonal (triangulated there on export it
    is the original surface: vertices 0 and 2). Returns (V, faces)."""
    V = [Vector(v) for v in V]
    F = [tuple(f) for f in F if len(set(f)) == 3]
    nrm = [(V[f[1]] - V[f[0]]).cross(V[f[2]] - V[f[0]]).normalized() for f in F]
    edges = {}
    for i, f in enumerate(F):
        for k in range(3):
            edges.setdefault(frozenset((f[k], f[(k + 1) % 3])), []).append(i)
    quad, score, nb = {}, {}, [[] for _ in F]
    for e, fs in edges.items():
        if len(fs) != 2:
            continue
        a, b = fs
        fa = F[a]
        k = next(k for k in range(3) if fa[k] not in e)  # a's corner opposite the shared edge
        q = (fa[(k + 1) % 3], next(v for v in F[b] if v not in e), fa[(k + 2) % 3], fa[k])  # a's winding, diagonal 0-2
        dev = 0.0
        for j in range(4):
            c = V[q[j]]
            ang = math.degrees((V[q[j - 1]] - c).angle(V[q[(j + 1) % 4]] - c, 0.0))
            dev = 999 if ang > 175 else max(dev, abs(ang - 90))  # no concave or degenerate quads
            if dev == 999:
                break
        if dev == 999:
            continue
        key = (min(a, b), max(a, b))
        quad[key], score[key] = q, nrm[a].dot(nrm[b]) - dev / 90
        nb[a].append(b)
        nb[b].append(a)
    mate = [-1] * len(F)
    for a, b in sorted(score, key=lambda k: -score[k]):
        if mate[a] < 0 and mate[b] < 0:
            mate[a], mate[b] = b, a

    def augment(s):  # alternating path from single s to another single (no blossoms: enough at this size)
        prev, stack, seen = {s: None}, [s], {s}
        while stack:
            u = stack.pop()
            for v in sorted(nb[u], key=lambda v: score[(min(u, v), max(u, v))]):
                if v in seen:
                    continue
                seen.add(v)
                if mate[v] < 0:
                    path, x = [v, u], u
                    while prev[x] is not None:
                        x, odd = prev[x][0], prev[x][1]
                        path += [odd, x]
                    for i in range(0, len(path), 2):
                        mate[path[i]], mate[path[i + 1]] = path[i + 1], path[i]
                    return
                w = mate[v]
                if w not in seen:
                    seen.add(w)
                    prev[w] = (u, v)
                    stack.append(w)

    for _ in range(4):
        for s in range(len(F)):
            if mate[s] < 0:
                augment(s)
    def has_open(i):
        f = F[i]
        return any(len(edges[frozenset((f[k], f[(k + 1) % 3]))]) == 1 for k in range(3))

    def to_border(s):  # an odd single: shift pairs along an alternating path until the single one has an open edge
        prev, queue, seen = {s: None}, [s], {s}
        while queue:
            u = queue.pop(0)
            if has_open(u):
                mate[u] = -1
                while prev[u] is not None:
                    x, v = prev[u]
                    mate[v], mate[x] = x, v
                    u = x
                return
            for v in nb[u]:
                w = mate[v]
                if v not in seen and w >= 0 and w not in seen:
                    seen |= {v, w}
                    prev[w] = (u, v)
                    queue.append(w)

    for s in range(len(F)):
        if mate[s] < 0:
            to_border(s)
    assert all(m < 0 or mate[m] == i for i, m in enumerate(mate))
    out = []
    for i, f in enumerate(F):
        if mate[i] >= 0:
            if i < mate[i]:
                out.append(quad[(i, mate[i])])
            continue
        for k in range(3):
            if len(edges[frozenset((f[k], f[(k + 1) % 3]))]) == 1:  # an open edge: split it
                V.append((V[f[k]] + V[f[(k + 1) % 3]]) / 2)
                f = (f[k], len(V) - 1, f[(k + 1) % 3], f[(k + 2) % 3])
                break
        out.append(f)
    return V, out


SWEEP_ACROSS = 17             # points across the top
SWEEP_STEP = 6


def cyl_back(x):
    """Where the arm plate ends at this x: the wrist cylinder's elbow-side half circle."""
    cx, cy, r, t = WRIST_CYL
    d = abs(x - cx)
    return cy - math.sqrt(r * r - d * d) if d < r else cy  # beside it, no further forward than its centre line


def arm_top(x, y, xl, xr):
    """The arm plate's top at (x, y): crowned across, then the two scoops."""
    u = min(1.0, max(0.0, (x - xl) / max(xr - xl, 1e-6)))
    base_top = BASE_Z + BASE_T
    cx, cy, r, t = WRIST_CYL
    v = min(1.0, max(0.0, (cy - y) / (cy - SWEEP_TIP[1])))  # 0 at the wrist end, 1 at the elbow point
    ridge = ARM_T[0] + (ARM_T[1] - ARM_T[0]) * math.sin(math.pi * v)
    z = base_top + ridge * math.cos(u * math.pi / 2)
    cyl_top = base_top + t
    front = cy  # the scoop runs down to the cylinder's top
    # the wrist arc: from the two wrist corners back to WRIST_ARC behind the front, a parabola across the forearm
    xm = (xl + xr) / 2
    arc_y = front - WRIST_ARC * (1 - ((x - xm) / ((xr - xl) / 2)) ** 2)
    if y > arc_y:
        k = (y - arc_y) / max(front - arc_y, 1e-6)  # 0 on the arc, 1 at the wrist end
        z = cyl_top + (z - cyl_top) * (1 - k) ** SCOOP_POWER
    # the elbow arc: a quadratic through ELBOW_ARC; behind it (toward the tip, on the thumb side) the elbow scoop
    (tx, ty), (mx, my), (ex, ey) = ELBOW_ARC
    if y < my:
        # the arc's y at this x on each branch: tip -> apex (little-finger side), apex -> thumb edge
        if x >= mx:
            ay = my + (ty - my) * ((x - mx) / max(tx - mx, 1e-6)) ** 2
        else:
            ay = my + (ey - my) * ((mx - x) / max(mx - ex, 1e-6)) ** 2
        if y < ay:
            k = min(1.0, (ay - y) / max(ay - ty, 1e-6))
            z = base_top + (z - base_top) * (1 - k) ** SCOOP_POWER
    # the elbow end thins to almost nothing at the point (idren2023 2026-11-03: the left arm)
    if ELBOW_TAPER:
        k = max(0.0, min(1.0, (ELBOW_TAPER[0] - y) / (ELBOW_TAPER[0] - SWEEP_TIP[1])))
        z = base_top + (z - base_top) * (1 - (3 * k * k - 2 * k ** 3) * (1 - ELBOW_TAPER[1]))
    return max(z, base_top)


PLATE_GRID = (17, 48)         # the plates' quad grid: points across, rows from the front arc to the point


CYL_BLEND = 40                # over this length behind the cylinder's centre the plates' edges ease in to its width


def plate_edge(y, side):
    """The plates' edge at y (side -1 thumb, +1 little finger): the forearm's outline, easing in to the wrist cylinder
    behind its centre and round its front half circle ahead of it (idren2023: the plate ends in a half circle)."""
    cx, cy, r, t = WRIST_CYL
    base = ramp(SWEEP_THUMB if side < 0 else SWEEP_FINGER, y)
    if y >= cy:
        return cx + side * math.sqrt(max(r * r - (y - cy) ** 2, 0.0))
    k = max(0.0, min(1.0, (y - (cy - CYL_BLEND)) / CYL_BLEND))
    k = 3 * k * k - 2 * k ** 3
    return base + (cx + side * r - base) * k


def cyl_front(x):
    """The base plate's front: the wrist cylinder's hand-side half circle."""
    cx, cy, r, t = WRIST_CYL
    d = min(abs(x - cx), r)
    return cy + math.sqrt(r * r - d * d)


def plate_grid(front_of=None):
    """The plates' plan grid, rows[v][u] = (x, y): across from the thumb edge to the little-finger edge, back from the
    front (the wrist cylinder's elbow-side half circle, or the forearm's wrist end beside it) to the elbow point; every
    row follows the front's curve, so the quads stay clean."""
    nu, nv = PLATE_GRID
    front, (tx, ty) = WRIST_CYL[1], SWEEP_TIP
    front_of = front_of or cyl_back
    rows = []
    for j in range(nv + 1):
        v = j / nv
        row = []
        for i in range(nu):
            u = i / (nu - 1)
            y = front
            for _ in range(6):  # x depends on y, y on x: settle them
                xl, xr = plate_edge(y, -1), plate_edge(y, 1)
                x = xl + (xr - xl) * u
                y0 = front_of(x)
                y = y0 + (ty - y0) * v
            x = x + (tx - x) * v ** 6  # the last rows close in on the point
            row.append((x, y))
        rows.append(row)
    return rows


def plate_solid(rows, z_bot, z_top):
    """A solid on the plan grid: its top z_top(x, y), its bottom z_bot(x, y), walls round its edge, the last row
    welded to the point."""
    bm = new_bm()
    top = [[bm.verts.new(Vector((x, y, z_top(x, y))) / 1000) for x, y in row] for row in rows]
    bot = [[bm.verts.new(Vector((x, y, z_bot(x, y))) / 1000) for x, y in row] for row in rows]
    nv, nu = len(rows) - 1, len(rows[0])
    for j in range(nv):
        for i in range(nu - 1):
            bm.faces.new((top[j][i], top[j][i + 1], top[j + 1][i + 1], top[j + 1][i]))
            bm.faces.new((bot[j][i + 1], bot[j][i], bot[j + 1][i], bot[j + 1][i + 1]))
    edge = [(j, 0) for j in range(nv + 1)] + [(nv, i) for i in range(1, nu)] +            [(j, nu - 1) for j in range(nv - 1, -1, -1)] + [(0, i) for i in range(nu - 2, 0, -1)]
    for (j0, i0), (j1, i1) in zip(edge, edge[1:] + edge[:1]):
        bm.faces.new((bot[j0][i0], bot[j1][i1], top[j1][i1], top[j0][i0]))
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0002)
    bmesh.ops.dissolve_degenerate(bm, edges=bm.edges, dist=0.0002)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return bm


def sweep_shell():
    """The arm plate: on the plan grid, from the base plate's top to arm_top."""
    rows = plate_grid()
    base_top = BASE_Z + BASE_T

    def top(x, y):
        return arm_top(x, y, plate_edge(y, -1), plate_edge(y, 1))
    print('SBD plates: %d x %d grid' % PLATE_GRID)
    return plate_solid(rows, lambda x, y: base_top, top)


def base_plate():
    """The 3 mm plate under the arm plate and the wrist cylinder: the arm plate's outline, ending in the cylinder's
    half circle (idren2023 2026-11-03)."""
    return plate_solid(plate_grid(cyl_front), lambda x, y: BASE_Z, lambda x, y: BASE_Z + BASE_T)


def ccw_poly(poly):
    poly = list(poly)
    area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(poly, poly[1:] + poly[:1]))
    return poly if area > 0 else poly[::-1]


def to_forearm_height(p):
    """A shell point kept between the forearm's top and its flat underside (FOREARM_TOP / _BOTTOM)."""
    q = p * 1000
    top = FOREARM_TOP[0] + FOREARM_TOP[1] * q.y
    bottom = FOREARM_BOTTOM[0] + FOREARM_BOTTOM[1] * q.y
    q.z = min(max(q.z, bottom), top)
    return q / 1000


def cylinder_fit(verts):
    """A stock cylinder piece's axis (its longest spread), its two ends along it and its radius (90th percentile)."""
    import numpy as np
    a = np.array([list(p) for p in verts]) * 1000
    c = a.mean(0)
    axis = Vector(np.linalg.eigh(np.cov((a - c).T))[1][:, 2])
    pr = (a - c) @ np.array(axis)
    radial = np.linalg.norm((a - c) - np.outer(pr, np.array(axis)), axis=1)
    return Vector(c) + axis * pr.min(), Vector(c) + axis * pr.max(), float(np.percentile(radial, 90))


def cylinder_between(p0, p1, r, segments):
    bm = along_y(0, 0, 0, (p1 - p0).length, r, segments=segments)
    bm.transform(Matrix.Translation(p0 / 1000) @ Vector((0, 1, 0)).rotation_difference(p1 - p0).to_matrix().to_4x4())
    return bm


def machined_arm():
    obs = []
    if opt.get('shell') == 'stock':  # the stock shell, warped onto the figure and cut (the earlier route)
        v, t, u = node_mesh('cis_superbattledroid_r_forearm')
        bm = bm_from([to_forearm_height(shell_warp(forearm_place(p))) for p in v], t, u, range(len(t)))
        knife_project(bm, 'sbd_shell_cuts.json')
        cut(bm, Vector((0, WRIST_CUT / 1000, 0)), Vector((0, 1, 0)), False)
        obs.append(finish('sbd_piece_shell', bm, 'forearm'))
    else:
        if EDIT_PLATE:
            bm = edit_plate()
            global PLATE_TREE
            from mathutils.bvhtree import BVHTree
            PLATE_TREE = BVHTree.FromBMesh(bm)  # (the guns are set onto it)
            outline, zmin = underside_outline(bm)
            print('edit plate: underside z %.1f, outline %d points' % (zmin, len(outline)))
            obs.append(finish('sbd_piece_shell', bm, 'forearm', grid=True, shading=True))
            for side, strip in zip(('thumb', 'pinky'), base_strips(outline)):
                obs.append(finish('sbd_piece_base_' + side, strip, 'forearm', grid=True))
        else:
            obs.append(finish('sbd_piece_shell', sweep_shell(), 'forearm', bevel=False, grid=True))
            obs.append(finish('sbd_piece_base_plate', base_plate(), 'forearm', grid=True))
        cx, cy, r, t = WRIST_CYL
        z0 = WRIST_CYL_Z
        obs.append(finish('sbd_piece_wrist_cylinder', cylinder_between(
            Vector((cx, cy, z0)), Vector((cx, cy, z0 + t)), r, 28), 'forearm'))
        if True:  # the hinge bar across the wrist and the dome under it
            br, (bx0, bx1) = WRIST_BAR
            obs.append(finish('sbd_piece_wrist_bar', along_x(bx0, bx1, 0, 0, br, segments=20), 'forearm'))
            dx, dr, dd = WRIST_DOME
            dome = new_bm()
            bmesh.ops.create_uvsphere(dome, u_segments=24, v_segments=12, radius=1.0,
                                      matrix=Matrix.Translation(Vector((dx, 0, BASE_Z)) / 1000) @
                                      Matrix.Diagonal((dr / 1000, dr / 1000, dd / 1000, 1)))
            cut(dome, Vector((0, 0, BASE_Z / 1000)), Vector((0, 0, 1)), False)  # the lower half
            cut(dome, Vector((0, 0, 0)), Vector((0, 1, 0)), False)  # behind the hinge
            obs.append(finish('sbd_piece_wrist_dome', dome, 'forearm', raw=True))
    fits = {}
    for name, kind in STOCK_CYLINDERS.items():
        back, front, r = cylinder_fit([forearm_place(p) for p in node_mesh(name)[0]])
        if front.y < back.y:
            back, front = front, back
        fits[name] = (kind, back, front, r * GUN_SCALE)
    # the barrels exactly along the arm (y) and so parallel, each through its stock piece's middle; each housing on
    # its barrel's axis (idren2023 2026-11-03: the fitted axes were a few degrees apart)
    barrels = {n: (b + f) / 2 for n, (k, b, f, r) in fits.items() if k == 'barrel'}
    # the housings sit on the plate's crown (the smooth left-arm plate): the guns lifted together, still parallel
    barrel_lift = 0.0
    lift = {}  # each barrel's own lift (idren2023's plate): its axis on the plate's top, the lower half welded in
    if EDIT_PLATE:
        for name, m in barrels.items():
            back, front = fits[name][1], fits[name][2]
            tops = []
            for y in range(round(GUN_BACK[name]) - 5, round(min(front.y, MUZZLE_Y)) + 1, 2):  # (with the housing)
                hit = PLATE_TREE.ray_cast(Vector((m.x, y, 200)) / 1000, Vector((0, 0, -1)))
                if hit[0] is not None:
                    tops.append(hit[0].z * 1000)
            lift[name] = max(tops) - m.z if tops else 0.0
            print('SBD %s set onto the plate: lift %.1f mm (plate top %.1f..%.1f under it)' % (name, lift[name], min(tops), max(tops)))
    global GUN_FITS, GUN_PLACED
    shift, placed = {}, {}
    GUN_FITS, GUN_PLACED = fits, placed
    for name, (kind, back, front, r) in fits.items():
        mid = (back + front) / 2
        own = name
        if kind == 'housing':
            own = min(barrels, key=lambda n: Vector((barrels[n].x - mid.x, barrels[n].z - mid.z)).length)
            mid = barrels[own].copy()
            mid.y = (back.y + front.y) / 2
        half = Vector((0, (front.y - back.y) / 2, 0))
        back, front = mid - half, mid + half
        dz = lift.get(own, barrel_lift)
        back.z += dz; front.z += dz
        if kind == 'barrel':
            front.y = MUZZLE_Y  # the muzzles side by side (idren2023: one barrel is longer, the openings are level)
            shift[name] = GUN_BACK[name] - back.y
            back.y = GUN_BACK[name]
        else:
            back.y += shift[own]; front.y += shift[own]
        print('SBD stock %s (%s): %s -> %s r %.1f' % (name, kind, tuple(round(x) for x in back),
                                                     tuple(round(x) for x in front), r))
        placed[name] = (kind, back, front, r)
        if kind == 'barrel':
            steps = BARREL_STEPS + ((1.0, None),)
            for (f0, share), (f1, _) in zip(steps, steps[1:]):
                obs.append(finish('sbd_piece_%s_%d' % (name, round(f0 * 100)), cylinder_between(
                    back.lerp(front, f0), back.lerp(front, f1), r * share, 10), 'forearm', bevel=False))
        else:
            obs.append(finish('sbd_piece_' + name, cylinder_between(back, front, r, 12), 'forearm'))
    # a block between the two barrels, down between their housings, joining them: across from one housing's inner
    # side to the other's, along where both housings overlap, from the plate up to just over the barrel axes
    hs = sorted((v for v in placed.values() if v[0] == 'housing'), key=lambda v: v[1].x)
    if len(hs) == 2:
        (_, b0, f0, r0), (_, b1, f1, r1) = hs
        x0, x1 = b0.x + r0 - GUN_BRIDGE[1], b1.x - r1 + GUN_BRIDGE[1]
        y0, y1 = max(b0.y, b1.y), min(f0.y, f1.y)
        z1 = max(b0.z, b1.z) + GUN_BRIDGE[0] / 2
        z0 = min(b0.z, b1.z) - GUN_BRIDGE[0]
        blk = new_bm()
        bmesh.ops.create_cube(blk, size=1.0, matrix=Matrix.Translation(Vector(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)) / 1000)
                              @ Matrix.Diagonal(((x1 - x0) / 1000, (y1 - y0) / 1000, (z1 - z0) / 1000, 1)))
        print('SBD gun bridge: x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f' % (x0, x1, y0, y1, z0, z1))
        obs.append(finish('sbd_piece_gun_bridge', blk, 'forearm'))
    if True:  # the capsule under the plate
        (xa, ya), (xb, yb), r = CAPSULE
        za = zb = BASE_Z - CAPSULE_DROP + r  # CAPSULE_DROP below the base plate
        front, back = Vector((xa, ya, za)), Vector((xb, yb, zb))
        cap = cylinder_between(back, front, r, 12)
        for end in (front,):  # the elbow end flat (idren2023 2026-11-03), the wrist end rounded
            bmesh.ops.create_uvsphere(cap, u_segments=12, v_segments=6, radius=r / 1000,
                                      matrix=Matrix.Translation(end / 1000))
        cut(cap, Vector((0, 0, (BASE_Z + BASE_T / 2) / 1000)), Vector((0, 0, 1)), False)  # only below: hidden from above
        ob = finish('sbd_piece_capsule', cap, 'forearm', raw=True)
        obs.append(ob)
    # the elbow joint (idren2023 2026-11-03: kept so the arm can be attached to the body later): a disc
    # behind the plate's end edge, its axis square to the plate, a pin through it chamfered at both ends, a short neck
    # to the plate
    (ex, ey), (dz0, dz1), dr = ELBOW_DISC
    obs.append(finish('sbd_piece_elbowdisc', cylinder_between(Vector((ex, ey, dz0)), Vector((ex, ey, dz1)), dr, 28),
                      'forearm'))
    (pz0, pz1), pr, ch = ELBOW_PIN
    pin = new_bm()
    for z0, z1, r0, r1 in ((pz0 - ch, pz0, pr - ch, pr), (pz0, pz1, pr, pr), (pz1, pz1 + ch, pr, pr - ch)):
        bmesh.ops.create_cone(pin, cap_ends=True, segments=24, radius1=r0 / 1000, radius2=r1 / 1000,
                              depth=(z1 - z0) / 1000, matrix=Matrix.Translation(Vector((ex, ey, (z0 + z1) / 2)) / 1000))
    obs.append(finish('sbd_piece_elbowpin', pin, 'forearm', bevel=False))
    nz = ELBOW_NECK[3]
    obs.append(finish('sbd_piece_elbowneck', cylinder_between(Vector((ex, ELBOW_NECK[0], nz)),
                                                              Vector((ex, ELBOW_NECK[1], nz)), ELBOW_NECK[2], 12),
                      'forearm'))
    texture_arm(obs)
    return obs


def uv_transfer(ob, src_v, src_t, src_uv, warp=None):
    """UVs for a machined piece from a stock mesh (our frame): each corner takes the uv of the nearest point on the
    stock surface; warp (optional) moves the piece's points onto the stock piece first (a barrel onto its stock
    cylinder), the piece itself stays where it is."""
    src_bm = bm_from(src_v, src_t, src_uv, list(range(len(src_t))))
    me = bpy.data.meshes.new('sbd_uvsrc')
    src_bm.to_mesh(me)
    src_bm.free()
    src = bpy.data.objects.new('sbd_uvsrc', me)
    bpy.context.scene.collection.objects.link(src)
    keep = [v.co.copy() for v in ob.data.vertices]
    if warp:
        for v in ob.data.vertices:
            v.co = warp(v.co * 1000) / 1000
    if not ob.data.uv_layers:
        ob.data.uv_layers.new(name='UV')
    mod = ob.modifiers.new('uv', 'DATA_TRANSFER')
    mod.object = src
    mod.use_loop_data = True
    mod.data_types_loops = {'UV'}
    mod.loop_mapping = 'POLYINTERP_NEAREST'
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.modifier_apply(modifier='uv')
    for v, co in zip(ob.data.vertices, keep):
        v.co = co
    bpy.data.objects.remove(src, do_unlink=True)


# The stock texture's islands (uv; cis_1st_sbdroid_hands is 128 px): the barrel strip (u along, v around), the small
# round cap (the barrels' ends), the big round cap (the hand's end cap), a plain plate (the hand's back)
TEX_STRIP = ((0.01, 0.71), (0.79, 0.98))
TEX_MECH = ((0.50, 0.70), (0.79, 0.98))  # the strip's ringed part: machinery (the block between the guns)
TEX_ROUND = (0.80, 0.885)  # across the strip: its painted light band to its dark band (a lit tube's shading)
TEX_CAP = ((0.48, 0.078), 0.055)
TEX_DISC = ((0.75, 0.115), (0.1, 0.1))
TEX_PLAIN = ((0.08, 0.38), (0.55, 0.75))
TEX_PLATE = ((0.452, 0.995), (0.005, 0.998))  # the plate island's room (its painted part and the black round it)
TEX_PIPE = ((0.01, 0.48), (0.79, 0.98))  # the strip's plain pipe (left of its rings): the capsule and elbow pipe
TEX_HAND = ((0.015, 0.43), (0.316, 0.707), (0.015, 0.316))  # u; v of the hand's back plate; v of its finger plate
TUBES = ('cylinder', 'wrist_bar', 'elbowneck', 'capsule', 'wristpin')
DISCS = ('elbowdisc', 'elbowpin', 'wrist_cylinder', 'wrist_dome')


def _plate_uv_spline():
    import numpy as np
    d = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'sbd_plate_uvmap.json')))
    P = np.array([p[:2] for p in d['pairs']], float)
    T = np.array([p[2:] for p in d['pairs']], float)
    n = len(P)

    def U(r):
        return np.where(r > 0, r * r * np.log(np.maximum(r, 1e-9)), 0.0)
    K = U(np.linalg.norm(P[:, None] - P[None], axis=2))
    A = np.zeros((n + 3, n + 3))
    A[:n, :n] = K
    A[:n, n] = A[n, :n] = 1
    A[:n, n + 1:] = P
    A[n + 1:, :n] = P.T
    W = np.linalg.solve(A, np.vstack((T, np.zeros((3, 2)))))

    def f(x, y):
        k = U(np.linalg.norm(P - (x, y), axis=1))
        return tuple(k @ W[:n] + W[n] + x * W[n + 1] + y * W[n + 2])
    return f


def plate_uv(x, y):
    global _PLATE_UV
    if '_PLATE_UV' not in globals():
        _PLATE_UV = _plate_uv_spline()
    return _PLATE_UV(x, y)


def project_uniform(P, faces_uv, region, region_top=None):
    """Each face projected along its own main axis, one scale (texture per mm) for the whole mesh, centred in its
    region; faces looking up (n.z > 0) into region_top when given. No stretch: the scale is the same both ways."""
    lo = Vector([min(p[k] for p in P) for k in range(3)])
    size = Vector([max(p[k] for p in P) for k in range(3)]) - lo
    regs = [r for r in (region, region_top) if r]
    sc = min(min((r[0][1] - r[0][0]) / (size[a] or 1), (r[1][1] - r[1][0]) / (size[b] or 1))
             for r in regs for a, b in ((0, 1), (1, 2), (0, 2)))
    for normal, loops in faces_uv:
        k = max(range(3), key=lambda k: abs(normal[k]))
        a, b = {0: (1, 2), 1: (0, 2), 2: (0, 1)}[k]
        (u0, u1), (v0, v1) = region_top if region_top and k == 2 and normal.z > 0 else region
        cu = (u0 + u1) / 2 - size[a] * sc / 2
        cv = (v0 + v1) / 2 - size[b] * sc / 2
        for vi, set_uv in loops:
            q = P[vi] - lo
            set_uv((cu + q[a] * sc, cv + q[b] * sc))


def _uv_setter(store, key):
    def f(x):
        store[key].uv = x
    return f


def texture_hand(ob):
    """The hand on the texture's hand plates (idren2023 2026-11-03): projected straight onto them at the stock fist's
    rest shape, one scale both ways; the back of the hand (faces looking up) onto the back plate, every other face
    onto the finger plate; fingers toward the plates' top."""
    me = ob.data
    P = [v.co * 1000 for v in me.vertices]
    (u0, u1), (b0, b1), (f0, f1) = TEX_HAND
    uv = me.uv_layers.active.data
    project_uniform(P, [(p.normal, [(me.loops[li].vertex_index, _uv_setter(uv, li)) for li in p.loop_indices])
                        for p in me.polygons], ((u0, u1), (f0, f1)), ((u0, u1), (b0, b1)))


def _loop_setter(l, uvl):
    def f(x):
        l[uvl].uv = x
    return f


def unwrap_plate(ob):
    """The plate's texture with the least warping (idren2023 2026-11-03): its top (faces looking up) flattened by
    Blender's conformal unwrap (least angle distortion) with idren2023's drawn map pinned (tools/data/sbd_plate_uvmap.json:
    the plate vertex nearest each of his points held where his spline puts it); its side walls unrolled along the
    outline (along: the outline's length, across: height) onto the strip's plain pipe, whose streaks run along; its
    flat underside projected onto the plain plate."""
    me = ob.data
    bm = bmesh.new()
    bm.from_mesh(me)
    uvl = bm.loops.layers.uv.active
    top = {f for f in bm.faces if f.normal.z > 0.3}
    under = {f for f in bm.faces if f.normal.z < -0.7}
    walls = set(bm.faces) - top - under
    for f in top:  # the spline first (the unwrap's start and the pins' places)
        for l in f.loops:
            l[uvl].uv = plate_uv(l.vert.co.x * 1000, l.vert.co.y * 1000)
    d = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'sbd_plate_uvmap.json')))
    tv = {v for f in top for v in f.verts}
    pinned = set()
    outer = {9, 10, 11, 12, 13, 23, 24}  # his blue outline's points: free (they would squeeze the flanks; pin_outline=1 pins)
    for k, (x, y, u, v) in enumerate(d['pairs']):
        if k in outer and opt.get('pin_outline') != '1':
            continue
        pinned.add(min(tv, key=lambda q: (q.co.x * 1000 - x) ** 2 + (q.co.y * 1000 - y) ** 2))
    for f in top:
        for l in f.loops:
            l[uvl].pin_uv = l.vert in pinned
    for e in bm.edges:
        e.seam = len({f in top for f in e.link_faces}) > 1
    outline, _ = underside_outline(bm)
    seg = [0.0]
    for k in range(1, len(outline) + 1):
        seg.append(seg[-1] + math.dist(outline[k - 1], outline[k % len(outline)]))
    total = seg[-1]

    def along(x, y):
        best, at = 1e9, 0.0
        for k in range(len(outline)):
            a, b = Vector(outline[k]), Vector(outline[(k + 1) % len(outline)])
            ab = b - a
            t = max(0.0, min(1.0, (Vector((x, y)) - a).dot(ab) / (ab.length_squared or 1)))
            dd = (a + ab * t - Vector((x, y))).length
            if dd < best:
                best, at = dd, seg[k] + ab.length * t
        return at
    (u0, u1), (v0, v1) = TEX_PIPE
    zs = [v.co.z * 1000 for v in bm.verts]
    z0, z1 = min(zs), max(zs)
    dens = (u1 - u0) / total  # the same texture per mm across as along: a thin band through the pipe's middle
    vm = (v0 + v1) / 2 - (z1 - z0) * dens / 2
    for f in walls:
        a = [along(l.vert.co.x * 1000, l.vert.co.y * 1000) / total for l in f.loops]
        if max(a) - min(a) > 0.5:
            a = [x + 1 if x < 0.5 else x for x in a]
        for l, x in zip(f.loops, a):
            l[uvl].uv = (u0 + min(x, 1.0) * (u1 - u0), vm + (l.vert.co.z * 1000 - z0) * dens)
    bm.verts.index_update()
    P = [v.co * 1000 for v in bm.verts]
    project_uniform(P, [(f.normal, [(l.vert.index, _loop_setter(l, uvl)) for l in f.loops]) for f in under], TEX_PLAIN)
    groups = {'top': [f.index for f in top], 'walls': [f.index for f in walls], 'underside': [f.index for f in under]}
    for f in bm.faces:
        f.select = f in top
    bm.to_mesh(me)
    bm.free()
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_mode(type='FACE')
    ebm = bmesh.from_edit_mesh(me)  # only the top's faces (a face whose corners all touch the top is not one of them)
    ebm.select_mode = {'FACE'}
    ebm.faces.ensure_lookup_table()
    for f in ebm.faces:
        f.select_set(False)
    for i in groups['top']:
        ebm.faces[i].select_set(True)
    bmesh.update_edit_mesh(me)
    if opt.get('plate_unwrap') != '0':  # (plate_unwrap=0: his spline only, to compare)
        bpy.ops.uv.unwrap(method=opt.get('plate_method', 'CONFORMAL'), margin=0.0)
    bpy.ops.object.mode_set(mode='OBJECT')
    if opt.get('pin_outline') != '1':  # the flanks opened out: the whole top scaled evenly about his inner points' middle
        uv = me.uv_layers.active.data   # until it fits the plate island (an even scale adds no stretch)
        loops = [li for i in groups['top'] for li in me.polygons[i].loop_indices]
        pins = [Vector(uv[li].uv) for li in loops if uv[li].pin_uv]
        c = sum(pins, Vector((0, 0))) / len(pins)
        (iu0, iu1), (iv0, iv1) = TEX_PLATE
        k = 1.0
        for li in loops:
            d = Vector(uv[li].uv) - c
            for lim, comp in ((iu1 - c.x, d.x), (iu0 - c.x, d.x), (iv1 - c.y, d.y), (iv0 - c.y, d.y)):
                if comp * lim > 0 and abs(comp) > abs(lim):
                    k = min(k, lim / comp)
        for li in loops:
            uv[li].uv = c + (Vector(uv[li].uv) - c) * k
        print('SBD plate top scaled %.3f to fit the island' % k)
    for g, ids in groups.items():
        uv_report('shell ' + g, ob, ids)
    print('SBD plate: top %d faces unwrapped with %d pins, walls %d, underside %d' % (len(top), len(pinned),
                                                                                    len(walls), len(under)))


def uv_distortion(ob):
    """Per face: how unevenly the texture is stretched (the larger over the smaller of the two stretches of its
    first triangle's mapping, 1 = none); a corner colour 'distort' (green 1 .. red 2+)."""
    me = ob.data
    uv = me.uv_layers.active.data
    aniso = []
    for p in me.polygons:
        li = list(p.loop_indices)
        a, b, c = (me.vertices[me.loops[k].vertex_index].co * 1000 for k in li[:3])
        e1, e2 = b - a, c - a
        x = e1.normalized()
        y = (e2 - x * e2.dot(x)).normalized()
        A = ((e1.dot(x), e2.dot(x)), (e1.dot(y), e2.dot(y)))
        ua, ub, uc = (Vector(uv[k].uv) for k in li[:3])
        t1, t2 = ub - ua, uc - ua
        det = A[0][0] * A[1][1] - A[0][1] * A[1][0]
        if abs(det) < 1e-9:
            aniso.append(1.0)
            continue
        inv = ((A[1][1] / det, -A[0][1] / det), (-A[1][0] / det, A[0][0] / det))
        J = [[t1[r] * inv[0][cc] + t2[r] * inv[1][cc] for cc in range(2)] for r in range(2)]
        aa = J[0][0] ** 2 + J[1][0] ** 2
        bb = J[0][0] * J[0][1] + J[1][0] * J[1][1]
        dd = J[0][1] ** 2 + J[1][1] ** 2
        tr, de = aa + dd, aa * dd - bb * bb
        disc = math.sqrt(max(tr * tr / 4 - de, 0))
        s1, s2 = math.sqrt(max(tr / 2 + disc, 0)), math.sqrt(max(tr / 2 - disc, 0))
        aniso.append(s1 / s2 if s2 > 1e-9 else 99.0)
    col = me.color_attributes.get('distort') or me.color_attributes.new('distort', 'FLOAT_COLOR', 'CORNER')
    for p, an in zip(me.polygons, aniso):
        t = max(0.0, min(1.0, an - 1))
        for li in p.loop_indices:
            col.data[li].color = (t, 1 - t, 0.1, 1)
    return aniso


def uv_report(name, ob, ids=None):
    """The stretch over a piece's surface (by area: half of it under the median, a tenth over 'worst 10%')."""
    an = uv_distortion(ob)
    ps = ob.data.polygons
    rows = sorted((an[i], ps[i].area) for i in (ids if ids is not None else range(len(ps))))
    tot = sum(a for _, a in rows) or 1

    def at(q):
        acc = 0
        for v, a in rows:
            acc += a
            if acc >= q * tot:
                return v
        return rows[-1][0]
    print('SBD uv %-28s stretch median %.2f worst 10%% %.2f' % (name, at(0.5), at(0.9)))


def texture_arm(obs):
    """The forearm's texture (idren2023 2026-11-03: each piece on its own): every piece unwrapped onto the stock
    texture's island for its kind: round pieces around and along onto the barrel strip, their flat ends onto a round
    cap, discs' faces onto the big round cap, the plate projected from above by the stock forearm's own top mapping
    (an affine fit of its uvs to its x y), blocks onto the plain plate."""
    import numpy as np
    sv, st, su = node_mesh('cis_superbattledroid_r_forearm')
    sv0 = sv
    sv = [forearm_place(v) * 1000 for v in sv]
    rows, uvs = [], []
    for tri, tuv in zip(st, su):
        a, b, c = (sv[k] for k in tri)
        if (b - a).cross(c - a).normalized().z > 0.5:
            for k, uv in zip(tri, tuv):
                rows.append((sv[k].x, sv[k].y, 1))
                uvs.append(uv)
    M, res = np.linalg.lstsq(np.array(rows), np.array(uvs), rcond=None)[:2]
    print('SBD plate texture: affine fit to %d stock corners, rms %.3f uv' % (len(rows), math.sqrt(res.sum() / len(rows))))
    for ob in obs:
        name = ob.name[len('sbd_piece_'):]
        bm = bmesh.new()
        bm.from_mesh(ob.data)
        uvl = bm.loops.layers.uv.active or bm.loops.layers.uv.new('UV')
        P = [v.co * 1000 for v in bm.verts]
        c0 = sum(P, Vector()) / len(P)
        cov = np.cov(np.array([tuple(p - c0) for p in P]).T)
        w, vec = np.linalg.eigh(cov)
        if name == 'shell':
            bm.free()
            unwrap_plate(ob)
            continue
        elif any(name.startswith(k) for k in TUBES + DISCS):
            disc = any(name.startswith(k) for k in DISCS)
            axis = Vector(vec[:, 0] if disc else vec[:, 2]).normalized()
            gun = next((n for n in STOCK_CYLINDERS if name == n or name.startswith(n + '_')), None)
            if gun:  # a barrel's segments share one mapping along the whole barrel
                _, b, f_, r = GUN_PLACED[gun]
                axis, base, length = (f_ - b).normalized(), b, (f_ - b).length
            else:
                ts = [(p - c0).dot(axis) for p in P]
                base, length = c0 + axis * min(ts), max(ts) - min(ts) or 1
            ref = Vector((0, 0, -1 if name.startswith('capsule') else 1))  # the side the painted light faces: up
            side = (ref - axis * ref.dot(axis))                              # (the capsule, seen from below: down)
            side = side.normalized() if side.length > 0.1 else axis.orthogonal().normalized()
            side2 = axis.cross(side)
            rad = max(((p - base) - axis * (p - base).dot(axis)).length for p in P) or 1
            housing = gun and STOCK_CYLINDERS[gun] == 'housing'
            (u0, u1), _ = TEX_PIPE if housing or not gun else TEX_STRIP  # the rings only at a barrel's muzzle
            v0, v1 = TEX_ROUND
            dome_end = dome_sign = None
            if name == 'capsule':  # which end is round: there the points come in toward the axis
                ts = [(p - base).dot(axis) for p in P]
                near = [((p - base) - axis * t).length for p, t in zip(P, ts)]
                lo_r = min(r for r, t in zip(near, ts) if t < min(ts) + 1)
                hi_r = min(r for r, t in zip(near, ts) if t > max(ts) - 1)
                dome_sign = 1 if hi_r < lo_r else -1
                dome_end = (max(ts) - rad) if dome_sign > 0 else (min(ts) + rad)
            for f in bm.faces:
                if abs(f.normal.dot(axis)) > 0.7:  # a flat end
                    (cu, cv), r = (TEX_DISC[0], TEX_DISC[1]) if disc else (TEX_CAP[0], (TEX_CAP[1],) * 2)
                    for l in f.loops:
                        d = l.vert.co * 1000 - base
                        l[uvl].uv = (cu + d.dot(side) / rad * r[0], cv + d.dot(side2) / rad * r[1])
                else:  # around: the strip's painted light (TEX_ROUND) on the lit side to its dark on the other, both
                    # halves alike (no seam); along: the same texture per mm
                    dens = (v1 - v0) / (math.pi * rad)
                    ang = []
                    for l in f.loops:
                        d = l.vert.co * 1000 - base
                        a = math.atan2(d.dot(side2), d.dot(side)) / (2 * math.pi) % 1
                        ang.append(2 * min(a, 1 - a))
                    for l, a in zip(f.loops, ang):
                        mm = (l.vert.co * 1000 - base).dot(axis)
                        if dome_end is not None:  # the capsule's round end: along its curve, not along the axis
                            past = (mm - dome_end) if dome_sign > 0 else (dome_end - mm)
                            if past > 0:
                                past = rad * math.asin(min(1.0, past / rad))
                                mm = dome_end + past if dome_sign > 0 else dome_end - past
                        u = (u1 - (length - mm) * dens) if gun and not housing else (u0 + mm * dens)  # muzzle: rings
                        l[uvl].uv = (min(max(u, u0), u1), v0 + a * (v1 - v0))
        else:  # a block: each face projected along its own main axis onto the plain plate, one scale both ways
            bm.verts.index_update()
            project_uniform(P, [(f.normal, [(l.vert.index, _loop_setter(l, uvl)) for l in f.loops]) for f in bm.faces],
                            TEX_MECH if name == 'gun_bridge' else TEX_PLAIN)
        bm.to_mesh(ob.data)
        bm.free()


def uv_from_stock(ob, tri_ids):
    """The stock pieces' texture carried onto a machined part: each new corner takes the uv of the nearest point on
    the stock surface (Blender's data transfer, nearest face interpolated)."""
    src_bm = bm_from(hand_v, hand_t, hand_uv, tri_ids)
    me = bpy.data.meshes.new('sbd_uvsrc')
    src_bm.to_mesh(me)
    src_bm.free()
    src = bpy.data.objects.new('sbd_uvsrc', me)
    bpy.context.scene.collection.objects.link(src)
    if not ob.data.uv_layers:
        ob.data.uv_layers.new(name='UV')
    mod = ob.modifiers.new('uv', 'DATA_TRANSFER')
    mod.object = src
    mod.use_loop_data = True
    mod.data_types_loops = {'UV'}
    mod.loop_mapping = 'POLYINTERP_NEAREST'
    bpy.ops.object.select_all(action='DESELECT')
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.modifier_apply(modifier='uv')
    bpy.data.objects.remove(src, do_unlink=True)


if STOCK:
    hand_obs = [finish('sbd_piece_block', bm_from(hand_v, hand_t, hand_uv, P['block']), 'hand', raw=RAW)]
    finger_parts = (('middle', 'grip_1'), ('tip', 'grip_2'))
else:
    hand_obs = machined_finger()
    finger_parts = ()
thumb_parts = (('thumb_1', 'thumb_1'), ('thumb_2', 'thumb_2'))
if not STOCK:
    thumbs = machined_thumb()
    bpy.ops.object.select_all(action='DESELECT')
    for o in thumbs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = thumbs[0]
    bpy.ops.object.join()
    uv_from_stock(thumbs[0], P['thumb_1'] + P['thumb_2'])
    hand_obs.append(thumbs[0])
    thumb_parts = ()
for key, bone in finger_parts + thumb_parts:
    hand_obs.append(finish('sbd_piece_' + key, bm_from(hand_v, hand_t, hand_uv, P[key]), bone, raw=RAW))
if STOCK:
    bv, bt, bu = node_mesh('sphere1')
    hand_obs.append(finish('sbd_piece_ball', bm_from(bv, bt, bu, list(range(len(bt)))), 'hand', raw=True))
hand = part('sbd_hand', hand_obs)
if not STOCK:
    texture_hand(hand)
    uv_report('hand', hand)
if STOCK:
    arm = part('sbd_arm', [finish('sbd_piece_arm%d' % k, bm_from(v, t, u, list(range(len(t)))), 'forearm', raw=True)
                           for k, (v, t, u) in enumerate(arm_parts)])
else:
    arm_obs = machined_arm()
    # the left arm (idren2023 2026-11-03): the right one without its guns, mirrored. In the game the left arm is the right
    # arm's model drawn mirrored, so its data stay unmirrored here (export these); the mirror is the left skeleton's
    # (sbd_skeleton_l: the same bones, scaled -1 in x, set LEFT_GAP beside the right one for viewing)
    left_obs = []
    for o in arm_obs + [hand]:
        if o.name.startswith('sbd_piece_cylinder') or o.name == 'sbd_piece_gun_bridge':
            continue
        c = o.copy()
        c.data = o.data.copy()
        c.modifiers.clear()
        bpy.context.scene.collection.objects.link(c)
        left_obs.append(c)
    for o in arm_obs:
        uv_report(o.name[10:], o)
    arm = part('sbd_arm', arm_obs)
    rig_l = rig.copy()
    rig_l.name = 'sbd_skeleton_l'
    bpy.context.scene.collection.objects.link(rig_l)
    rig_l.scale = (-1, 1, 1)
    rig_l.location = (LEFT_GAP / 1000, 0, 0)
    arm_l = part('sbd_arm_l', left_obs)
    arm_l.parent = rig_l
    arm_l.matrix_parent_inverse = Matrix.Identity(4)
    arm_l.matrix_basis = Matrix.Identity(4)
    arm_l.modifiers['rig'].object = rig_l
    if opt.get('export') == '1':
        # for tools/body_export.py (the master blend re/body/r_arm_grips.blend): the left arm a part on the one sbd
        # skeleton (the game mirrors it: BodyArm draws the _l parts for the left hand), the viewing skeleton gone;
        # the wrist blaster's grip (weap_sbd_wristblaster, property grip cis_wristblaster, placed directly: its msh
        # cis_weap_inf_sbdroidmuzzle is one triangle): the weapon model with its hp_fire 25 mm ahead of our barrels'
        # muzzles' middle (the game's flash 3.5 cm ahead of the opening, as Aim's FLASH_AHEAD_OF_OPENING_METERS at
        # game scale), its +z along the barrels, +y up (the back of the hand), at our scale; the grip axis as the
        # human rifle's (the controller's handle in the hand, re/body/grip-weap_rifle.json)
        arm_l.parent = rig
        arm_l.matrix_parent_inverse = Matrix.Identity(4)
        arm_l.matrix_basis = Matrix.Identity(4)
        arm_l.modifiers['rig'].object = rig
        bpy.data.objects.remove(rig_l, do_unlink=True)
        fronts = [GUN_PLACED[n][2] for n in GUN_PLACED if STOCK_CYLINDERS[n] == 'barrel']
        muzzle = sum(fronts, Vector()) / len(fronts) + Vector((0, GUN_FLASH_AHEAD, 0))
        # cis_weap_inf_sbdroidmuzzle's hp_fire relative to its root node: the game draws the model from that node's
        # frame (Aim's GetMuzzleInWeaponFrame reads -0.060 0.024 -0.018; run557-558: with its msh-world place, -0.245
        # 0.317 0.482, our arm sat the root's offset away from where the game fires)
        HP_FIRE = Vector((-0.05973474, 0.02429916, -0.01783611))
        R = Matrix(((-1, 0, 0), (0, 0, 1), (0, 1, 0))).to_4x4()  # msh x y z -> ours -x z y
        place = R @ Matrix.Scale(SCALE, 4)
        place = Matrix.Translation(muzzle / 1000 - place @ HP_FIRE) @ place
        me = bpy.data.meshes.new('weap_sbd_wristblaster')
        me.from_pydata([HP_FIRE, HP_FIRE + Vector((0.01, 0, 0)), HP_FIRE + Vector((0, 0.01, 0)),
                        HP_FIRE + Vector((0, 0, 0.03))], [], [(0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3)])
        w = bpy.data.objects.new('weap_sbd_wristblaster', me)
        bpy.context.scene.collection.objects.link(w)
        w.parent = rig
        w.matrix_world = place
        w['grip'] = 'cis_wristblaster'
        w['direct'] = 1
        g = json.load(open(os.path.join('re', 'body', 'grip-weap_rifle.json')))
        json.dump({'weapon': 'weap_sbd_wristblaster', 'axis_point': g['axis_point'], 'axis_direction': g['axis_direction'],
                   'note': 'the human rifle grip axis (the controller in the hand), tools/body_sbd.py export=1'},
                  open(os.path.join('re', 'body', 'grip-weap_sbd_wristblaster.json'), 'w'), indent=1)
        print('SBD export: hp_fire at %s mm (our frame), left arm on sbd_skeleton' % (tuple(round(x) for x in muzzle),))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))

# ---- render sheets ----
if RENDER:
    scene = bpy.context.scene

    def set_pose(pose):
        for pb in rig.pose.bones:
            pb.rotation_mode = 'ZXY' if pb.name.endswith('_1') and not pb.name.startswith('thumb') else 'XYZ'
            pb.rotation_euler = [math.radians(x) for x in pose.get(pb.name, (0, 0, 0))]
        rl = bpy.data.objects.get('sbd_skeleton_l')
        for pb in (rl.pose.bones if rl else ()):
            pb.rotation_mode = rig.pose.bones[pb.name].rotation_mode
            pb.rotation_euler = rig.pose.bones[pb.name].rotation_euler
        bpy.context.view_layer.update()

    poses = [('rest (the stock fist)', {})]
    if 'poses' in opt:
        poses = []
        for line in open(opt['poses']):
            if '|' in line:
                label, spec = line.rstrip('\n').split('|')[:2]
                poses.append((label, {b: [float(x) for x in v.split(',')] for b, v in
                                      (p.split(':') for p in spec.split(';') if p)}))
    for o in scene.objects:
        o.hide_render = (o not in (hand, arm) + ((bpy.data.objects.get('sbd_arm_l'),) if opt.get('left') == '1' else ())
                         or o in (hand, arm) and opt.get('only') == 'left'
                         and o.type == 'MESH' or o is arm and opt.get('only') == 'hand' or
                         o is hand and opt.get('only') == 'arm')
    if 'ref' in opt:  # ref=<json V,F in our frame, mm>: a reference mesh drawn in x-ray with ours
        d = json.load(open(opt['ref']))
        me = bpy.data.meshes.new('ref')
        me.from_pydata([[c / 1000 for c in v] for v in d['V']], [], d['F'])
        col = me.color_attributes.new('piece', 'FLOAT_COLOR', 'POINT')
        for c in col.data:
            c.color = (0.9, 0.9, 0.9, 1)
        ref = bpy.data.objects.new('ref', me)
        scene.collection.objects.link(ref)
        ref.hide_render = opt.get('ref_only') == '0'
        if opt.get('ref_only') == '1':
            hand.hide_render = arm.hide_render = True
        scene.display.shading.show_xray = opt.get('xray') == '1'
        scene.display.shading.xray_alpha = 0.45
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.color_type = {'solid': 'SINGLE', 'pieces': 'VERTEX', 'distort': 'VERTEX'}.get(
        opt.get('shade'), 'TEXTURE')
    if opt.get('shade') == 'distort':  # the uv stretch heat map (green none .. red 2x)
        for o in (hand, arm, bpy.data.objects.get('sbd_arm_l')):
            if o and o.data.color_attributes.get('distort'):
                o.data.color_attributes.active_color = o.data.color_attributes['distort']
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.show_cavity = True
    scene.render.resolution_x, scene.render.resolution_y = 600, 600
    scene.world = scene.world or bpy.data.worlds.new('sbd_world')
    scene.world.color = (0.55, 0.57, 0.6)
    cam = bpy.data.objects.new('sbd_cam', bpy.data.cameras.new('sbd_cam'))
    cam.data.type = 'ORTHO'
    scene.collection.objects.link(cam)
    scene.camera = cam
    VIEWS = (('thumb side', (-1, 0, 0), (0, 0, 1)), ('little-finger side', (1, 0, 0), (0, 0, 1)),
             ('back (from above)', (0, -0.3, 1), (0, 1, 0)), ('front', (-0.3, 1, 0.2), (0, 0, 1)),
             ('palm (from below)', (0, -0.3, -1), (0, 1, 0)), ('thumb side, below', (-1, 0.3, -0.7), (0, 0, 1)),
             ('from the hand, above', (-0.3, 0.8, 0.6), (0, 0, 1)), ('first person', (0.1, -0.6, 0.8), (0, 1, 0)))
    VIEWS = [v for k, v in enumerate(VIEWS) if str(k) in opt.get('views', '0123')]
    CENTRE = Vector([float(c) / 1000 for c in opt.get('centre', '-10,40,-15').split(',')])
    files = []
    for k, (label, pose) in enumerate(poses):
        set_pose(pose)
        for view, d, up in VIEWS:
            cam.data.ortho_scale = float(opt.get('size', 0.26))
            d = Vector(d).normalized()
            cam.location = CENTRE + d * 2
            right = Vector(up).cross(d).normalized()
            cam.rotation_euler = Matrix((right, d.cross(right), d)).transposed().to_euler()
            path = os.path.abspath('%s-%d-%s.png' % (RENDER, k, view.replace(' ', '')))
            scene.render.filepath = path
            bpy.ops.render.render(write_still=True)
            files.append((label, view, path))
    json.dump(files, open(os.path.abspath(RENDER + '-files.json'), 'w'))
    print('SBD rendered %d views' % len(files))
