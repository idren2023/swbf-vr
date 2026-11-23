"""Blender (blender -b --factory-startup re/body/r_arm_weapons.blend -P tools/body_sleeve.py -- <out.blend> [sheet=<prefix>]):
the first-person sleeve rebuilt as a quad grid laid out in its texture (idren2023 2026-10-25: the arms up to the hand's
quality, the texture's details as geometry).

The stock sleeve is one texture island unwrapped as a cylinder (all_1st_inf_trooper_arms: u along the arm, 0 at the
elbow end, the cuff from u 0.66; v around it, the seam along its length). So the new surface is a grid in uv space:
rows at even steps along u, columns at even steps around v, and every painted feature's edges (FEATURES: the cuff's
rims, the strap's edge, the tab, the pocket) are grid lines with a support line one edge width inside, so each rises
over one row of quads and its loops follow the paint exactly. Every grid point is put where that uv lies on the
current smooth sleeve (r_forearm_retopo: the stock's own loops subdivided twice, uvs interpolated), so the shape and
the texture's placement stay the stock's; the uvs are the grid's own, exact. Then along the normal: the features'
heights (rounded edges), and the painted folds of the cloth as low relief (the luminance's medium band: light raised,
dark sunk; damped on the cuff's leather and under features). The grid's two seam columns are welded (the uv seam kept).

The sleeve's tiny second island (the leather cone into the glove's wrist, inside the cuff) is kept as it was. Weights:
copied from the old sleeve at the nearest point (the same rules: forearm / forearm_twist / wrist_twist / hand, tools/
body_forearm.py). The result replaces r_forearm_retopo's mesh (object, rig and modifiers kept); prints the grid,
quad count and relief range. With sheet=: six close-ups old | new (orthographic, textured) in <prefix>.png."""
import bpy, bmesh, math, os, sys
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import intersect_point_tri_2d, barycentric_transform

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
SHEET = next((a[6:] for a in args[1:] if a.startswith('sheet=')), None)
STEP_U, STEP_V = 0.02, 1 / 40.0   # the grid's even steps (about 5 mm along, 6 mm around)
AROUND = 72                       # points round the sleeve (the body's upper arm ring: 18 cage columns x 4)
MERGE_UV = 0.006                  # an even line this close to a feature line is dropped (no slivers)
FOLD_MM = 1.8                     # the painted folds' relief at full strength (cloth)
SMOOTH = 6                        # passes of neighbour averaging over the cloth's base surface (the stock's ridges)
FOLD_BANDS = (4, 18)              # texels of a 256 texture: luminance blurred by the first minus by the second
CUFF_U = 0.662                    # where the cuff's leather begins (folds damped beyond)
# (kind, (u0, v0, u1, v1, corner radius in uv), height mm, edge mm), from tools/body_refine.py's all_1st_trooper
# list, the stud left out (its own mesh, r_forearm_stud), and the strap's edge across the cuff added (the line at
# v 0.215 under the tab: the strap below it lies over the cuff, 1.2 mm).
FEATURES = [
    ('rect', (0.662, 0.03, 0.706, 0.99, 0.0), 3.5, 2.0),     # cuff rim, sleeve side
    # (no rim on the wrist side: the cuff is the glove's gauntlet, joined to the glove by a seam: tools/body_assemble.py)
    ('rect', (0.706, 0.03, 0.944, 0.215, 0.0), 1.2, 0.8),    # the strap under the tab
    # (the tab under the stud is a flap of its own: FLAP below)
    ('rect', (0.168, 0.302, 0.434, 0.580, 0.04), 3.0, 1.2),  # sleeve pocket
]

old = bpy.data.objects['r_forearm_retopo']
me0 = old.data
uv0 = me0.uv_layers.active
# ---- the reference surface: the main island's triangles in uv and 3D, bucketed by uv -------------------------------
bm0 = bmesh.new()
bm0.from_mesh(me0)
bmesh.ops.triangulate(bm0, faces=bm0.faces[:])
uvl = bm0.loops.layers.uv.active
tris = []
for f in bm0.faces:
    uvs = [l[uvl].uv.copy() for l in f.loops]
    if max(p.y for p in uvs) < 0.03:      # the cone's island (v 0.007..0.021): kept apart
        continue
    tris.append((uvs, [l.vert.co.copy() for l in f.loops], [l.vert.normal.copy() for l in f.loops]))
NB = 64
buckets = {}
for i, (uvs, _, _) in enumerate(tris):
    x0, x1 = int(min(p.x for p in uvs) * NB), int(max(p.x for p in uvs) * NB)
    y0, y1 = int(min(p.y for p in uvs) * NB), int(max(p.y for p in uvs) * NB)
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            buckets.setdefault((x, y), []).append(i)
U_MIN = min(p.x for t in tris for p in t[0])
U_MAX = max(p.x for t in tris for p in t[0])
V_MIN = min(p.y for t in tris for p in t[0])
V_MAX = max(p.y for t in tris for p in t[0])


def surface(u, v):
    """The reference's point and normal at uv (u, v), or None outside the island."""
    q = Vector((u, v))
    for i in buckets.get((int(u * NB), int(v * NB)), ()):
        uvs, ps, ns = tris[i]
        if intersect_point_tri_2d(q, uvs[0], uvs[1], uvs[2]):
            a, b, c = (Vector((p.x, p.y, 0)) for p in uvs)
            q3 = Vector((u, v, 0))
            p = barycentric_transform(q3, a, b, c, ps[0], ps[1], ps[2])
            n = barycentric_transform(q3, a, b, c, ns[0], ns[1], ns[2]).normalized()
            return p, n
    return None


def nearest_inside(u, v):
    """surface() at (u, v), or at the nearest uv inside the island (its ragged boundary)."""
    for r in (0, 0.002, 0.004, 0.008, 0.012, 0.02):
        for du, dv in ((0, 0), (r, 0), (-r, 0), (0, r), (0, -r), (r, r), (-r, -r), (r, -r), (-r, r)):
            s = surface(u + du, v + dv)
            if s:
                return s
    return None


# metres per uv along u and around v (the features' edge widths in uv)
mid = [surface(0.5, V_MIN + (V_MAX - V_MIN) * k / 8) for k in range(9)]
M_PER_V = sum((mid[k + 1][0] - mid[k][0]).length for k in range(8) if mid[k] and mid[k + 1]) / (V_MAX - V_MIN)
along = [surface(U_MIN + (U_MAX - U_MIN) * k / 10, 0.5) for k in range(11)]
M_PER_U = sum((along[k + 1][0] - along[k][0]).length for k in range(10) if along[k] and along[k + 1]) / (U_MAX - U_MIN)
print('SLEEVE island u %.3f..%.3f v %.3f..%.3f; %.0f mm per u, %.0f mm per v; reference %d tris' % (
    U_MIN, U_MAX, V_MIN, V_MAX, M_PER_U * 1000, M_PER_V * 1000, len(tris)))

# ---- grid lines ------------------------------------------------------------------------------------------------------
def lines(lo, hi, step, keep):
    keep = sorted(k for k in keep if lo < k < hi)
    even = [lo + (hi - lo) * i / round((hi - lo) / step) for i in range(round((hi - lo) / step) + 1)]
    out = [x for x in even if x in (lo, hi) or all(abs(x - k) > MERGE_UV for k in keep)]
    return sorted(set(out + keep))


ku, kv = [], []
for kind, (u0, v0, u1, v1, r), h, edge in FEATURES:
    eu, ev = edge / 1000 / M_PER_U, edge / 1000 / M_PER_V
    ku += [u0, u0 + eu, u1 - eu, u1]
    if v0 > V_MIN + 0.01:
        kv += [v0, v0 + ev]
    if v1 < V_MAX - 0.01:
        kv += [v1 - ev, v1]
US = lines(U_MIN, U_MAX, STEP_U, ku)
VS = lines(V_MIN, V_MAX, STEP_V, kv)
# exactly AROUND points round the sleeve (the seam's two lines are one): the upper arm's end ring has as many, so the
# elbow section between them is all quads (tools/body_assemble.py); even lines added in the widest gaps or taken
# out of the narrowest, feature lines kept
feature_v = set(k for k in kv if V_MIN < k < V_MAX)
while len(VS) - 1 > AROUND:
    cand = [i for i in range(1, len(VS) - 1) if VS[i] not in feature_v]
    i = min(cand, key=lambda i: VS[i + 1] - VS[i - 1])
    del VS[i]
while len(VS) - 1 < AROUND:
    i = max(range(len(VS) - 1), key=lambda i: VS[i + 1] - VS[i])
    VS.insert(i + 1, (VS[i] + VS[i + 1]) / 2)

# ---- heights ---------------------------------------------------------------------------------------------------------
def sdf_rect(u, v, u0, v0, u1, v1, r):
    cx, cy, hx, hy = (u0 + u1) / 2, (v0 + v1) / 2, (u1 - u0) / 2 - r, (v1 - v0) / 2 - r
    qx, qy = abs(u - cx) - hx, abs(v - cy) - hy
    return r - (math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0))


def feature_height(u, v):
    """The highest feature at (u, v) in metres, and how much one covers it (0..1)."""
    best, cover = 0.0, 0.0
    for kind, (u0, v0, u1, v1, r), h, edge in FEATURES:
        # the edge width in uv along the nearer direction (u and v scale differently)
        d_m = min(sdf_rect(u, v, u0, v0, u1, v1, r) * M_PER_U, sdf_rect(u, v, u0, v0, u1, v1, r) * M_PER_V)
        du = min(u - u0, u1 - u) * M_PER_U
        dv = min(v - v0, v1 - v) * M_PER_V if (v0 > V_MIN + 0.01 or v1 < V_MAX - 0.01) else 1.0
        d = min(du, dv) if r == 0 else d_m
        if d > 0:
            t = min(d / (edge / 1000), 1.0)
            best = max(best, h / 1000 * math.sqrt(1 - (1 - t) ** 2))
            cover = max(cover, t)
    return best, cover


img = next(m.node_tree.nodes['Image Texture'].image for m in me0.materials
           if m.use_nodes and 'Image Texture' in m.node_tree.nodes and 'arms' in m.name.lower()) \
    if any('arms' in m.name.lower() for m in me0.materials) else None
if img is None:
    path = 'C:/LucasArts/BFBuilder/Assets/Sides/ALL/MSH/all_1st_inf_trooper_arms.tga'
    img = bpy.data.images.load(path)
W_, H_ = img.size
px = img.pixels[:]
lum = [0.3 * px[4 * i] + 0.59 * px[4 * i + 1] + 0.11 * px[4 * i + 2] for i in range(W_ * H_)]


def blur(a, r):
    def one(a, stride, n, count, step_line):
        out = a[:]
        for line in range(count):
            base = line * step_line
            vals = [a[base + k * stride] for k in range(n)]
            pre = [0.0]
            for x in vals:
                pre.append(pre[-1] + x)
            for k in range(n):
                lo, hi = max(0, k - r), min(n, k + r + 1)
                out[base + k * stride] = (pre[hi] - pre[lo]) / (hi - lo)
        return out
    return one(one(a, 1, W_, H_, W_), W_, H_, W_, 1)


ra, rb = (max(1, b * W_ // 256) for b in FOLD_BANDS)
band = [x - y for x, y in zip(blur(lum, ra), blur(lum, rb))]
scale = sorted(abs(x) for x in band)[int(len(band) * 0.98)] or 1.0


def fold(u, v):
    x, y = min(max(u, 0), 1) * (W_ - 1), min(max(v, 0), 1) * (H_ - 1)
    x0, y0 = int(x), int(y)
    x1, y1 = min(x0 + 1, W_ - 1), min(y0 + 1, H_ - 1)
    fx, fy = x - x0, y - y0
    a = band[y0 * W_ + x0] * (1 - fx) + band[y0 * W_ + x1] * fx
    b = band[y1 * W_ + x0] * (1 - fx) + band[y1 * W_ + x1] * fx
    return max(-1.0, min(1.0, (a * (1 - fy) + b * fy) / scale))


# ---- the grid --------------------------------------------------------------------------------------------------------
bm = bmesh.new()
uvn = bm.loops.layers.uv.new('UV')
base = {}
for i, u in enumerate(US):
    for j, v in enumerate(VS):
        s = nearest_inside(u, v)
        if s is not None:
            base[i, j] = [s[0].copy(), s[1].copy()]
# the cloth's base smoothed (the stock sleeve's facets left sharp ridges along it): each point toward the mean of its
# four grid neighbours (around the seam too), the cuff, the elbow's open edge and the row by the cuff kept
cuff_row = min(i for i, u in enumerate(US) if u >= CUFF_U)
LAST = len(VS) - 1
for i in range(len(US)):   # the seam's two columns are one line of points: start them together
    if (i, 0) in base and (i, LAST) in base:
        base[i, 0][0] = base[i, LAST][0] = (base[i, 0][0] + base[i, LAST][0]) / 2
for _ in range(SMOOTH):
    moved = {}
    for (i, j), (p, n) in base.items():
        if i == 0 or i >= cuff_row - 1 or j == LAST:
            continue
        # around: the seam column (0, the same points as LAST) has LAST - 1 on its other side
        left, right = (LAST - 1 if j == 0 else j - 1), j + 1
        nb = [base.get((i + 1, j)), base.get((i - 1, j)), base.get((i, left)), base.get((i, right))]
        nb = [q for q in nb if q]
        mean = sum((q[0] for q in nb), Vector()) / len(nb)
        moved[i, j] = p.lerp(mean, 0.5)
    for (i, j), p in moved.items():
        base[i, j][0] = p
        if j == 0 and (i, LAST) in base:
            base[i, LAST][0] = p
grid, lifts = {}, []
for i, u in enumerate(US):
    for j, v in enumerate(VS):
        if (i, j) not in base:
            continue
        p, n = base[i, j]
        hf, cover = feature_height(u, v)
        damp = 0.25 if u >= CUFF_U else 1.0
        # folds fade out toward the island's ends (the elbow's open edge, the cuff) and under features
        lift = hf + FOLD_MM / 1000 * fold(u, v) * damp * (1 - cover)
        lifts.append(lift)
        grid[i, j] = (bm.verts.new(p + n * lift), u, v)
for i in range(len(US) - 1):
    for j in range(len(VS) - 1):
        corners = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
        if all(c in grid for c in corners):
            f = bm.faces.new([grid[c][0] for c in corners])
            for l, c in zip(f.loops, corners):
                l[uvn].uv = (grid[c][1], grid[c][2])
            f.material_index = 1
            f.smooth = True
# ---- the strap's flap (idren2023 2026-10-25: a true flap, attached at the strap's seam by its flat end): its own leather
# layer over the cuff, the painted tab's outline (u across, v from the flat end at the strap's seam to the rounded
# free end), FLAP_LIFT proud of the cuff, ramping down into it over FLAP_ATTACH at the flat end, a side wall down to
# the cuff round the free edges; textured with the tab's own paint. The stud rides on it (lifted as well).
FLAP_U0, FLAP_U1, FLAP_V0, FLAP_V1 = 0.782, 0.908, 0.232, 0.418
FLAP_LIFT, FLAP_ATTACH, FLAP_COLS, FLAP_ROWS = 0.0030, 0.004, 10, 14  # 3 mm (idren2023: a little thicker)
fr_m = (FLAP_U1 - FLAP_U0) / 2 * M_PER_U          # the rounded end's radius (metres)
fv_c = FLAP_V1 - fr_m / M_PER_V                    # where the rounding begins (v)
fu_c = (FLAP_U0 + FLAP_U1) / 2
v_end = fv_c + 0.92 * fr_m / M_PER_V               # the last row (a little short of the tip: no zero-width row)


def flap_point(i, j):
    v = FLAP_V0 + (v_end - FLAP_V0) * i / FLAP_ROWS
    hw = fr_m if v <= fv_c else math.sqrt(max(fr_m ** 2 - ((v - fv_c) * M_PER_V) ** 2, 0.0))
    u = fu_c + (j / FLAP_COLS * 2 - 1) * hw / M_PER_U
    srf = nearest_inside(u, v)
    lift = FLAP_LIFT * min(1.0, ((v - FLAP_V0) * M_PER_V) / FLAP_ATTACH) ** 0.5
    return srf, u, v, lift


flap_top, flap_low = {}, {}
for i in range(FLAP_ROWS + 1):
    for j in range(FLAP_COLS + 1):
        (p0, n0), u, v, lift = flap_point(i, j)
        flap_top[i, j] = (bm.verts.new(p0 + n0 * lift), u, v)
        free = (j in (0, FLAP_COLS) or i == FLAP_ROWS) and i > 0
        if free:
            flap_low[i, j] = (bm.verts.new(p0 + n0 * 0.0002), u, v)


def flap_face(keys, src):
    f = bm.faces.new([src[k_][0] for k_ in keys])
    for l, k_ in zip(f.loops, keys):
        l[uvn].uv = (flap_top[k_][1], flap_top[k_][2])
    f.material_index = 1
    f.smooth = True
    return f


for i in range(FLAP_ROWS):
    for j in range(FLAP_COLS):
        flap_face([(i, j), (i, j + 1), (i + 1, j + 1), (i + 1, j)], flap_top)
# the walls: down each side and across the rounded end
edges = [((i, 0), (i + 1, 0)) for i in range(1, FLAP_ROWS)] + \
        [((i + 1, FLAP_COLS), (i, FLAP_COLS)) for i in range(1, FLAP_ROWS)] + \
        [((FLAP_ROWS, j), (FLAP_ROWS, j + 1)) for j in range(FLAP_COLS)]
for a_, b_ in edges:
    f = bm.faces.new([flap_top[a_][0], flap_top[b_][0], flap_low[b_][0], flap_low[a_][0]])
    for l, k_ in zip(f.loops, (a_, b_, b_, a_)):
        l[uvn].uv = (flap_top[k_][1], flap_top[k_][2])
    f.material_index = 1
    f.smooth = False
# the stud onto the flap
stud = bpy.data.objects.get('r_forearm_stud')
if stud:
    (sp, sn), _, _, _ = flap_point(FLAP_ROWS // 2, FLAP_COLS // 2)
    for v in stud.data.vertices:
        v.co += sn * FLAP_LIFT
print('SLEEVE flap: %d x %d quads and its walls, %.1f mm proud, radius %.0f mm; stud lifted' % (
    FLAP_ROWS, FLAP_COLS, FLAP_LIFT * 1000, fr_m * 1000))

# the seam: the first and last columns meet in 3D (welded, the uvs stay apart)
seam = [grid[i, 0][0] for i in range(len(US)) if (i, 0) in grid] + \
       [grid[i, len(VS) - 1][0] for i in range(len(US)) if (i, len(VS) - 1) in grid]
gap = max((grid[i, 0][0].co - grid[i, len(VS) - 1][0].co).length for i in range(len(US))
          if (i, 0) in grid and (i, len(VS) - 1) in grid)
bmesh.ops.remove_doubles(bm, verts=seam, dist=0.004)
bm.normal_update()
# outward normals (the grid's winding follows the uv layout)
probe = next(iter(bm.faces))
centre = sum((v.co for v in bm.verts), Vector()) / len(bm.verts)
if probe.normal.dot(probe.calc_center_median() - centre) < 0:
    bmesh.ops.reverse_faces(bm, faces=bm.faces[:])
# the cone into the glove, as it was
cone = bmesh.new()
cone.from_mesh(me0)
cuvl = cone.loops.layers.uv.active
bmesh.ops.delete(cone, geom=[f for f in cone.faces if max(l[cuvl].uv.y for l in f.loops) >= 0.03], context='FACES')
bmesh.ops.delete(cone, geom=[v for v in cone.verts if not v.link_faces], context='VERTS')
cone_faces = len(cone.faces)
tmp = bpy.data.meshes.new('cone_tmp')
cone.to_mesh(tmp)
bm.from_mesh(tmp)
quads = sum(1 for f in bm.faces if len(f.verts) == 4)
new = bpy.data.meshes.new('r_forearm_retopo')
bm.to_mesh(new)
new.uv_layers.active.name = uv0.name
for mt in me0.materials:
    new.materials.append(mt)
print('SLEEVE grid %d x %d lines (u, v): %d faces, %d quads (+%d cone faces), %d tris; seam gap %.1f mm welded; '
      'relief %.1f..%.1f mm' % (len(US), len(VS), len(new.polygons), quads, cone_faces,
                                 sum(len(p.vertices) - 2 for p in new.polygons), gap * 1000,
                                 min(lifts) * 1000, max(lifts) * 1000))

# ---- weights from the old sleeve, then swap the mesh in ---------------------------------------------------------------
ob0 = bmesh.new()
ob0.from_mesh(me0)
tree0 = BVHTree.FromBMesh(ob0)
names = [g.name for g in old.vertex_groups]
wold = [{names[g.group]: g.weight for g in v.groups} for v in me0.vertices]
ob0.faces.ensure_lookup_table()
keep_old = me0
old.data = new
for g in list(old.vertex_groups):
    old.vertex_groups.remove(g)
G = {n: old.vertex_groups.new(name=n) for n in names}
for v in new.vertices:
    loc, nrm, fi, dist = tree0.find_nearest(v.co)
    f = ob0.faces[fi]
    best = min(f.verts, key=lambda x: (x.co - loc).length)
    for n, w in wold[best.index].items():
        if w > 0.001:
            G[n].add([v.index], w, 'REPLACE')
keep_old.name = 'r_forearm_retopo_old'
keep_old.use_fake_user = True

if SHEET:
    # per view two pictures, the old sleeve and the new one with its edges drawn over it (a wireframe copy), for
    # tools/sheet.ps1: <prefix>-<view>-old.png, <prefix>-<view>-new.png
    scene = bpy.context.scene
    view_old = bpy.data.objects.new('sleeve_old_view', keep_old)
    scene.collection.objects.link(view_old)
    view_old.matrix_world = old.matrix_world.copy()
    wire = bpy.data.objects.new('sleeve_wire', new)
    scene.collection.objects.link(wire)
    wire.matrix_world = old.matrix_world.copy()
    wm = wire.modifiers.new('wire', 'WIREFRAME')
    wm.thickness = 0.0005
    wm.use_replace = True
    wm.offset = 1.0
    black = bpy.data.materials.new('wire_black')
    black.diffuse_color = (0.02, 0.02, 0.02, 1)
    wire.data = new.copy()
    wire.data.materials.clear()
    wire.data.materials.append(black)
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.show_cavity = True
    scene.render.resolution_x, scene.render.resolution_y = 700, 700
    cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.data.type = 'ORTHO'
    cam.data.ortho_scale = 0.30
    target = sum((old.matrix_world @ v.co for v in new.vertices), Vector()) / len(new.vertices)
    shots = []
    for name, d in (('back', (0, 0, 1)), ('palm', (0, 0, -1)), ('thumb', (1, 0, 0)), ('cuff3q', (0.5, 0.5, 0.7))):
        dv = Vector(d).normalized()
        cam.location = target + dv * 1.0
        cam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
        for which, shown, colour in (('old', ('sleeve_old_view',), 'TEXTURE'),
                                     ('new', ('r_forearm_retopo', 'sleeve_wire'), 'MATERIAL')):
            for o in scene.objects:
                o.hide_render = o.name not in shown
            scene.display.shading.color_type = 'TEXTURE'
            path = os.path.abspath('%s-%s-%s.png' % (SHEET, name, which))
            scene.render.filepath = path
            bpy.ops.render.render(write_still=True)
            shots.append(path)
    print('SHEET ' + ' '.join(shots))
    for n in ('sleeve_old_view', 'sleeve_wire', 'cam'):
        bpy.data.objects.remove(bpy.data.objects[n])
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
print('SAVED', OUT)
