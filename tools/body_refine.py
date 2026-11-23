"""Blender (blender -b --factory-startup <file.blend> -P tools/body_refine.py -- <object> <bone,bone,..> <out-name>
<unit> [adaptive|full] [mm-per-material,..]): the part of a body weighted to the given bones, subdivided once and
then again only along the features' edges (adaptive: the model), or three times everywhere (full: the source a
normal map is baked from); its surface moved along the normal by a height map in uv space: the modelled features of FEATURES[unit] (raised parts
drawn where the texture paints them) plus the texture's fine detail (luminance minus its blur: light = raised, dark =
sunk, mm per material, damped on the features); saved as object <out-name> into the
same .blend."""
import bpy, bmesh, math, os, sys
from mathutils import Vector

# per unit: (kind, material index, shape, height mm, edge mm)
#   rect: (u0, v0, u1, v1, corner radius in uv): rises over the edge width by a quarter circle to the height
#   dome: (u, v, radius in uv): a cap of the height over the radius
#   uvs read from the texture with tools/body_view.py's -uv pictures (u = x / width, v = 1 - y / height)
FEATURES = {
    'all_1st_trooper': [
        ('rect', 1, (0.662, 0.03, 0.706, 0.99, 0.0), 3.5, 2.0),     # cuff rim, sleeve side
        ('rect', 1, (0.944, 0.03, 0.998, 0.99, 0.0), 3.5, 2.0),     # cuff rim, wrist side
        ('rect', 1, (0.782, 0.232, 0.908, 0.418, 0.05), 2.5, 0.8),  # tab under the stud
        ('dome', 1, (0.848, 0.328, 0.034), 5.0, 0.0),              # brass stud (on the tab)
        ('rect', 1, (0.168, 0.302, 0.434, 0.580, 0.04), 3.0, 1.2),  # sleeve pocket
    ],
}

args = sys.argv[sys.argv.index('--') + 1:]
OBJ, BONES, NAME, UNIT = args[0], args[1].split(','), args[2], args[3]
FULL = len(args) > 4 and args[4] == 'full'
MM = [float(x) for x in args[5].split(',')] if len(args) > 5 else [1.5]
BLUR = 6  # texels of a 256 texture: detail finer than this becomes relief
scene = bpy.context.scene
src = bpy.data.objects[OBJ]


def detail_map(img):
    """Luminance minus its box blur, scaled to -1..1 by the 98th percentile."""
    w, h = img.size
    px = img.pixels[:]
    lum = [0.3 * px[4 * i] + 0.59 * px[4 * i + 1] + 0.11 * px[4 * i + 2] for i in range(w * h)]
    r = max(1, BLUR * w // 256)

    def box(a, stride, n, count):
        out = a[:]
        for line in range(count):
            base = line * (w if stride == 1 else 1)
            s = 0.0
            vals = [a[base + k * stride] for k in range(n)]
            pre = [0.0]
            for v in vals:
                pre.append(pre[-1] + v)
            for k in range(n):
                lo, hi = max(0, k - r), min(n, k + r + 1)
                out[base + k * stride] = (pre[hi] - pre[lo]) / (hi - lo)
        return out

    blur = box(box(lum, 1, w, h), w, h, w)
    d = [a - b for a, b in zip(lum, blur)]
    scale = sorted(abs(x) for x in d)[int(len(d) * 0.98)] or 1.0
    return [max(-1.0, min(1.0, x / scale)) for x in d], w, h


def sample(m, u, v):
    d, w, h = m
    x, y = (u % 1.0) * (w - 1), (v % 1.0) * (h - 1)
    x0, y0 = int(x), int(y)
    x1, y1 = min(x0 + 1, w - 1), min(y0 + 1, h - 1)
    fx, fy = x - x0, y - y0
    a = d[y0 * w + x0] * (1 - fx) + d[y0 * w + x1] * fx
    b = d[y1 * w + x0] * (1 - fx) + d[y1 * w + x1] * fx
    return a * (1 - fy) + b * fy


# the part, as its own object
groups = {src.vertex_groups[b].index for b in BONES if b in src.vertex_groups}
bm = bmesh.new()
bm.from_mesh(src.data)
deform = bm.verts.layers.deform.active
keep = {v.index for v in bm.verts if any(g in groups and w > 0.5 for g, w in v[deform].items())}
bmesh.ops.delete(bm, geom=[f for f in bm.faces if not all(v.index in keep for v in f.verts)], context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
me = bpy.data.meshes.new(NAME)
bm.to_mesh(me)
bm.free()
old = bpy.data.objects.get(NAME)
if old:
    bpy.data.objects.remove(old)
part = bpy.data.objects.new(NAME, me)
scene.collection.objects.link(part)
for m in src.data.materials:
    me.materials.append(m)
for g in src.vertex_groups:
    part.vertex_groups.new(name=g.name)
part.matrix_world = src.matrix_world.copy()
part.parent = src.parent
src_tris = len(me.polygons)

bpy.context.view_layer.objects.active = part
mod = part.modifiers.new('subd', 'SUBSURF')
mod.levels = 3 if FULL else 1
mod.uv_smooth = 'PRESERVE_CORNERS'
bpy.ops.object.modifier_apply(modifier='subd')

def sdf_rect(u, v, u0, v0, u1, v1, r):
    """Distance inside a rounded rectangle (positive inside), uv units."""
    cx, cy, hx, hy = (u0 + u1) / 2, (v0 + v1) / 2, (u1 - u0) / 2 - r, (v1 - v0) / 2 - r
    qx, qy = abs(u - cx) - hx, abs(v - cy) - hy
    outside = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0)
    return r - outside


def m_per_uv(mi, box):
    """Metres per uv unit over the part's faces of material mi whose uv centre lies in box."""
    a3 = auv = 0.0
    for f in me.polygons:
        if f.material_index != mi:
            continue
        uvs = [uvd[li].uv for li in f.loop_indices]
        c = sum(uvs, Vector((0, 0))) / len(uvs)
        if not (box[0] <= c.x <= box[2] and box[1] <= c.y <= box[3]):
            continue
        a3 += f.area
        auv += sum(abs((uvs[k] - uvs[0]).cross(uvs[k + 1] - uvs[0])) / 2 for k in range(1, len(uvs) - 1))
    return math.sqrt(a3 / auv) if auv else 0.25


uvd = me.uv_layers.active.data
features = []
for kind, mi, shape, h, edge in FEATURES.get(UNIT, []):
    box = (shape[0] - shape[2], shape[1] - shape[2], shape[0] + shape[2], shape[1] + shape[2]) if kind == 'dome' else shape[:4]
    scale = m_per_uv(mi, box)
    features.append((kind, mi, shape, h, (edge / 1000) / scale))
    print('FEATURE', kind, shape, 'mm per texel %.2f' % (scale * 1000 / 256))
m_per_uv_mat = {mi: m_per_uv(mi, (0, 0, 1, 1)) for mi in range(len(me.materials))}


def feature_height(mi, u, v):
    """Highest feature at (u, v) in mm (features stack: a dome on a tab adds to it), and 1 where one covers it."""
    base, dome, cover = 0.0, 0.0, 0.0
    for kind, fmi, shape, h, edge in features:
        if fmi != mi:
            continue
        if kind == 'dome':
            d = 1 - math.hypot(u - shape[0], v - shape[1]) / shape[2]
            if d > 0:
                dome = max(dome, h * math.sqrt(1 - (1 - d) ** 2))
                cover = 1.0
        else:
            d = sdf_rect(u, v, *shape)
            if d > 0:
                t = min(d / edge, 1.0) if edge > 0 else 1.0
                base = max(base, h * math.sqrt(1 - (1 - t) ** 2))
                cover = max(cover, t)
    return base + dome, cover




def near_feature(mi, u, v, band):
    """Whether (u, v) lies within band of a feature's rising edge, or on a dome."""
    for kind, fmi, shape, h, edge in features:
        if fmi != mi:
            continue
        if kind == 'dome':
            if math.hypot(u - shape[0], v - shape[1]) < shape[2] + band:
                return True
        elif -band < sdf_rect(u, v, *shape) < edge + band:
            return True
    return False


bm = bmesh.new()
bm.from_mesh(me)
uvl = bm.loops.layers.uv.active
if not FULL:
    # density where the features rise: edges in a band along their outlines, halved three times
    for _ in range(3):
        pick = set()
        for e in bm.edges:
            for loop in e.link_loops:
                m = (loop[uvl].uv + loop.link_loop_next[uvl].uv) / 2
                band = (1.5 / 1000) / max(1e-6, m_per_uv_mat.get(loop.face.material_index, 0.25))
                if near_feature(loop.face.material_index, m.x, m.y, band):
                    pick.add(e)
                    break
        if not pick:
            break
        bmesh.ops.subdivide_edges(bm, edges=list(pick), cuts=1, use_grid_fill=True)
        bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.normal_update()

# relief: per vertex the mean of its corners' samples (uv seams), along the smooth normal
maps = {}
for mi, mat in enumerate(me.materials):
    img = next((n.image for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image), None)
    if img and MM[min(mi, len(MM) - 1)]:
        maps[mi] = detail_map(img)
moves = []
for v in bm.verts:
    s, n = 0.0, 0
    for loop in v.link_loops:
        mi = loop.face.material_index
        u = loop[uvl].uv
        fh, cover = feature_height(mi, u.x, u.y)
        s += fh + (sample(maps[mi], u.x, u.y) * MM[min(mi, len(MM) - 1)] * (1 - 0.85 * cover) if mi in maps else 0)
        n += 1
    moves.append(v.normal * (s / n / 1000.0) if n else Vector())
for v, d in zip(bm.verts, moves):
    v.co += d
bmesh.ops.triangulate(bm, faces=bm.faces[:])
bm.to_mesh(me)
bm.free()
for p in me.polygons:
    p.use_smooth = True
a = part.modifiers.new('skeleton', 'ARMATURE')
a.object = src.parent
print('REFINED', NAME, 'full' if FULL else 'adaptive', 'from', src_tris, 'tris to', len(me.polygons), 'verts',
      len(me.vertices), 'relief mm', MM)
bpy.ops.wm.save_mainfile()
