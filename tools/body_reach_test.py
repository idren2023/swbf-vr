"""Reach test (idren2023 2026-10-26: IK from the controllers only; the head places the lower spine, hips and legs; a
player reaching far forward leans the model's upper body so its arms do not stretch): the assembled player, the eye
fixed where the headset is (standing), each hand solved onto its target by Blender's IK through the arm, collarbone,
ribcage and upper spine (the spine stiff: it bends only once the arm alone cannot reach), the hips not moving. Each
pose rendered from the eye and from the side.
Run: blender -b re/body/player_stock.blend -P tools/body_reach_test.py -- <out prefix>
"""
import bpy, math, os, sys
from mathutils import Vector

prefix = os.path.abspath(sys.argv[sys.argv.index('--') + 1])
MODE = (sys.argv[sys.argv.index('--') + 2:] + ['reach'])[0]     # reach | natural (everyday poses, natural gaze)
sc = bpy.context.scene
pl = bpy.data.objects['player']
rig = pl.parent
for o in sc.objects:
    if o.type == 'MESH':
        o.hide_render = o.name not in ('player', 'r_body_packs')
EYE = Vector((0.0, -0.11, 1.875))    # between the eyes: the stock head's nose tip at 1.84; a 2.00 m man (0.936 of it)
CHAIN = ['l_forearm', 'bone_l_upperarm', 'root_l_upperarm', 'eff_l_clavicle', 'bone_l_clavicle', 'root_l_clavicle',
         'eff_ribcage', 'bone_ribcage', 'bone_b_spine']
STIFF = {'bone_l_clavicle': 0.7, 'bone_ribcage': 0.97, 'bone_b_spine': 0.98}   # the arm reaches first
targets = {}
for s in 'lr':
    e = bpy.data.objects.new('target_' + s, None)
    sc.collection.objects.link(e)
    targets[s] = e
    c = rig.pose.bones['%s_forearm' % s].constraints.new('IK')
    c.target = e
    c.chain_count = len(CHAIN)
    c.use_tail = True
    c.use_stretch = False
    for bn in CHAIN[1:]:
        pb = rig.pose.bones[bn.replace('_l_', '_%s_' % s)]
        if bn in ('root_l_upperarm', 'eff_l_clavicle', 'root_l_clavicle', 'eff_ribcage'):
            pb.lock_ik_x = pb.lock_ik_y = pb.lock_ik_z = True
        elif bn in STIFF:
            st = STIFF[bn]
            pb.ik_stiffness_x = pb.ik_stiffness_y = pb.ik_stiffness_z = st
        if bn in ('bone_ribcage', 'bone_b_spine'):
            pb.lock_ik_y = False
arm_len = sum((rig.data.bones[n].tail_local - rig.data.bones[n].head_local).length
              for n in ('bone_l_upperarm', 'l_forearm'))
sh = rig.data.bones['bone_l_upperarm'].head_local
print('REACH arm (shoulder to wrist) %.3f m; shoulder %.2f m below and %.2f m behind the eye' % (
    arm_len, EYE.z - sh.z, sh.y - EYE.y))
# targets: wrist places relative to the eye (x right of the model's left is +x; forward is -y)
P = lambda x, fwd, down: EYE + Vector((x, -fwd, -down))
POSES = [
    ('both_near', {'l': P(0.20, 0.45, 0.40), 'r': P(-0.20, 0.45, 0.40)}),
    ('both_full', {'l': P(0.20, 0.65, 0.30), 'r': P(-0.20, 0.65, 0.30)}),
    ('right_far', {'l': P(0.25, 0.25, 0.65), 'r': P(-0.15, 0.90, 0.25)}),
    ('right_far_low', {'l': P(0.25, 0.25, 0.65), 'r': P(-0.10, 0.85, 0.75)}),
    ('both_far_up', {'l': P(0.25, 0.75, -0.05), 'r': P(-0.25, 0.75, -0.05)}),
    ('right_across', {'l': P(0.25, 0.25, 0.65), 'r': P(0.35, 0.75, 0.30)}),
]
# everyday first-person poses (idren2023 2026-10-26): (name, targets, gaze as (yaw left, pitch down) in degrees)
NATURAL = [
    ('idle_ahead', {'l': P(0.24, 0.02, 0.92), 'r': P(-0.24, 0.02, 0.92)}, (0, 10)),
    ('idle_look_down', {'l': P(0.24, 0.02, 0.92), 'r': P(-0.24, 0.02, 0.92)}, (0, 60)),
    ('rifle_ready', {'l': P(0.04, 0.50, 0.48), 'r': P(-0.12, 0.28, 0.50)}, (0, 15)),
    ('rifle_ready_look_down', {'l': P(0.04, 0.50, 0.48), 'r': P(-0.12, 0.28, 0.50)}, (0, 45)),
    ('aiming', {'l': P(0.02, 0.52, 0.24), 'r': P(-0.08, 0.26, 0.20)}, (0, 2)),
    ('check_left_wrist', {'l': P(0.06, 0.33, 0.38), 'r': P(-0.24, 0.02, 0.92)}, (12, 40)),
    ('look_left_walking', {'l': P(0.22, -0.05, 0.92), 'r': P(-0.22, 0.10, 0.90)}, (60, 20)),
    ('pistol_one_hand', {'l': P(0.24, 0.02, 0.92), 'r': P(-0.10, 0.50, 0.30)}, (0, 8)),
]
if MODE == 'natural':
    POSES = [(n, t) for n, t, _ in NATURAL]
    GAZE = {n: g for n, _, g in NATURAL}
else:
    GAZE = {}
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'
sc.display.shading.color_type = 'TEXTURE'
sc.render.resolution_x = sc.render.resolution_y = 560
eye = bpy.data.objects.new('eye', bpy.data.cameras.new('eye'))
sc.collection.objects.link(eye)
eye.data.angle = math.radians(100)
eye.data.clip_start = 0.12      # what is this near the eye is not drawn (the neck's cap, the collar)
side = bpy.data.objects.new('side', bpy.data.cameras.new('side'))
sc.collection.objects.link(side)
side.data.type = 'ORTHO'
side.data.ortho_scale = 2.3
side.location = Vector((2.5, -0.35, 1.05))
side.rotation_euler = Vector((-1, 0, 0)).to_track_quat('-Z', 'Y').to_euler()
for name, tg in POSES:
    for s, p in tg.items():
        targets[s].location = p
    bpy.context.view_layer.update()
    mid = (tg['l'] + tg['r']) / 2
    look = (mid - EYE).normalized()
    if name in GAZE:
        yaw, pitch = (math.radians(a) for a in GAZE[name])
        look = Vector((math.sin(yaw) * math.cos(pitch), -math.cos(yaw) * math.cos(pitch), -math.sin(pitch)))
    eye.location = EYE
    eye.rotation_euler = look.to_track_quat('-Z', 'Y').to_euler()
    lean = math.degrees((rig.pose.bones['bone_ribcage'].tail - rig.pose.bones['bone_b_spine'].head).angle(Vector((0, 0, 1))))
    reach = [(rig.pose.bones['%s_forearm' % s].tail - p).length * 1000 for s, p in tg.items()]
    print('REACH %-14s upper body leaning %4.1f deg; wrists off their targets %s mm' % (
        name, lean, ', '.join('%.0f' % r for r in reach)))
    for cam, tag in ((eye, 'eye'), (side, 'side')):
        sc.camera = cam
        sc.render.filepath = '%s-%s-%s.png' % (prefix, name, tag)
        bpy.ops.render.render(write_still=True)
