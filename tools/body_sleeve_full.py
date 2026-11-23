"""The arm as one sleeve (idren2023 2026-10-26: the elbow join of the stock upper arm and our forearm still showed; one
baggy jacket sleeve instead, the forearm texture's patch an elbow pad). On the assembled player (tools/body_assemble.py
output): per arm the old upper arm (tools/body_stock_prep.py), the elbow bridge and our sleeve's grey part are taken
away; in their place one 72-point tube from under the vest's shoulder pad to the cuff's rim, along the arm's bones:
- its girth: the old surface's, smoothed along the arm (no jog), widened (BAG) and hanging a little (HANG), folds
  bunching inside the elbow and gathering before the cuff, its last rings eased onto the cuff's own ring (joined);
- one texture layout on our first-person sleeve texture: u along the arm (top 0, the patch over the elbow at its own
  scale, the cuff's rim at 0.662), v around continuing the cuff's own columns, turned along the arm so the patch's
  middle lies on the point of the elbow; the patch raised PAD_MM as an elbow pad;
- its weights: each point the nearest old point's, then smoothed along the arm (a wide blend across the elbow).
Run: blender -b --factory-startup re/body/player_stock.blend -P tools/body_sleeve_full.py -- <out.blend> [sheet=<prefix>]
"""
import bpy, bmesh, math, os, sys
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
SHEET = next((a[6:] for a in args[1:] if a.startswith('sheet=')), None)
AROUND = 72
RING_STEP = 0.008
CUFF_U = 0.662                 # the cuff's rim in the sleeve texture (u along the arm)
PATCH_U = (0.166, 0.434)       # the patch in the sleeve texture (u along, v around)
PATCH_V = (0.303, 0.584)
U_PER_M = 2.0                  # the sleeve texture's own scale along the arm (u per metre: our forearm)
BAG = 1.0                      # the stock's own girth (it is baggy already)
HANG = 0.0                     # metres it hangs below the arm's line mid-way
PAD_MM = 2.5
V0, VSPAN = 0.03, 0.96          # the sleeve texture's band round the arm
BLEND_RINGS = 7                # rings eased onto the cuff's ring
WEIGHT_SMOOTH = 14
pl = bpy.data.objects['player']
rig = pl.parent
me = pl.data
arms_mat = next(i for i, m in enumerate(me.materials) if m and 'all_1st_inf_trooper_arms' in m.name.lower())
B = rig.data.bones
bm = bmesh.new()
bm.from_mesh(me)
uvl = bm.loops.layers.uv.active
dl = bm.verts.layers.deform.active
ua = bm.faces.layers.int.get('upper_arm')
gi = {g.name: g.index for g in pl.vertex_groups}
# the stock soldier's own arm (idren2023 2026-10-26: the arm should look like the stock's, a baggy jacket sleeve, as its
# quad remesh did): its sleeve island's surface, in the stock skeleton's rest frame, is the girth the new sleeve takes
import numpy as np  # noqa: E402
with bpy.data.libraries.load(os.path.abspath('re/body/all_inf_soldier.blend')) as (src_, dst_):
    dst_.objects = ['all_inf_soldier_hi', 'all_inf_soldier_skeleton']
stk, stk_rig = dst_.objects
_isl = np.load('re/body/seg/texmaps.npz')['island']
_N = _isl.shape[0]
_sb = bmesh.new()
_sb.from_mesh(stk.data)
_suv = _sb.loops.layers.uv.active
_keep = []
for f in _sb.faces:
    c_ = sum((l[_suv].uv for l in f.loops), Vector((0, 0))) / len(f.loops)
    if f.material_index == 0 and _isl[min(int((1 - c_.y % 1) * _N), _N - 1), min(int(c_.x % 1 * _N), _N - 1)] in (10, 11):
        _keep.append([v.index for v in f.verts])
stock_tree = BVHTree.FromPolygons([v.co.copy() for v in _sb.verts], _keep)
SB_ = stk_rig.data.bones


def sleeve_uv(l):
    """A loop's place in the sleeve texture (the player's arms texture holds it in its upper half)."""
    return l[uvl].uv.x, (l[uvl].uv.y - 0.5) * 2


def smooth01(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


report = []
for s in 'lr':
    sh = B['bone_%s_upperarm' % s].head_local.copy()
    el = B['%s_forearm' % s].head_local.copy()
    wr = B['%s_forearm' % s].tail_local.copy()
    d_up = (el - sh).normalized()
    d_fo = (wr - el).normalized()
    side_sign = 1 if el.x > 0 else -1
    # ---- the old arm surface: the upper arm, the bridge, our sleeve's grey ----------------------------------------
    old = []
    for f in bm.faces:
        if f.material_index == arms_mat:
            us = [sleeve_uv(l) for l in f.loops]
            if not (f.calc_center_median().x * side_sign > 0.15):
                continue
            along_ = (f.calc_center_median() - el).dot(d_fo)
            if (any(v_ < 0 for _, v_ in us) and along_ < 0.12) or (max(u_ for u_, _ in us) <= CUFF_U + 1e-4 and
                                                                    min(v_ for _, v_ in us) >= 0):
                old.append(f)           # the elbow's strip (not the wrist's) or the grey before the cuff's rim
        elif ua is not None and f[ua] and f.calc_center_median().x * side_sign > 0.15:
            old.append(f)
    old_set = set(old)
    old_verts = {v for f in old for v in f.verts}
    tree = BVHTree.FromPolygons([v.co.copy() for v in bm.verts], [[v.index for v in f.verts] for f in old]) \
        if bm.verts.index_update() is None else None
    kd = KDTree(len(old_verts))
    ov = list(old_verts)
    for i, v in enumerate(ov):
        kd.insert(v.co, i)
    kd.balance()
    old_w = [dict(v[dl]) for v in ov]
    s_top = min((v.co - sh).dot(d_up) for v in old_verts)
    # the cuff's ring: the open loop the old faces leave at the cuff (the cuff's vertices bordering the old grey)
    cuff_ring = [v for v in old_verts if any(f not in old_set for f in v.link_faces) and (v.co - el).dot(d_fo) > 0.05]
    cc = sum((v.co for v in cuff_ring), Vector()) / len(cuff_ring)
    bmesh.ops.delete(bm, geom=old, context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    # the cuff's ring: of the open loops left, the one nearest the old surface's wrist end with AROUND points
    loops_, seen_ = [], set()
    for e in bm.edges:
        if not e.is_boundary or e in seen_:
            continue
        lp_, st_ = set(), [e]
        while st_:
            x = st_.pop()
            if x in seen_:
                continue
            seen_.add(x)
            lp_.update(x.verts)
            for v in x.verts:
                st_ += [y for y in v.link_edges if y.is_boundary and y not in seen_]
        c_ = sum((v.co for v in lp_), Vector()) / len(lp_)
        loops_.append((len(lp_), (c_ - el).dot(d_fo), (c_ - cc).length, lp_))
    near_ = sorted([x for x in loops_ if x[2] < 0.12], key=lambda x: x[2])
    print('SLEEVEFULL %s open loops near the cuff: %s' % (s, ', '.join('%d pts %.3f m along, %.3f from' % x[:3] for x in near_[:6])))
    cuff_ring = list(next((x[3] for x in near_ if x[0] == AROUND), near_[0][3]))
    cc = sum((v.co for v in cuff_ring), Vector()) / len(cuff_ring)
    # ---- the path: shoulder (under the pad) -> elbow -> the cuff ring's centre, one smooth curve ---------------------
    p0 = sh + d_up * s_top
    L_up = (el - p0).length
    L_fo = (cc - el).dot(d_fo)

    def path(t):           # t 0..1 over the arm's length; a Hermite curve through the elbow's straight line
        p1, t0, t1 = cc, d_up * (L_up + L_fo), d_fo * (L_up + L_fo)
        h00, h10, h01, h11 = 2 * t ** 3 - 3 * t ** 2 + 1, t ** 3 - 2 * t ** 2 + t, -2 * t ** 3 + 3 * t ** 2, t ** 3 - t ** 2
        return h00 * p0 + h10 * t0 + h01 * p1 + h11 * t1

    length = L_up + L_fo
    n_rings = int(round(length / RING_STEP))
    s_elbow = L_up
    ref = Vector((0, 0, 1))
    # the cuff ring in angle order round the forearm's axis, its first point at the top
    ref_c = (ref - d_fo * ref.dot(d_fo)).normalized()
    side_c = d_fo.cross(ref_c)
    ang_c = {v: math.atan2((v.co - cc).dot(side_c), (v.co - cc).dot(ref_c)) % (2 * math.pi) for v in cuff_ring}
    cuff_ring.sort(key=lambda v: ang_c[v])
    # each cuff column's v (from the cuff face on it)
    cuff_v = []
    for v in cuff_ring:
        vs = [sleeve_uv(l)[1] for l in v.link_loops if l.face.material_index == arms_mat]
        cuff_v.append(sum(vs) / len(vs) if vs else 0.0)
    if len(cuff_ring) != AROUND:
        print('SLEEVEFULL %s: the cuff ring has %d points, not %d' % (s, len(cuff_ring), AROUND))
    nA = len(cuff_ring)
    # ---- rings: the old girth per column, smoothed along, baggy, folds ----------------------------------------------
    raw = []
    frames = []
    for j in range(n_rings + 1):
        t = j / n_rings
        c = path(t)
        tan = (path(min(t + 1e-3, 1)) - path(max(t - 1e-3, 0))).normalized()
        r_ = (ref - tan * ref.dot(tan)).normalized()
        sd = tan.cross(r_)
        frames.append((c, tan, r_, sd))
        # the same place on the stock arm: the same fraction along its upper arm or its forearm
        sj_ = length * t
        if sj_ <= L_up:
            b_ = SB_['bone_%s_upperarm' % s]
            fr = (s_top + sj_) / (s_top + L_up)
        else:
            b_ = SB_['bone_%s_forearm' % s]
            fr = (sj_ - L_up) / ((B['%s_forearm' % s].tail_local - el).length)
        bh_, bt_ = b_.head_local, b_.tail_local
        st_c = bh_.lerp(bt_, fr)
        st_d = (bt_ - bh_).normalized()
        st_r = (ref - st_d * ref.dot(st_d)).normalized()
        st_s = st_d.cross(st_r)
        row = []
        for k in range(nA):
            a = ang_c[cuff_ring[k]]
            dr_st = st_r * math.cos(a) + st_s * math.sin(a)
            hit = stock_tree.ray_cast(st_c + dr_st * 0.2, -dr_st, 0.2)[0]
            row.append((hit - st_c).dot(dr_st) if hit is not None else None)
        raw.append(row)
    for k in range(nA):           # gaps filled from the column's neighbours along
        col = [raw[j][k] for j in range(n_rings + 1)]
        known = [(j, r) for j, r in enumerate(col) if r is not None]
        for j in range(n_rings + 1):
            if col[j] is None:
                col[j] = min(known, key=lambda x: abs(x[0] - j))[1] if known else 0.05
        for _ in range(8):        # smoothed along the arm: the jog at the old elbow gone
            col = [col[0]] + [(col[j - 1] + 2 * col[j] + col[j + 1]) / 4 for j in range(1, n_rings)] + [col[-1]]
        for j in range(n_rings + 1):
            raw[j][k] = col[j]
    new_rings = []
    for j in range(n_rings + 1):
        c, tan, r_, sd = frames[j]
        sj = length * j / n_rings
        mid = math.sin(math.pi * j / n_rings)
        row = []
        for k in range(nA):
            a = ang_c[cuff_ring[k]]
            dr = r_ * math.cos(a) + sd * math.sin(a)
            r = raw[j][k] * (1 + (BAG - 1) * mid ** 0.5)
            # folds bunching inside the elbow (its front: the side facing -y), and gathering before the cuff
            inside = max(0.0, -dr.y)
            r += 0.002 * math.sin(2 * math.pi * sj / 0.022) * inside ** 2 * max(0.0, 1 - abs(sj - s_elbow) / 0.06)
            r += 0.0012 * math.sin(7 * a + sj * 90) * max(0.0, 1 - (length - sj) / 0.05)
            p = c + dr * r - Vector((0, 0, HANG * mid))
            # the last rings onto the cuff's own ring
            w_c = smooth01((j - (n_rings - BLEND_RINGS)) / BLEND_RINGS)
            p = p.lerp(cuff_ring[k].co, w_c)
            row.append(p)
        new_rings.append(row)
    # ---- the mesh: rings joined, the last onto the cuff's ring ---------------------------------------------------
    rows_v = [[bm.verts.new(p) for p in row] for row in new_rings[:-1]] + [cuff_ring]
    # texture: u along by stations (top 0, the patch over the elbow at the texture's own scale, the cuff's rim)
    pu0, pu1 = PATCH_U
    half = (pu1 - pu0) / 2 / U_PER_M
    pc_u = (pu0 + pu1) / 2

    def u_at(sj):
        if sj <= s_elbow - half:
            return pu0 * sj / max(s_elbow - half, 1e-3)
        if sj <= s_elbow + half:
            return pc_u + (sj - s_elbow) * U_PER_M
        return pu1 + (CUFF_U - pu1) * (sj - s_elbow - half) / max(length - s_elbow - half, 1e-3)
    # v: the cuff's columns, turned along the arm so the patch's middle lies on the elbow's point (+y: its back)
    je = int(round(s_elbow / length * n_rings))
    c_e, _, r_e, sd_e = frames[je]
    k_pt = max(range(nA), key=lambda k: (r_e * math.cos(ang_c[cuff_ring[k]]) + sd_e * math.sin(ang_c[cuff_ring[k]])).y)
    pv_mid = (PATCH_V[0] + PATCH_V[1]) / 2
    twist = (pv_mid - cuff_v[k_pt] + 0.5) % 1.0 - 0.5
    # the columns' v increasing round the ring
    vdir = 1 if ((cuff_v[1] - cuff_v[0] + 0.5) % 1.0 - 0.5) > 0 else -1

    def v_at(j, k):
        sj = length * j / n_rings
        w = 1.0 if sj <= s_elbow else smooth01(1 - (sj - s_elbow) / max(0.75 * (length - s_elbow), 1e-3))
        return cuff_v[k] + twist * w

    for j in range(len(rows_v) - 1):
        for k in range(nA):
            k1 = (k + 1) % nA
            f = bm.faces.new([rows_v[j][k], rows_v[j][k1], rows_v[j + 1][k1], rows_v[j + 1][k]])
            f.material_index = arms_mat
            f.smooth = True
            cs = [(j, k), (j, k1), (j + 1, k1), (j + 1, k)]
            vs = [v_at(a, b) for a, b in cs]
            base = vs[0]
            # inside the sleeve's band (v 0.03..0.99 of its texture: dark borders outside), no wrap across the seam
            vs = [V0 + (x - V0) % VSPAN for x in vs]
            if max(vs) - min(vs) > VSPAN / 2:
                vs = [x + VSPAN if x < V0 + VSPAN / 2 else x for x in vs]
                vs = [min(x, V0 + VSPAN) for x in vs]
            for l, (a, b), vv in zip(f.loops, cs, vs):
                l[uvl].uv = (u_at(length * a / n_rings), 0.5 + 0.5 * vv)
    # the elbow pad raised: points inside the patch (in its texture place), eased at its rim
    bm.normal_update()
    raised = 0
    for j in range(len(rows_v) - 1):
        for k in range(nA):
            v = rows_v[j][k]
            u_ = u_at(length * j / n_rings)
            vv = V0 + (v_at(j, k) - V0) % VSPAN
            du = min(u_ - pu0, pu1 - u_) / U_PER_M
            dv = min(vv - PATCH_V[0], PATCH_V[1] - vv) * 2 * math.pi * 0.05     # metres round (about 5 cm radius)
            inside = min(du, dv)
            if inside > 0:
                v.co += v.normal * PAD_MM / 1000 * smooth01(inside / 0.005)
                raised += 1
    # weights: the nearest old point's, smoothed along the columns (the cuff's ring kept)
    for j in range(len(rows_v) - 1):
        for v in rows_v[j]:
            _, i, _ = kd.find(v.co)
            for g, w in old_w[i].items():
                v[dl][g] = w
    for _ in range(WEIGHT_SMOOTH):
        for k in range(nA):
            col = [dict(rows_v[j][k][dl]) for j in range(len(rows_v))]
            for j in range(1, len(rows_v) - 1):
                keys = set(col[j - 1]) | set(col[j]) | set(col[j + 1])
                nw = {g: (col[j - 1].get(g, 0) + 2 * col[j].get(g, 0) + col[j + 1].get(g, 0)) / 4 for g in keys}
                tot = sum(nw.values()) or 1
                dv_ = rows_v[j][k][dl]
                dv_.clear()
                for g, w in nw.items():
                    if w / tot > 0.002:
                        dv_[g] = w / tot
    report.append('%s: %d rings, %.3f m (upper %.3f, fore %.3f), the pad on column %d, %d points raised' % (
        s, len(rows_v), length, L_up, L_fo, k_pt, raised))
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
bm.to_mesh(me)
print('SLEEVEFULL ' + '; '.join(report))
print('SLEEVEFULL player: %d faces, %d tris' % (len(me.polygons), sum(len(p.vertices) - 2 for p in me.polygons)))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
