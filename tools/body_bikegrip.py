"""Blender (blender -b --factory-startup re/body/r_arm_grips.blend -P tools/body_bikegrip.py -- <out-dir> [fit]
[only=<variant>,..] [radius=<mm>]): the speeder bikes' and the STAP's handlebar grip for every hand model of ours
(PLAN 61 j, headset run 19: the hands and forearms sat badly on the bike grips; there the stock arms' rifle fist was
put on the bar, its grip axis across the bar).

- fit: per skeleton (human: every human unit and the Wookiee wear the one glove; b1; sbd) a bar of the bikes' grip
  radius (game metres, radius=, default 19 mm) is laid through the hand on the skeleton's grip axis (BAR_AXES below:
  the human and the B1 take their rifle grip's axis, the super battle droid the tunnel of its fist), moved off the
  palm until the palm clears it by PALM_GAP, and each digit closed joint by joint, from open, until it touches the
  bar (it may press TOLERANCE into it, out of view); prints the poses (lines for tools/body_poses.py, group
  'vehicle' / 'b1 vehicle' / 'sbd vehicle', label 'bike grip (fitted)') and each skeleton's bar axis in arm space
  (BodyArm.cpp BAR_AXES), and writes <out-dir>/bikegrip-fit.json. Without `fit` the poses are read from
  tools/body_poses.py (falling back to the fit when a label is missing there).
- renders, per hand model (trooper sleeve, stormtrooper and clone armour, Wookiee fur, B1, super battle droid) and
  bike (speeder bike, STAP), one sheet <out-dir>/bikegrip-<variant>.png: rows before (the old placement: the hand's
  rifle hold with the old Handlebars pose, approximately what the stock arms showed) and after (the new pose placed
  as BodyArm::DrawOnBar places it: the bar's axis through the fist, the hand rolled about the bar so that the
  forearm points at the elbow of a two-bone arm from the rider's shoulder, the wrist bending the rest, at most
  MAX_WRIST_BEND); columns: the rider's eye, from the outside, from below in front, close-up.

The solve here is the same as BodyArm.cpp's (SolveBar): change both together."""
import bpy, json, math, os, sys
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

args = sys.argv[sys.argv.index('--') + 1:]
OUT = os.path.abspath(args[0])
FIT = 'fit' in args[1:]
opt = dict(a.split('=', 1) for a in args[1:] if '=' in a)
ONLY = opt['only'].split(',') if 'only' in opt else None
os.makedirs(OUT, exist_ok=True)

ARM_SCALE = 0.7116788  # our arm space is the game's size times this (BodyArm ArmToGrip)
RADIUS = float(opt.get('radius', 19)) / 1000 * ARM_SCALE  # the bar in arm space
PALM_GAP = 0.0008
TOLERANCE = 0.0008
MAX_WRIST_BEND = 30.0  # degrees (BodyArm MAX_BAR_WRIST_DEGREES)
# the rider's shoulder from the eye (Aim's SHOULDER_*: an adult's proportions) and the upper arm
SHOULDER_BELOW, SHOULDER_SIDE, SHOULDER_BEHIND, UPPER_ARM = 0.24, 0.19, 0.07, 0.30
POLE = Vector((0.6, -0.8, 0.2)).normalized()  # the elbow out and down, a little back (bar frame: out, up, back)

ASSETS = 'C:/LucasArts/BFBuilder/Assets/Sides/'
# the bikes in hull model space (msh: x the rider's left, y up, z forward): the rider's eye (Handlebars BIKES) and the
# right grip's bar, its centre and its outward direction (measured on the hull mesh: tools/body_bikegrip.py fit prints
# nothing for these; the numbers came from the grip's vertices, a principal axis through them); the old placement
# (Handlebars before PLAN 61 j: grip centre, pitch, roll)
BIKES = {
    'speeder bike': dict(msh=ASSETS + 'IMP/MSH/imp_hover_speederbike.msh', eye=(0.0, 1.62, 0.28),
                         centre=(-0.175, 1.245, 0.705), outward=(-0.998, -0.012, -0.054),
                         old=((-0.16, 1.27, 0.68), 20.0, 40.0)),
    'STAP': dict(msh=ASSETS + 'CIS/MSH/cis_hover_stap.msh', eye=(0.0, 2.93, -0.82),
                 centre=(-0.31, 2.42, -0.51), outward=(-0.803, -0.595, -0.032),
                 old=((-0.34, 2.43, -0.57), 5.0, 40.0)),
}

def _fingers(names, n, ratios=(1.0, 1.2, 0.8)):
    return [[('%s_%d' % (f, j), 0, -1, ratios[j - 1]) for j in range(1, n + 1)] for f in names]


# per skeleton: its rig, the mesh the fit reads, the open start pose, the sweeps (per digit its joints in order: bone,
# Euler axis, closing sign; x < 0 curls every digit of every skeleton), the palm's bones, the grip axis (a grip file's
# or point + direction), the weapon the old placement held and the old pose
SKELETONS = {
    'human': dict(rig='r_hand_stock_skeleton', fit='r_hand_retopo', group='vehicle', palm=['hand'],
                  # knuckles splayed as the library's full wrap, digits straight; the thumb out of the palm
                  open={'index_1': (10, 0, -20), 'middle_1': (10, 0, 0), 'ring_1': (10, 0, 0), 'pinky_1': (10, 0, 0),
                        'index_2': (5, 0, 0), 'middle_2': (5, 0, 0), 'ring_2': (5, 0, 0), 'pinky_2': (5, 0, 0),
                        'thumb_1': (-4, 1, 0), 'thumb_2': (0, 0, -1), 'thumb_3': (0, 1, 0)},
                  sweeps=_fingers(('index', 'middle', 'ring', 'pinky'), 3) +
                  [[('thumb_1', 2, 1, 1.0), ('thumb_2', 0, -1, 1.0), ('thumb_3', 0, -1, 1.0)]],
                  thumb_stop=['index_2', 'middle_2', 'index_3'],
                  axis='rifle', weapon='weap_rifle', old_pose='stock: stock grip'),
    'b1': dict(rig='b1_skeleton', fit='b1_hand', group='b1 vehicle', palm=['hand'],
               # from its fist (the 'b1 controller' fist idren2023 approved; its rest is open round the E-5's thin grip)
               closed_rest=True, open={'trigger_1': (-65, 0, 0), 'trigger_2': (-35, 0, 0), 'grip_1': (-65, 0, 0),
                                       'grip_2': (-35, 0, 0), 'thumb_1': (-20, 0, -10), 'thumb_2': (5, 0, 0)},
               sweeps=[[('trigger_1', 0, -1, 1.0), ('trigger_2', 0, -1, 1.0)], [('grip_1', 0, -1, 1.0), ('grip_2', 0, -1, 1.0)],
                       [('thumb_1', 0, -1, 1.0), ('thumb_2', 0, -1, 1.0)]],
               thumb_stop=['trigger_2', 'grip_2'],
               axis='cis_rifle', weapon='weap_cis_rifle', old_pose=None),
    'sbd': dict(rig='sbd_skeleton', fit='sbd_hand', group='sbd vehicle', palm=['hand'],
                closed_rest=True, open={},  # the stock fist
                sweeps=[[('grip_1', 0, -1, 1.0), ('grip_2', 0, -1, 1.0)],
                        [('thumb_1', 0, -1, 1.0), ('thumb_2', 0, -1, 1.0), ('thumb_3', 0, -1, 1.0)]],
                thumb_stop=['grip_2'],
                axis=((-0.0014, 0.060, -0.030), (1.0, -0.15, 0.0)), weapon=None, old_pose=None),
}
# the parts drawn for each hand model (BodyArm IsPartShown) and the glove texture of the unit (None: the blend's)
VARIANTS = {
    'trooper': ('human', ['r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud'], None),
    'stormtrooper': ('human', ['r_hand_retopo', 'forearm_imp_1st_stormtrooper'],
                     ASSETS + 'IMP/MSH/imp_1st_inf_stormtrooper_hands.tga'),
    'clone': ('human', ['r_hand_retopo', 'forearm_rep_1st_trooper'], ASSETS + 'REP/MSH/rep_1st_inf_clonetrooper_hands.tga'),
    'wookiee': ('human', ['r_hand_retopo', 'forearm_all_1st_wookie', 'hair_all_1st_wookie', 'hair_all_1st_wookie_hand'],
                os.path.abspath('re/body/all_1st_inf_wookie_hands.png')),
    'b1': ('b1', ['b1_hand', 'b1_arm'], None),
    'sbd': ('sbd', ['sbd_hand', 'sbd_arm'], None),
}
LABEL = 'bike grip (fitted)'

src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'body_poses.py')).read()
ns = {}
exec(src[src.index('FINGERS = '):src.index('scene = bpy.context.scene')], ns)
LIBRARY = {'%s: %s' % (g, l): p for g, ps in ns['GROUPS'] for l, p in ps}

scene = bpy.context.scene
dg = bpy.context.evaluated_depsgraph_get


def rig_of(k):
    return bpy.data.objects[SKELETONS[k]['rig']]


def set_pose(rig, p):
    for pb in rig.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
        pb.location = (0, 0, 0)
        for c in pb.constraints:
            c.mute = True  # BodyArm poses no twist bone by constraint
    for n, d in p.items():
        if n in rig.pose.bones:
            pb = rig.pose.bones[n]
            pb.rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
            pb.rotation_euler = [math.radians(x) for x in d]
    bpy.context.view_layer.update()


def straighten(rig, toward):
    """BodyArm ComputeSkin's wrist step: the forearm turned about the wrist (the arm space origin) so it runs from the
    elbow along `toward` (arm space); the hand back at its rest with its pose."""
    if 'forearm' not in rig.pose.bones:
        return
    fpb, hpb = rig.pose.bones['forearm'], rig.pose.bones['hand']
    along = (-fpb.bone.head_local).normalized()
    turn = along.rotation_difference(Vector(toward).normalized()).to_matrix().to_4x4()
    saved = hpb.matrix.copy()
    fpb.matrix = turn @ fpb.matrix
    bpy.context.view_layer.update()
    hpb.matrix = saved
    bpy.context.view_layer.update()


def axis_of(k):
    a = SKELETONS[k]['axis']
    if isinstance(a, str):
        g = json.load(open(os.path.abspath('re/body/grip-weap_%s.json' % a)))
        return Vector(g['axis_point']), Vector(g['axis_direction']).normalized()
    return Vector(a[0]), Vector(a[1]).normalized()


def weights(obj, bones):
    names = {g.index: g.name for g in obj.vertex_groups}
    return [v.index for v in obj.data.vertices if sum(g.weight for g in v.groups if names[g.group] in bones) >= 0.4]


def posed_points(obj, ids):
    me = obj.evaluated_get(dg()).to_mesh()
    inv = obj.parent.matrix_world.inverted() @ obj.matrix_world
    pts = [inv @ me.vertices[i].co for i in ids]
    obj.evaluated_get(dg()).to_mesh_clear()
    return pts


def clearance(pts, point, direction):
    worst = 1.0
    for q in pts:
        d = q - point
        r = (d - direction * d.dot(direction)).length - RADIUS
        worst = min(worst, r)
    return worst


def fit(k):
    """The bike grip pose of skeleton k: palm off the bar, every digit closed onto it joint by joint."""
    s = SKELETONS[k]
    rig, obj = rig_of(k), bpy.data.objects[s['fit']]
    point, direction = axis_of(k)
    pose = {b: tuple(float(x) for x in v) for b, v in s['open'].items()}
    set_pose(rig, pose)
    if s.get('closed_rest'):
        return fit_closed(k, pose, point, direction)
    palm_ids = weights(obj, s['palm'])
    for _ in range(8):  # off the palm, along the palm's normal (the hand's -z), to PALM_GAP
        c = clearance(posed_points(obj, palm_ids), point, direction)
        point = point - Vector((0, 0, PALM_GAP - c))
        if abs(c - PALM_GAP) < 0.0002:
            break
    log = []
    names = {g.index: g.name for g in obj.vertex_groups}
    stop_ids = weights(obj, s.get('thumb_stop', []))  # the thumb stops on these too (the fingers' middle bones)

    def closest(ids_a, ids_b):
        a, b = posed_points(obj, ids_a), posed_points(obj, ids_b)
        return min((p - q).length for p in a[::3] for q in b[::3]) if a and b else 1.0

    for chain in s['sweeps']:
        is_thumb = chain[0][0].startswith('thumb')
        # phases: every joint closing together (by its ratio) until the digit touches; then the joints after the
        # first; then the last alone (a hand closing on a bar)
        for first in range(len(chain)):
            moving = chain[first:]
            ids = weights(obj, [c[0] for c in moving])
            tip_ids = weights(obj, [chain[-1][0]])
            start = {c[0]: pose.get(c[0], (0.0, 0.0, 0.0)) for c in moving}

            def at(t):
                for b, axis, sign, ratio in moving:
                    v = list(start[b])
                    v[axis] = start[b][axis] + sign * ratio * t
                    pose[b] = tuple(v)
                set_pose(rig, pose)
                if clearance(posed_points(obj, ids), point, direction) < -TOLERANCE:
                    return True
                return bool(is_thumb and stop_ids and touching_at_start is False and
                            closest(tip_ids, stop_ids) < 0.004)

            touching_at_start = None
            if is_thumb and stop_ids:
                set_pose(rig, pose)
                touching_at_start = closest(tip_ids, stop_ids) < 0.004
            t = 0.0
            if at(0.0):  # in the bar from the start: opened until free
                while t > -60 and at(t - 2.0):
                    t -= 2.0
                t -= 2.0
            else:
                while t < 150 and not at(t + 2.0):
                    t += 2.0
                for step in (1.5, 1.0, 0.5):
                    if not at(t + step):
                        t += step
                        break
            at(t)
            for b, axis, sign, ratio in moving:
                v = list(pose[b])
                v[axis] = round(v[axis], 1)
                pose[b] = tuple(v)
            log.append('%s+ %.1f' % (moving[0][0], t))
        set_pose(rig, pose)
    print('FIT %s: %s' % (k, ', '.join(log)))
    return pose, point, direction


def fit_closed(k, pose, point, direction):
    """A droid hand whose rest is its stock closed hand (the B1's round the E-5's grip, the super battle droid's
    fist): the bar's axis put in the middle of the fist's tunnel (the line, along the grip axis's direction, farthest
    from every vertex, searched 1 mm apart within 16 mm), then each digit opened joint by joint only as far as it
    must be to clear the bar."""
    s = SKELETONS[k]
    rig, obj = rig_of(k), bpy.data.objects[s['fit']]
    # the digits' and palm's vertices within the fingers' span along the axis (the infinite line otherwise meets the
    # thumb, the wrist or the rod beside the fist)
    finger_ids = weights(obj, [c[0] for chain in s['sweeps'] if not chain[0][0].startswith('thumb') for c in chain])
    fingers = posed_points(obj, finger_ids)
    lo = min((p - point).dot(direction) for p in fingers)
    hi = max((p - point).dot(direction) for p in fingers)
    pts = [p for p in posed_points(obj, weights(obj, s['palm'] + [c[0] for chain in s['sweeps'] for c in chain]))
           if lo <= (p - point).dot(direction) <= hi]
    u = direction.cross(Vector((0, 0, 1))).normalized()  # across the axis in the hand's plane
    w = direction.cross(u).normalized()
    def enclosed(q):
        # the fist round the line: a vertex within 30 mm in at least 7 of 8 directions square to it
        seen = set()
        for p in pts:
            d = p - q
            d = d - direction * d.dot(direction)
            if d.length < 0.03:
                seen.add(int((math.atan2(d.dot(w), d.dot(u)) + math.pi) / (math.pi / 4)) % 8)
        return len(seen) >= 7

    best = (-1.0, point)
    for i in range(-16, 17):
        for j in range(-16, 17):
            q = point + u * (i * 0.001) + w * (j * 0.001)
            c = clearance(pts, q, direction)
            if c > best[0] and enclosed(q):
                best = (c, q)
    point = best[1]
    log = ['tunnel clearance %.1f mm' % (best[0] * 1000)]
    for chain in s['sweeps']:
        for n in reversed(range(len(chain))):  # the end joint first: a joint nearer the hand opens only if still in
            b, axis, sign, _ = chain[n]
            ids = weights(obj, [c[0] for c in chain[n:]])
            v = list(pose.get(b, (0.0, 0.0, 0.0)))
            opened = 0.0
            while opened < 60:
                pose[b] = tuple(v[:axis] + [v[axis] - sign * opened] + v[axis + 1:])
                set_pose(rig, pose)
                if clearance(posed_points(obj, ids), point, direction) >= -TOLERANCE:
                    break
                opened += 1.0
            log.append('%s opened %.0f' % (b, opened))
    print('FIT %s: %s' % (k, ', '.join(log)))
    return pose, point, direction


def solve_bar(k, point, direction, shoulder, twist=0.0):
    """BodyArm SolveBar: arm space -> bar frame (x out along the bar, y up, z back; game metres): the hand's bar axis on
    the bar's, rolled about it so that the forearm points at the solved elbow. Returns (4x4 arm -> bar, the forearm
    direction to straighten toward in arm space)."""
    rig = rig_of(k)
    fhead = rig.data.bones['forearm'].head_local
    forearm = fhead.length / ARM_SCALE
    x_a = direction.normalized()
    ref = Vector((0, 0, 1)) - x_a * x_a.z  # the back of the hand, square to the axis
    ref.normalize()
    z_a = x_a.cross(ref)
    r0 = Matrix((x_a, ref, z_a))  # rows: arm -> bar at roll 0 (x_a -> x, back of hand -> up)
    sh = Vector(shoulder)

    def at(theta):
        c, s_ = math.cos(theta), math.sin(theta)
        rx = Matrix(((1, 0, 0), (0, c, -s_), (0, s_, c)))
        r = rx @ r0
        wrist = r @ (-point) / ARM_SCALE
        natural = r @ Vector((0, 1, 0))
        d = wrist - sh
        dist = min(max(d.length, abs(UPPER_ARM - forearm) + 0.01), UPPER_ARM + forearm - 0.001)
        d.normalize()
        to_c = (UPPER_ARM ** 2 - forearm ** 2 + dist ** 2) / (2 * dist)
        rad = math.sqrt(max(UPPER_ARM ** 2 - to_c ** 2, 0))
        pole = POLE - d * POLE.dot(d)
        pole.normalize()
        elbow = sh + d * to_c + pole * rad
        want = (wrist - elbow).normalized()
        return r, natural.dot(want), want

    best = max((at(math.radians(t)) + (math.radians(t),) for t in range(0, 360, 5)), key=lambda q: q[1])
    theta = best[3]
    for t in [theta + math.radians(x * 0.5) for x in range(-10, 11)]:
        q = at(t)
        if q[1] > best[1]:
            best = q + (t,)
    r, score, want = at(best[3] + twist)
    toward = r.transposed() @ want
    bend = math.degrees(toward.angle(Vector((0, 1, 0))))
    if bend > MAX_WRIST_BEND:
        toward = Vector((0, 1, 0)).slerp(toward, MAX_WRIST_BEND / bend)
    m = (r @ Matrix.Diagonal((1 / ARM_SCALE,) * 3)).to_4x4()
    m.translation = r @ (-point) / ARM_SCALE
    return m, toward, bend, math.degrees(best[3])


def bar_frame(bike):
    """Bar frame -> hull (right grip): rows x out along the bar, y up, z back; origin the bar's centre."""
    x = Vector(bike['outward']).normalized()
    y = Vector((0, 1, 0)) - x * x.y
    y.normalize()
    z = x.cross(y)
    m = Matrix((x, y, z)).transposed().to_4x4()
    m.translation = bike['centre']
    return m


def shoulder_in_bar(bike, frame):
    e = Vector(bike['eye'])
    s = e + Vector((-SHOULDER_SIDE, -SHOULDER_BELOW, -SHOULDER_BEHIND))  # the right shoulder (hull x is the left)
    return frame.inverted() @ s


def old_placement(k, bike):
    """Handlebars::GetHandPose before PLAN 61 j as the stock arms drew it: the rifle-holding hand put on the old grip
    pose (the aim pose: pointing ahead tilted down by pitch, rolled thumb-inward by roll), its weapon grip point at
    the controller's grip pose there (SIMULATED_GRIP_IN_AIM). Approximate: the stock arms' fist, not ours."""
    centre, pitch, roll = bike['old']
    p, r = math.radians(pitch), -math.radians(roll)  # right hand: negative roll
    # rows right, up, back in resting space (x right, y up, z back) -> hull: x_h = -x_r, y_h = y_r, z_h = -z_r
    ps, pc, rs, rc = math.sin(p), math.cos(p), math.sin(r), math.cos(r)
    rows = [Vector((rc, -pc * rs, ps * rs)), Vector((rs, pc * rc, -ps * rc)), Vector((0, ps, pc))]
    flip = Vector((-1, 1, -1))
    right, up, back = [Vector((v.x * flip.x, v.y * flip.y, v.z * flip.z)) for v in rows]
    origin = Vector(centre)
    s = SKELETONS[k]
    if s['weapon'] and s['weapon'] in bpy.data.objects:
        w = bpy.data.objects[s['weapon']]
        wm = rig_of(k).matrix_world.inverted() @ w.matrix_world
        fwd = (wm.to_3x3() @ Vector((0, 0, 1))).normalized()
        upw = (wm.to_3x3() @ Vector((0, 1, 0))).normalized()
    else:
        fwd, upw = Vector((0, 1, 0)), Vector((0, 0, 1))
    left = upw.cross(fwd).normalized()  # the weapon's +x (the shooter's left)
    upw = fwd.cross(left).normalized()
    # weapon frame in arm space -> aim frame in hull: +z forward (-back), +y up, +x left (-right)
    arm_w = Matrix((left, upw, fwd)).transposed()  # columns: weapon axes in arm space
    hull_w = Matrix((-right, up, -back)).transposed()
    rot = hull_w @ arm_w.inverted()
    point, _ = axis_of(k)
    grip_off = Vector((0, -0.02, 0.03))  # SIMULATED_GRIP_IN_AIM's origin in the aim frame (right, up, back)
    grip_hull = origin + right * grip_off.x + up * grip_off.y + back * grip_off.z
    m = (rot @ Matrix.Diagonal((1 / ARM_SCALE,) * 3)).to_4x4()
    m.translation = grip_hull - rot @ point / ARM_SCALE
    return m


def bar_object(k, point, direction):
    """The fitted bar (arm space, on the rig): where the hand closes, beside the hull's low-poly grip."""
    import bmesh
    me = bpy.data.meshes.new('bar_' + k)
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=24, radius1=RADIUS, radius2=RADIUS, depth=0.13)
    bm.to_mesh(me)
    bm.free()
    m = bpy.data.materials.new('bar_' + k)
    m.diffuse_color = (0.95, 0.55, 0.1, 1)
    me.materials.append(m)
    ob = bpy.data.objects.new('bar_' + k, me)
    scene.collection.objects.link(ob)
    ob.parent = rig_of(k)
    rot = Vector((0, 0, 1)).rotation_difference(direction.normalized()).to_matrix().to_4x4()
    rot.translation = point
    ob.matrix_parent_inverse = Matrix()
    ob.matrix_basis = rot
    return ob


# ---- the bike hulls as meshes (hull model space as Blender's world; textured) ----
def hull_object(name, path):
    model = mshgeo.load(path)
    verts, faces, uvs, mats = [], [], [], {}
    for tri, uv, tex in mshgeo.triangles(model):
        base = len(verts)
        verts += [tuple(p) for p in tri]
        faces.append((base, base + 1, base + 2))
        uvs.append(uv)
        mats.setdefault(tex, len(mats))
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    layer = me.uv_layers.new()
    tex_of = []
    for tri, uv, tex in mshgeo.triangles(model):
        tex_of.append(tex)
    for i, poly in enumerate(me.polygons):
        for j, li in enumerate(poly.loop_indices):
            layer.data[li].uv = uvs[i][j]
    for tex, idx in mats.items():
        m = bpy.data.materials.new('%s_%d' % (name, idx))
        m.use_nodes = True
        f = os.path.join(os.path.dirname(path), tex) if tex else ''
        if tex and os.path.exists(f):
            node = m.node_tree.nodes.new('ShaderNodeTexImage')
            node.image = bpy.data.images.load(f, check_existing=True)
            m.node_tree.links.new(node.outputs['Color'], m.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
        else:
            m.diffuse_color = (0.5, 0.5, 0.5, 1)
        me.materials.append(m)
    for i, poly in enumerate(me.polygons):
        poly.material_index = mats[tex_of[i]]
    ob = bpy.data.objects.new(name, me)
    scene.collection.objects.link(ob)
    return ob


def glove_texture(objs, path):
    """The unit's glove texture on the glove (the blend has the trooper's)."""
    if not path or not os.path.exists(path):
        return
    img = bpy.data.images.load(path, check_existing=True)
    for o in objs:
        if not o.name.startswith('r_hand'):
            continue
        for slot in o.material_slots:
            if slot.material and slot.material.use_nodes:
                m = slot.material.copy()
                for n in m.node_tree.nodes:
                    if n.type == 'TEX_IMAGE':
                        n.image = img
                slot.material = m


# ---- rendering ----
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
scene.display.shading.show_backface_culling = False
TW, TH = 520, 420
scene.render.resolution_x, scene.render.resolution_y = TW, TH
cam_data = bpy.data.cameras.new('bikecam')
cam = bpy.data.objects.new('bikecam', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
TMP = os.path.join(OUT, '_tile.png')


def look(eye, target, up=(0, 1, 0)):
    eye, target, up = Vector(eye), Vector(target), Vector(up)
    back = (eye - target).normalized()
    right = up.cross(back).normalized()
    up2 = back.cross(right)
    m = Matrix((right, up2, back)).transposed().to_4x4()
    m.translation = eye
    cam.matrix_world = m


def render_tile():
    scene.render.filepath = TMP
    bpy.ops.render.render(write_still=True)
    im = bpy.data.images.load(TMP, check_existing=False)
    px = list(im.pixels[:])
    bpy.data.images.remove(im)
    return px


def label_tile(text, w, h):
    sc = bpy.data.scenes.get('label') or bpy.data.scenes.new('label')
    for o in list(sc.objects):
        bpy.data.objects.remove(o)
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.display.shading.light = 'FLAT'
    sc.display.shading.color_type = 'SINGLE'
    sc.display.shading.single_color = (0.95, 0.95, 0.95)
    sc.display.shading.background_type = 'VIEWPORT'
    sc.display.shading.background_color = (0.12, 0.12, 0.12)
    sc.render.resolution_x, sc.render.resolution_y = w, h
    cu = bpy.data.curves.new('lbl', 'FONT')
    cu.body = text
    cu.size = 1.0
    t = bpy.data.objects.new('lbl', cu)
    sc.collection.objects.link(t)
    cdl = bpy.data.cameras.new('lblcam')
    cdl.type = 'ORTHO'
    cdl.ortho_scale = w / h * 1.7 if w > h else 1.7
    cm = bpy.data.objects.new('lblcam', cdl)
    sc.collection.objects.link(cm)
    sc.camera = cm
    cm.location = (cdl.ortho_scale / 2 - 0.4, 0.3, 5)
    sc.render.filepath = TMP
    bpy.ops.render.render(write_still=True, scene=sc.name)
    im = bpy.data.images.load(TMP, check_existing=False)
    px = list(im.pixels[:])
    bpy.data.images.remove(im)
    return px


def sheet(path, rows, row_labels, col_labels):
    """rows: lists of tiles (TW x TH pixels each); a label band above each row."""
    import numpy as np
    LH = 46
    cols = max(len(r) for r in rows)
    W, H = TW * cols, (TH + LH) * len(rows) + LH
    out = np.full((H, W, 4), 0.12, dtype=np.float32)
    out[..., 3] = 1
    head = np.array(label_tile('   '.join(col_labels), W, LH), dtype=np.float32).reshape(LH, W, 4)
    out[H - LH:H] = head
    for i, (r, lab) in enumerate(zip(rows, row_labels)):
        y = H - LH - (i + 1) * (TH + LH)
        band = np.array(label_tile(lab, W, LH), dtype=np.float32).reshape(LH, W, 4)
        out[y + TH:y + TH + LH] = band
        for j, t in enumerate(r):
            out[y:y + TH, j * TW:(j + 1) * TW] = np.array(t, dtype=np.float32).reshape(TH, TW, 4)
    im = bpy.data.images.new('sheet', W, H)
    im.pixels[:] = out.ravel()
    im.filepath_raw = path
    im.file_format = 'PNG'
    im.save()
    bpy.data.images.remove(im)
    print('SHEET', path)


def views(bike, hand_world_point):
    """(label, camera setup) per column, in hull space round the right grip."""
    e, c = Vector(bike['eye']), Vector(bike['centre'])
    out = []
    out.append(('rider\'s eye', lambda: (setattr(cam_data, 'type', 'PERSP'), setattr(cam_data, 'lens', 18),
                                          look(e + Vector((0.02, 0, 0)), c + Vector((0.05, -0.05, 0))))))
    out.append(('outside', lambda: (setattr(cam_data, 'type', 'ORTHO'), setattr(cam_data, 'ortho_scale', 0.55),
                                    look(c + Vector((-1.2, 0.15, -0.05)), c + Vector((0, -0.05, -0.08))))))
    out.append(('below, ahead', lambda: (setattr(cam_data, 'type', 'ORTHO'), setattr(cam_data, 'ortho_scale', 0.5),
                                         look(c + Vector((-0.35, -0.6, 0.9)), c + Vector((0, -0.04, -0.05))))))
    out.append(('close, behind', lambda: (setattr(cam_data, 'type', 'ORTHO'), setattr(cam_data, 'ortho_scale', 0.32),
                                          look(c + Vector((-0.25, 0.35, -0.9)), c + Vector((0.0, -0.02, -0.03))))))
    return out


def main():
    results = {}
    for k in SKELETONS:
        label = '%s: %s' % (SKELETONS[k]['group'], LABEL)
        if FIT or label not in LIBRARY:
            pose, point, direction = fit(k)
        else:
            pose = LIBRARY[label]
            point, direction = axis_of(k)
            # the axis as fitted: kept in the fit file
            f = os.path.join(OUT, 'bikegrip-fit.json')
            if os.path.exists(f):
                saved = json.load(open(f)).get(k)
                if saved:
                    point, direction = Vector(saved['axis_point']), Vector(saved['axis_direction'])
        results[k] = dict(pose={b: list(v) for b, v in pose.items()}, axis_point=list(point),
                          axis_direction=list(direction.normalized()))
        print("POSE ('%s', pose(%s))," % (LABEL, ', '.join('%s=(%g, %g, %g)' % ((b,) + tuple(v)) for b, v in pose.items())))
        print('AXIS %s point %.4f %.4f %.4f direction %.4f %.4f %.4f' % ((k,) + tuple(point) + tuple(direction.normalized())))
    if FIT:
        json.dump(results, open(os.path.join(OUT, 'bikegrip-fit.json'), 'w'), indent=1)
    # each skeleton's hand on its bar in arm space (the fit itself): thumb side, palm, back, along the bar
    for k in SKELETONS:
        if ONLY and not any(VARIANTS[v][0] == k for v in ONLY):
            continue
        r = results[k]
        rig = rig_of(k)
        rig.matrix_world = Matrix()
        bar = bar_object(k, Vector(r['axis_point']), Vector(r['axis_direction']))
        shown = [bpy.data.objects[p] for v, (kk, parts, _) in VARIANTS.items() if kk == k for p in parts[:1]]
        for o in scene.objects:
            o.hide_render = not (o in shown or o is bar or o is cam)
        set_pose(rig, {b: tuple(x) for b, x in r['pose'].items()})
        c = Vector(r['axis_point'])
        cam_data.type = 'ORTHO'
        cam_data.ortho_scale = 0.16
        tiles = []
        for eye, up in (((-1, 0.1, 0.1), (0, 0, 1)), ((0, 0.1, -1), (0, 1, 0)), ((0.1, -0.2, 1), (0, 1, 0)),
                        ((1, -0.25, 0.03), (0, 0, 1)), ((-0.3, 1, -0.4), (0, 0, 1))):
            look(c + Vector(eye), c, up)
            tiles.append(render_tile())
        sheet(os.path.join(OUT, 'bikegrip-fit-%s.png' % k), [tiles], ['%s: %s on the bar (radius %.0f mm in the game)' % (k, LABEL, RADIUS / ARM_SCALE * 1000)],
              ['thumb side', 'palm', 'back', 'along the bar (from the little finger)', 'fingertips'])
        bpy.data.objects.remove(bar)
    hulls = {name: hull_object(name, b['msh']) for name, b in BIKES.items()}
    # a thin marker of the bar's axis (the hull's grips are low-poly: shows where the hand closes)
    for v, (k, parts, glove) in VARIANTS.items():
        if ONLY and v not in ONLY:
            continue
        rig = rig_of(k)
        objs = [bpy.data.objects[p] for p in parts if p in bpy.data.objects]
        glove_texture(objs, glove)
        rows, labels = [], []
        for name, bike in BIKES.items():
            for o in scene.objects:
                o.hide_render = not (o in objs or o is hulls[name] or o is cam)
            frame = bar_frame(bike)
            shoulder = shoulder_in_bar(bike, frame)
            # before
            old = old_placement(k, bike)
            before = LIBRARY.get(SKELETONS[k]['old_pose'], {}) if SKELETONS[k]['old_pose'] else {}
            set_pose(rig, before)
            rig.matrix_world = old
            bpy.context.view_layer.update()
            tiles = []
            for lab, setup in views(bike, None):
                setup()
                tiles.append(render_tile())
            rows.append(tiles)
            labels.append('%s, %s: BEFORE (the rifle hold on the old grip pose, approx.)' % (v, name))
            # after
            r = results[k]
            m, toward, bend, roll = solve_bar(k, Vector(r['axis_point']), Vector(r['axis_direction']), shoulder)
            set_pose(rig, {b: tuple(x) for b, x in r['pose'].items()})
            straighten(rig, toward)
            rig.matrix_world = frame @ m
            bpy.context.view_layer.update()
            tiles = []
            for lab, setup in views(bike, None):
                setup()
                tiles.append(render_tile())
            rows.append(tiles)
            labels.append('%s, %s: AFTER (bike grip pose; roll %.0f deg, wrist bend %.0f deg)' % (v, name, roll, min(bend, MAX_WRIST_BEND)))
            print('PLACED %s %s: roll %.1f bend %.1f toward %s' % (v, name, roll, bend, tuple(round(x, 3) for x in toward)))
        sheet(os.path.join(OUT, 'bikegrip-%s.png' % v), rows, labels, [lab for lab, _ in views(BIKES['STAP'], None)])
        rig.matrix_world = Matrix()
    if os.path.exists(TMP):
        os.remove(TMP)


main()
