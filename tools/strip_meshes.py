"""python tools/strip_meshes.py <in.inc> <out.inc>: an arm, body or seated-trooper include with the positions, normals
and uvs of our game-derived tables left out (zeros; tools/data/meshpatch/meshpatch.json names the tables). Each such
table becomes writable and registers itself with GameMeshes, which fills those columns at start from the player's
own game models and our offsets (tools/meshpatch.py). The rest (bones, weights, triangles, our own meshes) is copied
as it is."""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = json.load(open(os.path.join(HERE, 'data', 'meshpatch', 'meshpatch.json')))
ROW_ARRAYS = re.compile(r'^(\s*\{)\{[^}]*\}, \{[^}]*\}, \{[^}]*\}')  # BodyArmVertex, PlayerBodyVertex
ROW_STOCK = re.compile(r'^(\s*\{)\{[^}]*\}, \{[^}]*\}, [^,]+, [^,]+,')  # StockVertex: position, normal, u, v, texture


def strip(match):
    kind, name, size, body = match.group(1), match.group(2), match.group(3), match.group(4)
    if name not in TABLES:
        return match.group(0)
    rows = []
    for line in body.split('\n'):
        line, n = ROW_ARRAYS.subn(r'\1{0, 0, 0}, {0, 0, 0}, {0, 0}', line)
        if not n:
            line = ROW_STOCK.sub(r'\1{0, 0, 0}, {0, 0, 0}, 0, 0,', line)
        rows.append(line)
    return ('%s %s[%s] = {  // position, normal and uv made at start: GameMeshes\n%s\n};\n'
            '[[maybe_unused]] const bool %s_REGISTERED = GameMeshes::Register("%s", %s, sizeof(%s[0]), '
            'static_cast<int>(sizeof(%s) / sizeof(%s[0])));' % (kind, name, size, '\n'.join(rows), name, name, name,
                                                                  name, name, name))


text = open(sys.argv[1]).read()
first, _, rest = text.partition('\n')  # the generator's header: says where the full include came from
first = re.sub(r'(: do not edit, never commit|: not in git, do not edit\.)$', '', first)
text = ('%s; the game-derived parts left out (made at start), this copy is in git (src/data): tools/build.sh writes it '
        'again where the full include exists, do not edit\n%s' % (first, rest))
text, count = re.subn(r'constexpr (\w+) (\w+)\[(\d*)\] = \{[^\n]*\n(.*?)\n\};', strip, text, flags=re.S)
found = sum(1 for name in TABLES if '%s_REGISTERED' % name in text)
open(sys.argv[2], 'w', newline='\n').write(text)
print('strip_meshes: %d tables left to GameMeshes in %s' % (found, sys.argv[2]))
