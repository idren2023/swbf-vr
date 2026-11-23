"""Where the generators get the game's models: the mod tools' .msh (default, mshgeo) or, with SWBF_SOURCE=lvl, the
player's own .lvl files (lvlgeo) relabelled into .msh nodes by tools/lvllabels.json (tools/lvlmatch.py). Both give
mshgeo's shape: {'materials', 'nodes': [{'name', 'world', 'segments': [{'tris', 'wpos', 'wnrm', 'uv', 'texture'}]}]}.
"""
import json
import os

import lvlgeo
import mshgeo

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = 'C:/LucasArts/BFBuilder/Assets/Sides/'
LVL = os.environ.get('SWBF_LVL', 'C:/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront (Classic 2004)'
                     '/GameData/Data/_lvl_pc/')
_labels = None


def from_lvl():
    return os.environ.get('SWBF_SOURCE') == 'lvl'


def _rel(path):
    p = path.replace(chr(92), '/')
    return p[len(ASSETS):] if p.startswith(ASSETS) else p


def exists(path):
    if not from_lvl():
        return os.path.exists(path)
    return _rel(path) in labels()


def labels():
    global _labels
    if _labels is None:
        with open(os.path.join(HERE, 'lvllabels.json')) as f:
            _labels = json.load(f)
    return _labels


def load(path):
    if not from_lvl():
        return mshgeo.load(path)
    entry = labels()[_rel(path)]
    model = lvlgeo.load(LVL + entry['lvl'], entry['model'])
    shift = entry['shift']
    nodes, order = {}, []
    identity = ([[1.0, 0, 0], [0, 1.0, 0], [0, 0, 1.0]], [0.0, 0, 0])
    for seg, runs in zip(model['segments'], entry['segments']):
        texture = seg['texture'] + '.tga' if seg['texture'] else ''
        wpos = [[p[k] + shift[k] for k in range(3)] for p in seg['wpos']]
        uv = [(u, 1.0 - v) for u, v in seg['uv']]  # munged v runs the other way
        i = 0
        for label, count in runs:
            if label not in nodes:
                nodes[label] = {'name': label, 'world': identity, 'segments': []}
                order.append(label)
            node = nodes[label]
            # one segment per (node, lvl segment): the lvl segment's vertices, this run's triangles
            if not node['segments'] or node['segments'][-1]['src'] is not seg:
                node['segments'].append({'src': seg, 'tris': [], 'wpos': wpos, 'pos': wpos, 'wnrm': seg['wnrm'],
                                         'nrm': seg['wnrm'], 'uv': uv, 'texture': texture})
            node['segments'][-1]['tris'] += seg['tris'][i:i + count]
            i += count
    # each node's segments keep only the vertices their triangles use (callers take a node's box from its wpos)
    for node in nodes.values():
        for seg in node['segments']:
            src, remap = seg.pop('src'), {}
            for tri in seg['tris']:
                for v in tri:
                    remap.setdefault(v, len(remap))
            keep = sorted(remap, key=remap.get)
            seg['tris'] = [tuple(remap[v] for v in tri) for tri in seg['tris']]
            for field in ('wpos', 'wnrm', 'uv'):
                seg[field] = [seg[field][v] for v in keep]
            seg['pos'], seg['nrm'] = seg['wpos'], seg['wnrm']
    return {'materials': [], 'nodes': [nodes[n] for n in order]}
