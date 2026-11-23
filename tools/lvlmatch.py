"""Labels the triangles of the game's munged models with the .msh node they came from, so that our generators can
read the player's own .lvl files instead of the mod tools' .msh (docs/research/runtime-assets.md).

    python tools/lvlmatch.py            matches every model in MODELS, writes tools/lvllabels.json, prints a summary

The munger merges a model's static nodes into one segment per material, and drops the root's translation; our
generators select and cut by node name in the .msh's space. lvllabels.json holds, per model, that translation and
per segment the node name of each triangle (run-length coded). It is our own data: names and indices, no geometry.
Needs the mod tools (C:/LucasArts/BFBuilder) once, on the developer's PC; the player needs only the game.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lvlgeo  # noqa: E402
import mshgeo  # noqa: E402

ASSETS = 'C:/LucasArts/BFBuilder/Assets/Sides/'
LVL = 'C:/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront (Classic 2004)/GameData/Data/_lvl_pc/'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lvllabels.json')

# msh path under ASSETS -> (lvl path under _lvl_pc, model name in it)
MODELS = {
    'ALL/MSH/all_fly_xwing.msh': ('SIDE/all.lvl', 'all_fly_xwing'),
    'ALL/MSH/all_fly_ywing.msh': ('SIDE/all.lvl', 'all_fly_ywing'),
    'ALL/MSH/all_fly_snowspeeder.msh': ('SIDE/all.lvl', 'all_fly_snowspeeder'),
    'IMP/MSH/imp_fly_tiefighter.msh': ('SIDE/imp.lvl', 'imp_fly_tiefighter'),
    'IMP/MSH/imp_fly_tiebomber.msh': ('SIDE/imp.lvl', 'imp_fly_tiebomber'),
    'REP/MSH/rep_fly_jedifighter.msh': ('SIDE/rep.lvl', 'rep_fly_jedifighter'),
    'REP/MSH/REP_LA_AT_Gunship.msh': ('SIDE/rep.lvl', 'REP_LA_AT_Gunship'),
    'REP/MSH/rep_walk_atte.msh': ('SIDE/rep.lvl', 'rep_walk_atte'),
    'CIS/MSH/cis_fly_droidfighter.msh': ('SIDE/cis.lvl', 'cis_fly_droidfighter'),
    'CIS/MSH/cis_fly_geofighter.msh': ('SIDE/cis.lvl', 'cis_fly_geofighter'),
    'ALL/MSH/all_1st_cockpit_xwing.msh': ('FPM/ALL/allxwing.lvl', 'all_1st_cockpit_xwing'),
    'ALL/MSH/all_1st_cockpit_ywing.msh': ('FPM/ALL/allywing.lvl', 'all_1st_cockpit_ywing'),
    'ALL/MSH/all_1st_cockpit_snowspeeder.msh': ('FPM/ALL/allsspdr.lvl', 'all_1st_cockpit_snowspeeder'),
    'ALL/MSH/all_1st_cockpit_combatspeeder.msh': ('FPM/ALL/allcmspd.lvl', 'all_1st_cockpit_combatspeeder'),
    'IMP/MSH/imp_1st_cockpit_tiefighter.msh': ('FPM/IMP/imptiftr.lvl', 'imp_1st_cockpit_tiefighter'),
    'IMP/MSH/imp_1st_cockpit_tiebomber.msh': ('FPM/IMP/imptibmr.lvl', 'imp_1st_cockpit_tiebomber'),
    'IMP/MSH/imp_1st_cockpit_atat.msh': ('FPM/IMP/impatat.lvl', 'imp_1st_cockpit_atat'),
    'IMP/MSH/imp_1st_cockpit_ATST.msh': ('FPM/IMP/atst.lvl', 'imp_1st_cockpit_ATST'),
    'REP/MSH/rep_1st_cockpit_jedistarfighter.msh': ('FPM/REP/repjedfi.lvl', 'rep_1st_cockpit_jedistarfighter'),
    'REP/MSH/rep_1st_cockpit_gunship.msh': ('FPM/REP/repgunsh.lvl', 'rep_1st_cockpit_gunship'),
    'CIS/MSH/cis_1st_cockpit_starfighter.msh': ('FPM/CIS/cisstrft.lvl', 'cis_1st_cockpit_starfighter'),
}

TOL = 0.002  # metres: the munger's bone-space round trip moves vertices by a few micrometres


def stem(texture):
    return os.path.splitext(texture)[0].lower()


def cell(p):
    return tuple(int(round(v / 0.02)) for v in p)


def same_triangle(a, b):
    """Whether two triangles have the same three corners within TOL, in any order."""
    return all(any(all(abs(p[i] - q[i]) <= TOL for i in range(3)) for q in b) for p in a)


def msh_triangles(model):
    """(node name, texture, three positions, three uvs) of every drawn triangle."""
    for node in model['nodes']:
        if not mshgeo.is_drawn(node):
            continue
        for s in node['segments']:
            uv = s.get('uv') or [(0, 0)] * len(s['wpos'])
            for a, b, c in s['tris']:
                yield node['name'], s['texture'], (s['wpos'][a], s['wpos'][b], s['wpos'][c]), (uv[a], uv[b], uv[c])


def shift_candidates(mtris, lvl):
    """Translations that may take the lvl model onto the msh (the munger drops the root's translation): the
    differences of the two bounding boxes' minima, maxima and centres."""
    m = [p for _, _, tri, _ in mtris for p in tri]
    l_ = [p for s in lvl['segments'] for p in s['wpos']]
    lo = lambda P, i: min(p[i] for p in P)
    hi = lambda P, i: max(p[i] for p in P)
    out = [(0.0, 0.0, 0.0)]
    for f in (lambda P, i: lo(P, i), lambda P, i: hi(P, i), lambda P, i: (lo(P, i) + hi(P, i)) / 2):
        out.append(tuple(f(m, i) - f(l_, i) for i in range(3)))
    # per axis mixtures (one end of an axis can differ when a part is missing from one side)
    for i in range(1, 3):
        for j in range(1, 3):
            for k in range(1, 3):
                out.append((out[i][0], out[j][1], out[k][2]))
    return out


def match(msh_rel):
    lvl_rel, name = MODELS[msh_rel]
    msh = mshgeo.load(ASSETS + msh_rel)
    lvl = lvlgeo.load(LVL + lvl_rel, name)
    mtris = list(msh_triangles(msh))
    grid = {}
    for i, (node, tex, tri, _) in enumerate(mtris):
        centre = [sum(p[k] for p in tri) / 3 for k in range(3)]
        grid.setdefault((stem(tex),) + cell(centre), []).append(i)

    def hits(shift, sample):
        n = 0
        for s in lvl['segments']:
            for a, b, c in s['tris'][::sample]:
                tri = [[s['wpos'][v][k] + shift[k] for k in range(3)] for v in (a, b, c)]
                centre = cell([sum(p[k] for p in tri) / 3 for k in range(3)])
                n += any(same_triangle(tri, mtris[i][2]) for i in grid.get((stem(s['texture']),) + centre, []))
        return n

    shift = max(shift_candidates(mtris, lvl), key=lambda sh: hits(sh, 7))
    shift = tuple(round(v, 4) + 0.0 for v in shift)
    taken = [False] * len(mtris)
    labels, unmatched, total = [], 0, 0
    for s in lvl['segments']:
        runs = []
        for a, b, c in s['tris']:
            tri = [[s['wpos'][v][k] + shift[k] for k in range(3)] for v in (a, b, c)]
            centre = cell([sum(p[k] for p in tri) / 3 for k in range(3)])
            label, spare = '', None
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        for i in grid.get((stem(s['texture']), centre[0] + dx, centre[1] + dy, centre[2] + dz), []):
                            if same_triangle(tri, mtris[i][2]):
                                if not taken[i]:
                                    label = mtris[i][0]
                                    taken[i] = True
                                    break
                                spare = mtris[i][0]
                        if label:
                            break
                    if label:
                        break
                if label:
                    break
            label = label or spare or ''
            unmatched += not label
            total += 1
            if runs and runs[-1][0] == label:
                runs[-1][1] += 1
            else:
                runs.append([label, 1])
        labels.append(runs)
    missing = taken.count(False)
    return {'lvl': lvl_rel, 'model': name, 'shift': shift, 'segments': labels}, total, unmatched, missing, len(mtris)


def main():
    out, ok = {}, True
    for msh_rel in MODELS:
        if not os.path.exists(ASSETS + msh_rel):
            print('%-44s no msh' % msh_rel)
            ok = False
            continue
        entry, total, unmatched, missing, mcount = match(msh_rel)
        out[msh_rel] = entry
        print('%-44s shift %s  lvl %5d tris, %4d unlabelled; msh %5d, %4d not found' % (
            msh_rel, entry['shift'], total, unmatched, mcount, missing))
    with open(OUT, 'w') as f:
        json.dump(out, f, indent=0, sort_keys=True)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
