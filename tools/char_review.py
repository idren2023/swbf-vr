"""Review of a built character body, seam by seam (step 5 of the per-character pipeline): every border between two
pieces is found, close-ups are framed along it, and each is rendered four ways: pieces with edges, textured, textured
with edges, the stock model from the same camera. One sheet per pair of pieces. Also numbers per border: how much it
turns at each point (a smooth border turns little; a staircase turns 45-90 degrees at every corner).
Run: blender -b --factory-startup <built.blend> -P tools/char_review.py -- <stock.blend> <stock object> <out dir>
     (rebel soldier: re/body/r_body_garments.blend ... re/body/all_inf_soldier.blend all_inf_soldier_hi re/body/review)
"""
import bpy, bmesh, math, os, sys
import numpy as np
from mathutils import Vector, Matrix
from mathutils.kdtree import KDTree

stock_blend, stock_name, out = sys.argv[sys.argv.index('--') + 1:][:3]
out = os.path.abspath(out)
os.makedirs(out, exist_ok=True)
SPACING, PER_PAIR, FRAME, PX = 0.09, 6, 0.13, 384
sc = bpy.context.scene
pieces_ob = bpy.data.objects['r_body_pieces']
tex_ob = bpy.data.objects['r_body_quads']
with bpy.data.libraries.load(os.path.abspath(stock_blend)) as (src, dst):
    dst.objects = [stock_name]
stock = dst.objects[0]
sc.collection.objects.link(stock)
stock.parent = None
stock.matrix_world = Matrix.Identity(4)
for m in stock.modifiers:
    m.show_render = m.type == 'SUBSURF'

# ---- the borders: boundary points of each piece, paired with the nearest point of another piece ------------------
me = pieces_ob.data
mats = [m.name.replace('piece_', '') for m in me.materials]
bm = bmesh.new()
bm.from_mesh(me)
bm.verts.ensure_lookup_table()
vmat = {}
for f in bm.faces:
    for v in f.verts:
        vmat[v.index] = f.material_index
kd = KDTree(len(bm.verts))
for v in bm.verts:
    kd.insert(v.co, v.index)
kd.balance()
pairs = {}
turns = {}
# the border a viewer sees: the fold of a turned-under edge where the piece has one (crease_edge), else its open edge
_cr = bm.edges.layers.float.get('crease_edge')
for v in bm.verts:
    be = [e for e in v.link_edges if (e[_cr] > 0.5 if _cr is not None else False)] or          [e for e in v.link_edges if e.is_boundary]
    if len(be) != 2:
        continue
    a = vmat[v.index]
    other = next((i for co, i, d in kd.find_n(v.co, 12) if vmat.get(i, a) != a and d < 0.012), None)
    if other is None:
        continue
    key = tuple(sorted((mats[a], mats[vmat[other]])))
    n = v.normal.copy()
    pairs.setdefault(key, []).append((v.co.copy(), n))
    d1 = (be[0].other_vert(v).co - v.co).normalized()
    d2 = (v.co - be[1].other_vert(v).co).normalized()
    turns.setdefault(key, []).append(math.degrees(d1.angle(d2, 0.0)))
print('REVIEW border turning per pair (degrees at each point: median, 90th percentile, share over 30)')
for key in sorted(turns):
    t = np.array(turns[key])
    print('REVIEW %-18s points %5d  median %5.1f  p90 %5.1f  over30 %4.1f%%' % (
        '%s/%s' % key, len(t), np.median(t), np.percentile(t, 90), (t > 30).mean() * 100))

# ---- close-ups ------------------------------------------------------------------------------------------------
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'
sc.display.shading.show_cavity = True
sc.render.resolution_x = sc.render.resolution_y = PX
sc.render.film_transparent = False
cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.type = 'ORTHO'
cam.data.ortho_scale = FRAME
# the edges: a wireframe copy of the built mesh, rendered alone on transparent and laid over
wire = bpy.data.objects.new('wire', tex_ob.data.copy())
sc.collection.objects.link(wire)
wire.matrix_world = tex_ob.matrix_world
wm = wire.modifiers.new('w', 'WIREFRAME')
wm.thickness = 0.0005
wm.use_replace = True
wm.offset = 1.0
black = bpy.data.materials.new('black')
black.diffuse_color = (0, 0, 0, 1)
wire.data.materials.clear()
wire.data.materials.append(black)
packs = [o for o in sc.objects if o.name.startswith('r_body_packs')]


def render(objs, ctype, path, transparent=False):
    for o in sc.objects:
        if o.type == 'MESH':
            o.hide_render = o not in objs
    sc.display.shading.color_type = ctype
    sc.render.film_transparent = transparent
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    a = np.array(img.pixels[:], np.float32).reshape(PX, PX, 4)[::-1].copy()
    bpy.data.images.remove(img)
    return a


tmp = os.path.join(out, '_tmp.png')
for key, pts in sorted(pairs.items()):
    # samples spread along the border: farthest-point picks at least SPACING apart
    chosen = [pts[0]]
    while len(chosen) < PER_PAIR:
        far = max(pts, key=lambda p: min((p[0] - c[0]).length for c in chosen))
        if min((far[0] - c[0]).length for c in chosen) < SPACING:
            break
        chosen.append(far)
    rows = []
    for co, n in chosen:
        d = n.normalized() if n.length > 0.1 else Vector((0, -1, 0))
        cam.location = co + d * 1.0
        cam.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()
        p_ = render([pieces_ob] + packs, 'MATERIAL', tmp)
        t_ = render([tex_ob] + packs, 'TEXTURE', tmp)
        w_ = render([wire], 'MATERIAL', tmp, transparent=True)
        s_ = render([stock], 'TEXTURE', tmp)
        al = w_[..., 3:4]
        rows.append(np.concatenate([p_ * (1 - al) + w_ * al, t_, t_ * (1 - al) + w_ * al, s_], 1))
    sheet = np.concatenate(rows, 0)
    h, w = sheet.shape[:2]
    im = bpy.data.images.new('sheet', w, h)
    im.pixels[:] = np.ascontiguousarray(sheet[::-1]).ravel()
    im.filepath_raw = os.path.join(out, 'seam-%s-%s.png' % key)
    im.file_format = 'PNG'
    im.save()
    print('REVIEW sheet %s/%s: %d close-ups' % (key[0], key[1], len(rows)))
os.remove(tmp)
