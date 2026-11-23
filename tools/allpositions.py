"""Renders every seat of every vehicle shell (idren2023 2026-11-09: renders of all positions after the texturing pass).

Each seat is drawn as the game draws it: the shell and, unless the design is the whole cockpit ("# whole" in the obj),
the game's own first-person model from the vehicle's ODF (FirstPerson, through ClassParent), and for a gunner's cabin
the cabin's cockpit model (GunnerSeat's table). Each design's own eye (the "# eye" line tools/shellpreview.sh writes into re/shells/<design>.obj) and, for designs
with a gunner's cabin, the gunner's eye from src/GunnerSeat.cpp's cabin table (literal or a ShellDesigns.h constant).
One sheet per seat: ahead, down at the dash, left, right, behind. Writes <out>/<design>[-gunner].png and prints them.

    sh tools/shellpreview.sh $(cat re/shells/designs.txt)
    python tools/allpositions.py re/gallery/pass-2026-11-09 [design ...]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIDES = 'C:/LucasArts/BFBuilder/Assets/Sides'
TEXDIRS = []
for side in ('ALL', 'IMP', 'REP', 'CIS', 'Common'):
    TEXDIRS += ['--texdir', '%s/%s/MSH' % (SIDES, side)]
VIEWS = 'ahead:0:-5,dash:0:-35,left:-75:-20,right:75:-20,behind:180:-10'


def design_eye(design):
    for line in open(os.path.join(ROOT, 're/shells/%s.obj' % design), encoding='utf-8', errors='replace'):
        if line.startswith('# eye '):
            return [float(x) for x in line.split()[2:5]]
        if line.startswith('v '):
            break
    return None


def find_file(folder_glob, name):
    """A file by name (any case) under the sides' folders matching folder_glob, or None."""
    import glob
    want = name.lower()
    for path in glob.glob(os.path.join(SIDES, folder_glob)):
        if os.path.basename(path).lower() == want:
            return path
    return None


def first_person_model(odf, seen=None):
    """The pilot's first-person model name of an ODF (its first FirstPerson), following ClassParent."""
    seen = seen or set()
    path = find_file('*/odf/*.odf', odf + '.odf')
    if not path or odf.lower() in seen:
        return None
    seen.add(odf.lower())
    parent = None
    for line in open(path, encoding='latin-1'):
        m = re.match(r'\s*FirstPerson\s*=\s*"?([^"\s]+)"?', line, re.I)
        if m and ';' in m.group(1):
            return m.group(1).split(';')[1]
        m = re.match(r'\s*ClassParent\s*=\s*"?([^"\s]+)"?', line, re.I)
        if m:
            parent = m.group(1)
    return first_person_model(parent, seen) if parent else None


def msh_of(model):
    return find_file('*/MSH/*.msh', model + '.msh') if model else None


def is_whole(design):
    for line in open(os.path.join(ROOT, 're/shells/%s.obj' % design), encoding='utf-8', errors='replace'):
        if line.startswith('# whole '):
            return line.split()[2] == '1'
        if line.startswith('v '):
            break
    return False


def gunner_models():
    """design -> the gunner cabin's cockpit model name (GunnerSeat.cpp), where it has one."""
    source = open(os.path.join(ROOT, 'src/GunnerSeat.cpp'), encoding='utf-8').read()
    return {m.group(1): m.group(2) for m in re.finditer(r'\{"[^"]+",\s*"([^"]+)",\s*"([^"]+)"', source)}


def gunner_eyes():
    """design -> gunner eye, from GunnerSeat.cpp's cabins."""
    header = open(os.path.join(ROOT, 'src/ShellDesigns.h'), encoding='utf-8').read()
    consts = {m.group(1): [float(x.strip().rstrip('f')) for x in m.group(2).split(',')]
              for m in re.finditer(r'constexpr float (\w+)\[3\] = \{([^}]*)\}', header)}
    source = open(os.path.join(ROOT, 'src/GunnerSeat.cpp'), encoding='utf-8').read()
    eyes = {}
    for m in re.finditer(r'\{"[^"]+",\s*"([^"]+)",\s*[^,]+,\s*\w+,\s*(\{[^}]*\}|[\w:]+),\s*(\{[^}]*\}|[\w:]+)', source):
        design, eye = m.group(1), m.group(3)
        if eye.startswith('{'):
            values = []
            for x in eye.strip('{}').split(','):
                x = x.strip()
                ref = re.match(r'(?:\w+::)?(\w+)\[(\d)\]$', x)  # a constant's element (ShellDesigns::X_EYE[0])
                values.append(consts[ref.group(1)][int(ref.group(2))] if ref and ref.group(1) in consts
                              else float(x.rstrip('f')) if re.match(r'^-?[\d.]+f?$', x) else float('nan'))
            if any(v != v for v in values):
                continue
        else:
            values = consts.get(eye.split('::')[-1])
        if values and len(values) == 3:
            eyes.setdefault(design, values)
    return eyes


def main():
    out = sys.argv[1]
    only = sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    designs = only or open(os.path.join(ROOT, 're/shells/designs.txt')).read().split()
    gunners = gunner_eyes()
    cabinModels = gunner_models()
    for design in designs:
        eye = design_eye(design)
        seats = [('', eye)] if eye else []
        if design in gunners and (not eye or max(abs(a - b) for a, b in zip(gunners[design], eye)) > 0.05):
            seats.append(('-gunner', gunners[design]))
        for suffix, at in seats:
            path = os.path.join(out, design + suffix + '.png')
            if suffix:
                msh = msh_of(cabinModels.get(design))
            else:
                msh = None if is_whole(design) else msh_of(cabinModels.get(design) if design in cabinModels and
                                                           not first_person_model(design) else first_person_model(design))
            extra = ['--msh', msh] if msh else []
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools/shelltour.py'), path, '--obj',
                            os.path.join(ROOT, 're/shells/%s.obj' % design)] + extra + TEXDIRS +
                           ['--eye', 'seat', str(at[0]), str(at[1]), str(at[2]), '--views', VIEWS, '--columns', '5'],
                           check=False, capture_output=True)
            print((path if os.path.exists(path) else path + ' (failed)') + ('  + ' + os.path.basename(msh) if msh else ''))


if __name__ == '__main__':
    main()
