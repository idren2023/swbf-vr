"""Blender (blender -b --factory-startup <file.blend> -P tools/body_islands.py -- <object> <bone,..> <material> <out.json>
[texture]):
the uv islands of a stock model's part (faces touching the bones, of one material): per island its faces, uv bounds,
boundary loops in uv, and for each boundary edge the island it is sewn to (the same two 3D vertices on another
island's boundary). The islands are the original artist's panels; the json feeds the builder that lays a new mesh
out on them."""
import bpy, bmesh, json, sys

args = sys.argv[sys.argv.index('--') + 1:]
obj, bones, MAT, OUT = bpy.data.objects[args[0]], args[1].split(','), int(args[2]), args[3]
bm = bmesh.new()
bm.from_mesh(obj.data)
deform = bm.verts.layers.deform.active
uvl = bm.loops.layers.uv.active
groups = {obj.vertex_groups[b].index for b in bones}
faces = [f for f in bm.faces if f.material_index == MAT and
         any(any(g in groups and w > 0.5 for g, w in v[deform].items()) for v in f.verts)]
fset = set(faces)


def key(l):
    return (round(l[uvl].uv.x, 5), round(l[uvl].uv.y, 5))


# islands: faces joined across an edge when both sides give that edge the same uvs
parent = {f: f for f in faces}


def find(f):
    while parent[f] is not f:
        parent[f] = parent[parent[f]]
        f = parent[f]
    return f


for f in faces:
    for l in f.loops:
        e = l.edge
        for l2 in e.link_loops:
            g = l2.face
            if g is f or g not in fset:
                continue
            # l runs a->b, l2 runs b->a on a manifold edge
            if key(l) == key(l2.link_loop_next) and key(l.link_loop_next) == key(l2):
                parent[find(f)] = find(g)
islands = {}
for f in faces:
    islands.setdefault(find(f), []).append(f)
ids = {root: i for i, root in enumerate(sorted(islands, key=lambda r: -len(islands[r])))}
island_of = {f: ids[find(f)] for f in faces}

out = []
for root, fs in sorted(islands.items(), key=lambda kv: ids[kv[0]]):
    i = ids[root]
    # boundary half-edges: uv edges not shared with a face of the same island
    bnd = []
    for f in fs:
        for l in f.loops:
            a, b = key(l), key(l.link_loop_next)
            twin = [l2 for l2 in l.edge.link_loops if l2.face is not f and l2.face in fset and island_of[l2.face] == i
                    and key(l2) == b and key(l2.link_loop_next) == a]
            if twin:
                continue
            other = [island_of[l2.face] for l2 in l.edge.link_loops if l2.face is not f and l2.face in fset]
            bnd.append((a, b, other[0] if other else None, [round(c, 4) for c in l.vert.co],
                        [round(c, 4) for c in l.link_loop_next.vert.co]))
    # chain into loops
    nxt = {}
    for e in bnd:
        nxt.setdefault(e[0], []).append(e)
    loops, used = [], set()
    for e in bnd:
        if id(e) in used:
            continue
        loop, cur = [], e
        while cur and id(cur) not in used:
            used.add(id(cur))
            loop.append({'uv': cur[0], 'sewn_to': cur[2], 'co': cur[3]})
            cand = [c for c in nxt.get(cur[1], []) if id(c) not in used]
            cur = cand[0] if cand else None
        loops.append(loop)
    us = [l[uvl].uv.x for f in fs for l in f.loops]
    vs = [l[uvl].uv.y for f in fs for l in f.loops]
    sewn = {}
    for lp in loops:
        for p in lp:
            sewn[p['sewn_to']] = sewn.get(p['sewn_to'], 0) + 1
    print('ISLAND %d faces %d u %.3f-%.3f v %.3f-%.3f loops %s sewn to %s' % (
        i, len(fs), min(us), max(us), min(vs), max(vs), [len(lp) for lp in loops], sewn))
    out.append({'faces': len(fs), 'bounds': [min(us), min(vs), max(us), max(vs)], 'loops': loops})
json.dump(out, open(OUT, 'w'))

# picture: the texture 4x, island outlines cyan, each sewn pair joined by a yellow line between the edges' middles
if len(args) > 4:
    import os
    img = bpy.data.images.load(args[4])
    w, h = img.size
    k = 4
    px = img.pixels[:]
    cw, ch = w * k, h * k
    pic = [0.0] * (cw * ch * 4)
    for y in range(ch):
        for x in range(cw):
            s = ((y // k) * w + x // k) * 4
            o = (y * cw + x) * 4
            pic[o:o + 4] = (px[s] * 0.6, px[s + 1] * 0.6, px[s + 2] * 0.6, 1.0)

    def line(a, b, col):
        n = int(max(abs(b[0] - a[0]) * cw, abs(b[1] - a[1]) * ch)) + 1
        for t in range(n + 1):
            x = int((a[0] + (b[0] - a[0]) * t / n) * cw)
            y = int((a[1] + (b[1] - a[1]) * t / n) * ch)
            if 0 <= x < cw and 0 <= y < ch:
                pic[(y * cw + x) * 4:(y * cw + x) * 4 + 3] = col

    edges = {}
    for isl in out:
        for lp in isl['loops']:
            for p, q in zip(lp, lp[1:] + lp[:1]):
                line(p['uv'], q['uv'], (0.1, 0.95, 1.0))
                k3 = tuple(sorted((tuple(p['co']), tuple(q['co']))))
                mid = ((p['uv'][0] + q['uv'][0]) / 2, (p['uv'][1] + q['uv'][1]) / 2)
                edges.setdefault(k3, []).append(mid)
    pairs = [m for m in edges.values() if len(m) == 2]
    for a, b in pairs:
        line(a, b, (1.0, 0.9, 0.1))
    print('SEWN PAIRS', len(pairs), 'unpaired', sum(1 for m in edges.values() if len(m) != 2))
    im = bpy.data.images.new('sew', cw, ch)
    im.pixels = pic
    im.filepath_raw = os.path.abspath(os.path.splitext(OUT)[0] + '.png')
    im.file_format = 'PNG'
    im.save()
