"""Blender (blender -b --factory-startup <stock.blend> -P tools/body_fromstock.py -- <stock-object> <out.blend>
[hand-length-mm] [fit] [weights.txt] [skeleton=<blend>]): the hand built from the stock first-person hand itself (idren2023 2026-10-21), so its uvs are the
stock's by construction.

1. Clone the stock hand's glove faces (bone_r_wrist, the hands texture), move them into the hand frame (wrist at the
   origin, +y toward the middle knuckle, +x toward the pinky, +z the back of the hand) and scale them to a real hand
   (wrist to middle finger tip, default 195 mm).
2. Fit our skeleton (hand, three bones a finger, thumb 1-3) to it: finger joints at the centres of the stock's edge
   loops (rows of the uv pattern, PLAN 51 B), the thumb's MCP / IP at its strip's rows, its CMC low in the palm.
3. Weights from the uv pattern: a vertex's place on the pattern says which finger or the palm it belongs to and how
   far along; smooth blends across each joint row.
4. Pose the grip open into a relaxed rest (fingers straightened then curled 14 / 18 / 10 deg, the thumb out from
   the palm), apply it as the rest, and report the stretch (edge lengths against the stock's) and the texture's
   distortion (3D area over uv area per face, spread around the median) before and after."""
import bpy, bmesh, math, os, sys
from mathutils import Matrix, Quaternion, Vector

args = sys.argv[sys.argv.index('--') + 1:]
OBJ, OUT = args[0], args[1]
HAND_MM = next((float(a) for a in args[2:] if a.replace('.', '').isdigit()), 195.0)
scene = bpy.context.scene
stock = bpy.data.objects[OBJ]
skel = bpy.data.objects[OBJ + '_skeleton']
hg = stock.vertex_groups['bone_r_wrist'].index


def smooth(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


# ---- 1. clone ---------------------------------------------------------------------------------------------------
bm = bmesh.new()
bm.from_mesh(stock.data)
bm.transform(stock.matrix_world)
dl = bm.verts.layers.deform.active
keep = {f for f in bm.faces if f.material_index == 0 and all(hg in v[dl] for v in f.verts)}
bmesh.ops.delete(bm, geom=[f for f in bm.faces if f not in keep], context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
uvl = bm.loops.layers.uv.active
wrist = skel.matrix_world @ skel.data.bones['bone_r_wrist'].head_local


def texel(l):
    return l[uvl].uv.x * 256, (1 - l[uvl].uv.y) * 256


def centre(sel):
    pts = {v for v in bm.verts for l in v.link_loops if sel(*texel(l))}
    return sum((v.co for v in pts), Vector()) / len(pts) if pts else None


FINGERS = ['index', 'middle', 'ring', 'pinky']
# texel columns of each finger strip (back, palm), joint rows MCP / PIP / DIP (back from the stock's loops; palm the
# stock's loops), the row where the pointed tip starts on the back
BACK = {'index': ((87, 115), (125, 80, 46), 20), 'middle': ((55, 85), (126, 74, 41), 10),
        'ring': ((30, 53), (125, 91, 56), 28), 'pinky': ((1, 26), (126, 95, 73), 51)}
PALM = {'index': ((166, 187), (123, 80, 47)), 'middle': ((188, 210), (113, 74, 40)),
        'ring': ((212, 235), (116, 91, 57)), 'pinky': ((237, 255), (118, 94, 71))}
TIP_APEX = {'index': ((100, 6), (178, 12)), 'middle': ((70, 1), (198, 0)), 'ring': ((41, 17), (224, 22)),
            'pinky': ((15, 42), (246, 43))}
THUMB_X = (116, 167)
THUMB_ROWS = (150, 125)      # MCP (the strip's base), IP
THUMB_APEX = (143, 97)

verts = list(bm.verts)
joints = {}
for f in FINGERS:
    # each joint between its loop on the back strip and the same loop on the palm strip, only from this finger's
    # strips (a slice through the grip also caught the thumb web on the index and the ring finger on the pinky:
    # knuckles 33 / 19 / 10 mm apart, idren2023: index and pinky angles off)
    (x0, x1), rows, shoulder = BACK[f]
    (px0, px1), prows = PALM[f]
    cs = []
    for r, pr in zip(rows, prows):
        back = centre(lambda x, y: x0 <= x <= x1 and abs(y - r) < 3)
        palm = centre(lambda x, y: px0 <= x <= px1 and abs(y - pr) < 3)
        if back is None or palm is None:
            raise SystemExit('no loop at %s rows %d / %d (back %s palm %s)' % (f, r, pr, back, palm))
        cs.append((back + palm) / 2)
        if len(cs) == 1:
            mcp_back = back
    # the knuckle (MCP): the stock's palm loop there is not the back loop's partner (rows 113 / 116 / 118 against
    # 126 / 125 / 126); the back loop's centre pushed into the finger by half its thickness at the PIP
    back_pip = centre(lambda x, y: x0 <= x <= x1 and abs(y - rows[1]) < 3)
    cs[0] = mcp_back + (cs[1] - back_pip) * 1.1
    # the tip: the painted fingertip's apex (back and palm), 5 mm back inside toward the DIP (the farthest vertex
    # along the last segment found palm vertices on the curled fingers)
    (bx, by), (px, py) = TIP_APEX[f]
    apex = centre(lambda x, y: math.hypot(x - bx, y - by) < 3 or math.hypot(x - px, y - py) < 3)
    tip = apex + (cs[2] - apex).normalized() * 0.005 / 0.712
    # keep the tip bone in the finger's bending plane (through its first two segments): the painted apex's centre
    # lies off to one side on the pinky, which bent the tip outward once straightened (idren2023)
    n = (cs[1] - cs[0]).cross(cs[2] - cs[1])
    if n.length > 1e-6:
        n.normalize()
        v = tip - cs[2]
        L = v.length
        v -= n * v.dot(n)
        tip = cs[2] + v.normalized() * L
    joints[f] = cs + [tip]
thumb_mcp = centre(lambda x, y: THUMB_X[0] <= x <= THUMB_X[1] and abs(y - THUMB_ROWS[0]) < 2.5)
thumb_ip = centre(lambda x, y: THUMB_X[0] <= x <= THUMB_X[1] and abs(y - THUMB_ROWS[1]) < 2.5)
thumb_apex = centre(lambda x, y: math.hypot(x - THUMB_APEX[0], y - THUMB_APEX[1]) < 4)
thumb_tip = thumb_apex + (thumb_ip - thumb_apex).normalized() * 0.005 / 0.712

# hand frame and scale
y_axis = (joints['middle'][0] - wrist).normalized()
x_axis = joints['pinky'][0] - joints['index'][0]
x_axis = (x_axis - y_axis * x_axis.dot(y_axis)).normalized()
z_axis = x_axis.cross(y_axis)
back_pt = centre(lambda x, y: 1 <= x <= 115 and 150 < y < 200)
if (back_pt - wrist).dot(z_axis) < 0:
    x_axis, z_axis = -x_axis, -z_axis
mid_len = (joints['middle'][0] - wrist).length + sum((joints['middle'][i + 1] - joints['middle'][i]).length for i in range(3))
# the stock hand is 274 mm wrist to middle finger tip (tools/body_measure.py, its joint rings); the scale from that
# fixed length, not from these joints (their chain shortens as their placement changes)
k = HAND_MM / 274.0
to_hand = Matrix((x_axis, y_axis, z_axis))


def H(p):
    return (to_hand @ (p - wrist)) * k


# the stock -> hand frame transform, for placing the stock's weapons (hp_weapons) on our hand
import json
json.dump({'to_hand': [list(r) for r in to_hand], 'wrist': list(wrist), 'scale': k},
          open(os.path.join(os.path.dirname(os.path.abspath(OUT)), 'hand-frame-%s.json' % OBJ), 'w'))


for v in verts:
    v.co = H(v.co)
joints = {f: [H(p) for p in js] for f, js in joints.items()}
thumb_mcp, thumb_ip, thumb_tip = H(thumb_mcp), H(thumb_ip), H(thumb_tip)
# the thumb's CMC: low on the palm's thumb side, inside the thenar (anatomy; the stock has no joint there)
thumb_cmc = Vector((-0.020, 0.016, -0.008))
print('SCALE %.3f  middle MCP %s  thumb CMC %s MCP %s IP %s tip %s (mm)' % (
    k, tuple(round(c * 1000) for c in joints['middle'][0]), tuple(round(c * 1000) for c in thumb_cmc),
    tuple(round(c * 1000) for c in thumb_mcp), tuple(round(c * 1000) for c in thumb_ip), tuple(round(c * 1000) for c in thumb_tip)))

# ---- 3. weights from the pattern -----------------------------------------------------------------------------------
def chain(y, rows, names, band):
    """Along a strip: y (texel row, smaller = toward the tip) past each joint row blends into the next bone."""
    w = {names[0]: 1.0}
    for row, n0, n1 in zip(rows, names, names[1:]):
        t = smooth(row + band, row - band, y)
        if t <= 0:
            break
        carry = w.pop(n0)
        w[n0] = carry * (1 - t)
        w[n1] = carry * t
    return {n: x for n, x in w.items() if x > 0.001}


def corner_weights(x, y):
    if THUMB_X[0] <= x <= THUMB_X[1] and y < 205:
        # the thumb strip from the knuckle up; the column below it is the thenar, on the metacarpal
        return chain(y, (215, THUMB_ROWS[0], THUMB_ROWS[1]), ['hand', 'thumb_1', 'thumb_2', 'thumb_3'], 8)
    best = None
    for f in FINGERS:
        for (x0, x1), rows in ((BACK[f][0], BACK[f][1]), PALM[f]):
            dx = 0 if x0 <= x <= x1 else min(abs(x - x0), abs(x - x1))
            if best is None or dx < best[0]:
                best = (dx, f, rows)
    dx, f, rows = best
    w = chain(y, rows, ['hand', f + '_1', f + '_2', f + '_3'], 6)
    if dx > 0 and y > rows[0] - 6:
        # between the strips (webs, palm) the finger's pull fades sideways
        fade = 1 - smooth(0, 6, dx)
        w = {n: (x * fade if n != 'hand' else x) for n, x in w.items()}
        w['hand'] = w.get('hand', 0) + (1 - sum(w.values()))
    return w


vw = []
for v in verts:
    acc = {}
    for l in v.link_loops:
        for n, x in corner_weights(*texel(l)).items():
            acc[n] = acc.get(n, 0) + x / len(v.link_loops)
    vw.append(acc)

me = bpy.data.meshes.new('r_hand_stock')
bm.verts.layers.deform.remove(dl)  # the stock's own weights (all on its wrist bone) must not come along
bm.to_mesh(me)
bm.free()
for p in me.polygons:
    p.use_smooth = True
for o in list(scene.objects):
    o.hide_render = True
obj = bpy.data.objects.new('r_hand_stock', me)
scene.collection.objects.link(obj)
me.materials.append(stock.data.materials[0])
stock_edges = [(e.vertices[0], e.vertices[1], (me.vertices[e.vertices[0]].co - me.vertices[e.vertices[1]].co).length)
               for e in me.edges]

# ---- 2. skeleton --------------------------------------------------------------------------------------------------
arm_data = bpy.data.armatures.new('r_hand_stock_skeleton')
rig = bpy.data.objects.new('r_hand_stock_skeleton', arm_data)
scene.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
lat = Vector((1, 0, 0))


def bone(name, head, tail, parent=None):
    b = arm_data.edit_bones.new(name)
    b.head, b.tail = head, tail
    d = (tail - head).normalized()
    b.align_roll(lat.cross(d))
    if parent:
        b.parent = arm_data.edit_bones[parent]
    return b


bone('hand', Vector(), joints['middle'][0])
for f in FINGERS:
    js = joints[f]
    for i in range(3):
        bone('%s_%d' % (f, i + 1), js[i], js[i + 1], 'hand' if i == 0 else '%s_%d' % (f, i))
for i, (a, b) in enumerate(((thumb_cmc, thumb_mcp), (thumb_mcp, thumb_ip), (thumb_ip, thumb_tip))):
    bone('thumb_%d' % (i + 1), a, b, 'hand' if i == 0 else 'thumb_%d' % i)
# bones edited by hand in Blender ('skeleton=<blend>', made by tools/body_skeledit.py): heads and tails from there,
# in the stock grip (the same hand frame), before anything else uses them
SKEL = next((a_[9:] for a_ in args[2:] if a_.startswith('skeleton=')), None)
if SKEL:
    with bpy.data.libraries.load(os.path.abspath(SKEL)) as (s_, d_):
        d_.armatures = ['r_hand_stock_skeleton']
    edited = d_.armatures[0]
    for eb in arm_data.edit_bones:
        b_ = edited.bones.get(eb.name)
        if b_:
            eb.head, eb.tail = b_.head_local.copy(), b_.tail_local.copy()
    joints = {f: [arm_data.edit_bones['%s_1' % f].head.copy()] + [arm_data.edit_bones['%s_%d' % (f, k)].tail.copy()
                                                                   for k in (1, 2, 3)] for f in FINGERS}
    thumb_cmc, thumb_mcp = arm_data.edit_bones['thumb_1'].head.copy(), arm_data.edit_bones['thumb_1'].tail.copy()
    thumb_ip, thumb_tip = arm_data.edit_bones['thumb_2'].tail.copy(), arm_data.edit_bones['thumb_3'].tail.copy()
    # his bones' x axes (the bending axes as he left them), carried into the rest below in place of computed ones
    edited_x = {b_.name: b_.matrix_local.to_3x3().col[0].normalized() for b_ in edited.bones}
    print('SKELETON bones from', SKEL)
bpy.ops.object.mode_set(mode='OBJECT')
for b in arm_data.bones:
    obj.vertex_groups.new(name=b.name)
if 'fit' in args[2:]:
    # skeleton fitted into the stock's grip, no weights, no posing (weights are set by hand, PLAN 51 B)
    obj.parent = rig
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
    print('FIT saved', OUT)
    sys.exit(0)
if any(a.endswith('.txt') for a in args[2:]):
    # weights set by hand, copied as written (tools/body_weights_<unit>_r.txt)
    table = next(a for a in args[2:] if a.endswith('.txt'))
    vw = [None] * len(vw)
    for line in open(table):
        line = line.split('#')[0].strip()
        if line.startswith(('@', '+', '!', '^', '%')):
            continue  # vertex placements, extra splits and split-vertex weights, applied below
        if line:
            ws, ids = line.split('=')
            for i in ids.split():
                vw[int(i)] = {b_.split(':')[0]: float(b_.split(':')[1]) for b_ in ws.split()}
    assert all(w is not None for w in vw), 'a vertex has no weights in ' + table
for i, w in enumerate(vw):
    total = sum(w.values()) or 1
    for n, x in w.items():
        obj.vertex_groups[n].add([i], x / total, 'REPLACE')
obj.parent = rig
mod = obj.modifiers.new('skeleton', 'ARMATURE')
mod.object = rig

# ---- 4. open the grip into a relaxed rest -------------------------------------------------------------------------
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='POSE')
for pb in rig.pose.bones:
    pb.rotation_mode = 'QUATERNION'


def aim(pb, want):
    """Turn a pose bone (about its head) so it points along want (armature space)."""
    bpy.context.view_layer.update()
    have = (pb.tail - pb.head).normalized()
    R = have.rotation_difference(want.normalized())
    W = pb.matrix.to_quaternion()
    pb.rotation_quaternion = pb.rotation_quaternion @ W.inverted() @ R @ W


P = rig.pose.bones
# relaxed spread (deg, + toward the pinky): fingers nearly parallel, not fanned along the wrist -> knuckle lines
# (that pointed the index 17 deg toward the thumb, idren2023: its angles looked off)
SPLAY = {'index': -7, 'middle': -1, 'ring': 4, 'pinky': 9}
# a relaxed hand's cascade: each finger curls a little more than the one before it (MCP, PIP, DIP deg)
CURL = {'index': (10, 14, 8), 'middle': (14, 19, 10), 'ring': (18, 24, 12), 'pinky': (22, 28, 14)}
CONVERGE = 4  # deg: curled fingertips lean toward the middle finger, as a real hand's


def finger_lat(f):
    """A finger's own bending axis: square to its spread direction, tilted so the curled tip converges."""
    return Matrix.Rotation(math.radians(-SPLAY[f] - CONVERGE * (SPLAY[f] > 0) + CONVERGE * (SPLAY[f] < 0)), 3, 'Z') @ Vector((1, 0, 0))


for f in FINGERS:
    base_dir = Matrix.Rotation(math.radians(-SPLAY[f]), 3, 'Z') @ Vector((0, 1, 0))
    want = Matrix.Rotation(-math.radians(CURL[f][0]), 3, finger_lat(f)) @ base_dir
    aim(P[f + '_1'], want)
    for i in (2, 3):
        bpy.context.view_layer.update()
        parent_dir = (P['%s_%d' % (f, i - 1)].tail - P['%s_%d' % (f, i - 1)].head).normalized()
        aim(P['%s_%d' % (f, i)], Matrix.Rotation(-math.radians(CURL[f][i - 1]), 3, finger_lat(f)) @ parent_dir)
# the thumb out from the palm: forward, out and toward the palm, the tip phalanges straighter
aim(P['thumb_1'], Vector((-0.55, 0.72, -0.42)))
aim(P['thumb_2'], Vector((-0.38, 0.84, -0.38)))
aim(P['thumb_3'], Vector((-0.26, 0.90, -0.34)))
bpy.ops.object.mode_set(mode='OBJECT')
bpy.context.view_layer.update()


def report(label, mesh):
    ratios = sorted((mesh.vertices[a].co - mesh.vertices[b].co).length / max(l0, 1e-9) for a, b, l0 in stock_edges)
    n = len(ratios)
    uvd = mesh.uv_layers.active.data
    dens = []
    for p in mesh.polygons:
        u = [uvd[i].uv for i in p.loop_indices]
        auv = sum(abs((u[i] - u[0]).cross(u[i + 1] - u[0])) / 2 for i in range(1, len(u) - 1))
        if auv > 1e-9:
            dens.append(p.area / auv)
    dens.sort()
    med = dens[len(dens) // 2]
    print('%-6s edges vs stock: p1 %.2f p99 %.2f max %.2f | texture (3D/uv area over median): p5 %.2f p95 %.2f max %.2f' % (
        label, ratios[int(n * 0.01)], ratios[int(n * 0.99)], ratios[-1], dens[int(len(dens) * 0.05)] / med,
        dens[int(len(dens) * 0.95)] / med, dens[-1] / med))


report('STOCK', me)
bpy.context.view_layer.objects.active = obj
bpy.ops.object.modifier_apply(modifier='skeleton')
# the base pose's shape, frozen (idren2023 2026-10-21): opened through the weights once and kept in
# re/body/rest-<object>.json (game-derived: in re/), so later weight edits only change how the hand bends;
# 'refreeze' opens it through the current weights again and overwrites the file
import json
REST_FILE = os.path.join(os.path.dirname(os.path.abspath(OUT)), 'rest-%s.json' % OBJ)
if os.path.exists(REST_FILE) and 'refreeze' not in args[2:]:
    for v, co in zip(me.vertices, json.load(open(REST_FILE))):
        v.co = co
    print('REST frozen shape from', REST_FILE)
else:
    json.dump([list(v.co) for v in me.vertices], open(REST_FILE, 'w'))
    print('REST shape frozen to', REST_FILE)
report('OPENED', me)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='POSE')
# the thumb's bending axes from the stock grip itself (idren2023 2026-10-21; the texture halves gave an axis the stock's
# own curled grip needed 37-40 deg of sideways turn to reach): each joint bends in the plane of its two bones in the
# grip (n = parent direction x child direction), carried with the child bone from the grip into the opened rest; the
# base bone takes the knuckle's
grip_dir = {'thumb_1': (thumb_mcp - thumb_cmc).normalized(), 'thumb_2': (thumb_ip - thumb_mcp).normalized(),
            'thumb_3': (thumb_tip - thumb_ip).normalized()}
grip_axis = {'thumb_2': grip_dir['thumb_1'].cross(grip_dir['thumb_2']).normalized(),
             'thumb_3': grip_dir['thumb_2'].cross(grip_dir['thumb_3']).normalized()}
grip_axis['thumb_1'] = grip_axis['thumb_2']
bpy.context.view_layer.update()
thumb_grip_axis = {}
for n_, ax_ in grip_axis.items():
    pb_ = rig.pose.bones[n_]
    local = pb_.bone.matrix_local.to_3x3().inverted() @ ax_      # in the bone's frame at the grip
    thumb_grip_axis[n_] = (pb_.matrix.to_3x3() @ local).normalized()  # the same, carried into the opened pose
edited_rest_x = {}
if SKEL:
    for pb_ in rig.pose.bones:
        local = pb_.bone.matrix_local.to_3x3().inverted() @ edited_x[pb_.name]
        edited_rest_x[pb_.name] = (pb_.matrix.to_3x3() @ local).normalized()
print('THUMB grip bends: knuckle %.0f deg, tip joint %.0f deg' % (
    math.degrees(grip_dir['thumb_1'].angle(grip_dir['thumb_2'])), math.degrees(grip_dir['thumb_2'].angle(grip_dir['thumb_3']))))
bpy.ops.pose.armature_apply(selected=False)
# bone rolls for the new rest: every finger bone's x axis is its bending axis (the hand's side axis), the thumb's
# turned with its pronation, as tools/body_hand.py and the pose test expect (rotation x < 0 curls toward the palm)
# the thumb's bending axes from the glove (idren2023: is the tip rotating on the proper axis? its rolls came from the
# earlier hand: off by 27 / 78 deg): per segment the direction from the thumb strip's palm half to its back half
# (texels x 118-168, rows 95-152; back = x < 143), square to the bone; the bending axis is bone x that, signed so a
# negative x rotation curls toward the palm half; the base bone takes the middle bone's
bm_t = bmesh.new()
bm_t.from_mesh(me)
uv_t = bm_t.loops.layers.uv.active
thumb_axis = {}
for n in ('thumb_2', 'thumb_3'):
    b_ = arm_data.bones[n]
    h_, t_ = b_.head_local, b_.tail_local
    d_ = (t_ - h_).normalized()
    halves = ([], [])
    for v in bm_t.verts:
        q = v.co - h_
        if not 0.0 <= q.dot(d_) / (t_ - h_).length <= 1.0 or (q - d_ * q.dot(d_)).length > 0.025:
            continue
        for l in v.link_loops:
            x, y = l[uv_t].uv.x * 256, (1 - l[uv_t].uv.y) * 256
            if 118 <= x <= 168 and 95 <= y <= 152:
                halves[0 if x < 143 else 1].append(v.co.copy())
    back = sum(halves[0], Vector()) / len(halves[0]) - sum(halves[1], Vector()) / len(halves[1])
    back = (back - d_ * back.dot(d_)).normalized()
    thumb_axis[n] = (d_.cross(back).normalized(), back)
# the middle segment has only ~17 strip vertices (one of them moved by a hand placement): all three bones take the
# tip segment's (~107) axis, squared to each bone below; a thumb's knuckle and tip joint bend about near-parallel axes
thumb_axis['thumb_1'] = thumb_axis['thumb_2'] = thumb_axis['thumb_3']
bm_t.free()
STRIPS = {'index': ((87, 115), (166, 187)), 'middle': ((55, 85), (188, 210)), 'ring': ((30, 53), (212, 235)),
          'pinky': ((1, 26), (237, 255))}
bm_t = bmesh.new()
bm_t.from_mesh(me)
uv_t = bm_t.loops.layers.uv.active
finger_axis = {}
for f_, ((bx0, bx1), (px0, px1)) in STRIPS.items():
    axes, backs = [], []
    for k_ in (1, 2, 3):
        b_ = arm_data.bones['%s_%d' % (f_, k_)]
        h_, t_ = b_.head_local, b_.tail_local
        d_ = (t_ - h_).normalized()
        bk_, pm_ = [], []
        for v in bm_t.verts:
            q = v.co - h_
            if not -0.1 <= q.dot(d_) / (t_ - h_).length <= 1.1 or (q - d_ * q.dot(d_)).length > 0.02:
                continue
            for l in v.link_loops:
                x = l[uv_t].uv.x * 256
                if bx0 <= x <= bx1:
                    bk_.append(v.co.copy())
                elif px0 <= x <= px1:
                    pm_.append(v.co.copy())
        back = sum(bk_, Vector()) / len(bk_) - sum(pm_, Vector()) / len(pm_)
        back = (back - d_ * back.dot(d_)).normalized()
        axes.append(d_.cross(back).normalized())
        backs.append(back)
        finger_axis['%s_%d' % (f_, k_)] = (axes[-1], back)
    ax = sum(axes, Vector()).normalized()
    print('FINGER AXIS %-6s segments spread %.1f deg around their mean' % (f_, max(math.degrees(a_.angle(ax)) for a_ in axes)))
bm_t.free()
bpy.ops.object.mode_set(mode='EDIT')
for eb in arm_data.edit_bones:
    d = (eb.tail - eb.head).normalized()
    if SKEL:
        # rolls as idren2023 left them (his x axes, carried from the grip into the rest)
        ax = edited_rest_x[eb.name]
        ax = (ax - d * ax.dot(d)).normalized()
        z_ = ax.cross(d).normalized()
        eb.align_roll(z_)
        if eb.matrix.to_3x3().col[0].dot(ax) < 0:
            eb.align_roll(-z_)
        continue
    if eb.name.startswith('thumb'):
        # x = -n (n from the grip: a turn of +a about n curls the child toward where the grip has it, so a turn of
        # -a about x does: x < 0 curls palm-ward, as on the fingers)
        ax = -thumb_grip_axis[eb.name]
        ax = (ax - d * ax.dot(d)).normalized()
        z_ = ax.cross(d).normalized()
        eb.align_roll(z_)
        if eb.matrix.to_3x3().col[0].dot(ax) < 0:
            eb.align_roll(-z_)
    elif eb.name == 'hand':
        eb.align_roll(Vector((0, 0, 1)))
    else:
        # each bone's bending axis from its own glove segment: the back strip against the palm strip (index and pinky
        # are rolled along their length in the stock: their segments' axes differ by 17-21 deg, middle / ring by 3);
        # x < 0 curls palm-ward (idren2023: are all the bones rotating on their proper axis? the spread-based guess was
        # 3-26 deg off)
        ax, back = finger_axis[eb.name]
        ax = (ax - d * ax.dot(d)).normalized()
        z_ = ax.cross(d).normalized()
        eb.align_roll(z_)
        if (-(eb.matrix.to_3x3().col[0].cross(d))).dot(-back) < 0:
            eb.align_roll(-z_)
# a two-bone thumb ('%two_bone_thumb' in the table; idren2023 2026-10-21): the base and middle bone merged into one,
# from the thumb's base joint to its tip joint (the knuckle no longer bends: its ring kept sinking and shearing); the
# tip bone hangs from it; the middle bone's weights go to the base bone
TWO_BONE_THUMB = any(l.strip().startswith('%two_bone_thumb') for a_ in args[2:] if a_.endswith('.txt') for l in open(a_))
if TWO_BONE_THUMB:
    E = arm_data.edit_bones
    E['thumb_1'].tail = E['thumb_2'].tail.copy()
    E['thumb_3'].parent = E['thumb_1']
    d = (E['thumb_1'].tail - E['thumb_1'].head).normalized()
    ax = -thumb_grip_axis['thumb_1']
    ax = (ax - d * ax.dot(d)).normalized()
    z_ = ax.cross(d).normalized()
    E['thumb_1'].align_roll(z_)
    if E['thumb_1'].matrix.to_3x3().col[0].dot(ax) < 0:
        E['thumb_1'].align_roll(-z_)
    E.remove(E['thumb_2'])
bpy.ops.object.mode_set(mode='OBJECT')
if TWO_BONE_THUMB:
    g1, g2 = obj.vertex_groups['thumb_1'], obj.vertex_groups['thumb_2']
    for v in me.vertices:
        w2 = next((g.weight for g in v.groups if g.group == g2.index), 0.0)
        if w2:
            w1 = next((g.weight for g in v.groups if g.group == g1.index), 0.0)
            g1.add([v.index], w1 + w2, 'REPLACE')
    obj.vertex_groups.remove(g2)
    print('THUMB two bones: base joint to tip joint, tip')
# vertex placements set by hand in the weights table ('@v = a b ..': v to the mean of those vertices, in the rest)
for a_ in args[2:]:
    if a_.endswith('.txt'):
        for line in open(a_):
            line = line.split('#')[0].strip()
            if line.startswith('@'):
                v_, ids = line[1:].split('=')
                ids = [int(x) for x in ids.split()]
                me.vertices[int(v_)].co = sum((me.vertices[i].co for i in ids), Vector()) / len(ids)
                print('PLACED', v_.strip(), 'at the mean of', ids)
if 'split' in args[2:]:
    # more geometry for cleaner skinning (idren2023 2026-10-21), the base pose's texture unchanged: every edge split at
    # its midpoint, no smoothing, so the new faces lie on the old ones and their uvs split alike; the original
    # vertices keep their numbers (the hand-set table still applies), each new one takes the mean of its edge's two
    # hand-set weights
    before = [(v.co.copy()) for v in me.vertices]
    bm = bmesh.new()
    bm.from_mesh(me)
    dl2 = bm.verts.layers.deform.active
    from mathutils.kdtree import KDTree
    far = [0.0]

    def split_once(edges):
        """Split these edges at their midpoints; each new vertex gets the mean of its edge's two ends' weights."""
        mids = []
        for e in edges:
            a_, b_ = e.verts
            w = {}
            for u in (a_, b_):
                for g, x in u[dl2].items():
                    w[g] = w.get(g, 0) + x / 2
            mids.append(((a_.co + b_.co) / 2, w))
        kd = KDTree(len(mids))
        for i_, (c_, w_) in enumerate(mids):
            kd.insert(c_, i_)
        kd.balance()
        # the texture before the split: every triangle's 3D corners and uvs, to restore the mapping exactly after it
        # (a face cut on only some edges took a seam vertex's uv from the wrong side)
        from mathutils.bvhtree import BVHTree
        from mathutils.geometry import barycentric_transform
        uvl2 = bm.loops.layers.uv.active
        tris = [([l.vert.co.copy() for l in f.loops], [l[uvl2].uv.to_3d() for l in f.loops]) for f in bm.faces]
        tree = BVHTree.FromPolygons([c for c in (t[0] for t in tris) for c in c],
                                    [[3 * i + k for k in range(3)] for i in range(len(tris))])
        n_old = len(bm.verts)
        bmesh.ops.subdivide_edges(bm, edges=edges, cuts=1, use_grid_fill=True)
        bmesh.ops.triangulate(bm, faces=bm.faces[:])
        for f in bm.faces:
            loc, nrm, i_, dist = tree.find_nearest(f.calc_center_median())
            co, uv = tris[i_]
            for l in f.loops:
                u = barycentric_transform(l.vert.co, co[0], co[1], co[2], uv[0], uv[1], uv[2])
                l[uvl2].uv = (u.x, u.y)
        bm.verts.ensure_lookup_table()
        for v in bm.verts[n_old:]:
            c_, i_, d_ = kd.find(v.co)
            far[0] = max(far[0], d_)
            v[dl2].clear()
            for g, x in mids[i_][1].items():
                v[dl2][g] = x

    extra = []
    for a_ in args[2:]:
        if a_.endswith('.txt'):
            for line in open(a_):
                line = line.split('#')[0].strip()
                if line.startswith('+'):
                    extra += [int(x) for x in line[1:].split()]
    if extra:
        bm.verts.ensure_lookup_table()
        faces = {f for i_ in extra for f in bm.verts[i_].link_faces}
        split_once(list({e for f in faces for e in f.edges}))
        print('SPLIT extra around', extra, ':', len(faces), 'faces')
    split_once(bm.edges[:])
    far = far[0]
    # the joint rule ('%joint_rule' in the table; idren2023 2026-10-22): every split vertex on one finger (or the thumb)
    # weighted by where it lies along its finger relative to the joint pivots (blend half-widths: knuckle 8 mm, middle
    # joint 5, end joint 4, thumb joints 6); on the thumb's knuckle the palm side keeps part on the hand (15 % at 10-18
    # mm toward the palm, 30 % beyond, faded out from -6 / +4 mm to -16 / +18); webs (two fingers) keep their average
    JOINT_RULE = any(l.strip().startswith('%joint_rule') for a_ in args[2:] if a_.endswith('.txt') for l in open(a_))
    if JOINT_RULE:
        bm.verts.ensure_lookup_table()
        gname = {g.index: g.name for g in obj.vertex_groups}
        gidx = {v_: k_ for k_, v_ in gname.items()}
        BB = {b_.name: b_ for b_ in arm_data.bones}
        HALF = {1: 0.008, 2: 0.005, 3: 0.004}

        def smooth01(t):
            t = max(0.0, min(1.0, t))
            return t * t * (3 - 2 * t)

        def along(v_, bone):
            b_ = BB[bone]
            return (v_.co - b_.head_local).dot((b_.tail_local - b_.head_local).normalized())

        b2 = BB['thumb_2']
        t_ax = (b2.tail_local - b2.head_local).normalized()
        t_palm = -b2.matrix_local.to_3x3().col[2].normalized()   # his rolls: z = the back (nail side)
        n_rule = 0
        for v in bm.verts[197:]:
            w = {gname[g]: x for g, x in v[dl2].items() if x > 0.001}
            digits = {n_.split('_')[0] for n_ in w if n_ != 'hand'}
            if len(digits) != 1:
                continue
            # only vertices already between two bones of one joint (their edge ends straddle it): elsewhere the
            # averaged weights stay, so they keep matching their neighbours (the webs, the palm)
            if len([x for x in w.values() if x >= 0.05]) < 2:
                continue
            f_ = digits.pop()
            new = {}
            if f_ == 'thumb':
                a_ = along(v, 'thumb_2') * 1000
                if a_ < -16 and 'thumb_2' not in w and 'thumb_3' not in w:
                    continue
                share = {'thumb_1': 1.0}
                for nxt, prev in (('thumb_2', 'thumb_1'), ('thumb_3', 'thumb_2')):
                    t_ = smooth01((along(v, nxt) + 0.006) / 0.012)
                    c_ = share.pop(prev)
                    share[prev], share[nxt] = c_ * (1 - t_), c_ * t_
                    if t_ < 1:
                        break
                side = (v.co - b2.head_local - t_ax * (v.co - b2.head_local).dot(t_ax)).dot(t_palm) * 1000
                fade = 1.0 if -6 <= a_ <= 4 else (max(0.0, (a_ + 16) / 10) if a_ < -6 else max(0.0, (18 - a_) / 14))
                h_ = (0.3 if side >= 18 else (0.15 if side >= 10 else 0.0)) * fade
                h_ = max(h_, w.get('hand', 0.0) if a_ <= 10 else 0.0)
                new = {n_: x * (1 - h_) for n_, x in share.items()}
                new['hand'] = h_
            else:
                share = {'hand': 1.0}
                for k_ in (1, 2, 3):
                    nxt, prev = '%s_%d' % (f_, k_), ('hand' if k_ == 1 else '%s_%d' % (f_, k_ - 1))
                    t_ = smooth01((along(v, nxt) + HALF[k_]) / (2 * HALF[k_]))
                    c_ = share.pop(prev)
                    share[prev], share[nxt] = c_ * (1 - t_), c_ * t_
                    if t_ < 1:
                        break
                new = share
            v[dl2].clear()
            for n_, x in new.items():
                if x > 0.005:
                    v[dl2][gidx[n_]] = x
            n_rule += 1
        print('SPLIT joint rule on', n_rule, 'split vertices')
    # weights set by hand on split vertices ('!bone:w .. = ids' in the table), over their averaged ones
    bm.verts.ensure_lookup_table()
    gi = {g.name: g.index for g in obj.vertex_groups}
    n_set = 0
    for a_ in args[2:]:
        if a_.endswith('.txt'):
            for line in open(a_):
                line = line.split('#')[0].strip()
                if line.startswith('!'):
                    ws, ids = line[1:].split('=')
                    w = {}
                    for b_ in ws.split():
                        n_, x_ = b_.split(':')
                        if TWO_BONE_THUMB and n_ == 'thumb_2':
                            n_ = 'thumb_1'
                        w[gi[n_]] = w.get(gi[n_], 0) + float(x_)
                    for i_ in ids.split():
                        v = bm.verts[int(i_)]
                        v[dl2].clear()
                        for g, x in w.items():
                            v[dl2][g] = x
                        n_set += 1
    if n_set:
        print('SPLIT weights set by hand on', n_set, 'split vertices')
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(me)
    bm.free()
    moved = max((me.vertices[i].co - c).length for i, c in enumerate(before))
    print('SPLIT new vertices matched to their edge midpoint within %.2g m;' % far, 'tris', len(me.polygons), 'verts', len(me.vertices), 'original vertices moved by at most %.6f m' % moved)
mod = obj.modifiers.new('skeleton', 'ARMATURE')
mod.object = rig
print('BUILT r_hand_stock tris', sum(len(p.vertices) - 2 for p in me.polygons), 'verts', len(me.vertices),
      'bones', len(arm_data.bones), 'max bones per vertex', max(len([g for g in v.groups if g.weight > 0.001]) for v in me.vertices))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
