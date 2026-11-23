"""Blender (blender -b --factory-startup re/body/r_arm_grips.blend -P tools/body_save.py -- [set=arm] [out=tools/data/models]):
saves our first-person arm models from the master blend as text into git (idren2023 2026-11-04: every model we made
rebuildable from git plus the installed game; tools/body_rebuild.py builds the blend back, tools/body_export.py then
writes re/body/BodyArm.inc from it as before).

Saved (tools/body_model_io.py, one json per object in <out>/<set>/, a manifest listing them in build order):
- the skeletons tools/body_export.py exports (the human rig: r_hand_retopo's parent; every armature with a `skeleton`
  property), their bones' rest frames and the current pose;
- every part it exports (r_hand_retopo, r_forearm_retopo, r_forearm_stud, forearm_*, hair_*, the meshes on another
  skeleton with a `texture` property): points, faces, uvs, attributes, weights, materials by image path;
- every weap_ grip object: the game weapon's msh and the map from its coordinates to the object's (the game's model is
  read again on rebuild, never saved), or, for a placement of ours (direct), the mesh;
- our grip files re/body/grip-*.json and re/body/b1-stock-tweaks.json, copied as they are.
Another set: set=player objects=player,r_body_packs on re/body/player_stock.blend (the player body,
tools/body_export_player.py). Run it after every change to the master blend that should be kept, then commit tools/data/models.
"""
import bpy
import glob
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import body_model_io as io  # noqa: E402

args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
opt = dict(a.split('=', 1) for a in args if '=' in a)
SET = opt.get('set', 'arm')
OUT = os.path.join(opt.get('out', os.path.join('tools', 'data', 'models')), SET)
os.makedirs(OUT, exist_ok=True)
for f in glob.glob(os.path.join(OUT, '*.json')) + glob.glob(os.path.join(OUT, 'grips', '*.json')):
    os.remove(f)

if 'objects' in opt:  # another set (set=player objects=player,r_body_packs): those meshes and their armatures
    names = opt['objects'].split(',')
    rigs = sorted({bpy.data.objects[n].parent for n in names if bpy.data.objects[n].parent}, key=lambda o: o.name)
    for ob in rigs:
        io.write(io.save_armature(ob), os.path.join(OUT, ob.name + '.json'))
    for n in names:
        io.write(io.save_mesh(bpy.data.objects[n]), os.path.join(OUT, n + '.json'))
    with open(os.path.join(OUT, 'manifest.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'set': SET, 'source': os.path.basename(bpy.data.filepath),
                   'objects': [{'name': o.name, 'kind': 'armature'} for o in rigs] +
                              [{'name': n, 'kind': 'mesh'} for n in names], 'files': []}, f, indent=1)
        f.write('\n')
    print('SAVE %s: %d skeletons, %d meshes' % (SET, len(rigs), len(names)))
    sys.exit(0)
human = bpy.data.objects['r_hand_retopo'].parent
rigs = [human] + sorted((o for o in bpy.data.objects if o.type == 'ARMATURE' and 'skeleton' in o and o is not human),
                        key=lambda o: o['skeleton'])
parts = ['r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud']
parts += sorted(o.name for o in bpy.data.objects if o.type == 'MESH' and o.name.startswith(('forearm_', 'hair_')))
parts += sorted(o.name for o in bpy.data.objects if o.type == 'MESH' and o.parent and o.parent.type == 'ARMATURE'
                and 'skeleton' in o.parent and 'texture' in o)
weapons = sorted(o.name for o in bpy.data.objects if o.name.startswith('weap_') and o.type == 'MESH')
order, kinds = [], {}
for ob in rigs:
    io.write(io.save_armature(ob), os.path.join(OUT, ob.name + '.json'))
    order.append(ob.name)
    kinds[ob.name] = 'armature'
for name in parts:
    io.write(io.save_mesh(bpy.data.objects[name]), os.path.join(OUT, name + '.json'))
    order.append(name)
    kinds[name] = 'mesh'
fits = []
for name in weapons:
    d = io.save_weapon(bpy.data.objects[name])
    kinds[name] = 'weapon' if 'weapon_msh' in d else 'mesh'
    if 'weapon_msh' in d:
        fits.append(d['fit_error_m'])
    io.write(d, os.path.join(OUT, name + '.json'))
    order.append(name)
os.makedirs(os.path.join(OUT, 'grips'), exist_ok=True)
copied = []
for f in sorted(glob.glob(os.path.join('re', 'body', 'grip-*.json'))) + [os.path.join('re', 'body', 'b1-stock-tweaks.json')]:
    if os.path.exists(f):
        shutil.copyfile(f, os.path.join(OUT, 'grips', os.path.basename(f)))
        copied.append(os.path.basename(f))
manifest = {'set': SET, 'source': os.path.basename(bpy.data.filepath),
            'objects': [{'name': n, 'kind': kinds[n]} for n in order], 'files': copied}
with open(os.path.join(OUT, 'manifest.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump(manifest, f, indent=1)
    f.write('\n')
size = sum(os.path.getsize(p) for p in glob.glob(os.path.join(OUT, '**', '*.json'), recursive=True))
print('SAVE %s: %d skeletons, %d parts, %d weapons (%d as msh + map, worst fit %.2g m), %d files copied, %.1f MB'
      % (SET, len(rigs), len(parts), len(weapons), len(fits), max(fits or [0]), len(copied), size / 1e6))
