"""Blender (blender -b --factory-startup re/body/all_inf_soldier.blend -P tools/body_tubes.py -- <out.blend> [sheet=<prefix>]):
the player's body rebuilt as clean quads (idren2023 2026-10-25: the body up to the hands' quality, our arms joined to it),
by the hand's method (tools/body_retopo.py): quad tubes round the skeleton projected onto the stock surface, so the
shape stays the artist's and only the edge flow changes; the texture is the stock's own layout (tools/body_uvlib.py).

Topology (the Rebel soldier in its T-pose: z up, facing -y; the head removed, first person):
- torso: rings of TORSO_COLS columns round the spine (column 0 at the front's middle), from just above the crotch to
  the neck's opening, at ROW_STEP and at the texture's bands (belt, the vest's hem; BANDS);
- the shoulders: on each side an opening in the torso's grid between the armpit and the shoulder's top (the thumb's
  opening on the hand): its border is the upper arm's first ring, the arm a tube along bone_*_upperarm to just short
  of the elbow, open there (our first-person forearm goes on: tools/body_assemble.py);
- the legs: the torso's lowest ring split at the front's and the back's middle; each leg's first ring is its half and
  a crotch line between the legs (shared by both), the leg a tube along thigh, calf and foot (a smooth curve through
  hip, knee, ankle and toe: the ankle's bend kept wider than the tube), closed at the toe by a quad cap.
Every vertex is placed by a ray from the tube's axis to the part's own surface (all_inf_soldier_hi, the stock
subdivided: packs, head and hands left out), then one cage -> SUBD Catmull-Clark levels, each vertex back onto the
surface (nearest point), the uvs and materials from the stock, seams kept. Object r_body_quads (parented to the
skeleton; weights are the next step). Prints counts per part; with sheet=: front, back, side, three-quarter, textured
with the edges drawn over (<prefix>-<view>.png)."""
import bpy, bmesh, math, os, sys
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree
from mathutils.geometry import intersect_point_line
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from body_uvlib import transfer_uv

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
SHEET = next((a[6:] for a in args[1:] if a.startswith('sheet=')), None)
TORSO_COLS = 32
CROTCH_POINTS = 3          # the crotch line's inner points (a leg's ring: TORSO_COLS / 2 + 1 + CROTCH_POINTS)
ROW_STEP = 0.035           # cage rings about this far apart (metres)
SUBD = 2                   # Catmull-Clark levels on the cage (about 9 mm quads)
RELAX = 15                 # cage passes: each point toward its neighbours' mean, back onto the surface
# the texture's bands as rings (front z, back z; measured on the stock: the belt rises 5 cm toward the back): belt's
# lower and upper edge, the vest's hem; their cage edges creased (crisp after subdivision)
BANDS = ((1.140, 1.190), (1.200, 1.240), (1.235, 1.290))
# raised as geometry by the texture's colour at each point (metres): the vest's padded cloth, the belt, the boots'
# gaiters; HEIGHT_SMOOTH passes soften the steps that are not on a creased ring
LIFT = {'vest': 0.003, 'belt': 0.004, 'gaiter': 0.002}
HEIGHT_SMOOTH = 2
# the painted cloth folds as low relief (idren2023 2026-10-25, as on the sleeve: tools/body_sleeve.py): the luminance's
# medium band (blurred by the first minus by the second, texels of the 512 texture: light raised, dark sunk), on the
# plain cloth only (the vest's tan, the shirt's and sleeves' grey: the trousers' camouflage would read as bumps)
FOLD_MM_BODY = 2.0
FOLD_BANDS_BODY = (5, 28)
WEIGHT_SMOOTH = 25          # passes of neighbour averaging on the weights (the stock's are nearly rigid at joints)
UPPERARM_END = 0.80        # the arm tube ends this far along the stock upper arm; tools/body_assemble.py lengthens it and bridges the elbow

scene = bpy.context.scene
hi = bpy.data.objects['all_inf_soldier_hi']
stock = bpy.data.objects['all_inf_soldier']
rig = bpy.data.objects['all_inf_soldier_skeleton']
mw = rig.matrix_world


def bone(name):
    b = rig.data.bones[name]
    return mw @ b.head_local, mw @ b.tail_local


# ---- the target surfaces: the stock's faces by their strongest bone --------------------------------------------------
def part_tree(bones, materials=(0,)):
    src = hi.data
    names = [g.name for g in hi.vertex_groups]
    major = []
    for v in src.vertices:
        g = max(v.groups, key=lambda x: x.weight) if v.groups else None
        major.append(names[g.group] if g else '')
    bm = bmesh.new()
    bm.from_mesh(src)  # its own coordinates: the skeleton's (the object stands 1.3 m aside in the file)
    drop = [f for f in bm.faces if f.material_index not in materials or
            sum(1 for v in f.verts if major[v.index] in bones) * 2 <= len(f.verts)]
    bmesh.ops.delete(bm, geom=drop, context='FACES')
    return BVHTree.FromBMesh(bm), bm


TORSO_BONES = {'bone_pelvis', 'bone_a_spine', 'bone_b_spine', 'bone_ribcage', 'bone_l_clavicle', 'bone_r_clavicle',
               'bone_neck', 'bone_l_thigh', 'bone_r_thigh'}
torso_tree, torso_bm = part_tree(TORSO_BONES)
arm_tree = {s: part_tree({'bone_%s_upperarm' % s, 'bone_%s_clavicle' % s, 'bone_ribcage', 'bone_%s_forearm' % s})[0]
            for s in 'lr'}
leg_tree = {s: part_tree({'bone_%s_thigh' % s, 'bone_%s_calf' % s, 'bone_%s_foot' % s, 'bone_%s_toe' % s,
                          'bone_pelvis'})[0] for s in 'lr'}
whole_tree, whole_bm = part_tree(TORSO_BONES | {'bone_%s_%s' % (s, b) for s in 'lr'
                                                 for b in ('upperarm', 'forearm', 'calf', 'foot', 'toe')})


def armhole_seams():
    """Per side, the stock's texture seam between the torso's island (the vest) and the arm's islands (sleeve,
    shoulder): segments (a, b) in the skeleton's space."""
    hb_ = bmesh.new()
    hb_.from_mesh(hi.data)
    uvl_ = hb_.loops.layers.uv.active
    dl_ = hb_.verts.layers.deform.active
    names_ = [g.name for g in hi.vertex_groups]
    hb_.faces.ensure_lookup_table()
    parent_ = list(range(len(hb_.faces)))

    def find(i):
        while parent_[i] != i:
            parent_[i] = parent_[parent_[i]]
            i = parent_[i]
        return i

    def same(e):
        a, b = e.link_faces
        if a.material_index != b.material_index:
            return False
        for v in e.verts:
            ua = next(l[uvl_].uv for l in a.loops if l.vert is v)
            ub = next(l[uvl_].uv for l in b.loops if l.vert is v)
            if (ua - ub).length > 1e-5:
                return False
        return True

    for e in hb_.edges:
        if len(e.link_faces) == 2 and same(e):
            a, b = e.link_faces
            parent_[find(a.index)] = find(b.index)
    size = {}
    for f in hb_.faces:
        size[find(f.index)] = size.get(find(f.index), 0) + 1
    torso_isl = max((r for r in size if hb_.faces[r].material_index == 0), key=lambda r: size[r])

    def bone_of(f):
        cnt = {}
        for v in f.verts:
            if v[dl_]:
                g = max(v[dl_].items(), key=lambda t: t[1])[0]
                cnt[names_[g]] = cnt.get(names_[g], 0) + 1
        return max(cnt, key=cnt.get) if cnt else ''

    out = {'l': [], 'r': []}
    for e in hb_.edges:
        if len(e.link_faces) != 2 or same(e):
            continue
        a, b = e.link_faces
        ia, ib = find(a.index), find(b.index)
        if (ia == torso_isl) == (ib == torso_isl):
            continue
        other = b if ia == torso_isl else a
        bn = bone_of(other)
        for sd in 'lr':
            if bn in ('bone_%s_upperarm' % sd, 'bone_%s_clavicle' % sd):
                out[sd].append((e.verts[0].co.copy(), e.verts[1].co.copy()))
    return out


def ray(tree, origin, d, reach=0.35):
    loc = tree.ray_cast(origin, d, reach)[0]
    if loc is None:
        loc = tree.find_nearest(origin + d * 0.05)[0]
    return loc


bm = bmesh.new()
counts = {}

# ---- torso ----------------------------------------------------------------------------------------------------------
pelvis_h, _ = bone('bone_pelvis')
neck_h, neck_t = bone('bone_neck')
hip_l, knee_l = bone('bone_l_thigh')
sh_l, elbow_l = bone('bone_l_upperarm')
# the crotch: the lowest point of the torso's surface under the pelvis's middle
crotch = torso_tree.ray_cast(Vector((0, pelvis_h.y, pelvis_h.z)), Vector((0, 0, -1)), 1.0)[0]
z_bottom = crotch.z + 0.03
z_top = neck_h.z + 0.01   # (0.04 pinched the collar's top ring into a fan)
# the shoulders' opening: from under the armpit to over the arm's top, measured on the arm's surface at its root
root = sh_l + (elbow_l - sh_l) * 0.12
arm_top = arm_tree['l'].ray_cast(root, Vector((0, 0, 1)), 0.3)[0]
arm_low = arm_tree['l'].ray_cast(root, Vector((0, 0, -1)), 0.3)[0]
z_pit, z_shtop = arm_low.z - 0.01, arm_top.z + 0.005
# rows as (front z, back z): even rows tilted as the belt near it, the tilt fading to level at the crotch and the
# armpit; the bands' rows exactly; the opening's edges level
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
# a support ring just under the vest's hem: the vest's step happens between the two
HEM_SUPPORT = (BANDS[2][0] - 0.004, BANDS[2][1] - 0.004)
rows = sorted(rows + list(BANDS) + [HEM_SUPPORT])
pit_row = rows.index((z_pit, z_pit))
top_row = rows.index((z_shtop, z_shtop))
band_rows = [rows.index(b) for b in BANDS]
axis_y = pelvis_h.y
HOLE_HALF = 2   # columns either side of the side's middle (the opening is 4 columns wide)


def torso_dir(k):
    a = 2 * math.pi * k / TORSO_COLS
    return Vector((math.sin(a), -math.cos(a), 0))   # column 0 the front (-y), TORSO_COLS / 4 the left side (+x)


side_col = {'l': TORSO_COLS // 4, 'r': 3 * TORSO_COLS // 4}


def in_hole(i, k, inner=True):
    for s, c in side_col.items():
        dk = (k - c) % TORSO_COLS
        dk = dk - TORSO_COLS if dk > TORSO_COLS // 2 else dk
        if inner and pit_row < i < top_row and abs(dk) < HOLE_HALF:
            return s
        if not inner and pit_row <= i <= top_row and abs(dk) <= HOLE_HALF:
            return s
    return None


grid = {}
for i, (zf, zb) in enumerate(rows):
    for k in range(TORSO_COLS):
        if in_hole(i, k):
            continue
        a = 2 * math.pi * k / TORSO_COLS
        o = Vector((0, axis_y, zf + (zb - zf) * (1 - math.cos(a)) / 2))
        grid[i, k] = bm.verts.new(ray(torso_tree, o, torso_dir(k)))
for i in range(len(rows) - 1):
    for k in range(TORSO_COLS):
        c = [(i, k), (i, (k + 1) % TORSO_COLS), (i + 1, (k + 1) % TORSO_COLS), (i + 1, k)]
        # a cell inside an opening: all four corners on its border or inside
        if all(in_hole(a, b, inner=False) for a, b in c) and in_hole(i, k, False) == in_hole(i + 1, (k + 1) % TORSO_COLS, False):
            continue
        if all(x in grid for x in c):
            bm.faces.new([grid[x] for x in c])
counts['torso'] = len(bm.faces)


# ---- tubes ----------------------------------------------------------------------------------------------------------
def transport(frames_t, ref):
    """Reference directions square to each tangent, carried along without twisting."""
    out = []
    r = ref
    for t in frames_t:
        r = (r - t * r.dot(t)).normalized()
        out.append(r)
    return out


def tube(first, axis_pts, tree, close_cap=False):
    """A tube from the ring of verts `first` along axis_pts (the first point the ring's own centre): each ring's
    columns at the first ring's angles about the axis, eased to even spacing over the first rings."""
    tans = []
    for j in range(len(axis_pts)):
        a, b = axis_pts[max(0, j - 1)], axis_pts[min(len(axis_pts) - 1, j + 1)]
        tans.append((b - a).normalized())
    c0 = axis_pts[0]
    ref0 = (first[0].co - c0)
    refs = transport(tans, ref0)
    t0 = tans[0]
    side0 = t0.cross(refs[0])
    ang0 = []
    for v in first:
        q = v.co - c0
        q = q - t0 * q.dot(t0)
        ang0.append(math.atan2(q.dot(side0), q.dot(refs[0])))
    # unwrap so the angles run one way round
    for k in range(1, len(ang0)):
        while ang0[k] < ang0[k - 1] - math.pi:
            ang0[k] += 2 * math.pi
        while ang0[k] > ang0[k - 1] + math.pi:
            ang0[k] -= 2 * math.pi
    turn = 1 if ang0[-1] > ang0[0] else -1
    n = len(first)
    even = [ang0[0] + turn * 2 * math.pi * k / n for k in range(n)]
    prev = list(first)
    rings = [prev]
    for j in range(1, len(axis_pts)):
        e = min(1.0, j / 4.0)
        side = tans[j].cross(refs[j])
        ring = []
        for k in range(n):
            a = ang0[k] * (1 - e) + even[k] * e
            d = refs[j] * math.cos(a) + side * math.sin(a)
            ring.append(bm.verts.new(ray(tree, axis_pts[j], d.normalized(), 0.3)))
        for k in range(n):
            bm.faces.new([prev[k], prev[(k + 1) % n], ring[(k + 1) % n], ring[k]])
        prev = ring
        rings.append(ring)
    if close_cap:
        cap(prev, axis_pts[-1], tans[-1], tree)
    return rings


def cap(ring, centre, tan, tree):
    """Closes a ring of 2(a + b) verts by an a x b quad patch, its inner points pushed onto the surface ahead."""
    n = len(ring)
    a = n // 4
    b = n // 2 - a
    # border walk: bottom (a), right (b), top (a), left (b)
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
            # Coons patch of the border
            p = ((1 - v) * border[x, 0].co + v * border[x, b].co + (1 - u) * border[0, y].co + u * border[a, y].co
                 - ((1 - u) * (1 - v) * border[0, 0].co + u * (1 - v) * border[a, 0].co
                    + (1 - u) * v * border[0, b].co + u * v * border[a, b].co))
            loc = tree.ray_cast(p - tan * 0.05, tan, 0.15)[0] or tree.find_nearest(p)[0]
            pts[x, y] = bm.verts.new(loc)
    for x in range(a):
        for y in range(b):
            bm.faces.new([pts[x, y], pts[x + 1, y], pts[x + 1, y + 1], pts[x, y + 1]])


def hole_border(s):
    """The opening's border verts in order round it."""
    c = side_col[s]
    ks = [(c + d) % TORSO_COLS for d in range(-HOLE_HALF, HOLE_HALF + 1)]
    out = [grid[pit_row, k] for k in ks]
    out += [grid[i, ks[-1]] for i in range(pit_row + 1, top_row + 1)]
    out += [grid[top_row, k] for k in reversed(ks[:-1])]
    out += [grid[i, ks[0]] for i in range(top_row - 1, pit_row, -1)]
    return out


SEAMS = armhole_seams()
arm_rings, leg_rings = {}, {}
seam_snapped = set()
print('BODY armhole seams: left %d, right %d stock edges' % (len(SEAMS['l']), len(SEAMS['r'])))
for s in 'lr':
    sh, el = bone('bone_%s_upperarm' % s)
    border = hole_border(s)
    # the border onto the arm's root: each point by a ray from the root's centre toward it (its own direction)
    root_c = sh + (el - sh) * 0.12
    t = (el - sh).normalized()
    for v in border:
        d = v.co - root_c
        d = (d - t * d.dot(t)).normalized()
        v.co = ray(arm_tree[s], root_c, d, 0.3)
    # then onto the armhole's texture seam (the vest's edge against the sleeve), each to its nearest point there, so
    # no quad crosses it (the seam's two sides take their own textures cleanly)
    segs = SEAMS[s]
    if segs:
        for v in border:
            best = None
            for a_, b_ in segs:
                q, f_ = intersect_point_line(v.co, a_, b_)
                q = a_ if f_ < 0 else b_ if f_ > 1 else q
                if best is None or (q - v.co).length < (best - v.co).length:
                    best = q
            v.co = best
    seam_snapped.update(border)
    # a ring just past the border (the vest's armhole: the vest steps down onto the sleeve between the two)
    first = 0.12 + 0.004 / (el - sh).length
    stations = [root_c + (el - sh) * f for f in [0.12, first] + [first + (UPPERARM_END - first) * j / 8 for j in range(1, 9)]]
    arm_rings[s] = tube(border, stations, arm_tree[s])
counts['torso+arms'] = len(bm.faces)

# legs: the lowest torso ring split; the crotch line between the legs
low = [grid[0, k] for k in range(TORSO_COLS)]
yc = sum(v.co.y for v in low) / len(low)
crotch_pts = []
for j in range(1, CROTCH_POINTS + 1):
    phi = math.pi * j / (CROTCH_POINTS + 1)      # from the front (-y) under the crotch to the back (+y)
    d = Vector((0, -math.cos(phi), -math.sin(phi)))
    crotch_pts.append(bm.verts.new(ray(torso_tree, Vector((0, yc, z_bottom)), d, 0.3)))
half = TORSO_COLS // 2
for s, ks in (('l', list(range(0, half + 1))), ('r', [0] + list(range(TORSO_COLS - 1, half - 1, -1)))):
    first = [low[k] for k in ks] + list(reversed(crotch_pts))
    if s == 'r':
        first = list(reversed(first))
    hip, knee = bone('bone_%s_thigh' % s)
    _, ankle = bone('bone_%s_calf' % s)
    _, toe = bone('bone_%s_foot' % s)
    centre = sum((v.co for v in first), Vector()) / len(first)

    def bez(p0, p1, p2, p3, t):
        return (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t ** 2 * p2 + t ** 3 * p3
    # the leg's axis: down the thigh and calf, then a wide bend at the ankle into the foot toward the toe
    pts = [centre.lerp(knee, f) for f in (0, 0.25, 0.5, 0.75)]
    pts += [knee.lerp(ankle, f) for f in (0, 0.2, 0.4, 0.6, 0.75)]
    heel_turn = ankle + Vector((0, -0.02, -0.03))
    for j in range(1, 7):
        pts.append(bez(knee.lerp(ankle, 0.75), ankle + Vector((0, 0, -0.02)), heel_turn + Vector((0, -0.05, -0.02)),
                       Vector((toe.x, toe.y + 0.02, toe.z + 0.03)), j / 6))
    leg_rings[s] = tube(first, pts, leg_tree[s], close_cap=True)
counts['all'] = len(bm.faces)

# ---- the vest as a garment layer (idren2023 2026-10-25: its hem, front edges and armholes as loops; no saw-teeth) ----
# each torso cage point from the hem up is vest or shirt by the texture's colour there; on each row the two points
# either side of a change slid round to where the paint changes (the vest's front edges follow the lapels), the shirt
# side's one VEST_STEP further on; the arm's border ring is the vest's armhole. The lift follows this after the
# subdivision (VEST_LIFT), stepping between these loops and their neighbours (the hem's support ring, the arm's ring
# just past its border).
VEST_LIFT, VEST_STEP, VEST_UNDER = 0.003, 0.004, 0.35   # 3 mm proud, a 4 mm step, the step's points 1 mm under
uvb = bmesh.new()
uvb.from_mesh(hi.data)
bmesh.ops.triangulate(uvb, faces=uvb.faces[:])
uvb.faces.ensure_lookup_table()
uvb_l = uvb.loops.layers.uv.active
uvtree = BVHTree.FromBMesh(uvb)
cls_img = bpy.data.images.load('C:/LucasArts/BFBuilder/Assets/Sides/ALL/MSH/All_Inf_trooper.tga')
CW, CH = cls_img.size
cls_px = cls_img.pixels[:]


def is_vest(p):
    loc, _, fi, _ = uvtree.find_nearest(p)
    f = uvb.faces[fi]
    if f.material_index != 0:
        return False
    a, b, c = (l.vert.co for l in f.loops)
    from mathutils.geometry import barycentric_transform
    uv = barycentric_transform(loc, a, b, c, *(l[uvb_l].uv.to_3d() for l in f.loops))
    acc = [0.0, 0.0, 0.0]
    x0, y0 = int(uv.x % 1.0 * CW), int(uv.y % 1.0 * CH)
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            i = 4 * (min(max(y0 + dy, 0), CH - 1) * CW + min(max(x0 + dx, 0), CW - 1))
            for c_ in range(3):
                acc[c_] += cls_px[i + c_]
    r, g, b_ = (x / 25 for x in acc)
    return uv.x > 0.35 and uv.y > 0.43 and r - b_ > 0.10 and r >= g - 0.01


hem_row = rows.index(BANDS[2])
vest = {}
vest_fixed = set()
for (i, k), v in grid.items():
    vest[v] = 1.0 if i >= hem_row and is_vest(v.co) else 0.0
for v in seam_snapped:
    vest[v] = 1.0


def row_point(i, a):
    zf, zb = rows[i]
    o = Vector((0, axis_y, zf + (zb - zf) * (1 - math.cos(a)) / 2))
    return ray(torso_tree, o, Vector((math.sin(a), -math.cos(a), 0)))


snapped_edges = 0
vest_edge, under = set(), set()     # the vest's edge points and the shirt's points a step under them
for i in range(hem_row, len(rows)):
    for k in range(TORSO_COLS):
        k2 = (k + 1) % TORSO_COLS
        a_v, b_v = grid.get((i, k)), grid.get((i, k2))
        if a_v is None or b_v is None or vest[a_v] == vest[b_v] or a_v in vest_fixed or b_v in vest_fixed \
                or a_v in seam_snapped or b_v in seam_snapped:
            continue
        lo, hi_ = 2 * math.pi * k / TORSO_COLS, 2 * math.pi * (k + 1) / TORSO_COLS
        lo_vest = vest[a_v] > 0.5
        for _ in range(8):
            mid = (lo + hi_) / 2
            if is_vest(row_point(i, mid)) == lo_vest:
                lo = mid
            else:
                hi_ = mid
        t_ = (lo + hi_) / 2
        r_ = (row_point(i, t_) - Vector((0, axis_y, rows[i][0]))).length or 0.15
        da = VEST_STEP / r_
        # the vest's point on the change, the shirt's point a step on, both kept by the relax
        if lo_vest:
            a_v.co, b_v.co = row_point(i, t_), row_point(i, min(t_ + da, 2 * math.pi * (k + 1) / TORSO_COLS))
        else:
            a_v.co, b_v.co = row_point(i, max(t_ - da, 2 * math.pi * k / TORSO_COLS)), row_point(i, t_)
        vest_fixed.update((a_v, b_v))
        (vest_edge if lo_vest else under).add(a_v)
        (under if lo_vest else vest_edge).add(b_v)
        snapped_edges += 1
print('BODY vest: %d cage points vest, %d front-edge changes snapped onto the paint' % (
    sum(1 for x in vest.values() if x > 0.5), snapped_edges))

# the boots' gaiters as a layer too (idren2023: the trousers tuck into the boots, but the boots were not raised): a leg's
# cage point is gaiter where the stock's texture there is the boots' island (u under 0.30, v under 0.22); along each
# column the change between two rings slid onto the island's edge (bisected), the trouser's point a step above it
def is_gaiter(p_):
    loc, _, fi, _ = uvtree.find_nearest(p_)
    f = uvb.faces[fi]
    a, b, c = (l.vert.co for l in f.loops)
    from mathutils.geometry import barycentric_transform
    uv = barycentric_transform(loc, a, b, c, *(l[uvb_l].uv.to_3d() for l in f.loops))
    return f.material_index == 0 and uv.x < 0.30 and uv.y < 0.22


gaiter_edges = 0
for s_, rings_ in leg_rings.items():
    for ring in rings_:
        for v in ring:
            vest[v] = 1.0 if is_gaiter(v.co) else 0.0
    n_ = len(rings_[0])
    for j in range(len(rings_) - 1):
        for k in range(n_):
            a_v, b_v = rings_[j][k], rings_[j + 1][k]
            if vest[a_v] == vest[b_v] or a_v in vest_fixed or b_v in vest_fixed:
                continue
            lo, hi_ = 0.0, 1.0
            a_g = vest[a_v] > 0.5
            for _ in range(8):
                mid = (lo + hi_) / 2
                q = leg_tree[s_].find_nearest(a_v.co.lerp(b_v.co, mid))[0]
                if is_gaiter(q) == a_g:
                    lo = mid
                else:
                    hi_ = mid
            t_ = (lo + hi_) / 2
            span = (b_v.co - a_v.co).length or 0.03
            step = min(VEST_STEP / span, 0.45)
            edge_ = leg_tree[s_].find_nearest(a_v.co.lerp(b_v.co, t_))[0]
            other = leg_tree[s_].find_nearest(a_v.co.lerp(b_v.co, t_ + step if a_g else t_ - step))[0]
            if a_g:
                a_v.co, b_v.co = edge_, other
                vest_edge.add(a_v)
                under.add(b_v)
            else:
                a_v.co, b_v.co = other, edge_
                vest_edge.add(b_v)
                under.add(a_v)
            vest_fixed.update((a_v, b_v))
            gaiter_edges += 1
print('BODY gaiters: %d column changes snapped onto the boots\' edge' % gaiter_edges)
# the layers' edges curl under: the points a step outside them sink (VEST_UNDER of the lift), and the cage's edges
# along each layer's edge are creased (crisp after the subdivision: no blending)
for v in under:
    vest[v] = -VEST_UNDER
for v in arm_rings.get('l', [[], []])[1] + arm_rings.get('r', [[], []])[1]:
    vest[v] = -VEST_UNDER
hs_row = rows.index(HEM_SUPPORT)
for k in range(TORSO_COLS):
    if (hs_row, k) in grid and vest.get(grid[hs_row, k], 0.0) <= 0:
        vest[grid[hs_row, k]] = -VEST_UNDER
edge_set = vest_edge | seam_snapped | {grid[hem_row, k] for k in range(TORSO_COLS) if (hem_row, k) in grid
                                       and vest.get(grid[hem_row, k], 0) > 0.5}
layer_creases = [(e.verts[0], e.verts[1]) for e in bm.edges if e.verts[0] in edge_set and e.verts[1] in edge_set]
print('BODY layers: %d edge points, %d creased cage edges, %d points curled under' % (
    len(edge_set), len(layer_creases), sum(1 for x in vest.values() if x < 0)))

# ---- the cage relaxed (the junctions' crowded points spread), subdivided, onto the surface, uvs ---------------------
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
bm.verts.ensure_lookup_table()
nbrs = [[e.other_vert(v) for e in v.link_edges] for v in bm.verts]
band_verts = {grid[i, k] for i in band_rows for k in range(TORSO_COLS) if (i, k) in grid}
fixed = [any(e.is_boundary for e in v.link_edges) or v in band_verts or v in seam_snapped or v in vest_fixed
         for v in bm.verts]
crease_pairs = [(grid[i, k], grid[i, (k + 1) % TORSO_COLS]) for i in band_rows for k in range(TORSO_COLS)
                if (i, k) in grid and (i, (k + 1) % TORSO_COLS) in grid]
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
cage_copy = bpy.data.meshes.new('r_body_quads_cage_view')
bm.to_mesh(cage_copy)
bm.verts.index_update()
crease_keys = {frozenset((a.index, b.index)) for a, b in crease_pairs + layer_creases}
cage = bpy.data.meshes.new('r_body_quads_cage')
bm.to_mesh(cage)
ce = cage.attributes.new('crease_edge', 'FLOAT', 'EDGE')
cage_vest = [vest.get(v, 0.0) for v in bm.verts]
cage_tree_bm = bmesh.new()
cage_tree_bm.from_mesh(cage)
cage_tree_bm.faces.ensure_lookup_table()
cage_tree = BVHTree.FromBMesh(cage_tree_bm)
for e in cage.edges:
    if frozenset(e.vertices) in crease_keys:
        ce.data[e.index].value = 1.0
ob = bpy.data.objects.new('r_body_quads', cage)
scene.collection.objects.link(ob)
sub = ob.modifiers.new('subd', 'SUBSURF')
sub.levels = SUBD
bpy.context.view_layer.objects.active = ob
for o_ in scene.objects:
    o_.select_set(o_ is ob)
bpy.ops.object.modifier_apply(modifier='subd')
me = ob.data
# onto the surface along each point's own normal (the nearest point folded faces under the vest's overhang on the
# shoulders), the nearest point where no ray finds it within 3 cm
for v in me.vertices:
    n = v.normal
    best = None
    for d in (n, -n):
        hit_ = whole_tree.ray_cast(v.co, d, 0.03)[0]
        if hit_ is not None and (best is None or (hit_ - v.co).length < (best - v.co).length):
            best = hit_
    v.co = best if best is not None else whole_tree.find_nearest(v.co)[0]
# despiking: where the stock's vest overlaps the shirt (armholes, hem) points flip between the two surfaces and fold
# quads into shards; a point standing out from its neighbours' middle by more than half their mean distance goes to it
DESPIKE = 6
dnb = [[] for _ in me.vertices]
for e in me.edges:
    a_, b_ = e.vertices
    dnb[a_].append(b_)
    dnb[b_].append(a_)
despiked = 0
for _ in range(DESPIKE):
    moved = []
    for i, v in enumerate(me.vertices):
        if len(dnb[i]) < 3:
            continue
        mean = sum((me.vertices[j].co for j in dnb[i]), Vector()) / len(dnb[i])
        span = sum((me.vertices[j].co - v.co).length for j in dnb[i]) / len(dnb[i])
        if (v.co - mean).length > 0.5 * span:
            moved.append((v, mean))
    for v, mean in moved:
        v.co = mean
    despiked += len(moved)
print('BODY despiked: %d moves over %d passes' % (despiked, DESPIKE))

# the stock's texture seams as our edges (idren2023 2026-10-25: quads by the seams between garments took the wrong
# texture): every stock edge where the two faces' uvs or materials differ, sampled every 2 mm; a point of ours nearer
# to one than SEAM_SNAP of its mean edge length goes onto it, so our quads lie on one side of each seam
from mathutils.kdtree import KDTree
SEAM_SNAP = 0.45
sb_ = bmesh.new()
sb_.from_mesh(hi.data)
s_uv = sb_.loops.layers.uv.active


def differs(e):
    a, b = e.link_faces
    if a.material_index != b.material_index:
        return True
    for v in e.verts:
        ua = next(l[s_uv].uv for l in a.loops if l.vert is v)
        ub = next(l[s_uv].uv for l in b.loops if l.vert is v)
        if (ua - ub).length > 1e-5:
            return True
    return False


seam_segs = [(e.verts[0].co.copy(), e.verts[1].co.copy()) for e in sb_.edges
             if len(e.link_faces) == 2 and differs(e)]
samples = []
for si, (a, b) in enumerate(seam_segs):
    n_ = max(1, int((b - a).length / 0.002))
    for t in range(n_ + 1):
        samples.append((a.lerp(b, t / n_), si))
kd = KDTree(len(samples))
for i, (q, _) in enumerate(samples):
    kd.insert(q, i)
kd.balance()
snapped = 0
for i, v in enumerate(me.vertices):
    if not dnb[i]:
        continue
    span = sum((me.vertices[j].co - v.co).length for j in dnb[i]) / len(dnb[i])
    q, idx, dist = kd.find(v.co)
    if dist < SEAM_SNAP * span:
        a, b = seam_segs[samples[idx][1]]
        p_, f_ = intersect_point_line(v.co, a, b)
        v.co = a if f_ < 0 else b if f_ > 1 else p_
        snapped += 1
print('BODY seams: %d stock seam edges; %d points onto them' % (len(seam_segs), snapped))
for p in me.polygons:
    p.use_smooth = True
for m in stock.data.materials:
    me.materials.append(m)
stock_mesh = stock.data.copy()
stock_mesh.transform(stock.matrix_world)
# the texture looked up from 2 mm outside (where the stock's garments overlap, the outer one: the vest over the
# shirt), then back
me.update()
outward = [v.normal.copy() * 0.002 for v in me.vertices]
for v, o_ in zip(me.vertices, outward):
    v.co += o_
seams, clamped = transfer_uv(me, stock_mesh)
for v, o_ in zip(me.vertices, outward):
    v.co -= o_
# ---- raised features by the texture's colour --------------------------------------------------------------------------
img = bpy.data.images.load('C:/LucasArts/BFBuilder/Assets/Sides/ALL/MSH/All_Inf_trooper.tga')
IW, IH = img.size
ipx = img.pixels[:]


def colour(u, v):
    # the mean of a 5 x 5 texel patch (the paint's grain left out)
    x0, y0 = int(u % 1.0 * IW), int(v % 1.0 * IH)
    acc = [0.0, 0.0, 0.0]
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            i = 4 * (min(max(y0 + dy, 0), IH - 1) * IW + min(max(x0 + dx, 0), IW - 1))
            for c in range(3):
                acc[c] += ipx[i + c]
    return [a / 25 for a in acc]


ARM_LINES = [(bone('bone_%s_upperarm' % sd)[0], (bone('bone_%s_upperarm' % sd)[1] - bone('bone_%s_upperarm' % sd)[0]).normalized())
             for sd in 'lr']


def on_arm(co):
    """Past the shoulder joint along an upper arm and near its line."""
    for sh_, d_ in ARM_LINES:
        sv = (co - sh_).dot(d_)
        if sv > 0.01 and ((co - sh_) - d_ * sv).length < 0.10:
            return True
    return False


def feature(u, v, z, mat, co=None):
    if mat != 0:
        return None
    r, g, b = colour(u, v)
    torso = u > 0.35 and v < 0.78
    if torso and 0.389 <= v <= 0.423:
        return 'belt'
    tan = r - b > 0.10 and r >= g - 0.01
    if False:   # the vest is a layer of its own now (VEST_LIFT)
        return 'vest'
    if v < 0.22 and u < 0.30 and tan:      # the calves' island: the boots' gaiters
        return 'gaiter'
    return None


def blur(a, r):
    def one(a_, stride, n_, count, step_line):
        out = a_[:]
        for line in range(count):
            base = line * step_line
            pre = [0.0]
            for k_ in range(n_):
                pre.append(pre[-1] + a_[base + k_ * stride])
            for k_ in range(n_):
                lo, hi_ = max(0, k_ - r), min(n_, k_ + r + 1)
                out[base + k_ * stride] = (pre[hi_] - pre[lo]) / (hi_ - lo)
        return out
    return one(one(a, 1, IW, IH, IW), IW, IH, IW, 1)


lum_img = [0.3 * ipx[4 * i] + 0.59 * ipx[4 * i + 1] + 0.11 * ipx[4 * i + 2] for i in range(IW * IH)]
fold_band = [x - y for x, y in zip(blur(lum_img, FOLD_BANDS_BODY[0]), blur(lum_img, FOLD_BANDS_BODY[1]))]
fold_scale = sorted(abs(x) for x in fold_band)[int(len(fold_band) * 0.98)] or 1.0


def fold_at(u, v):
    x, y = int(u % 1.0 * IW), int(v % 1.0 * IH)
    return max(-1.0, min(1.0, fold_band[min(y, IH - 1) * IW + min(x, IW - 1)] / fold_scale))


def plain_cloth(u, v, mat):
    if mat != 0:
        return False
    r, g, b = colour(u, v)
    tan = r - b > 0.10 and r >= g - 0.01
    grey = max(r, g, b) - min(r, g, b) < 0.06
    return (tan or grey) and not (u > 0.35 and 0.38 <= v <= 0.43)   # not the belt


uvd = me.uv_layers.active.data
height = [0.0] * len(me.vertices)
folds = [0.0] * len(me.vertices)
seen = [0] * len(me.vertices)
for poly in me.polygons:
    for li in poly.loop_indices:
        vi = me.loops[li].vertex_index
        f = feature(uvd[li].uv.x, uvd[li].uv.y, me.vertices[vi].co.z, poly.material_index, me.vertices[vi].co)
        height[vi] += LIFT.get(f, 0.0)
        u_, v_ = uvd[li].uv.x, uvd[li].uv.y
        if plain_cloth(u_, v_, poly.material_index):
            folds[vi] += FOLD_MM_BODY / 1000 * fold_at(u_, v_)
        seen[vi] += 1
height = [(h + fo) / max(n, 1) for h, fo, n in zip(height, folds, seen)]
vn = [[] for _ in me.vertices]
for e in me.edges:
    a, b = e.vertices
    vn[a].append(b)
    vn[b].append(a)
creased = set()
if me.attributes.get('crease_edge'):
    cdat = me.attributes['crease_edge'].data
    for e in me.edges:
        if cdat[e.index].value > 0.5:
            creased.update(e.vertices)
for _ in range(HEIGHT_SMOOTH):
    height = [h if i in creased else (h + sum(height[j] for j in vn[i]) / len(vn[i])) / 2 if vn[i] else h
              for i, h in enumerate(height)]
# the vest's layer: each point's vest share from the cage face it lies on (its corners by inverse distance),
# sharpened: it steps between the loops the cage set
vest_lift = []
for v in me.vertices:
    loc, _, fi, _ = cage_tree.find_nearest(v.co)
    f = cage_tree_bm.faces[fi]
    ws = [1.0 / max((c.co - loc).length, 1e-5) for c in f.verts]
    x = sum(w * cage_vest[c.index] for w, c in zip(ws, f.verts)) / sum(ws)
    if x >= 0:
        t_ = min(1.0, max(0.0, (x - 0.3) / 0.4))
        vest_lift.append(VEST_LIFT * t_ * t_ * (3 - 2 * t_))
    else:
        vest_lift.append(VEST_LIFT * x)     # curled under
height = [h + vl for h, vl in zip(height, vest_lift)]
for v, h in zip(me.vertices, height):
    v.co += v.normal * h
print('BODY raised: %d points (vest layer %.0f, belt %.0f, gaiters %.0f mm), %d on creased rings' % (
    sum(1 for h in height if h > 0.0005), VEST_LIFT * 1000, LIFT['belt'] * 1000, LIFT['gaiter'] * 1000,
    len(creased)))

# ---- weights: the stock's at the nearest point, then smoothed over the joints ----------------------------------------
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
    # barycentric weights of the face's corners
    from mathutils.geometry import barycentric_transform
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
        for j in vn[i]:
            for g, x in weights[j].items():
                acc[g] = acc.get(g, 0.0) + x
        tot = sum(acc.values()) or 1.0
        nxt.append({g: x / tot for g, x in acc.items() if x / tot > 0.002})
    weights = nxt
groups = {}
for i, w in enumerate(weights):
    top = sorted(w.items(), key=lambda t: -t[1])[:4]
    tot = sum(x for _, x in top) or 1.0
    for g, x in top:
        name = hnames[g]
        if name not in groups:
            groups[name] = ob.vertex_groups.new(name=name)
        groups[name].add([i], x / tot, 'REPLACE')
arm_mod = ob.modifiers.new('skeleton', 'ARMATURE')
arm_mod.object = rig
print('BODY weights: %d bones, up to 4 a point, %d smoothing passes' % (len(groups), WEIGHT_SMOOTH))

# ---- the packs: the stock's (high mesh) backpack, chest kit and belt pouches, as they are ---------------------------
pb = bmesh.new()
pb.from_mesh(stock.data)   # the low mesh: the high one rounded and skewed the backpack's boxes
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
pm = packs.modifiers.new('skeleton', 'ARMATURE')
pm.object = rig
for p_ in packs_me.polygons:
    p_.use_smooth = True
print('BODY packs: %d faces (%d tris) from the stock\'s high mesh' % (
    len(packs_me.polygons), sum(len(p_.vertices) - 2 for p_ in packs_me.polygons)))
quads = sum(1 for p in me.polygons if len(p.vertices) == 4)
print('BODY cage: torso %d faces, + arms %d, + legs %d; rows %d (crotch z %.3f, armpit %.3f, shoulder top %.3f)' % (
    counts['torso'], counts['torso+arms'], counts['all'], len(rows), crotch.z, z_pit, z_shtop))
print('BODY subdivided x%d: %d faces (%d quads), %d tris; uv: %d stock seams, %d corners clamped' % (
    SUBD, len(me.polygons), quads, sum(len(p.vertices) - 2 for p in me.polygons), seams, clamped))
ob.parent = rig

# ---- the body's own texture, baked (idren2023 2026-10-25: quads by the seams between garments took the wrong texture) ---
# the stock's uv seams cut across our quads wherever they run between our edges; so the body gets its own layout (one
# island per smooth stretch, cut only along our edges) and the soldier's 4x texture (re/body/hd) is baked onto it
# from the stock surface itself: each texel looks along the normal at the stock's surface (the outer garment where
# two overlap), whatever its seams. The stock's uvs stay as layer 'stock' (the features above used them).
BAKE_SIZE = 2048
BAKED = os.path.abspath('re/body/player_body.png')
HD_BODY = os.path.abspath('re/body/hd/All_Inf_trooper-hd.png')
me.uv_layers.active.name = 'stock'
baked_uv = me.uv_layers.new(name='baked')
me.uv_layers.active = baked_uv
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
body_faces = [p_.index for p_ in me.polygons if p_.material_index == 0]
me.materials.append(mat_b)
mb_index = len(me.materials) - 1
# the source: the stock's high mesh at the skeleton's place (it stands 1.3 m aside in the file), its body material
# on the 4x texture, only its body faces seen (packs and head left out of the rays)
src = hi.copy()
src.data = hi.data.copy()
src.matrix_world = Matrix.Identity(4)
for m_ in list(src.modifiers):
    src.modifiers.remove(m_)
scene.collection.objects.link(src)
hd_img = bpy.data.images.load(HD_BODY)
src_mats = {}
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
for p_ in me.polygons:
    if p_.material_index == 0:
        p_.material_index = mb_index
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
# (the armature modifier would pose nothing: the rest pose; keep the mesh as it is)
bpy.ops.object.bake(type='DIFFUSE', pass_filter={'COLOR'}, use_clear=True, target='IMAGE_TEXTURES')
img_b.filepath_raw = BAKED
img_b.file_format = 'PNG'
img_b.save()
bpy.data.objects.remove(src)
scene.render.engine = 'BLENDER_WORKBENCH'
print('BODY baked: %d faces onto their own layout, %d x %d -> %s (from the stock\'s 4x texture)' % (
    len(body_faces), BAKE_SIZE, BAKE_SIZE, BAKED))

# ---- the chest plate (idren2023 2026-10-25: square, modelled on top): a 1.5 mm plate with crisp edges over its painted
# place on the vest, its face the paint's own (the stock's texture, PLATE_UV), on the ribcage
PLATE_UV = (0.904, 0.623, 0.946, 0.660)
PLATE_THICK = 0.0015
sbm2 = bmesh.new()
sbm2.from_mesh(stock.data)
bmesh.ops.triangulate(sbm2, faces=sbm2.faces[:])
s_uv2 = sbm2.loops.layers.uv.active
cu, cv = (PLATE_UV[0] + PLATE_UV[2]) / 2, (PLATE_UV[1] + PLATE_UV[3]) / 2
from mathutils.geometry import intersect_point_tri_2d, barycentric_transform
plate_face = None
for f in sbm2.faces:
    uvs_ = [l[s_uv2].uv for l in f.loops]
    if f.material_index == 0 and intersect_point_tri_2d(Vector((cu, cv)), *uvs_):
        plate_face = f
        break
if plate_face:
    ps_ = [l.vert.co for l in plate_face.loops]
    us_ = [l[s_uv2].uv.to_3d() for l in plate_face.loops]
    at_uv = lambda u, v: barycentric_transform(Vector((u, v, 0)), *us_, *ps_)
    c3 = at_uv(cu, cv)
    du = (at_uv(cu + 0.01, cv) - c3) / 0.01        # metres per uv along u, v on the surface
    dv = (at_uv(cu, cv + 0.01) - c3) / 0.01
    nrm = du.cross(dv).normalized()
    if nrm.dot(c3 - Vector((0, c3.y, c3.z))) < 0 and nrm.y > 0:
        nrm = -nrm
    if nrm.y > 0:                                   # the chest faces -y
        nrm = -nrm
    # onto our surface (the vest's lift included) along the normal
    me.update()
    bvh_me = BVHTree.FromPolygons([v.co for v in me.vertices], [p_.vertices[:] for p_ in me.polygons])
    hit_ = bvh_me.ray_cast(c3 + nrm * 0.05, -nrm, 0.1)[0] or c3
    base = hit_ + nrm * 0.0003
    hu = du * (PLATE_UV[2] - PLATE_UV[0]) / 2
    hv = dv * (PLATE_UV[3] - PLATE_UV[1]) / 2
    # square and upright: its long side along the body's vertical (projected onto the surface), the other square to
    # it, each as long as painted
    up_ = Vector((0, 0, 1))
    up_ = (up_ - nrm * up_.dot(nrm)).normalized()
    across = up_.cross(nrm).normalized()
    if abs(hv.normalized().dot(up_)) > abs(hu.normalized().dot(up_)):
        hu, hv = across * hu.length * (1 if hu.dot(across) >= 0 else -1), up_ * hv.length * (1 if hv.dot(up_) >= 0 else -1)
    else:
        hu, hv = up_ * hu.length * (1 if hu.dot(up_) >= 0 else -1), across * hv.length * (1 if hv.dot(across) >= 0 else -1)
    corners = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    pb_ = bmesh.new()
    pb_.from_mesh(me)
    puv = pb_.loops.layers.uv.get('baked') or pb_.loops.layers.uv.active
    pdl = pb_.verts.layers.deform.active
    rib = ob.vertex_groups['bone_ribcage'].index
    top = [pb_.verts.new(base + nrm * PLATE_THICK + hu * a + hv * b) for a, b in corners]
    low = [pb_.verts.new(base - nrm * 0.001 + hu * a * 1.0 + hv * b * 1.0) for a, b in corners]
    for v in top + low:
        v[pdl][rib] = 1.0
    uvc = [(PLATE_UV[0] if a < 0 else PLATE_UV[2], PLATE_UV[1] if b < 0 else PLATE_UV[3]) for a, b in corners]
    f = pb_.faces.new(top)
    for l, uv in zip(f.loops, uvc):
        l[puv].uv = uv
    f.material_index = 0
    for k in range(4):
        k2 = (k + 1) % 4
        f = pb_.faces.new([low[k], low[k2], top[k2], top[k]])
        for l, uv in zip(f.loops, (uvc[k], uvc[k2], uvc[k2], uvc[k])):
            l[puv].uv = uv
        f.material_index = 0
    bmesh.ops.recalc_face_normals(pb_, faces=pb_.faces[-5:])
    pb_.to_mesh(me)
    print('BODY plate: %.0f x %.0f mm, %.1f mm thick, on the chest' % (hu.length * 2000, hv.length * 2000,
                                                                       PLATE_THICK * 1000))
else:
    print('BODY plate: its painted place not found')

if SHEET:
    for o in scene.objects:
        o.hide_render = o.name not in ('r_body_quads', 'r_body_packs')
    wire = bpy.data.objects.new('wire', cage_copy)
    scene.collection.objects.link(wire)
    wm = wire.modifiers.new('wire', 'WIREFRAME')
    wm.thickness = 0.002
    wm.use_replace = True
    black = bpy.data.materials.new('black')
    black.diffuse_color = (0.02, 0.02, 0.02, 1)
    wire.data.materials.clear()
    wire.data.materials.append(black)
    wire.hide_render = False
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.color_type = 'TEXTURE'
    scene.display.shading.light = 'STUDIO'
    scene.render.resolution_x, scene.render.resolution_y = 800, 1000
    cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.data.type = 'ORTHO'
    cam.data.ortho_scale = 2.3
    target = Vector((0, 0, 1.0))
    for name, d in (('front', (0, -1, 0.1)), ('back', (0, 1, 0.1)), ('side', (1, 0, 0.1)), ('3q', (0.6, -0.7, 0.4))):
        dv = Vector(d).normalized()
        cam.location = target + dv * 4
        cam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
        scene.render.filepath = os.path.abspath('%s-%s.png' % (SHEET, name))
        bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(wire)
    bpy.data.objects.remove(cam)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
print('SAVED', OUT)
