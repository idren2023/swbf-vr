"""Blender (blender -b --factory-startup re/body/r_body_quads.blend -P tools/body_assemble.py -- <out.blend> [sheet=<prefix>]):
the player's full first-person character as one mesh on one skeleton (PLAN 51 B; idren2023 2026-10-25: the forearm and
upper arm joined by geometry, not placement; the upper arm anatomically long; one model, one skeleton).

In the T-pose:
- the skeleton: the soldier's (all_inf_soldier_skeleton) with each upper arm lengthened to UPPER_TO_FORE times the
  forearm (a human humerus to forearm; the stock's upper arm is shorter than its forearm), the elbow and everything
  below it moved out; our arm's rig joined in per side, its bones named <side>_<bone> (<side>_forearm a child of
  bone_<side>_upperarm, then <side>_hand, the twist and finger bones). The stock forearm and hand bones stay unused.
- the body (tools/body_tubes.py: r_body_quads, its upper arms ending before the elbow) with each upper arm stretched
  along its bone past the deltoid (STRETCH_FROM of the stock bone) to its new length;
- our arm (re/body/r_arm_sleeve.blend: the glove, the texture-grid sleeve, the cuff's stud) laid on the new forearm
  (our forearm bone along it, scaled to its length; the left the right mirrored), the sleeve's elbow end trimmed;
- the elbow: ELBOW_RINGS rings bridging the upper arm's open end and the sleeve's (both AROUND points: all quads),
  a smooth curve per column along the arm, the third-person sleeve's texture carried on from the upper arm, weights
  eased from the upper arm to the forearm across it;
- everything joined into one object, player (body, arms, hands, studs; the packs stay their own object).
With sheet=: front, side, three-quarter pictures of the T-pose, the arms lowered with the elbows bent, and a stride."""
import bpy, bmesh, json, math, os, sys
from mathutils import Matrix, Vector, Quaternion
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from body_bake import (loader, sample, leather_as_glove, seam_blend, clean_highlights, paint_strips,  # noqa: E402
                       to_json)

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
SHEET = next((a[6:] for a in args[1:] if a.startswith('sheet=')), None)
UPPER_TO_FORE = 1.22      # upper arm : forearm (humerus to radius, human)
STRETCH_FROM = 0.35       # of the stock upper arm: the deltoid stays, the arm beyond it stretches
TRIM_U = 0.06             # the sleeve's rows at its elbow end cut away (grid u from the elbow)
ELBOW_RINGS = 5
BLEND_UP, BLEND_DOWN = 4, 6  # rings of the upper arm and of the sleeve the elbow's paint blend reaches over
TAPER = 0.06              # metres of the upper arm eased to the sleeve's girth before the elbow
WRIST_AHEAD = 0.020        # the glove cut back to this far ahead of the cuff's front rim (metres, our arm's scale): the gauntlet tapers in
SEAM_GROOVE = 0.0005       # the seam's middle points sunk this far
# our grey sleeve at 2x (tools/body_recolour.py on the 4x-then-halved copy: the body's texel size) and the glove
OPT = dict(a.split('=', 1) for a in args[1:] if '=' in a)     # per character (another character: its 1st-person paints)
ARMS_TEXTURE = os.path.abspath(OPT.get('arms_texture', 're/body/hd/all_1st_inf_trooper_arms_grey-4x.png'))  # 4x: as sharp as the glove
HANDS_TEXTURE = os.path.abspath(OPT.get('hands_texture', 're/body/hd/all_1st_inf_trooper_hands-hd.png'))
# the body's texture 4x (tools: Upscayl's high-fidelity-4x on a copy, re/body/hd/; the game's file untouched)
BODY_TEXTURE = os.path.abspath(next((a[13:] for a in args[1:] if a.startswith('body_texture=')),
                                    're/body/player_body.png'))   # the body's texture (body_texture=: the stock's 4x)
PACKS_TEXTURE = os.path.abspath('re/body/hd/All_Inf_Trooper_packs-hd.png')
PLAYER_ARMS = os.path.abspath(OPT.get('out_dir', 're/body') + '/player_arms.png')
PLAYER_HANDS = os.path.abspath(OPT.get('out_dir', 're/body') + '/player_hands.png')  # the glove's texture, its wrist's painted highlights cleaned
HIGHLIGHT_REACH = 0.035    # glove faces this near the seam (metres, our arm's scale) have their light patches cleaned
HIGHLIGHT_OVER = 0.08      # a texel this much lighter than the leather round it is a highlight  # the sleeve's texture (top half) + the elbow strips

body = bpy.data.objects['r_body_quads']
skel = body.parent
scene = bpy.context.scene


def activate(obj, others=()):
    for o in scene.objects:
        o.select_set(o is obj or o in others)
    bpy.context.view_layer.objects.active = obj


# ---- the upper arms lengthened --------------------------------------------------------------------------------------
SB = skel.data.bones
geo = {}
for s in 'lr':
    sh = SB['bone_%s_upperarm' % s].head_local.copy()
    el = SB['bone_%s_upperarm' % s].tail_local.copy()
    fore = (SB['bone_%s_forearm' % s].tail_local - SB['bone_%s_forearm' % s].head_local).length
    d = (el - sh).normalized()
    L_old = (el - sh).length
    L_new = UPPER_TO_FORE * fore
    geo[s] = (sh, d, L_old, L_new, fore)
activate(skel)
bpy.ops.object.mode_set(mode='EDIT')
eb = skel.data.edit_bones
for s in 'lr':
    sh, d, L_old, L_new, _ = geo[s]
    delta = d * (L_new - L_old)
    for n in ('bone_%s_forearm' % s, 'eff_%s_forearm' % s, 'root_%s_hand' % s, 'bone_%s_hand' % s):
        eb[n].use_connect = False
    eb['bone_%s_upperarm' % s].tail += delta
    for n in ('bone_%s_forearm' % s, 'eff_%s_forearm' % s, 'root_%s_hand' % s, 'bone_%s_hand' % s):
        eb[n].head += delta
        eb[n].tail += delta
    # the arm straight in the rest pose (idren2023 2026-10-26: the stock skeleton's T-pose arm zigzags, its upper arm 13
    # degrees back and its forearm 16 forward: the joined arm kinked and stepped at the elbow): the forearm and the
    # hand turned about the elbow onto the upper arm's line (our bind pose; the game's poses are applied relative)
    E_ = sh + d * L_new
    f_dir = (eb['bone_%s_forearm' % s].tail - eb['bone_%s_forearm' % s].head).normalized()
    turn = f_dir.rotation_difference(d).to_matrix().to_4x4()
    M_ = Matrix.Translation(E_) @ turn @ Matrix.Translation(-E_)
    for n in ('bone_%s_forearm' % s, 'eff_%s_forearm' % s, 'root_%s_hand' % s, 'bone_%s_hand' % s):
        eb[n].transform(M_)
    print("ASSEMBLE arm %s: the forearm turned %.1f degrees onto the upper arm's line" % (
        s, math.degrees(f_dir.angle(d))))
bpy.ops.object.mode_set(mode='OBJECT')
me = body.data
gidx = {g.name: g.index for g in body.vertex_groups}
stretched = 0
for v in me.vertices:
    for s in 'lr':
        sh, d, L_old, L_new, _ = geo[s]
        w = sum(g.weight for g in v.groups if g.group in (gidx.get('bone_%s_upperarm' % s), gidx.get('bone_%s_forearm' % s)))
        sv = (v.co - sh).dot(d)
        s0 = STRETCH_FROM * L_old
        if w > 0.3 and sv > s0:
            k = (L_new - s0) / (L_old - s0)
            v.co += d * (sv - s0) * (k - 1)
            stretched += 1
print('ASSEMBLE upper arms: %.3f -> %.3f m (%.2f x the forearm), %d body points stretched' % (
    geo['r'][2], geo['r'][3], UPPER_TO_FORE, stretched))

# ---- our arms on the new forearms -------------------------------------------------------------------------------------
src = os.path.abspath('re/body/r_arm_sleeve.blend')
with bpy.data.libraries.load(src) as (data_from, data_to):
    data_to.objects = ['r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud', 'r_hand_stock_skeleton']
parts = {o.name: o for o in data_to.objects}
arm_rig = parts['r_hand_stock_skeleton']
for o in data_to.objects:
    scene.collection.objects.link(o)
for n in ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud'):
    parts[n].parent = arm_rig
for pb in arm_rig.pose.bones:
    for c in list(pb.constraints):
        pb.constraints.remove(c)
fb = arm_rig.data.bones['forearm']
E, Wr = fb.head_local.copy(), fb.tail_local.copy()
our_len = (Wr - E).length


def place(side):
    bf = SB['bone_%s_forearm' % side]
    bh, bt = bf.head_local.copy(), bf.tail_local.copy()
    sc = (bt - bh).length / our_len
    y_our = (Wr - E).normalized()
    z_our = (Vector((0, 0, 1)) - y_our * y_our.z).normalized()
    x_our = y_our.cross(z_our)
    y_body = (bt - bh).normalized()
    z_body = (Vector((0, 0, 1)) - y_body * y_body.z).normalized()
    x_body = y_body.cross(z_body)
    R = (Matrix((x_body, y_body, z_body)).transposed() @ Matrix((x_our, y_our, z_our)).transposed().inverted()).to_4x4()
    return Matrix.Translation(bh) @ R @ Matrix.Scale(sc, 4) @ Matrix.Translation(-E), sc


mat_r, scale = place('r')
arm_rig.matrix_world = mat_r
left = {}
for n, o in list(parts.items()):
    c = o.copy()
    if o.data:
        c.data = o.data.copy()
    c.name = 'l' + n[1:]
    scene.collection.objects.link(c)
    left[n] = c
for n in ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud'):
    left[n].parent = left['r_hand_stock_skeleton']
left['r_hand_stock_skeleton'].matrix_world = Matrix.Scale(-1, 4, Vector((1, 0, 0))) @ mat_r
sides = {'r': parts, 'l': left}
print('ASSEMBLE arms: ours scaled %.3f onto the new forearms, the left mirrored' % scale)

# the sleeve's elbow end trimmed (its grid's first rows), names prefixed by side (bones and vertex groups)
for s, ps in sides.items():
    sl = ps['r_forearm_retopo'].data
    b = bmesh.new()
    b.from_mesh(sl)
    uvl = b.loops.layers.uv.active
    # the elbow end's first rows, and the leather cone into the glove (its own island, v under 0.03): the cuff is
    # joined to the glove by a seam instead
    cut = [f for f in b.faces if f.material_index == 1 and (any(l[uvl].uv.x < TRIM_U and l[uvl].uv.y >= 0.03 for l in f.loops)
                                                            or max(l[uvl].uv.y for l in f.loops) < 0.03)]
    bmesh.ops.delete(b, geom=cut, context='FACES')
    bmesh.ops.delete(b, geom=[v for v in b.verts if not v.link_faces], context='VERTS')
    b.to_mesh(sl)
    # the meshes' groups first, their armature modifiers gone (Blender renames a deformed mesh's groups with its
    # bones: the left copies still pointed at the right rig)
    for n in ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud'):
        for m in list(ps[n].modifiers):
            ps[n].modifiers.remove(m)
        for g in ps[n].vertex_groups:
            g.name = '%s_%s' % (s, g.name)


def ring_centre(obj, near, along):
    """The centre (world) of obj's open loop nearest `near`, and its size."""
    b_ = bmesh.new()
    b_.from_mesh(obj.data)
    b_.transform(obj.matrix_world)
    best = None
    seen_ = set()
    for e in b_.edges:
        if not e.is_boundary or e in seen_:
            continue
        loop_, stack = set(), [e]
        while stack:
            x = stack.pop()
            if x in seen_:
                continue
            seen_.add(x)
            loop_.update(x.verts)
            for v in x.verts:
                stack += [y for y in v.link_edges if y.is_boundary and y not in seen_]
        c_ = sum((v.co for v in loop_), Vector()) / len(loop_)
        score = (c_ - near).length
        if len(loop_) < 20:          # a stray hole, not an arm's ring
            continue
        if best is None or score < best[0]:
            q_ = [v.co - c_ for v in loop_]
            r_ = sum((x - along * x.dot(along)).length for x in q_) / len(q_)
            best = (score, c_, len(loop_), r_, [v.co.copy() for v in loop_])
    return best[1], best[2], best[3], best[4]


def radius_at(pts, centre, along, ref, ang):
    """The loop's radius at angle ang round `along` (from `ref`), by its two nearest points' angles."""
    side = along.cross(ref)
    pol = []
    for p in pts:
        q = p - centre
        q -= along * q.dot(along)
        pol.append((math.atan2(q.dot(side), q.dot(ref)), q.length))
    pol.sort()
    for (a0, r0), (a1, r1) in zip(pol, pol[1:] + [(pol[0][0] + 2 * math.pi, pol[0][1])]):
        if a0 <= ang <= a1 or a0 <= ang + 2 * math.pi <= a1:
            a = ang if a0 <= ang <= a1 else ang + 2 * math.pi
            return r0 + (r1 - r0) * (a - a0) / max(a1 - a0, 1e-6)
    return pol[0][1]


# the sleeve's elbow ring centred on the upper arm's end ring (the bone lines are not the surfaces' middles: the
# forearm sat high), by moving the arm square to the arm's line
bpy.context.view_layer.update()
for s, ps in sides.items():
    sh, d, L_old, L_new, _ = geo[s]
    elbow = sh + d * L_new
    c_body, n_body, r_body, _ = ring_centre(body, elbow - d * 0.04, d)
    c_arm, n_arm, r_arm, arm_pts = ring_centre(ps['r_forearm_retopo'], elbow + d * 0.02, d)
    ref_ = Vector((0, 0, 1))
    ref_ = (ref_ - d * ref_.dot(d)).normalized()
    side_ = d.cross(ref_)
    # (idren2023 2026-10-25: the forearm stays on its bone's line; the upper arm's stretched part bends to meet it, so
    # the two make one straight arm in the T-pose)
    off = c_arm - c_body
    off -= d * off.dot(d)
    s0 = STRETCH_FROM * L_old
    s_end = (c_body - sh).dot(d)
    gidx_ = {g.name: g.index for g in body.vertex_groups}
    groups_ = (gidx_.get('bone_%s_upperarm' % s), gidx_.get('bone_%s_forearm' % s))
    moved_ = 0
    for v in body.data.vertices:
        w = sum(g.weight for g in v.groups if g.group in groups_)
        sv = (v.co - sh).dot(d)
        if w > 0.3 and sv > s0:
            # over the last TAPER metres the upper arm's section eased, angle by angle, to the sleeve's elbow ring (the
            # stock's upper arm is an oval, our sleeve round: changed within the elbow's bridge, the arm jogged)
            tt = min(1.0, max(0.0, (sv - (s_end - TAPER)) / TAPER))
            tt = tt * tt * (3 - 2 * tt)
            if tt > 0:
                rad = v.co - (c_body + d * (sv - (c_body - sh).dot(d)))
                rad -= d * rad.dot(d)
                if rad.length > 1e-4:
                    ang = math.atan2(rad.dot(side_), rad.dot(ref_))
                    r_t = radius_at(arm_pts, c_arm, d, ref_, ang)
                    v.co += rad.normalized() * (r_t - rad.length) * tt
            v.co += off * min(1.0, (sv - s0) / max(s_end - s0, 1e-3))
            moved_ += 1
    print("ASSEMBLE arm %s: the upper arm's end bent %.1f mm (%.1f up) onto the forearm's line (%d points); its girth "
          "%.1f -> %.1f mm at the elbow" % (s, off.length * 1000, off.z * 1000, moved_, r_body * 1000, r_arm * 1000))
for s, ps in sides.items():
    for bone in ps['r_hand_stock_skeleton'].data.bones:
        bone.name = '%s_%s' % (s, bone.name)

# ---- one skeleton ---------------------------------------------------------------------------------------------------
# the arm meshes keep their places when their rigs are merged away
bpy.context.view_layer.update()
for s_ in 'lr':
    for n in ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud'):
        o_ = sides[s_][n]
        keep = o_.matrix_world.copy()
        o_.parent = None
        o_.matrix_world = keep
activate(skel, [sides[s]['r_hand_stock_skeleton'] for s in 'lr'])
bpy.ops.object.join()
bpy.ops.object.mode_set(mode='EDIT')
eb = skel.data.edit_bones
for s in 'lr':
    eb['%s_forearm' % s].parent = eb['bone_%s_upperarm' % s]
    eb['%s_forearm' % s].use_connect = False
bpy.ops.object.mode_set(mode='OBJECT')
print('ASSEMBLE skeleton: %d bones (the soldier\'s, our arms\' as <side>_<bone>)' % len(skel.data.bones))

# ---- one mesh ---------------------------------------------------------------------------------------------------------
# the body's weights on the stock forearm and hand bones go to ours
for s in 'lr':
    for old, new in (('bone_%s_forearm' % s, '%s_forearm' % s), ('bone_%s_hand' % s, '%s_hand' % s)):
        g = body.vertex_groups.get(old)
        if g:
            g.name = new
arm_meshes = [sides[s][n] for s in 'lr' for n in ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud')]
# one texture layout layer, one name: the body's baked one (its stock layer dropped), the arms' own
for o_ in [body] + arm_meshes:
    uvs_ = o_.data.uv_layers
    keep = uvs_.get('baked') or uvs_.active
    for l_ in [l_ for l_ in uvs_ if l_.name != keep.name]:
        uvs_.remove(l_)
    uvs_[0].name = 'UV'
activate(body, arm_meshes)
bpy.ops.object.join()
body.name = 'player'
me = body.data
# one arms material (the stock body's new upper arms use our sleeve's texture too: tools/body_stock_prep.py)
arms_slots = [i for i, m in enumerate(me.materials) if m and 'all_1st_inf_trooper_arms' in m.name.lower()]
for p_ in me.polygons:
    if p_.material_index in arms_slots[1:]:
        p_.material_index = arms_slots[0]

# ---- the elbows -------------------------------------------------------------------------------------------------------
bm = bmesh.new()
bm.from_mesh(me)
bm.verts.ensure_lookup_table()
uvl = bm.loops.layers.uv.active
dl = bm.verts.layers.deform.active
gid = {g.name: g.index for g in body.vertex_groups}
body_mat = next(i for i, m in enumerate(me.materials) if m and m.name == 'player_body')


def loops_of_boundary():
    seen, out = set(), []
    for e in bm.edges:
        if not e.is_boundary or e in seen:
            continue
        cyc = [e.verts[0]]
        cur = e.verts[0]
        edge = e
        while True:
            seen.add(edge)
            nxt = edge.other_vert(cur)
            if nxt is cyc[0]:
                break
            cyc.append(nxt)
            cur = nxt
            edge = next((x for x in cur.link_edges if x.is_boundary and x not in seen), None)
            if edge is None:
                break
        out.append(cyc)
    return out


def angle_order(cyc, centre, d):
    ref = (cyc[0].co - centre)
    ref = (ref - d * ref.dot(d)).normalized()
    side = d.cross(ref)

    def ang(v):
        q = v.co - centre
        return math.atan2(q.dot(side), q.dot(ref))
    diff = (ang(cyc[1]) - ang(cyc[0]) + math.pi) % (2 * math.pi) - math.pi
    return cyc if diff > 0 else [cyc[0]] + list(reversed(cyc[1:]))


SLEEVE_H = bpy.data.images.load(ARMS_TEXTURE).size[1]
STRIP_W, STRIP_H = bpy.data.images.load(ARMS_TEXTURE).size[0], SLEEVE_H // 4
bridged = []
strips = []
elbow_loops = []
arms_mat = next(i for i, m in enumerate(me.materials) if m and 'all_1st_inf_trooper_arms' in m.name.lower())
for s in 'lr':
    sh, d, L_old, L_new, fore = geo[s]
    elbow = sh + d * L_new
    cand = []
    for cyc in loops_of_boundary():
        c = sum((v.co for v in cyc), Vector()) / len(cyc)
        if (c - elbow).length < 0.15 and abs((c - sh).dot(d) - L_new) < 0.12 and len(cyc) >= 20:
            cand.append(((c - sh).dot(d), c, cyc))
    if len(cand) < 2:
        print('ASSEMBLE elbow %s: open ends not found (%d)' % (s, len(cand)))
        for cyc in loops_of_boundary():
            c = sum((v.co for v in cyc), Vector()) / len(cyc)
            print('ASSEMBLE   open loop: %d points, along %.3f, from the elbow %.3f' % (
                len(cyc), (c - sh).dot(d), (c - elbow).length))
        continue
    cand.sort(key=lambda t: t[0])
    c72 = [c_ for c_ in cand if len(c_[2]) == 72]     # the upper arm's and the sleeve's rings (not stray openings)
    if len(c72) >= 2:
        cand = c72
    (sa, ca, A), (sb, cb, B) = cand[0], cand[-1]
    if len(A) != len(B):
        print('ASSEMBLE elbow %s: the ends differ (%d upper arm, %d sleeve): not bridged; candidates %s' % (
            s, len(A), len(B), ', '.join('%d pts at %.3f' % (len(c_[2]), c_[0]) for c_ in cand)))
        continue
    centre = (ca + cb) / 2
    A = angle_order(A, centre, d)
    B = angle_order(B, centre, d)
    n = len(A)
    off = min(range(n), key=lambda o: sum((A[k].co - B[(k + o) % n].co).length for k in range(0, n, 4)))
    B = [B[(k + off) % n] for k in range(n)]
    gap = sb - sa
    # each column's two textures: the body sleeve's carried on from the upper arm's last row, and our sleeve's
    # mirrored back from its first row; the boundary edge's face gives the end's uv and the step along the arm (its
    # two inner corners). The elbow's quads take a strip baked from both, cross-fading along the elbow (below).
    def cells(R):
        out = []
        for k in range(n):
            a0, a1 = R[k], R[(k + 1) % n]
            e = next((x for x in a0.link_edges if x.other_vert(a0) is a1), None)
            f = e.link_faces[0] if e and e.link_faces else None
            if f is None:
                out.append(None)
                continue
            lu = {l.vert: l[uvl].uv.copy() for l in f.loops}
            inner = [v for v in f.verts if v not in (a0, a1)]
            i0 = min(inner, key=lambda v: (v.co - a0.co).length)
            i1 = min(inner, key=lambda v: (v.co - a1.co).length)
            row = ((a0.co - i0.co).length + (a1.co - i1.co).length) / 2 or 0.01
            out.append((lu[a0], lu[i0] - lu[a0], lu[a1], lu[i1] - lu[a1], row))
        return out
    a_face = next((f for e in A[0].link_edges for f in e.link_faces), None)
    a_tex = 'body' if a_face is not None and a_face.material_index == body_mat else 'arms'
    # (idren2023 2026-10-26: the stock upper arm's paint and our sleeve's blended over a longer stretch) the strip covers
    # BLEND_UP rings of the upper arm, the bridge and BLEND_DOWN rings of the sleeve; each side's paint read at its own
    # rings' texture places, carried on past its end
    def walk(R, steps):
        out_, used_ = [list(R)], set(R)
        for _ in range(steps):
            nxt = []
            for v in out_[-1]:
                cand = [e.other_vert(v) for e in v.link_edges if e.other_vert(v) not in used_]
                if len(cand) != 1:
                    return out_
                nxt.append(cand[0])
            used_.update(nxt)
            out_.append(nxt)
        return out_

    def ring_uv(R, mat):
        """Per column k: the texture places of R[k] and R[k+1] in one face of `mat` on that edge."""
        out_ = []
        for k in range(n):
            a0, a1 = R[k], R[(k + 1) % n]
            f = next((f for e in a0.link_edges if e.other_vert(a0) is a1 for f in e.link_faces
                      if f.material_index == mat), None)
            if f is None:
                out_.append(out_[-1] if out_ else (Vector((0, 0)), Vector((0, 0))))
                continue
            lu = {l.vert: l[uvl].uv.copy() for l in f.loops}
            out_.append((lu[a0], lu[a1]))
        return out_

    ups = walk(A, BLEND_UP)               # A, then up the upper arm
    downs = walk(B, BLEND_DOWN)           # B, then down the sleeve
    a_mat = a_face.material_index if a_face is not None else body_mat
    s_of = lambda R: sum((v.co - sh).dot(d) for v in R) / len(R)
    side_a = [(s_of(R), ring_uv(R, a_mat)) for R in reversed(ups)]          # s increasing to A
    side_b = [(s_of(R), ring_uv(R, arms_mat)) for R in downs]               # s increasing from B
    s_lo, s_hi = side_a[0][0], side_b[-1][0]
    strip_index = len(strips)
    strips.append((s, side_a, side_b, s_lo, s_hi, n, a_tex, 'arms', 'elbowx'))
    # the existing faces of the zone onto the strip (their rings' place along it)
    zone_rings = list(reversed(ups))[:-1]
    sleeve_rings = downs[1:]

    def remap(R0, R1):
        for k in range(n):
            a0, a1, b1 = R0[k], R0[(k + 1) % n], R1[(k + 1) % n]
            f = next((f for f in a0.link_faces if a1 in f.verts and b1 in f.verts), None)
            if f is None:
                continue
            f.material_index = arms_mat
            for l in f.loops:
                kk = k + (1 if l.vert in (a1, b1) else 0)
                sv_ = (l.vert.co - sh).dot(d)
                l[uvl].uv = (kk / n, (strip_index * STRIP_H + (sv_ - s_lo) / (s_hi - s_lo) * STRIP_H) / (2 * SLEEVE_H))
                elbow_loops.append(l)
    for R0, R1 in zip(zone_rings, zone_rings[1:] + [A]):
        remap(R0, R1)
    for R0, R1 in zip([B] + sleeve_rings[:-1], sleeve_rings):
        remap(R0, R1)
    rings = [A]
    for j in range(1, ELBOW_RINGS + 1):
        t = j / (ELBOW_RINGS + 1)
        h00, h10, h01, h11 = 2 * t ** 3 - 3 * t ** 2 + 1, t ** 3 - 2 * t ** 2 + t, -2 * t ** 3 + 3 * t ** 2, t ** 3 - t ** 2
        ring = []
        for k in range(n):
            p = h00 * A[k].co + h10 * d * gap + h01 * B[k].co + h11 * d * gap
            v = bm.verts.new(p)
            wf = t * t * (3 - 2 * t)
            v[dl][gid['bone_%s_upperarm' % s]] = 1 - wf
            v[dl][gid['%s_forearm' % s]] = wf
            ring.append(v)
        rings.append(ring)
    rings.append(B)
    for j in range(len(rings) - 1):
        for k in range(n):
            f = bm.faces.new([rings[j][k], rings[j][(k + 1) % n], rings[j + 1][(k + 1) % n], rings[j + 1][k]])
            f.smooth = True
            f.material_index = arms_mat
            # the strip: around by column, along by ring (the sleeve's texture's lower half, this elbow's rows)
            for l, (kk, jj) in zip(f.loops, ((k, j), (k + 1, j), (k + 1, j + 1), (k, j + 1))):
                sv_ = (l.vert.co - sh).dot(d)
                l[uvl].uv = (kk / n, (strip_index * STRIP_H + (sv_ - s_lo) / (s_hi - s_lo) * STRIP_H) / (2 * SLEEVE_H))
                elbow_loops.append(l)
    bridged.append('%s %d points, %.0f mm' % (s, n, gap * 1000))
# ---- the wrists: the cuff (the glove's gauntlet) joined to the glove by a seam -------------------------------------
def boundary_cycles():
    return loops_of_boundary()


def ring_cells(R):
    """Per segment of ring R: the end's uvs and the step back into the surface it bounds (its face's inner corners)."""
    out = []
    m_ = len(R)
    for k in range(m_):
        a0, a1 = R[k], R[(k + 1) % m_]
        e = next((x for x in a0.link_edges if x.other_vert(a0) is a1), None)
        f = e.link_faces[0] if e and e.link_faces else None
        if f is None:
            out.append(None)
            continue
        lu = {l.vert: l[uvl].uv.copy() for l in f.loops}
        inner = [v for v in f.verts if v not in (a0, a1)]
        i0 = min(inner, key=lambda v: (v.co - a0.co).length)
        i1 = min(inner, key=lambda v: (v.co - a1.co).length)
        row = ((a0.co - i0.co).length + (a1.co - i1.co).length) / 2 or 0.001
        out.append((lu[a0], lu[i0] - lu[a0], lu[a1], lu[i1] - lu[a1], row))
    return out


seams = []
glove_uvs = []
near_glove_tris = []
for s in 'lr':
    fb_ = skel.data.bones['%s_forearm' % s]
    E_, W_ = fb_.head_local.copy(), fb_.tail_local.copy()
    d = (W_ - E_).normalized()
    near = [(len(c), c) for c in boundary_cycles()
            if (sum((v.co for v in c), Vector()) / len(c) - W_).length < 0.10]
    cuff = next((c for n_, c in near if n_ == 72), None)
    glove = next((c for n_, c in near if n_ != 72 and n_ > 20), None)
    if not cuff or not glove:
        print('ASSEMBLE wrist %s: openings not found (%s)' % (s, [n_ for n_, _ in near]))
        continue
    s_cuff = (sum((v.co for v in cuff), Vector()) / len(cuff) - E_).dot(d)
    # the glove cut back ring by ring (its rings round the wrist) to just ahead of the cuff's rim
    ring, prev, cut_rings = set(glove), set(), 0
    while True:
        c_ = sum((v.co for v in ring), Vector()) / len(ring)
        if (c_ - E_).dot(d) >= s_cuff + WRIST_AHEAD * scale or cut_rings > 12:
            break
        nxt = {e.other_vert(v) for v in ring for e in v.link_edges} - ring - prev
        if len(nxt) != len(ring):
            break
        bmesh.ops.delete(bm, geom=[f for f in {f for v in ring for f in v.link_faces}], context='FACES')
        bmesh.ops.delete(bm, geom=[v for v in ring if v.is_valid and not v.link_faces], context='VERTS')
        prev, ring = set(), nxt
        cut_rings += 1
    glove = next((c for c in boundary_cycles() if set(c) == ring), None)
    if glove is None:
        print('ASSEMBLE wrist %s: the glove\'s cut ring is not a clean loop' % s)
        continue
    centre = (sum((v.co for v in cuff), Vector()) / len(cuff) + sum((v.co for v in glove), Vector()) / len(glove)) / 2
    A = angle_order(cuff, centre, d)
    B = angle_order(glove, centre, d)
    n, m = len(A), len(B)
    ref = (A[0].co - centre)
    ref = (ref - d * ref.dot(d)).normalized()
    side_ = d.cross(ref)
    ang = lambda v: math.atan2((v.co - centre).dot(side_), (v.co - centre).dot(ref))
    start = min(range(m), key=lambda k: abs(ang(B[k])))
    B = [B[(k + start) % m] for k in range(m)]
    gap = max(((sum((v.co for v in B), Vector()) / m) - (sum((v.co for v in A), Vector()) / n)).dot(d), 0.002)
    cA, cB = ring_cells(A), ring_cells(B)
    cB_centre = sum((v.co for v in B), Vector()) / len(B)
    for f in bm.faces:
        if f.is_valid and f.material_index != arms_mat and len(f.verts) >= 3 and \
                (f.calc_center_median() - cB_centre).length < HIGHLIGHT_REACH * scale and \
                'hands' in me.materials[f.material_index].name.lower():
            near_glove_tris.append([l[uvl].uv.copy() for l in f.loops])
    hands_mat = next(i for i, m_ in enumerate(me.materials) if m_ and 'hands' in m_.name.lower())
    for f in bm.faces:
        if f.material_index == hands_mat and f.is_valid and (f.calc_center_median() - W_).length < 0.07 * scale:
            glove_uvs.extend(l[uvl].uv.copy() for l in f.loops)
    strip_index = len(strips)
    strips.append((s, cA, cB, gap, n, m, 'arms', 'hands', 'seam'))

    def at_b(fr):
        x = fr * m
        k = int(math.floor(x)) % m
        return B[k].co.lerp(B[(k + 1) % m].co, x - math.floor(x))

    def herm(p0, p1, t):
        h00, h10, h01, h11 = 2 * t ** 3 - 3 * t ** 2 + 1, t ** 3 - 2 * t ** 2 + t, -2 * t ** 3 + 3 * t ** 2, t ** 3 - t ** 2
        return h00 * p0 + h10 * d * gap + h01 * p1 + h11 * d * gap

    def weigh(v, t):
        w = t * t * (3 - 2 * t)
        v[dl][gid['%s_wrist_twist' % s]] = 1 - w
        v[dl][gid['%s_hand' % s]] = w

    # rings: the cuff's count over its first part, the glove's over the rest, the ten 3:1 steps between them (their
    # middle points sunk: the seam's groove); radii eased by the curve (flat at both ends: it hugs the wrist)
    TN, TMID, TM = (0.15, 0.3, 0.45), 0.53, (0.62, 0.76, 0.89)

    def at_a(fr):
        x = fr * n
        k_ = int(math.floor(x)) % n
        return A[k_].co.lerp(A[(k_ + 1) % n].co, x - math.floor(x))

    ring_n = [A]
    for t in TN:
        ring = []
        for k in range(n):
            v = bm.verts.new(herm(A[k].co, at_b(k / n), t))
            weigh(v, t)
            ring.append(v)
        ring_n.append(ring)
    ring_m = []
    for t in TM:
        ring = []
        for k in range(m):
            v = bm.verts.new(herm(at_a(k / m), B[k].co, t))
            weigh(v, t)
            ring.append(v)
        ring_m.append(ring)
    ring_m.append(B)
    tn = (0,) + TN
    tm = TM + (1,)

    def face(vs, coords):
        f = bm.faces.new(vs)
        f.material_index = arms_mat
        f.smooth = True
        for l, (fu, t) in zip(f.loops, coords):
            l[uvl].uv = (fu, (strip_index * STRIP_H + t * STRIP_H) / (2 * SLEEVE_H))
            elbow_loops.append(l)
    for j in range(len(ring_n) - 1):
        R0, R1_, t0_, t1_ = ring_n[j], ring_n[j + 1], tn[j], tn[j + 1]
        for k in range(n):
            face([R0[k], R0[(k + 1) % n], R1_[(k + 1) % n], R1_[k]],
                 ((k / n, t0_), ((k + 1) / n, t0_), ((k + 1) / n, t1_), (k / n, t1_)))
    for j in range(len(ring_m) - 1):
        R0, R1_, t0_, t1_ = ring_m[j], ring_m[j + 1], tm[j], tm[j + 1]
        for k in range(m):
            face([R0[k], R0[(k + 1) % m], R1_[(k + 1) % m], R1_[k]],
                 ((k / m, t0_), ((k + 1) / m, t0_), ((k + 1) / m, t1_), (k / m, t1_)))
    top, bot, TT, TB = ring_n[-1], ring_m[0], tn[-1], tm[0]
    units = (n - m) // 2
    starts = {int(u * n / units) for u in range(units)}
    k, b = 0, 0
    fu = lambda kk: kk / n
    fb_u = lambda bb: bb / m
    while k < n:
        if k in starts and units > 0:
            t0, t1, t2, t3 = top[k % n], top[(k + 1) % n], top[(k + 2) % n], top[(k + 3) % n]
            b0, b1 = bot[b % m], bot[(b + 1) % m]
            mids = []
            for j, frac in ((1, 1 / 3), (2, 2 / 3)):
                p_ = (top[(k + j) % n].co + b0.co.lerp(b1.co, frac)) / 2
                radial = (p_ - centre) - d * (p_ - centre).dot(d)
                v = bm.verts.new(p_ - radial.normalized() * SEAM_GROOVE * scale)
                weigh(v, TMID)
                mids.append(v)
            m0, m1 = mids
            u0_, u1_ = (fu(k + 1) + fb_u(b + 1 / 3)) / 2, (fu(k + 2) + fb_u(b + 2 / 3)) / 2
            face([t0, t1, m0, b0], ((fu(k), TT), (fu(k + 1), TT), (u0_, TMID), (fb_u(b), TB)))
            face([t1, t2, m1, m0], ((fu(k + 1), TT), (fu(k + 2), TT), (u1_, TMID), (u0_, TMID)))
            face([t2, t3, b1, m1], ((fu(k + 2), TT), (fu(k + 3), TT), (fb_u(b + 1), TB), (u1_, TMID)))
            face([m0, m1, b1, b0], ((u0_, TMID), (u1_, TMID), (fb_u(b + 1), TB), (fb_u(b), TB)))
            k += 3
        else:
            face([top[k % n], top[(k + 1) % n], bot[(b + 1) % m], bot[b % m]],
                 ((k / n, TT), ((k + 1) / n, TT), ((b + 1) / m, TB), (b / m, TB)))
            k += 1
        b += 1
    seams.append('%s %d -> %d points (%d glove rings cut back), %.1f mm' % (s, n, m, cut_rings, gap * 1000))
print('ASSEMBLE wrists: %s' % ('; '.join(seams) or 'none'))

# the sleeve's texture moved to the upper half of a texture twice as tall; the strips baked into the lower half
elbow_set = set(elbow_loops)
for f in bm.faces:
    if f.material_index == arms_mat:
        for l in f.loops:
            if l not in elbow_set:
                l[uvl].uv.y = 0.5 + l[uvl].uv.y * 0.5
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
bm.to_mesh(me)
print('ASSEMBLE elbows: %s' % ('; '.join(bridged) or 'none'))


arms_tex = loader(ARMS_TEXTURE)
body_tex = loader(BODY_TEXTURE)
hands_tex = loader(HANDS_TEXTURE)


CUFF_U = 0.662
arms_tex = seam_blend(arms_tex)
if glove_uvs:
    arms_tex = leather_as_glove(arms_tex, hands_tex, glove_uvs, CUFF_U)


if near_glove_tris:
    hands_tex = clean_highlights(hands_tex, near_glove_tris)
    him = bpy.data.images.new('player_hands', hands_tex[0], hands_tex[1], alpha=True)
    him.pixels[:] = hands_tex[2]
    him.filepath_raw = PLAYER_HANDS
    him.file_format = 'PNG'
    him.save()
TEX = {'arms': arms_tex, 'body': body_tex, 'hands': hands_tex}
AW, AH = arms_tex[0], arms_tex[1]
canvas = [0.0] * (AW * 2 * AH * 4)
for y in range(AH):      # the sleeve's texture: the upper half
    src_row = arms_tex[2][y * AW * 4:(y + 1) * AW * 4]
    canvas[(AH + y) * AW * 4:(AH + 1 + y) * AW * 4] = src_row
paint_strips(canvas, strips, TEX, AW, STRIP_H, STRIP_W)
# the recipe of both baked textures (tools/body_bake.py rebuilds them from it and the textures: no blends needed)
BAKE = OPT.get('bake', os.path.join('tools', 'data', 'models', 'player', 'bake.json'))
os.makedirs(os.path.dirname(os.path.abspath(BAKE)), exist_ok=True)
with open(BAKE, 'w', encoding='utf-8') as f_:
    json.dump({'textures': {k_: os.path.relpath(v_).replace(os.sep, '/') for k_, v_ in
                            (('arms', ARMS_TEXTURE), ('body', BODY_TEXTURE), ('hands', HANDS_TEXTURE))},
               'strip_h': STRIP_H, 'strips': to_json(strips), 'glove_uvs': to_json(glove_uvs),
               'near_glove_tris': to_json(near_glove_tris)}, f_, separators=(',', ':'))
print('ASSEMBLE bake recipe -> %s' % BAKE)
img = bpy.data.images.new('player_arms', AW, 2 * AH, alpha=True)
img.pixels[:] = canvas
img.filepath_raw = PLAYER_ARMS
img.file_format = 'PNG'
img.save()
for m in me.materials:
    if m and m.use_nodes and 'all_1st_inf_trooper_arms' in m.name.lower():
        for nd in m.node_tree.nodes:
            if nd.type == 'TEX_IMAGE':
                nd.image = img
print('ASSEMBLE elbow strips: %d baked into %s (%d x %d; the sleeve in its upper half)' % (len(strips), PLAYER_ARMS, AW, 2 * AH))
# the body's and packs' materials on the 4x textures
hd = {'all_inf_trooper.tga': bpy.data.images.load(os.path.abspath('re/body/hd/All_Inf_trooper-hd.png')),  # (the chest plate) 'all_inf_trooper_packs.tga': bpy.data.images.load(PACKS_TEXTURE),
      'all_1st_inf_trooper_hands.tga': bpy.data.images.load(PLAYER_HANDS if os.path.exists(PLAYER_HANDS) else HANDS_TEXTURE)}
for m in bpy.data.materials:
    if m and m.use_nodes and m.name.lower() in hd:
        for nd in m.node_tree.nodes:
            if nd.type == 'TEX_IMAGE':
                nd.image = hd[m.name.lower()]
quads = sum(1 for p in me.polygons if len(p.vertices) == 4)
print('ASSEMBLE player: %d faces (%d quads), %d tris; packs %d tris' % (
    len(me.polygons), quads, sum(len(p.vertices) - 2 for p in me.polygons),
    sum(len(p.vertices) - 2 for p in bpy.data.objects['r_body_packs'].data.polygons) if 'r_body_packs' in bpy.data.objects else 0))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))

if not SHEET:
    sys.exit(0)
# ---- pictures ---------------------------------------------------------------------------------------------------------
POSES = [('T-pose', {}),
         ('arms lowered, elbows bent', {'bone_r_upperarm': ('y', -70), 'bone_l_upperarm': ('y', 70),
                                        'r_forearm': ('z', 80), 'l_forearm': ('z', -80)}),
         ('stride, crouched, chest turned', {'bone_r_upperarm': ('y', -60), 'bone_l_upperarm': ('y', 60),
                                             'r_forearm': ('z', 100), 'l_forearm': ('z', -40),
                                             'bone_r_thigh': ('x', -45), 'bone_r_calf': ('x', 70),
                                             'bone_l_thigh': ('x', 25), 'bone_l_calf': ('x', 30),
                                             'bone_ribcage': ('z', 20)})]


def set_pose(spec):
    for pb in skel.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.rotation_quaternion = (1, 0, 0, 0)
    bpy.context.view_layer.update()
    for n, (axis, deg) in spec.items():
        pb = skel.pose.bones[n]
        Rq = pb.bone.matrix_local.to_3x3().normalized().to_quaternion()
        qw = Quaternion({'x': (1, 0, 0), 'y': (0, 1, 0), 'z': (0, 0, 1)}[axis], math.radians(deg))
        pb.rotation_quaternion = Rq.inverted() @ qw @ Rq
    bpy.context.view_layer.update()


scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'TEXTURE'
scene.render.resolution_x, scene.render.resolution_y = 600, 700
cd = bpy.data.cameras.new('cam')
cd.type = 'ORTHO'
cd.ortho_scale = 2.3
cam = bpy.data.objects.new('cam', cd)
scene.collection.objects.link(cam)
scene.camera = cam
for o in scene.objects:
    if o.type == 'MESH':
        o.hide_render = o.name not in ('player', 'r_body_packs')
VIEWS = (('front', Vector((0, -1, 0))), ('side', Vector((-1, 0, 0))), ('3/4', Vector((-0.6, -0.8, 0.25))))
tiles = []
for label, spec in POSES:
    set_pose(spec)
    for vname, dv in VIEWS:
        cam.location = Vector((0, 0, 1.0)) + dv.normalized() * 5
        cam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
        path = os.path.abspath('%s-%d.png' % (SHEET, len(tiles)))
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        tiles.append(path)
cols, rows = len(VIEWS), len(POSES)
W, Hh = scene.render.resolution_x, scene.render.resolution_y
img = bpy.data.images.new('sheet', W * cols, Hh * rows)
px = [0.0] * (W * cols * Hh * rows * 4)
for k, path in enumerate(tiles):
    im = bpy.data.images.load(path)
    srcp = list(im.pixels)
    r_, c_ = rows - 1 - k // cols, k % cols
    for y in range(Hh):
        o = ((r_ * Hh + y) * W * cols + c_ * W) * 4
        px[o:o + W * 4] = srcp[y * W * 4:(y + 1) * W * 4]
    os.remove(path)
img.pixels = px
img.filepath_raw = os.path.abspath(SHEET + '.png')
img.file_format = 'PNG'
img.save()
print('SHEET', SHEET + '.png')
