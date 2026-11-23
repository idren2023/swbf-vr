"""Blender (blender -b --factory-startup <hand.blend> -P tools/body_poses.py -- <out-prefix> [object] [xray | pies] [edges] [palmviews] [zoom=<vertex>] [zoomsize=<m>] [allverts] [dqs] [bones=thumb,..] [labels=thumb,..] [see] [wire] [with=<object> ..]
[pose=<label start> ..]
[bone=x,y,z ..]): the hand's pose
library, each pose from three sides (thumb side, palm three-quarter, back three-quarter), textured, one sheet per group
(<out-prefix>-<group>.png, a row per pose). Angles are degrees on top of the relaxed rest (x < 0 bends toward the
palm; thumb_1 z < 0 swings the thumb across the palm, z > 0 out; finger _1 z < 0 toward the pinky). Rebel trooper
kit: blaster rifle, blaster pistol, thermal detonators; vehicles: the mod's lever handles (trigger + thumb button),
the turret / bike bars; the controller's empty-hand states (Quest Touch: grip, trigger, thumb rest)."""
import bpy, math, os, sys
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
OBJ = next((a for a in args[1:] if a not in ('xray', 'pies', 'edges', 'palmviews', 'allverts', 'dqs', 'see', 'wire') and '=' not in a), 'r_hand_stock')
PIES = 'pies' in args[1:]  # xray, and each numbered vertex a pie chart of its bone weights
EDGES = 'edges' in args[1:]  # the mesh's edges as thin dark lines; with them the glove is drawn opaque (near side only)
SEE = 'see' in args[1:]
WIRE = 'wire' in args[1:]  # only the glove with its edges drawn (no bones, numbers or pies)  # with edges: the glove semi-transparent instead, the bones inside it where they are
EDGES = EDGES or WIRE
XRAY = 'xray' in args[1:] or PIES or EDGES  # bones, near-surface original vertices numbered (see-through unless edges)
PALM_VIEWS = 'palmviews' in args[1:]  # views aimed at the palm: straight on, the thumb's palm side, between
ZOOM = next((int(a[5:]) for a in args[1:] if a.startswith('zoom=')), None)  # close-up centred on this vertex
ZOOM_SIZE = next((float(a[9:]) for a in args[1:] if a.startswith('zoomsize=')), 0.07)  # close-up width (m)
HAND_ZOOM = next((float(a[9:]) for a in args[1:] if a.startswith('handzoom=')), None)  # close-up of the hand, this wide (m)
TILE = next((tuple(int(x) for x in a[5:].split('x')) for a in args[1:] if a.startswith('tile=')), None)  # tile size WxH
ALL_VERTS = 'allverts' in args[1:]  # number every vertex (split ones too), not only the stock's 197
DQS = 'dqs' in args[1:]
WITH = [a[5:] for a in args[1:] if a.startswith('with=')]  # other objects to show (e.g. weap_rifle)
BONES_ONLY = next((a[6:].split(',') for a in args[1:] if a.startswith('bones=')), None)  # draw only bones starting so
LABELS_ONLY = next((a[7:].split(',') for a in args[1:] if a.startswith('labels=')), None)  # number only vertices
# weighted at least 30 % to bones starting so  # dual-quaternion skinning (keeps volume at bent joints) instead of linear blending
# weapon=<pistol|rifle|thermaldetonator>: as the game holds it (BodyArm): the weapon (weap_<name>, placed by
# tools/body_placeweapon.py) shown with the forearm and cuff stud, its fitted pose, and the forearm swung about the wrist
# into line with the barrel, the hand kept on the grip (BodyArm's "wrist" step); the barrel from the same msh -> arm fit
# tools/body_export.py writes (the msh's +z)
WEAPON = next((a[7:] for a in args[1:] if a.startswith('weapon=')), None)
WEAPON_POSES = {'pistol': 'blaster pistol (fitted)', 'rifle': 'blaster rifle (fitted)',
                'thermaldetonator': 'thermal detonator (fitted)', 'ammocanister': 'ammo canister (fitted)',
                'sniperrifle': 'Rebel sniper rifle (stock-hold fit)', 'launcher': 'rocket launcher (fitted)',
                'imp_rifle': 'E-11 rifle (fitted)', 'imp_pistol': 'Imperial pistol (fitted)',
                'rep_pistol': 'DC-17 pistol (fitted)', 'rep_rifle': 'DC-15 rifle (fitted)',
                'imp_sniperrifle': 'Imperial sniper rifle (fitted)',
                'rep_sniperrifle': 'clone sniper rifle (fitted)', 'fusioncutter': 'fusion cutter (fitted)',
                'imp_fusioncutter': 'Imperial fusion cutter (fitted)', 'rep_fusioncutter': 'clone fusion cutter (fitted)',
                'imp_launcher': 'Imperial launcher (fitted)', 'rep_launcher': 'clone launcher (fitted)',
                'seeker': 'seeker (fitted)', 'com_grenadethermal': 'grenade (fitted)', 'item_landmine': 'landmine (fitted)',
                'item_timebomb': 'time bomb (fitted)', 'item_powerup': 'health / ammo dispenser (fitted)',
                'item_probedroid': 'recon droid (fitted)',
                'item_thermaldetonator': 'grenade (fitted)', 'item_imp_thermaldetonator': 'grenade (fitted)',
                'bowcaster': 'bowcaster (fitted)', 'com_torpedo': 'torpedo launcher (fitted)'}
if WEAPON:
    WITH += [n for n in ('weap_' + WEAPON, 'r_forearm_retopo', 'r_forearm_stud') if n not in WITH]
ONLY_POSES = [a[5:] for a in args[1:] if a.startswith('pose=')] or ([WEAPON_POSES[WEAPON]] if WEAPON else [])  # render only poses whose label starts with these
OVERRIDE = {a.split('=')[0]: tuple(float(x) for x in a.split('=')[1].split(',')) for a in args[1:]
            if '=' in a and not a.startswith(('pose=', 'zoom', 'bones=', 'labels=', 'with=', 'weapon=', 'handzoom=', 'tile=', 'check'))}  # bone=x,y,z on top of every rendered pose
FINGERS = ('index', 'middle', 'ring', 'pinky')


def wrap(mcp, pip, dip, fingers=FINGERS):
    return {f + s: (a, 0, 0) for f in fingers for s, a in (('_1', mcp), ('_2', pip), ('_3', dip))}


def pose(*parts, **bones):
    out = {}
    for p in parts:
        out.update(p)
    out.update(bones)
    return out


# thumb swings across the palm: 70 % of the base bone's turn kept, the rest moved into the middle bone (the base bone
# is long and starts near the wrist, so its whole turn tore the thumb pad at the palm's edge, idren2023); fitted so the
# tip ends within about 7 mm of the all-base versions
# fitted weapon grips: the thumb's two outer joints bent sideways (z -15..-40) so it curls toward the index / middle; with
# their bends alone it curled down toward the pinky (idren2023 2026-11-01); rolling thumb_1 instead (b464) twisted the thumb
# the thumb on idren2023's skeleton (2026-10-22, three bones, his rolls): every thumb pose from the stock artist's own grip
# (its angles on this skeleton are almost pure bends): wrap / pinch 1.0, thermal 0.9, fist 1.4, resting on the
# controller 0.6, lever 0.5, along a handle's side 0.3; thumbs-up and the throw release open it outward
THUMB_WRAP = {'thumb_1': (-4, 1, 18), 'thumb_2': (-16, 0, -1), 'thumb_3': (-20, 1, 7)}   # around a handle
THUMB_THERMAL = {'thumb_1': (-4, 1, 16), 'thumb_2': (-14, 0, -1), 'thumb_3': (-18, 1, 6)}
THUMB_PINCH = {'thumb_1': (-4, 1, 18), 'thumb_2': (-16, 0, -1), 'thumb_3': (-20, 1, 7)}
THUMB_FIST = {'thumb_1': (-6, 1, 25), 'thumb_2': (-22, 0, -1), 'thumb_3': (-28, 1, 10)}
THUMB_SIDE = {'thumb_1': (-1, 0, 5), 'thumb_2': (-5, 0, 0), 'thumb_3': (-6, 0, 2)}        # along a handle's side
THUMB_REST = {'thumb_1': (-2, 1, 11), 'thumb_2': (-10, 0, -1), 'thumb_3': (-12, 1, 4)}    # on the controller's face
# thumbs up: the thumb opened outward from the rest (z < 0: away from the palm on idren2023's rolls; first guess)
THUMB_UP = {'thumb_1': (5, 0, -10), 'thumb_2': (10, 0, 0), 'thumb_3': (5, 0, 0)}
TRIGGER_HOOK = {'index_1': (-30, 0, -20), 'index_2': (-45, 0, 0), 'index_3': (-20, 0, 0)}  # pad on a trigger, swung toward the others
TRIGGER_PULL = {'index_1': (-40, 0, -15), 'index_2': (-60, 0, 0), 'index_3': (-30, 0, 0)}
POINT_INDEX = {'index_1': (-14, 0, -20), 'index_2': (10.9, 0.1, -1.2), 'index_3': (5.9, 0.1, -1.1)}  # z < 0 on the knuckle: toward the other fingers; _2 / _3: in line with the bone before (idren2023), less a 3 / 2 deg curl

# the stock first-person model's own grip (every joint where the stock had it): the start of every weapon's pose
STOCK_GRIP = {'index_1': (-2, 0, -11), 'index_2': (-62, 1, 2), 'index_3': (-37, -3, -8), 'middle_1': (-54, 1, -1), 'middle_2': (-35, 0, 0), 'middle_3': (-42, 0, 1), 'ring_1': (-63, -7, 12), 'ring_2': (-32, -1, -3), 'ring_3': (-15, 0, 2), 'pinky_1': (-60, -11, 20), 'pinky_2': (-27, 1, 5), 'pinky_3': (-28, 0, -1), 'thumb_1': (-4, 1, 18), 'thumb_2': (-16, 0, -1), 'thumb_3': (-20, 1, 7)}
GROUPS = [
    ('primary', [  # the gun hand
        ('rifle / pistol grip, finger on the trigger guard', pose(wrap(-55, -70, -35, FINGERS[1:]), THUMB_WRAP,
                                                                   index_1=(-20, 0, -12), index_2=(-25, 0, 0), index_3=(-10, 0, 0))),
        ('rifle / pistol grip, trigger pulled', pose(wrap(-55, -70, -35, FINGERS[1:]), THUMB_WRAP, TRIGGER_PULL)),
        ('thermal detonator in the hand', pose(wrap(-35, -45, -25), THUMB_THERMAL, index_1=(-35, 0, -15))),
        ('pinch (magazine, grenade pin)', pose(wrap(-40, -55, -30, FINGERS[2:]), wrap(-25, -40, -20, ('middle',)),
                                               {'index_1': (-30, 0, -15), 'index_2': (-35, 0, 0), 'index_3': (-15, 0, 0)},
                                               THUMB_PINCH)),
    ]),
    ('offhand', [  # the support hand (drawn on the right rig; the left hand is its mirror)
        ('rifle foregrip, cupped under the barrel', pose(wrap(-35, -50, -25), THUMB_SIDE)),
        ('pistol support, wrapped over the gun hand', pose(wrap(-45, -55, -25), THUMB_SIDE)),
        ('thermal detonator: throw release (open)', pose(wrap(15, 10, 5), {'thumb_1': (8, 0, -12), 'thumb_2': (8, 0, 0), 'thumb_3': (5, 0, 0)},
                                                          # fingers splayed (z > 0 toward the thumb)
                                                          index_1=(15, 0, 10), middle_1=(15, 0, 2), ring_1=(15, 0, -6), pinky_1=(15, 0, -14))),
    ]),
    ('vehicle', [
        ('lever handle: grip, index on the trigger, thumb on the button', pose(wrap(-55, -70, -35, FINGERS[1:]), TRIGGER_HOOK,
                                                                          {'thumb_1': (-2, 0, 9), 'thumb_2': (-8, 0, 0), 'thumb_3': (-10, 0, 4)})),
        ('turret / bike bar: full wrap', pose(wrap(-60, -75, -40), THUMB_WRAP, index_1=(-60, 0, -20))),
        # 2026-11-07 (PLAN 61 j, bike hands): closed onto a 19 mm bar by tools/body_bikegrip.py fit (each finger curled
        # until it touches, the thumb along the bar's top); BodyArm DrawOnBar, BAR_AXES
        ('bike grip (fitted)', pose(index_1=(-35.5, 0, -20), index_2=(-50.2, 0, 0), index_3=(-37.6, 0, 0),
                                    middle_1=(-27.5, 0, 0), middle_2=(-40, 0, 0), middle_3=(-30, 0, 0),
                                    ring_1=(-25, 0, 0), ring_2=(-37.6, 0, 0), ring_3=(-28.4, 0, 0),
                                    pinky_1=(-23.5, 0, 0), pinky_2=(-35.2, 0, 0), pinky_3=(-27.6, 0, 0),
                                    thumb_1=(-4, 1, -16), thumb_2=(11, 0, -1), thumb_3=(9.5, 1, 0))),
    ]),
    # the droids' bike grips (2026-11-07, tools/body_bikegrip.py fit): the B1's controller fist and the super battle
    # droid's stock fist, the bar through the fist's tunnel (nothing had to open)
    ('b1 vehicle', [
        ('bike grip (fitted)', pose(trigger_1=(-65, 0, 0), trigger_2=(-35, 0, 0), grip_1=(-65, 0, 0), grip_2=(-35, 0, 0),
                                    thumb_1=(-20, 0, -10), thumb_2=(5, 0, 0))),
    ]),
    ('sbd vehicle', [
        ('bike grip (fitted)', pose()),
    ]),
    ('stock', [  # the stock first-person model's own grip: every joint back where the stock had it (0.0 mm, computed
                 # by aiming each bone from our rest skeleton to the stock grip's joints)
        ('stock grip', STOCK_GRIP),
    ]),
    ('fitted', [  # closed onto the rebel's weapons by contact (tools/body_placeweapon.py, tools/body_fitgrip.py: the grip in our
        # the pistol (2026-10-24): the hand 10 mm forward on the grip (the handle on the line from the thumb's web to the heel of the palm, idren2023), fingers wrapped 133-143 deg, the index through the guard bent evenly (middle joint -30, end joint -25)
        ('blaster pistol (fitted)', pose(index_1=(6, 0, -4), index_2=(-45, 0, 0), index_3=(-44, 0, 0), middle_1=(-48, 0, 0), middle_2=(-18, 0, 0), middle_3=(-13, 0, 0), ring_1=(-54, 0, -1), ring_2=(-6, 0, 0), ring_3=(-3, 0, 0), pinky_1=(-54, 0, 10), pinky_2=(0, 0, 0), pinky_3=(0, 0, 0), thumb_1=(-5, 0, 35), thumb_2=(-10, 0, -20), thumb_3=(-6, 0, -20))),
        # the rifle tuned (tools/body_tunegrip.py fingerwrap tipspress=5 thumbwrap, 2026-10-24): fingers wrapped round the grip, tips pressing in up to 5 mm out of view
        ('blaster rifle (fitted)', pose(index_1=(12, 0, -24), index_2=(-84, 0, 0), index_3=(-70, 0, 0), middle_1=(-54, 0, -6), middle_2=(-30, 0, 0), middle_3=(-26, 0, 0), ring_1=(-54, 0, -1), ring_2=(-30, 0, 0), ring_3=(-16, 0, 0), pinky_1=(-63, 0, 8), pinky_2=(-27, 0, 0), pinky_3=(-25, 0, 0), thumb_1=(-10, 0, 20), thumb_2=(-10, 0, -20), thumb_3=(-6, 0, -20))),
        # the manual reload's ammo canister (tools/body_canister.py at the game's place, the hand turned +15 / -10 deg on it; idren2023): all four fingers one curl (-10 / -40 / -28, the little finger swung 5 deg toward the ring), nothing through the outsides
        ('ammo canister (fitted)', pose(index_1=(-10, 0, 0), index_2=(-40, 0, 0), index_3=(-28, 0, 0), middle_1=(-10, 0, 0), middle_2=(-40, 0, 0), middle_3=(-28, 0, 0), ring_1=(-10, 0, 0), ring_2=(-40, 0, 0), ring_3=(-28, 0, 0), pinky_1=(-10, 0, -5), pinky_2=(-40, 0, 0), pinky_3=(-28, 0, 0), thumb_1=(-13, 1, 38), thumb_2=(-12, 0, -1), thumb_3=(-15, 1, 5))),
        # 2026-10-31 (every weapon's grip): the stock hold (tools/body_gripbox.py, body_placeweapon.py stock), the fingers
        # closed from the fitted rifle's (tools/body_fitgrip.py frompose)
        ('sniper rifle (fitted)', pose(index_1=(-3, 0, -14), index_2=(-81, 1, 3), index_3=(-48, -4, -10), middle_1=(-14, 0, 0), middle_2=(-8, 0, 0), middle_3=(-9, 0, 0), ring_1=(-44, -5, 8), ring_2=(-32, -1, -3), ring_3=(-15, 0, 2), pinky_1=(-32, -6, 11), pinky_2=(-35, 1, 6), pinky_3=(-37, 0, -1), thumb_1=(-24, 3, 40), thumb_2=(-18, 0, -2), thumb_3=(-22, 2, 8))),
        ('rocket launcher (fitted)', pose(index_1=(9, 0, -30), index_2=(-36, 0, 0), index_3=(-34, 0, 0), middle_1=(-39, 0, -8), middle_2=(-45, 0, 0), middle_3=(-22, 0, 0), ring_1=(-51, 0, -3), ring_2=(-18, 0, 0), ring_3=(-6, 0, 0), pinky_1=(-54, 0, 16), pinky_2=(-30, 0, 0), pinky_3=(-16, 0, 0), thumb_1=(-10, 0, 25), thumb_2=(-25, 0, -35), thumb_3=(-8, 0, -15))),
        # the Imperial E-11 (2026-10-31): every side's arms hold weapons at the same hp_weapons on the same wrist, but the
        # E-11's raked grip sits 18 mm nearer the wrist than the Rebel rifle's: slid forward by that (body_shiftweapon.py
        # 0 18 0), the barrel line kept, then tools/body_tunegrip.py from the fitted rifle (grip=-35,76,43,128)
        ('E-11 rifle (fitted)', pose(index_1=(12, 0, -18), index_2=(-75, 0, 0), index_3=(-46, 0, 0), middle_1=(-42, 0, -6), middle_2=(-36, 0, 0), middle_3=(-31, 0, 0), ring_1=(-60, 0, -1), ring_2=(-12, 0, 0), ring_3=(-7, 0, 0), pinky_1=(-66, 0, 16), pinky_2=(-12, 0, 0), pinky_3=(-10, 0, 0), thumb_1=(-20, 0, 55), thumb_2=(-20, 0, -35), thumb_3=(-20, 0, -35))),
        # 2026-10-31: the same way (body_shiftweapon.py by the y between their grip's axis and the Rebel's, then body_tunegrip.py
        # from the fitted pistol / rifle)
        ('Imperial pistol (fitted)', STOCK_GRIP),  # imp_pistol: the stock hold (idren2023 2026-11-08; was the E-11 fingers, b463)
        ('DC-17 pistol (fitted)', pose(index_1=(9, 0, 2), index_2=(-39, 0, 0), index_3=(-24, 0, 0), middle_1=(-57, 0, -6), middle_2=(-21, 0, 0), middle_3=(-21, 0, 0), ring_1=(-60, 0, -1), ring_2=(-12, 0, 0), ring_3=(-7, 0, 0), pinky_1=(-69, 0, 4), pinky_2=(-3, 0, 0), pinky_3=(-3, 0, 0), thumb_1=(-5, 0, 25), thumb_2=(-20, 0, -30), thumb_3=(-8, 0, -15))),
        ('DC-15 rifle (fitted)', pose(index_1=(3, 0, 12), index_2=(-30, 0, 0), index_3=(-21, 0, 0), middle_1=(-33, 0, -6), middle_2=(-27, 0, 0), middle_3=(-12, 0, 0), ring_1=(-45, 0, 1), ring_2=(-9, 0, 0), ring_3=(-3, 0, 0), pinky_1=(-15, 0, 4), pinky_2=(-15, 0, 0), pinky_3=(-25, 0, 0), thumb_1=(5, 0, 45), thumb_2=(-20, 0, -40), thumb_3=(-8, 0, -20))),
        ('Imperial sniper rifle (fitted)', pose(index_1=(12, 0, -20), index_2=(-48, 0, 0), index_3=(-44, 0, 0), middle_1=(-50, 0, 6), middle_2=(-40, 0, 0), middle_3=(-20, 0, 0), ring_1=(-45, 0, 6), ring_2=(-30, 0, 0), ring_3=(-15, 0, 0), pinky_1=(-40, 0, 8), pinky_2=(-42, 0, 0), pinky_3=(-23, 0, 0), thumb_1=(-15, 0, 50), thumb_2=(-25, 0, -35), thumb_3=(-20, 0, -35))),
        # 2026-10-31 later: the snipers and fusion cutters the same way; then (idren2023: every weapon its own pose) every
        # other side's weapon refitted by tools/body_tunegrip.py keep fingerwrap tipspress=5 thumbwrap indexring=<its guard>
        ('Rebel sniper rifle (stock-hold fit)', pose(index_1=(12, 0, -28), index_2=(-42, 0, 0), index_3=(-39, 0, 0), middle_1=(-50, 0, 6), middle_2=(-40, 0, 0), middle_3=(-20, 0, 0), ring_1=(-45, 0, 8), ring_2=(-36, 0, 0), ring_3=(-18, 0, 0), pinky_1=(-40, 0, 10), pinky_2=(-45, 0, 0), pinky_3=(-22, 0, 0), thumb_1=(-15, 0, 50), thumb_2=(-25, 0, -35), thumb_3=(-20, 0, -35))),
        ('clone sniper rifle (fitted)', pose(index_1=(12, 0, 12), index_2=(-30, 0, 0), index_3=(-28, 0, 0), middle_1=(-18, 0, -6), middle_2=(-48, 0, 0), middle_3=(-26, 0, 0), ring_1=(-30, 0, -1), ring_2=(-30, 0, 0), ring_3=(-21, 0, 0), pinky_1=(-24, 0, 4), pinky_2=(-48, 0, 0), pinky_3=(-26, 0, 0), thumb_1=(-15, 0, 30), thumb_2=(-20, 0, -25), thumb_3=(-8, 0, -25))),
        ('fusion cutter (fitted)', pose(index_1=(-24, 0, -20), index_2=(-60, 0, 0), index_3=(-33, 0, 0), middle_1=(-30, 0, -8), middle_2=(-30, 0, 0), middle_3=(-20, 0, 0), ring_1=(-60, 0, -1), ring_2=(-12, 0, 0), ring_3=(-7, 0, 0), pinky_1=(-60, 0, 4), pinky_2=(-12, 0, 0), pinky_3=(-8, 0, 0), thumb_1=(0, 0, 25), thumb_2=(-20, 0, -30), thumb_3=(-5, 0, -15))),
        # the Imperial and clone launchers at the stock hold (as the Rebel's), tuned from the fitted launcher
        ('Imperial launcher (fitted)', pose(index_1=(-20, 0, -6), index_2=(-70, 0, 0), index_3=(-45, 0, 0), middle_1=(-50, 0, 4), middle_2=(-35, 0, 0), middle_3=(-20, 0, 0), ring_1=(-54, 0, 8), ring_2=(-18, 0, 0), ring_3=(-13, 0, 0), pinky_1=(-60, 0, 14), pinky_2=(-12, 0, 0), pinky_3=(-7, 0, 0), thumb_1=(-20, 0, 55), thumb_2=(-20, 0, -30), thumb_3=(-8, 0, -20))),
        ('clone launcher (fitted)', pose(index_1=(-40, 0, -12), index_2=(-30, 0, 0), index_3=(-20, 0, 0), middle_1=(-45, 0, -4), middle_2=(-25, 0, 0), middle_3=(-15, 0, 0), ring_1=(-55, 0, 5), ring_2=(-15, 0, 0), ring_3=(-8, 0, 0), pinky_1=(-60, 0, 10), pinky_2=(-15, 0, 0), pinky_3=(-8, 0, 0), thumb_1=(-20, 0, 60), thumb_2=(-10, 0, -25), thumb_3=(-6, 0, -25))),
        ('Imperial fusion cutter (fitted)', pose(index_1=(-24, 0, -20), index_2=(-60, 0, 0), index_3=(-33, 0, 0), middle_1=(-30, 0, -8), middle_2=(-30, 0, 0), middle_3=(-20, 0, 0), ring_1=(-60, 0, -1), ring_2=(-12, 0, 0), ring_3=(-7, 0, 0), pinky_1=(-60, 0, 4), pinky_2=(-12, 0, 0), pinky_3=(-8, 0, 0), thumb_1=(0, 0, 25), thumb_2=(-20, 0, -30), thumb_3=(-5, 0, -15))),
        ('clone fusion cutter (fitted)', pose(index_1=(-24, 0, -20), index_2=(-60, 0, 0), index_3=(-33, 0, 0), middle_1=(-30, 0, -8), middle_2=(-30, 0, 0), middle_3=(-20, 0, 0), ring_1=(-60, 0, -1), ring_2=(-12, 0, 0), ring_3=(-7, 0, 0), pinky_1=(-60, 0, 4), pinky_2=(-12, 0, 0), pinky_3=(-8, 0, 0), thumb_1=(0, 0, 25), thumb_2=(-20, 0, -30), thumb_3=(-5, 0, -15))),
        # secondary items (2026-11-01): the shotgun's seeker model, the grenade every side draws (com_1st_weap_inf_grenadethermal),
        # and the items Holster draws in the hand itself (weap_item_*, at its sizes)
        ('seeker (fitted)', pose(index_1=(0, 0, -16), index_2=(-18, 0, 0), index_3=(-10, 0, 0), middle_1=(-15, 0, 0), middle_2=(-45, 0, 0), middle_3=(-32, 0, 0), ring_1=(-6, 0, 10), ring_2=(-18, 0, 0), ring_3=(-15, 0, 0), pinky_1=(-30, 0, 15), pinky_2=(-18, 0, 0), pinky_3=(-18, 0, 0), thumb_1=(-10, 0, 30), thumb_2=(0, 0, 0), thumb_3=(0, 0, 0))),
        ('bowcaster (fitted)', pose(index_1=(12, 0, -16), index_2=(-54, 0, 0), index_3=(-54, 0, 0), middle_1=(-9, 0, -4), middle_2=(-33, 0, 0), middle_3=(-24, 0, 0), ring_1=(-12, 0, 1), ring_2=(-33, 0, 0), ring_3=(-16, 0, 0), pinky_1=(-3, 0, 2), pinky_2=(-33, 0, 0), pinky_3=(-16, 0, 0), thumb_1=(-25, 0, 60), thumb_2=(-10, 0, -25), thumb_3=(-6, 0, -25))),
        ('torpedo launcher (fitted)', pose(index_1=(12, 0, -24), index_2=(-84, 0, 0), index_3=(-58, 0, 0), middle_1=(-42, 0, -6), middle_2=(-42, 0, 0), middle_3=(-29, 0, 0), ring_1=(-54, 0, -1), ring_2=(-24, 0, 0), ring_3=(-17, 0, 0), pinky_1=(-60, 0, 4), pinky_2=(-30, 0, 0), pinky_3=(-26, 0, 0), thumb_1=(-15, 0, 40), thumb_2=(-20, 0, -25), thumb_3=(-20, 0, -25))),
        ('grenade (fitted)', pose(index_1=(-20, 0, -11), index_2=(-45, 0, 0), index_3=(-31, 0, 0), middle_1=(-20, 0, 0), middle_2=(-35, 0, 0), middle_3=(-24, 0, 0), ring_1=(-15, 0, 0), ring_2=(-30, 0, 0), ring_3=(-21, 0, 0), pinky_1=(-55, 0, 0), pinky_2=(-60, 0, 0), pinky_3=(-42, 0, 0), thumb_1=(10, 0, 5), thumb_2=(-20, 0, 0), thumb_3=(-16, 0, 0))),
        ('landmine (fitted)', pose(index_1=(-78, 1, -2), index_2=(14, 0, -1), index_3=(8, 0, -1), middle_1=(-71, 1, -2), middle_2=(16, 0, 0), middle_3=(10, 0, 0), ring_1=(-67, -2, 3), ring_2=(24, 0, 0), ring_3=(12, 0, 0), pinky_1=(-59, -5, 11), pinky_2=(28, 0, 0), pinky_3=(14, 0, 2), thumb_1=(-10, -38, 56), thumb_2=(14, 0, -6), thumb_3=(10, 0, 0))),
        ('time bomb (fitted)', pose(index_1=(-35, 0, -8), index_2=(-45, 0, 0), index_3=(-30, 0, 0), middle_1=(-35, 0, 0), middle_2=(-45, 0, 0), middle_3=(-30, 0, 0), ring_1=(-35, 0, 0), ring_2=(-45, 0, 0), ring_3=(-30, 0, 0), pinky_1=(-35, 0, -5), pinky_2=(-45, 0, 0), pinky_3=(-30, 0, 0), thumb_1=(0, 0, 40), thumb_2=(-20, 0, -20), thumb_3=(-16, 0, -20))),
        ('health / ammo dispenser (fitted)', pose(index_1=(-10, 0, -5), index_2=(-40, 0, 0), index_3=(-28, 0, 0), middle_1=(-10, 0, 0), middle_2=(-40, 0, 0), middle_3=(-28, 0, 0), ring_1=(-15, 0, 0), ring_2=(-45, 0, 0), ring_3=(-31, 0, 0), pinky_1=(-10, 0, -5), pinky_2=(-40, 0, 0), pinky_3=(-28, 0, 0), thumb_1=(-13, 1, 38), thumb_2=(-12, 0, -15), thumb_3=(-15, 1, -15))),
        ('recon droid (fitted)', pose(index_1=(-10, 0, -16), index_2=(-12, 0, 0), index_3=(-8, 0, 0), middle_1=(-10, 0, -4), middle_2=(-10, 0, 0), middle_3=(-6, 0, 0), ring_1=(-10, 0, 8), ring_2=(-12, 0, 0), ring_3=(-8, 0, 0), pinky_1=(-4, 0, 16), pinky_2=(-6, 0, 0), pinky_3=(-4, 0, 0), thumb_1=(15, 0, 10), thumb_2=(0, 0, 0), thumb_3=(0, 0, 0))),
        ('thermal detonator (fitted)', pose(index_1=(-26, 0, -11), index_2=(-34, 0, 0), index_3=(-19, 0, 0),
                                            middle_1=(-16, 0, 0), middle_2=(-20, 0, 0), middle_3=(-11, 0, 0),
                                            ring_1=(-11, 0, 0), ring_2=(-14, 0, 0), ring_3=(-8, 0, 0),
                                            pinky_1=(-10, 0, 0), pinky_2=(-13, 0, 0), pinky_3=(-7, 0, 0),
                                            thumb_1=(-2, 1, 9), thumb_2=(-8, 0, -1), thumb_3=(-10, 1, 3))),
    ]),
    ('controller', [  # empty hand from the Touch controller's buttons and touch sensors
        ('nothing held', {}),
        ('grip only', pose(wrap(-55, -70, -35, FINGERS[1:]), THUMB_REST)),
        ('trigger only', pose(TRIGGER_PULL, THUMB_REST)),
        ('fist (grip + trigger, thumb on the face)', pose(wrap(-80, -95, -60), THUMB_FIST, index_1=(-80, 0, -15))),
        ('point (grip, trigger untouched)', pose(wrap(-80, -95, -60, FINGERS[1:]), THUMB_FIST, POINT_INDEX)),
        ('thumbs up (grip + trigger, thumb off)', pose(wrap(-80, -95, -60), THUMB_UP, index_1=(-80, 0, -15))),
    ]),
    # the B1 battle droid's own skeleton (tools/body_b1.py; PLAN 60 step 4): trigger_1-2, grip_1-2, thumb_1-2, its rest
    # the stock closed hand; groups named 'b1 <group>' pose its bones (tools/body_export.py, BodyArm FindSkeletonPose)
    ('b1 fitted', [  # the B1's grips (idren2023 2026-11-02; placements re/body/b1-stock-tweaks.json, tools/body_b1.py stock=1)
        ('E-5 rifle (stock hold)', {}),  # cis_rifle: the stock pose (the rest)
        ('CIS pistol (stock hold)', {}),  # cis_pistol: the stock pose (the rest)
        ('CIS sniper rifle (fitted)', pose(trigger_1=(-6, 0, 0), trigger_2=(59, 0, 0), grip_1=(-50, 0, 0), grip_2=(-12, 0, 0), thumb_1=(-27, 0, 0), thumb_2=(28, 0, 0))),  # cis_sniperrifle
        ('CIS rocket launcher (stock hold)', {}),  # cis_launcher: the stock pose (the rest)
        ('CIS fusion cutter (stock hold)', {}),  # cis_fusioncutter: the stock pose (the rest)
        ('torpedo launcher (fitted)', pose(trigger_1=(-60, 0, 0), grip_1=(-60, 0, 0), thumb_1=(-45, 0, 41), thumb_2=(26, 0, 0))),  # com_torpedo
        ('grenade (fitted)', pose(grip_1=(-31, 0, 0), grip_2=(41, 0, 0), thumb_1=(-5, 0, 30), thumb_2=(90, 0, 0), trigger_1=(-33, 0, 0), trigger_2=(41, 0, 0))),  # com_grenadethermal
        ('landmine (fitted)', pose(trigger_1=(-74, 0, 0), trigger_2=(79, 0, 0), grip_1=(-78, 0, 0), grip_2=(95, 0, 0), thumb_1=(-44, 0, 0), thumb_2=(-68, 0, 0))),  # item_landmine
        ('health / ammo dispenser (fitted)', pose(thumb_2=(-116, 0, 0))),  # item_powerup
        ('recon droid (fitted)', pose(grip_1=(-111, 0, 0), grip_2=(-87, 0, 0), thumb_1=(-41, 0, 1), thumb_2=(-58, 0, 0), trigger_1=(-19, 0, 0), trigger_2=(-74, 0, 0))),  # item_probedroid
    ]),
    # the B1's empty hand from the controller (idren2023 2026-11-02: gestures as human as the droid hand allows; labels as
    # the human ones, BodyArm ControllerPose): the open thumb along the human's relaxed thumb, the fist's thumb across
    # the front of the curled fingers, thumbs up out to the side; every pose checked free of the thumb clipping
    # the super battle droid (tools/body_sbd.py, bones grip_1-2: the finger slab, thumb_1-3; rest = the stock fist):
    # grip_1 + opens the slab, grip_2 + straightens its tip, thumb_1 x + swings the thumb out from under the palm
    # (renders 2026-11-04)
    ('sbd controller', [
        ('nothing held', pose(grip_1=(80, 0, 0), grip_2=(60, 0, 0), thumb_1=(35, 0, 0))),
        ('thumb rest', pose(grip_1=(80, 0, 0), grip_2=(60, 0, 0))),
        ('grip only', pose(thumb_1=(30, 0, 0))),
        ('trigger only', pose(grip_1=(35, 0, 0), grip_2=(30, 0, 0))),
        ('fist (grip + trigger, thumb on the face)', pose()),
        ('point (grip, trigger untouched)', pose(grip_1=(85, 0, 0), grip_2=(60, 0, 0))),
        ('thumbs up (grip + trigger, thumb off)', pose(thumb_1=(70, 0, 0), thumb_2=(20, 0, 0))),
    ]),
    ('b1 controller', [
        ('nothing held', pose(trigger_1=(5, 0, 0), trigger_2=(65, 0, 0), grip_2=(75, 0, 0), thumb_1=(40, -30, 10),
                              thumb_2=(-83, 0, 0))),
        ('thumb rest', pose(trigger_1=(5, 0, 0), trigger_2=(65, 0, 0), grip_2=(75, 0, 0), thumb_1=(10, 15, 30),
                            thumb_2=(-43, 0, 0))),
        ('grip only', pose(trigger_1=(5, 0, 0), trigger_2=(65, 0, 0), grip_1=(-65, 0, 0), grip_2=(-35, 0, 0),
                           thumb_1=(10, 15, 30), thumb_2=(-43, 0, 0))),
        ('trigger only', pose(trigger_1=(-45, 0, 0), trigger_2=(-10, 0, 0), grip_2=(75, 0, 0), thumb_1=(10, 15, 30),
                              thumb_2=(-43, 0, 0))),
        ('fist (grip + trigger, thumb on the face)', pose(trigger_1=(-65, 0, 0), trigger_2=(-35, 0, 0),
                                                         grip_1=(-65, 0, 0), grip_2=(-35, 0, 0), thumb_1=(-20, 0, -10),
                                                         thumb_2=(5, 0, 0))),
        ('point (grip, trigger untouched)', pose(trigger_2=(92, 0, 0), grip_1=(-65, 0, 0), grip_2=(-35, 0, 0),
                                                 thumb_1=(-20, 0, -10), thumb_2=(5, 0, 0))),
        ('thumbs up (grip + trigger, thumb off)', pose(trigger_1=(-65, 0, 0), trigger_2=(-35, 0, 0),
                                                       grip_1=(-65, 0, 0), grip_2=(-35, 0, 0), thumb_1=(30, -60, 60),
                                                       thumb_2=(-75, 0, 0))),
    ]),
]

scene = bpy.context.scene
obj = bpy.data.objects[OBJ]
rig = obj.parent
for m_ in obj.modifiers:
    if m_.type == 'ARMATURE':
        m_.use_deform_preserve_volume = DQS
for o in scene.objects:
    o.hide_render = o is not obj and o.name not in WITH
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
TW, TH = TILE or ((1000, 900) if PALM_VIEWS else ((700, 640) if XRAY else (420, 380)))
if XRAY:
    scene.display.shading.show_xray = SEE or not EDGES
    scene.display.shading.xray_alpha = 0.55
    from mathutils.bvhtree import BVHTree
    import bmesh
    BONE_COL = {'hand': (0.85, 0.85, 0.85, 1), 'index': (0.95, 0.2, 0.2, 1), 'middle': (0.2, 0.85, 0.2, 1),
                'ring': (0.25, 0.45, 1, 1), 'pinky': (1, 0.85, 0.1, 1), 'thumb': (0.9, 0.3, 0.95, 1)}
    MATS = {}
    for k_, c_ in BONE_COL.items():
        m_ = bpy.data.materials.new('bone_' + k_)
        m_.diffuse_color = c_
        MATS[k_] = m_
    EDGE_MAT = bpy.data.materials.new('edge')
    EDGE_MAT.diffuse_color = (1.0, 0.85, 0.2, 1) if WIRE else (0.02, 0.02, 0.02, 1)  # bright lines on the wire sheets
    DOT = bpy.data.materials.new('dot')
    DOT.diffuse_color = (1, 0, 0, 1)
    LBL = bpy.data.materials.new('lbl')
    LBL.diffuse_color = (1, 1, 0.35, 1)
    N_ORIG = 197  # the stock's vertices (the hand-set table's numbers); split vertices come after them
    # one colour a bone (pies, rods and the key): the hand grey, each finger its own hue family, base dark to tip light
    BONE_RGB = {'hand': (0.55, 0.55, 0.55),
                'index_1': (0.75, 0.0, 0.0), 'index_2': (1.0, 0.45, 0.0), 'index_3': (1.0, 0.9, 0.0),
                'middle_1': (0.0, 0.45, 0.0), 'middle_2': (0.15, 0.85, 0.15), 'middle_3': (0.7, 1.0, 0.35),
                'ring_1': (0.0, 0.1, 0.6), 'ring_2': (0.1, 0.45, 1.0), 'ring_3': (0.35, 0.9, 1.0),
                'pinky_1': (0.4, 0.2, 0.05), 'pinky_2': (0.75, 0.5, 0.25), 'pinky_3': (1.0, 0.6, 0.8),
                'thumb_1': (0.4, 0.0, 0.55), 'thumb_2': (0.85, 0.15, 0.85), 'thumb_3': (0.75, 0.65, 1.0),
                'forearm': (0.25, 0.25, 0.25), 'forearm_twist': (0.4, 0.3, 0.3), 'wrist_twist': (0.6, 0.45, 0.4)}

    def bone_colour(name):
        return BONE_RGB[name] + (1,)

    PIE_MATS = {}
    for b_ in [b.name for b in bpy.data.objects[OBJ].parent.data.bones]:
        m_ = bpy.data.materials.new('pie_' + b_)
        m_.diffuse_color = bone_colour(b_)
        PIE_MATS[b_] = m_
    GROUP_NAMES = {g.index: g.name for g in bpy.data.objects[OBJ].vertex_groups}
scene.render.resolution_x, scene.render.resolution_y = TW, TH
cd = bpy.data.cameras.new('poses')
cd.type = 'ORTHO'
cam = bpy.data.objects.new('poses', cd)
scene.collection.objects.link(cam)
scene.camera = cam
VIEWS = (('thumb side', Vector((-1, 0.1, 0.25)), Vector((0, 0, 1))),
         ('palm 3/4', Vector((-0.5, 0.3, -0.8)), Vector((0, 1, 0))),
         ('back 3/4', Vector((-0.4, -0.3, 0.85)), Vector((0, 1, 0))))
if WEAPON:
    # as held: also from behind the weapon, where the player's eyes are (idren2023 2026-10-24)
    VIEWS = VIEWS + (('behind, left', Vector((-0.15, -1, -0.35)), Vector((-1, 0, 0))),
                     ('behind, right', Vector((-0.15, -1, 0.8)), Vector((-1, 0, 0))),
                ('behind, top down', Vector((-1, -0.45, 0.1)), Vector((0, 1, 0))))
if PALM_VIEWS:
    VIEWS = (('palm', Vector((0, 0.05, -1)), Vector((0, 1, 0))),
             ("thumb's palm side", Vector((-0.75, 0.1, -0.65)), Vector((0, 1, 0))),
             ('palm, from the pinky side', Vector((0.5, 0.05, -0.85)), Vector((0, 1, 0))))


def weapon_forward(name):
    """The weapon's barrel in arm space as the game's arm takes it: the msh's +z through the msh -> arm fit."""
    import mshgeo
    import numpy as np
    w = bpy.data.objects['weap_' + name]
    side, short = (name[:3], name[4:]) if len(name) > 4 and name[3] == '_' else ('all', name)  # imp_rifle: another side's
    folder = {'all': 'ALL', 'imp': 'IMP', 'rep': 'REP', 'cis': 'CIS'}[side]
    model = mshgeo.load('C:/LucasArts/BFBuilder/Assets/Sides/%s/MSH/%s_1st_weap_inf_%s.msh' % (folder, side, short))
    raw = []
    for node in model['nodes']:
        if not mshgeo.is_drawn(node) or 'lens' in node['name'].lower():
            continue
        for seg in node['segments']:
            if not seg.get('uv') or not seg.get('texture') or seg['texture'].endswith('.pic'):
                continue
            raw += [tuple(q) for q in seg['wpos']]
    placed = [w.matrix_world @ v.co for v in w.data.vertices]
    A = np.linalg.lstsq(np.array([[*r, 1.0] for r in raw]), np.array([list(q) for q in placed]), rcond=None)[0]
    # the barrel's own axis in model space where it is not +z (Aim.cpp BARREL_CORRECTIONS, BodyArm GRIP_BARRELS)
    barrel = {'pistol': (0.0778, 0.0, 0.9970)}.get(name, (0.0, 0.0, 1.0))
    return Vector([sum(barrel[k] * A[k][c] for k in range(3)) for c in range(3)]).normalized()


FORWARD = None
if WEAPON:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    # thrown items keep the wrist as fitted (BodyArm: no barrel to line the forearm up with)
    FORWARD = (Vector((0, 1, 0)) if WEAPON in ('thermaldetonator', 'ammocanister', 'com_grenadethermal') or
               WEAPON.startswith('item_') else weapon_forward(WEAPON))
    for pb in rig.pose.bones:
        for c in pb.constraints:
            c.mute = True  # the game's twist bones take no share of a hand turned only to stay on the grip


def straighten_wrist():
    """BodyArm's wrist step: the forearm turned about the wrist (arm space origin) so it runs from the elbow along
    FORWARD; the hand put back where it was (its rest), the fingers with it."""
    if 'forearm' not in rig.pose.bones:
        return
    fpb, hpb = rig.pose.bones['forearm'], rig.pose.bones['hand']
    along = (-fpb.bone.head_local).normalized()
    turn = along.rotation_difference(FORWARD).to_matrix().to_4x4()
    fpb.matrix = turn @ fpb.matrix
    bpy.context.view_layer.update()
    hpb.matrix = hpb.bone.matrix_local.copy()
    bpy.context.view_layer.update()


def set_pose(p):
    for pb in rig.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
        pb.location = (0, 0, 0)  # the wrist step moves the forearm's head
    for n, d in dict(p, **OVERRIDE).items():
        if n in rig.pose.bones:  # a two-bone thumb has no thumb_2
            pb = rig.pose.bones[n]
            # a finger's knuckle: the sideways swing first, then the curl in the swung frame (Blender 'ZXY'); with the
            # curl first, a swing on a curled finger twisted it off its neighbours' line (fist index 13 deg -> 6)
            pb.rotation_mode = 'ZXY' if n.endswith('_1') and not n.startswith('thumb') else 'XYZ'
            pb.rotation_euler = [math.radians(x) for x in d]
    bpy.context.view_layer.update()
    if FORWARD is not None:
        straighten_wrist()
    if os.environ.get('LINECHECK'):
        for n in ('index_1', 'index_2', 'index_3'):
            pb = rig.pose.bones[n]
            y = pb.matrix.to_3x3().col[1].normalized()
            print('LINE', n, 'y', tuple(round(c, 3) for c in y), 'angle to parent y %.1f deg' % math.degrees(
                y.angle(pb.parent.matrix.to_3x3().col[1].normalized())))


CHECK = 'check' in args[1:] or 'checkonly' in args[1:]


def finger_check():
    """Per finger bone: the glove's vertices (mostly that bone's) inside the held weapon, the deepest (mm), and the
    gap from the bone's tip to the weapon's surface (mm): clipping and fingers in the air, without a picture."""
    import bmesh
    from mathutils.bvhtree import BVHTree
    dg = bpy.context.evaluated_depsgraph_get()
    wo = bpy.data.objects['weap_' + WEAPON]
    bm = bmesh.new()
    bm.from_object(wo, dg)
    bm.transform(wo.matrix_world)
    tree = BVHTree.FromBMesh(bm)
    me_ = obj.evaluated_get(dg).to_mesh()
    names = {g.index: g.name for g in obj.vertex_groups}
    out = []
    for f in ('index', 'middle', 'ring', 'pinky', 'thumb'):
        for k in (1, 2, 3):
            b = '%s_%d' % (f, k)
            n, deep = 0, 0.0
            for v in obj.data.vertices:
                wsum = sum(g.weight for g in v.groups if names.get(g.group) == b)
                if wsum < 0.5:
                    continue
                q = obj.matrix_world @ me_.vertices[v.index].co
                loc, nrm, _, d = tree.find_nearest(q)
                if loc is not None and (q - loc).dot(nrm) < 0:
                    n += 1
                    deep = max(deep, d)
            pb = rig.pose.bones[b]
            tip = rig.matrix_world @ pb.tail
            loc, nrm, _, d = tree.find_nearest(tip)
            out.append('%s in %d/%.0f gap %.0f' % (b, n, deep * 1000, d * 1000 if (tip - loc).dot(nrm) >= 0 else -d * 1000))
    print('CHECK ' + '; '.join(out))
    # the thumb's aim: its end bone's direction against the line from its end joint to the index / middle gap (the
    # middle of their middle joints), and how far it leans toward the pinky (the hand's x, mm per 100 mm of bone)
    hm = rig.pose.bones['hand'].matrix.inverted()
    t2, t3 = rig.pose.bones['thumb_3'].head, rig.pose.bones['thumb_3'].tail
    gap = (rig.pose.bones['index_2'].head + rig.pose.bones['middle_2'].head) / 2
    d, w = (hm.to_3x3() @ (t3 - t2)).normalized(), (hm.to_3x3() @ (gap - t2)).normalized()
    nail = (hm.to_3x3() @ rig.pose.bones['thumb_3'].matrix.to_3x3().col[2]).normalized()  # the end bone's back (its nail)
    print('THUMB aim %.0f deg off the index/middle gap; leans pinky %+.0f; tip %s mm (hand frame), gap %s; nail %s' % (
        math.degrees(d.angle(w)), d.x * 100, tuple(round(c * 1000) for c in hm @ t3), tuple(round(c * 1000) for c in hm @ gap),
        tuple(round(c, 2) for c in nail)))


tmp = os.path.abspath(OUT + '-tmp.png')


def render_label(text, w, h):
    """The pose's name as white text on a dark band, w x h pixels (a second scene, so the hand's settings stay)."""
    sc = bpy.data.scenes.get('label') or bpy.data.scenes.new('label')
    for o in list(sc.objects):
        bpy.data.objects.remove(o)
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.display.shading.light = 'FLAT'
    sc.display.shading.color_type = 'SINGLE'
    sc.display.shading.single_color = (0.95, 0.95, 0.95)
    sc.display.shading.background_type = 'VIEWPORT'
    sc.display.shading.background_color = (0.12, 0.12, 0.12)
    sc.render.resolution_x, sc.render.resolution_y = w, h
    cu = bpy.data.curves.new('lbl', 'FONT')
    cu.body = text
    cu.size = 1.0
    t = bpy.data.objects.new('lbl', cu)
    sc.collection.objects.link(t)
    cdl = bpy.data.cameras.new('lblcam')
    cdl.type = 'ORTHO'
    cdl.ortho_scale = w / h * 1.7 if w > h else 1.7
    cm = bpy.data.objects.new('lblcam', cdl)
    sc.collection.objects.link(cm)
    sc.camera = cm
    cm.location = (cdl.ortho_scale / 2 - 0.4, 0.3, 5)
    sc.render.filepath = tmp
    bpy.ops.render.render(write_still=True, scene=sc.name)
    im = bpy.data.images.load(tmp, check_existing=False)
    px = im.pixels[:]
    bpy.data.images.remove(im)
    return px


for group, poses in GROUPS:
    if group.startswith('b1 '):  # another skeleton's (tools/body_b1.py renders the B1's)
        continue
    if ONLY_POSES:
        poses = [(l, p) for l, p in poses if any(l.startswith(o) for o in ONLY_POSES)]
        if not poses:
            continue
    tiles = []
    for label, p in poses:
        set_pose(p)
        if CHECK and WEAPON:
            finger_check()
            if 'checkonly' in args[1:]:
                continue
        pts = [obj.matrix_world @ v.co for v in obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh().vertices]
        c = sum(pts, Vector()) / len(pts)
        cd.ortho_scale = max((q - c).length for q in pts) * 2.3
        if ZOOM is not None:
            c = pts[ZOOM]
            cd.ortho_scale = ZOOM_SIZE
        if HAND_ZOOM:
            rg = obj.parent
            c = rg.matrix_world @ rg.pose.bones['hand'].head.lerp(rg.pose.bones['middle_1'].head, 0.7)
            cd.ortho_scale = HAND_ZOOM
        for vname, d, up in VIEWS:
            d = d.normalized()
            up = (up - d * up.dot(d)).normalized()
            cam.matrix_world = Matrix.Translation(c + d * 2) @ Matrix((d.cross(up) * -1, up, d)).transposed().to_4x4()
            extras = []
            if XRAY:
                me_ = obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
                bm_ = bmesh.new()
                bm_.from_mesh(me_)
                bm_.transform(obj.matrix_world)
                bm_.normal_update()
                tree = BVHTree.FromBMesh(bm_)
                bm_.verts.ensure_lookup_table()
                for pb in [b for b in rig.pose.bones if not WIRE and (not BONES_ONLY or b.name.startswith(tuple(BONES_ONLY)))]:
                    h, t = rig.matrix_world @ pb.head, rig.matrix_world @ pb.tail
                    bpy.ops.mesh.primitive_cylinder_add(radius=0.0018, depth=(t - h).length, location=(h + t) / 2, vertices=8)
                    r_ = bpy.context.object
                    r_.rotation_euler = (t - h).to_track_quat('Z', 'Y').to_euler()
                    r_.data.materials.append(PIE_MATS[pb.name] if PIES else MATS[pb.name.split('_')[0]])
                    extras.append(r_)
                    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.003, location=h, segments=10, ring_count=6)
                    bpy.context.object.data.materials.append(PIE_MATS[pb.name] if PIES else MATS[pb.name.split('_')[0]])
                    extras.append(bpy.context.object)
                if EDGES:
                    wm = me_.copy()
                    wo = bpy.data.objects.new('wire', wm)
                    wo.matrix_world = obj.matrix_world.copy()
                    scene.collection.objects.link(wo)
                    wmod = wo.modifiers.new('w', 'WIREFRAME')
                    wmod.thickness = cd.ortho_scale * (0.0022 if WIRE else 0.0012)
                    wmod.use_replace = True
                    wmod.offset = 1.0
                    wm.materials.clear()
                    wm.materials.append(EDGE_MAT)
                    extras.append(wo)
                rot = cam.matrix_world.to_euler()
                for i_ in ([] if WIRE else range(len(bm_.verts) if ALL_VERTS else N_ORIG)):
                    v_ = bm_.verts[i_]
                    if ZOOM is not None and (v_.co - c).length > cd.ortho_scale * 0.7:
                        continue  # outside the close-up
                    if LABELS_ONLY and sum(g.weight for g in obj.data.vertices[i_].groups
                                           if GROUP_NAMES[g.group].startswith(tuple(LABELS_ONLY))) < 0.3:
                        continue
                    # visible: the first surface seen from the camera along this line is at this vertex (no test of
                    # the vertex normal: a vertex on a fold faces sideways yet shows)
                    hit = tree.ray_cast(v_.co + d * 0.5, -d)
                    if hit[0] is None or (hit[0] - v_.co).dot(d) > 0.0012:
                        continue  # something nearer the camera covers it
                    if PIES:
                        # a disc facing the camera, a slice per bone in proportion to its weight
                        ws = sorted(((GROUP_NAMES[g.group], g.weight) for g in obj.data.vertices[i_].groups
                                     if g.weight > 0.005), key=lambda x: x[0])
                        tot = sum(w for n, w in ws) or 1
                        R = cd.ortho_scale * (0.006 if ZOOM is not None else 0.008)
                        rx = d.cross(up).normalized()
                        verts_, faces_, mats_ = [v_.co + d * 0.02], [], []
                        a0 = 0.0
                        for n, w in ws:
                            steps = max(2, int(24 * w / tot))
                            first = len(verts_)
                            for k in range(steps + 1):
                                ang = 2 * math.pi * (a0 + (w / tot) * k / steps)
                                verts_.append(v_.co + d * 0.02 + (rx * math.sin(ang) + up * math.cos(ang)) * R)
                            for k in range(steps):
                                faces_.append((0, first + k, first + k + 1))
                                mats_.append(n)
                            a0 += w / tot
                        pm = bpy.data.meshes.new('pie')
                        pm.from_pydata(verts_, [], faces_)
                        names_ = sorted(set(mats_))
                        for n in names_:
                            pm.materials.append(PIE_MATS[n])
                        for f_, n in zip(pm.polygons, mats_):
                            f_.material_index = names_.index(n)
                        po = bpy.data.objects.new('pie', pm)
                        scene.collection.objects.link(po)
                        extras.append(po)
                    else:
                        bpy.ops.mesh.primitive_uv_sphere_add(radius=0.0009, location=v_.co, segments=6, ring_count=4)
                        bpy.context.object.data.materials.append(DOT)
                        extras.append(bpy.context.object)
                    cu = bpy.data.curves.new('l', 'FONT')
                    cu.body = str(i_)
                    cu.size = cd.ortho_scale * 0.022
                    cu.materials.append(LBL)
                    t_ = bpy.data.objects.new('l', cu)
                    t_.location = v_.co + d * 0.03 + (d.cross(up).normalized() * cd.ortho_scale * 0.009 if PIES else up * 0.001)
                    t_.rotation_euler = rot
                    scene.collection.objects.link(t_)
                    extras.append(t_)
                bm_.free()
            # drawn over the hand in a second pass: labels, pies, and with edges (opaque glove) the bones
            overlay = [e_ for e_ in extras if e_.type == 'FONT' or e_.name.startswith('pie')
                       or (EDGES and not SEE and e_.name.startswith(('Cylinder', 'Sphere')))] if (PIES or EDGES) else []
            for e_ in overlay:
                e_.hide_render = True
            scene.render.filepath = tmp
            bpy.ops.render.render(write_still=True)
            if overlay:
                # second pass: only the pies and their numbers, opaque, over the see-through hand
                im = bpy.data.images.load(tmp, check_existing=False)
                base_px = list(im.pixels[:])
                bpy.data.images.remove(im)
                hidden = [o for o in scene.objects if not o.hide_render and o not in overlay]
                for o in hidden:
                    o.hide_render = True
                for e_ in overlay:
                    e_.hide_render = False
                scene.display.shading.show_xray = False
                scene.render.film_transparent = True
                bpy.ops.render.render(write_still=True)
                im = bpy.data.images.load(tmp, check_existing=False)
                top = im.pixels[:]
                bpy.data.images.remove(im)
                for i_ in range(0, len(base_px), 4):
                    a_ = top[i_ + 3]
                    if a_ > 0:
                        for k in range(3):
                            base_px[i_ + k] = base_px[i_ + k] * (1 - a_) + top[i_ + k] * a_
                im = bpy.data.images.new('c', TW, TH)
                im.pixels = base_px
                im.filepath_raw = tmp
                im.file_format = 'PNG'
                im.save()
                bpy.data.images.remove(im)
                for o in hidden:
                    o.hide_render = False
                scene.display.shading.show_xray = SEE or not EDGES
                scene.render.film_transparent = False
            for e_ in extras:
                bpy.data.objects.remove(e_)
            im = bpy.data.images.load(tmp, check_existing=False)
            tiles.append(im.pixels[:])
            bpy.data.images.remove(im)
        print('POSE %-8s %s' % (group, label))
    cols, rows = len(VIEWS), len(poses)
    LH = 44  # a label band above each pose's row
    sheet = [0.0] * (TW * cols * (TH + LH) * rows * 4)
    for i, px in enumerate(tiles):
        cx, cy = i % cols, rows - 1 - i // cols
        for y in range(TH):
            o = ((cy * (TH + LH) + y) * TW * cols + cx * TW) * 4
            sheet[o:o + TW * 4] = px[y * TW * 4:(y + 1) * TW * 4]
    for r, (label, p) in enumerate(poses):
        lab = render_label('%d. %s' % (r + 1, label), TW * cols, LH)
        cy = rows - 1 - r
        for y in range(LH):
            o = ((cy * (TH + LH) + TH + y) * TW * cols) * 4
            sheet[o:o + TW * cols * 4] = lab[y * TW * cols * 4:(y + 1) * TW * cols * 4]
    if PIES:
        # the key: a swatch per bone in a strip under the sheet (names in the order of BONE_RGB, left to right)
        KH = 40
        key = [0.08] * (TW * cols * KH * 4)
        names_ = list(BONE_RGB)
        sw = TW * cols // len(names_)
        for j_, n in enumerate(names_):
            for y in range(6, KH - 6):
                for x in range(j_ * sw + 4, (j_ + 1) * sw - 4):
                    key[(y * TW * cols + x) * 4:(y * TW * cols + x) * 4 + 4] = list(BONE_RGB[n]) + [1]
        sheet = key + sheet
        rows_h = (TH + LH) * rows + KH
        print('KEY left to right:', ' '.join(names_))
    else:
        rows_h = (TH + LH) * rows
    im = bpy.data.images.new(group, TW * cols, rows_h)
    im.pixels = sheet
    im.filepath_raw = os.path.abspath('%s-%s%s.png' % (OUT, group, '-pies' if PIES else ('-xray' if XRAY else '')))
    im.file_format = 'PNG'
    im.save()
os.remove(tmp)
