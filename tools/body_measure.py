"""Blender (blender -b --factory-startup <stock.blend> -P tools/body_measure.py -- <stock-object> [hand-length-mm]):
the stock first-person hand's proportions in a hand frame (origin the wrist, +y toward the middle knuckle, +x toward
the pinky, +z the back of the hand), scaled so wrist to middle finger tip is hand-length (default 195 mm): knuckle
centres, segment lengths, finger widths and thicknesses, the palm's width and thickness along it, the thumb's
segments. Joint rings come from the stock's edge loops (back-strip rows of the uv pattern, PLAN 51 B); lengths do not
depend on its grip pose, positions on the palm do not either (the palm is rigid)."""
import bpy, math, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
OBJ = args[0]
HAND_MM = float(args[1]) if len(args) > 1 else 195.0
stock = bpy.data.objects[OBJ]
me = stock.data
uv = me.uv_layers.active.data
hg = stock.vertex_groups['bone_r_wrist'].index
skel = bpy.data.objects.get(OBJ + '_skeleton')
wrist = skel.matrix_world @ skel.data.bones['bone_r_wrist'].head_local

C = []
for p in me.polygons:
    if p.material_index != 0:
        continue
    for li in p.loop_indices:
        v = me.vertices[me.loops[li].vertex_index]
        if any(g.group == hg for g in v.groups):
            C.append((uv[li].uv.x * 256, (1 - uv[li].uv.y) * 256, (stock.matrix_world @ v.co).freeze()))
VERTS = list({c[2] for c in C})


def centre(sel):
    pts = {co for x, y, co in C if sel(x, y)}
    return sum(pts, Vector()) / len(pts) if pts else None


FINGERS = ['index', 'middle', 'ring', 'pinky']
BACK = {'index': ((87, 115), (125, 80, 46), 20, (100, 6)), 'middle': ((55, 85), (126, 74, 41), 10, (70, 1)),
        'ring': ((30, 53), (125, 91, 56), 28, (41, 17)), 'pinky': ((1, 26), (126, 95, 73), 51, (15, 42))}


def ring(j, axis, reach=0.03):
    pts = [p for p in VERTS if abs((p - j).dot(axis)) < 0.003 and (p - j).length < reach]
    return pts


joints, rings_ = {}, {}
for f in FINGERS:
    (x0, x1), rows, shoulder, apex = BACK[f]
    back_pts = [centre(lambda x, y, r=r: x0 - 1 <= x <= x1 + 1 and abs(y - r) < 2.5) for r in rows + (shoulder,)]
    cs, rs = [], []
    for i, j in enumerate(back_pts):
        d = (back_pts[min(i + 1, 3)] - back_pts[max(i - 1, 0)]).normalized()
        pts = ring(j, d)
        cs.append(sum(pts, Vector()) / len(pts))
        rs.append((pts, d))
    # the tip: the stock vertex farthest along the last segment
    d = (cs[3] - cs[2]).normalized()
    tip = max((p for p in VERTS if (p - cs[2]).length < 0.06), key=lambda p: (p - cs[2]).dot(d))
    joints[f] = cs[:3] + [tip]
    rings_[f] = rs
mcps = [joints[f][0] for f in FINGERS]
y_axis = (joints['middle'][0] - wrist).normalized()
x_axis = joints['pinky'][0] - joints['index'][0]
x_axis = (x_axis - y_axis * x_axis.dot(y_axis)).normalized()
z_axis = x_axis.cross(y_axis)
back_mid = centre(lambda x, y: 1 <= x <= 115 and 150 < y < 200)
if (back_mid - wrist).dot(z_axis) < 0:
    x_axis, z_axis = -x_axis, -z_axis
to_hand = Matrix((x_axis, y_axis, z_axis))


def H(p):
    return to_hand @ (p - wrist)


mid_len = sum(((joints['middle'][i + 1] - joints['middle'][i]).length for i in range(3)), (joints['middle'][0] - wrist).length)
k = HAND_MM / 1000 / mid_len
print('SCALE stock x %.3f (stock wrist to middle tip %.0f mm -> %.0f mm)' % (k, mid_len * 1000, HAND_MM))
for f in FINGERS:
    js = joints[f]
    segs = [(js[i + 1] - js[i]).length * k * 1000 for i in range(3)]
    m = H(js[0]) * k * 1000
    widths = []
    for pts, d in rings_[f][:3]:
        lat = (x_axis - d * x_axis.dot(d)).normalized()
        up = d.cross(lat).normalized()
        xs = [(p).dot(lat) for p in pts]
        zs = [(p).dot(up) for p in pts]
        widths.append(((max(xs) - min(xs)) * k * 1000, (max(zs) - min(zs)) * k * 1000))
    print('FINGER %-6s MCP x %6.1f y %6.1f z %6.1f mm | segments %5.1f %5.1f %5.1f mm | rings w x t %s' % (
        (f, m.x, m.y, m.z) + tuple(segs) + (' '.join('%.0fx%.0f' % wt for wt in widths),)))
# palm cross-sections: stock vertices in slabs across the wrist -> knuckle line (not on fingers or thumb)
for frac in (0.0, 0.25, 0.5, 0.75, 0.95):
    yv = frac * (H(joints['middle'][0]).y)
    pts = [H(p) for p in VERTS if abs(H(p).y - yv) < 0.006]
    pts = [p for p in pts if abs(p.z) < 0.06 and -0.08 < p.x < 0.08]
    if pts:
        xs, zs = [p.x for p in pts], [p.z for p in pts]
        print('PALM at %3.0f%% (y %5.1f mm): x %6.1f .. %6.1f  z %6.1f .. %6.1f mm' % (
            frac * 100, yv * k * 1000, min(xs) * k * 1000, max(xs) * k * 1000, min(zs) * k * 1000, max(zs) * k * 1000))
# thumb: the strip's rows (island x 116-167) from its base (y 150) to the tip
rows = sorted({round(y) for x, y, co in C if 116 <= x <= 167 and 95 <= y <= 152})
print('THUMB uv rows', rows)
tcs = []
for r in [150] + [r for r in rows if 100 < r < 148][::-1]:
    c = centre(lambda x, y, r=r: 118 <= x <= 166 and abs(y - r) < 1.5)
    if c is not None:
        tcs.append((r, c))
ttip = centre(lambda x, y: math.hypot(x - 143, y - 97) < 6)
for r, c in tcs:
    h = H(c) * k * 1000
    print('THUMB row %3d centre x %6.1f y %6.1f z %6.1f mm' % (r, h.x, h.y, h.z))
h = H(ttip) * k * 1000
print('THUMB tip      x %6.1f y %6.1f z %6.1f mm' % (h.x, h.y, h.z))
