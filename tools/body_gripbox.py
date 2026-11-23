"""Blender (blender -b --factory-startup <hand+weapons.blend> -P tools/body_gripbox.py -- <weapon-object> [pose=<label>]
[reach=mm]): finds a weapon's grip box for tools/body_placeweapon.py without reading grids (2026-10-31, the grips for
every weapon): the weapon where tools/body_weapons.py left it (the stock hold), our hand in a grip pose (default the
fitted rifle's), the tunnel the curled middle, ring and pinky make (as body_placeweapon.py fits it); the weapon's
vertices within `reach` mm (default 30) of the tunnel's axis and within its span (index side to pinky side, 20 mm
past each end) are the grip. Prints `GRIPBOX x0,x1,y0,y1` in mm (the hand frame, as grip= takes it) and how many
vertices it holds."""
import bpy, math, os, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
WEAP = args[0]
opt = dict(a.split('=', 1) for a in args[1:] if '=' in a)
LABEL = opt.get('pose', 'blaster rifle (fitted)')
REACH = float(opt.get('reach', 30)) / 1000
hand = bpy.data.objects['r_hand_retopo']
rig = hand.parent
w = bpy.data.objects[WEAP]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'body_poses.py')).read()
ns = {}
exec(src[src.index('FINGERS = '):src.index('scene = bpy.context.scene')], ns)
POSE = dict(next(p for g, ps in ns['GROUPS'] for l, p in ps if l.startswith(LABEL)))
for pb in rig.pose.bones:
    pb.rotation_mode = 'XYZ'
    pb.rotation_euler = (0, 0, 0)
for n, d in POSE.items():
    if n in rig.pose.bones:
        pb = rig.pose.bones[n]
        pb.rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
        pb.rotation_euler = [math.radians(a) for a in d]
bpy.context.view_layer.update()
M = rig.matrix_world
P = rig.pose.bones
# the tunnel: the middle of the curled middle, ring and pinky (their joints' centroid), its axis middle -> pinky
cent = {f: sum([M @ P[f + '_1'].head, M @ P[f + '_2'].head, M @ P[f + '_3'].head, M @ P[f + '_3'].tail], Vector()) / 4
        for f in ('index', 'middle', 'ring', 'pinky')}
axis = (cent['pinky'] - cent['index']).normalized()
origin = cent['middle']
span0 = (cent['index'] - origin).dot(axis) - 0.02
span1 = (cent['pinky'] - origin).dot(axis) + 0.02
pts = [w.matrix_world @ v.co for v in w.data.vertices]
grip = []
for p in pts:
    d = p - origin
    t = d.dot(axis)
    if span0 <= t <= span1 and (d - axis * t).length <= REACH:
        grip.append(p)
if not grip:
    print('GRIPBOX none: no vertex within %.0f mm of the tunnel' % (REACH * 1000))
else:
    x0, x1 = min(p.x for p in grip), max(p.x for p in grip)
    y0, y1 = min(p.y for p in grip), max(p.y for p in grip)
    print('GRIPBOX %.0f,%.0f,%.0f,%.0f (%d of %d vertices; tunnel axis %.2f %.2f %.2f)' % (
        x0 * 1000, x1 * 1000, y0 * 1000, y1 * 1000, len(grip), len(pts), axis.x, axis.y, axis.z))
