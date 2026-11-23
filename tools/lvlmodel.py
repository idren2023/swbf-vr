"""lvlmodel.py <file.lvl> <model name> <out.obj>: a model packed in a munged .lvl (skel + modl chunks) as an OBJ in model
space: segments placed by their node (BNAM) through the skeleton's XFRM (3x3 rotation + translation per node, row
vectors), triangle strips from IBUF, positions from VBUF (stride 36: pos, normal, colour, uv; 32: pos, normal, uv)."""
import struct
import sys

lvl, model_name, out = sys.argv[1], sys.argv[2].encode(), sys.argv[3]
d = open(lvl, 'rb').read()


def chunks(start, end):
    j = start
    while j + 8 <= end:
        tag = d[j:j + 4]
        n = struct.unpack_from('<I', d, j + 4)[0]
        yield tag, j + 8, n
        j += 8 + n + ((-n) % 4)


def find(tag, name):
    i = 0
    while True:
        i = d.find(tag, i)
        if i < 0:
            raise SystemExit('no %s for %s' % (tag, name))
        n = struct.unpack_from('<I', d, i + 4)[0]
        body = d[i + 8:i + 8 + 64]
        if name + b'\0' in body[:48] and (b'NAME' in body[:8] or b'INFO' in body[:8]):
            return i + 8, n
        i += 4


# Skeleton: names, parents, local transforms.
s, n = find(b'skel', model_name)
names = parents = None
xfrm = None
for tag, p, m in chunks(s, s + n):
    if tag == b'NAME':
        names = d[p:p + m].split(b'\0')[:-1]
    elif tag == b'PRNT':
        parents = d[p:p + m].split(b'\0')[:-1]
    elif tag == b'XFRM':
        xfrm = [struct.unpack_from('<12f', d, p + 48 * k) for k in range(m // 48)]
world = {}


def node_world(index):
    name = names[index]
    if name in world:
        return world[name]
    r = xfrm[index]
    local = ([r[0:3], r[3:6], r[6:9]], r[9:12])
    parent = parents[index] if index < len(parents) else b''
    if parent and parent in names and parent != name:
        pr, pt = node_world(names.index(parent))
        rot = [[sum(local[0][i][k] * pr[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
        pos = [sum(local[1][k] * pr[k][j] for k in range(3)) + pt[j] for j in range(3)]
        world[name] = (rot, pos)
    else:
        world[name] = local
    return world[name]


for index in range(len(names)):
    node_world(index)

# Model: the first modl with this name (the high-detail one).
s, n = find(b'modl', model_name)
verts, faces = [], []
for tag, p, m in chunks(s, s + n):
    if tag != b'segm':
        continue
    strip, positions, bone = [], [], b''
    for t2, p2, m2 in chunks(p, p + m):
        if t2 == b'IBUF':
            count = struct.unpack_from('<I', d, p2)[0]
            strip = list(struct.unpack_from('<%dH' % count, d, p2 + 4))
        elif t2 == b'VBUF':
            count, stride, flags = struct.unpack_from('<III', d, p2)
            if stride >= 32:
                positions = [struct.unpack_from('<3f', d, p2 + 12 + k * stride) for k in range(count)]
        elif t2 == b'BNAM':
            bone = d[p2:p2 + m2].split(b'\0')[0]
    rot, pos = world.get(bone, ([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0]))
    base = len(verts)
    for v in positions:
        verts.append([sum(v[k] * rot[k][j] for k in range(3)) + pos[j] for j in range(3)])
    for k in range(len(strip) - 2):
        a, b, c = strip[k], strip[k + 1], strip[k + 2]
        if a == b or b == c or a == c:
            continue
        faces.append((base + a, base + b, base + c) if k % 2 == 0 else (base + b, base + a, base + c))
with open(out, 'w') as f:
    for v in verts:
        f.write('v %.4f %.4f %.4f\n' % tuple(v))
    for a, b, c in faces:
        f.write('f %d %d %d\n' % (a + 1, b + 1, c + 1))
xs, ys, zs = zip(*verts)
print('%d vertices, %d triangles; x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f' % (
    len(verts), len(faces), min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
