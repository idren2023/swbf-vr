"""Blender (blender -b --factory-startup <blend with the B1: tools/body_b1.py> -P tools/body_b1grab.py -- <grip>
[web=x,y,z] [heel=x,y,z] [palm=mm] [top=mm] [back=mm] [turn=deg] [up=mm] [keep] [press=mm] [set=<bone>:x,y,z ..]
[render=<out>] [save] [write]): a weapon grabbed by the B1 battle droid's hand (PLAN 60 step 4d, idren2023 2026-11-02:
grips for every droid weapon), as tools/body_grab.py does for the human hand, on the B1's simpler digits (trigger,
grip finger and thumb, two hinged segments each):

1. the weapon (weap_cis_<name>, or weap_b1_<grip> for the common models and items) turned so its grip's centre line
   (GRIPS: read off the msh's side view, with its vertices' reach from it) runs from the web to the heel of the hand, then moved so
   the grip's top is at the web (`top` mm above it), its back at the web (`back` mm behind) and its side on the palm's
   underside (`palm`, z mm); `turn` turns the line about the hand's z, `up` slides the weapon along it afterwards;
   keep: the weapon left where it is (the common models and items start where the human hand holds them);
2. the grip finger and the thumb closed onto it: of the poses (curl at each joint, the thumb's base also swung) whose
   points (the B1 hand's own vertices on those bones) stay off the weapon (`press` mm into it allowed) and out of it,
   the one lying closest along its surface; the trigger finger's tip segment onto the trigger (GRIPS) where there is
   one, else closed as the grip finger;
3. set=: a bone's angles after the search.

save: the weapon's place (in_hand_bone) and the controller's grip axis (the grip's centre line) in
re/body/grip-<object>.json; write: the pose into tools/body_poses.py's 'b1 fitted' group; render=: <out>.png, the
posed hand with the weapon from five sides; dump=<json>: the posed triangles for tools/body_b1sheet.py."""
import bpy, bmesh, json, math, os, re, sys
from mathutils import Euler, Matrix, Vector
from mathutils.bvhtree import BVHTree

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import mshgeo

args = sys.argv[sys.argv.index('--') + 1:]
GRIP = args[0]
opt = {}
for a in args[1:]:
    if '=' in a:
        k, v = a.split('=', 1)
        opt.setdefault(k, []).append(v)
one = lambda k, d=None: opt.get(k, [d])[-1]
vec = lambda s: Vector([float(x) / 1000 for x in s.split(',')])
A = 'C:/LucasArts/BFBuilder/Assets/Sides/'

# per CIS weapon, read off the msh's side views (z forward, y up, mm, the model's middle in x): its grip's centre line
# from the top (where the frame starts) to the bottom, how far its vertices lie from that line (the low-poly grips
# have vertices only at their corners), and its trigger (None: no trigger)
GRIPS = {'cis_rifle': ((-62, -15), (-100, -120), 48, (-15, -25)),
         'cis_pistol': ((-60, -15), (-125, -135), 40, (-30, -25)),
         'cis_sniperrifle': ((-75, 28), (-105, -45), 28, (22, -20)),  # the thumbhole stock's wrist, before the hole
         'cis_launcher': ((-60, -6), (-60, -130), 36, (5, -25)),
         'cis_fusioncutter': ((-70, -120), (-70, -210), 45, None)}
LABELS = {'cis_rifle': 'E-5 rifle (fitted)', 'cis_pistol': 'CIS pistol (fitted)',
          'cis_sniperrifle': 'CIS sniper rifle (fitted)', 'cis_launcher': 'CIS rocket launcher (fitted)',
          'cis_fusioncutter': 'CIS fusion cutter (fitted)', 'com_torpedo': 'torpedo launcher (fitted)',
          'com_grenadethermal': 'grenade (fitted)', 'item_landmine': 'landmine (fitted)',
          'item_probedroid': 'recon droid (fitted)', 'item_powerup': 'health / ammo dispenser (fitted)'}
rig = bpy.data.objects['b1_skeleton']
hand = bpy.data.objects['b1_hand']
w = bpy.data.objects['weap_' + GRIP] if GRIP.startswith('cis_') else bpy.data.objects['weap_b1_' + GRIP]
JSON_PATH = os.path.join('re', 'body', 'grip-%s.json' % w.name)
saved = json.load(open(JSON_PATH)) if os.path.exists(JSON_PATH) else {}
PRESS = float(one('press', 1.0)) / 1000
PALM_Z = float(one('palm', -16)) / 1000
# the web and the heel of the B1's palm: the grip under the palm block, behind the knuckles, so the long finger bars
# drop in front of it and their tips curl back round it
WEB = vec(one('web', '-30,20,-15'))
HEEL = vec(one('heel', '25,5,-15'))
KEEP = 'keep' in args or not GRIP.startswith('cis_')
HAND_REST = rig.matrix_world @ rig.data.bones['hand'].matrix_local

# ---- the skeleton's kinematics (rest frames in arm space; poses as Euler angles on them, as BodyArm) ----
BONES = {b.name: b for b in rig.data.bones}
ORDER = {n: 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ' for n in BONES}
DIGITS = {'trigger': ('trigger_1', 'trigger_2'), 'grip': ('grip_1', 'grip_2'), 'thumb': ('thumb_1', 'thumb_2')}


def posed(pose):
    """Each bone's posed frame (arm space) for a pose {bone: degrees}."""
    out = {}

    def frame(n):
        if n in out:
            return out[n]
        b = BONES[n]
        rot = Euler([math.radians(a) for a in pose.get(n, (0, 0, 0))], ORDER[n]).to_matrix().to_4x4()
        local = (b.parent.matrix_local.inverted() @ b.matrix_local) if b.parent else b.matrix_local
        out[n] = (frame(b.parent.name) if b.parent else Matrix()) @ local @ rot
        return out[n]
    for n in BONES:
        frame(n)
    return out


# the hand's own vertices on each digit bone (rest, arm space), a sample of each
names = {g.index: g.name for g in hand.vertex_groups}
SAMPLES = {}
for v in hand.data.vertices:
    for g in v.groups:
        if g.weight > 0.5 and names[g.group] in ORDER:
            SAMPLES.setdefault(names[g.group], []).append(v.co.copy())
for n, pts in SAMPLES.items():
    SAMPLES[n] = pts[::max(1, len(pts) // 48)]
INVERSE_REST = {n: b.matrix_local.inverted() for n, b in BONES.items()}


def points(frames, n):
    m = frames[n] @ INVERSE_REST[n]
    return [m @ p for p in SAMPLES.get(n, [])]


# ---- the weapon: its msh vertices (as tools/body_b1.py built the object, in that order) ----
def msh_vertices():
    path = A + 'CIS/MSH/cis_1st_weap_inf_%s.msh' % GRIP[4:]
    out = []
    for node in mshgeo.load(path)['nodes']:
        if not mshgeo.is_drawn(node) or 'lens' in node['name'].lower():
            continue
        for seg in node['segments']:
            if not seg.get('uv') or not seg.get('texture') or seg['texture'].endswith('.pic'):
                continue
            out += [Vector(p) * 1000 for p in seg['wpos']]
    assert len(out) == len(w.data.vertices), (len(out), len(w.data.vertices))
    return out


def world_vertices():
    return [w.matrix_world @ v.co for v in w.data.vertices]


turned = 0.0
TRIGGER = None
if not KEEP:
    top_msh, bottom_msh, reach, trigger_msh = GRIPS[GRIP]
    msh = msh_vertices()
    import numpy as np
    # the msh (mm) -> world map of the weapon as it stands (affine, fitted over its vertices)
    X = np.array([[p.x, p.y, p.z, 1.0] for p in msh])
    fit = np.linalg.lstsq(X, np.array([list(p) for p in world_vertices()]), rcond=None)[0]
    to_world = lambda p: Vector(list(np.array([p.x, p.y, p.z, 1.0]) @ fit))
    mid_x = sum(p.x for p in msh) / len(msh)
    top3, bottom3 = Vector((mid_x, top_msh[1], top_msh[0])), Vector((mid_x, bottom_msh[1], bottom_msh[0]))
    along = (bottom3 - top3).normalized()
    # the grip as points over its surface (the low-poly grips have vertices only at their corners): each triangle's
    # barycentric samples about 4 mm apart (msh), kept where they lie within reach of the centre line
    w.data.calc_loop_triangles()
    grip_samples = []  # (vertex a, b, c, weights)
    for tri in w.data.loop_triangles:
        a_, b_, c_ = (msh[i] for i in tri.vertices)
        n = max(1, int(max((a_ - b_).length, (b_ - c_).length, (c_ - a_).length) / 4))
        for i in range(n + 1):
            for j in range(n + 1 - i):
                u, v = i / n, j / n
                p = a_ * (1 - u - v) + b_ * u + c_ * v
                t = (p - top3).dot(along)
                q = p - top3 - along * t
                if -5 <= t <= (bottom3 - top3).length + 5 and math.hypot(q.y, q.z) < reach:
                    grip_samples.append((tuple(tri.vertices), (1 - u - v, u, v)))

    def grip_points():
        V = world_vertices()
        return [V[i] * k + V[j] * l + V[m] * o for (i, j, m), (k, l, o) in grip_samples]
    G = grip_points()
    c = to_world((top3 + bottom3) / 2)
    a = (to_world(bottom3) - to_world(top3)).normalized()
    AXIS = (HEEL - WEB).normalized()
    if one('turn'):
        AXIS = Matrix.Rotation(math.radians(float(one('turn'))), 3, 'Z') @ AXIS
    turn = a.rotation_difference(AXIS).to_matrix().to_4x4()
    turned = math.degrees(a.angle(AXIS))
    w.matrix_world = Matrix.Translation(c) @ turn @ Matrix.Translation(-c) @ w.matrix_world
    bpy.context.view_layer.update()
    G = grip_points()
    ez = Vector((0, 0, 1))
    ef = ez.cross(AXIS).normalized()  # forward across the grip (the trigger's side)
    s_top = min(p.dot(AXIS) for p in G)
    near_top = [p for p in G if p.dot(AXIS) < s_top + 0.025]
    back = min(p.dot(ef) for p in near_top)
    face = max(p.z for p in G)
    move = (AXIS * (WEB.dot(AXIS) - float(one('top', 0)) / 1000 - s_top) +
            ef * (WEB.dot(ef) - float(one('back', 0)) / 1000 - back) + ez * (PALM_Z - face))
    w.matrix_world = Matrix.Translation(move) @ w.matrix_world
    bpy.context.view_layer.update()
    G = grip_points()
    s = sorted(p.dot(AXIS) for p in G)
    mid_s = (s[0] + s[-1]) / 2
    centroid = sum(G, Vector()) / len(G)
    AXIS_POINT = centroid + AXIS * (mid_s - centroid.dot(AXIS))  # the grip's centre line, half way down
    print('B1GRAB %s: grip %d surface points, turned %.0f deg, moved %s mm; grip %.0f mm long' % (
        GRIP, len(grip_samples), turned, tuple(round(x * 1000) for x in move), (s[-1] - s[0]) * 1000))
else:
    G = world_vertices()
    AXIS = Vector(saved.get('axis_direction', (1, 0, 0)))
    AXIS_POINT = Vector(saved.get('axis_point', sum(G, Vector()) / len(G)))
    trigger_msh = None
if one('up'):
    w.matrix_world = Matrix.Translation(-AXIS * float(one('up')) / 1000) @ w.matrix_world  # the hand up the grip
    AXIS_POINT = AXIS_POINT - AXIS * float(one('up')) / 1000
    bpy.context.view_layer.update()
if one('trigger'):  # the trigger in the stock model's arm space (tools/body_b1.py stock=1 bakes it there)
    TRIGGER = w.matrix_world @ vec(one('trigger'))
if not KEEP and trigger_msh:  # the trigger's msh point carried as the weapon's vertices are (refitted where it now is)
    fit = np.linalg.lstsq(X, np.array([list(p) for p in world_vertices()]), rcond=None)[0]
    TRIGGER = Vector(list(np.array([mid_x, trigger_msh[1], trigger_msh[0], 1.0]) @ fit))

# ---- the weapon as a surface and a volume ----
bm = bmesh.new()
bm.from_mesh(w.data)
bm.transform(w.matrix_world)
WT = BVHTree.FromBMesh(bm)
RAYS = (Vector((0, 0, 1)), Vector((0, 0, -1)), Vector((0.3, 0.9, 0.1)).normalized())


def inside(p):
    votes = 0
    for d in RAYS:
        q, n = p.copy(), 0
        for _ in range(24):
            hit = WT.ray_cast(q, d, 1.0)
            if hit[0] is None:
                break
            n += 1
            q = hit[0] + d * 1e-5
        votes += n % 2
    return votes >= 2


def depth(p):
    """How far p is from the weapon's surface: positive outside, negative inside."""
    loc, _, _, dist = WT.find_nearest(p, 0.2)
    if loc is None:
        return 0.2
    return -dist if dist < 0.03 and inside(p) else dist


def digit_score(frames, bones):
    """None if the digit goes into the weapon, else how far its tip segment (and less, its base) lies off it."""
    total = 0.0
    for k, n in enumerate(bones):
        ds = [depth(p) for p in points(frames, n)]
        if min(ds) < -PRESS:
            return None
        total += (1.0 if k else 0.3) * sum(max(d, 0) for d in ds) / len(ds)
    return total


def segment_distance(p, a, b):
    ab = b - a
    t = max(0.0, min(1.0, (p - a).dot(ab) / ab.length_squared))
    return (a + ab * t - p).length


def search(digit, base_pose):
    b1, b2 = DIGITS[digit]
    best = None
    swings = range(-40, 41, 10) if digit == 'thumb' else (0,)

    def trial(x1, z1, x2):
        pose = dict(base_pose)
        pose[b1], pose[b2] = (x1, 0, z1), (x2, 0, 0)
        frames = posed(pose)
        score = digit_score(frames, (b1, b2))
        if score is None:
            return None
        if digit == 'trigger' and TRIGGER is not None:
            head = frames[b2].translation
            tail = frames[b2] @ Vector((0, BONES[b2].length, 0))
            score = segment_distance(TRIGGER, head, tail) + 0.1 * score
        return score
    # a bone of the digit given with set= keeps its angle; the other is searched
    x1s = [FIXED[b1][0]] if b1 in FIXED else range(int(one('open', 30)), -121, -6)  # open=: how far up it may flex
    x2s = [FIXED[b2][0]] if b2 in FIXED else range(60, -121, -6)
    if b1 in FIXED:
        swings = (FIXED[b1][2],)
    for x1 in x1s:
        for z1 in swings:
            for x2 in x2s:
                sc = trial(x1, z1, x2)
                if sc is not None and (best is None or sc < best[0]):
                    best = (sc, x1, z1, x2)
    if best is None:
        print('B1GRAB %s: no pose clear of the weapon' % digit)
        return base_pose
    _, x1, z1, x2 = best
    for step in (3, 1):  # refined round the best
        for dx1 in ((0,) if b1 in FIXED else (-step, 0, step)):
            for dz1 in ((-step, 0, step) if digit == 'thumb' and b1 not in FIXED else (0,)):
                for dx2 in ((0,) if b2 in FIXED else (-step, 0, step)):
                    sc = trial(x1 + dx1, z1 + dz1, x2 + dx2)
                    if sc is not None and sc < best[0]:
                        best = (sc, x1 + dx1, z1 + dz1, x2 + dx2)
        _, x1, z1, x2 = best
    pose = dict(base_pose)
    pose[b1], pose[b2] = (x1, 0, z1), (x2, 0, 0)
    print('B1GRAB %s: %s %s, %s %s; %s %.1f mm' % (
        digit, b1, pose[b1], b2, pose[b2], 'tip segment from the trigger' if digit == 'trigger' and TRIGGER else
        'mean gap', best[0] * 1000))
    return pose


pose = {}
FIXED = {}
for a in opt.get('set', []):  # bones held as given; a digit with both its bones set is not searched
    n, v = a.split(':')
    FIXED[n] = tuple(float(x) for x in v.split(','))
pose.update(FIXED)
if one('slide'):  # the weapon slid along this direction (arm space) until the set= bones just clear it
    sd = Vector([float(x) for x in one('slide').split(',')]).normalized()
    fixed_frames = posed(pose)
    fixed_points = [p for n in set(FIXED) | {'hand'} if n in SAMPLES for p in points(fixed_frames, n)]  # and the palm
    for mm in range(0, 121):
        if all(depth(p - sd * (mm / 1000)) > -PRESS for p in fixed_points):
            break
    w.matrix_world = Matrix.Translation(sd * (mm / 1000)) @ w.matrix_world
    bpy.context.view_layer.update()
    bm = bmesh.new()
    bm.from_mesh(w.data)
    bm.transform(w.matrix_world)
    WT = BVHTree.FromBMesh(bm)
    print('B1GRAB slid %d mm along %s' % (mm, tuple(round(x, 2) for x in sd)))
    TWEAK_SLIDE = list(sd * mm)
for digit in ('grip', 'thumb', 'trigger'):
    if not all(b in FIXED for b in DIGITS[digit]):
        pose = search(digit, pose)
pose.update(FIXED)
frames = posed(pose)
for n in ORDER:
    if SAMPLES.get(n):
        ds = [depth(p) for p in points(frames, n)]
        print('B1GRAB check %-10s closest %.1f mm%s' % (n, min(ds) * 1000, ' (in the weapon)' if min(ds) < -PRESS else ''))

# ---- results ----
if 'save' in args:
    saved.update(weapon=w.name, in_hand_bone=[list(r) for r in HAND_REST.inverted() @ w.matrix_world],
                 axis_point=list(HAND_REST.inverted() @ AXIS_POINT), axis_direction=list(HAND_REST.inverted().to_3x3() @ AXIS),
                 placement='tools/body_b1grab.py ' + ' '.join(args))
    json.dump(saved, open(JSON_PATH, 'w'), indent=1)
    print('B1GRAB saved %s' % JSON_PATH)
if 'write' in args:
    path = os.path.join(TOOLS, 'body_poses.py')
    src = open(path, encoding='utf-8').read()
    line = "        ('%s', {%s})," % (LABELS[GRIP], ', '.join("'%s': (%s)" % (n, ', '.join('%g' % x for x in d))
                                                           for n, d in sorted(pose.items())))
    old = re.search(r"^        \('%s', \{.*\}\),$" % re.escape(LABELS[GRIP]), src[src.index("('b1 fitted'"):], re.M)
    if old:
        start = src.index("('b1 fitted'") + old.start()
        src = src[:start] + line + src[start + len(old.group(0)):]
    else:
        marker = "    ('b1 fitted', [  # the B1's grips (tools/body_b1grab.py)\n"
        src = src.replace(marker, marker + line + '\n')
    open(path, 'w', encoding='utf-8', newline='\n').write(src)
    print('B1GRAB wrote the pose of %s' % LABELS[GRIP])
if 'tweak' in args:  # the pose into tools/body_b1.py's stock tweaks (re/body/b1-stock-tweaks.json)
    tp = os.path.join('re', 'body', 'b1-stock-tweaks.json')
    tweaks = json.load(open(tp)) if os.path.exists(tp) else {}
    entry = tweaks.setdefault(GRIP[4:] if GRIP.startswith('cis_') else GRIP, {})
    entry['pose'] = {n: list(d) for n, d in pose.items()}
    if one('slide'):  # the slide as one more step after the tweak's own
        last = entry
        while 'after' in last:
            last = last['after']
        last['after'] = {'move': TWEAK_SLIDE}
    json.dump(tweaks, open(tp, 'w'), indent=1)
    print('B1GRAB tweaked the pose of %s' % GRIP)
if one('dump'):  # the posed hand's and the weapon's triangles (arm space, mm) for tools/body_b1sheet.py
    bone_of = {}
    for v in hand.data.vertices:
        g = max(v.groups, key=lambda g: g.weight)
        bone_of[v.index] = names[g.group]
    skin = {n: frames[n] @ INVERSE_REST[n] for n in frames}
    hv = [list(skin[bone_of[v.index]] @ v.co * 1000) for v in hand.data.vertices]
    hand.data.calc_loop_triangles()
    w.data.calc_loop_triangles()
    wv = [list(p * 1000) for p in world_vertices()]
    json.dump({'grip': GRIP, 'pose': pose, 'hand': hv, 'hand_tris': [list(t.vertices) for t in hand.data.loop_triangles],
               'bone': [bone_of[i] for i in range(len(hv))], 'weapon': wv,
               'weapon_tris': [list(t.vertices) for t in w.data.loop_triangles],
               'trigger': list(TRIGGER * 1000) if TRIGGER else None, 'axis_point': list(AXIS_POINT * 1000),
               'axis': list(AXIS)}, open(one('dump'), 'w'))
if one('render'):
    OUT = os.path.abspath(one('render'))
    for pb in rig.pose.bones:
        pb.rotation_mode = ORDER[pb.name]
        pb.rotation_euler = [math.radians(x) for x in pose.get(pb.name, (0, 0, 0))]
    scene = bpy.context.scene
    for o in scene.objects:
        o.hide_render = o.type == 'MESH' and o not in (hand, w, bpy.data.objects.get('b1_arm'))
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.color_type = 'TEXTURE'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.show_cavity = True
    scene.world = scene.world or bpy.data.worlds.new('b1_world')
    scene.world.color = (0.55, 0.57, 0.6)
    TS = 420
    scene.render.resolution_x = scene.render.resolution_y = TS
    cam = bpy.data.objects.new('grab', bpy.data.cameras.new('grab'))
    cam.data.type = 'ORTHO'
    cam.data.ortho_scale = float(one('scale', 0.17))
    scene.collection.objects.link(cam)
    scene.camera = cam
    centre = AXIS_POINT
    # the gun's top is the hand's -x, its right side the back of the hand (+z)
    VIEWS = (('from the eye (above, behind)', (-0.8, -0.55, 0.25)), ("the gun's right side", (-0.1, 0.05, 1)),
             ("the gun's left side", (-0.1, 0.05, -1)), ('from below', (1, 0.15, 0.1)), ('from the front', (-0.3, 1, 0.2)))
    import numpy as np
    tiles = []
    for k, (label, d) in enumerate(VIEWS):
        d = Vector(d).normalized()
        cam.location = centre + d
        cam.rotation_euler = (-d).to_track_quat('-Z', 'X').to_euler()  # the gun's top (the hand's -x) up
        scene.render.filepath = OUT + '-%d.png' % k
        bpy.ops.render.render(write_still=True)
        im = bpy.data.images.load(scene.render.filepath)
        tiles.append(np.array(im.pixels[:]).reshape(TS, TS, 4))
        bpy.data.images.remove(im)
        os.remove(scene.render.filepath)
    sheet = np.concatenate(tiles, axis=1)
    im = bpy.data.images.new('sheet', TS * len(tiles), TS, alpha=True)
    im.pixels = sheet.ravel()
    im.filepath_raw = OUT + '.png'
    im.file_format = 'PNG'
    im.save()
    print('B1GRAB rendered %s.png' % OUT)
