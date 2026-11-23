"""Blender, two steps:
  blender -b --factory-startup <a.blend> -P tools/body_uvcompare.py -- dump <object> <bone,..|all> <out.json>
      the uv edges of the object's faces (weighted > 0.5 to the bones, or all), per texture file
  blender -b --factory-startup -P tools/body_uvcompare.py -- draw <textures-dir> <out-prefix> <a.json> <b.json>
      each texture at 4x with a's edges magenta and b's cyan: <out-prefix>-<texture>.png"""
import bpy, bmesh, json, os, sys

args = sys.argv[sys.argv.index('--') + 1:]
if args[0] == 'dump':
    obj, bones, out = bpy.data.objects[args[1]], args[2].split(','), args[3]
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    deform = bm.verts.layers.deform.active
    uvl = bm.loops.layers.uv.active
    groups = {obj.vertex_groups[b].index for b in bones if b in obj.vertex_groups}
    edges = {}
    for f in bm.faces:
        if bones != ['all'] and not all(any(g in groups and w > 0.5 for g, w in v[deform].items()) for v in f.verts):
            continue
        mat = obj.data.materials[f.material_index]
        img = next((n.image for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image), None)
        if not img:
            continue
        uvs = [tuple(l[uvl].uv) for l in f.loops]
        edges.setdefault(os.path.basename(img.filepath), []).extend(
            [a + b for a, b in zip(uvs, uvs[1:] + uvs[:1])])
    json.dump(edges, open(out, 'w'))
    print('DUMP', out, {k: len(v) for k, v in edges.items()})
else:
    tex, prefix, ja, jb = args[1], args[2], json.load(open(args[3])), json.load(open(args[4]))
    for name in sorted(set(ja) | set(jb)):
        img = bpy.data.images.load(os.path.join(tex, name))
        w, h = img.size
        k = 4
        px = img.pixels[:]
        cw, ch = w * k, h * k
        out = [0.0] * (cw * ch * 4)
        for y in range(ch):
            src = (y // k) * w
            row = y * cw
            for x in range(cw):
                s = (src + x // k) * 4
                o = (row + x) * 4
                out[o] = px[s] * 0.75
                out[o + 1] = px[s + 1] * 0.75
                out[o + 2] = px[s + 2] * 0.75
                out[o + 3] = 1.0
        for edges, colour in ((ja.get(name, []), (1.0, 0.1, 0.9)), (jb.get(name, []), (0.1, 0.95, 1.0))):
            for u0, v0, u1, v1 in edges:
                n = int(max(abs(u1 - u0) * cw, abs(v1 - v0) * ch)) + 1
                for t in range(n + 1):
                    x = int((u0 + (u1 - u0) * t / n) * cw)
                    y = int((v0 + (v1 - v0) * t / n) * ch)
                    if 0 <= x < cw and 0 <= y < ch:
                        o = (y * cw + x) * 4
                        out[o:o + 3] = colour
        im = bpy.data.images.new('cmp', cw, ch)
        im.pixels = out
        im.filepath_raw = os.path.abspath('%s-%s.png' % (prefix, os.path.splitext(name)[0]))
        im.file_format = 'PNG'
        im.save()
        print('DRAWN', im.filepath_raw)
