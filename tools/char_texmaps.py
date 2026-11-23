"""Per-texel maps of a stock character's texture, for segmenting its paint into fabrics (step 1 of the per-character
pipeline): for every texel of the main material's texture, where on the body it lies (position and normal in the
model's own coordinates), which texture island it is on, how it curves there (creases and ridges), and whether the model uses it at all.
Run: blender -b --factory-startup <char>.blend -P tools/char_texmaps.py -- <object> <size> <out.npz>
     (rebel soldier: re/body/all_inf_soldier.blend all_inf_soldier_hi 2048 re/body/seg/texmaps.npz)
"""
import bpy, bmesh, numpy as np, os, sys

obj_name, size, out = sys.argv[sys.argv.index('--') + 1:][:3]
N = int(size)
ob = bpy.data.objects[obj_name]
bm = bmesh.new()
bm.from_mesh(ob.data)
bmesh.ops.triangulate(bm, faces=bm.faces[:])
uvl = bm.loops.layers.uv.active
tris = [f for f in bm.faces if f.material_index == 0]
# texture islands: triangles joined by shared UV edges
key = lambda l: (round(l[uvl].uv.x, 5), round(l[uvl].uv.y, 5))
parent = list(range(len(tris)))


def find(a):
    while parent[a] != a:
        parent[a] = parent[parent[a]]
        a = parent[a]
    return a


edge_owner = {}
for ti, f in enumerate(tris):
    for l in f.loops:
        e = tuple(sorted((key(l), key(l.link_loop_next))))
        if e in edge_owner:
            parent[find(ti)] = find(edge_owner[e])
        else:
            edge_owner[e] = ti
roots = {}
island_of = [roots.setdefault(find(t), len(roots)) for t in range(len(tris))]

# curvature per point (the umbrella: how far the neighbours' mean lies along the normal, over the squared edge length):
# positive in a crease (where one garment meets what lies under it), negative on a ridge (a garment's raised edge)
bm.verts.ensure_lookup_table()
curv_v = {}
for v in bm.verts:
    nb = [e.other_vert(v) for e in v.link_edges]
    if not nb:
        curv_v[v.index] = 0.0
        continue
    mean = sum((n_.co for n_ in nb), v.co * 0) / len(nb)
    el = sum((n_.co - v.co).length for n_ in nb) / len(nb)
    curv_v[v.index] = (mean - v.co).dot(v.normal) / max(el * el, 1e-8)
pos = np.zeros((N, N, 3), np.float32)
curv = np.zeros((N, N), np.float32)
nrm = np.zeros((N, N, 3), np.float32)
island = np.full((N, N), -1, np.int32)
for ti, f in enumerate(tris):
    ls = f.loops
    uv = np.array([[l[uvl].uv.x * N, (1 - l[uvl].uv.y) * N] for l in ls])    # row 0 = the texture's top
    p3 = np.array([l.vert.co[:] for l in ls])
    n3 = np.array([l.vert.normal[:] for l in ls])
    c3 = np.array([curv_v[l.vert.index] for l in ls])
    x0, y0 = np.floor(uv.min(0)).astype(int)
    x1, y1 = np.ceil(uv.max(0)).astype(int)
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, N), min(y1, N)
    if x1 <= x0 or y1 <= y0:
        continue
    (ax, ay), (bx, by), (cx, cy) = uv
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(d) < 1e-9:
        continue
    ys, xs = np.mgrid[y0:y1, x0:x1] + 0.5
    l1 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / d
    l2 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / d
    l3 = 1 - l1 - l2
    inside = (l1 >= -0.01) & (l2 >= -0.01) & (l3 >= -0.01)
    w = np.stack([l1, l2, l3], -1)[inside]
    yy, xx = (ys[inside] - 0.5).astype(int), (xs[inside] - 0.5).astype(int)
    pos[yy, xx] = w @ p3
    nrm[yy, xx] = w @ n3
    curv[yy, xx] = w @ c3
    island[yy, xx] = island_of[ti]
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
np.savez_compressed(os.path.abspath(out), pos=pos, nrm=nrm, island=island, curv=curv)
print('TEXMAPS %d triangles, %d islands, %.1f%% of the texture used -> %s' % (
    len(tris), len(roots), (island >= 0).mean() * 100, out))
