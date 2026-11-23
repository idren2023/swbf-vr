"""Blender (blender -b --factory-startup re/body/r_arm_grips.blend -P tools/body_furcards.py -- [unit=all_1st_wookie]
[seed=1] [hand=3] [scale=1.1] [width=1.3] [lift=12] [cos=0.55]): fur cards over the back of our hand and fingers
(idren2023 2026-11-01/02: the Wookiee's hands need fur cards, transparency to sell the fur; about three on the back of
the hand and one per finger, on its base bone only).

The back of each bone is the faces of r_hand_retopo that mostly hang from it with their normal within acos(`cos`) of
the bone's z axis (the back of the hand). Over it, in the plane of its mean normal with the bone's direction as
"along": `hand` cards side by side across the back of the hand (each a `width` share of the hand's breadth, so they
overlap), one card per finger on index_1 .. pinky_1 and thumb_2 (thumb_1's z points across the palm), each from the
wrist / knuckle end, `scale` times the back's length. A card is a strip of four quads that follows the surface 1.5 mm
above it (rooted 1 mm under it), rising `lift` degrees more toward its tip, narrowing to 60 % there; it hangs wholly
from its bone, so it bends with it. Each takes a random window of the strand quarter of the unit's texture (u 0.5-1,
msh v 0.5 at the roots to 0 at the tips, up the image as Blender's uvs), so neighbours differ.

Saved into the .blend as hair_<unit>_hand with `texture` <unit's arms texture>_hair (our copy with its alpha:
tools/body_export.py OVERRIDES); BodyArm draws it alpha-tested with the unit's forearm (IsPartShown)."""
import bpy, math, random, sys

args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
opt = dict(a.split('=', 1) for a in args if '=' in a)
UNIT = opt.get('unit', 'all_1st_wookie')
TEXTURE = opt.get('texture', 'all_1st_inf_wookie_hair')
random.seed(int(opt.get('seed', 1)))
HAND_CARDS = int(opt.get('hand', 3))
SCALE = float(opt.get('scale', 1.1))
WIDTH = float(opt.get('width', 1.3))
LIFT = math.radians(float(opt.get('lift', 12)))
COS = float(opt.get('cos', 0.55))
NAME = 'hair_%s_hand' % UNIT
BONES = ('hand', 'index_1', 'middle_1', 'ring_1', 'pinky_1', 'thumb_2')
STEPS = 4
ABOVE = 0.0015

hand = bpy.data.objects['r_hand_retopo']
rig = hand.parent
me = hand.data
groups = {g.index: g.name for g in hand.vertex_groups}
bones = rig.data.bones


def dominant(face):
    total = {}
    for vi in face.vertices:
        for g in me.vertices[vi].groups:
            total[groups[g.group]] = total.get(groups[g.group], 0) + g.weight
    return max(total, key=total.get) if total else None


me.calc_loop_triangles()
back = {b: [] for b in BONES}  # per bone: (corner positions, normal, area)
for tri in me.loop_triangles:
    bone = dominant(tri)
    if bone in back and tri.normal.dot(bones[bone].z_axis) >= COS:
        back[bone].append(([me.vertices[i].co.copy() for i in tri.vertices], tri.normal.copy(), tri.area))

verts, faces, uvs, owner, report = [], [], [], [], []
for bone in BONES:
    tris = back[bone]
    if not tris:
        report.append('%s none' % bone)
        continue
    b = bones[bone]
    n = sum((t[1] * t[2] for t in tris), tris[0][1] * 0).normalized()
    along = (b.tail_local - b.head_local).normalized()
    d = (along - n * along.dot(n)).normalized()
    w = n.cross(d).normalized()
    points = [p for t in tris for p in t[0]] + [sum(t[0], t[0][0] * 0) / 3 for t in tris]
    ts = sorted(p.dot(d) for p in points)
    ws = sorted(p.dot(w) for p in points)
    t0, t1 = ts[int(0.03 * len(ts))], ts[int(0.97 * len(ts)) - 1]
    w0, w1 = ws[int(0.03 * len(ws))], ws[int(0.97 * len(ws)) - 1]
    origin = n * (sum(p.dot(n) for p in points) / len(points))

    def height(t, s, reach):
        """The surface's height along n near (t, s): the highest back point within reach (else the mean)."""
        near = [p.dot(n) for p in points if abs(p.dot(d) - t) < reach and abs(p.dot(w) - s) < reach]
        return max(near) if near else origin.dot(n)

    count = HAND_CARDS if bone == 'hand' else 1
    breadth = (w1 - w0) / count
    length = (t1 - t0) * SCALE
    for k in range(count):
        s = w0 + breadth * (k + 0.5)
        width = breadth * WIDTH
        reach = max(0.004, width * 0.3)
        span = 0.25 if bone == 'hand' else 0.15
        u0 = 0.5 + random.random() * (0.5 - span)
        base = len(verts)
        for i in range(STEPS + 1):
            f = i / STEPS
            t = t0 + length * f
            h = height(min(t, t1), s, reach) + (ABOVE if i else -0.001) + length * f * math.sin(LIFT) * f
            centre = d * t + w * s + n * h
            half = width * (0.5 - 0.2 * f)
            verts += [centre - w * half, centre + w * half]
            v = 0.48 - 0.46 * f  # roots at the strand quarter's top (v 0.5), tips toward the image's bottom
            uvs.append([(u0, v), (u0 + span, v)])
        for i in range(STEPS):
            faces.append((base + 2 * i, base + 2 * i + 1, base + 2 * i + 3, base + 2 * i + 2))
        owner += [bone] * (2 * STEPS + 2)
    report.append('%s %d x %.0f x %.0f mm' % (bone, count, length * 1000, breadth * WIDTH * 1000))

old = bpy.data.objects.get(NAME)
if old:
    bpy.data.objects.remove(old)
mesh = bpy.data.meshes.new(NAME)
mesh.from_pydata(verts, [], faces)
layer = mesh.uv_layers.new(name='UV')
flat = [uv for row in uvs for uv in row]
for poly in mesh.polygons:
    for li in poly.loop_indices:
        layer.data[li].uv = flat[mesh.loops[li].vertex_index]
for p in mesh.polygons:
    p.use_smooth = True
ob = bpy.data.objects.new(NAME, mesh)
bpy.context.scene.collection.objects.link(ob)
ob['texture'] = TEXTURE
for bone in set(owner):
    g = ob.vertex_groups.new(name=bone)
    g.add([i for i, o in enumerate(owner) if o == bone], 1.0, 'REPLACE')
ob.parent = rig
ob.matrix_world = hand.matrix_world.copy()
mod = ob.modifiers.new('rig', 'ARMATURE')
mod.object = rig
src = bpy.data.objects.get('hair_' + UNIT)
if src and src.data.materials:
    mesh.materials.append(src.data.materials[0])
bpy.ops.wm.save_mainfile()
print('FURCARDS %s: %d cards, %d triangles; %s' % (NAME, len(faces) // STEPS, 2 * len(faces), ', '.join(report)))
