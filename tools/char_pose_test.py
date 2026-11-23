"""Deformation test from the player's eye (idren2023 2026-10-26: will the body hold up when the player looks down and
moves?): the stock body (head and gloves off), and game meshes (weights copied from the stock, smoothed), posed the
way a VR player moves and rendered from the eye looking down, textured and as clay with edges.
Run: blender -b <stock.blend> -P tools/char_pose_test.py -- <config.json> <stock object> <hd.png> <out dir> \
        <name>=<mesh.blend> [...]
"""
import bpy, bmesh, json, math, os, sys
import numpy as np
from mathutils import Vector, Matrix, Euler

args = sys.argv[sys.argv.index('--') + 1:]
cfg = json.load(open(args[0]))
stock_name, hd, out = args[1], os.path.abspath(args[2]), os.path.abspath(args[3])
others = dict(a.split('=', 1) for a in args[4:])
os.makedirs(out, exist_ok=True)
sc = bpy.context.scene
rig = bpy.data.objects[cfg['lowpoly']['skeleton']]
island_map = np.load(cfg['texmaps'])['island']
N = island_map.shape[0]
hd_img = bpy.data.images.load(hd)


def tex_material(name):
    m = bpy.data.materials.new(name)
    m.diffuse_color = (0.75, 0.75, 0.75, 1)       # its clay colour (the edges pass draws materials' own colours)
    m.use_nodes = True
    t = m.node_tree.nodes.new('ShaderNodeTexImage')
    m.node_tree.nodes.active = t  # (textured views draw the active image node)
    t.image = hd_img
    m.node_tree.links.new(t.outputs['Color'], m.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
    return m


# ---- the stock body: its main material's body islands only (no head, no gloves, no packs) -------------------------
st = bpy.data.objects[stock_name]
stock = st.copy()
stock.data = st.data.copy()
stock.name = 'stock'
sc.collection.objects.link(stock)
mw_ = st.matrix_world.copy()
stock.parent = None
stock.matrix_world = Matrix.Identity(4)
st.hide_render = True
bm = bmesh.new()
bm.from_mesh(stock.data)
uvl = bm.loops.layers.uv.active


def texel(uv):
    return min(int((1 - uv.y % 1.0) * N), N - 1), min(int(uv.x % 1.0 * N), N - 1)


bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != 0 or island_map[texel(
    sum((l[uvl].uv for l in f.loops), Vector((0, 0))) / len(f.loops))] not in cfg['lowpoly']['body_islands']],
    context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
bm.to_mesh(stock.data)
stock.data.materials.clear()
stock.data.materials.append(tex_material('stock_tex'))
for p in stock.data.polygons:
    p.material_index = 0
    p.use_smooth = True
for m in stock.modifiers:
    if m.type == 'ARMATURE':
        m.object = rig
meshes = {'stock': stock}

# ---- the game meshes: weights from the stock (nearest face, interpolated), smoothed, normalized -------------------
for name, path in others.items():
    with bpy.data.libraries.load(os.path.abspath(path)) as (src, dst):
        dst.objects = ['body_low']
    o = dst.objects[0]
    o.name = name
    sc.collection.objects.link(o)
    o.parent = None
    o.matrix_world = Matrix.Identity(4)
    for m in list(o.modifiers):
        o.modifiers.remove(m)
    for g in stock.vertex_groups:
        o.vertex_groups.new(name=g.name)
    dt = o.modifiers.new('w', 'DATA_TRANSFER')
    dt.object = stock
    dt.use_vert_data = True
    dt.data_types_verts = {'VGROUP_WEIGHTS'}
    dt.vert_mapping = 'POLYINTERP_NEAREST'
    dt.layers_vgroup_select_src = 'ALL'
    dt.layers_vgroup_select_dst = 'NAME'
    # the stock's weights are read in its rest shape
    arm_mods = [m for m in stock.modifiers if m.type == 'ARMATURE']
    for m in arm_mods:
        m.show_viewport = False
    bpy.context.view_layer.objects.active = o
    for x in sc.objects:
        x.select_set(x is o)
    bpy.ops.object.modifier_apply(modifier='w')
    for m in arm_mods:
        m.show_viewport = True
    # smoothed over the edges (each weight half way to its neighbours' mean, a few times), then normalized
    nv = len(o.data.vertices)
    W = np.zeros((nv, len(o.vertex_groups)), np.float32)
    for v in o.data.vertices:
        for g in v.groups:
            W[v.index, g.group] = g.weight
    ea = np.array([e.vertices[:] for e in o.data.edges])
    deg = np.bincount(ea.ravel(), minlength=nv).astype(np.float32)[:, None]
    for _ in range(cfg.get('pose_test', {}).get('smooth', 6)):
        acc = np.zeros_like(W)
        np.add.at(acc, ea[:, 0], W[ea[:, 1]])
        np.add.at(acc, ea[:, 1], W[ea[:, 0]])
        W = 0.5 * W + 0.5 * acc / np.maximum(deg, 1)
    W /= np.maximum(W.sum(1, keepdims=True), 1e-6)
    for gi, g in enumerate(o.vertex_groups):
        idx = np.nonzero(W[:, gi] > 0.001)[0]
        g.remove(list(range(nv)))
        for i_ in idx:
            g.add([int(i_)], float(W[i_, gi]), 'REPLACE')
    am = o.modifiers.new('skeleton', 'ARMATURE')
    am.object = rig
    o.data.materials.clear()
    o.data.materials.append(tex_material(name + '_tex'))
    for p in o.data.polygons:
        p.material_index = 0
        p.use_smooth = True
    meshes[name] = o
# wire copies (posed with them)
black = bpy.data.materials.new('black')
black.diffuse_color = (0, 0, 0, 1)
wires = {}
for name, o in meshes.items():
    w = o.copy()
    w.data = o.data.copy()
    w.name = name + '_wire'
    sc.collection.objects.link(w)
    wm = w.modifiers.new('wire', 'WIREFRAME')
    wm.thickness = 0.0015
    wm.use_replace = True
    w.data.materials.clear()
    w.data.materials.append(black)
    wires[name] = w

# ---- poses (world-space turns about each bone's head, parents first) -----------------------------------------------
ARMS_DOWN = {'bone_l_upperarm': ('Y', 70), 'bone_r_upperarm': ('Y', -70)}     # +Y lowers the left arm
POSES = {
    'stand': {},
    'lean_fwd': {'bone_a_spine': ('X', 12), 'bone_b_spine': ('X', 12), 'bone_ribcage': ('X', 12)},
    'lean_side': {'bone_a_spine': ('Y', 9), 'bone_b_spine': ('Y', 9), 'bone_ribcage': ('Y', 9)},
    'twist': {'bone_a_spine': ('Z', 15), 'bone_b_spine': ('Z', 15), 'bone_ribcage': ('Z', 15)},
    'crouch': {'bone_l_thigh': ('X', -75), 'bone_r_thigh': ('X', -75), 'bone_l_calf': ('X', 120),
               'bone_r_calf': ('X', 120), 'bone_a_spine': ('X', 15)},
    'arms_up': {'bone_l_upperarm': ('Y', -60), 'bone_r_upperarm': ('Y', 60)},
    'reach_across': {'bone_r_upperarm': ('Z', 80), 'bone_l_upperarm': ('Y', 70), 'bone_r_clavicle': ('Z', 15)},
}
ORDER = [b.name for b in rig.data.bones]   # parents come before children


def set_pose(spec):
    for pb in rig.pose.bones:
        pb.matrix_basis = Matrix.Identity(4)
    bpy.context.view_layer.update()
    full = dict(ARMS_DOWN)
    full.update(spec)
    for bn in ORDER:
        if bn not in full:
            continue
        ax, deg = full[bn]
        pb = rig.pose.bones[bn]
        h = pb.head.copy()
        r = Matrix.Rotation(math.radians(deg), 4, ax)
        pb.matrix = Matrix.Translation(h) @ r @ Matrix.Translation(-h) @ pb.matrix
        bpy.context.view_layer.update()


# ---- the eye camera: in front of the neck's top, turned with the ribcage, looking forward and down -----------------
cam = bpy.data.objects.new('eye', bpy.data.cameras.new('eye'))
sc.collection.objects.link(cam)
sc.camera = cam
cam.data.angle = math.radians(100)
cam.data.clip_start = 0.02
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'
sc.display.shading.show_cavity = True
sc.render.resolution_x = sc.render.resolution_y = 420
sc.render.film_transparent = False


def place_eye():
    rib = rig.pose.bones['bone_ribcage']
    neck = rig.pose.bones['bone_neck']
    rot = rib.matrix.to_3x3() @ rig.data.bones['bone_ribcage'].matrix_local.to_3x3().inverted()
    eye = neck.tail + rot @ Vector((0, -0.16, -0.02))     # the eyes: in front of the chest's front (y -0.15)
    look = rot @ Vector((0, -0.45, -0.89))
    cam.location = eye
    cam.rotation_euler = look.to_track_quat('-Z', 'Y').to_euler()


for pose, spec in POSES.items():
    set_pose(spec)
    place_eye()
    for name in meshes:
        for mode in ('tex', 'wire'):
            for o in sc.objects:
                if o.type == 'MESH':
                    o.hide_render = not (o is meshes[name] or (mode == 'wire' and o is wires[name]))
            sc.display.shading.color_type = 'TEXTURE' if mode == 'tex' else 'MATERIAL'
            sc.render.filepath = os.path.join(out, '%s-%s-%s.png' % (pose, name, mode))
            bpy.ops.render.render(write_still=True)
# close views from outside where the body bends: the left shoulder (front three-quarter) and the waist
ocam = bpy.data.objects.new('orbit', bpy.data.cameras.new('orbit'))
sc.collection.objects.link(ocam)
ocam.data.type = 'ORTHO'
CLOSE = {'shoulder': ('bone_l_upperarm', 'head', Vector((0.5, -1, 0.35)), 0.55),
         'waist': ('bone_a_spine', 'head', Vector((0.45, -1, 0.15)), 0.75)}
sc.camera = ocam
for pose, spec in POSES.items():
    set_pose(spec)
    for vn, (bn, end, d, s_) in CLOSE.items():
        pb = rig.pose.bones[bn]
        tgt = pb.head if end == 'head' else pb.tail
        dv = d.normalized()
        ocam.data.ortho_scale = s_
        ocam.location = tgt + dv * 2
        ocam.rotation_euler = (-dv).to_track_quat('-Z', 'Y').to_euler()
        for name in meshes:
            for o in sc.objects:
                if o.type == 'MESH':
                    o.hide_render = not (o is meshes[name] or o is wires[name])
            sc.display.shading.color_type = 'MATERIAL'
            sc.render.filepath = os.path.join(out, '%s-%s-%s.png' % (pose, name, vn))
            bpy.ops.render.render(write_still=True)
print('POSETEST %d poses x %s -> %s' % (len(POSES), list(meshes), out))
