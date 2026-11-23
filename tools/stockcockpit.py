#!/usr/bin/env python3
"""Pieces of the game's own first-person cockpits fitted into our starfighter cockpits (idren2023 2026-10-27: the stock
canopies have better texture detail than the exteriors; scale them to our cockpits, which are at the player's size,
and use pieces of them to update or replace parts of the canopies and interiors). The stock first-person models are
built two to three times too big (the X-wing's canopy frame 3.2 m wide); each piece here is moved into our cockpit's
space (the eye at the game's (0, 1.5, 1.5), -z ahead, as the stock models' own) either by fitting its box onto a
target box per axis (a canopy onto the ship's own canopy) or by scaling it about the eye and moving it.

    python tools/stockcockpit.py [out.inc] [--obj DIR]

Reads the mod tools' msh files (not in the repository: the generated file goes to build/gen/, never into git) and
writes a C++ include for src/ShellDesigns.cpp: per ship a table of StockVertex (texture 1 the cockpit's texture, 2 its
effects texture, 4 its window glass; v turned top-down). Without the msh files it writes empty tables. --obj also
writes <DIR>/stock_<odf>.obj for tools/shelltour.py.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import modelsource  # noqa: E402

ASSETS = 'C:/LucasArts/BFBuilder/Assets/Sides/'
EYE = (0.0, 1.5, 1.5)

XWING_CANOPY = ['cockpitframe', 'cockpitwin1', 'cockpitwin2', 'cockpitwin3', 'wire1', 'wire2']
XWING_GLASS = ['cockpitwin1', 'cockpitwin2', 'cockpitwin3']
XWING_GAUGES = ['radar1', 'tach', 'amp', 'tubelong']

# Per ship: the C++ table name, the msh, and its pieces: (nodes, how[, cut]). how is ('fit', fit nodes, target min,
# target max): the fit nodes' box onto the target box, per axis; or ('scale', factor, offset): about the eye, then
# moved. cut (min, max, after the move): triangles wholly inside it are left out. (The droid's 'window' node is its
# opaque red visor pane: left out, the design puts its viewscreen there.)
SHIPS = [
    ('XWING', 'ALL/MSH/all_1st_cockpit_xwing.msh', [
        # (no canopy: the hull's own frame and glass match each other; the stock canopy fitted by its glass's box
        # left frames without panes and panes short of their frames, idren2023 2026-11-09)
        (XWING_GAUGES, ('scale', 0.45, (0.0, -0.02, 0.0))),
    ]),
    ('YWING', 'ALL/MSH/all_1st_cockpit_ywing.msh', [
        # (no canopy: the hull's own frame and glass match each other; the stock canopy fitted by its glass's box
        # left frames without panes and panes short of their frames, idren2023 2026-11-09)
        (XWING_GAUGES, ('scale', 0.45, (0.0, -0.04, 0.0))),
    ]),
    ('TIE', 'IMP/MSH/imp_1st_cockpit_tiefighter.msh', [
        (['dash', 'target', 'map', 'map1', 'dashfx', 'dashfx2', 'dashfx4', 'dashfx5', 'dashfx6', 'dashfx7', 'dashfx8'],
         ('scale', 0.55, (0.0, -0.45, 0.19))),
    ]),
    ('TIEBOMBER', 'IMP/MSH/imp_1st_cockpit_tiebomber.msh', [
        (['dash', 'target', 'dashfx', 'dashfx1'], ('scale', 0.55, (0.0, -0.45, 0.19))),
    ]),
    ('JEDI', 'REP/MSH/rep_1st_cockpit_jedistarfighter.msh', [
        # (no glass: the hull's own canopy glass matches its frame; the stock glass fitted by its box did not)
        (['display1', 'displayholder2', 'holder1', 'holder2', 'holder3'], ('fit', ['displayholder2'], (-0.11, 1.06, 0.83), (0.11, 1.36, 0.88))),
    ]),
    ('DROID', 'CIS/MSH/cis_1st_cockpit_starfighter.msh', [
        (['cis_1st_cockpit_starfighter1', 'mapball', 'circle1', 'circle2', 'lightfx1', 'lightfx2', 'lightfx3',
          'lightfx4', 'lightfx5'], ('scale', 0.30, (0.0, 0.0, 0.0))),
    ]),
    # The snowspeeder's dash (idren2023 2026-11-08: the pilot view rebuilt from the film and the game; the HUD's screens on
    # the dash): the stock three-part dash (radar left, cluster middle, buttons right) and its lamps, about the eye at
    # the stock models' scale to the player.
    ('SNOW', 'ALL/MSH/all_1st_cockpit_snowspeeder.msh', [
        (['leftside', 'rightside', 'dashcntr', 'cube3', 'cube4', 'cube7', 'cube10', 'cube13', 'cube16', 'cube17',
          'cube18', 'cube19', 'cube20'], ('scale', 0.45, (0.0, 0.0, 0.0))),
    ]),
    # The gunship pilot's cockpit drawn whole by us (idren2023 2026-11-11: the dash squashed, not fitting the sill): the
    # stock frame, glass and floor at their size; the dash and its instruments 1.12x about the dash's middle
    # (0, 0.93, -0.24), so its ports reach the frame's sides (x +-0.97 of +-0.98).
    ('GUNSHIP', 'REP/MSH/rep_1st_cockpit_gunship.msh', [
        (['rep_1st_cockpit_gunship', 'frame', 'glass'], ('scale', 1.0, (0.0, 0.0, 0.0))),
        (['scrolscrn1', 'upperdash', 'frontdial', 'knob2', 'knob1', 'screen', 'port2', 'port5', 'fx4', 'dashboard',
          'fx3', 'but4', 'cylinderdial', 'but1', 'but3', 'but2'], ('scale', 1.12, (-0.0000, 0.0684, 0.2088))),
    ]),
    # The walkers' cabs as one mirrored mesh (idren2023 2026-11-11: the pilot's and co-pilot's sides one complete mesh, the
    # AT-AT's front one glass panel): the pilot's model cut at the mirror plane x = M and mirrored onto the other side.
    # ('mirror', M, kept once (pieces across the plane drawn whole, not mirrored), spanned (a window quad: its far
    # edge replaced by the mirror of its near one), left out).
    ('ATAT', 'IMP/MSH/imp_1st_cockpit_atat.msh', [
        (['ATATdriver', 'centerconsolescreen', 'window', 'panel2', 'panel7', 'illumination14', 'panel9', 'panel8',
          'shiftcase', 'illumination10', 'illumination11', 'illumination13', 'panel11', 'panel10', 'windowframe',
          'shell', 'button1', 'headstringertop', 'illumination7', 'centerconsole', 'dashstringer'],
         ('mirror', 0.91, ['centerconsole', 'centerconsolescreen', 'illumination13', 'illumination14'], ['window'])),
    ]),
    # The AT-ST: the co-pilot on the pilot's right (idren2023 2026-11-11), the plane past the pilot's window (x 0.98) and
    # vent box (1.30); the stock mesh wall at the pilot's right ('walls') left out, the cab opens across.
    ('ATST', 'IMP/MSH/imp_1st_cockpit_ATST.msh', [
        (['imp_1st_cockpit_ATST1', 'longhose', 'shock', 'box3', 'box1', 'smltube2', 'smltube3', 'centerconsole1',
          'box4', 'smltube1', 'dash', 'windoor', 'fx1', 'centerconsole', 'fx2', 'fx3', 'ACvent'],
         ('mirror', 1.30, [], [])),
    ]),
]


def clip_x(points, xmin):
    """Sutherland-Hodgman against x = xmin, keeping x >= xmin; points: [(pos, normal, uv)]."""
    out = []
    for i, (p, n, t) in enumerate(points):
        q, m, u = points[(i + 1) % len(points)]
        if p[0] >= xmin:
            out.append((p, n, t))
        if (p[0] >= xmin) != (q[0] >= xmin):
            f = (xmin - p[0]) / (q[0] - p[0])
            out.append((tuple(p[k] + (q[k] - p[k]) * f for k in range(3)), n,
                        tuple(t[k] + (u[k] - t[k]) * f for k in range(2))))
    return out


def clip_below(points, xmax):
    """clip_x mirrored: keeps x <= xmax."""
    flipped = [((-p[0], p[1], p[2]), n, t) for p, n, t in points]
    return [((-p[0], p[1], p[2]), n, t) for p, n, t in clip_x(flipped, -xmax)]


def mirror_pieces(by_name, names, plane, once, span):
    """The walkers' cabs: each node cut at x = plane and mirrored about it (winding reversed), or kept whole once, or
    (a window quad) spanned across both sides."""
    import mshgeo
    out = []
    for name in names:
        node = by_name.get(name.lower())
        if not node or not mshgeo.is_drawn(node):
            continue
        for s in node['segments']:
            kind = kind_of(s['texture'])
            if not kind or not s['wpos']:
                continue
            uv = s.get('uv') or [(0, 0)] * len(s['wpos'])
            nrm = s['wnrm'] if len(s.get('wnrm', [])) == len(s['wpos']) else [(0.0, 1.0, 0.0)] * len(s['wpos'])
            pos = list(s['wpos'])
            if name in span:
                near = [p for p in pos if p[0] < plane]
                for i, p in enumerate(pos):
                    if p[0] > plane:
                        q = min(near, key=lambda q: (q[1] - p[1]) ** 2 + (q[2] - p[2]) ** 2)
                        pos[i] = (2 * plane - q[0], q[1], q[2])
            for tri in s['tris']:
                poly = [(tuple(pos[i]), tuple(nrm[i]), (uv[i][0], 1 - uv[i][1])) for i in tri]
                if name in once or name in span:
                    polys = [poly]
                else:
                    cut = clip_below(poly, plane)
                    mirrored = [((2 * plane - p[0], p[1], p[2]), (-n[0], n[1], n[2]), t) for p, n, t in reversed(cut)]
                    polys = [cut, mirrored]
                for poly in polys:
                    for k in range(1, len(poly) - 1):
                        a, b, c = poly[0], poly[k], poly[k + 1]
                        out.append(([a[0], b[0], c[0]], [a[1], b[1], c[1]], [a[2], b[2], c[2]], kind))
    return out


def kind_of(texture):
    t = texture.lower()
    if 'win' in t or 'canopy' in t or 'glass' in t:
        return 4
    if '_fx' in t:
        return 2
    return 1 if t else 0


def pieces(path, parts):
    import mshgeo
    import modelsource
    model = modelsource.load(path)
    by_name = {n.get('name', '').lower(): n for n in model['nodes']}
    out = []
    for part in parts:
        names, how = part[0], part[1]
        if how[0] == 'mirror':
            out += mirror_pieces(by_name, names, how[1], how[2], how[3])
            continue
        cut = part[2] if len(part) > 2 else None
        clips = part[3] if len(part) > 3 else {}
        if how[0] == 'fit':
            fit = [p for name in how[1] for s in by_name[name.lower()]['segments'] for p in s['wpos']]
            lo = [min(p[k] for p in fit) for k in range(3)]
            hi = [max(p[k] for p in fit) for k in range(3)]
            tlo, thi = how[2], how[3]
            # the stock models look along -z as ours do: their nearest point (largest z) maps to the target's largest
            def move(p, lo=lo, hi=hi, tlo=tlo, thi=thi):
                return tuple(tlo[k] + (p[k] - lo[k]) / ((hi[k] - lo[k]) or 1) * (thi[k] - tlo[k]) for k in range(3))
            scale = [(thi[k] - tlo[k]) / ((hi[k] - lo[k]) or 1) for k in range(3)]
        else:
            f, offset = how[1], how[2]
            def move(p, f=f, offset=offset):
                return tuple(EYE[k] + (p[k] - EYE[k]) * f + offset[k] for k in range(3))
            scale = [f, f, f]
        for name in names:
            node = by_name.get(name.lower())
            if not node or not mshgeo.is_drawn(node):
                continue
            for s in node['segments']:
                kind = kind_of(s['texture'])
                if not kind or not s['wpos']:
                    continue
                uv = s.get('uv') or [(0, 0)] * len(s['wpos'])
                nrm = s['wnrm'] if len(s.get('wnrm', [])) == len(s['wpos']) else None
                for tri in s['tris']:
                    ps = [move(s['wpos'][i]) for i in tri]
                    if cut and all(cut[0][k] < p[k] < cut[1][k] for p in ps for k in range(3)):
                        continue
                    if nrm:
                        # normals under a scale per axis: divided by it, made unit again
                        ns = []
                        for i in tri:
                            n = [nrm[i][k] / (scale[k] or 1) for k in range(3)]
                            length = sum(c * c for c in n) ** 0.5 or 1
                            ns.append(tuple(c / length for c in n))
                    else:
                        ns = [(0.0, 1.0, 0.0)] * 3
                    ts = [(uv[i][0], 1 - uv[i][1]) for i in tri]
                    if name in clips:
                        poly = clip_x(list(zip(ps, ns, ts)), clips[name])
                        for k in range(1, len(poly) - 1):
                            a, b, c = poly[0], poly[k], poly[k + 1]
                            out.append(([a[0], b[0], c[0]], [a[1], b[1], c[1]], [a[2], b[2], c[2]], kind))
                        continue
                    out.append((ps, ns, ts, kind))
    return out


def main():
    args = sys.argv[1:]
    obj_dir = None
    if '--obj' in args:
        i = args.index('--obj')
        obj_dir = args[i + 1]
        del args[i:i + 2]
    out = args[0] if args else 'build/gen/StarfighterStockCockpits.inc'
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'w') as f:
        f.write('// Generated by tools/stockcockpit.py from the mod tools\' first-person cockpits: not in git, do not edit.\n')
        for table, msh, parts in SHIPS:
            path = ASSETS + msh
            tris = pieces(path, parts) if modelsource.exists(path) else []
            f.write('constexpr int STOCK_%s_COUNT = %d;\n' % (table, 3 * len(tris)))
            f.write('constexpr StockVertex STOCK_%s[%d] = {\n' % (table, max(3 * len(tris), 1)))
            for positions, normals, uvs, kind in tris:
                for p, n, t in zip(positions, normals, uvs):
                    f.write('    {{%.4ff, %.4ff, %.4ff}, {%.4ff, %.4ff, %.4ff}, %.5ff, %.5ff, %d},\n' % (*p, *n, *t, kind))
            if not tris:
                f.write('    {{0, 0, 0}, {0, 1, 0}, 0, 0, 1},\n')
            f.write('};\n')
            print('%s: %d triangles' % (table, len(tris)))
            if obj_dir:
                os.makedirs(obj_dir, exist_ok=True)
                texture = {1: os.path.basename(msh)[:-4], 2: '#glow', 4: '#glass'}
                with open(os.path.join(obj_dir, 'stock_%s.obj' % table.lower()), 'w') as o:
                    n = 0
                    for positions, _, uvs, kind in tris:
                        o.write('usemtl %s\n' % texture[kind])
                        for p, t in zip(positions, uvs):
                            o.write('v %.4f %.4f %.4f\nvt %.5f %.5f\n' % (*p, *t))
                        o.write('f %d/%d %d/%d %d/%d\n' % (n + 1, n + 1, n + 2, n + 2, n + 3, n + 3))
                        n += 3


if __name__ == '__main__':
    main()
