"""The detailed model of a stock character, for baking only (never drawn in the game; idren2023 2026-10-26): the stock body
subdivided and given the folds of its paint's shading, then each garment a real shell over it - cut out by its fabric
map, the cut edge relaxed along the surface (so it follows the outline, not the faces), lifted, given thickness with
a rim, rounded by one more subdivision. Overlaps do not matter: only what shows from outside is baked.
Run: blender -b --factory-startup <stock.blend> -P tools/char_highpoly.py -- <config.json> <object> <height_base.png> <out.blend>
"""
import bpy, bmesh, json, os, sys
import numpy as np
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree

cfg_path, obj_name, base_h, out = sys.argv[sys.argv.index('--') + 1:][:4]
cfg = json.load(open(cfg_path))
hp = cfg['highpoly']
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


# ---- the base: the body's faces (main material, body islands), welded, subdivided, folds displaced ----------------
bm = bmesh.new()
bm.from_mesh(stock.data)
uvl = bm.loops.layers.uv.active
drop = []
for f in bm.faces:
    c = sum((l[uvl].uv for l in f.loops), Vector((0, 0))) / len(f.loops)
    if f.material_index != 0 or island_map[texel(c)] not in hp['body_islands']:
        drop.append(f)
bmesh.ops.delete(bm, geom=drop, context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=0.0002)
# the openings closed as on the game mesh (the neck: the head is off), so the bake finds a surface there too
bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=0)
bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
me = bpy.data.meshes.new('hp_base')
bm.to_mesh(me)
base = bpy.data.objects.new('hp_base', me)
sc.collection.objects.link(base)
base.data.materials.append(stock.data.materials[0])
sub = base.modifiers.new('sub', 'SUBSURF')
sub.levels = sub.render_levels = hp['subd']
sub.uv_smooth = 'PRESERVE_BOUNDARIES'
himg = bpy.data.images.load(os.path.abspath(base_h))
himg.colorspace_settings.name = 'Non-Color'
rng = json.load(open(base_h.rsplit('.', 1)[0] + '.json'))
tex = bpy.data.textures.new('hb', 'IMAGE')
tex.image = himg
tex.extension = 'EXTEND'
d = base.modifiers.new('disp', 'DISPLACE')
d.texture = tex
d.texture_coords = 'UV'
span = (rng['max_mm'] - rng['min_mm']) / 1000
d.strength = span
d.mid_level = -rng['min_mm'] / 1000 / span
for p in base.data.polygons:
    p.use_smooth = True
bpy.context.view_layer.objects.active = base
for o in sc.objects:
    o.select_set(o is base)
bpy.ops.object.modifier_apply(modifier='sub')
bpy.ops.object.modifier_apply(modifier='disp')
print('HIGHPOLY base: %d faces' % len(base.data.polygons))
bvh = BVHTree.FromObject(base, bpy.context.evaluated_depsgraph_get())

# ---- the garment shells ------------------------------------------------------------------------------------------


def load_mask(path):
    im = bpy.data.images.load(os.path.abspath(path))
    W, H = im.size
    a = np.array(im.pixels[:], np.float32).reshape(H, W, 4)[::-1, :, 0] > 0.5     # row 0 = the texture's top
    bpy.data.images.remove(im)
    return a


uv_b = base.data.uv_layers.active.data
shells = []
for sh in hp['shells']:
    mask = load_mask(sh['mask'])
    if 'rows' in sh:
        keep = np.zeros_like(mask)
        keep[sh['rows'][0]:sh['rows'][1]] = True
        mask &= keep
    sb = bmesh.new()
    sb.from_mesh(base.data)
    suv = sb.loops.layers.uv.active
    out_f = []
    for f in sb.faces:
        c = sum((l[suv].uv for l in f.loops), Vector((0, 0))) / len(f.loops)
        if not mask[texel(c)]:
            out_f.append(f)
    bmesh.ops.delete(sb, geom=out_f, context='FACES')
    bmesh.ops.delete(sb, geom=[v for v in sb.verts if not v.link_faces], context='VERTS')
    # specks (pieces of under 40 faces) dropped
    seen = set()
    small = []
    for f in sb.faces:
        if f in seen:
            continue
        comp, stack = [], [f]
        seen.add(f)
        while stack:
            g = stack.pop()
            comp.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h not in seen:
                        seen.add(h)
                        stack.append(h)
        if len(comp) < 40:
            small += comp
    bmesh.ops.delete(sb, geom=small, context='FACES')
    bmesh.ops.delete(sb, geom=[v for v in sb.verts if not v.link_faces], context='VERTS')
    # pinch points (a point where the edge meets itself: more than two edge sides) and their faces dropped: offset and
    # thickness explode there (long spikes)
    for _ in range(3):
        pinch = [v for v in sb.verts if sum(1 for e in v.link_edges if e.is_boundary) > 2 or
                 (v.link_edges and all(e.is_boundary for e in v.link_edges))]
        if not pinch:
            break
        bmesh.ops.delete(sb, geom=list({f for v in pinch for f in v.link_faces}), context='FACES')
        bmesh.ops.delete(sb, geom=[v for v in sb.verts if not v.link_faces], context='VERTS')
    # the cut edge relaxed along the surface: each edge point to the mean of its two edge neighbours, back onto the base
    for _ in range(hp['edge_relax']):
        moves = []
        for v in sb.verts:
            be = [e for e in v.link_edges if e.is_boundary]
            if len(be) != 2:
                continue
            mean = (be[0].other_vert(v).co + be[1].other_vert(v).co) / 2
            moves.append((v, bvh.find_nearest(v.co.lerp(mean, 0.6))[0]))
        for v, p_ in moves:
            v.co = p_
        # the points next to the edge follow half way (no thin slivers along it)
        for v in sb.verts:
            if any(e.is_boundary for e in v.link_edges):
                continue
            nb = [e.other_vert(v) for e in v.link_edges]
            if any(any(e.is_boundary for e in n_.link_edges) for n_ in nb):
                v.co = bvh.find_nearest(v.co.lerp(sum((n_.co for n_ in nb), Vector()) / len(nb), 0.5))[0]
    sb.normal_update()
    # a shell can go under another and come out over it (idren2023 2026-10-26: the jacket and its hood one piece, beneath
    # the vest at the front, above it from the clavicles up): its offset and thickness blend by height on the body
    ov = sh.get('over')
    sb.verts.index_update()
    w_over = {}
    for v in sb.verts:
        w = 0.0
        if ov:
            t = (v.co.z - ov['z'][0]) / (ov['z'][1] - ov['z'][0])
            t = min(max(t, 0.0), 1.0)
            w = t * t * (3 - 2 * t)
        w_over[v.index] = w
        off = sh['offset'] + w * (ov['offset'] - sh['offset']) if ov else sh['offset']
        v.co += v.normal * off / 1000
    sm = bpy.data.meshes.new('hp_' + sh['name'])
    sb.to_mesh(sm)
    so = bpy.data.objects.new('hp_' + sh['name'], sm)
    sc.collection.objects.link(so)
    so.data.materials.append(stock.data.materials[0])
    sol = so.modifiers.new('thick', 'SOLIDIFY')
    sol.thickness = sh['thick'] / 1000
    sol.offset = 1.0
    sol.use_rim = True
    if ov:
        g = so.vertex_groups.new(name='over')
        for i_, w in w_over.items():
            g.add([i_], w, 'REPLACE')
        sol.vertex_group = 'over'
        sol.thickness_vertex_group = ov['thick_factor_under']
    sol.use_even_offset = False
    sol.thickness_clamp = 0.0
    rnd = so.modifiers.new('round', 'SUBSURF')
    rnd.levels = rnd.render_levels = 1
    for p in so.data.polygons:
        p.use_smooth = True
    shells.append(so)
    print('HIGHPOLY shell %-6s %d faces' % (sh['name'], len(sm.polygons)))
for o in list(sc.objects):
    if o not in shells and o is not base and o.type == 'MESH':
        bpy.data.objects.remove(o)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(out))
print('HIGHPOLY -> %s' % out)
