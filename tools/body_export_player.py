"""Blender (blender -b re/body/player_stock.blend -P tools/body_export_player.py -- <out.inc> [arm=<BodyArm.inc>]
[size=<texture side>]): writes the assembled player (tools/body_assemble.py: the stock body, our arms and hands, on the
soldier's skeleton) for the DLL as a C++ include (generated, never in git; tools/build.sh copies re/body/PlayerBody.inc
into build/gen when it exists). PLAN Next session (idren2023 2026-10-26): one body in the game, every bone ours.

- The whole mesh (the body, our arms and hands, the packs), positions and normals in the msh's model space (y up, +z
  forward, the model's left +x), up to four bone weights a vertex.
- Bones: every bone the mesh is weighted to, parents first (each one's parent the nearest weighted ancestor; the rig's
  root_ / eff_ helpers and the stock forearms / hands, which nothing is weighted to, are skipped), with the rest frame
  (bone -> model, row vectors; Blender's bone axes, y along the bone) and its inverse. The DLL poses them all itself.
- The rest eye (as tools/body_reach_test.py's EYE) and the map from BodyArm's arm space (BodyArm.inc: the right hand's
  rig, the wrist at the origin) to our model space for the right arm (a similarity fitted over the forearm's, hand's and
  fingers' heads; the left arm is its mirror across x), so the DLL can take the hands and fingers from BodyArm's poses.
- Parts per material: the body (our 4x texture, scaled to size, A8R8G8B8 rows from the top), the arms' material (our
  arms texture) if any, the packs (the game's All_Inf_Trooper_packs by name)."""
import bpy, os, re, sys
import numpy as np
from mathutils import Vector

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
OPT = dict(a.split('=', 1) for a in args[1:] if '=' in a)
ARM_INC = OPT.get('arm', 'build/gen/BodyArm.inc')
SIZE = int(OPT.get('size', 1024))
GAME_PACKS = 'All_Inf_Trooper_packs'
EYE_BLENDER = Vector((0.0, -0.11, 1.875))    # between the eyes (tools/body_reach_test.py): the model is a 2.00 m man

pl = bpy.data.objects['player']
pk = bpy.data.objects['r_body_packs']
rig = pl.parent
C = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], float)    # Blender -> msh (x, z, -y)


def to_msh(v):
    return (v[0], v[2], -v[1])


weighted = set()
for o in (pl, pk):
    weighted |= {g.name for g in o.vertex_groups}
bones = [b for b in rig.data.bones if b.name in weighted]     # rig order: parents before children
names = [b.name for b in bones]


def parent_of(b):
    p = b.parent
    while p and p.name not in weighted:
        p = p.parent
    return names.index(p.name) if p else -1


rest = []
for b in bones:
    m = C @ np.array(rig.matrix_world @ b.matrix_local)      # column vectors, bone -> msh
    rest.append(m.T)                                            # row vectors
print('EXPORT %d bones: %s' % (len(names), names))

# BodyArm's arm space -> our model space (rows), fitted over the right arm's heads
arm_text = open(ARM_INC).read()
arm_rest = {}
for m in re.finditer(r'\{"(\w+)", (-?\d+), \{([^}]*)\}, "(\w+)"', arm_text):
    vals = [float(x.rstrip('f')) for x in m.group(3).split(',')]
    arm_rest[m.group(1)] = np.array(vals[12:15])                # column storage: the translation
pairs = [(n, 'r_' + n) for n in arm_rest if 'r_' + n in names and not n.endswith('twist')]
src = np.array([arm_rest[a] for a, _ in pairs])
dst = np.array([rest[names.index(o)][3, :3] for _, o in pairs])
mu_s, mu_d = src.mean(0), dst.mean(0)
U, S, Vt = np.linalg.svd((dst - mu_d).T @ (src - mu_s))
D = np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))])
R = U @ D @ Vt                                      # dst = k R src + t (column vectors)
k = (S * np.diag(D)).sum() / ((src - mu_s) ** 2).sum()
t = mu_d - k * R @ mu_s
fit = np.abs((k * (R @ src.T)).T + t - dst).max()
arm_to_model = np.eye(4)
arm_to_model[:3, :3] = (k * R).T                    # row vectors
arm_to_model[3, :3] = t
print('EXPORT arm space -> model: scale %.4f, %d heads, worst %.2f mm (%s)' % (
    k, len(pairs), fit * 1000, ', '.join(a for a, _ in pairs)))

textures = []       # (key, width, height, pixels)


def own_texture(img):
    for i, (key, *_r) in enumerate(textures):
        if key == img.name:
            return i
    im = img.copy()
    if im.size[0] > SIZE or im.size[1] > SIZE:
        im.scale(min(SIZE, im.size[0]), min(SIZE, im.size[1]))
    w, h = im.size
    px = np.array(im.pixels[:], np.float32).reshape(h, w, 4)[::-1]       # rows from the top
    px = np.clip(np.round(px * 255), 0, 255).astype(np.uint32)
    argb = (px[..., 3] << 24) | (px[..., 0] << 16) | (px[..., 1] << 8) | px[..., 2]
    textures.append((img.name, w, h, argb.ravel()))
    return len(textures) - 1


parts = []          # (label, own texture index or -1, game texture name, vertices, indices)


def export_object(obj, texture_of):
    me = obj.data
    me.calc_loop_triangles()
    gnames = [g.name for g in obj.vertex_groups]
    vw = []
    for v in me.vertices:
        ws = {}
        for g in v.groups:
            if g.weight > 0:
                ws[gnames[g.group]] = ws.get(gnames[g.group], 0) + g.weight
        top = sorted(ws.items(), key=lambda x: -x[1])[:4]
        s = sum(w for _, w in top) or 1
        vw.append([(names.index(n), w / s) for n, w in top])
    by_mat = {}
    for tri in me.loop_triangles:
        by_mat.setdefault(tri.material_index, []).append(tri)
    uvl = me.uv_layers.active.data
    to_rig = rig.matrix_world.inverted() @ obj.matrix_world
    for mi, tris in sorted(by_mat.items()):
        own, game_name = texture_of(mi)
        verts, index_of, idx = [], {}, []
        for tri in tris:
            for c in range(3):
                li, vi = tri.loops[c], tri.vertices[c]
                uv = uvl[li].uv
                nrm = Vector(tri.split_normals[c])
                key = (vi, round(uv.x, 6), round(uv.y, 6), round(nrm.x, 3), round(nrm.y, 3), round(nrm.z, 3))
                if key not in index_of:
                    index_of[key] = len(verts)
                    p = rig.matrix_world @ to_rig @ me.vertices[vi].co
                    n = (obj.matrix_world.to_3x3() @ nrm).normalized()
                    verts.append((to_msh(p), to_msh(n), (uv.x, 1 - uv.y), vw[vi]))
                idx.append(index_of[key])
        assert len(verts) < 65536, 'part %s: %d vertices' % (obj.name, len(verts))
        parts.append(('%s_%d' % (obj.name, mi), own, game_name, verts, idx))
        print('EXPORT part %s material %d: %d vertices, %d triangles' % (obj.name, mi, len(verts), len(idx) // 3))


def player_texture(mi):
    m = pl.data.materials[mi]
    img = next(n.image for n in m.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image)
    return own_texture(img), ''


export_object(pl, player_texture)
export_object(pk, lambda mi: (-1, GAME_PACKS))


def f(x):
    s = '%.6g' % x
    return s + ('f' if ('.' in s or 'e' in s) else '.f')


def floats(a):
    return ', '.join(f(x) for x in np.ravel(a))


out = ['// generated by tools/body_export_player.py from re/body/player_stock.blend: do not edit, never commit',
       '// model space: the msh\'s (y up, +z forward, the model\'s left +x); matrices row vectors']
out.append('constexpr PlayerBodyBone PLAYER_BODY_BONES[] = {  // name, parent, rest (bone -> model), its inverse')
for name, m in zip(names, rest):
    out.append('    {"%s", %d, {%s}, {%s}},' % (name, parent_of(bones[names.index(name)]), floats(m),
                                                floats(np.linalg.inv(m))))
out.append('};')
out.append('constexpr float PLAYER_BODY_EYE[3] = {%s};  // the eyes at rest' % floats(to_msh(EYE_BLENDER)))
out.append('constexpr float PLAYER_BODY_ARM_TO_MODEL[16] = {%s};  // BodyArm arm space -> model, right arm' %
           floats(arm_to_model))
for i, (key, w, h, px) in enumerate(textures):
    out.append('// texture %d: %s' % (i, key))
    out.append('constexpr unsigned PLAYER_BODY_TEXELS_%d[] = {' % i)
    for r in range(0, len(px), 16):
        out.append(','.join('0x%x' % x for x in px[r:r + 16]) + ',')
    out.append('};')
out.append('constexpr PlayerBodyTexture PLAYER_BODY_TEXTURES[] = {')
for i, (key, w, h, px) in enumerate(textures):
    out.append('    {%d, %d, PLAYER_BODY_TEXELS_%d},' % (w, h, i))
out.append('};')
for p, (label, own, game_name, verts, idx) in enumerate(parts):
    out.append('constexpr PlayerBodyVertex PLAYER_BODY_VERTICES_%d[] = {' % p)
    for pos, nrm, uv, ws in verts:
        ws = ws + [(0, 0.0)] * (4 - len(ws))
        out.append('    {{%s}, {%s}, {%s}, {%s}, {%s}},' % (
            floats(pos), floats(nrm), floats(uv), ', '.join(str(b) for b, _ in ws), ', '.join(f(w) for _, w in ws)))
    out.append('};')
    out.append('constexpr unsigned short PLAYER_BODY_INDICES_%d[] = {' % p)
    for r in range(0, len(idx), 24):
        out.append(','.join(str(x) for x in idx[r:r + 24]) + ',')
    out.append('};')
out.append('constexpr PlayerBodyPart PLAYER_BODY_PARTS[] = {  // name, own texture (-1: the game\'s by name), game texture')
for p, (label, own, game_name, verts, idx) in enumerate(parts):
    out.append('    {"%s", %d, "%s", PLAYER_BODY_VERTICES_%d, %d, PLAYER_BODY_INDICES_%d, %d},' % (
        label, own, game_name, p, len(verts), p, len(idx)))
out.append('};')
open(OUT, 'w').write('\n'.join(out) + '\n')
print('EXPORT -> %s: %d bones, %d parts, %d textures, %d vertices, %d triangles' % (
    OUT, len(names), len(parts), len(textures), sum(len(p[3]) for p in parts), sum(len(p[4]) for p in parts) // 3))
