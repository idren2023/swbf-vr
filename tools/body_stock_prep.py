"""The stock soldier's body made ready for our arms (idren2023 2026-10-26: the stock mesh goes forward; our arms attached):
its body faces as they are (stock shape, stock texture layout, its weights), the head off and the neck closed, the
grey sleeve cut off below the vest's shoulder pads, and in its place an upper arm of AROUND points round (the ring our
sleeve's elbow end has: tools/body_assemble.py bridges the two) lying on the stock's upper arm, from tucked under the
pad to the elbow, wearing the stock sleeve's own paint (its texture places from the stock triangle under each face). Saved as r_body_quads (+ r_body_packs) on the
soldier's skeleton, the input tools/body_assemble.py takes.
Run: blender -b --factory-startup re/body/all_inf_soldier.blend -P tools/body_stock_prep.py -- <config.json> <out.blend>
"""
import bpy, bmesh, json, math, os, sys
import numpy as np
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree
from mathutils.geometry import barycentric_transform

cfg_path, out = sys.argv[sys.argv.index('--') + 1:][:2]
cfg = json.load(open(cfg_path))
AROUND = 72
RING_STEP = 0.012          # metres between the upper arm's rings
TUCK = 0.025               # the upper arm starts this far up under the pad
INSET = 0.0015             # and this far inside the stock's sleeve surface
U0, U1 = 0.06, 0.30        # the band of our sleeve's texture (u along the arm) laid along the upper arm
sc = bpy.context.scene
rig = bpy.data.objects[cfg['lowpoly']['skeleton']]
AP = cfg.get('arm_prep', {})
st = bpy.data.objects[AP.get('stock', 'all_inf_soldier_hi')]
island_map = np.load(cfg['texmaps'])['island']
N = island_map.shape[0]
PADS = AP.get('pad_islands', [10])              # the vest's shoulder pads (none: the sleeve is cut at the armhole)
SLEEVES = AP.get('sleeve_islands', {'l': [11], 'r': [11]})
PACKS_MAT = AP.get('packs_material', 1)


def texel(uv):
    return min(int((1 - uv.y % 1.0) * N), N - 1), min(int(uv.x % 1.0 * N), N - 1)


body = st.copy()
body.data = st.data.copy()
body.name = 'r_body_quads'
sc.collection.objects.link(body)
for m in body.modifiers:
    if m.type == 'ARMATURE':
        m.object = rig
bm = bmesh.new()
bm.from_mesh(body.data)
uvl = bm.loops.layers.uv.active
isl = {f: int(island_map[texel(sum((l[uvl].uv for l in f.loops), Vector((0, 0))) / len(f.loops))]) for f in bm.faces}
# the packs their own object; the head, face and gloves off
packs = [f for f in bm.faces if f.material_index == PACKS_MAT]
pk = st.copy()
pk.data = st.data.copy()
pk.name = 'r_body_packs'
sc.collection.objects.link(pk)
pb_ = bmesh.new()
pb_.from_mesh(pk.data)
bmesh.ops.delete(pb_, geom=[f for f in pb_.faces if f.material_index != PACKS_MAT], context='FACES')
bmesh.ops.delete(pb_, geom=[v for v in pb_.verts if not v.link_faces], context='VERTS')
pb_.to_mesh(pk.data)
keep_isl = set(cfg['lowpoly']['body_islands'])
bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != 0 or isl[f] not in keep_isl], context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
isl = {f: v for f, v in isl.items() if f.is_valid}

# ---- per arm: the pad's end along the arm, the stock sleeve's surface, then the sleeve cut off below the pad --------
B = rig.data.bones
dl = bm.verts.layers.deform.active
arms = {}
for s, sign in (('l', 1), ('r', -1)):
    sh = rig.matrix_world @ B['bone_%s_upperarm' % s].head_local
    el = rig.matrix_world @ B['bone_%s_upperarm' % s].tail_local
    d = (el - sh).normalized()
    L = (el - sh).length
    side = [f for f in bm.faces if (f.calc_center_median() - sh).dot(d) > -0.05 and f.calc_center_median().x * sign > 0.15]
    sleeve = [f for f in side if isl[f] in SLEEVES[s]]
    pad_s = [(v.co - sh).dot(d) for f in side if isl[f] in PADS for v in f.verts]
    # no pad: the sleeve cut at the armhole (its start), the new arm tucked into the body there
    # no pad: the stock sleeve kept over the shoulder (to AP['keep_along'] metres along the upper arm), the new arm
    # tucked into it there as under a pad
    s_pad = max(pad_s) if pad_s else AP.get('keep_along', 0.07)
    bm.verts.index_update()
    tree = BVHTree.FromPolygons([v.co.copy() for v in bm.verts], [[v.index for v in f.verts] for f in sleeve])
    # the stock sleeve's triangles with their texture places: the new upper arm wears the stock's own sleeve paint
    tri_co, tri_uv = [], []
    for f in sleeve:
        ls = f.loops
        for i in range(1, len(ls) - 1):
            tri_co.append([ls[0].vert.co.copy(), ls[i].vert.co.copy(), ls[i + 1].vert.co.copy()])
            tri_uv.append([ls[0][uvl].uv.to_3d(), ls[i][uvl].uv.to_3d(), ls[i + 1][uvl].uv.to_3d()])
    tri_tree = BVHTree.FromPolygons([p for t in tri_co for p in t], [[3 * i, 3 * i + 1, 3 * i + 2] for i in range(len(tri_co))])
    s_cut = s_pad - 0.008
    cut = [f for f in sleeve if (f.calc_center_median() - sh).dot(d) > s_cut]
    # and any of the body's faces past the elbow on this arm (the stock forearm's pieces of other islands)
    cut += [f for f in side if f not in sleeve and (f.calc_center_median() - sh).dot(d) > L - 0.02 and f.calc_center_median().z > sh.z - 0.15]
    arms[s] = (sh, d, L, s_pad, s_cut, tree, (tri_tree, tri_co, tri_uv))
    bmesh.ops.delete(bm, geom=cut, context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    print('STOCKPREP arm %s: the pad ends %.3f m along the upper arm (of %.3f); the sleeve cut below it (%d faces)' % (
        s, s_pad, L, len(cut)))
# the neck closed (the head off): the open loop nearest the neck bone filled, the others (the pads' and the sleeve
# stubs' edges) stay open
neck = rig.matrix_world @ B['bone_neck'].head_local
loops, seen = [], set()
for e in bm.edges:
    if not e.is_boundary or e in seen:
        continue
    comp, stack = [], [e]
    while stack:
        x = stack.pop()
        if x in seen:
            continue
        seen.add(x)
        comp.append(x)
        for v in x.verts:
            stack += [y for y in v.link_edges if y.is_boundary and y not in seen]
    loops.append(comp)
neck_loop = min(loops, key=lambda c: (sum((v.co for e in c for v in e.verts), Vector()) / (2 * len(c)) - neck).length)
bmesh.ops.holes_fill(bm, edges=neck_loop, sides=0)

# ---- the new upper arms: AROUND points round, on the stock's sleeve surface (inset), weights from the stock ---------
upper_l = bm.faces.layers.int.new('upper_arm')     # 1: the new upper arms' faces (our sleeve's material)
gnames = [g.name for g in body.vertex_groups]
gi = {n: i for i, n in enumerate(gnames)}
for s, (sh, d, L, s_pad, s_cut, tree, (tri_tree, tri_co, tri_uv)) in arms.items():
    ref = Vector((0, 0, 1))
    ref = (ref - d * ref.dot(d)).normalized()
    side_ = d.cross(ref)
    s0 = s_cut - TUCK
    n_rings = max(2, int(round((L - s0) / RING_STEP)))
    rings = []
    for j in range(n_rings + 1):
        sj = s0 + (L - s0) * j / n_rings
        c = sh + d * sj
        ring = []
        last_r = None
        for k in range(AROUND):
            a = 2 * math.pi * k / AROUND
            dr = ref * math.cos(a) + side_ * math.sin(a)
            hit = tree.ray_cast(c + dr * 0.25, -dr, 0.25)[0] if tree else None
            r = ((hit - c).dot(dr) - INSET) if hit is not None else None
            ring.append(r)
        known = [r for r in ring if r is not None]
        mean_r = sum(known) / len(known) if known else 0.045
        ring = [r if r is not None else mean_r for r in ring]
        # under the pad (the rows above the cut) a little narrower still: tucked in
        if sj < s_cut:
            ring = [r - 0.002 for r in ring]
        rings.append((sj, c, ring))
    verts = []
    for sj, c, ring in rings:
        row = []
        for k, r in enumerate(ring):
            a = 2 * math.pi * k / AROUND
            v = bm.verts.new(c + (ref * math.cos(a) + side_ * math.sin(a)) * r)
            row.append(v)
        verts.append(row)
    bm.verts.index_update()
    # weights: the upper arm, the first rows (under the pad) partly the clavicle
    for j, row in enumerate(verts):
        for v in row:
            v[dl][gi['bone_%s_upperarm' % s]] = 1.0
            if j < 3 and ('bone_%s_clavicle' % s) in gi:
                v[dl][gi['bone_%s_clavicle' % s]] = 0.3 * (3 - j) / 3
    for j in range(len(verts) - 1):
        for k in range(AROUND):
            f = bm.faces.new([verts[j][k], verts[j][(k + 1) % AROUND], verts[j + 1][(k + 1) % AROUND], verts[j + 1][k]])
            f[upper_l] = 1
            f.smooth = True
            # the stock sleeve's paint (idren2023 2026-10-26: with our first-person sleeve's texture the upper arm wore
            # the forearm's cargo pocket): all four corners through the one stock triangle nearest the face's middle
            # (one triangle: never across the stock's seam)
            _, _, ti, _ = tri_tree.find_nearest(f.calc_center_median())
            a_, b_, c_ = tri_co[ti]
            for l in f.loops:
                l[uvl].uv = barycentric_transform(l.vert.co, a_, b_, c_, *tri_uv[ti]).to_2d()
    print('STOCKPREP upper arm %s: %d rings of %d points from %.3f to %.3f m along it' % (
        s, len(verts), AROUND, s0, L))
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
bm.to_mesh(body.data)
# materials: 0 the stock body (its 4x texture), 1 our sleeve's grey
hd = bpy.data.images.load(os.path.abspath(cfg['hd']))
arms_img = hd
body.data.materials.clear()
for name, img in (('player_body', hd), ('all_1st_inf_trooper_arms.tga', arms_img)):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    t = m.node_tree.nodes.new('ShaderNodeTexImage')
    m.node_tree.nodes.active = t  # (textured views draw the active image node)
    t.image = img
    m.node_tree.links.new(t.outputs['Color'], m.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
    body.data.materials.append(m)
ua = [d_.value for d_ in body.data.attributes['upper_arm'].data]
for p in body.data.polygons:
    p.use_smooth = True
    p.material_index = 0       # the new upper arms too: the stock's paint (ua kept for reference)
# placed on the skeleton itself (the stock object stands 1.3 m aside in its file; its points are in the skeleton's space)
for o in (body, pk):
    o.parent = rig
    o.matrix_parent_inverse = Matrix.Identity(4)
    o.matrix_basis = Matrix.Identity(4)
for o in list(sc.objects):
    if o.type == 'MESH' and o not in (body, pk):
        bpy.data.objects.remove(o)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(out))
print('STOCKPREP -> %s (%d faces)' % (out, len(body.data.polygons)))
