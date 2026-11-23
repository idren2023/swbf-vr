"""Geometry of a model in a munged game .lvl file (the player's own GameData/Data/_lvl_pc), for building our tables
from the game's files instead of the mod tools' .msh (docs/research/runtime-assets.md).

    python tools/lvlgeo.py <file.lvl> <model>      prints segments, vertex formats, bounds

Chunks read (ucfb): skel (INFO, NAME, PRNT, XFRM: 3x3 rotation + translation per bone, row vectors, local to the
parent) and the first modl of that name (the high-detail one): segm (TNAM texture, IBUF one triangle strip with
degenerate joins, VBUF vertex buffers in several formats, BNAM the bone it hangs on). The munger merges a model's
static nodes into one segment per material on its root bone, so node names are gone: tools/lvlmatch.py labels the
triangles again from the .msh.
"""
import struct
import sys

IDENTITY = ([[1.0, 0, 0], [0, 1.0, 0], [0, 0, 1.0]], [0.0, 0, 0])


class Lvl:
    def __init__(self, path):
        self.d = open(path, 'rb').read()

    def chunks(self, start, end):
        d, j = self.d, start
        while j + 8 <= end:
            tag = d[j:j + 4]
            n = struct.unpack_from('<I', d, j + 4)[0]
            yield tag, j + 8, n
            j += 8 + n + ((-n) % 4)

    def top(self):
        """The chunks inside the file's ucfb."""
        for tag, p, n in self.chunks(0, len(self.d)):
            if tag == b'ucfb':
                return list(self.chunks(p, p + n))
        return []

    def name_of(self, p, n):
        for tag, q, m in self.chunks(p, p + n):
            if tag in (b'NAME', b'INFO'):  # modl: NAME; skel: the name leads INFO
                return self.d[q:q + m].split(b'\0')[0].decode('latin1')
        return None

    def find(self, tag, name):
        """The first chunk of this tag and name anywhere in the file (side files nest them in lvl_ chunks)."""
        low, d, i = name.lower(), self.d, 0
        while (i := d.find(tag, i)) >= 0:
            n = struct.unpack_from('<I', d, i + 4)[0] if i + 8 <= len(d) else 0
            if i % 4 == 0 and 0 < n <= len(d) - i - 8 and d[i + 8:i + 12] in (b'NAME', b'INFO'):
                if (self.name_of(i + 8, n) or '').lower() == low:
                    return i + 8, n
            i += 4
        return None


def compose(local, parent):
    (r1, t1), (r2, t2) = local, parent
    rows = [[sum(r1[i][k] * r2[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    t = [sum(t1[k] * r2[k][j] for k in range(3)) + t2[j] for j in range(3)]
    return rows, t


def bones(lvl, name):
    """Bone name -> world frame (rows, translation) from the model's skel."""
    found = lvl.find(b'skel', name)
    if not found:
        return {}
    p, n = found
    names = parents = xfrm = None
    for tag, q, m in lvl.chunks(p, p + n):
        if tag == b'NAME':
            names = [s.decode('latin1') for s in lvl.d[q:q + m].split(b'\0')[:-1]]
        elif tag == b'PRNT':
            parents = [s.decode('latin1') for s in lvl.d[q:q + m].split(b'\0')[:-1]]
        elif tag == b'XFRM':
            xfrm = [struct.unpack_from('<12f', lvl.d, q + 48 * k) for k in range(m // 48)]
    world = {}

    def frame(i):
        name_i = names[i]
        if name_i not in world:
            r = xfrm[i]
            local = ([list(r[0:3]), list(r[3:6]), list(r[6:9])], list(r[9:12]))
            parent = parents[i] if parents and i < len(parents) else ''
            world[name_i] = local
            if parent and parent in names and parent != name_i:
                world[name_i] = compose(local, frame(names.index(parent)))
        return world[name_i]

    for i in range(len(names or [])):
        frame(i)
    world['__order__'] = names or []
    return world


def strip_triangles(strip):
    tris = []
    for k in range(len(strip) - 2):
        a, b, c = strip[k], strip[k + 1], strip[k + 2]
        if a == b or b == c or a == c:
            continue
        tris.append((a, b, c) if k % 2 == 0 else (b, a, c))
    return tris


def load(path, name):
    """{'segments': [{'texture', 'bone', 'tris', 'wpos', 'wnrm', 'uv', 'format'}], 'bones'} of the first modl."""
    lvl = Lvl(path)
    found = lvl.find(b'modl', name)
    if not found:
        raise KeyError('%s: no modl %s' % (path, name))
    world = bones(lvl, name)
    p, n = found
    segments = []
    for tag, q, m in lvl.chunks(p, p + n):
        if tag != b'segm':
            continue
        seg = {'texture': '', 'bone': '', 'tris': []}
        strip, buffers, skin, bmap = [], [], None, None
        for t2, q2, m2 in lvl.chunks(q, q + m):
            if t2 == b'TNAM' and struct.unpack_from('<I', lvl.d, q2)[0] == 0:  # slot 0 the base texture, 1 bump
                seg['texture'] = lvl.d[q2 + 4:q2 + m2].split(b'\0')[0].decode('latin1')
            elif t2 == b'IBUF':
                c = struct.unpack_from('<I', lvl.d, q2)[0]
                strip = list(struct.unpack_from('<%dH' % c, lvl.d, q2 + 4))
            elif t2 == b'VBUF':
                buffers.append(struct.unpack_from('<III', lvl.d, q2) + (q2 + 12,))
            elif t2 == b'BNAM':
                seg['bone'] = lvl.d[q2:q2 + m2].split(b'\0')[0].decode('latin1')
            elif t2 == b'SKIN':  # per vertex: an index into BMAP
                c = struct.unpack_from('<I', lvl.d, q2)[0]
                skin = lvl.d[q2 + 8:q2 + 8 + c]  # count, weights per vertex (1), then one byte each
            elif t2 == b'BMAP':  # BMAP index -> skeleton bone index
                c = struct.unpack_from('<I', lvl.d, q2)[0]
                bmap = lvl.d[q2 + 4:q2 + 4 + c]
        seg['tris'] = strip_triangles(strip)
        # the uncompressed buffer with the most fields; position first, normal and uv last
        count, stride, flags, base = max(buffers, key=lambda b: b[1])
        seg['format'] = (stride, flags)
        d = lvl.d
        pos = [struct.unpack_from('<3f', d, base + k * stride) for k in range(count)]
        # flags: 0x2 position, 0x20 normal, 0x40 tangent and binormal, 0x4 colour, 0x200 uv, in that order
        noff = 16 if flags & 0x4 else 12  # 0x4: four bytes after the position (bone index or colour)
        nrm = [struct.unpack_from('<3f', d, base + k * stride + noff) for k in range(count)]
        uv = [struct.unpack_from('<2f', d, base + k * stride + stride - 8) for k in range(count)]
        order = world.get('__order__', [])
        if skin is not None and bmap is not None:  # hard skinned: each vertex in its bone's space
            frames = [world.get(order[bmap[skin[k]]], IDENTITY) if bmap[skin[k]] < len(order) else IDENTITY
                      for k in range(count)]
        else:
            frames = [world.get(seg['bone'], IDENTITY)] * count
        seg['wpos'] = [[sum(v[k] * f[0][k][j] for k in range(3)) + f[1][j] for j in range(3)]
                       for v, f in zip(pos, frames)]
        seg['wnrm'] = [[sum(v[k] * f[0][k][j] for k in range(3)) for j in range(3)] for v, f in zip(nrm, frames)]
        seg['uv'] = uv
        segments.append(seg)
    return {'segments': segments, 'bones': world, 'hash': fnv1a(lvl.d[p:p + n])}  # hash: the modl chunk's bytes


if __name__ == '__main__':
    model = load(sys.argv[1], sys.argv[2])
    allp = []
    for i, s in enumerate(model['segments']):
        allp += s['wpos']
        print('  seg %2d %-28s bone %-14s format %s  verts %4d tris %4d' % (
            i, s['texture'], s['bone'], s['format'], len(s['wpos']), len(s['tris'])))
    print('bounds', ['%.2f..%.2f' % (min(p[i] for p in allp), max(p[i] for p in allp)) for i in range(3)])


# --- textures (tex_: NAME, INFO, then one FMT_ per stored format: INFO (format, width, height, ...), FACE, LVL_ per mip
# level with BODY the pixels). We read the top level of the best uncompressed format: Direct3D's A8R8G8B8 (21),
# R5G6B5 (23) or A4R4G4B4 (26); DXT formats are not decoded (every texture we use has an uncompressed copy).
TEXTURE_FORMATS = (21, 23, 26)


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def decode_pixels(fmt, width, height, body):
    """ARGB 0xAARRGGBB per pixel, rows from the top; integer expansion to 8 bits (x << n | x >> (m - n))."""
    out = []
    if fmt == 21:
        out = list(struct.unpack_from('<%dI' % (width * height), body))
    elif fmt == 23:
        for (v,) in struct.iter_unpack('<H', body[:2 * width * height]):
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            out.append(0xFF000000 | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2))
    elif fmt == 26:
        for (v,) in struct.iter_unpack('<H', body[:2 * width * height]):
            a, r, g, b = (v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15
            out.append((a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | b * 17)
    return out


def load_texture(path, name):
    """(width, height, ARGB pixels, format, fnv1a of the level's bytes) of the texture's best uncompressed format, or
    None."""
    lvl = Lvl(path)
    found = lvl.find(b'tex_', name)
    if not found:
        return None
    p, n = found
    best = None
    for tag, q, m in lvl.chunks(p, p + n):
        if tag != b'FMT_':
            continue
        fmt = width = height = None
        body = None
        for t2, q2, m2 in lvl.chunks(q, q + m):
            if t2 == b'INFO':
                fmt, width, height = struct.unpack_from('<IHH', lvl.d, q2)
            elif t2 == b'FACE':
                for t3, q3, m3 in lvl.chunks(q2, q2 + m2):
                    if t3 != b'LVL_' or body is not None:
                        continue
                    for t4, q4, m4 in lvl.chunks(q3, q3 + m3):
                        if t4 == b'BODY':
                            body = lvl.d[q4:q4 + m4]
        if fmt in TEXTURE_FORMATS and body is not None:
            rank = TEXTURE_FORMATS.index(fmt)
            if best is None or rank < best[0]:
                best = (rank, fmt, width, height, body)
    if best is None:
        return None
    _, fmt, width, height, body = best
    return width, height, decode_pixels(fmt, width, height, body), fmt, fnv1a(body)


def resize_bilinear(pixels, width, height, out_width, out_height):
    """Integer bilinear resampling (pixel centres aligned, edges clamped), the same arithmetic as src/GameTextures.cpp
    so both make the very same base image."""
    def axis(out_size, size):
        table = []
        for x in range(out_size):
            f = ((2 * x + 1) * size * 256) // (2 * out_size) - 128
            if f < 0:
                table.append((0, 0, 0))
                continue
            x0 = f >> 8
            w = f & 255
            if x0 >= size - 1:
                table.append((size - 1, size - 1, 0))
            else:
                table.append((x0, x0 + 1, w))
        return table
    xs, ys = axis(out_width, width), axis(out_height, height)
    out = []
    for y0, y1, wy in ys:
        row0, row1 = y0 * width, y1 * width
        for x0, x1, wx in xs:
            p00, p10, p01, p11 = pixels[row0 + x0], pixels[row0 + x1], pixels[row1 + x0], pixels[row1 + x1]
            v = 0
            for shift in (24, 16, 8, 0):
                c = (((p00 >> shift) & 255) * (256 - wx) + ((p10 >> shift) & 255) * wx) * (256 - wy) + \
                    (((p01 >> shift) & 255) * (256 - wx) + ((p11 >> shift) & 255) * wx) * wy
                v |= ((c + 32768) >> 16) << shift
            out.append(v)
    return out
