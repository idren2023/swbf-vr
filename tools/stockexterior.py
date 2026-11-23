#!/usr/bin/env python3
"""The starfighters' own hulls around our cockpits (idren2023 2026-10-27: the cockpits accurate to the exterior layout):
each fighter's exterior model from the mod tools, moved into its cockpit's space (eye at the game's (0, 1.5, 1.5),
-z ahead; the exterior's +z is ahead, so x and z turn round), so the nose, wings, cannons, astromech and canopy frame
seen from the seat are the ship's own. The exterior's own cockpit inside (a tub and dash under the glass) is left out
by a box per ship, keeping faces that look away from the eye (the hull's outside).

    python tools/stockexterior.py [out.inc] [--obj DIR]

Reads the mod tools' msh files (not in the repository: the generated file goes to build/gen/, never into git) and
writes a C++ include for src/ShellDesigns.cpp: per ship a table of StockVertex (position, normal, texture position
(v turned top-down as tools/mirrorstock.py does),
texture 3 the exterior's texture, 4 its glass). Without the msh files it writes empty tables. --obj also writes
<DIR>/ext_<odf>.obj for tools/shelltour.py.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

ASSETS = 'C:/LucasArts/BFBuilder/Assets/Sides/'
COCKPIT_EYE = (0.0, 1.5, 1.5)
DROID_CAP_Y = 1.47  # the droid's base where its head sat (exterior space)

# odf, msh, eye in the exterior's space, the exterior's texture, its glass textures, the box of the exterior's own
# cockpit inside to leave out (exterior space, min and max, then 'all' to leave out even faces turned away from the
# eye; or ('sphere', centre, radius): everything in it), the C++
# table name.
# the AT-TE's main barrel (p_main_barrel and its muzzle), exterior space
ATTE_BARREL_BOX = ((-0.35, 7.05, 4.70), (0.35, 7.90, 14.10))
SHIPS = [
    ('all_fly_xwing', 'ALL/MSH/all_fly_xwing.msh', (0.0, 2.52, 1.30), 'all_fly_xwing.tga', ('all_fly_ywing_windscreen.tga',),
     ((-0.53, 1.20, 0.30), (0.53, 2.80, 2.80)), 'XWING'),
    ('imp_fly_tiefighter', 'IMP/MSH/imp_fly_tiefighter.msh', (0.0, 4.25, 0.00), 'imp_fly_tiefighter.tga', (),
     ('sphere', (0.0, 4.188, 0.0), 1.45), 'TIE'),
    ('rep_fly_jedifighter', 'REP/MSH/rep_fly_jedifighter.msh', (0.0, 1.72, -1.35), 'rep_fly_fighter.tga',
     ('rep_fly_fighter_canopy.tga',), ((-0.62, 0.30, -2.70), (0.62, 1.40, -0.20)), 'JEDI'),
    ('cis_fly_droidfighter', 'CIS/MSH/cis_fly_droidfighter.msh', (0.0, 2.55, 1.20), 'cis_fly_droidstarfighter.tga', (),
     ((0, 0, 0), (0, 0, 0)), 'DROID'),
    ('cis_fly_geofighter', 'CIS/MSH/cis_fly_geofighter.msh', (0.0, 2.95, -0.25), 'cis_fly_fighter.tga',
     ('cis_fly_fighter_canopy.tga',), ((-0.58, 1.50, -1.40), (0.58, 2.66, 0.80), 'all'), 'GEO'),
    ('all_fly_ywing', 'ALL/MSH/all_fly_ywing.msh', (0.0, 2.10, 5.15), 'all_fly_ywing.tga', ('all_fly_ywing_windscreen.tga',),
     ((-0.53, 1.00, 3.60), (0.53, 2.43, 7.05)), 'YWING'),
    ('imp_fly_tiebomber', 'IMP/MSH/imp_fly_tiebomber.msh', (-1.75, 2.74, 1.45), 'imp_fly_tiebomber.tga', (),
     ((-2.85, 1.55, -3.25), (-0.62, 3.85, 2.75), 'all'), 'TIEBOMBER'),  # the whole pilot pod: our ball replaces it
    ('all_fly_snowspeeder', 'ALL/MSH/all_fly_snowspeeder.msh', (0.0, 1.15, 0.25), 'all_fly_speeder.tga',
     ('all_fly_snowspeeder_windscreen.tga',), ((-0.56, 0.15, -1.66), (0.56, 1.40, 1.02)), 'SNOWSPEEDER'),
    # the AT-TE pilot's cab in its nose (idren2023 2026-11-04): his eye 1 m behind the amber window across the nose's front
    # (z 8.3, y 5.2..6.3), the cab's inside cut out with the window (ours is glazed)
    ('rep_walk_atte', 'REP/MSH/rep_walk_atte.msh', (0.0, 5.60, 7.30), 'Rep_walk_sixleg.tga', (),
     ((-0.80, 3.60, 3.00), (0.80, 6.70, 8.95), 'all'), 'ATTE_CAB'),
    # the AT-TE's main cannon gunner, open air on its back (hp_main_gunner (0, 7.1, 1.72); his eye 1.5 m over it, up
    # in the open hatch: lower, the stock seat's hood is 0.3 m before his eyes); the cannon and
    # his seat and visor left out of the hull (ATTE_CANNON) and kept as the turret's (ATTE_GUN), which turns with it
    ('rep_walk_atte', 'REP/MSH/rep_walk_atte.msh', (0.0, 8.60, 1.72), 'Rep_walk_sixleg.tga', (),
     ((-1.00, 6.70, 0.60), (1.00, 9.20, 13.30), 'all'), 'ATTE_CANNON'),
    ('rep_walk_atte', 'REP/MSH/rep_walk_atte.msh', (0.0, 8.60, 1.72), 'Rep_walk_sixleg.tga', (),
     ((-1.00, 6.70, 0.60), (1.00, 9.20, 13.30), 'only'), 'ATTE_GUN'),
    ('rep_walk_atte', 'REP/MSH/rep_walk_atte.msh', (0.0, 8.60, 1.72), 'Rep_walk_sixleg.tga', (),
     ATTE_BARREL_BOX + ('only',), 'ATTE_BARREL'),
    # the gunship's ball turrets (the balls themselves, their own texture, are left out: ours turn with the guns)
    ('rep_fly_gunship', 'REP/MSH/REP_LA_AT_Gunship.msh', (-2.80, 1.06, 2.45), 'REP_LA_AT.tga', (),
     ((0, 0, 0), (0, 0, 0)), 'GUNSHIP_BALL2'),
    ('rep_fly_gunship', 'REP/MSH/REP_LA_AT_Gunship.msh', (2.93, 1.06, 2.45), 'REP_LA_AT.tga', (),
     ((0, 0, 0), (0, 0, 0)), 'GUNSHIP_BALL3'),
    # the gunship's door passenger (TURRET4, PilotPosition hp_passenger1 (-0.971, 0.101, -0.186), "standing"): his eye
    # 1.65 m over the bay's floor at the starboard door; the exterior models the troop bay (floor, ceiling at y 3.39,
    # both doors open from z -2.5 to 1.8): nothing left out
    ('rep_fly_gunship', 'REP/MSH/REP_LA_AT_Gunship.msh', (-0.97, 1.75, -0.19), 'REP_LA_AT.tga', (),
     ((0, 0, 0), (0, 0, 0)), 'GUNSHIP_DOOR'),
]


# Hull faces inside an ellipsoid (exterior space: centre, radii) also left out: the Geonosian's own pilot and dash
# under its bubble (idren2023 2026-10-27: they stood in front of our dash).
BUBBLES = {'GEO': ((0.0, 2.67, -0.315), (0.55, 0.46, 0.99))}
# Hull faces with their middles in a box (exterior space: min, max) left out as well: the Jedi's own dash cowl and its
# dark panel, which stood up through our dash (idren2023, headset run 18).
CUT_BOXES = {'JEDI': [((-0.5, 1.22, -1.10), (0.5, 1.55, -0.70))],
             # the AT-TE gunner's stock visor (a targeting picture right before his eyes: he aims with his head), and
             # the barrel, its own table (it pitches about its trunnion: headset run 19)
             # the snowspeeder's own tub floor (it covered the stick's floor plate and the seat cushion), up to 12 cm
             # under the eye (the sill): its 'all' box had reached over the eye and took the canopy's frame and roof
             # (idren2023 2026-11-09: the canopy gone, the windows dark)
             'SNOWSPEEDER': [((-0.56, 0.15, -1.66), (0.56, 1.03, 1.02))],
             # the Y-wing's fuselage deck inside the canopy's back end (cockpit x +-0.40, y 1.40..1.56, z 2.50..2.95): it
             # stood at the gunner's chest height behind him (idren2023 2026-11-09: the gunner seat clipping)
             'YWING': [((-0.40, 2.00, 3.70), (0.40, 2.16, 4.15))],
             'ATTE_GUN': [((-0.5, 7.7, 1.85), (0.5, 8.5, 2.4)), ATTE_BARREL_BOX]}


def in_cut_box(piece, table):
    if table not in CUT_BOXES:
        return False
    c = [sum(corner[0][k] for corner in piece) / 3 for k in range(3)]
    return any(all(low[k] < c[k] < high[k] for k in range(3)) for low, high in CUT_BOXES[table])


def in_bubble(piece, table):
    if table not in BUBBLES:
        return False
    centre, radii = BUBBLES[table]
    c = [sum(corner[0][k] for corner in piece) / 3 for k in range(3)]
    # from 3 cm under the sill (headset run 19: pieces of the ship's own dash between the 'all' box's top, 1 cm under
    # the sill, and the sill stood in our well beside the eyes)
    return c[1] > centre[1] - 0.03 and sum(((c[k] - centre[k]) / radii[k]) ** 2 for k in range(3)) < 1


def to_cockpit(p, eye):
    return (-(p[0] - eye[0]) + COCKPIT_EYE[0], p[1] - eye[1] + COCKPIT_EYE[1], -(p[2] - eye[2]) + COCKPIT_EYE[2])


def pieces(corners, box, longest=0.15):
    """The triangle as it is, or, where it crosses the edge of the region left out, cut into pieces no longer than
    `longest` along that edge (their middles then decide), corners interpolated."""
    ps = [c[0] for c in corners]
    if box[0] == 'sphere':
        centre, radius = box[1], box[2]
        nearest = [min(max(centre[k], min(p[k] for p in ps)), max(p[k] for p in ps)) for k in range(3)]
        if sum((nearest[k] - centre[k]) ** 2 for k in range(3)) > radius * radius:
            return [corners]  # wholly outside
        if all(sum((p[k] - centre[k]) ** 2 for k in range(3)) < radius * radius for p in ps):
            return [corners]  # wholly inside
    else:
        low, high = box[0], box[1]
        if any(max(p[k] for p in ps) < low[k] or min(p[k] for p in ps) > high[k] for k in range(3)):
            return [corners]
        if all(low[k] < p[k] < high[k] for p in ps for k in range(3)):
            return [corners]
    def mid(a, b):
        return (tuple((a[0][k] + b[0][k]) / 2 for k in range(3)), tuple((a[1][k] + b[1][k]) / 2 for k in range(2)),
                tuple((a[2][k] + b[2][k]) / 2 for k in range(3)))
    edge = max(sum((ps[i][k] - ps[(i + 1) % 3][k]) ** 2 for k in range(3)) ** 0.5 for i in range(3))
    if edge <= longest:
        return [corners]
    a, b, c = corners
    ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
    out = []
    for t in ((a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)):
        out += pieces(list(t), box, longest)
    return out


def dropped(piece, box, eye, kind):
    """Whether a piece lies in the exterior's own cockpit: in the ball (sphere), or in the box facing the eye (by its
    model normals: the models' windings are not all the same way round)."""
    ps = [c[0] for c in piece]
    c = [sum(p[k] for p in ps) / 3 for k in range(3)]
    if box[0] == 'sphere':
        return sum((c[k] - box[1][k]) ** 2 for k in range(3)) < box[2] ** 2
    if kind != 3 or not all(box[0][k] < c[k] < box[1][k] for k in range(3)):
        return False
    if len(box) > 2 and box[2] == 'all':
        return True  # the droid's head: everything in it, whichever way it faces
    n = [sum(corner[2][k] for corner in piece) for k in range(3)]
    return sum(n[k] * (eye[k] - c[k]) for k in range(3)) > 0


# Nodes of a ship's model that are its cockpit's own inside, kept whole whatever the cut box leaves out and drawn in
# the design's cap finish (kind 5, mapped by real size): idren2023 2026-11-09, the Y-wing's canopy rebuilt to the game's
# own (its cockpit_interior node is the canopy's ceilings, the sloped sides' inner faces, the frame bands between the
# windows and the rear end plate; the cut box had taken it as the cockpit to clear, and the hull's outer pieces were
# left showing through the cabin).
KEPT_NODES = {'YWING': ['cockpit_interior']}


def ship_triangles(ship):
    """(positions, normals, uvs, kind) in cockpit space, kind 3 hull, 4 glass."""
    import mshgeo
    import modelsource
    odf, msh, eye, texture, glass, box, table = ship
    path = ASSETS + msh
    if not modelsource.exists(path):
        return []
    out = []
    model = modelsource.load(path)
    for node in model['nodes']:
        if not mshgeo.is_drawn(node):
            continue
        for s in node['segments']:
            name = s['texture'].lower()
            kind = 3 if name == texture.lower() else 4 if name in [g.lower() for g in glass] else 0
            if not kind or not s['wpos']:
                continue
            uv = s.get('uv') or [(0, 0)] * len(s['wpos'])
            nrm = s['wnrm'] if len(s.get('wnrm', [])) == len(s['wpos']) else None
            if kind == 3 and (node.get('name') or '').lower() in KEPT_NODES.get(table, []):
                for tri in s['tris']:
                    ps = [to_cockpit(s['wpos'][i], eye) for i in tri]
                    e1 = [ps[1][k] - ps[0][k] for k in range(3)]
                    e2 = [ps[2][k] - ps[0][k] for k in range(3)]
                    n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
                    ln = sum(x * x for x in n) ** 0.5 or 1
                    if abs(n[1]) / ln > 0.9 and max(p[1] for p in ps) < 1.60:
                        continue  # its floor (the game's crew sit in a shallow box): it crossed our cabin at chest height
                    out.append(([to_cockpit(s['wpos'][i], eye) for i in tri],
                                [(-(nrm[i][0]), nrm[i][1], -(nrm[i][2])) if nrm else (0.0, 1.0, 0.0) for i in tri],
                                [(0.0, 0.0)] * 3, 5))
                continue
            for tri in s['tris']:
                if table == 'DROID' and all(uv[i][0] > 0.88 and uv[i][1] < 0.50 for i in tri):
                    # the droid's head (idren2023 2026-10-27: it rises off its base; the eye is the head): a flat cap
                    # where it sat, kind 5 (ShellDesigns draws it in gunmetal)
                    ps = [(s['wpos'][i][0], DROID_CAP_Y, s['wpos'][i][2]) for i in tri]
                    out.append(([to_cockpit(p, eye) for p in ps], [(0.0, 1.0, 0.0)] * 3, [(0.0, 0.0)] * 3, 5))
                    continue
                corners = [(s['wpos'][i], (uv[i][0], 1 - uv[i][1]), nrm[i] if nrm else (0.0, 1.0, 0.0)) for i in tri]
                if len(box) > 2 and box[2] == 'only':
                    # a moving part's table (the AT-TE's cannon): only the triangles with their middles in the box
                    c = [sum(corner[0][k] for corner in corners) / 3 for k in range(3)]
                    if kind == 3 and all(box[0][k] < c[k] < box[1][k] for k in range(3)) and                             not in_cut_box([(corner[0],) for corner in corners], table):
                        out.append(([to_cockpit(corner[0], eye) for corner in corners],
                                    [(-corner[2][0], corner[2][1], -corner[2][2]) for corner in corners],
                                    [corner[1] for corner in corners], kind))
                    continue
                split = pieces(corners, box)
                kept = [piece for piece in split if not dropped(piece, box, eye, kind)
                        and not (kind == 3 and (in_bubble(piece, table) or in_cut_box(piece, table)))]
                for piece in [corners] if len(kept) == len(split) else kept:
                    ps = [c[0] for c in piece]
                    positions = [to_cockpit(p, eye) for p in ps]
                    normals = [(-c[2][0], c[2][1], -c[2][2]) for c in piece]
                    out.append((positions, normals, [c[1] for c in piece], kind))
    return out


def without_twins(tris, eye=(0.0, 1.5, 1.5)):
    """Leaves out triangles lying wholly on another of the same kind in the same plane (within 4 mm): the exteriors'
    back-to-back and doubled faces, which the shells draw without culling, so the two fought for depth (2026-11-11,
    the Jedi's hull beside the dash in shards). Of a pair, the one facing the eye is kept."""
    def sub(a, b):
        return [a[k] - b[k] for k in range(3)]

    def cross(a, b):
        return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]

    def dot(a, b):
        return sum(a[k] * b[k] for k in range(3))

    info = []
    for t in tris:
        ps = t[0]
        n = cross(sub(ps[1], ps[0]), sub(ps[2], ps[0]))
        length = dot(n, n) ** 0.5
        c = [sum(p[k] for p in ps) / 3 for k in range(3)]
        n = [x / length for x in n] if length > 1e-9 else None
        facing = n is not None and dot(n, sub(eye, c)) > 0
        info.append((t, n, c, facing))
    order = sorted(range(len(info)), key=lambda i: not info[i][3])
    cells = {}
    kept = []

    def covers(a, b):
        ta, na = a[0], a[1]
        if na is None or b[1] is None or a[0][3] != b[0][3] or abs(dot(na, b[1])) < 0.995:
            return False
        for p in b[0][0]:
            if abs(dot(sub(p, ta[0][0]), na)) > 0.004:
                return False
            for k in range(3):
                e0, e1 = ta[0][k], ta[0][(k + 1) % 3]
                edge = sub(e1, e0)
                if dot(cross(edge, sub(p, e0)), na) < -0.004 * (dot(edge, edge) ** 0.5):
                    return False
        return True

    for i in order:
        cell = tuple(int(x // 0.5) for x in info[i][2])
        near = [j for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)
                for j in cells.get((cell[0] + dx, cell[1] + dy, cell[2] + dz), [])]
        if any(covers(info[j], info[i]) for j in near):
            continue
        cells.setdefault(cell, []).append(i)
        kept.append(i)
    return [info[i][0] for i in sorted(kept)]


def main():
    args = sys.argv[1:]
    obj_dir = None
    if '--obj' in args:
        i = args.index('--obj')
        obj_dir = args[i + 1]
        del args[i:i + 2]
    out = args[0] if args else 'build/gen/StarfighterExteriors.inc'
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'w') as f:
        f.write('// Generated by tools/stockexterior.py from the mod tools\' exterior models: not in git, do not edit.\n')
        for ship in SHIPS:
            tris = ship_triangles(ship)
            before = len(tris)
            tris = without_twins(tris)
            table = ship[6]
            f.write('constexpr int EXTERIOR_%s_COUNT = %d;\n' % (table, 3 * len(tris)))
            f.write('constexpr StockVertex EXTERIOR_%s[%d] = {\n' % (table, max(3 * len(tris), 1)))
            for positions, normals, uvs, kind in tris:
                for p, n, t in zip(positions, normals, uvs):
                    f.write('    {{%.4ff, %.4ff, %.4ff}, {%.4ff, %.4ff, %.4ff}, %.5ff, %.5ff, %d},\n' % (*p, *n, *t, kind))
            if not tris:
                f.write('    {{0, 0, 0}, {0, 1, 0}, 0, 0, 3},\n')
            f.write('};\n')
            print('%s: %d triangles (%d doubled left out)' % (ship[0], len(tris), before - len(tris)))
            if obj_dir:
                os.makedirs(obj_dir, exist_ok=True)
                # one file per table (a ship may have several: the gunship's balls, the AT-TE's cannon)
                with open(os.path.join(obj_dir, 'ext_%s.obj' % ship[6].lower()), 'w') as o:
                    n = 0
                    for positions, _, uvs, kind in tris:
                        o.write('usemtl %s\n' % (ship[3] if kind == 3 else '#glass'))
                        for p, t in zip(positions, uvs):
                            # v back to the model's own way up (the table's is turned top-down for the game;
                            # idren2023 2026-11-04: the previews were mistextured)
                            o.write('v %.4f %.4f %.4f\nvt %.5f %.5f\n' % (*p, t[0], 1 - t[1]))
                        o.write('f %d/%d %d/%d %d/%d\n' % (n + 1, n + 1, n + 2, n + 2, n + 3, n + 3))
                        n += 3


if __name__ == '__main__':
    main()
