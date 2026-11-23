"""Software renderer for checking the cockpit shells against the game's cockpit models (PLAN 54): textured triangles
with a z-buffer, written as PNG. No numpy, no PIL (pure Python; a few seconds a picture).

    python tools/meshview.py out.png --eye X Y Z --yaw DEG --pitch DEG [--fov DEG] [--size W H]
                             [--msh file.msh] [--obj file.obj] [--texdir DIR] [--wire] [--glass]

Model space as the msh: the eye looks along -z at yaw 0 (yaw left positive, pitch up positive). Textures are found by
name (TGA) in --texdir and next to the msh. An OBJ's `usemtl <texture name>` picks its texture; `# shade` lines are
ignored. --glass draws the shell's glass (usemtl #glass) as a pale see-through tint over what lies behind it. Faces without a texture are drawn grey; lighting: half Lambert from a light above and ahead, so shapes read.
"""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
import mshgeo  # noqa: E402


ALPHAS = {}  # id(pixels) -> alpha rows, for 32-bit TGAs (textured glass)


def read_tga(path):
    d = open(path, 'rb').read()
    idlen, cmtype, itype = d[0], d[1], d[2]
    w, h, bpp, desc = struct.unpack('<HHBB', d[12:18])
    off = 18 + idlen + (struct.unpack('<H', d[5:7])[0] * (d[7] // 8) if cmtype else 0)
    px = bpp // 8
    data = bytearray()
    if itype == 2:
        data = d[off:off + w * h * px]
    elif itype == 10:
        i = off
        while len(data) < w * h * px:
            c = d[i]
            i += 1
            n = (c & 0x7F) + 1
            if c & 0x80:
                data += d[i:i + px] * n
                i += px
            else:
                data += d[i:i + px * n]
                i += px * n
    else:
        return None
    rows = [data[r * w * px:(r + 1) * w * px] for r in range(h)]
    if not (desc & 0x20):
        rows.reverse()
    pixels = [[(r[x * px + 2], r[x * px + 1], r[x * px]) for x in range(w)] for r in rows]
    if px == 4:
        ALPHAS[id(pixels)] = [[r[x * px + 3] for x in range(w)] for r in rows]
    return w, h, pixels


def write_png(path, w, h, pixels):
    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in pixels)

    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)

    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def load_obj(path):
    """Triangles of an OBJ: (positions, uvs, texture)."""
    vs, vts, out, tex, color = [], [], [], '', None
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == '#' and len(p) >= 3 and p[1] == 'color':
            v = int(p[2], 16)
            color = ((v >> 16) & 255, (v >> 8) & 255, v & 255)
            continue
        if p[0] == '#' and len(p) >= 11 and p[1] == 'cutout':
            # An opening cut in the game's cockpit model (ShellDesigns Cutout): a convex polygon, fanned.
            pts = [tuple(map(float, p[2 + 3 * k:5 + 3 * k])) for k in range((len(p) - 2) // 3)]
            for k in range(1, len(pts) - 1):
                out.append(((pts[0], pts[k], pts[k + 1]), ((0, 0),) * 3, '#cut', None))
            continue
        if p[0] == 'v':
            vs.append(tuple(map(float, p[1:4])))
        elif p[0] == 'vt':
            vts.append(tuple(map(float, p[1:3])))
        elif p[0] == 'usemtl':
            tex = p[1] if len(p) > 1 else ''
        elif p[0] == 'f':
            idx = []
            for q in p[1:]:
                parts = q.split('/')
                idx.append((int(parts[0]) - 1, int(parts[1]) - 1 if len(parts) > 1 and parts[1] else -1))
            for k in range(1, len(idx) - 1):
                tri = (idx[0], idx[k], idx[k + 1])
                out.append((tuple(vs[i] for i, _ in tri), tuple(vts[t] if t >= 0 else (0, 0) for _, t in tri), tex,
                            color))
    return out


def render(tris, eye, yaw, pitch, fov, size, textures, wire=False, show_glass=False):
    w, h = size
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    focal = (w / 2) / math.tan(math.radians(fov) / 2)
    light = (0.35, 0.8, 0.5)
    ln = math.sqrt(sum(c * c for c in light))
    light = tuple(c / ln for c in light)
    # The outside (seen through windows and holes): a sky and ground backdrop by each row's elevation, so window
    # openings read as the outside world, not as missing geometry (idren2023 2026-10-19).
    color = []
    for py in range(h):
        elevation = pitch + math.degrees(math.atan((h / 2 - py - 0.5) / focal))
        if elevation >= 0:
            t = min(1.0, elevation / 40)
            row = (int(185 - 60 * t), int(205 - 45 * t), int(225 - 20 * t))
        else:
            row = (112, 104, 86)
        color.append([row] * w)
    depth = [[1e30] * w for _ in range(h)]
    near = 0.02
    # The shell's depth mask (#cut, from the OBJ): the cockpit model's triangles (the msh's: no tint field) are not drawn
    # behind it, as CockpitShell's depth-only mask in the game.
    cut = [[1e30] * w for _ in range(h)]  # the nearest mask depth per pixel
    tris = sorted(tris, key=lambda t: t[2] != '#cut')

    def to_view(p):
        x, y, z = p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]
        # yaw about y (left positive), then pitch about x
        x, z = x * cy + z * sy, -x * sy + z * cy
        y, z = y * cp + z * sp, -y * sp + z * cp
        return x, y, -z  # forward distance positive

    glass = []

    def draw(tri, blend=False):
        (p0, p1, p2), uvs, texname = tri[0], tri[1], tri[2]
        tint = tri[3] if len(tri) > 3 else None
        is_cockpit = len(tri) == 3
        is_cut = texname == '#cut'
        v = [to_view(p) for p in (p0, p1, p2)]
        if all(q[2] < near for q in v):
            return
        e1 = [p1[i] - p0[i] for i in range(3)]
        e2 = [p2[i] - p0[i] for i in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        nl = math.sqrt(sum(c * c for c in n)) or 1
        n = tuple(c / nl for c in n)
        shade = 0.55 + 0.45 * abs(sum(n[i] * light[i] for i in range(3)))
        if texname.startswith('#glass') and not blend:
            if show_glass or texname.startswith('#glass='):  # textured glass is always drawn (it is the look)
                glass.append(tri)  # --glass: blended over everything after the opaque faces
            return
        glass_tex = textures.get(texname[7:].lower()) if blend and texname.startswith('#glass=') else None
        if blend:
            texname, tint = '#flat', (205, 225, 240)
        tex = textures.get(texname.lower()) if texname and not texname.startswith('#') else None
        glow = texname == '#glow'
        # Clip at the near plane (a triangle reaching behind the eye was dropped whole until 2026-10-19: walls right
        # beside the eye vanished and read as holes), then fan the clipped polygon.
        polygon = []
        corners = list(zip(v, uvs))
        for index, (q, uv) in enumerate(corners):
            qn, uvn = corners[(index + 1) % 3]
            if q[2] >= near:
                polygon.append((q, uv))
            if (q[2] >= near) != (qn[2] >= near):
                t = (near - q[2]) / (qn[2] - q[2])
                polygon.append((tuple(q[k] + (qn[k] - q[k]) * t for k in range(3)),
                                tuple(uv[k] + (uvn[k] - uv[k]) * t for k in range(2))))
        pieces = [((polygon[0][0], polygon[k][0], polygon[k + 1][0]), (polygon[0][1], polygon[k][1], polygon[k + 1][1]))
                  for k in range(1, len(polygon) - 1)]
        for v, uvs in pieces:
            scr = [(w / 2 + q[0] * focal / q[2], h / 2 - q[1] * focal / q[2], q[2]) for q in v]
            minx = max(0, int(min(s[0] for s in scr)))
            maxx = min(w - 1, int(max(s[0] for s in scr)) + 1)
            miny = max(0, int(min(s[1] for s in scr)))
            maxy = min(h - 1, int(max(s[1] for s in scr)) + 1)
            (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = scr
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-9 or minx > maxx or miny > maxy:
                continue
            iz = (1 / z0, 1 / z1, 1 / z2)
            for py in range(miny, maxy + 1):
                fy = py + 0.5
                for px in range(minx, maxx + 1):
                    fx = px + 0.5
                    a = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area
                    b = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area
                    c = 1 - a - b
                    if a < 0 or b < 0 or c < 0:
                        continue
                    if is_cut:
                        cut[py][px] = min(cut[py][px], 1 / (a * iz[0] + b * iz[1] + c * iz[2]))
                        continue
                    if is_cockpit and 1 / (a * iz[0] + b * iz[1] + c * iz[2]) >= cut[py][px]:
                        continue
                    if wire and min(a, b, c) > 0.04:
                        continue
                    inv = a * iz[0] + b * iz[1] + c * iz[2]
                    z = 1 / inv
                    if z >= depth[py][px]:
                        continue
                    if blend:
                        # glass: a pale tint, more opaque toward grazing angles (a fake Fresnel), no depth write
                        facing = abs(sum(n[i] * (eye[i] - (p0[i] + p1[i] + p2[i]) / 3) for i in range(3))) / max(1e-6, math.sqrt(sum((eye[i] - (p0[i] + p1[i] + p2[i]) / 3) ** 2 for i in range(3))))
                        k = 0.18 + 0.30 * (1 - facing)
                        glass_colour = tint
                        if glass_tex:
                            # the window texture: its colour, its alpha (grime) on top of a light tint
                            tw, th, tp = glass_tex
                            u = (a * uvs[0][0] * iz[0] + b * uvs[1][0] * iz[1] + c * uvs[2][0] * iz[2]) / inv
                            vv = (a * uvs[0][1] * iz[0] + b * uvs[1][1] * iz[1] + c * uvs[2][1] * iz[2]) / inv
                            tx = int((u % 1.0) * tw) % tw
                            ty = int(((1 - vv) % 1.0) * th) % th
                            rgb = tp[ty][tx]
                            alpha = ALPHAS.get(id(tp))
                            k = min(0.95, 0.10 + 0.25 * (1 - facing) + (alpha[ty][tx] / 255.0 if alpha else 0.3) * 0.8)
                            glass_colour = tuple(rgb[j] * (0x98, 0xB0, 0xC4)[j] / 255 for j in range(3))
                        old = color[py][px]
                        color[py][px] = tuple(int(old[j] * (1 - k) + glass_colour[j] * k) for j in range(3))
                        continue
                    depth[py][px] = z
                    if tex:
                        tw, th, tp = tex
                        u = (a * uvs[0][0] * iz[0] + b * uvs[1][0] * iz[1] + c * uvs[2][0] * iz[2]) / inv
                        vv = (a * uvs[0][1] * iz[0] + b * uvs[1][1] * iz[1] + c * uvs[2][1] * iz[2]) / inv
                        tx = int((u % 1.0) * tw) % tw
                        ty = int(((1 - vv) % 1.0) * th) % th  # msh / OBJ v up
                        r, g, bb = tp[ty][tx]
                        if tint:
                            r, g, bb = r * tint[0] / 128, g * tint[1] / 128, bb * tint[2] / 128
                    elif tint:
                        r, g, bb = tint
                    else:
                        r, g, bb = 150, 150, 150
                    k = 1.0 if glow else shade
                    color[py][px] = (min(255, int(r * k)), min(255, int(g * k)), min(255, int(bb * k)))

    for tri in tris:
        draw(tri)
    for tri in glass:
        draw(tri, True)
    return color


def msh_triangles(model):
    """The msh's drawn triangles, those of its window nodes (named *window*, as shellcheck) marked as glass: see-through
    unless --glass, as the game shows the outside through them."""
    for node in model['nodes']:
        if not mshgeo.is_drawn(node):
            continue
        is_window = 'window' in node.get('name', '').lower()
        for s in node['segments']:
            uv = s.get('uv') or [(0, 0)] * len(s['wpos'])
            for a, b, c in s['tris']:
                yield (s['wpos'][a], s['wpos'][b], s['wpos'][c]), (uv[a], uv[b], uv[c]),                     '#glass' if is_window else s['texture']


def main():
    args = sys.argv[1:]
    out = args.pop(0)
    eye, yaw, pitch, fov, size = (0, 1.5, 1.5), 0.0, 0.0, 100.0, (480, 360)
    mshs, objs, texdirs, wire, show_glass = [], [], [], False, False
    while args:
        a = args.pop(0)
        if a == '--eye':
            eye = tuple(float(args.pop(0)) for _ in range(3))
        elif a == '--yaw':
            yaw = float(args.pop(0))
        elif a == '--pitch':
            pitch = float(args.pop(0))
        elif a == '--fov':
            fov = float(args.pop(0))
        elif a == '--size':
            size = (int(args.pop(0)), int(args.pop(0)))
        elif a == '--msh':
            mshs.append(args.pop(0))
        elif a == '--obj':
            objs.append(args.pop(0))
        elif a == '--texdir':
            texdirs.append(args.pop(0))
        elif a == '--wire':
            wire = True
        elif a == '--glass':
            show_glass = True
    tris = []
    for m in mshs:
        tris += list(msh_triangles(mshgeo.load(m)))
        texdirs.append(os.path.dirname(m))
    for o in objs:
        tris += load_obj(o)
    textures = {}
    for tri in tris:
        t = tri[2]
        key = t.lower()
        if not t or t.startswith('#') or key in textures:
            continue
        textures[key] = None
        for d in texdirs:
            for name in (t, t + '.tga', t.replace('.tga', '') + '.tga'):
                path = os.path.join(d, name)
                if os.path.exists(path):
                    textures[key] = read_tga(path)
                    break
            if textures[key]:
                break
    write_png(out, size[0], size[1], render(tris, eye, yaw, pitch, fov, size, textures, wire, show_glass))
    print(out, len(tris), 'triangles')


if __name__ == '__main__':
    main()
