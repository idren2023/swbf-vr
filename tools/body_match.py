"""Blender (blender -b --factory-startup <ours.blend> -P tools/body_match.py -- <stock.blend> <stock-object> <out-prefix>):
poses our hand rig like the stock first-person hand and renders both side by side for comparison.

The stock hand has no finger bones; its pose is measured from its edge loops at the joints (rows of the uv pattern,
see PLAN 51 B): per finger the centre of the vertices on each joint row (MCP, PIP, DIP) and the tip, which give the
bend at each joint; the thumb by its tip and base. Our rig gets the same bends (less its rest curl), is scaled and
moved onto the stock hand (wrist, knuckle line, back-of-hand direction) and rendered from the same camera relative to each, joined side by side (stock left)."""
import bpy, math, os, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
STOCK_BLEND, STOCK_OBJ, OUT = args[0], args[1], args[2]
scene = bpy.context.scene
ours = bpy.data.objects['r_arm']
rig = ours.parent

with bpy.data.libraries.load(STOCK_BLEND) as (src, dst):
    dst.objects = [STOCK_OBJ, STOCK_OBJ + '_skeleton'] if STOCK_OBJ + '_skeleton' in src.objects else [STOCK_OBJ]
for o in dst.objects:
    scene.collection.objects.link(o)
stock = bpy.data.objects[STOCK_OBJ]
me = stock.data
uv = me.uv_layers.active.data
hand_group = stock.vertex_groups['bone_r_wrist'].index

# joint rows in texels (x range of the strip, rows MCP / PIP / DIP), back half then palm half, and tip apexes
FINGERS = ['index', 'middle', 'ring', 'pinky']
BACK = {'index': ((87, 115), (125, 80, 46), (100, 6)), 'middle': ((55, 85), (126, 74, 41), (70, 1)),
        'ring': ((30, 53), (125, 91, 56), (41, 17)), 'pinky': ((1, 26), (126, 95, 73), (15, 42))}
PALM = {'index': ((166, 187), (123, 80, 47), (178, 12)), 'middle': ((188, 210), (113, 74, 40), (198, 0)),
        'ring': ((212, 235), (116, 91, 57), (224, 22)), 'pinky': ((237, 255), (118, 94, 71), (246, 43))}


def corners():
    for p in me.polygons:
        if p.material_index != 0:
            continue
        for li in p.loop_indices:
            v = me.vertices[me.loops[li].vertex_index]
            if any(g.group == hand_group for g in v.groups):
                yield uv[li].uv.x * 256, (1 - uv[li].uv.y) * 256, stock.matrix_world @ v.co


C = list(corners())


def centre(sel):
    pts = {tuple(round(c, 5) for c in co): co for x, y, co in C if sel(x, y)}
    return sum(pts.values(), Vector()) / len(pts) if pts else None


# joint points from the back strip's rows only (a back row and the palm row of the same loop share their seam
# vertices, but the palm rows are not reliably the same loops), then the row where the pointed tip starts
SHOULDER = {'index': 20, 'middle': 10, 'ring': 28, 'pinky': 51}
stock_joints = {}
for f in FINGERS:
    (bx0, bx1), brows, _ = BACK[f]
    js = [centre(lambda x, y, r=r: bx0 - 1 <= x <= bx1 + 1 and abs(y - r) < 2.5) for r in brows + (SHOULDER[f],)]
    stock_joints[f] = js
    print('JOINTS %-6s' % f, [tuple(round(c, 3) for c in j) if j else None for j in js],
          'segments mm', [round((b_ - a_).length * 1000) for a_, b_ in zip(js, js[1:]) if a_ and b_])
stock_thumb_tip = centre(lambda x, y: math.hypot(x - 143, y - 97) < 9)
stock_thumb_base = centre(lambda x, y: 118 <= x <= 168 and abs(y - 150) < 4)
wrist_stock = bpy.data.objects[STOCK_OBJ + '_skeleton'].matrix_world @ \
    bpy.data.objects[STOCK_OBJ + '_skeleton'].data.bones['bone_r_wrist'].head_local if STOCK_OBJ + '_skeleton' in bpy.data.objects \
    else centre(lambda x, y: y > 240)
palm_mcp = centre(lambda x, y: 166 <= x <= 255 and abs(y - 118) < 6)
back_mcp = centre(lambda x, y: 1 <= x <= 115 and abs(y - 125) < 3)
back_dir = (back_mcp - palm_mcp).normalized()


hand_axis = (sum((stock_joints[f][0] for f in FINGERS), Vector()) / 4 - wrist_stock).normalized()
lateral_axis = (stock_joints['pinky'][0] - stock_joints['index'][0]).normalized()
# a bend is a turn about the hand's side-to-side axis; its sign from the turn that takes the back-of-hand
# direction toward the palm (a curl), the same sense at every joint however far the finger has curled
curl_sign = 1 if hand_axis.cross(-back_dir).dot(lateral_axis) > 0 else -1


def angle(a, b, _unused=None):
    a, b = a.normalized(), b.normalized()
    ang = math.degrees(a.angle(b))
    return ang if a.cross(b).dot(lateral_axis) * curl_sign > 0 else -ang


bends = {}
for f in FINGERS:
    m, p, d, t = stock_joints[f]
    bends[f] = (angle(hand_axis, p - m), angle(p - m, d - p), angle(d - p, t - d))
    print('STOCK %-6s MCP %5.1f PIP %5.1f DIP %5.1f deg' % ((f,) + bends[f]))

# our rig: those bends less the rest curl (the rest pose is already curled 14 / 18 / 10)
REST = (14, 18, 10)
for pb in rig.pose.bones:
    pb.rotation_mode = 'XYZ'
    pb.rotation_euler = (0, 0, 0)
for f in FINGERS:
    for j in range(3):
        rig.pose.bones['%s_%d' % (f, j + 1)].rotation_euler.x = -math.radians(bends[f][j] - REST[j])
bpy.context.view_layer.update()


def frame(wrist, mcp_index, mcp_middle, mcp_pinky, back):
    y = (mcp_middle - wrist).normalized()
    x = (mcp_pinky - mcp_index)
    x = (x - y * x.dot(y)).normalized()
    z = x.cross(y)
    if z.dot(back) < 0:
        x = -x
        z = -z
    m = Matrix((x, y, z)).transposed().to_4x4()
    m.translation = wrist
    return m


bones = rig.pose.bones
W = rig.matrix_world
ours_mcp = {f: W @ bones[f + '_1'].head for f in FINGERS}
ours_frame = frame(W @ bones['hand'].head, ours_mcp['index'], ours_mcp['middle'], ours_mcp['pinky'], Vector((0, 0, 1)))
stock_frame = frame(wrist_stock, stock_joints['index'][0], stock_joints['middle'][0], stock_joints['pinky'][0], back_dir)
scale = (stock_joints['middle'][0] - wrist_stock).length / (ours_mcp['middle'] - W @ bones['hand'].head).length
print('SCALE stock / ours %.2f (wrist to middle knuckle)' % scale)

# thumb: turn its base bone so its tip points where the stock thumb's does, in the hand's frame
to_stock = stock_frame @ Matrix.Scale(scale, 4) @ ours_frame.inverted()
rig.matrix_world = to_stock @ rig.matrix_world
bpy.context.view_layer.update()
t1 = bones['thumb_1']
ours_tip = rig.matrix_world @ bones['thumb_3'].tail
base = rig.matrix_world @ t1.head
want = (stock_thumb_tip - base).normalized()
have = (ours_tip - base).normalized()
rot_world = have.rotation_difference(want)
bone_world = (rig.matrix_world @ t1.matrix).to_quaternion()
t1.rotation_mode = 'QUATERNION'
t1.rotation_quaternion = bone_world.inverted() @ rot_world @ bone_world
bpy.context.view_layer.update()
print('THUMB turned %.0f deg to the stock tip' % math.degrees(have.angle(want)))

# texture placement against the stock, part by part: a point on a finger segment is (how far along the segment,
# angle around the finger from its back), on the palm (along wrist -> knuckles, across, toward the back); the stock
# point with the same description gives the stock's uv there. Per face of our glove the gap to our uv, in texels,
# per bone; and a picture: the texture with an arrow from our uv (cyan end) to the stock's (yellow end). The thumb
# is left out (its pose does not match the stock's yet).
from mathutils.geometry import barycentric_transform
import bmesh
sb = bmesh.new()
sb.from_mesh(me)
sb.transform(stock.matrix_world)
sdl = sb.verts.layers.deform.active
keep = {f for f in sb.faces if f.material_index == 0 and all(any(g == hand_group for g in v[sdl].keys()) for v in f.verts)}
bmesh.ops.delete(sb, geom=[f for f in sb.faces if f not in keep], context='FACES')
bmesh.ops.triangulate(sb, faces=sb.faces[:])
suv = sb.loops.layers.uv.active
# stock ring centres: every stock vertex within 3 mm of the plane through the back-row point, across the finger
s_verts = [v.co.copy() for v in sb.verts]


def ring_centre(j, axis):
    pts = [p for p in s_verts if abs((p - j).dot(axis)) < 0.003 and (p - j).length < 0.03]
    return sum(pts, Vector()) / len(pts) if pts else j


stock_axes = {}
for f in FINGERS:
    js = stock_joints[f]
    cs = []
    for i, j in enumerate(js):
        d = (js[min(i + 1, 3)] - js[max(i - 1, 0)]).normalized()
        cs.append(ring_centre(j, d))
    stock_axes[f] = cs
lat_s = (stock_axes['pinky'][0] - stock_axes['index'][0]).normalized()
bpy.context.view_layer.update()
RW = rig.matrix_world
our_axes = {f: [RW @ bones[f + '_1'].head, RW @ bones[f + '_1'].tail, RW @ bones[f + '_2'].tail, RW @ bones[f + '_3'].tail]
            for f in FINGERS}
lat_o = (our_axes['pinky'][0] - our_axes['index'][0]).normalized()
hand_dir = {id(lat_s): hand_axis,
            id(lat_o): (sum((our_axes[f][0] for f in FINGERS), Vector()) / 4 - RW @ bones['hand'].head).normalized()}


def seg_coords(p, a, b, lat, back):
    """(how far along a -> b, angle around it from the back of the finger toward its pinky side). The finger's
    back is lat x direction (fingers bend about the side axis, so this stays on the back however far a segment has
    curled); `back` only fixes the sign once, for a straight finger."""
    d = b - a
    t = (p - a).dot(d) / d.length_squared
    r = p - (a + d * t)
    sign = 1 if lat.cross(hand_dir[id(lat)]).dot(back) > 0 else -1
    up = lat.cross(d.normalized()).normalized() * sign
    side = (lat - d.normalized() * lat.dot(d.normalized())).normalized()
    return t, math.atan2(r.dot(side), r.dot(up))


def hand_coords(p, wrist, mcps, back):
    y = (sum(mcps, Vector()) / len(mcps) - wrist)
    x = mcps[-1] - mcps[0]
    x = (x - y.normalized() * x.dot(y.normalized()))
    z = x.normalized().cross(y.normalized())
    if z.dot(back) < 0:
        z = -z
    q = p - wrist
    return q.dot(y) / y.length_squared, q.dot(x) / x.length_squared * 2, q.dot(z) / 0.02


# stock samples: a grid on every triangle, each with its uv, sorted into finger segments or the palm
samples = {}
for tri in sb.faces:
    co = [l.vert.co for l in tri.loops]
    tv = [l[suv].uv for l in tri.loops]
    for i in range(5):
        for j in range(5 - i):
            w = (i / 4, j / 4, 1 - i / 4 - j / 4)
            p = co[0] * w[0] + co[1] * w[1] + co[2] * w[2]
            uvp = tv[0] * w[0] + tv[1] * w[1] + tv[2] * w[2]
            best = None
            for f in FINGERS:
                ax = stock_axes[f]
                for k in range(3):
                    t, ang = seg_coords(p, ax[k], ax[k + 1], lat_s, back_dir)
                    if -0.15 <= t <= (1.6 if k == 2 else 1.0):
                        dd = (p - (ax[k] + (ax[k + 1] - ax[k]) * max(0, min(1, t)))).length
                        if dd < 0.03 and (best is None or dd < best[0]):
                            best = (dd, (f, k), (t, ang))
            if best and best[0] < 0.022:
                samples.setdefault(best[1], []).append((best[2], uvp))
            else:
                samples.setdefault('palm', []).append((hand_coords(p, wrist_stock, [stock_axes[f][0] for f in FINGERS], back_dir), uvp))
dg = bpy.context.evaluated_depsgraph_get()
om = ours.evaluated_get(dg).to_mesh()
ouv = om.uv_layers.active.data
names = {g.index: g.name for g in ours.vertex_groups}
our_back = (rig.matrix_world.to_3x3() @ Vector((0, 0, 1))).normalized()
gaps, arrows, shifts, moves = {}, [], {}, {}
for p_ in om.polygons:
    if p_.material_index != 0:
        continue
    vs = [om.vertices[i] for i in p_.vertices]
    bone = max(((names[g.group], g.weight) for v in vs for g in v.groups), key=lambda gw: gw[1])[0]
    if bone.startswith('thumb') or bone.startswith('forearm'):
        continue
    c = ours.matrix_world @ p_.center
    if bone == 'hand':
        key, q = 'palm', hand_coords(c, RW @ bones['hand'].head, [our_axes[f][0] for f in FINGERS], our_back)
        dist = lambda a: (a[0] - q[0]) ** 2 + (a[1] - q[1]) ** 2 + (a[2] - q[2]) ** 2 * 0.3
    else:
        f, k = bone.split('_')[0], int(bone.split('_')[1]) - 1
        ax = our_axes[f]
        key, q = (f, k), seg_coords(c, ax[k], ax[k + 1], lat_o, our_back)
        dist = lambda a: (a[0] - q[0]) ** 2 + (math.atan2(math.sin(a[1] - q[1]), math.cos(a[1] - q[1])) / math.pi) ** 2
    cand = samples.get(key)
    if not cand:
        continue
    near = sorted(cand, key=lambda sp: dist(sp[0]))[:3]
    st = sum((sp[1] for sp in near), Vector((0, 0))) / len(near)
    our = sum((ouv[i].uv for i in p_.loop_indices), Vector((0, 0))) / p_.loop_total
    gap = (st - our).length * 256
    gaps.setdefault(bone, []).append(gap)
    arrows.append((our.x, our.y, st.x, st.y, gap))
    if gap < 40:
        for li in p_.loop_indices:
            luv = ouv[li].uv
            key = (om.loops[li].vertex_index, 'back' if luv.x < 0.47 else ('palm' if luv.x > 0.64 else 'column'))
            moves.setdefault(key, []).append(st - our)
    if gap < 40:
        side = 'back' if our.x < 0.47 else ('palm' if our.x > 0.64 else 'column')
        shifts.setdefault((bone, side), []).append(((st.x - our.x) * 256, (our.y - st.y) * 256))
for bone, g in sorted(gaps.items()):
    g.sort()
    print('UVGAP %-10s faces %3d median %5.1f  p90 %5.1f texels' % (bone, len(g), g[len(g) // 2], g[int(len(g) * 0.9)]))
# the systematic part: mean move from our uv to the stock's per bone and side (texels; +x right, +y down the
# texture), faces within 40 texels (larger gaps are the other side of a seam)
for (bone, side), v in sorted(shifts.items()):
    if len(v) >= 4:
        print('SHIFT %-10s %-6s faces %3d  dx %+5.1f  dy %+5.1f' % (bone, side, len(v), sum(a for a, b in v) / len(v), sum(b for a, b in v) / len(v)))
# targets for the builder: per vertex and side the mean move toward the stock, smoothed twice over neighbours,
# capped at 15 texels; written per face corner (face index, vertex index -> uv), the mesh's indices being the
# builder's (tools/body_hand.py reads re/body/uv-targets.json with its 'targets' argument)
import json
nbr = {}
for e in om.edges:
    a_, b_ = e.vertices
    nbr.setdefault(a_, set()).add(b_)
    nbr.setdefault(b_, set()).add(a_)
delta = {k: sum(v, Vector((0, 0))) / len(v) for k, v in moves.items()}
for _ in range(2):
    new = {}
    for (vi, side), d in delta.items():
        ns = [delta[(n, side)] for n in nbr.get(vi, ()) if (n, side) in delta]
        new[(vi, side)] = (d * 2 + sum(ns, Vector((0, 0)))) / (2 + len(ns))
    delta = new
cap = 15 / 256
targets = {}
for p_ in om.polygons:
    if p_.material_index != 0:
        continue
    for li in p_.loop_indices:
        vi = om.loops[li].vertex_index
        luv = ouv[li].uv
        side = 'back' if luv.x < 0.47 else ('palm' if luv.x > 0.64 else 'column')
        d = delta.get((vi, side))
        if d is None:
            continue
        if d.length > cap:
            d = d * (cap / d.length)
        targets['%d:%d' % (p_.index, vi)] = [luv.x + d.x, luv.y + d.y]
json.dump(targets, open(os.path.join(os.path.dirname(os.path.abspath(OUT)), 'uv-targets.json'), 'w'))
print('TARGETS', len(targets), 'face corners')
img = bpy.data.images.load('C:/LucasArts/BFBuilder/Assets/Sides/ALL/MSH/all_1st_inf_trooper_hands.tga')
w_, h_ = img.size
k_ = 4
px_ = img.pixels[:]
cw, ch = w_ * k_, h_ * k_
pic = [0.0] * (cw * ch * 4)
for y in range(ch):
    for x in range(cw):
        s_ = ((y // k_) * w_ + x // k_) * 4
        pic[(y * cw + x) * 4:(y * cw + x) * 4 + 4] = (px_[s_] * 0.55, px_[s_ + 1] * 0.55, px_[s_ + 2] * 0.55, 1.0)
for u0, v0, u1, v1, dist in arrows:
    n = int(max(abs(u1 - u0) * cw, abs(v1 - v0) * ch)) + 1
    for t in range(n + 1):
        f_ = t / n
        x = int((u0 + (u1 - u0) * f_) * cw)
        y = int((v0 + (v1 - v0) * f_) * ch)
        if 0 <= x < cw and 0 <= y < ch:
            pic[(y * cw + x) * 4:(y * cw + x) * 4 + 3] = (0.2 + 0.8 * f_, 0.9, 1.0 - 0.9 * f_)
im = bpy.data.images.new('gaps', cw, ch)
im.pixels = pic
im.filepath_raw = os.path.abspath(OUT + '-uvgap.png')
im.file_format = 'PNG'
im.save()

# side by side: ours moved off the stock hand along its lateral axis (toward its pinky side)
lateral = (stock_joints['pinky'][0] - stock_joints['index'][0]).normalized()
offset = lateral * 0.16
rig.matrix_world = Matrix.Translation(offset) @ rig.matrix_world
for o in scene.objects:
    if o.type == 'MESH' and o not in (stock, ours):
        o.hide_render = True

scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
W_, H_ = 700, 700
scene.render.resolution_x, scene.render.resolution_y = W_, H_
cam_data = bpy.data.cameras.new('cmp')
cam_data.type = 'ORTHO'
cam_data.ortho_scale = 0.30
cam = bpy.data.objects.new('cmp', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
mid = (stock_joints['middle'][1] + wrist_stock) / 2
fwd = hand_axis
others = [o for o in scene.objects if o.type == 'MESH' and o not in (stock, ours)]
for o in others:
    o.hide_render = True
VIEWS = (('back', back_dir), ('palm', -back_dir), ('thumb side', -lateral), ('pinky side', lateral), ('front', fwd),
         ('back three-quarter', (back_dir - lateral * 0.7 - fwd * 0.4).normalized()),
         ('palm three-quarter', (-back_dir - lateral * 0.7 + fwd * 0.3).normalized()))
for label, d in VIEWS:
    up = fwd if label in ('back', 'palm') else back_dir
    up = (up - d * up.dot(d)).normalized()
    rot = Matrix((d.cross(up) * -1, up, d)).transposed().to_4x4()  # camera x right, y up, looking along -z = -d
    halves = []
    for obj, shift in ((stock, Vector()), (ours, offset)):
        stock.hide_render = obj is not stock
        ours.hide_render = obj is not ours
        cam.matrix_world = Matrix.Translation(mid + shift + d * 2) @ rot
        scene.render.filepath = os.path.abspath('%s-tmp.png' % OUT)
        bpy.ops.render.render(write_still=True)
        im = bpy.data.images.load(scene.render.filepath, check_existing=False)
        halves.append(im.pixels[:])
        bpy.data.images.remove(im)
    sheet = [0.0] * (2 * W_ * H_ * 4)
    for y in range(H_):
        for i, px in enumerate(halves):
            sheet[(y * 2 * W_ + i * W_) * 4:(y * 2 * W_ + (i + 1) * W_) * 4] = px[y * W_ * 4:(y + 1) * W_ * 4]
    out = bpy.data.images.new('cmp', 2 * W_, H_)
    out.pixels = sheet
    out.filepath_raw = os.path.abspath('%s-%s.png' % (OUT, label.replace(' ', '')))
    out.file_format = 'PNG'
    out.save()
os.remove(os.path.abspath('%s-tmp.png' % OUT))
print('RENDERED', OUT)
