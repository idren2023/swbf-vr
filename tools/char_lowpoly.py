"""The game mesh of a stock character (step 2 of the bake plan; idren2023 2026-10-26): a clean all-quad mesh, its edge
loops along the fabric borders (from the fabric maps), at a game polygon count; the detailed model is baked onto it.
The source: the stock body (body islands only: head off) subdivided and displaced by the relief map (the layers'
steps belong in the silhouette); the borders between fabrics marked sharp; QuadriFlow lays an even quad field
aligned to them; the result shrink-wrapped back onto the source. Its own texture layout (smart project, for now).
Run: blender -b --factory-startup <stock.blend> -P tools/char_lowpoly.py -- <config.json> <object> <height.png | none> <out.blend>
"""
import bpy, bmesh, json, math, os, sys
import numpy as np
from mathutils import Vector, Matrix

cfg_path, obj_name, height, out = sys.argv[sys.argv.index('--') + 1:][:4]
cfg = json.load(open(cfg_path))
lp = cfg['lowpoly']
sc = bpy.context.scene
stock = bpy.data.objects[obj_name]
stock.parent = None
stock.matrix_world = Matrix.Identity(4)
for m in list(stock.modifiers):
    stock.modifiers.remove(m)
island_map = np.load(cfg['texmaps'])['island']
N = island_map.shape[0]


def texel(uv):
    return min(int((1 - uv.y % 1.0) * N), N - 1), min(int(uv.x % 1.0 * N), N - 1)


def load_mask(path):
    im = bpy.data.images.load(os.path.abspath(path))
    W, H = im.size
    a = np.array(im.pixels[:], np.float32).reshape(H, W, 4)[::-1, :, 0] > 0.5
    bpy.data.images.remove(im)
    return a


# one label per texel: the fabrics in the config's order (later ones win), 0 = none
fabrics = list(cfg['fabrics'])
label = np.zeros((N, N), np.int32)
for i, fname in enumerate(fabrics):
    label[load_mask(cfg['out'][fname])] = i + 1

# ---- the source: body faces, welded, subdivided, relief ---------------------------------------------------------
bm = bmesh.new()
bm.from_mesh(stock.data)
uvl = bm.loops.layers.uv.active
drop = [f for f in bm.faces if f.material_index != 0 or island_map[texel(
    sum((l[uvl].uv for l in f.loops), Vector((0, 0))) / len(f.loops))] not in lp['body_islands']]
bmesh.ops.delete(bm, geom=drop, context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=0.0002)
# manifold for QuadriFlow: edges with more than two faces lose their extra faces; the openings closed (the neck too:
# the head is off); normals consistent
for _ in range(3):
    bad = [e for e in bm.edges if len(e.link_faces) > 2]
    if not bad:
        break
    extra = set()
    for e in bad:
        extra.update(sorted(e.link_faces, key=lambda f: f.calc_area())[:len(e.link_faces) - 2])
    bmesh.ops.delete(bm, geom=list(extra), context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
n_open = sum(1 for e in bm.edges if e.is_boundary)
bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=0)
# a hole whose edge touches itself (a pinch point) does not fill: the faces round each pinch out, filled again
for _ in range(6):
    pinch = [v for v in bm.verts if sum(1 for e in v.link_edges if e.is_boundary) > 2]
    if not pinch and not any(e.is_boundary for e in bm.edges):
        break
    bmesh.ops.delete(bm, geom=list({f for v in pinch for f in v.link_faces}), context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=0)
bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
def _same_dir(e):
    f1, f2 = e.link_faces
    l1 = next(l for l in f1.loops if l.edge is e)
    l2 = next(l for l in f2.loops if l.edge is e)
    return l1.vert is l2.vert


def _fan_broken(v):
    fs = set(v.link_faces)
    if not fs:
        return False
    seen, stack = set(), [next(iter(fs))]
    while stack:
        f = stack.pop()
        if f in seen:
            continue
        seen.add(f)
        for e in f.edges:
            if v in e.verts:
                stack.extend(g for g in e.link_faces if g not in seen)
    return len(seen) != len(fs)


print('LOWPOLY flipped edges %d, pinched points %d' % (
    sum(1 for e in bm.edges if len(e.link_faces) == 2 and _same_dir(e)), sum(1 for v in bm.verts if _fan_broken(v))))
print('LOWPOLY openings closed (%d edges); non-manifold edges left %d, open edges left %d' % (
    n_open, sum(1 for e in bm.edges if len(e.link_faces) > 2), sum(1 for e in bm.edges if e.is_boundary)))
me = bpy.data.meshes.new('lp_source')
bm.to_mesh(me)
src = bpy.data.objects.new('lp_source', me)
sc.collection.objects.link(src)
sub = src.modifiers.new('sub', 'SUBSURF')
sub.levels = lp['source_subd']
sub.uv_smooth = 'PRESERVE_BOUNDARIES'
if height != 'none':        # 'none': straight over the stock's surface (idren2023 2026-10-26: compare with the stock)
    himg = bpy.data.images.load(os.path.abspath(height))
    himg.colorspace_settings.name = 'Non-Color'
    rng = json.load(open(height.rsplit('.', 1)[0] + '.json'))
    tex = bpy.data.textures.new('h', 'IMAGE')
    tex.image = himg
    tex.extension = 'EXTEND'
    d = src.modifiers.new('disp', 'DISPLACE')
    d.texture = tex
    d.texture_coords = 'UV'
    span = (rng['max_mm'] - rng['min_mm']) / 1000
    d.strength = span
    d.mid_level = -rng['min_mm'] / 1000 / span
bpy.context.view_layer.objects.active = src
for o in sc.objects:
    o.select_set(o is src)
bpy.ops.object.modifier_apply(modifier='sub')
if height != 'none':
    bpy.ops.object.modifier_apply(modifier='disp')

# the fabric borders sharp (QuadriFlow aligns its field to sharp edges); edges under 0.1 mm collapsed first
# (QuadriFlow refuses a mesh with any: the stock's tiny edges, subdivided)
bm = bmesh.new()
bm.from_mesh(src.data)
for _ in range(4):
    short = [e for e in bm.edges if e.calc_length() < 0.00015]
    if not short:
        break
    bmesh.ops.collapse(bm, edges=short)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
uvl = bm.loops.layers.uv.active
bmesh.ops.triangulate(bm, faces=bm.faces[:])
for e in bm.edges:
    e.smooth = True
# the fabric outlines cut into the source exactly (marching triangles: each edge whose ends lie on two fabrics split
# where the paint changes, found along its texture place; the two splits of a triangle joined): smooth lines of edges
# for QuadriFlow to follow, not the steps of whole faces
vlab = {v: int(label[texel(v.link_loops[0][uvl].uv)]) for v in bm.verts if v.link_loops}
cut_v = set()
for e in [e for e in bm.edges if e.link_loops and vlab.get(e.verts[0], 0) != vlab.get(e.verts[1], 0)
          and vlab.get(e.verts[0], 0) > 0 and vlab.get(e.verts[1], 0) > 0]:
    l0 = e.link_loops[0]
    a_v = l0.vert
    uv_a, uv_b = l0[uvl].uv.copy(), l0.link_loop_next[uvl].uv.copy()
    la = vlab[a_v]
    lo_, hi_ = 0.0, 1.0
    for _ in range(8):
        mid = (lo_ + hi_) / 2
        if int(label[texel(uv_a.lerp(uv_b, mid))]) == la:
            lo_ = mid
        else:
            hi_ = mid
    t = min(max((lo_ + hi_) / 2, 0.15), 0.85)
    _, nv = bmesh.utils.edge_split(e, a_v, t)
    cut_v.add(nv)
n_cut = 0
for f in list(bm.faces):
    nv = [v for v in f.verts if v in cut_v]
    if len(nv) == 2:
        r = bmesh.ops.connect_verts(bm, verts=nv)
        for e in r['edges']:
            e.smooth = False
            n_cut += 1
# the joint rings: planes square to the bones at the shoulders, elbows, hips and knees cut into the limb round each
rig = bpy.data.objects[lp['skeleton']]
n_ring = 0
for bname, frac, rad, half in lp['joint_rings']:
    for side in 'lr':
        b = rig.data.bones[bname % side]
        h, t_ = rig.matrix_world @ b.head_local, rig.matrix_world @ b.tail_local
        axis = (t_ - h).normalized()
        pc = h.lerp(t_, frac)
        near = []
        for f in bm.faces:
            dv = f.calc_center_median() - pc
            if abs(dv.dot(axis)) < half and (dv - axis * dv.dot(axis)).length < rad:
                near.append(f)
        if not near:
            continue
        geom = list({g for f in near for g in list(f.verts) + list(f.edges)}) + near
        r = bmesh.ops.bisect_plane(bm, geom=geom, dist=0.0002, plane_co=pc, plane_no=axis)
        for g in r['geom_cut']:
            if isinstance(g, bmesh.types.BMEdge):
                g.smooth = False
                n_ring += 1
bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
for _ in range(6):          # the cuts' own tiny edges collapsed (QuadriFlow refuses edges under 0.1 mm)
    short = [e for e in bm.edges if e.calc_length() < 0.00015]
    if not short:
        break
    bmesh.ops.collapse(bm, edges=short)
bmesh.ops.dissolve_degenerate(bm, dist=0.00015, edges=bm.edges[:])
bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
for _ in range(3):          # an edge the collapse left with three faces: its smallest extra face out
    bad = [e for e in bm.edges if len(e.link_faces) > 2]
    if not bad:
        break
    extra = set()
    for e in bad:
        extra.update(sorted(e.link_faces, key=lambda f: f.calc_area())[:len(e.link_faces) - 2])
    bmesh.ops.delete(bm, geom=list(extra), context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=0)
bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
print('LOWPOLY after the cuts: short edges %d, non-manifold %d, open %d' % (
    sum(1 for e in bm.edges if e.calc_length() < 0.0001), sum(1 for e in bm.edges if len(e.link_faces) > 2),
    sum(1 for e in bm.edges if e.is_boundary)))
bm.to_mesh(src.data)
print('LOWPOLY source: %d faces; outline cut %d edges, joint rings %d edges (sharp)' % (
    len(src.data.polygons), n_cut, n_ring))

# ---- QuadriFlow, then back onto the source ------------------------------------------------------------------------
low = src.copy()
low.data = src.data.copy()
low.name = 'body_low'
sc.collection.objects.link(low)
bpy.context.view_layer.objects.active = low
for o in sc.objects:
    o.select_set(o is low)
res = bpy.ops.object.quadriflow_remesh(use_mesh_symmetry=False, use_preserve_sharp=True, use_preserve_boundary=True,
                                 preserve_attributes=False, smooth_normals=False, mode='FACES',
                                 target_faces=lp['target_quads'], seed=0)
print('LOWPOLY quadriflow:', res)
sw = low.modifiers.new('wrap', 'SHRINKWRAP')
sw.target = src
sw.wrap_method = 'NEAREST_SURFACEPOINT'
bpy.ops.object.modifier_apply(modifier='wrap')
for p in low.data.polygons:
    p.use_smooth = True
nq = sum(1 for p in low.data.polygons if len(p.vertices) == 4)
print('LOWPOLY game mesh: %d faces (%d quads), %d tris' % (
    len(low.data.polygons), nq, sum(len(p.vertices) - 2 for p in low.data.polygons)))

# ---- the fabric borders onto the outline: each border point of the game mesh (between faces of two fabrics) moves to
# the nearest point of the two fabrics' outline (texel-exact, in 3D through texmaps), the points round them relaxed
from mathutils.bvhtree import BVHTree  # noqa: E402
from mathutils.kdtree import KDTree  # noqa: E402
pos_map = np.load(cfg['texmaps'])['pos']
outline = {}
for dy, dx in ((0, 1), (1, 0)):
    a_ = label[:N - dy, :N - dx]
    b_ = label[dy:, dx:]
    ys, xs = np.nonzero((a_ != b_) & (a_ > 0) & (b_ > 0))
    for y, x in zip(ys, xs):
        k = tuple(sorted((int(a_[y, x]), int(b_[y, x]))))
        outline.setdefault(k, []).append(pos_map[y, x])
kds = {}
for k, pts in outline.items():
    kd = KDTree(len(pts))
    for i, p_ in enumerate(pts):
        kd.insert(Vector(p_.tolist()), i)
    kd.balance()
    kds[k] = kd
src_bvh = BVHTree.FromObject(src, bpy.context.evaluated_depsgraph_get())
suv_ = src.data.uv_layers.active.data


def face_label(p):
    _, _, fi, _ = src_bvh.find_nearest(p.calc_center_median() if hasattr(p, 'calc_center_median') else p.center)
    sp = src.data.polygons[fi]
    uv = sum((suv_[li].uv for li in sp.loop_indices), Vector((0, 0))) / len(sp.loop_indices)
    return int(label[texel(uv)])


lb = bmesh.new()
lb.from_mesh(low.data)
lb.faces.ensure_lookup_table()
lb.verts.ensure_lookup_table()
snapped = 0
for rnd_ in range(lp['snap_rounds']):
    flab_l = {f.index: face_label(f) for f in lb.faces}
    # specks out: a face whose edge neighbours mostly agree on another fabric takes theirs
    for _ in range(2):
        for f in lb.faces:
            nl = [flab_l[g.index] for e in f.edges for g in e.link_faces if g is not f]
            if nl:
                best = max(set(nl), key=nl.count)
                if best != flab_l[f.index] and nl.count(best) >= 3:
                    flab_l[f.index] = best
    # small patches (under speck_faces faces of one fabric) take the fabric round them
    seen = set()
    for f in lb.faces:
        if f.index in seen:
            continue
        comp, stack = [], [f]
        seen.add(f.index)
        while stack:
            g = stack.pop()
            comp.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index not in seen and flab_l[h.index] == flab_l[f.index]:
                        seen.add(h.index)
                        stack.append(h)
        if len(comp) < lp['speck_faces']:
            cs = {g.index for g in comp}
            nl = [flab_l[h.index] for g in comp for e in g.edges for h in e.link_faces if h.index not in cs]
            if nl:
                best = max(set(nl), key=nl.count)
                for g in comp:
                    flab_l[g.index] = best
    border = {}
    for v in lb.verts:
        ls_ = {flab_l[f.index] for f in v.link_faces}
        if len(ls_) == 2 and 0 not in ls_:
            border[v] = tuple(sorted(ls_))
    for v, k in border.items():
        if k in kds:
            co, _, d_ = kds[k].find(v.co)
            if d_ < lp['snap_reach']:
                v.co = src_bvh.find_nearest(co)[0]
                snapped += rnd_ == 0
    # the row beside each border square to it: each neighbour off the border goes one edge length straight out from
    # the border (across it, on its own side), so the quads along a border run with it
    lb.normal_update()
    ring1 = {}
    for v, k in border.items():
        bn = [e.other_vert(v) for e in v.link_edges if border.get(e.other_vert(v)) == k]
        if len(bn) != 2:
            continue
        tng = (bn[1].co - bn[0].co).normalized()
        nrm_ = v.normal if v.normal.length > 0.1 else Vector((0, 0, 1))
        side = nrm_.cross(tng).normalized()
        for e in v.link_edges:
            n_ = e.other_vert(v)
            if n_ in border:
                continue
            sg = 1.0 if (n_.co - v.co).dot(side) >= 0 else -1.0
            ring1.setdefault(n_, []).append(v.co + side * sg * e.calc_length())
    if lp.get('square_rows'):
        for n_, ts in ring1.items():
            n_.co = src_bvh.find_nearest(n_.co.lerp(sum(ts, Vector()) / len(ts), 0.7))[0]
    else:
        ring1 = {}
    for _ in range(lp['relax']):
        new = {}
        for v in lb.verts:
            if v in border or v in ring1:
                continue
            nb = [e.other_vert(v) for e in v.link_edges]
            new[v] = src_bvh.find_nearest(v.co.lerp(sum((n_.co for n_ in nb), Vector()) / len(nb), 0.5))[0]
        for v, p_ in new.items():
            v.co = p_
fab_l = lb.faces.layers.int.new('fabric')     # each face's fabric (1 + its index in the config), specks out
for f in lb.faces:
    f[fab_l] = flab_l[f.index]
lb.to_mesh(low.data)
print('LOWPOLY borders onto the outline: %d points, %d rounds' % (snapped, lp['snap_rounds']))

# ---- its own texture layout ---------------------------------------------------------------------------------------
# the stock's own layout (hand-made, its seams out of sight): each face takes the island under its middle, each of its
# corners the texture place of the nearest point of that island; edges between islands marked as seams
from mathutils.geometry import barycentric_transform  # noqa: E402
sp_ = src.data.polygons
# the source's islands from its own connectivity (faces joined by an edge whose texture places agree on both sides):
# by the texture's place alone, islands that reuse the same paint (the stock's inner front reuses the vest's) mixed
parent_ = list(range(len(sp_)))


def _root(a):
    while parent_[a] != a:
        parent_[a] = parent_[parent_[a]]
        a = parent_[a]
    return a


edge_uv = {}
for p_ in sp_:
    lis = list(p_.loop_indices)
    for j, li in enumerate(lis):
        lj = lis[(j + 1) % len(lis)]
        va, vb = src.data.loops[li].vertex_index, src.data.loops[lj].vertex_index
        ua, ub = tuple(round(x, 5) for x in suv_[li].uv), tuple(round(x, 5) for x in suv_[lj].uv)
        key = (min(va, vb), max(va, vb))
        sig = (ua, ub) if va < vb else (ub, ua)
        if key in edge_uv:
            other, osig = edge_uv[key]
            if osig == sig:
                parent_[_root(p_.index)] = _root(other)
        else:
            edge_uv[key] = (p_.index, sig)
s_isl = [_root(p_.index) for p_ in sp_]
s_co = [v.co for v in src.data.vertices]
isl_bvh, isl_faces = {}, {}
for i_ in set(s_isl):
    fl = [p_.index for p_ in sp_ if s_isl[p_.index] == i_]
    isl_faces[i_] = fl
    isl_bvh[i_] = BVHTree.FromPolygons(s_co, [list(sp_[fi].vertices) for fi in fl])
uvo = low.data.uv_layers.new(name='UV')
low_isl = []
# each face's island: the one most of its corners lie on (by its middle alone, faces at an island's edge took the
# neighbouring island and stretched across it: wrong paint in diamonds on the vest)
corner_isl = {}
for v in low.data.vertices:
    corner_isl[v.index] = s_isl[src_bvh.find_nearest(v.co)[2]]
for p_ in low.data.polygons:
    # of the islands under its corners and middle, the one whose surface lies nearest to all its corners together
    # (by a vote, a face could take an island its corners are not on: each corner clamped to the island's border,
    # the face one colour - diamonds and blocks of wrong paint)
    cands = {corner_isl[vi] for vi in p_.vertices}
    cands.add(s_isl[src_bvh.find_nearest(p_.center)[2]])
    i_ = min(cands, key=lambda c_: sum(isl_bvh[c_].find_nearest(low.data.vertices[vi].co)[3] for vi in p_.vertices))
    low_isl.append(i_)
    for li, vi in zip(p_.loop_indices, p_.vertices):
        loc, _, k, _ = isl_bvh[i_].find_nearest(low.data.vertices[vi].co)
        sf = sp_[isl_faces[i_][k]]
        a_, b_, c_ = (s_co[x] for x in list(sf.vertices)[:3])
        ua, ub, uc = (suv_[x].uv.to_3d() for x in list(sf.loop_indices)[:3])
        uvo.data[li].uv = barycentric_transform(loc, a_, b_, c_, ua, ub, uc).to_2d()
ef = {}
for p_ in low.data.polygons:
    for ek in p_.edge_keys:
        ef.setdefault(ek, []).append(low_isl[p_.index])
n_seam = 0
for e in low.data.edges:
    l_ = ef.get(e.key, [])
    e.use_seam = len(l_) == 2 and l_[0] != l_[1]
    n_seam += e.use_seam
print("LOWPOLY texture layout: the stock's, %d islands, %d seam edges" % (len(set(low_isl)), n_seam))
for o in list(sc.objects):
    if o.type == 'MESH' and o not in (low, src):
        bpy.data.objects.remove(o)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(out))
print('LOWPOLY -> %s' % out)
