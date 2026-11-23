"""Blender (blender -b --factory-startup re/body/r_arm_weapons.blend -P tools/body_tunegrip.py -- <out.blend> <weapon>
grip=x0,x1,y0,y1 [pose=<label>] [palm=mm] [depth=mm] [save]): tunes a weapon in our hand (idren2023 2026-10-24: the gun in
the hand without clipping on the outside of the fingers, the grip in the palm), starting from the pose `pose` (default
the stock grip) and the weapon where it is now (tools/body_placeweapon.py stock):

- clipping: the hand's triangles crossing the weapon's (the gun showing through the glove), counted on each digit's
  own triangles (vertices weighted at least half to its bones) and on the palm's (hand / thumb_1, facing the palm).
- the palm: the weapon moves along the hand's y and z (1 mm steps, up to 40 mm toward the wrist, 30 mm either way
  across the palm) to the place nearest its start where the palm crosses nothing of the grip and comes nearest
  `palm` mm (default 1) of it.
- each digit (the index stays on the trigger as posed): its pose angles scaled (0.2 to 1.5 of the start) up to the most
  closed that crosses nothing.
Prints the measures before and after and the tuned pose (a line for tools/body_poses.py); with `save` writes the
weapon's place and grip axis (re/body/grip-weap_<weapon>.json, as tools/body_placeweapon.py) and saves <out.blend>."""
import bpy, json, math, os, sys
from mathutils import Matrix, Vector, geometry
from mathutils.bvhtree import BVHTree
import bmesh

args = sys.argv[sys.argv.index('--') + 1:]
OUT, WEAP = args[0], args[1]
opt = dict(a.split('=', 1) for a in args[2:] if '=' in a)
LABEL = opt.get('pose', 'stock grip')
PALM_GAP = float(opt.get('palm', 1)) / 1000
DEPTH = float(opt.get('depth', 0.5)) / 1000
x0, x1, y0, y1 = (float(v) / 1000 for v in opt['grip'].split(','))
hand = bpy.data.objects['r_hand_retopo']
rig = hand.parent
P = rig.pose.bones
w = bpy.data.objects['weap_' + WEAP]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'body_poses.py')).read()
ns = {}
exec(src[src.index('def wrap('):src.index('scene = bpy.context.scene')], dict(ns, **{'FINGERS': ('index', 'middle', 'ring', 'pinky')}), ns)
POSE = dict(next(p for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(LABEL)))
names = {g.index: g.name for g in hand.vertex_groups}
DIGITS = {f: ['%s_%d' % (f, k) for k in (1, 2, 3)] for f in ('index', 'middle', 'ring', 'pinky', 'thumb')}


def apply(p):
    for pb in P:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
        pb.location = (0, 0, 0)
    for n, d in p.items():
        if n in P:
            P[n].rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
            P[n].rotation_euler = [math.radians(x) for x in d]
    bpy.context.view_layer.update()
    me = hand.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
    return [hand.matrix_world @ v.co for v in me.vertices], [hand.matrix_world.to_3x3() @ v.normal for v in me.vertices]


own = {f: [v.index for v in hand.data.vertices if sum(g.weight for g in v.groups if names[g.group] in bones) > 0.5]
       for f, bones in DIGITS.items()}


W_START = w.matrix_world.copy()
# the grip's vertices (the box grip= in the weapon's place as tools/body_weapons.py left it: its mesh coordinates)
GRIP_IDS = {i for i, v in enumerate(w.data.vertices) if x0 <= v.co.x <= x1 and y0 <= v.co.y <= y1}


def weapon_tree(grip_only=False):
    """The weapon's triangles (or its grip's) in a BVH (the visible clipping is its surface crossing the hand's)."""
    b = bmesh.new()
    b.from_mesh(w.data)
    if grip_only:
        bmesh.ops.delete(b, geom=[v for v in b.verts if v.index not in GRIP_IDS], context='VERTS')
    b.transform(w.matrix_world)
    return BVHTree.FromBMesh(b)


hand.data.calc_loop_triangles()
TRIS = [tuple(t.vertices) for t in hand.data.loop_triangles]


def crossings(pts, ids, tree):
    """How many of the hand's triangles among the vertices ids cross the weapon's surface, and the nearest distance of
    those vertices to it."""
    idset = set(ids)
    tris = [t for t in TRIS if t[0] in idset and t[1] in idset and t[2] in idset]
    if not tris:
        return 0, 1e9
    mine = BVHTree.FromPolygons(pts, tris)
    count = len({a for a, _ in mine.overlap(tree)})
    nearest = min(tree.find_nearest(pts[i], 0.03)[3] if tree.find_nearest(pts[i], 0.03)[0] is not None else 1e9
                  for i in ids)
    return count, nearest


def outer_crossings(pts, ids, tree, axis_point, axis_dir):
    """Like crossings, counting only the hand's triangles that face away from the grip's axis (the back and sides of
    a finger wrapped round it): the gun showing through the outside of the glove. A pad pressing into the grip faces
    the axis and is let be."""
    idset = set(ids)
    tris = []
    for t in TRIS:
        if t[0] in idset and t[1] in idset and t[2] in idset:
            a_, b_, c_ = (pts[i] for i in t)
            n_ = (b_ - a_).cross(c_ - a_)
            centre = (a_ + b_ + c_) / 3
            r_ = centre - axis_point
            r_ -= axis_dir * r_.dot(axis_dir)
            if n_.dot(r_) > 0:
                tris.append(t)
    if not tris:
        return 0
    return len({x for x, _ in BVHTree.FromPolygons(pts, tris).overlap(tree)})


def measure(pose, tree, label):
    pts, nrms = apply(pose)
    up = (P['hand'].matrix.to_3x3()).col[2].normalized()
    palm_ids = [i for i, v in enumerate(hand.data.vertices)
                if sum(g.weight for g in v.groups if names[g.group] in ('hand', 'thumb_1')) > 0.8 and nrms[i].dot(up) < -0.3]
    out = {f: crossings(pts, ids, tree) for f, ids in own.items()}
    out['palm'] = crossings(pts, palm_ids, tree)
    print('TUNE %-7s %s' % (label, ' | '.join('%s %d crossing, near %.1f mm' % (k, c, n * 1000 if n < 1 else -1)
                                               for k, (c, n) in out.items())))
    return out, palm_ids


tree = weapon_tree()
before, palm_ids = measure(POSE, tree, 'before')
# ---- the palm: slide along the hand's z (out of the palm is -z) ------------------------------------------------------
up = Vector((0, 0, 1))
W0 = w.matrix_world.copy()
# the grip's axis where the weapon starts (re/body/grip-weap_<weapon>.json), its radius from the grip's vertices; the
# rule for clipping (idren2023 2026-10-24): what faces the gun while it is held (finger pads, the palm) may press into it
# a little, what faces away (the backs and sides) must not cross it
JSON_PATH = os.path.join('re', 'body', 'grip-weap_%s.json' % WEAP)
# the grip's own axis where the weapon hangs now: measured_axis_* when axis_* was set to another weapon's (the other
# sides' grips take the Rebel rifle's / pistol's axis for the controller, 2026-10-31)
_GRIP_JSON = json.load(open(JSON_PATH))
AXIS0 = Vector(_GRIP_JSON.get('measured_axis_point', _GRIP_JSON['axis_point']))
AXIS_DIR = Vector(_GRIP_JSON.get('measured_axis_direction', _GRIP_JSON['axis_direction'])).normalized()
GRIP_RADIUS = sum(((w.matrix_world @ v.co - AXIS0) - AXIS_DIR * (w.matrix_world @ v.co - AXIS0).dot(AXIS_DIR)).length
                  for i, v in enumerate(w.data.vertices) if i in GRIP_IDS) / max(len(GRIP_IDS), 1)
PRESS = float(opt.get('press', 2.5)) / 1000  # how deep the grip sinks into the palm (its cylinder against the palm)


def press_of(pts, ids, axis_point):
    """How deep the grip (as a cylinder of GRIP_RADIUS about its axis) reaches into the given vertices."""
    deepest = -1.0
    for i in ids:
        r_ = pts[i] - axis_point
        r_ -= AXIS_DIR * r_.dot(AXIS_DIR)
        deepest = max(deepest, GRIP_RADIUS - r_.length)
    return deepest
pts, _ = apply(POSE)
best = None
# along the hand's y (toward the wrist is negative) and z (out of the palm is negative), 1 mm steps: the place nearest
# the start where the palm crosses nothing of the grip and comes nearest PALM_GAP of it
fwd = Vector((0, 1, 0))
side = Vector((1, 0, 0))
SIDE_MM = int(opt.get('side', 0))       # also try moving along the hand's x (toward the little finger), +-SIDE_MM
CENTRE = [int(v) for v in opt.get('around', '').split(',')] if opt.get('around') else None  # y,z mm to search round
ys = range(CENTRE[0] - 6, CENTRE[0] + 7, 2) if CENTRE else range(-40, 11, 2)
zs = range(CENTRE[1] - 6, CENTRE[1] + 7, 2) if CENTRE else range(-30, 31, 2)
candidates = []
for kx in range(-SIDE_MM, SIDE_MM + 1, 2):
    for ky in ys:
        for kz in zs:
            candidates.append(side * (kx * 0.001) + fwd * (ky * 0.001) + up * (kz * 0.001))
DIGIT_SCORE = 'digits' in args          # rank places by how many digit crossings the start pose leaves (slower)
KEEP = 'keep' in args  # the weapon stays and the pose stays: only the named steps run
if KEEP:
    candidates = [Vector()]
for move in candidates:
    if True:
        w.matrix_world = Matrix.Translation(move) @ W0
        axis_here = AXIS0 + move
        c = outer_crossings(pts, palm_ids, weapon_tree(grip_only=True), axis_here, AXIS_DIR)
        n = press_of(pts, palm_ids, axis_here)
        if c:
            continue
        extra = 0
        if DIGIT_SCORE:
            t_all = weapon_tree()
            extra = sum(crossings(pts, own[f], t_all)[0] for f in ('index', 'thumb', 'pinky'))
        score = (extra, round(abs(n - PRESS) * 1000), move.length)
        if best is None or score < best[0]:
            best = (score, move, n)
move = best[1] if best else Vector()
w.matrix_world = Matrix.Translation(move) @ W0
tree = weapon_tree()
print('TUNE palm: weapon moved %.1f mm along x (toward the little finger), %.1f mm along y (toward the wrist negative), '
      '%.1f mm along z (out of the palm negative); the grip %.1f mm into the palm; start-pose crossings of index, thumb, '
      'little finger %s' % (move.x * 1000, move.y * 1000, move.z * 1000, best[2] * 1000 if best else -1,
                            best[0][0] if best else '-'))
# ---- contact (idren2023 2026-10-24: the pistol's grip loose on the palm side, the web between thumb and index should
# close round it): the weapon turned about the grip's axis point (about the hand's y and z, -12..12 deg) and moved
# (-6..6 mm in x, y, z) to where most palm and web vertices lie on the grip's surface (within 2 mm), the web counted
# three times, nothing through the back of the hand, the grip at most PRESS_MAX deep in the palm
if 'contact' in args:
    PRESS_MAX = float(opt.get('pressmax', 6)) / 1000
    pts, _ = apply(POSE)
    thumb_head = rig.matrix_world @ P['thumb_1'].head
    index_head = rig.matrix_world @ P['index_1'].head
    web_ids = [i for i, v in enumerate(hand.data.vertices)
               if sum(g.weight for g in v.groups if names[g.group] in ('thumb_1', 'hand', 'index_1')) > 0.9
               and sum(g.weight for g in v.groups if names[g.group] == 'thumb_1') > 0.05
               and (pts[i] - (thumb_head + index_head) / 2).length < 0.03]
    feel_ids = sorted(set(palm_ids) | set(web_ids))
    webset = set(web_ids)
    base = Matrix.Translation(move) @ W0
    pivot = AXIS0 + move

    def placed(rot_y, rot_z, shift):
        r_ = (Matrix.Rotation(math.radians(rot_z), 4, 'Z') @ Matrix.Rotation(math.radians(rot_y), 4, 'Y'))
        return Matrix.Translation(pivot + shift) @ r_ @ Matrix.Translation(-pivot) @ base, r_

    def score_at(mat, r_):
        w.matrix_world = mat
        gt = weapon_tree(grip_only=True)
        axis_p = mat @ (base.inverted() @ pivot)
        axis_d = (r_.to_3x3() @ AXIS_DIR).normalized()
        if outer_crossings(pts, feel_ids, gt, axis_p, axis_d):
            return None
        deep = -1.0
        for i in feel_ids:
            q = pts[i] - axis_p
            q -= axis_d * q.dot(axis_d)
            deep = max(deep, GRIP_RADIUS - q.length)
        if deep > PRESS_MAX:
            return None
        touch = 0
        for i in feel_ids:
            loc, _, _, dist = gt.find_nearest(pts[i], 0.01)
            if loc is not None and dist <= 0.002:
                touch += 3 if i in webset else 1
        return touch

    mat0, r0 = placed(0, 0, Vector())
    start_score = score_at(mat0, r0)
    print('TUNE contact: at the start %s touching' % start_score)
    RANGE = int(opt.get('contactmm', 6))
    best_c = None
    for ry in range(-16, 17, 4):
        for rz in range(-16, 17, 4):
            mat, r_ = placed(ry, rz, Vector())
            sc = score_at(mat, r_)
            if sc is not None and (best_c is None or sc > best_c[0]):
                best_c = (sc, ry, rz, Vector())
    if best_c:
        _, ry, rz, _ = best_c
        for kx in range(-RANGE, RANGE + 1, 2):
            for ky in range(-RANGE, RANGE + 1, 2):
                for kz in range(-6, 7, 2):
                    sh = Vector((kx, ky, kz)) * 0.001
                    mat, r_ = placed(ry, rz, sh)
                    sc = score_at(mat, r_)
                    if sc is not None and sc > best_c[0]:
                        best_c = (sc, ry, rz, sh)
        _, ry, rz, sh = best_c
        mat, r_ = placed(ry, rz, sh)
        w.matrix_world = mat
        new_axis = mat @ (base.inverted() @ pivot)
        AXIS_DIR = (r_.to_3x3() @ AXIS_DIR).normalized()
        AXIS0 = new_axis - move
        tree = weapon_tree()
        print('TUNE contact: turned %+d deg about y, %+d about z, moved %.0f %.0f %.0f mm; palm and web touching: %d '
              '(web counted x3; %d palm, %d web vertices)' % (ry, rz, sh.x * 1000, sh.y * 1000, sh.z * 1000,
                                                             best_c[0], len(palm_ids), len(web_ids)))
    else:
        w.matrix_world = base
        print('TUNE contact: no place without crossings (kept)')
# ---- knuckles (idren2023 2026-10-24: the pistol's finger knuckles closer to the grip so the fingers wrap round it; the
# palm may clip, it is hidden): the weapon moved (y toward the knuckles, z into the hand, x along the knuckle row,
# 1 mm steps up to `knucklemm`) so the middle, ring and little fingers' knuckle joints come `knucklegap` mm (default
# 10: the finger's half thickness) from the grip's surface, nothing through the back of the hand
if 'knuckles' in args:
    KG = float(opt.get('knucklegap', 10)) / 1000
    KR = int(opt.get('knucklemm', 20))
    pts, _ = apply(POSE)
    back_ids = [i for i, v in enumerate(hand.data.vertices)
                if sum(g.weight for g in v.groups if names[g.group] in ('hand', 'thumb_1')) > 0.5]
    knuckles = [rig.matrix_world @ P[f + '_1'].head for f in ('middle', 'ring', 'pinky')]
    M0 = w.matrix_world.copy()
    best_k = None
    for kx in range(-6, 7, 3):
        for ky in range(-4, KR + 1, 2):
            for kz in range(-KR, KR + 1, 2):
                sh = Vector((kx, ky, kz)) * 0.001
                w.matrix_world = Matrix.Translation(sh) @ M0
                gt = weapon_tree(grip_only=True)
                ds = []
                for k in knuckles:
                    loc, _, _, d = gt.find_nearest(k, 0.08)
                    ds.append(d if loc is not None else 0.08)
                err = sum(abs(d - KG) for d in ds) / len(ds)
                if best_k is not None and err >= best_k[0]:
                    continue
                if outer_crossings(pts, back_ids, gt, AXIS0 + move + sh, AXIS_DIR):
                    continue
                best_k = (err, sh, ds)
    # then turned about the grip's axis point (x tilts the knuckle row toward the grip, y rolls the hand round it,
    # z turns it across), the move refined round the best, the same aim
    best_r = (best_k[0], 0, 0, 0, best_k[1]) if best_k else None
    if best_k:
        pivot_k = AXIS0 + move
        for rx in range(-25, 26, 5):
            for ry in range(-15, 16, 5):
                for rz in range(-15, 16, 5):
                    R_ = (Matrix.Rotation(math.radians(rz), 4, 'Z') @ Matrix.Rotation(math.radians(ry), 4, 'Y') @
                          Matrix.Rotation(math.radians(rx), 4, 'X'))
                    for d_ in ((0, 0, 0), (0, 4, 0), (0, -4, 0), (0, 0, 4), (0, 0, -4)):
                        sh = best_k[1] + Vector(d_) * 0.001
                        mat = Matrix.Translation(pivot_k + sh) @ R_ @ Matrix.Translation(-pivot_k) @ M0
                        w.matrix_world = mat
                        gt = weapon_tree(grip_only=True)
                        ds = []
                        for k in knuckles:
                            loc, _, _, d = gt.find_nearest(k, 0.08)
                            ds.append(d if loc is not None else 0.08)
                        err = sum(abs(d - KG) for d in ds) / len(ds)
                        if err >= best_r[0] - 1e-5:
                            continue
                        ax_d = (R_.to_3x3() @ AXIS_DIR).normalized()
                        if outer_crossings(pts, back_ids, gt, pivot_k + sh, ax_d):
                            continue
                        best_r = (err, rx, ry, rz, sh, ds)
    if best_k:
        err, rx, ry, rz, sh = best_r[:5]
        if len(best_r) > 5:
            best_k = (err, sh, best_r[5])
        R_ = (Matrix.Rotation(math.radians(rz), 4, 'Z') @ Matrix.Rotation(math.radians(ry), 4, 'Y') @
              Matrix.Rotation(math.radians(rx), 4, 'X'))
        pivot_k = AXIS0 + move
        w.matrix_world = Matrix.Translation(pivot_k + sh) @ R_ @ Matrix.Translation(-pivot_k) @ M0
        AXIS_DIR = (R_.to_3x3() @ AXIS_DIR).normalized()
        print('TUNE knuckles: turned %+d %+d %+d deg about x, y, z' % (rx, ry, rz))
        move = move + sh
        tree = weapon_tree()
        print('TUNE knuckles: weapon moved %.0f %.0f %.0f mm (x, y toward the knuckles, z); knuckles %s mm from the '
              'grip' % (sh.x * 1000, sh.y * 1000, sh.z * 1000, ' '.join('%.0f' % (d * 1000) for d in best_k[2])))
    else:
        w.matrix_world = M0
        print('TUNE knuckles: no place without the grip through the back of the hand')
# ---- the digits ----------------------------------------------------------------------------------------------------
tuned = dict(POSE)
# each digit: its curl scaled (0.2 to 1.5 of the start pose) and its first joint swung sideways (z, -30 to +30 deg
# added), the most closed curl that crosses nothing, with the smallest swing that allows it; the index (on the trigger)
# and the thumb included
for f in (() if KEEP else ('index', 'middle', 'ring', 'pinky', 'thumb')):
    base = {n: POSE.get(n, (0, 0, 0)) for n in DIGITS[f]}
    first = DIGITS[f][0]
    choice = None
    for swing in (0, -5, 5, -10, 10, -15, 15, -20, 20, -25, 25, -30, 30):
        best_here = None
        for k in range(20, 151, 5):
            s_ = k / 100
            trial = dict(tuned, **{n: tuple(x * s_ for x in d) for n, d in base.items()})
            x_, y_, z_ = trial[first]
            trial[first] = (x_, y_, z_ + swing)
            pts, _ = apply(trial)
            c = outer_crossings(pts, own[f], tree, AXIS0 + move, AXIS_DIR)
            n = crossings(pts, own[f], tree)[1]
            if c == 0:
                best_here = (s_, n, swing, trial)
            elif best_here is not None:
                break
        if best_here and (choice is None or best_here[0] > choice[0] + 0.15):
            choice = best_here
    if choice:
        for n in DIGITS[f]:
            tuned[n] = tuple(round(x) for x in choice[3][n])
    print('TUNE %s x%.2f swing %+d (nearest %.1f mm)%s' % (
        f, choice[0] if choice else 0, choice[2] if choice else 0, choice[1] * 1000 if choice and choice[1] < 1 else -1,
        '' if choice else ': crosses at every curl and swing (left as posed)'))
# the fingers round the handle (idren2023 2026-10-24: the grip in the hand, wrapped): middle, ring and little finger, the
# knuckle and the two outer joints scaled apart (knuckle 0.2 to 1.2, outer joints 0.4 to 2.2 of the start pose), the
# gun nowhere through the outside of the finger (its pad may press in); the fingertip as far round the grip (past its
# axis away from the palm) as it goes
if 'fingerwrap' in args:
    jp = os.path.join('re', 'body', 'grip-weap_%s.json' % WEAP)
    g_ = json.load(open(jp))
    axis_point_f = AXIS0 + move
    axis_dir_f = AXIS_DIR
    # tipspress=<mm>: the fingertips (the last segment, hidden behind the grip from the player) may press in up to that
    # deep (idren2023 2026-10-24: the rifle's fingers stuck out straight past the grip)
    TIP_PRESS = float(opt['tipspress']) / 1000 if opt.get('tipspress') else None
    base_own = {f: [v.index for v in hand.data.vertices
                    if sum(g.weight for g in v.groups if names[g.group] in DIGITS[f][:2]) > 0.5] for f in DIGITS}
    tip_own = {f: [v.index for v in hand.data.vertices
                   if sum(g.weight for g in v.groups if names[g.group] == DIGITS[f][2]) > 0.5] for f in DIGITS}
    # the direction a curl turns about the grip's axis: the start pose's middle finger must wrap further than the
    # same finger held straight
    def wrap_of(f):
        chain = [rig.matrix_world @ P[DIGITS[f][0]].head, rig.matrix_world @ P[DIGITS[f][1]].head,
                 rig.matrix_world @ P[DIGITS[f][2]].head, rig.matrix_world @ P[DIGITS[f][2]].tail]
        flat = []
        for q in chain:
            q = q - axis_point_f
            flat.append(q - axis_dir_f * q.dot(axis_dir_f))
        total = 0.0
        for u_, v_ in zip(flat, flat[1:]):
            ang = u_.angle(v_, 0.0)
            total += ang if axis_dir_f.dot(u_.cross(v_)) >= 0 else -ang
        return total
    apply(POSE)
    curled = wrap_of('middle')
    apply(dict(POSE, middle_1=(0, 0, 0), middle_2=(0, 0, 0), middle_3=(0, 0, 0)))
    WRAP_SIGN = 1.0 if curled >= wrap_of('middle') else -1.0
    WRAP_FINGERS = opt.get('fingers', 'middle,ring,pinky').split(',')  # fingers=index,middle,.. (the canister: all four)
    for f in WRAP_FINGERS:
        start = {n: POSE.get(n, (0, 0, 0)) for n in DIGITS[f]}
        found = None
        for ka in range(20, int(opt.get('knucklemax', 120)) + 1, 10):
            for kb in range(40, 221, 15):
                a_, b_ = ka / 100, kb / 100
                trial = dict(tuned)
                trial[DIGITS[f][0]] = tuple(v * a_ for v in start[DIGITS[f][0]])
                for n in DIGITS[f][1:]:
                    trial[n] = tuple(v * b_ for v in start[n])
                pts, _ = apply(trial)
                if TIP_PRESS is not None:
                    if outer_crossings(pts, base_own[f], tree, axis_point_f, axis_dir_f):
                        continue
                    deep = -1.0
                    for i in tip_own[f]:
                        q = pts[i] - axis_point_f
                        q -= axis_dir_f * q.dot(axis_dir_f)
                        deep = max(deep, GRIP_RADIUS - q.length)
                    if deep > TIP_PRESS:
                        continue
                elif outer_crossings(pts, own[f], tree, axis_point_f, axis_dir_f):
                    continue
                cnt, near = crossings(pts, own[f], tree)
                tip = rig.matrix_world @ P[DIGITS[f][2]].tail
                rel = tip - axis_point_f
                rel -= axis_dir_f * rel.dot(axis_dir_f)
                # how far the finger wraps round the grip: the angle about the grip's axis swept from its knuckle
                # through its joints to its tip (summed segment by segment, so beyond 180 degrees too); a tip curling
                # back toward the palm still counts as more wrap (idren2023: the fingertips stuck out straight)
                chain = [rig.matrix_world @ P[DIGITS[f][0]].head, rig.matrix_world @ P[DIGITS[f][1]].head,
                         rig.matrix_world @ P[DIGITS[f][2]].head, tip]
                flat = []
                for q in chain:
                    q = q - axis_point_f
                    flat.append(q - axis_dir_f * q.dot(axis_dir_f))
                wrap = 0.0
                for u_, v_ in zip(flat, flat[1:]):
                    ang = u_.angle(v_, 0.0)
                    wrap += ang if axis_dir_f.dot(u_.cross(v_)) * WRAP_SIGN >= 0 else -ang
                key = (round(math.degrees(wrap) / 4), -near)
                if found is None or key > found[0]:
                    found = (key, trial, a_, b_, math.degrees(wrap), near)
        if found:
            for n in DIGITS[f]:
                tuned[n] = tuple(round(v) for v in found[1][n])
            print('TUNE %s wrapped: knuckle x%.2f, outer joints x%.2f; wrapped %.0f deg round the grip, nearest %.1f mm' % (
                f, found[2], found[3], found[4], found[5] * 1000))
# the index through the trigger guard (idren2023 2026-10-24: the trigger finger in the guard's ring): indexring=x0,x1,
# y0,y1 (mm, the ring's box in the weapon's start place, as grip=) gives the ring's centre (its vertices' middle); the
# index's knuckle (curl x -60..40 added, swing z -45..25) and outer joints (0 to 1.3 of the start: the ring lies ahead) put its last joint as near
# that centre as they go, nothing through the back of the finger (its pad may press on the trigger)
if opt.get('indexring'):
    rx0, rx1, ry0, ry1 = (float(v) / 1000 for v in opt['indexring'].split(','))
    ring = [w.matrix_world @ v.co for v in w.data.vertices if rx0 <= v.co.x <= rx1 and ry0 <= v.co.y <= ry1]
    ring_centre = sum(ring, Vector()) / len(ring)
    start = {n: tuned.get(n, (0, 0, 0)) for n in DIGITS['index']}
    # the finger through the ring (idren2023 2026-10-24): the ring's centre as near the finger's line (its middle joint
    # to its tip) as it goes, the end joint past the ring (the pad on the trigger inside it); the knuckle turned (curl
    # -60..40, swing -45..25 deg) and the two outer joints curled on their own (0..-70 deg)
    def line_distance(q, a_, b_):
        t = max(0.0, min(1.0, (q - a_).dot(b_ - a_) / max((b_ - a_).length_squared, 1e-12)))
        return (q - (a_ + (b_ - a_) * t)).length
    index_base = [v.index for v in hand.data.vertices
                  if sum(g.weight for g in v.groups if names[g.group] in ('index_1', 'index_2')) > 0.5]
    found = None
    for kx in range(-60, 41, 5):
        for kz in range(-45, 26, 5):
            for c2 in range(0, -71, -5):
                for c3 in range(0, -71, -5):
                    trial = dict(tuned)
                    x_, y_, z_ = start['index_1']
                    if x_ + kx > 0:
                        continue  # the knuckle never bent backwards
                    if c3 < c2 - 15:
                        continue  # the end joint at most 15 deg more bent than the middle (idren2023: the tip bent too much)
                    trial['index_1'] = (x_ + kx, y_, z_ + kz)
                    trial['index_2'] = (c2, 0, 0)
                    trial['index_3'] = (c3, 0, 0)
                    pts, _ = apply(trial)
                    j2 = rig.matrix_world @ P['index_2'].head
                    j3 = rig.matrix_world @ P['index_3'].head
                    tip = rig.matrix_world @ P['index_3'].tail
                    d = min(line_distance(ring_centre, j2, j3), line_distance(ring_centre, j3, tip))
                    # through the ring (within 2 mm), then the tip curled down onto the trigger inside the ring (the
                    # tip nearest the ring's centre, not sticking out past it); the last segment may press on the
                    # trigger, the first two must stay clear of the gun
                    score = (round(d * 1000 / 2), round((tip - ring_centre).length * 1000 / 2))
                    if found is not None and score >= found[5]:
                        continue
                    if outer_crossings(pts, index_base, tree, AXIS0 + move, AXIS_DIR):
                        continue
                    found = (d, trial, kx, kz, (c2, c3), score)
    if found:
        for n in DIGITS['index']:
            tuned[n] = tuple(round(v) for v in found[1][n])
        print('TUNE index in the ring: knuckle %+d curl, %+d swing, outer joints %s deg; the finger passes %.1f mm from '
              "the ring's centre (%d ring vertices)" % (found[2], found[3], found[4], found[0] * 1000, len(ring)))
    else:
        print('TUNE index in the ring: no pose without the gun through the finger')
# the thumb round the handle (idren2023 2026-10-24: on the other side of the handle from the palm, as a right thumb lies
# along the frame's left side): its base turned (x -40..30, z -40..40 deg) and its two outer joints curled (0.5 to 2.0
# of the start), no crossings; the tip as far past the grip's axis away from the palm as it goes (at least a quarter
# of the grip's radius), then resting nearest the gun
if 'thumbwrap' in args:
    jp = os.path.join('re', 'body', 'grip-weap_%s.json' % WEAP)
    g_ = json.load(open(jp))
    axis_point = AXIS0 + move
    axis_dir = AXIS_DIR
    grip_pts = [w.matrix_world @ v.co for i, v in enumerate(w.data.vertices) if i in GRIP_IDS]
    radius = sum(((q - axis_point) - axis_dir * (q - axis_point).dot(axis_dir)).length for q in grip_pts) / len(grip_pts)
    tip_bone = P['thumb_3']
    base_pose = {n: tuned.get(n, (0, 0, 0)) for n in DIGITS['thumb']}
    found = None
    # where a right thumb lies on a pistol grip: on the far side from the palm (past the axis by the radius and 8 mm),
    # toward the grip's top (the frame: 20 mm toward the thumb along the axis) and a little forward (15 mm)
    target = axis_point - Vector((0, 0, 1)) * (radius + 0.008) - axis_dir * 0.020 + Vector((0, 1, 0)) * 0.015
    if opt.get('thumbto'):
        # thumbto=<finger>: the thumb's tip meets that finger's tip (idren2023 2026-10-24: the canister held in a ring of
        # the thumb and the middle finger)
        apply(tuned)
        target = rig.matrix_world @ P[opt['thumbto'] + '_3'].tail
    for bx in range(-40, 61, 10):
        for bz in range(-60, 61, 10):
            for k in (50, 75, 100, 125, 150, 175, 200):
                c_ = k / 100
                trial = dict(tuned)
                x_, y_, z_ = base_pose['thumb_1']
                trial['thumb_1'] = (x_ + bx, y_, z_ + bz)
                trial['thumb_2'] = tuple(v * c_ for v in base_pose['thumb_2'])
                trial['thumb_3'] = tuple(v * c_ for v in base_pose['thumb_3'])
                pts, _ = apply(trial)
                tip = rig.matrix_world @ tip_bone.tail
                rel = tip - axis_point
                rel -= axis_dir * rel.dot(axis_dir)
                far = -rel.z      # past the axis away from the palm (the palm is at +z of the grip)
                if far < radius * 0.25:
                    continue
                if outer_crossings(pts, own['thumb'], tree, axis_point, axis_dir):
                    continue
                cnt, near = crossings(pts, own['thumb'], tree)
                if False:
                    continue
                key = (-round((tip - target).length * 1000 / 2), -near)
                if found is None or key > found[0]:
                    found = (key, trial, far, near, bx, bz, c_)
    if found:
        for n in DIGITS['thumb']:
            tuned[n] = tuple(round(v) for v in found[1][n])
        print('TUNE thumb wrapped: base %+d x, %+d z, outer joints x%.2f; tip %.0f mm past the grip axis (radius %.0f), '
              '%.0f mm from the target, nearest %.1f mm' % (found[4], found[5], found[6], found[2] * 1000, radius * 1000,
                                                           -found[0][0] * 2, found[3] * 1000))
    else:
        print('TUNE thumb wrapped: no place past the axis without crossings (left as tuned)')
# turnclear (idren2023 2026-10-24: the hand turned on the object rather than the object moved): the weapon turned about
# its grip axis point (x, y, z -25..25 deg, 5 deg steps), the smallest turn with nothing of any digit through its
# outside in the pose as it is (the little finger may swing -30..30 and curl its knuckle 0.6..1.4 to fit)
if 'turnclear' in args:
    pts0, _ = apply(tuned)
    M0 = w.matrix_world.copy()
    piv = AXIS0 + move
    best_t = None
    pinky_options = [(sw, ka) for sw in (0, -10, 10, -20, 20, -30, 30) for ka in (1.0, 0.8, 1.2, 0.6, 1.4)]
    for rx in range(-25, 26, 5):
        for ry in range(-25, 26, 5):
            for rz in range(-25, 26, 5):
                cost = abs(rx) + abs(ry) + abs(rz)
                if best_t is not None and cost >= best_t[0]:
                    continue
                R_ = (Matrix.Rotation(math.radians(rz), 4, 'Z') @ Matrix.Rotation(math.radians(ry), 4, 'Y') @
                      Matrix.Rotation(math.radians(rx), 4, 'X'))
                w.matrix_world = Matrix.Translation(piv) @ R_ @ Matrix.Translation(-piv) @ M0
                t_ = weapon_tree()
                ad = (R_.to_3x3() @ AXIS_DIR).normalized()
                if any(outer_crossings(pts0, own[f], t_, piv, ad) for f in ('index', 'middle', 'ring', 'thumb')):
                    continue
                # the little finger may swing and curl its knuckle to fit
                fit = None
                for sw, ka in pinky_options:
                    trial = dict(tuned)
                    x_, y_, z_ = tuned.get('pinky_1', (0, 0, 0))
                    trial['pinky_1'] = (x_ * ka, y_, z_ + sw)
                    pts_, _ = apply(trial)
                    if not outer_crossings(pts_, own['pinky'], t_, piv, ad) and not outer_crossings(
                            pts_, own['ring'], t_, piv, ad):
                        fit = trial
                        break
                if fit is None:
                    continue
                best_t = (cost, rx, ry, rz, R_, fit)
    if best_t:
        _, rx, ry, rz, R_, fit = best_t
        tuned = {k: tuple(round(v) for v in d) for k, d in fit.items()}
        w.matrix_world = Matrix.Translation(piv) @ R_ @ Matrix.Translation(-piv) @ M0
        AXIS_DIR = (R_.to_3x3() @ AXIS_DIR).normalized()
        tree = weapon_tree()
        print('TUNE turnclear: the hand turned %+d %+d %+d deg about x, y, z on the object' % (rx, ry, rz))
    else:
        w.matrix_world = M0
        print('TUNE turnclear: no turn within 25 deg clears every digit')
# alignfingers (idren2023 2026-10-24: the index, middle and ring fingers lined up with each other): one curl for all
# three (knuckle -10..-75, middle joint -10..-85, end joint 0.7 of the middle's; no sideways swing), the most closed
# with nothing of any of them through the gun's outside
if 'alignfingers' in args:
    trio = ('index', 'middle', 'ring')
    found = None
    for k1 in range(-10, -76, -5):
        for k2 in range(-10, -86, -5):
            trial = dict(tuned)
            for f in trio:
                trial[f + '_1'] = (k1, 0, 0)
                trial[f + '_2'] = (k2, 0, 0)
                trial[f + '_3'] = (round(k2 * 0.7), 0, 0)
            score = -(k1 + k2)
            if found is not None and score <= found[0]:
                continue
            pts, _ = apply(trial)
            if any(outer_crossings(pts, own[f], tree, AXIS0 + move, AXIS_DIR) for f in trio):
                continue
            found = (score, trial, k1, k2)
    if found:
        tuned = dict(found[1])
        print('TUNE alignfingers: index, middle, ring at knuckle %d, middle joint %d, end joint %d deg' % (
            found[2], found[3], round(found[3] * 0.7)))
    else:
        print('TUNE alignfingers: no shared curl clear of the gun')
# clearout (idren2023 2026-10-24: keep the pose, remove the clipping seen from outside): every digit the gun shows through
# from outside gets the smallest change of its rotations that clears it: the knuckle's curl (0.4..1.8 of it), the
# outer joints' (0.4..2.2) and a sideways swing of the knuckle (-30..30 deg)
if 'clearout' in args:
    for f in ('index', 'middle', 'ring', 'pinky', 'thumb') * 2:  # twice: a neighbour's change can touch the web
        pts, _ = apply(tuned)
        if not outer_crossings(pts, own[f], tree, AXIS0 + move, AXIS_DIR):
            continue
        start = {n: tuned.get(n, (0, 0, 0)) for n in DIGITS[f]}
        found = None
        for ka in range(40, 181, 10):
            for kb in range(40, 221, 15):
                for sw in range(-30, 31, 5):
                    cost = abs(ka - 100) / 100 + abs(kb - 100) / 100 + abs(sw) / 30
                    if found is not None and cost >= found[0]:
                        continue
                    trial = dict(tuned)
                    x_, y_, z_ = start[DIGITS[f][0]]
                    trial[DIGITS[f][0]] = (x_ * ka / 100, y_, z_ + sw)
                    for n in DIGITS[f][1:]:
                        trial[n] = tuple(v * kb / 100 for v in start[n])
                    pts, _ = apply(trial)
                    if outer_crossings(pts, own[f], tree, AXIS0 + move, AXIS_DIR):
                        continue
                    found = (cost, trial, ka, kb, sw)
        if found:
            for n in DIGITS[f]:
                tuned[n] = tuple(round(v) for v in found[1][n])
            print('TUNE clearout %s: knuckle x%.2f, outer joints x%.2f, swing %+d deg' % (f, found[2] / 100, found[3] / 100,
                                                                                    found[4]))
        else:
            print('TUNE clearout %s: no rotation clears it' % f)
after, _ = measure(tuned, tree, 'after')
# the clipping that shows: the gun through the outside (backs and sides) of each digit and of the hand
pts, _ = apply(tuned)
hand_ids_all = [i for i, v in enumerate(hand.data.vertices)
                if sum(g.weight for g in v.groups if names[g.group] in ('hand', 'thumb_1')) > 0.5]
outside = {f: outer_crossings(pts, own[f], tree, AXIS0 + move, AXIS_DIR) for f in own}
outside['hand'] = outer_crossings(pts, hand_ids_all, tree, AXIS0 + move, AXIS_DIR)
print('TUNE outside crossings (visible clipping): ' + ', '.join('%s %d' % kv for kv in outside.items()))
print('TUNEPOSE ' + repr({k: v for k, v in tuned.items() if any(v)}))
if 'save' in args:
    hb = rig.matrix_world @ rig.data.bones['hand'].matrix_local
    rel = hb.inverted() @ w.matrix_world
    jp = os.path.join('re', 'body', 'grip-weap_%s.json' % WEAP)
    g = json.load(open(jp))
    g['in_hand_bone'] = [list(r) for r in rel]
    g['axis_point'] = list(AXIS0 + move)
    g['axis_direction'] = list(AXIS_DIR)
    g['placement'] = 'stock, tuned (tools/body_tunegrip.py)'
    json.dump(g, open(jp, 'w'), indent=1)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
