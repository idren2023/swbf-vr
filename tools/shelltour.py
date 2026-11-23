#!/usr/bin/env python3
"""A picture tour of a cockpit shell without the game (idren2023 2026-10-19: the software renderer gives good views of the
interiors; game runs only for a final showcase): the game's first-person cockpit model and our shell (tools/
shellpreview.sh writes re/shells/<design>.obj) rendered by meshview from one or more named eyes in several directions,
in parallel, put on one labelled contact sheet.

    python tools/shelltour.py <out.png> --obj re/shells/<design>.obj [--msh cockpit.msh] [--texdir DIR]
                              --eye NAME X Y Z [--eye NAME X Y Z ...] [--views name:yaw:pitch,...] [--size W H]
                              [--fov DEG] [--columns N]

Eyes are in the shell's space (the cockpit model's: x right, y up, z back). Views default to ahead, ahead and down,
left, right, back and up, back and down, straight up (yaw positive turns toward +x, the shells' right; pitch up
positive). The sheet's
labels are drawn by Windows' own fonts through PowerShell (System.Drawing).
"""
import multiprocessing
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import meshview  # noqa: E402
import mshgeo  # noqa: E402

DEFAULT_VIEWS = [('ahead', 0, -10), ('ahead, down', 0, -45), ('left', -90, -10), ('right', 90, -10),
                 ('back, up', 170, 25), ('back, down', 175, -25), ('up', 0, 80)]

_tris = None
_textures = None


def _load(mshs, objs, texdirs):
    tris = []
    for m in mshs:
        tris += list(meshview.msh_triangles(mshgeo.load(m)))
        texdirs = texdirs + [os.path.dirname(m)]
    for o in objs:
        tris += meshview.load_obj(o)
    textures = {}
    for tri in tris:
        t = tri[2]
        if t and t.startswith('#glass='):
            t = t[7:]  # textured glass: load its window texture
        key = t.lower()
        if not t or t.startswith('#') or key in textures:
            continue
        textures[key] = None
        for d in texdirs:
            for name in (t, t + '.tga', t.replace('.tga', '') + '.tga'):
                path = os.path.join(d, name)
                if os.path.exists(path):
                    textures[key] = meshview.read_tga(path)
                    break
            if textures[key]:
                break
    return tris, textures


def _init(mshs, objs, texdirs):
    global _tris, _textures
    _tris, _textures = _load(mshs, objs, texdirs)


def _render(job):
    path, eye, yaw, pitch, fov, size = job
    meshview.write_png(path, size[0], size[1], meshview.render(_tris, eye, yaw, pitch, fov, size, _textures))
    return path


def main():
    args = sys.argv[1:]
    out = args.pop(0)
    mshs, objs, texdirs, eyes, views = [], [], [], [], DEFAULT_VIEWS
    size, fov, columns = (480, 360), 100.0, 4
    while args:
        a = args.pop(0)
        if a == '--msh':
            mshs.append(args.pop(0))
        elif a == '--obj':
            objs.append(args.pop(0))
        elif a == '--texdir':
            texdirs.append(args.pop(0))
        elif a == '--eye':
            eyes.append((args.pop(0), tuple(float(args.pop(0)) for _ in range(3))))
        elif a == '--views':
            views = [(v.split(':')[0], float(v.split(':')[1]), float(v.split(':')[2])) for v in args.pop(0).split(',')]
        elif a == '--size':
            size = (int(args.pop(0)), int(args.pop(0)))
        elif a == '--fov':
            fov = float(args.pop(0))
        elif a == '--columns':
            columns = int(args.pop(0))
    folder = os.path.splitext(out)[0] + '-views'
    os.makedirs(folder, exist_ok=True)
    jobs, labels = [], []
    for name, eye in eyes:
        for view, yaw, pitch in views:
            jobs.append((os.path.join(folder, '%02d.png' % len(jobs)), eye, yaw, pitch, fov, size))
            labels.append('%s: %s' % (name, view))
    with multiprocessing.Pool(min(len(jobs), os.cpu_count() or 4), _init, (mshs, objs, texdirs)) as pool:
        paths = pool.map(_render, jobs)
    script = os.path.join(folder, 'sheet.ps1')
    with open(script, 'w', encoding='utf-8') as f:
        f.write('Add-Type -AssemblyName System.Drawing\n')
        rows = (len(paths) + columns - 1) // columns
        f.write('$o = New-Object System.Drawing.Bitmap %d, %d\n' % (size[0] * columns, size[1] * rows))
        f.write('$g = [System.Drawing.Graphics]::FromImage($o)\n')
        f.write("$f = New-Object System.Drawing.Font 'Arial', 13, ([System.Drawing.FontStyle]::Bold)\n")
        for index, (path, label) in enumerate(zip(paths, labels)):
            x, y = (index % columns) * size[0], (index // columns) * size[1]
            f.write("$b = [System.Drawing.Image]::FromFile('%s'); $g.DrawImage($b, %d, %d, %d, %d); $b.Dispose()\n" %
                    (os.path.abspath(path), x, y, size[0], size[1]))
            f.write("$g.FillRectangle([System.Drawing.Brushes]::Black, %d, %d, %d, 22)\n" % (x, y, len(label) * 9 + 8))
            f.write("$g.DrawString('%s', $f, [System.Drawing.Brushes]::Yellow, %d, %d)\n" % (label.replace("'", "''"),
                                                                                             x + 4, y + 2))
        f.write("$o.Save('%s')\n" % os.path.abspath(out))
    subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script], check=True)
    print(out, len(paths), 'views')


if __name__ == '__main__':
    main()
