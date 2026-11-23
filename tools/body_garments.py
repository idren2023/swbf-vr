"""Blender (blender -b --factory-startup re/body/all_inf_soldier.blend -P tools/body_garments.py -- <out.blend>):
the player's body rebuilt with each fabric a piece of its own (idren2023 2026-10-25: the chest and up rough; the head
removed and its hole closed; the shoulders wholly under the beige vest, the grey shirt's sleeves coming out of its
armholes; each type of fabric cleanly its own piece). The previous body (tools/body_tubes.py) is kept as
re/body/v1/ and the git tag body-v1.

The cage is the hand's method as before (quad tubes round the skeleton, rays onto the stock surface, all_inf_soldier_hi):
a torso grid of TORSO_COLS columns in rings tilted with the belt (the belt's two edges and the vest's hem as rings),
an opening at each shoulder, the legs split at the crotch. New:
- the vest's front opening: on every ring from the hem up, the columns +-LAPEL slid onto the paint's edge of the vest
  (the lapels), the columns between spread evenly inside: the opening's edges are whole columns;
- the shoulders' openings are the vest's armholes: an even ring round the arm (no seam snapping); the sleeve is a tube
  of its own from a ring tucked SLEEVE_TUCK inside the armhole;
- each leg has a ring at the boots' top (where the stock's texture becomes the boots' island, front and back
  averaged): the boots start there;
- the neck's opening closed by a flat quad cap (the head removed).
Pieces (every cage face one, a material each; split apart, no shared points): vest (rings from the hem up, outside the
opening), shirt (the opening's columns, the neck's cap), sleeves (the arm tubes), suit (the green: crotch to the hem,
the legs to the boots), belt (a copy of the suit's belt rings, PIECE_LIFT proud), boots (boot top to the toe cap).
Every edge of the vest, the belt and the boots turned under (RIM_DEPTH: an extra ring, held where it is). Then
Catmull-Clark x SUBD, every point not held back onto the stock along its normal, despiked, each piece lifted by its
PIECE_LIFT; weights from the stock (nearest point, smoothed); the texture baked from the stock's 4x texture onto the
body's own layout, each piece's texels not of its fabric's colour filled from its own neighbours (no crossover).
Saves r_body_quads (the body), r_body_pieces (a copy coloured by piece, for checking), r_body_packs."""
import bpy, bmesh, math, os, sys
import numpy as np
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree
from mathutils.geometry import barycentric_transform

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
TORSO_COLS = 32
LAPEL = 2                  # the opening's edges: columns +-LAPEL
CROTCH_POINTS = 3
ROW_STEP = 0.035
SUBD = 2
RELAX = 15
BANDS = ((1.140, 1.190), (1.200, 1.240), (1.235, 1.290))   # belt's lower and upper edge, the vest's hem (front, back z)
ARM_ROOT = 0.12            # the armhole ring this far along the upper arm
SLEEVE_TUCK, SLEEVE_INSET = 0.012, 0.004
UPPERARM_END = 0.80
VEST_SLEEVE_END = 0.40     # the vest's short sleeve ends this far along the upper arm
RIM_DEPTH = 0.003          # a turned-under edge's depth
PIECES = ('vest', 'shirt', 'sleeves', 'suit', 'belt', 'boots', 'jacket')
PIECE_LIFT = {'vest': 0.003, 'shirt': 0.0, 'sleeves': 0.0, 'suit': 0.0, 'belt': 0.005, 'boots': 0.0025,
              'jacket': 0.0045}
PIECE_COLOUR = {'jacket': (0.25, 0.48, 0.24), 'vest': (0.80, 0.68, 0.45), 'shirt': (0.92, 0.92, 0.92), 'sleeves': (0.55, 0.57, 0.62),
                'suit': (0.30, 0.48, 0.25), 'belt': (0.18, 0.18, 0.18), 'boots': (0.62, 0.52, 0.40)}
RIMMED = ('vest', 'belt', 'boots', 'jacket')
WEIGHT_SMOOTH = 25
BAKE_SIZE = 2048
HD_BODY = os.path.abspath('re/body/hd/All_Inf_trooper-hd.png')
BAKED = os.path.abspath('re/body/player_body.png')

scene = bpy.context.scene
hi = bpy.data.objects['all_inf_soldier_hi']
stock = bpy.data.objects['all_inf_soldier']
rig = bpy.data.objects['all_inf_soldier_skeleton']
mw = rig.matrix_world


def bone(name):
    b = rig.data.bones[name]
    return mw @ b.head_local, mw @ b.tail_local


def part_tree(bones, materials=(0,)):
    src = hi.data
    names = [g.name for g in hi.vertex_groups]
    major = []
    for v in src.vertices:
        g = max(v.groups, key=lambda x: x.weight) if v.groups else None
        major.append(names[g.group] if g else '')
    b_ = bmesh.new()
    b_.from_mesh(src)   # its own coordinates: the skeleton's (the object stands 1.3 m aside in the file)
    drop = [f for f in b_.faces if f.material_index not in materials or
            sum(1 for v in f.verts if major[v.index] in bones) * 2 <= len(f.verts)]
    bmesh.ops.delete(b_, geom=drop, context='FACES')
    return BVHTree.FromBMesh(b_)


TORSO_BONES = {'bone_pelvis', 'bone_a_spine', 'bone_b_spine', 'bone_ribcage', 'bone_l_clavicle', 'bone_r_clavicle',
               'bone_neck', 'bone_l_thigh', 'bone_r_thigh'}
torso_tree = part_tree(TORSO_BONES)
arm_tree = {s: part_tree({'bone_%s_upperarm' % s, 'bone_%s_clavicle' % s, 'bone_ribcage', 'bone_%s_forearm' % s})
            for s in 'lr'}
leg_tree = {s: part_tree({'bone_%s_thigh' % s, 'bone_%s_calf' % s, 'bone_%s_foot' % s, 'bone_%s_toe' % s,
                          'bone_pelvis'}) for s in 'lr'}
whole_tree = part_tree(TORSO_BONES | {'bone_%s_%s' % (s, b) for s in 'lr'
                                      for b in ('upperarm', 'forearm', 'calf', 'foot', 'toe')})


def ray(tree, origin, d, reach=0.35):
    loc = tree.ray_cast(origin, d, reach)[0]
    return loc if loc is not None else tree.find_nearest(origin + d * 0.05)[0]


# ---- the stock's texture at a surface point (which fabric is painted there) -----------------------------------------
uvb = bmesh.new()
uvb.from_mesh(hi.data)
bmesh.ops.triangulate(uvb, faces=uvb.faces[:])
uvb.faces.ensure_lookup_table()
uvb_l = uvb.loops.layers.uv.active
uvtree = BVHTree.FromBMesh(uvb)
# the stock's grey sleeve alone (its island: u under 0.36, v 0.23..0.525): the sleeves are built and projected on it
# (on the whole stock their tops lay on the vest's shoulder pad, which covers the sleeve there: a bulge)
_sb = uvb.copy()
_sl = _sb.loops.layers.uv.active
bmesh.ops.delete(_sb, geom=[f for f in _sb.faces if f.material_index != 0 or
                            not all(l[_sl].uv.x < 0.36 and 0.23 < l[_sl].uv.y < 0.525 for l in f.loops)], context='FACES')
sleeve_tree = BVHTree.FromBMesh(_sb)
cls_img = bpy.data.images.load('C:/LucasArts/BFBuilder/Assets/Sides/ALL/MSH/All_Inf_trooper.tga')
CW, CH = cls_img.size
cls_px = cls_img.pixels[:]


def stock_uv(p):
    loc, _, fi, _ = uvtree.find_nearest(p)
    f = uvb.faces[fi]
    a, b, c = (l.vert.co for l in f.loops)
    return barycentric_transform(loc, a, b, c, *(l[uvb_l].uv.to_3d() for l in f.loops)), f.material_index


# the vest's paint (idren2023 2026-10-25 outlined its regions on the stock texture: tools/body_vestmask.py ->
# re/body/vest_mask.png, the outlines tightened by colour): where the vest is, decided by the texture's place
# (since 2026-10-26 from the paint itself: tools/char_fabrics.py with tools/chars/all_inf_soldier.json, no outlines)
_vm = bpy.data.images.load(os.path.abspath('re/body/seg/vest_mask.png'))
VW, VH = _vm.size
vest_map = np.array(_vm.pixels[:], dtype=np.float32).reshape(VH, VW, 4)[..., 0] > 0.5
# the green jacket's and hood's paint (idren2023 outlined them too: tools/body_jacketmask.py -> re/body/jacket_mask.png);
# the hood lies on top of the vest, so where both maps claim a texel it is the jacket's
_jm = bpy.data.images.load(os.path.abspath('re/body/seg/jacket_mask.png'))
jacket_map = np.array(_jm.pixels[:], dtype=np.float32).reshape(VH, VW, 4)[..., 0] > 0.5
vest_map &= ~jacket_map
# the two as one image for the bake: red the vest's paint, green the jacket's
_pm = bpy.data.images.new('paint_maps', VW, VH, alpha=False)
_pm_px = np.zeros((VH, VW, 4), np.float32)
_pm_px[..., 0] = vest_map
_pm_px[..., 1] = jacket_map
_pm_px[..., 3] = 1
_pm.pixels[:] = _pm_px.ravel()


def _map_at(m, p):
    uv, mi = stock_uv(p)
    return mi == 0 and bool(m[min(int(uv.y % 1.0 * VH), VH - 1), min(int(uv.x % 1.0 * VW), VW - 1)])


def in_vest(p):
    return _map_at(vest_map, p)


def in_jacket(p):
    return _map_at(jacket_map, p)


def is_tan(p):
    uv, mi = stock_uv(p)
    if mi != 0:
        return False
    acc = [0.0, 0.0, 0.0]
    x0, y0 = int(uv.x % 1.0 * CW), int(uv.y % 1.0 * CH)
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            i = 4 * (min(max(y0 + dy, 0), CH - 1) * CW + min(max(x0 + dx, 0), CW - 1))
            for c_ in range(3):
                acc[c_] += cls_px[i + c_]
    r, g, b_ = (x / 25 for x in acc)
    return r - b_ > 0.10 and r >= g - 0.01


def is_boot(p):
    uv, mi = stock_uv(p)
    return mi == 0 and uv.x < 0.30 and uv.y < 0.22


bm = bmesh.new()
pid = bm.faces.layers.int.new('piece')     # each face's piece (PIECES index)
hl = bm.verts.layers.int.new('held')       # 1: the projection leaves the point where it is
underf = bm.faces.layers.int.new('under')  # 1: the jacket below the collar: under the vest (lifted less)
rimf = bm.faces.layers.int.new('rim_face')  # 1: a turned-under edge's strip (textured with its fold's colour)
counts = {}


def set_piece(f, name):
    f[pid] = PIECES.index(name)


def hold(v):
    v[hl] = 1

# ---- torso rows ---------------------------------------------------------------------------------------------------
pelvis_h, _ = bone('bone_pelvis')
neck_h, _ = bone('bone_neck')
sh_l, elbow_l = bone('bone_l_upperarm')
crotch = torso_tree.ray_cast(Vector((0, pelvis_h.y, pelvis_h.z)), Vector((0, 0, -1)), 1.0)[0]
z_bottom = crotch.z + 0.03
root = sh_l + (elbow_l - sh_l) * ARM_ROOT
arm_top = arm_tree['l'].ray_cast(root, Vector((0, 0, 1)), 0.3)[0]
arm_low = arm_tree['l'].ray_cast(root, Vector((0, 0, -1)), 0.3)[0]
z_pit, z_shtop = arm_low.z - 0.01, arm_top.z + 0.005
# the torso runs up over the shoulders' tops to the neck (its top ring there, closed by the cap: at the shoulders' height
# the cap lay over them)
z_top = z_shtop + 0.045
belt_mid = (BANDS[0][0] + BANDS[1][0]) / 2
belt_tilt = BANDS[0][1] - BANDS[0][0]


def tilt_at(z):
    if z <= belt_mid:
        return belt_tilt * max(0.0, (z - z_bottom) / (belt_mid - z_bottom))
    return belt_tilt * max(0.0, (z_pit - z) / (z_pit - belt_mid))


even = []
z = z_bottom
while z < z_top - ROW_STEP / 2:
    even.append(z)
    z += ROW_STEP
even.append(z_top)
for target in (z_pit, z_shtop):
    i = min(range(len(even)), key=lambda k: abs(even[k] - target))
    even[i] = target
rows = [(z, z + tilt_at(z)) for z in even
        if z in (z_pit, z_shtop) or all(abs(z - f) > 0.015 and abs(z + tilt_at(z) - b) > 0.015 for f, b in BANDS)]
rows = sorted(rows + list(BANDS))
pit_row = rows.index((z_pit, z_pit))
top_row = rows.index((z_shtop, z_shtop))
belt_lo, belt_hi, hem_row = (rows.index(b) for b in BANDS)
axis_y = pelvis_h.y
HOLE_HALF = 2
side_col = {'l': TORSO_COLS // 4, 'r': 3 * TORSO_COLS // 4}
STEP_A = 2 * math.pi / TORSO_COLS


def row_point(i, a):
    zf, zb = rows[i]
    o = Vector((0, axis_y, zf + (zb - zf) * (1 - math.cos(a)) / 2))
    return ray(torso_tree, o, Vector((math.sin(a), -math.cos(a), 0)))


def in_hole(i, k, inner=True):
    for s, c in side_col.items():
        dk = (k - c) % TORSO_COLS
        dk = dk - TORSO_COLS if dk > TORSO_COLS // 2 else dk
        if inner and pit_row < i < top_row and abs(dk) < HOLE_HALF:
            return s
        if not inner and pit_row <= i <= top_row and abs(dk) <= HOLE_HALF:
            return s
    return None


# the vest's front opening per ring from the hem up: from the front's middle outward to the first vest paint
def is_grey(p):
    uv, mi = stock_uv(p)
    if mi != 0:
        return False
    i_ = 4 * (min(int(uv.y % 1.0 * CH), CH - 1) * CW + min(int(uv.x % 1.0 * CW), CW - 1))
    r, g, b_ = cls_px[i_:i_ + 3]
    return max(r, g, b_) - min(r, g, b_) < 0.10 and (r + g + b_) / 3 > 0.40


def opening(i):
    """The vest's edge either side of the front's middle, and inside it the green jacket's edge against the grey
    T-shirt (idren2023 2026-10-25: three layers: the grey jacket, a green jacket / hoodie over it, the vest over both):
    (vest left, vest right, jacket left, jacket right) angles from the middle."""
    vest_, inner_ = [], []
    for sign in (1, -1):
        a_edge = (LAPEL + 0.9) * STEP_A
        a_in = None
        for step in range(1, 60):
            a = sign * step * math.radians(1.0)
            if abs(a) > (LAPEL + 0.9) * STEP_A:
                break
            p_ = row_point(i, a)
            if a_in is None and not is_grey(p_):
                a_in = abs(a)
            if in_vest(p_):
                a_edge = abs(a)
                break
        a_edge = max(a_edge, 0.5 * STEP_A)
        a_in = min(a_in if a_in is not None else a_edge * 0.5, a_edge * 0.8)
        vest_.append(a_edge)
        inner_.append(max(a_in, 0.15 * STEP_A))
    return vest_ + inner_


def column_angle(i, k):
    """Columns -2 / +2 on the vest's edges, -1 / +1 on the jacket's inner edges, 0 between (from the hem up)."""
    kk = k if k <= TORSO_COLS // 2 else k - TORSO_COLS          # -16..16
    if i >= hem_row and abs(kk) <= LAPEL:
        vl, vr, jl, jr = open_cache[i]
        return {-2: -vr, -1: -jr, 0: (jl - jr) / 2, 1: jl, 2: vl}[kk]
    return kk * STEP_A


raw_open = {i: opening(i) for i in range(hem_row, len(rows))}
# the paint's edge is noisy ring to ring: each ring's opening the mean over its neighbours (two each way)
open_cache = {}
for i in raw_open:
    near_ = [raw_open[j] for j in range(i - 2, i + 3) if j in raw_open]
    open_cache[i] = tuple(sum(o_[c_] for o_ in near_) / len(near_) for c_ in range(4))
grid = {}
for i in range(len(rows)):
    for k in range(TORSO_COLS):
        if not in_hole(i, k):
            grid[i, k] = bm.verts.new(row_point(i, column_angle(i, k)))


# ---- the collar's lower edge on the paint: the hood's edge (back) and the vest's neckline (front) ------------------
# per column, the ring HOOD_RING moves to where the vest's paint ends going up (the hood / collar above it); the rings
# from WARP_FROM up to it are spread evenly below, the rings above it evenly up to the neck. The front's lapel and
# T-shirt columns stay as they are; the columns at the arm openings too (their rings are the openings' border).
HOOD_RING = len(rows) - 3
WARP_FROM = top_row - 6


def point_at(z, a):
    return ray(torso_tree, Vector((0, axis_y, z)), Vector((math.sin(a), -math.cos(a), 0)))


def warp_column(k):
    kk = k if k <= TORSO_COLS // 2 else k - TORSO_COLS
    if abs(kk) <= LAPEL:
        return False
    for c in side_col.values():
        dk = (k - c) % TORSO_COLS
        if min(dk, TORSO_COLS - dk) <= HOLE_HALF + 1:
            return False
    return all((i, k) in grid for i in range(WARP_FROM, len(rows)))


z_hood = {}
for k in range(TORSO_COLS):
    if not warp_column(k):
        continue
    a = column_angle(HOOD_RING, k)
    z_lo, z_hi = grid[WARP_FROM, k].co.z + 0.01, grid[len(rows) - 1, k].co.z - 0.012
    z = z_hi
    while z > z_lo and not in_vest(point_at(z, a)):
        z -= 0.003
    z_hood[k] = min(max(z + 0.0015, z_lo + 0.01), z_hi - 0.004)
# smoothed round the body (median of three, then mean of three) over the warped columns
ks_ = sorted(z_hood)
med = {k: sorted(z_hood.get((k + d) % TORSO_COLS, z_hood[k]) for d in (-1, 0, 1))[1] for k in ks_}
z_hood = {k: sum(med.get((k + d) % TORSO_COLS, med[k]) for d in (-1, 0, 1)) / 3 for k in ks_}
for k, zh in z_hood.items():
    a_of = {i: column_angle(i, k) for i in range(WARP_FROM, len(rows))}
    z0, z1 = grid[WARP_FROM, k].co.z, grid[len(rows) - 1, k].co.z
    n_lo, n_hi = HOOD_RING - WARP_FROM, len(rows) - 1 - HOOD_RING
    for i in range(WARP_FROM + 1, len(rows) - 1):
        z = z0 + (zh - z0) * (i - WARP_FROM) / n_lo if i <= HOOD_RING else zh + (z1 - zh) * (i - HOOD_RING) / n_hi
        grid[i, k].co = point_at(z, a_of[i])
print('BODY collar edge on the paint: %d columns, ring %d from %.3f to %.3f m' % (
    len(z_hood), HOOD_RING, min(z_hood.values()), max(z_hood.values())))

# the green hood (idren2023 2026-10-25: it lies on top of the vest round the neck and down over the clavicles): from the
# first ring above the shoulders' openings where most of the paint round the body (the front's middle left out) is
# not the vest's tan, up to the neck
hood_row = len(rows) - 2
for i in range(top_row, len(rows) - 1):
    cols_ = [k for k in range(TORSO_COLS) if abs(k if k <= TORSO_COLS // 2 else k - TORSO_COLS) > LAPEL and (i, k) in grid]
    green = sum(1 for k in cols_ if not in_vest(grid[i, k].co)) / max(len(cols_), 1)
    if green > 0.5:
        hood_row = i
        break


def cell_mid(i, k):
    c = [grid.get(x) for x in ((i, k), (i, (k + 1) % TORSO_COLS), (i + 1, (k + 1) % TORSO_COLS), (i + 1, k))]
    return sum((v.co for v in c if v), Vector()) / max(sum(1 for v in c if v), 1)


def cell_in_jacket(i, k):
    """Most of five points inside the cell (its middle, and halfway from it to each corner) on the jacket's paint."""
    c = [grid.get(x) for x in ((i, k), (i, (k + 1) % TORSO_COLS), (i + 1, (k + 1) % TORSO_COLS), (i + 1, k))]
    if None in c:
        return False
    mid = sum((v.co for v in c), Vector()) / 4
    return sum(in_jacket(q) for q in [mid] + [(mid + v.co) / 2 for v in c]) >= 3


jacket_cells = {(i, k) for i in range(belt_hi, len(rows) - 1) for k in range(TORSO_COLS) if cell_in_jacket(i, k)}


def _torso_piece(i, k):
    """idren2023 2026-10-25: the green jacket and its hood one piece over the vest, where its paint is (his outlines,
    tightened by colour): the hood on the back, the lapels, and the band between the vest's hem and the belt."""
    kk = k if k < TORSO_COLS // 2 else k - TORSO_COLS          # the cell between columns k and k + 1
    if i >= hem_row:
        if -1 <= kk < 1:
            return 'shirt'                      # the grey T-shirt in the middle
        if kk in (-2, 1) or i >= HOOD_RING:
            return 'jacket'                     # the lapels; the collar and hood above its edge (on the paint)
        return 'vest'
    if i >= belt_hi:
        # the band between the vest's hem and the belt the jacket's (idren2023's outline), the vest where its hem runs lower
        return 'vest' if in_vest(cell_mid(i, k)) else 'jacket'
    return 'suit'


# the jacket one piece: its cells that do not join up with the lapels, the collar or the band (specks of green paint
# on the vest) are the vest's
_cells = {(i, k): _torso_piece(i, k) for i in range(len(rows) - 1) for k in range(TORSO_COLS)}
_seen = [c for c, p_ in _cells.items() if p_ == 'jacket' and (c[0] >= hood_row or c[0] < hem_row)]
_joined = set(_seen)
while _seen:
    i, k = _seen.pop()
    for c in ((i + 1, k), (i - 1, k), (i, (k + 1) % TORSO_COLS), (i, (k - 1) % TORSO_COLS)):
        if _cells.get(c) == 'jacket' and c not in _joined:
            _joined.add(c)
            _seen.append(c)
_specks = [c for c, p_ in _cells.items() if p_ == 'jacket' and c not in _joined]
for c in _specks:
    _cells[c] = 'vest'
print('BODY jacket specks to the vest: %d' % len(_specks))


def torso_piece(i, k):
    return _cells[i, k]


belt_faces = []
vest_under = []
for i in range(len(rows) - 1):
    for k in range(TORSO_COLS):
        c = [(i, k), (i, (k + 1) % TORSO_COLS), (i + 1, (k + 1) % TORSO_COLS), (i + 1, k)]
        if all(in_hole(a, b, inner=False) for a, b in c) and in_hole(i, k, False) == in_hole(i + 1, (k + 1) % TORSO_COLS, False):
            continue
        if all(x in grid for x in c):
            f = bm.faces.new([grid[x] for x in c])
            set_piece(f, torso_piece(i, k))
            # idren2023 2026-10-26: no lapels on the stock: below the collar the jacket is the green seen between the vest
            # and the T-shirt, under the vest; only the collar and hood lie on top of it
            if torso_piece(i, k) == 'jacket' and i < HOOD_RING:
                f[underf] = 1
            # (no vest under the collar: idren2023 2026-10-26, the vest is soft and has no geometry once it goes under the
            # hoodie; it ends at the collar's edge)
            if False and torso_piece(i, k) == 'jacket' and i >= HOOD_RING and any(
                    torso_piece(i2, k2 % TORSO_COLS) == 'vest' for i2, k2 in ((i, k - 1), (i, k + 1), (i - 1, k))
                    if 0 <= i2 < len(rows) - 1):
                vest_under.append(f)
            if belt_lo <= i < belt_hi:
                belt_faces.append(f)
# the neck closed: a flat quad cap on the top ring (the head removed), a little domed
top = [grid[len(rows) - 1, k] for k in range(TORSO_COLS)]
ctop = sum((v.co for v in top), Vector()) / len(top)
na = TORSO_COLS // 4
nb_ = TORSO_COLS // 2 - na
brd = {}
k = 0
for x in range(na):
    brd[x, 0] = top[k]; k += 1
for y in range(nb_):
    brd[na, y] = top[k]; k += 1
for x in range(na, 0, -1):
    brd[x, nb_] = top[k]; k += 1
for y in range(nb_, 0, -1):
    brd[0, y] = top[k]; k += 1
cp = dict(brd)
for x in range(1, na):
    for y in range(1, nb_):
        u, v = x / na, y / nb_
        p = ((1 - v) * brd[x, 0].co + v * brd[x, nb_].co + (1 - u) * brd[0, y].co + u * brd[na, y].co
             - ((1 - u) * (1 - v) * brd[0, 0].co + u * (1 - v) * brd[na, 0].co + (1 - u) * v * brd[0, nb_].co
                + u * v * brd[na, nb_].co))
        p.z += 0.02 * math.sin(math.pi * u) * math.sin(math.pi * v)
        cp[x, y] = bm.verts.new(p)
        hold(cp[x, y])
for x in range(na):
    for y in range(nb_):
        f = bm.faces.new([cp[x, y], cp[x + 1, y], cp[x + 1, y + 1], cp[x, y + 1]])
        set_piece(f, 'shirt')
counts['torso'] = len(bm.faces)


# ---- tubes --------------------------------------------------------------------------------------------------------
def transport(frames_t, ref):
    out = []
    r = ref
    for t in frames_t:
        r = (r - t * r.dot(t)).normalized()
        out.append(r)
    return out


def tube(first, axis_pts, tree, piece_at, close_cap=False, inset=lambda j: 0.0):
    """A tube from the ring `first` along axis_pts (the first point its centre); piece_at(j) names the piece of the
    faces between ring j and j + 1."""
    tans = []
    for j in range(len(axis_pts)):
        a, b = axis_pts[max(0, j - 1)], axis_pts[min(len(axis_pts) - 1, j + 1)]
        tans.append((b - a).normalized())
    c0 = axis_pts[0]
    refs = transport(tans, first[0].co - c0)
    t0 = tans[0]
    side0 = t0.cross(refs[0])
    ang0 = []
    for v in first:
        q = v.co - c0
        q = q - t0 * q.dot(t0)
        ang0.append(math.atan2(q.dot(side0), q.dot(refs[0])))
    for k in range(1, len(ang0)):
        while ang0[k] < ang0[k - 1] - math.pi:
            ang0[k] += 2 * math.pi
        while ang0[k] > ang0[k - 1] + math.pi:
            ang0[k] -= 2 * math.pi
    turn = 1 if ang0[-1] > ang0[0] else -1
    n = len(first)
    evn = [ang0[0] + turn * 2 * math.pi * k / n for k in range(n)]
    prev = list(first)
    rings = [prev]
    for j in range(1, len(axis_pts)):
        e = min(1.0, j / 4.0)
        side = tans[j].cross(refs[j])
        ring = []
        for k in range(n):
            a = ang0[k] * (1 - e) + evn[k] * e
            d = refs[j] * math.cos(a) + side * math.sin(a)
            d = d.normalized()
            ring.append(bm.verts.new(ray(tree, axis_pts[j], d, 0.3) - d * inset(j)))
        for k in range(n):
            f = bm.faces.new([prev[k], prev[(k + 1) % n], ring[(k + 1) % n], ring[k]])
            set_piece(f, piece_at(j - 1))
        prev = ring
        rings.append(ring)
    if close_cap:
        cap(prev, axis_pts[-1], tans[-1], tree, piece_at(len(axis_pts) - 1))
    return rings


def cap(ring, centre, tan, tree, piece):
    n = len(ring)
    a = n // 4
    b = n // 2 - a
    border = {}
    k = 0
    for x in range(a):
        border[x, 0] = ring[k]; k += 1
    for y in range(b):
        border[a, y] = ring[k]; k += 1
    for x in range(a, 0, -1):
        border[x, b] = ring[k]; k += 1
    for y in range(b, 0, -1):
        border[0, y] = ring[k]; k += 1
    pts = dict(border)
    for x in range(1, a):
        for y in range(1, b):
            u, v = x / a, y / b
            p = ((1 - v) * border[x, 0].co + v * border[x, b].co + (1 - u) * border[0, y].co + u * border[a, y].co
                 - ((1 - u) * (1 - v) * border[0, 0].co + u * (1 - v) * border[a, 0].co
                    + (1 - u) * v * border[0, b].co + u * v * border[a, b].co))
            loc = tree.ray_cast(p - tan * 0.05, tan, 0.15)[0] or tree.find_nearest(p)[0]
            pts[x, y] = bm.verts.new(loc)
    for x in range(a):
        for y in range(b):
            f = bm.faces.new([pts[x, y], pts[x + 1, y], pts[x + 1, y + 1], pts[x, y + 1]])
            set_piece(f, piece)


def hole_border(s):
    c = side_col[s]
    ks = [(c + d) % TORSO_COLS for d in range(-HOLE_HALF, HOLE_HALF + 1)]
    out = [grid[pit_row, k] for k in ks]
    out += [grid[i, ks[-1]] for i in range(pit_row + 1, top_row + 1)]
    out += [grid[top_row, k] for k in reversed(ks[:-1])]
    out += [grid[i, ks[0]] for i in range(top_row - 1, pit_row, -1)]
    return out


# ---- the armholes and the sleeves -----------------------------------------------------------------------------------
arm_rings = {}
for s in 'lr':
    sh, el = bone('bone_%s_upperarm' % s)
    border = hole_border(s)
    root_c = sh + (el - sh) * ARM_ROOT
    t = (el - sh).normalized()
    ref = (border[0].co - root_c)
    ref = (ref - t * ref.dot(t)).normalized()
    side_ = t.cross(ref)
    angs = [math.atan2((v.co - root_c).dot(side_), (v.co - root_c).dot(ref)) for v in border]
    turn = 1 if ((angs[1] - angs[0] + math.pi) % (2 * math.pi) - math.pi) > 0 else -1
    radial = []
    for k, v in enumerate(border):
        a_ = turn * 2 * math.pi * k / len(border)
        d = ref * math.cos(a_) + side_ * math.sin(a_)
        v.co = ray(arm_tree[s], root_c, d, 0.3)
        radial.append(d)
    # the vest's short sleeve (idren2023 2026-10-25: it wraps the upper arm over the deltoid): the vest carries on from
    # the armhole along the arm to VEST_SLEEVE_END, on the stock's outer surface (its shoulder pad)
    # the short sleeve's end: where the vest's shoulder-pad paint ends along the arm (top, front, back; the mean)
    ends = []
    for dvec in (Vector((0, 0, 1)), Vector((0, -1, 0.3)), Vector((0, 1, 0.3))):
        dd = (dvec - t * dvec.dot(t)).normalized()
        lo, hi_ = ARM_ROOT, 0.8
        for _ in range(10):
            mid = (lo + hi_) / 2
            if in_vest(ray(arm_tree[s], sh + (el - sh) * mid, dd, 0.3)):
                lo = mid
            else:
                hi_ = mid
        ends.append((lo + hi_) / 2)
    sleeve_end = max(ARM_ROOT + 0.08, sum(ends) / len(ends))
    print('BODY vest sleeve %s ends at %.2f of the upper arm (top %.2f, front %.2f, back %.2f)' % (s, sleeve_end, *ends))
    vest_f = [ARM_ROOT + (sleeve_end - ARM_ROOT) * j / 3 for j in range(1, 4)]
    vest_st = [root_c] + [sh + (el - sh) * f for f in vest_f]
    vest_rings = tube(border, vest_st, arm_tree[s], lambda j: 'vest')
    end_ring, end_c = vest_rings[-1], vest_st[-1]
    # the grey sleeve: its own tube from a ring tucked under the short sleeve's end, kept under it for its first rings
    radial = []
    for v in end_ring:
        q = v.co - end_c
        radial.append((q - t * q.dot(t)).normalized())
    start = [bm.verts.new(v.co - r_ * SLEEVE_INSET - t * SLEEVE_TUCK) for v, r_ in zip(end_ring, radial)]
    for v in start:
        hold(v)
    first = sleeve_end + 0.004 / (el - sh).length
    stations = [end_c - t * SLEEVE_TUCK] + [sh + (el - sh) * f for f in
                                           [first] + [first + (UPPERARM_END - first) * j / 7 for j in range(1, 8)]]
    arm_rings[s] = tube(start, stations, sleeve_tree, lambda j: 'sleeves',
                        inset=lambda j: SLEEVE_INSET * max(0.0, 1 - (j - 1) / 2.0))
    for ring in arm_rings[s][1:3]:
        for v in ring:
            hold(v)
counts['arms'] = len(bm.faces) - counts['torso']

# ---- the legs, with a ring at the boots' top ------------------------------------------------------------------------
low = [grid[0, k] for k in range(TORSO_COLS)]
yc = sum(v.co.y for v in low) / len(low)
crotch_pts = []
for j in range(1, CROTCH_POINTS + 1):
    phi = math.pi * j / (CROTCH_POINTS + 1)
    d = Vector((0, -math.cos(phi), -math.sin(phi)))
    crotch_pts.append(bm.verts.new(ray(torso_tree, Vector((0, yc, z_bottom)), d, 0.3)))
half = TORSO_COLS // 2


def bez(p0, p1, p2, p3, t):
    return (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t ** 2 * p2 + t ** 3 * p3


boot_ring = {}
for s, ks in (('l', list(range(0, half + 1))), ('r', [0] + list(range(TORSO_COLS - 1, half - 1, -1)))):
    first = [low[k] for k in ks] + list(reversed(crotch_pts))
    if s == 'r':
        first = list(reversed(first))
    hip, knee = bone('bone_%s_thigh' % s)
    _, ankle = bone('bone_%s_calf' % s)
    _, toe = bone('bone_%s_foot' % s)
    centre = sum((v.co for v in first), Vector()) / len(first)
    # the boots' top on the calf: front and back, bisected along the calf where the stock becomes the boots' island
    tops = []
    for dsign in (-1, 1):
        lo, hi_ = 0.0, 1.0
        for _ in range(12):
            mid = (lo + hi_) / 2
            q = ray(leg_tree[s], knee.lerp(ankle, mid), Vector((0, dsign, 0)), 0.3)
            if is_boot(q):
                hi_ = mid
            else:
                lo = mid
        tops.append((lo + hi_) / 2)
    f_boot = sum(tops) / 2
    pts = [centre.lerp(knee, f) for f in (0, 0.25, 0.5, 0.75)]
    calf_f = sorted(set([0, 0.2, 0.4, 0.6, 0.75] + [f_boot]))
    calf_f = [f for f in calf_f if f == f_boot or abs(f - f_boot) > 0.06]
    pts += [knee.lerp(ankle, f) for f in calf_f]
    boot_j = 4 + calf_f.index(f_boot)
    heel_turn = ankle + Vector((0, -0.02, -0.03))
    for j in range(1, 7):
        pts.append(bez(knee.lerp(ankle, 0.75), ankle + Vector((0, 0, -0.02)), heel_turn + Vector((0, -0.05, -0.02)),
                       Vector((toe.x, toe.y + 0.02, toe.z + 0.03)), j / 6))
    rings_ = tube(first, pts, leg_tree[s], lambda j, bj=boot_j: 'boots' if j >= bj else 'suit', close_cap=True)
    boot_ring[s] = rings_[boot_j]
    print('BODY leg %s: the boots from %.2f of the calf (front %.2f, back %.2f)' % (s, f_boot, tops[0], tops[1]))
counts['legs'] = len(bm.faces) - counts['torso'] - counts['arms']

# ---- relax the cage (the crowded points spread), the edges and held points kept -------------------------------------
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
bm.verts.ensure_lookup_table()
nbrs = [[e.other_vert(v) for e in v.link_edges] for v in bm.verts]
lapel_cols = {grid[i, k % TORSO_COLS] for i in range(hem_row, len(rows)) for k in range(-LAPEL, LAPEL + 1)
              if (i, k % TORSO_COLS) in grid}
band_verts = {grid[i, k] for i in (belt_lo, belt_hi, hem_row) for k in range(TORSO_COLS) if (i, k) in grid}
for v in lapel_cols | band_verts | {v for r_ in boot_ring.values() for v in r_}:
    v.tag = True
fixed = [any(e.is_boundary for e in v.link_edges) or v[hl] or v.tag for v in bm.verts]
for _ in range(RELAX):
    new = []
    for v, ns, f in zip(bm.verts, nbrs, fixed):
        if f or not ns:
            new.append(v.co.copy())
            continue
        mean = sum((n.co for n in ns), Vector()) / len(ns)
        new.append(whole_tree.find_nearest(v.co.lerp(mean, 0.5))[0])
    for v, p in zip(bm.verts, new):
        v.co = p

# ---- the borders between pieces onto the paint's outline ------------------------------------------------------------
# a cage point on the border between two pieces moves across the border (along the surface) to where the paint changes
# from one fabric to the other, so the pieces' edges follow the paint instead of stepping cell by cell; the points
# beside them are relaxed after. Fabrics with a map: the vest and the jacket.
FABRIC_TEST = {'vest': in_vest, 'jacket': in_jacket}
SNAP_REACH, SNAP_STEP = 0.025, 0.002
bm.normal_update()
snapped = []
border = set()
why = {}
for v in bm.verts:
    ps = {f[pid] for f in v.link_faces}
    if len(ps) == 2:
        nm_ = '/'.join(sorted(PIECES[x] for x in ps))
        r_ = 'held' if v[hl] else 'tagged' if v.tag else 'free'
        why[nm_, r_] = why.get((nm_, r_), 0) + 1
    if len(ps) != 2 or v[hl] or v.tag:
        if len(ps) >= 2:
            border.add(v)
        continue
    border.add(v)
    a_, b_ = sorted(ps)
    if PIECES[a_] not in FABRIC_TEST:
        a_, b_ = b_, a_
    test = FABRIC_TEST.get(PIECES[a_])
    if test is None:
        continue
    ca = sum((f.calc_center_median() for f in v.link_faces if f[pid] == a_), Vector())
    cb = sum((f.calc_center_median() for f in v.link_faces if f[pid] == b_), Vector())
    ca /= sum(1 for f in v.link_faces if f[pid] == a_)
    cb /= sum(1 for f in v.link_faces if f[pid] == b_)
    d = cb - ca
    d -= v.normal * d.dot(v.normal)
    if d.length < 1e-6:
        continue
    d.normalize()
    steps = int(SNAP_REACH / SNAP_STEP)
    at = lambda t: whole_tree.find_nearest(v.co + d * t)[0]
    inside = test(v.co)
    best = None
    for n_ in range(1, steps + 1):
        t = (n_ - 0.5) * SNAP_STEP if inside else -(n_ - 0.5) * SNAP_STEP
        if test(at(t)) != inside:
            best = t
            break
    if best is not None:
        snapped.append((v, at(best), abs(best)))
for v, p_, _ in snapped:
    v.co = p_
for _ in range(RELAX):
    new = []
    for v, ns, f in zip(bm.verts, nbrs, fixed):
        if f or v in border or not ns:
            new.append(v.co.copy())
            continue
        mean = sum((n.co for n in ns), Vector()) / len(ns)
        new.append(whole_tree.find_nearest(v.co.lerp(mean, 0.5))[0])
    for v, p in zip(bm.verts, new):
        v.co = p
print('BODY border points by pair and state: %s' % ', '.join('%s %s %d' % (a, b, n) for (a, b), n in sorted(why.items())))
print('BODY borders onto the paint: %d of %d border points moved (mean %.1f mm, most %.1f mm)' % (
    len(snapped), len(border), 1000 * sum(m_ for *_, m_ in snapped) / max(len(snapped), 1),
    1000 * max([m_ for *_, m_ in snapped] or [0])))

# ---- the pieces: the belt a copy over the suit; every piece split from its neighbours -----------------------------
dupv = bmesh.ops.duplicate(bm, geom=vest_under)
for f in [g for g in dupv['geom'] if isinstance(g, bmesh.types.BMFace)]:
    set_piece(f, 'vest')
dup = bmesh.ops.duplicate(bm, geom=belt_faces)
for f in [g for g in dup['geom'] if isinstance(g, bmesh.types.BMFace)]:
    set_piece(f, 'belt')
split = [e for e in bm.edges if len(e.link_faces) == 2 and e.link_faces[0][pid] != e.link_faces[1][pid]]
bmesh.ops.split_edges(bm, edges=split)
bm.normal_update()
# every edge of the vest, the belt and the boots turned under: the edge ring extruded RIM_DEPTH inward (held)
rim_edges = [e for e in bm.edges if e.is_boundary and PIECES[e.link_faces[0][pid]] in RIMMED]
crease_keys = []
# each edge point's normal from its own piece's faces, taken before the extrusion (the new points' normals are not
# set: they were pushed straight down 3 mm, the rim lay in the surface folded on itself, a sawtooth along every edge)
rim_normal = {}
for e in rim_edges:
    for v in e.verts:
        if v not in rim_normal:
            n_ = sum((f.normal for f in v.link_faces), Vector())
            rim_normal[v] = n_.normalized() if n_.length > 1e-6 else Vector((0, 0, 1))
ext = bmesh.ops.extrude_edge_only(bm, edges=rim_edges)
new_verts = [g for g in ext['geom'] if isinstance(g, bmesh.types.BMVert)]
for v in new_verts:
    src_v = next((e.other_vert(v) for e in v.link_edges if e.other_vert(v) in rim_normal), None)
    n_ = rim_normal[src_v] if src_v is not None else Vector((0, 0, 1))
    v.co = v.co - n_ * RIM_DEPTH
    hold(v)
for f in [g for g in ext['geom'] if isinstance(g, bmesh.types.BMFace)]:
    f[rimf] = 1
    other = next((l.edge.link_faces[0] for l in f.loops if l.edge.link_faces and l.edge.link_faces[0] is not f), None)
    f[pid] = other[pid] if other else 0
    f[underf] = other[underf] if other else 0
crease_keys = [e for e in rim_edges]
for i in range(len(rows) - 2, belt_lo - 1, -1):    # the torso's pieces ring by ring (k 0 the front's middle)
    print('BODY ring %2d %s' % (i, ''.join(torso_piece(i, k)[0] if (i, k) in grid else ' ' for k in range(TORSO_COLS))))
print('BODY jacket cells %d, hood ring %d of %d (the openings\' top %d)' % (len(jacket_cells), hood_row, len(rows), top_row))
print('BODY pieces: %s; %d edges turned under' % (
    ', '.join('%s %d' % (p_, sum(1 for f in bm.faces if PIECES[f[pid]] == p_)) for p_ in PIECES), len(rim_edges)))

# ---- the cage -> mesh: materials by piece, helper groups (held, lift) for the subdivision ---------------------------
bm.verts.index_update()
cage = bpy.data.meshes.new('r_body_quads')
for p_ in PIECES:
    m_ = bpy.data.materials.new('piece_' + p_)
    m_.diffuse_color = PIECE_COLOUR[p_] + (1.0,)
    cage.materials.append(m_)
for f in bm.faces:
    f.material_index = f[pid]
bm.to_mesh(cage)
ce = cage.attributes.new('crease_edge', 'FLOAT', 'EDGE')
ck = {frozenset((e.verts[0].index, e.verts[1].index)) for e in crease_keys if e.is_valid}
for e in cage.edges:
    if frozenset(e.vertices) in ck:
        ce.data[e.index].value = 1.0
ob = bpy.data.objects.new('r_body_quads', cage)
scene.collection.objects.link(ob)
g_held = ob.vertex_groups.new(name='_held')
g_lift = ob.vertex_groups.new(name='_lift')
for v in bm.verts:
    if v[hl]:
        g_held.add([v.index], 1.0, 'REPLACE')
JACKET_UNDER_LIFT = 0.0015          # under the vest's 3 mm, over the T-shirt
vpiece = {}
vunder = set()
for f in bm.faces:
    for v in f.verts:
        vpiece[v.index] = PIECES[f[pid]]
        if f[underf]:
            vunder.add(v.index)
for i_, p_ in vpiece.items():
    lift_ = JACKET_UNDER_LIFT if p_ == 'jacket' and i_ in vunder else PIECE_LIFT[p_]
    if lift_ > 0:
        g_lift.add([i_], lift_ / 0.01, 'REPLACE')
sub = ob.modifiers.new('subd', 'SUBSURF')
sub.levels = SUBD
bpy.context.view_layer.objects.active = ob
for o_ in scene.objects:
    o_.select_set(o_ is ob)
bpy.ops.object.modifier_apply(modifier='subd')
me = ob.data
gi_held, gi_lift = ob.vertex_groups['_held'].index, ob.vertex_groups['_lift'].index
w_held = [next((g.weight for g in v.groups if g.group == gi_held), 0.0) for v in me.vertices]
w_lift = [next((g.weight for g in v.groups if g.group == gi_lift), 0.0) * 0.01 for v in me.vertices]
edge_faces = {}
for p_ in me.polygons:
    for ek in p_.edge_keys:
        edge_faces[ek] = edge_faces.get(ek, 0) + 1
boundary = {i for ek, c in edge_faces.items() if c == 1 for i in ek}
keep = {i for i, w in enumerate(w_held) if w > 0.3} | boundary
# the turned-under edges' fold lines kept as the subdivision draws them: projected point by point onto the stock (its
# own lapel ridges and folds) they came out ragged
_cr = me.attributes.get('crease_edge')
fold = set()
if _cr is not None:
    for e in me.edges:
        if _cr.data[e.index].value > 0.5:
            fold.update(e.vertices)
keep |= fold
print('BODY fold lines kept off the projection: %d points' % len(fold))
on_sleeve = set()
for p_ in me.polygons:
    if PIECES[p_.material_index] == 'sleeves':
        on_sleeve.update(p_.vertices)
# onto the stock along each point's normal (not the held ones)
for v in me.vertices:
    if v.index in keep:
        continue
    best = None
    for d in (v.normal, -v.normal):
        h_ = (sleeve_tree if v.index in on_sleeve else whole_tree).ray_cast(v.co, d, 0.03)[0]
        if h_ is not None and (best is None or (h_ - v.co).length < (best - v.co).length):
            best = h_
    v.co = best if best is not None else (sleeve_tree if v.index in on_sleeve else whole_tree).find_nearest(v.co)[0]
dnb = [[] for _ in me.vertices]
for e in me.edges:
    a_, b_ = e.vertices
    dnb[a_].append(b_)
    dnb[b_].append(a_)
despiked = 0
for _ in range(6):
    moved = []
    for i, v in enumerate(me.vertices):
        if len(dnb[i]) < 3 or i in keep:
            continue
        mean = sum((me.vertices[j].co for j in dnb[i]), Vector()) / len(dnb[i])
        span = sum((me.vertices[j].co - v.co).length for j in dnb[i]) / len(dnb[i])
        if (v.co - mean).length > 0.5 * span:
            moved.append((v, mean))
    for v, mean in moved:
        v.co = mean
    despiked += len(moved)
me.update()
for v, l_ in zip(me.vertices, w_lift):
    if l_ > 0:
        v.co += v.normal * l_
for n_ in ('_held', '_lift'):
    if ob.vertex_groups.get(n_):
        ob.vertex_groups.remove(ob.vertex_groups[n_])
for p_ in me.polygons:
    p_.use_smooth = True
piece_face = [p_.material_index for p_ in me.polygons]
print('BODY subdivided x%d: %d faces (%d quads), %d tris; %d points held, %d despike moves' % (
    SUBD, len(me.polygons), sum(1 for p_ in me.polygons if len(p_.vertices) == 4),
    sum(len(p_.vertices) - 2 for p_ in me.polygons), len(keep), despiked))

# ---- weights: the stock's at the nearest point, smoothed ------------------------------------------------------------
hb = bmesh.new()
hb.from_mesh(hi.data)
bmesh.ops.triangulate(hb, faces=hb.faces[:])
hb.faces.ensure_lookup_table()
hdl = hb.verts.layers.deform.active
htree = BVHTree.FromBMesh(hb)
hnames = [g.name for g in hi.vertex_groups]
weights = []
for v in me.vertices:
    loc, _, fi, _ = htree.find_nearest(v.co)
    f = hb.faces[fi]
    a, b, c = (x.co for x in f.verts)
    bc = barycentric_transform(loc, a, b, c, Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1)))
    w = {}
    for corner, k in zip(f.verts, bc):
        for g, x in corner[hdl].items():
            w[g] = w.get(g, 0.0) + x * max(k, 0.0)
    weights.append(w)
for _ in range(WEIGHT_SMOOTH):
    nxt = []
    for i, w in enumerate(weights):
        acc = dict(w)
        for j in dnb[i]:
            for g, x in weights[j].items():
                acc[g] = acc.get(g, 0.0) + x
        tot = sum(acc.values()) or 1.0
        nxt.append({g: x / tot for g, x in acc.items() if x / tot > 0.002})
    weights = nxt
groups = {}
for i, w in enumerate(weights):
    top_ = sorted(w.items(), key=lambda t_: -t_[1])[:4]
    tot = sum(x for _, x in top_) or 1.0
    for g, x in top_:
        name = hnames[g]
        if name not in groups:
            groups[name] = ob.vertex_groups.new(name=name)
        groups[name].add([i], x / tot, 'REPLACE')
ob.modifiers.new('skeleton', 'ARMATURE').object = rig
ob.parent = rig

# ---- a copy coloured by piece, for checking ----------------------------------------------------------------------
pv = bpy.data.objects.new('r_body_pieces', me.copy())
scene.collection.objects.link(pv)
pv.parent = rig
pv.modifiers.new('skeleton', 'ARMATURE').object = rig

# ---- the texture: the body's own layout, baked from the stock's 4x texture -------------------------------------------
uvl_ = me.uv_layers.new(name='UV')
me.uv_layers.active = uvl_
for o_ in scene.objects:
    o_.select_set(o_ is ob)
bpy.context.view_layer.objects.active = ob
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.003, area_weight=0.0, correct_aspect=True,
                         scale_to_bounds=False)
bpy.ops.object.mode_set(mode='OBJECT')
img_b = bpy.data.images.new('player_body', BAKE_SIZE, BAKE_SIZE, alpha=False)
mat_b = bpy.data.materials.new('player_body')
mat_b.use_nodes = True
nd = mat_b.node_tree.nodes.new('ShaderNodeTexImage')
mat_b.node_tree.nodes.active = nd  # (textured views draw the active image node)
nd.image = img_b
mat_b.node_tree.links.new(nd.outputs['Color'], mat_b.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
mat_b.node_tree.nodes.active = nd
me.materials.clear()
me.materials.append(mat_b)
for p_ in me.polygons:
    p_.material_index = 0
src = hi.copy()
src.data = hi.data.copy()
src.matrix_world = Matrix.Identity(4)
for m_ in list(src.modifiers):
    src.modifiers.remove(m_)
scene.collection.objects.link(src)
hd_img = bpy.data.images.load(HD_BODY)
for i_, m_ in enumerate(src.data.materials):
    if m_ and m_.name.lower() == 'all_inf_trooper.tga':
        mc = m_.copy()
        mc.use_nodes = True
        for n_ in mc.node_tree.nodes:
            if n_.type == 'TEX_IMAGE':
                n_.image = hd_img
        src.data.materials[i_] = mc
sbm = bmesh.new()
sbm.from_mesh(src.data)
bmesh.ops.delete(sbm, geom=[f for f in sbm.faces if f.material_index != 0], context='FACES')
sbm.to_mesh(src.data)
scene.render.engine = 'CYCLES'
scene.cycles.samples = 4
scene.cycles.device = 'CPU'
bk = scene.render.bake
bk.use_selected_to_active = True
bk.cage_extrusion = 0.03
bk.max_ray_distance = 0.08
bk.margin = 8
for o_ in scene.objects:
    o_.select_set(o_ in (ob, src))
bpy.context.view_layer.objects.active = ob
bpy.ops.object.bake(type='DIFFUSE', pass_filter={'COLOR'}, use_clear=True, target='IMAGE_TEXTURES')
# again with the vest map as the source's texture: each texel learns whether its colour came from the vest's paint
img_m = bpy.data.images.new('vest_source', BAKE_SIZE, BAKE_SIZE, alpha=False)
nd.image = img_m
for m_ in src.data.materials:
    if m_ and m_.use_nodes:
        for n_ in m_.node_tree.nodes:
            if n_.type == 'TEX_IMAGE' and n_.image is hd_img:
                n_.image = _pm
bpy.ops.object.bake(type='DIFFUSE', pass_filter={'COLOR'}, use_clear=True, target='IMAGE_TEXTURES')
nd.image = img_b
_mk = np.array(img_m.pixels[:], dtype=np.float32).reshape(BAKE_SIZE, BAKE_SIZE, 4)
from_vest, from_jacket = _mk[..., 0] > 0.5, _mk[..., 1] > 0.5
bpy.data.objects.remove(src)
scene.render.engine = 'BLENDER_WORKBENCH'

# each piece's texels that are not its fabric's colour filled from its own (no crossover); the shirt, suit and boots as
# baked (their paint mixes colours)
W_ = BAKE_SIZE
pix = np.array(img_b.pixels[:], dtype=np.float32).reshape(W_, W_, 4)
rgb = pix[..., :3]
lum_ = rgb @ np.array([0.3, 0.59, 0.11], dtype=np.float32)
sat_ = rgb.max(-1) - rgb.min(-1)
FABRIC = {'vest': from_vest,        # the vest's paint only (its map, baked)
          'sleeves': (sat_ < 0.09) & (lum_ > 0.33),
          'belt': lum_ < 0.40,
          'jacket': from_jacket,    # the jacket's paint only (its map, baked)
          'shirt': (sat_ < 0.12) & (lum_ > 0.35)}
uvd = me.uv_layers['UV'].data
for p_name, ok in FABRIC.items():
    pi_ = PIECES.index(p_name)
    mask = np.zeros((W_, W_), dtype=bool)
    for poly in me.polygons:
        if piece_face[poly.index] != pi_:
            continue
        pts_ = [uvd[li].uv for li in poly.loop_indices]
        for tri in ((0, 1, 2), (0, 2, 3)) if len(pts_) == 4 else ((0, 1, 2),):
            a_, b_, c_ = (pts_[i_] * W_ for i_ in tri)
            x0, x1 = int(max(0, min(a_.x, b_.x, c_.x))), int(min(W_ - 1, max(a_.x, b_.x, c_.x)) + 1)
            y0, y1 = int(max(0, min(a_.y, b_.y, c_.y))), int(min(W_ - 1, max(a_.y, b_.y, c_.y)) + 1)
            if x1 <= x0 or y1 <= y0:
                continue
            d_ = (b_.y - c_.y) * (a_.x - c_.x) + (c_.x - b_.x) * (a_.y - c_.y)
            if abs(d_) < 1e-9:
                continue
            ys, xs = np.mgrid[y0:y1, x0:x1]
            px_, py_ = xs + 0.5, ys + 0.5
            l1 = ((b_.y - c_.y) * (px_ - c_.x) + (c_.x - b_.x) * (py_ - c_.y)) / d_
            l2 = ((c_.y - a_.y) * (px_ - c_.x) + (a_.x - c_.x) * (py_ - c_.y)) / d_
            mask[y0:y1, x0:x1] |= (l1 >= -0.02) & (l2 >= -0.02) & (1 - l1 - l2 >= -0.02)
    valid = mask & ok
    bad = mask & ~valid
    total_bad = int(bad.sum())
    for _ in range(120):
        if not bad.any():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros((W_, W_), dtype=np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            sv = np.roll(np.roll(valid, dy, 0), dx, 1)
            acc += np.roll(np.roll(rgb, dy, 0), dx, 1) * sv[..., None]
            cnt += sv
        grow = bad & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        valid = valid | grow
        bad = bad & ~grow
    if bad.any() and valid.any():
        rgb[bad] = rgb[valid].mean(0)
    print('BODY fabric %s: %d of %d texels not its colour, filled from its own' % (p_name, total_bad, int(mask.sum())))
pix[..., :3] = rgb
img_b.pixels[:] = pix.ravel()
img_b.filepath_raw = BAKED
img_b.file_format = 'PNG'
img_b.save()
print('BODY baked: %d x %d -> %s' % (BAKE_SIZE, BAKE_SIZE, BAKED))
# the turned-under strips take the colour of the fold they hang from: each of their points the texture place of the
# nearest fold point (baked from the stock they showed whatever the bake's rays hit beside them: dark torn edges);
# after the bake and the fills, which their stretched places would spill over
_rf = me.attributes.get('rim_face')
if _rf is not None and fold:
    uvd_ = me.uv_layers['UV'].data
    fold_uv = {}
    for p_ in me.polygons:
        if not _rf.data[p_.index].value:
            for li, vi in zip(p_.loop_indices, p_.vertices):
                if vi in fold and vi not in fold_uv:
                    fold_uv[vi] = uvd_[li].uv.copy()
    kd_ = KDTree(len(fold_uv))
    for vi in fold_uv:
        kd_.insert(me.vertices[vi].co, vi)
    kd_.balance()
    n_rim = 0
    for p_ in me.polygons:
        if _rf.data[p_.index].value:
            n_rim += 1
            for li, vi in zip(p_.loop_indices, p_.vertices):
                uvd_[li].uv = fold_uv[vi] if vi in fold_uv else fold_uv[kd_.find(me.vertices[vi].co)[1]]
    print('BODY turned-under strips: %d faces textured from their folds' % n_rim)

# ---- the packs: the stock's low mesh (square boxes, its uvs as made) -------------------------------------------------
pb = bmesh.new()
pb.from_mesh(stock.data)
puv = pb.loops.layers.uv.active


def is_pack(f):
    if f.material_index == 1:
        return True
    uvs = [l[puv].uv for l in f.loops]
    return f.material_index == 0 and all(0.0 <= q.x <= 0.105 and 0.745 <= q.y <= 0.835 for q in uvs)


bmesh.ops.delete(pb, geom=[f for f in pb.faces if not is_pack(f)], context='FACES')
bmesh.ops.delete(pb, geom=[v for v in pb.verts if not v.link_faces], context='VERTS')
packs_me = bpy.data.meshes.new('r_body_packs')
pb.to_mesh(packs_me)
for m in stock.data.materials:
    packs_me.materials.append(m)
packs = bpy.data.objects.new('r_body_packs', packs_me)
scene.collection.objects.link(packs)
for g in stock.vertex_groups:
    packs.vertex_groups.new(name=g.name)
packs.parent = rig
packs.modifiers.new('skeleton', 'ARMATURE').object = rig
print('BODY cage: torso %d, arms %d, legs %d faces; rows %d' % (counts['torso'], counts['arms'], counts['legs'], len(rows)))
for o_ in (hi,):
    o_.hide_render = True
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
print('SAVED', OUT)
