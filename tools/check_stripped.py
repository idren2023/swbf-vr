"""python tools/check_stripped.py <folder>: refuses the stripped arm, body and seated includes (src/data/*.inc)
if any game-derived part is still in them: a texture array with pixels, or a row of a game-derived mesh table
(tools/data/meshpatch/meshpatch.json) whose position, normal or uv is not zero, or such a table not left to
GameMeshes. Exit 1 lists every problem."""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = json.load(open(os.path.join(HERE, 'data', 'meshpatch', 'meshpatch.json')))
NUMBER = re.compile(r'-?[0-9.]+(?:e[-+]?\d+)?f?')
problems = []
registered = set()
for path in sorted(glob.glob(os.path.join(sys.argv[1], '*.inc'))):
    name = os.path.basename(path)
    text = open(path).read()
    for m in re.finditer(r'unsigned ((?:BODY_ARM_TEXTURE|PLAYER_BODY_TEXELS)_\d+)\[([^\]]*)\] = \{([^}]*)\}', text):
        if m.group(2) != '1' or m.group(3).strip():
            problems.append('%s: %s still has pixels' % (name, m.group(1)))
    for m in re.finditer(r'(\w+) (\w+)\[\d*\] = \{[^\n]*\n(.*?)\n\};', text, flags=re.S):
        table = m.group(2)
        if table not in TABLES:
            continue
        if m.group(1) == 'constexpr' or '%s_REGISTERED' % table not in text:
            problems.append('%s: %s not left to GameMeshes' % (name, table))
        registered.add(table)
        for row, line in enumerate(m.group(3).split('\n')):
            values = [float(v.rstrip('f')) for v in NUMBER.findall(line)[:8]]
            if any(values):
                problems.append('%s: %s row %d has game data' % (name, table, row))
                break
for table in sorted(set(TABLES) - registered):
    problems.append('%s: missing from %s' % (table, sys.argv[1]))
for p in problems:
    print('check_stripped: ' + p)
print('check_stripped: %s (%d game-derived tables zero, texture arrays empty)' % ('clean' if not problems else
                                                                                  'REFUSED', len(registered)))
sys.exit(1 if problems else 0)
