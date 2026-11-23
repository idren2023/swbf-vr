"""Geometry of a SWBF .msh model (MSH2): nodes with their world transforms, and every segment's triangles with
positions in model space, normals, UVs and texture name. Used to study the first-person cockpit models (PLAN 54) and
to fit the cockpit shells to them (tools/meshview.py renders both).

    python tools/mshgeo.py <file.msh>     prints nodes, segments, bounds and open (boundary) edges

Chunks read: MATL/MATD (NAME, TX0D), MODL (NAME, PRNT, TRAN, MTYP), GEOM/SEGM (MATI, POSL, NRML, UV0L, NDXT, STRP).
TRAN = scale x3, rotation quaternion x y z w, translation x3; a node's world = local x parent's world (row vectors).
"""
import struct
import sys


def _chunks(b):
    j = 0
    while j + 8 <= len(b):
        tag = b[j:j + 4]
        length = struct.unpack_from('<I', b, j + 4)[0]
        yield tag, b[j + 8:j + 8 + length]
        j += 8 + length + ((-length) % 4)


def _name(c):
    return c.split(b'\0')[0].decode('latin1')


def quat_matrix(x, y, z, w):
    """Rotation of a unit quaternion as rows (row-vector convention: v' = v * M)."""
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
        [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
        [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)],
    ]


def compose(local, parent):
    """local then parent: each is (rows3x3, translation3)."""
    (r1, t1), (r2, t2) = local, parent
    rows = [[sum(r1[i][k] * r2[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    t = [sum(t1[k] * r2[k][j] for k in range(3)) + t2[j] for j in range(3)]
    return rows, t


def apply(point, frame):
    rows, t = frame
    return [sum(point[k] * rows[k][j] for k in range(3)) + t[j] for j in range(3)]


def apply_dir(vector, frame):
    rows, _ = frame
    return [sum(vector[k] * rows[k][j] for k in range(3)) for j in range(3)]


def load(path):
    data = open(path, 'rb').read()
    root = dict(_chunks(data))[b'HEDR']
    msh2 = dict(_chunks(root))[b'MSH2']
    materials, nodes = [], []
    for tag, c in _chunks(msh2):
        if tag == b'MATL':
            for t2, m in _chunks(c[4:]):
                if t2 == b'MATD':
                    fields = dict(_chunks(m))
                    materials.append({'name': _name(fields.get(b'NAME', b'')),
                                      'texture': _name(fields.get(b'TX0D', b''))})
        elif tag == b'MODL':
            node = {'segments': []}
            for t2, m in _chunks(c):
                if t2 == b'NAME':
                    node['name'] = _name(m)
                elif t2 == b'PRNT':
                    node['parent'] = _name(m)
                elif t2 == b'MTYP':
                    node['type'] = struct.unpack_from('<I', m)[0]
                elif t2 == b'TRAN':
                    node['tran'] = struct.unpack_from('<10f', m)
                elif t2 == b'GEOM':
                    for t3, g in _chunks(m):
                        if t3 != b'SEGM':
                            continue
                        segment = {'tris': []}
                        for t4, s in _chunks(g):
                            if t4 == b'MATI':
                                segment['material'] = struct.unpack_from('<I', s)[0]
                            elif t4 == b'POSL':
                                n = struct.unpack_from('<I', s)[0]
                                segment['pos'] = [struct.unpack_from('<3f', s, 4 + 12 * i) for i in range(n)]
                            elif t4 == b'NRML':
                                n = struct.unpack_from('<I', s)[0]
                                segment['nrm'] = [struct.unpack_from('<3f', s, 4 + 12 * i) for i in range(n)]
                            elif t4 == b'UV0L':
                                n = struct.unpack_from('<I', s)[0]
                                segment['uv'] = [struct.unpack_from('<2f', s, 4 + 8 * i) for i in range(n)]
                            elif t4 == b'STRP':
                                n = struct.unpack_from('<I', s)[0]
                                idx = struct.unpack_from('<%dH' % n, s, 4)
                                tris, i = [], 0
                                while i < n:
                                    # a strip starts with two indices flagged 0x8000
                                    if idx[i] & 0x8000 and i + 1 < n and idx[i + 1] & 0x8000:
                                        a, b2 = idx[i] & 0x7FFF, idx[i + 1] & 0x7FFF
                                        i += 2
                                        flip = False
                                        while i < n and not (idx[i] & 0x8000 and i + 1 < n and idx[i + 1] & 0x8000):
                                            c3 = idx[i] & 0x7FFF
                                            tris.append((b2, a, c3) if flip else (a, b2, c3))
                                            a, b2 = b2, c3
                                            flip = not flip
                                            i += 1
                                    else:
                                        i += 1
                                segment['strip_tris'] = tris
                            elif t4 == b'NDXT':
                                n = struct.unpack_from('<I', s)[0]
                                idx = struct.unpack_from('<%dH' % (3 * n), s, 4)
                                segment['tris'] = [idx[3 * i:3 * i + 3] for i in range(n)]
                        if not segment['tris'] and segment.get('strip_tris'):
                            segment['tris'] = segment['strip_tris']
                        node['segments'].append(segment)
            nodes.append(node)
    frames = {}

    def frame_of(node):
        if node['name'] in frames:
            return frames[node['name']]
        t = node.get('tran', (1, 1, 1, 0, 0, 0, 1, 0, 0, 0))
        rows = quat_matrix(*t[3:7])
        rows = [[rows[i][j] * t[i] for j in range(3)] for i in range(3)]
        local = (rows, list(t[7:10]))
        parent = next((n for n in nodes if n['name'] == node.get('parent')), None)
        frames[node['name']] = compose(local, frame_of(parent)) if parent else local
        return frames[node['name']]

    for node in nodes:
        node['world'] = frame_of(node)
        for segment in node['segments']:
            segment['texture'] = materials[segment.get('material', 0)]['texture'] if materials else ''
            segment['wpos'] = [apply(p, node['world']) for p in segment.get('pos', [])]
            segment['wnrm'] = [apply_dir(v, node['world']) for v in segment.get('nrm', [])]
    return {'materials': materials, 'nodes': nodes}


HIDDEN_PREFIXES = ('collision', 'hp_', 'p_', 'shadowvolume')


def is_drawn(node):
    """Whether the game draws this node (not collision, hardpoints, shadow volumes or the low-detail copy)."""
    name = node.get('name', '').lower()
    return not name.startswith(HIDDEN_PREFIXES) and 'lowrez' not in name and 'lowres' not in name


def triangles(model):
    """Every triangle of the drawn nodes: (three positions, three uvs, texture name)."""
    for node in model['nodes']:
        if not is_drawn(node):
            continue
        for s in node['segments']:
            uv = s.get('uv') or [(0, 0)] * len(s['wpos'])
            for a, b, c in s['tris']:
                yield (s['wpos'][a], s['wpos'][b], s['wpos'][c]), (uv[a], uv[b], uv[c]), s['texture']


def open_edges(model, places=3):
    """Boundary edges (used by one triangle only), positions rounded to merge split vertices."""
    count = {}
    for (p0, p1, p2), _, _ in triangles(model):
        keys = [tuple(round(v, places) for v in p) for p in (p0, p1, p2)]
        for i in range(3):
            edge = tuple(sorted((keys[i], keys[(i + 1) % 3])))
            count[edge] = count.get(edge, 0) + 1
    return [e for e, n in count.items() if n == 1]


if __name__ == '__main__':
    for path in sys.argv[1:]:
        model = load(path)
        print('##', path.replace('\\', '/').split('/')[-1])
        print('materials:', [(m['name'], m['texture']) for m in model['materials']])
        allp = []
        for node in model['nodes']:
            t = node['world'][1]
            tri = sum(len(s['tris']) for s in node['segments'])
            pts = [p for s in node['segments'] for p in s['wpos']]
            allp += pts
            box = ''
            if pts:
                box = ' bounds x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f' % (
                    min(p[0] for p in pts), max(p[0] for p in pts), min(p[1] for p in pts),
                    max(p[1] for p in pts), min(p[2] for p in pts), max(p[2] for p in pts))
            print('  %-24s parent %-16s type %s at %.3f %.3f %.3f  tris %d%s' % (
                node.get('name'), node.get('parent', '-'), node.get('type'), t[0], t[1], t[2], tri, box))
        edges = open_edges(model)
        print('triangles', sum(1 for _ in triangles(model)), 'open edges', len(edges))
