import sys
from collections import Counter

rows = []
for line in open(sys.argv[1]):
    parts = line.split()
    if len(parts) < 2 or parts[1] == '0':
        continue
    rows.append((parts[0], parts[2:]))
for eye in ('left', 'right'):
    sets = [Counter(p) for e, p in rows if e == eye]
    if not sets:
        continue
    allkeys = set().union(*sets)
    always = [k for k in allkeys if all(s[k] == sets[0][k] for s in sets)]
    varying = sorted(k for k in allkeys if k not in always)
    print(f'{eye}: renders {len(sets)}, patches per render {sorted(set(sum(s.values()) for s in sets))}, '
          f'distinct {len(allkeys)}, varying {len(varying)}')
    for k in varying[:16]:
        print('  ', k, ''.join(str(min(s[k], 9)) for s in sets))
# same render pair: left vs right
L = [Counter(p) for e, p in rows if e == 'left']
R = [Counter(p) for e, p in rows if e == 'right']
diffs = [len(set((a - b) + (b - a))) for a, b in zip(L, R)]
print('left vs right, patches differing per frame:', diffs)
