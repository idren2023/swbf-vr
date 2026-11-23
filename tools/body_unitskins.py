"""Blender (blender -b --factory-startup re/body/r_arm_sleeve.blend -P tools/body_unitskins.py -- <out-dir> <unit>
[body_poses.py options ..]): our arm in one unit's textures as BodyArm's UNIT_SKINS dresses it in the game (the glove
image swapped for the unit's hands texture, the sleeve's and stud's for its arms texture; the Rebel trooper's sleeve is
the game's grey override, re/body/all_1st_inf_trooper_arms_grey.png), then tools/body_poses.py's sheets with the rest of
the arguments (2026-10-31, PLAN 60). <out-dir>/<unit>-<group>.png."""
import bpy, os, sys

args = sys.argv[sys.argv.index('--') + 1:]
OUT, UNIT, REST = args[0], args[1], args[2:]
A = 'C:/LucasArts/BFBuilder/Assets/Sides/'
HERE = os.path.dirname(os.path.abspath(__file__))
SKINS = {  # as src/BodyArm.cpp UNIT_SKINS
    'all_1st_trooper': ('ALL/MSH/all_1st_inf_trooper_hands.tga', os.path.join(HERE, '..', 're', 'body', 'all_1st_inf_trooper_arms_grey.png')),
    'all_1st_medic': ('ALL/MSH/all_1st_inf_vanguard_hands.tga', 'ALL/MSH/all_1st_inf_vanguard_arms.tga'),
    'all_1st_marksperson': ('ALL/MSH/all_1st_inf_marksperson_hands.tga', 'ALL/MSH/all_1st_inf_marksperson_arms.tga'),
    'all_1st_pilot': ('ALL/MSH/all_1st_inf_pilot_hands.tga', 'ALL/MSH/all_1st_inf_pilot_arms.tga'),
    'all_1st_hothtrooper': ('ALL/MSH/all_1st_hothtrooper_hands.tga', 'ALL/MSH/all_1st_hothtrooper_arms.tga'),
    'all_1st_hothmarksperson': ('ALL/MSH/all_1st_inf_hothmarksperson_hands.tga', 'ALL/MSH/all_1st_inf_hothmarksperson_arms.tga'),
    'imp_1st_pilot': ('IMP/MSH/imp_1st_inf_pilot_hands.tga', 'IMP/MSH/imp_1st_inf_pilot_arms.tga'),
    'imp_1st_scout': ('IMP/MSH/imp_1st_inf_scout_hands.tga', 'IMP/MSH/imp_1st_inf_scout_arms.tga'),
    'imp_1st_snowtrooper': ('IMP/MSH/imp_1st_hothtrooper_hands.tga', 'IMP/MSH/imp_1st_hothtrooper_arms.tga'),
    # armoured units (tools/body_armour.py: their own forearm_<unit> in place of our sleeve; pass with=forearm_<unit>)
    'imp_1st_stormtrooper': ('IMP/MSH/imp_1st_inf_stormtrooper_hands.tga', 'IMP/MSH/imp_1st_inf_stormtrooper_arms.tga'),
    'imp_1st_darktrooper': ('IMP/MSH/imp_1st_inf_darktrooper_hands.tga', 'IMP/MSH/imp_1st_inf_darktrooper_arms.tga'),
    'rep_1st_trooper': ('REP/MSH/rep_1st_inf_clonetrooper_hands.tga', 'REP/MSH/rep_1st_inf_clonetrooper_arms.tga'),
}
hands, arms = (p if os.path.isabs(p) else A + p for p in SKINS[UNIT])


def swap(obj_name, path):
    image = bpy.data.images.load(os.path.abspath(path), check_existing=True)
    for slot in bpy.data.objects[obj_name].material_slots:
        if slot.material and slot.material.use_nodes:
            for node in slot.material.node_tree.nodes:
                if node.type == 'TEX_IMAGE':
                    node.image = image


# each part its own copy of its materials, so the swap touches only it
for name in ('r_hand_retopo', 'r_forearm_retopo', 'r_forearm_stud'):
    o = bpy.data.objects[name]
    for slot in o.material_slots:
        if slot.material:
            slot.material = slot.material.copy()
swap('r_hand_retopo', hands)
swap('r_forearm_retopo', arms)
swap('r_forearm_stud', arms)
# tool=wristposes: tools/body_wristposes.py's sheets instead (the rest of the arguments its own)
tool = next((a[5:] for a in REST if a.startswith('tool=')), 'poses')
REST = [a for a in REST if not a.startswith('tool=')]
script = 'body_wristposes.py' if tool == 'wristposes' else 'body_poses.py'
sys.argv = [sys.argv[0], '--', os.path.join(OUT, UNIT)] + REST
exec(compile(open(os.path.join(HERE, script)).read(), script, 'exec'))
