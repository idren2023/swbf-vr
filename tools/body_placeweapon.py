"""Blender (blender -b --factory-startup <hand+weapons.blend> -P tools/body_placeweapon.py -- <out.blend> <weapon-object>
<pose-label> grip=x0,x1,y0,y1 [hand=<object>] [top=mm] [save]): puts a weapon's grip in our hand (idren2023 2026-10-22:
the grip in our hand, not the stock placement, which leaves the hand in the gun).

- The grip: the weapon's vertices inside the box grip= (mm, in the weapon's place as tools/body_weapons.py left it,
  read off tools/body_weapview.py's grids; x0 at the frame, x1 at the butt); its axis from the top slab's centre to
  the bottom slab's, its top where the grip meets the frame.
- The hand's tunnel: the pose's curled middle, ring and pinky; for each a circle fitted through its joints (posed:
  knuckle, middle joint, end joint, tip); the tunnel's axis through the circles' centres (index side to pinky side).
- The weapon turned so its grip axis lies along the tunnel's axis and its barrel points as near the hand's forward
  (+y of the hand bone) as the axis allows; its grip's axis `back` mm (default: the grip's radius + 4) behind the
  middle finger's knuckle, its top `top` mm (default 10) toward the index from the middle finger's circle; then slid
  toward the palm until the palm's surface is within `gap` mm (default 2) of the grip's (the grip's faces only).
  tools/body_fitgrip.py then closes the fingers onto it. With `stock`: the weapon stays where the stock game hangs it
  (hp_weapons, as tools/body_weapons.py placed it); only its grip axis is measured (for BodyArm).
Prints the weapon's place relative to the hand bone and the grip's and the tunnel's radii; with `save` writes it to
re/body/grip-<weapon>.json (a 4x4 in the hand bone's rest frame, metres, our hand's scale) and saves <out.blend>."""
import bpy, json, math, os, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
OUT, WEAP, LABEL = args[0], args[1], args[2]
opt = dict(a.split('=', 1) for a in args[3:] if '=' in a)
x0, x1, y0, y1 = (float(v) / 1000 for v in opt['grip'].split(','))
TOP = float(opt.get('top', 10)) / 1000
GRIP_BACK_MM = opt.get('back')
hand = bpy.data.objects[opt.get('hand', 'r_hand_retopo')]
rig = hand.parent
w = bpy.data.objects[WEAP]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'body_poses.py')).read()
ns = {}
exec(src[src.index('FINGERS = '):src.index('scene = bpy.context.scene')], ns)
POSE = dict(next(p for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(LABEL)))


# ---- the grip ---------------------------------------------------------------------------------------------------------
W0 = w.matrix_world.copy()
wpts = [W0 @ v.co for v in w.data.vertices]
g = [p for p in wpts if x0 <= p.x <= x1 and y0 <= p.y <= y1]
# its axis from the top slab's centre (its 30 % nearest the frame) to the bottom slab's (the 30 % at the butt): the grips run across the palm, along the hand frame's x where tools/body_weapons.py put them
gx0, gx1 = min(p.x for p in g), max(p.x for p in g)
top_s = [p for p in g if p.x <= gx0 + 0.3 * (gx1 - gx0)]
bot_s = [p for p in g if p.x >= gx1 - 0.3 * (gx1 - gx0)]
ct, cb = sum(top_s, Vector()) / len(top_s), sum(bot_s, Vector()) / len(bot_s)
ga = (cb - ct).normalized()
gc = sum(g, Vector()) / len(g)
ts = [(p - gc).dot(ga) for p in g]
g_top = gc + ga * min(ts)
g_len = max(ts) - min(ts)
g_rad = sum(((p - gc) - ga * (p - gc).dot(ga)).length for p in g) / len(g)
barrel = Vector((0, 1, 0))                                # the weapon's forward where tools/body_weapons.py put it
# ---- the hand's tunnel ------------------------------------------------------------------------------------------------
for pb in rig.pose.bones:
    pb.rotation_mode = 'XYZ'
    pb.rotation_euler = (0, 0, 0)
for n, d in POSE.items():
    if n in rig.pose.bones:
        pb = rig.pose.bones[n]
        pb.rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
        pb.rotation_euler = [math.radians(a) for a in d]
bpy.context.view_layer.update()
M = rig.matrix_world


def circle(pts):
    """Centre and radius of the circle best through pts (in their best plane; algebraic fit)."""
    c = sum(pts, Vector()) / len(pts)
    # the plane: normal = smallest-variance direction (cross of the two main spreads)
    a = (pts[1] - pts[0]).normalized()
    b0 = (pts[-1] - pts[0])
    n = a.cross(b0).normalized()
    b = n.cross(a)
    xy = [((p - c).dot(a), (p - c).dot(b)) for p in pts]
    # solve x^2 + y^2 + D x + E y + F = 0 (least squares, 3x3 normal equations)
    A = Matrix(((0, 0, 0), (0, 0, 0), (0, 0, 0)))
    r = Vector((0, 0, 0))
    for x, y in xy:
        row = (x, y, 1.0)
        rhs = -(x * x + y * y)
        for i in range(3):
            for j in range(3):
                A[i][j] += row[i] * row[j]
            r[i] += row[i] * rhs
    D, E, F = A.inverted() @ r
    cx, cy = -D / 2, -E / 2
    return c + a * cx + b * cy, math.sqrt(max(cx * cx + cy * cy - F, 0.0))


cent, rads = {}, {}
for f in ('middle', 'ring', 'pinky'):
    P = rig.pose.bones
    pts = [M @ P[f + '_1'].head, M @ P[f + '_2'].head, M @ P[f + '_3'].head, M @ P[f + '_3'].tail]
    cent[f], rads[f] = circle(pts)
ta = (cent['pinky'] - cent['middle']).normalized()
tc = (cent['middle'] + cent['ring'] + cent['pinky']) / 3
fwd = (M.to_3x3() @ rig.pose.bones['hand'].matrix.to_3x3()).col[1].normalized()
# ---- the weapon's turn and place ---------------------------------------------------------------------------------------
def frame(axis, fwd_):
    f = (fwd_ - axis * fwd_.dot(axis)).normalized()
    return Matrix((axis, f, axis.cross(f))).transposed()


STOCK = 'stock' in args  # keep the weapon where the stock game hangs it (idren2023 2026-10-24: the stock hold first)
GRIP_BACK = float(GRIP_BACK_MM) / 1000 if GRIP_BACK_MM else g_rad + 0.004
Rw = frame(ga, barrel)
Rh = frame(ta, fwd)
R = Rh @ Rw.inverted()
# where across the hand: the grip's axis just behind the knuckle row (its front surface at the middle finger's
# knuckle: the fingers wrap it from there), at the tunnel's height to start with (the slide below sets it)
knuckle = M @ rig.pose.bones['middle_1'].head
fwd_t = (fwd - ta * fwd.dot(ta)).normalized()
base_pt = knuckle - fwd_t * (GRIP_BACK + 0.0)
base_pt += (tc - base_pt) - fwd_t * (tc - base_pt).dot(fwd_t) - ta * (tc - base_pt).dot(ta)
target_top = base_pt + ta * ((cent['middle'] - base_pt).dot(ta) - TOP)
# the grip's top on the tunnel's axis at target_top
T = Matrix.Translation(target_top) @ R.to_4x4() @ Matrix.Translation(-g_top)
if STOCK:
    T = Matrix.Identity(4)
    R = Matrix.Identity(3)
w.matrix_world = T @ W0
bpy.context.view_layer.update()
# against the palm: from there the weapon slides toward the palm (the hand's +z, square to the grip's axis) until the
# palm's surface (vertices on the hand bone facing the palm side) comes within GAP of the weapon's, without crossing
GAP = float(opt.get('gap', 2)) / 1000
hme = hand.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
hmw = hand.matrix_world
names = {gr.index: gr.name for gr in hand.vertex_groups}
up = (M.to_3x3() @ rig.pose.bones['hand'].matrix.to_3x3()).col[2].normalized()
up = (up - ta * up.dot(ta)).normalized()
palm = [i for i, v in enumerate(hand.data.vertices)
        if sum(gw.weight for gw in v.groups if names[gw.group] in ('hand', 'thumb_1')) > 0.8
        and (hmw.to_3x3() @ hme.vertices[i].normal).dot(up) < -0.3]
hme.calc_loop_triangles()
hv = [hmw @ v.co for v in hme.vertices]
pset = set(palm)
ptris = [list(t.vertices) for t in hme.loop_triangles if all(i in pset for i in t.vertices)]
from mathutils.bvhtree import BVHTree
import bmesh
gidx = {i for i, v in enumerate(w.data.vertices) if x0 <= (W0 @ v.co).x <= x1 and y0 <= (W0 @ v.co).y <= y1}
def wtree_at(off):
    # the grip's faces only (a rifle's stock runs back under the wrist)
    b_ = bmesh.new()
    b_.from_mesh(w.data)
    bmesh.ops.delete(b_, geom=[v for v in b_.verts if v.index not in gidx], context='VERTS')
    b_.transform(Matrix.Translation(up * off) @ w.matrix_world)
    return BVHTree.FromBMesh(b_)
ptree = BVHTree.FromPolygons(hv, ptris)
best = None
if STOCK:
    # the stock place, slid straight out of the palm (the hand's -z) only as far as the grip stops crossing it
    best = None
    for k in range(0, -81, -1):
        off = k * 0.0005
        wt = wtree_at(off)
        if not ptree.overlap(wt):
            best = (off, min(wt.find_nearest(hv[i])[3] for i in palm))
            break
for k in (range(0) if STOCK else range(-80, 121)):
    off = k * 0.0005
    wt = wtree_at(off)
    if ptree.overlap(wt):
        break
    d = min(wt.find_nearest(hv[i])[3] for i in palm)
    best = (off, d)
    if d <= GAP:
        break
off = best[0] if best else 0.0
w.matrix_world = Matrix.Translation(up * off) @ w.matrix_world
bpy.context.view_layer.update()
print('PLACE against the palm: moved %.1f mm toward it (negative: out of it), palm %.1f mm from the weapon (%d palm vertices)' % (
    off * 1000, best[1] * 1000 if best else -1, len(palm)))
hb = M @ rig.pose.bones['hand'].matrix
rel = hb.inverted() @ w.matrix_world
ang = math.degrees(ga.angle(barrel))
print('PLACE %s: grip %d verts, length %.0f mm, radius %.1f mm, at %.0f deg to the barrel; tunnel radii (bone circles) '
      'middle %.1f ring %.1f pinky %.1f mm; weapon turned %.0f deg, moved %.0f mm' % (
          WEAP, len(g), g_len * 1000, g_rad * 1000, ang, rads['middle'] * 1000, rads['ring'] * 1000,
          rads['pinky'] * 1000, math.degrees(R.to_quaternion().angle), (T.to_translation()).length * 1000))
if 'save' in args:
    # the grip's axis as it ends up in the hand (hand bone rest frame): through the middle finger's circle, from
    # the frame toward the butt; BodyArm puts this axis on the controller's grip pose (its handle's axis)
    axis_point = base_pt + ta * (cent['middle'] - base_pt).dot(ta) + up * off
    axis_direction = ta
    if STOCK:
        # the weapon's own grip line where it hangs, at the middle finger's curl
        axis_point = g_top + ga * (cent['middle'] - g_top).dot(ga) + up * off  # with the slide out of the palm
        axis_direction = ga
    json.dump({'weapon': WEAP, 'pose': LABEL, 'in_hand_bone': [list(r) for r in rel],
               'axis_point': list(axis_point), 'axis_direction': list(axis_direction),
               'placement': 'stock' if STOCK else 'fitted',
               'note': 'weapon object matrix relative to the hand bone (rest), metres, our hand scale 0.712 of stock'},
              open(os.path.join('re', 'body', 'grip-%s.json' % WEAP), 'w'), indent=1)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
