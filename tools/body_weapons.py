"""Blender (blender -b --factory-startup <hand.blend> -P tools/body_weapons.py -- <out.blend> <msh-dir> <arms.msh>
<weapon> ..): the stock first-person weapons placed in our hand as the game holds them: each weapon model at the stock
arms' hp_weapons (on the right wrist), carried into our hand's frame with the build's stock -> hand transform
(re/body/hand-frame-<object>.json: hand frame, scale 0.712), parented to our hand bone; textured. Saved with the
hand as <out.blend>, objects named weap_<weapon>, hidden from renders until a pose sheet asks for one (with=)."""
import bpy, json, os, sys
from mathutils import Matrix, Vector
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mshgeo

args = sys.argv[sys.argv.index('--') + 1:]
OUT, MSH_DIR, ARMS = args[0], args[1], args[2]
WEAPONS = args[3:]
hand = bpy.data.objects['r_hand_stock']
rig = hand.parent
frame = json.load(open(os.path.join(os.path.dirname(os.path.abspath(OUT)), 'hand-frame-all_1st_trooper.json')))
to_hand, wrist, k = Matrix(frame['to_hand']), Vector(frame['wrist']), frame['scale']


def to_blender(p):
    return Vector((p[0], -p[2], p[1]))  # as tools/body_import.py


def H(p):
    return (to_hand @ (p - wrist)) * k


arms = mshgeo.load(ARMS)
hp = next(n for n in arms['nodes'] if n['name'].lower() == 'hp_weapons')['world']
SIDE_DIRS = {'all': 'ALL', 'wok': 'ALL', 'imp': 'IMP', 'rep': 'REP', 'cis': 'CIS'}


COMMON_DIR = 'C:/LucasArts/BFBuilder/Assets/Shipped Add On/DataTAT3/Common/MSHs'


def resolve(name):
    """A weapon argument -> (its msh folder, model name, object name): `rifle` is the Rebel's (weap_rifle); `imp:rifle`
    another side's (imp_1st_weap_inf_rifle, weap_imp_rifle; 2026-10-31, every side's grips); `com:grenadethermal` the
    common models' (com_1st_weap_inf_grenadethermal); `item_landmine=com_weap_inf_landmine_placed@0.6` a common model
    that Holster draws in the hand itself, at that size (weap_item_landmine; 2026-11-01)."""
    if '=' in name:
        short, rest = name.split('=')
        return COMMON_DIR, rest.split('@')[0], 'weap_' + short
    if name.startswith('com:'):
        short = name[4:].split('@')[0]
        return COMMON_DIR, 'com_1st_weap_inf_' + short, 'weap_com_' + short
    if ':' not in name:
        return MSH_DIR, 'all_1st_weap_inf_' + name, 'weap_' + name
    side, short = name.split(':')
    folder = MSH_DIR.replace('/ALL/', '/%s/' % SIDE_DIRS[side])
    return folder, '%s_1st_weap_inf_%s' % (side, short), 'weap_%s_%s' % (side, short)


for arg in WEAPONS:
    folder, model_name, object_name = resolve(arg)
    w = object_name[5:]
    scale = float(arg.split('@')[1]) if '@' in arg else 1.0  # Holster's ItemScale: drawn at scale x the msh
    model = mshgeo.load(os.path.join(folder, model_name + '.msh'))
    verts, faces, uvs, mats = [], [], [], []
    for node in model['nodes']:
        if not mshgeo.is_drawn(node) or 'lens' in node['name'].lower():
            continue
        for seg in node['segments']:
            if not seg.get('uv') or not seg.get('texture') or seg['texture'].endswith('.pic'):
                continue
            if seg['texture'] not in mats:
                mats.append(seg['texture'])
            base = len(verts)
            for p in seg['wpos']:
                verts.append(H(to_blender(mshgeo.apply([c * scale for c in p], hp))))
            for a, b, c in seg['tris']:
                faces.append((base + a, base + b, base + c))
                uvs.append(([seg['uv'][a], seg['uv'][b], seg['uv'][c]], mats.index(seg['texture'])))
    me = bpy.data.meshes.new('weap_' + w)
    me.from_pydata(verts, [], faces)
    uvl = me.uv_layers.new(name='UV')
    for poly, (tuv, mi) in zip(me.polygons, uvs):
        for j, li in enumerate(poly.loop_indices):
            uvl.data[li].uv = tuv[j]
        poly.material_index = mi
        poly.use_smooth = True
    for t in mats:
        m = bpy.data.materials.new(t)
        m.use_nodes = True
        img = m.node_tree.nodes.new('ShaderNodeTexImage')
        m.node_tree.nodes.active = img  # (textured views draw the active image node)
        path = os.path.join(folder, t)
        if os.path.exists(path):
            img.image = bpy.data.images.load(path)
        m.node_tree.links.new(img.outputs['Color'], m.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
        me.materials.append(m)
    ob = bpy.data.objects.new('weap_' + w, me)
    bpy.context.scene.collection.objects.link(ob)
    if folder == COMMON_DIR:  # for tools/body_export.py: the msh it is fitted over, and the size the game draws it at
        ob['msh'] = os.path.join(folder, model_name + '.msh').replace(os.sep, '/')
        ob['item_scale'] = scale
    # parented to the hand bone, keeping its place (the palm is rigid: the hand bone's rest frame)
    ob.parent = rig
    ob.parent_type = 'BONE'
    ob.parent_bone = 'hand'
    ob.matrix_world = Matrix.Identity(4)
    bb = [Vector(c) for c in ob.bound_box]
    print('WEAPON %s: %d tris, in the hand frame x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f mm' % (
        w, len(faces), *(v * 1000 for a in range(3) for v in (min(c[a] for c in verts), max(c[a] for c in verts)))))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
