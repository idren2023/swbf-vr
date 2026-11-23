"""Blender (blender -b --factory-startup re/body/r_hand_retopo.blend -P tools/body_transfer.py -- <weights.blend> <out.blend>):
the retopology hand (r_hand_retopo) takes its texture and its weights from the approved stock-clone hand, so the
game's own texture is used unchanged (every class sharing the stock glove keeps working by swapping the image):

- uv: from the stock clone in this file (r_hand_stock, the game's uv layout), by tools/body_uvlib.py.
- weights: from r_hand_stock in <weights.blend> (the split hand, weights set by hand against idren2023's joints): each
  vertex the barycentric blend of the weights at the nearest point of its surface (both meshes lie on the same rest).
"""
import bpy, bmesh, os, sys
from mathutils import Vector, geometry
from mathutils.bvhtree import BVHTree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from body_uvlib import tri_bm, transfer_uv

args = sys.argv[sys.argv.index('--') + 1:]
WSRC, OUT = args[0], args[1]
ob = bpy.data.objects['r_hand_retopo']
stock = bpy.data.objects['r_hand_stock']
me = ob.data


# ---- uv -------------------------------------------------------------------------------------------------------------
seams, clamped = transfer_uv(me, stock.data)
if not me.materials:
    for m in stock.data.materials:
        me.materials.append(m)
print('TRANSFER uv: %d stock seam edges; %d of %d corners clamped to a seam' % (seams, clamped, len(me.loops)))

# ---- weights --------------------------------------------------------------------------------------------------------
with bpy.data.libraries.load(os.path.abspath(WSRC)) as (src, dst):
    dst.objects = ['r_hand_stock']
wsrc = dst.objects[0]
wb = tri_bm(wsrc.data)
dl = wb.verts.layers.deform.active
gname = {g.index: g.name for g in wsrc.vertex_groups}
wtree = BVHTree.FromBMesh(wb)
off = sorted((wtree.find_nearest(v.co)[3]) for v in me.vertices)
for g in list(ob.vertex_groups):
    ob.vertex_groups.remove(g)
groups = {n: ob.vertex_groups.new(name=n) for n in gname.values()}
for v in me.vertices:
    loc, _, fi, _ = wtree.find_nearest(v.co)
    f = wb.faces[fi]
    a, b, c = (x.co for x in f.verts)
    # barycentric weights of loc in the triangle
    bc = geometry.barycentric_transform(loc, a, b, c, Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1)))
    acc = {}
    for x, t in zip(f.verts, bc):
        for gi, w in x[dl].items():
            acc[gname[gi]] = acc.get(gname[gi], 0.0) + w * max(t, 0.0)
    acc = {n: w for n, w in acc.items() if w > 0.01}
    tot = sum(acc.values())
    for n, w in acc.items():
        groups[n].add([v.index], w / tot, 'REPLACE')
bpy.data.objects.remove(wsrc)

# ---- fingers by rule (idren2023: the transferred weights curled the fingers like noodles; best practice): each finger
# segment rigid on its bone, a narrow blend across each joint (smoothstep over +-JOINT_H of the plane halving the
# joint's angle: about one edge loop either side), the finger's base from the hand over KNUCKLE (mm before / after its
# knuckle's plane); the thumb likewise past its base; the hand / thumb base share (the thenar) kept from the transfer
rig = ob.parent
Bn = {b.name: b for b in rig.data.bones}
JOINT_H = 0.003
KNUCKLE = (0.006, 0.004)
THUMB_H = (0.004, 0.003)   # the thumb's knuckle (thumb_2), its tip joint (thumb_3)


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def dirn(n):
    return (Bn[n].tail_local - Bn[n].head_local).normalized()


def plane_d(p, joint_bone, prev_dir):
    """Signed distance of p past the plane through joint_bone's head halving the angle to the bone before it."""
    nrm = (prev_dir + dirn(joint_bone)).normalized()
    return (p - Bn[joint_bone].head_local).dot(nrm)


def seg_dist(p, names):
    best = 1e9
    for n in names:
        h, t = Bn[n].head_local, Bn[n].tail_local
        q, f = geometry.intersect_point_line(p, h, t)
        q = h.lerp(t, min(max(f, 0.0), 1.0))
        best = min(best, (p - q).length)
    return best


def chain_weights(p, bones, base_dir, h0, h1, hj):
    """Within one digit: the share of the digit (0 before its base band) and its bones' weights (summing to it)."""
    share = smooth((plane_d(p, bones[0], base_dir) + h0) / (h0 + h1))
    ws, rest = {}, share
    for k in range(1, len(bones)):
        hk = hj[k - 1] if isinstance(hj, tuple) else hj
        past = smooth((plane_d(p, bones[k], dirn(bones[k - 1])) + hk) / (2 * hk))
        ws[bones[k - 1]] = rest * (1 - past)
        rest *= past
    ws[bones[-1]] = rest
    return share, ws


hand_dir = dirn('hand')
finger_chains = {f: [f + '_1', f + '_2', f + '_3'] for f in ('index', 'middle', 'ring', 'pinky')}
for v in me.vertices:
    old = {ob.vertex_groups[g.group].name: g.weight for g in v.groups}
    thumb = sum(w for n, w in old.items() if n.startswith('thumb'))
    p = v.co
    new = {}
    # the thumb: its share from the transfer; inside it thumb_1 / thumb_2 / thumb_3 by the rule past the thumb's knuckle
    if thumb > 0:
        _, tw = chain_weights(p, ['thumb_1', 'thumb_2', 'thumb_3'], hand_dir, 1.0, 1.0, THUMB_H)
        tot = sum(tw.values())
        for n, w in tw.items():
            new[n] = thumb * w / tot
    # the four fingers: each finger's share by its knuckle band, split between neighbours by closeness to their bones
    # which finger: from the hand-set weights (a vertex's own finger; the webs shared as set by hand), by closeness
    # to the fingers' bones only where they give none (closeness alone gave the index's inner side ~30 % of the
    # middle finger: idren2023, the index wobbled in the point)
    inv = {f: sum(w for n, w in old.items() if n.startswith(f)) for f in finger_chains}
    if sum(inv.values()) < 1e-3:
        inv = {f: 1 / max(seg_dist(p, c), 1e-4) ** 4 for f, c in finger_chains.items()}
    isum = sum(inv.values())
    fsum = 0.0
    for f, c in finger_chains.items():
        share, fw = chain_weights(p, c, hand_dir, KNUCKLE[0], KNUCKLE[1], JOINT_H)
        k = inv[f] / isum * (1 - thumb)
        for n, w in fw.items():
            new[n] = new.get(n, 0.0) + w * k
        fsum += share * k
    new['hand'] = max(0.0, 1.0 - thumb - fsum)
    for g in list(v.groups):
        ob.vertex_groups[g.group].remove([v.index])
    tot = sum(new.values())
    for n, w in new.items():
        if w / tot > 0.005:
            groups[n].add([v.index], w / tot, 'REPLACE')
print('TRANSFER fingers by rule: joint band +-%.0f mm, knuckle -%.0f / +%.0f mm' % (JOINT_H * 1000, KNUCKLE[0] * 1000,
                                                                                 KNUCKLE[1] * 1000))
print('TRANSFER weights from %s: retopo off its surface median %.2f mm, max %.2f mm' % (
    WSRC, off[len(off) // 2] * 1000, off[-1] * 1000))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
