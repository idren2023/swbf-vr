"""Blender (blender -b --factory-startup -P tools/body_rebuild.py -- <out.blend> [set=arm] [from=tools/data/models]
[grips=re/body]): builds the blend tools/body_save.py saved, from git and the installed game (idren2023 2026-11-04):
skeletons, our meshes, the weapons read again from the mod tools' msh files and set where they were, materials on the
images by path (the game's textures from its folders, ours regenerated first by tools/body_textures.sh). Copies the
saved grip files back to <grips> (re/body), where tools/body_export.py reads them. Then:

    blender -b --factory-startup <out.blend> -P tools/body_export.py -- re/body/BodyArm.inc
"""
import bpy
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import body_model_io as io  # noqa: E402

args = sys.argv[sys.argv.index('--') + 1:]
OUT = os.path.abspath(args[0])
opt = dict(a.split('=', 1) for a in args[1:] if '=' in a)
SRC = os.path.join(opt.get('from', os.path.join('tools', 'data', 'models')), opt.get('set', 'arm'))
GRIPS = opt.get('grips', os.path.join('re', 'body'))

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
manifest = json.load(open(os.path.join(SRC, 'manifest.json'), encoding='utf-8'))
data = {e['name']: json.load(open(os.path.join(SRC, e['name'] + '.json'), encoding='utf-8')) for e in manifest['objects']}
built = {}
for e in manifest['objects']:
    d = data[e['name']]
    built[e['name']] = {'armature': io.build_armature, 'mesh': io.build_mesh, 'weapon': io.build_weapon}[e['kind']](d)
for name, ob in built.items():
    io.place(ob, data[name])
missing = [m['image_path'] for d in data.values() for m in d.get('materials', []) if m and m.get('image_path')
           and not os.path.exists(m['image_path'])]
os.makedirs(GRIPS, exist_ok=True)
for f in manifest.get('files', []):
    shutil.copyfile(os.path.join(SRC, 'grips', f), os.path.join(GRIPS, f))
bpy.ops.wm.save_as_mainfile(filepath=OUT, compress=True)
print('REBUILD %s: %d objects, %d grip files -> %s; images not found: %d %s'
      % (manifest['set'], len(built), len(manifest.get('files', [])), OUT, len(set(missing)), sorted(set(missing))[:5]))
