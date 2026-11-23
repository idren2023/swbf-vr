"""python tools/inccompare.py a.inc b.inc: compares two generated StockVertex tables as sets of triangles (order free,
positions to the millimetre, texture positions to 1/1000); prints per table what is only in one of them."""
import re
import sys
from collections import Counter

ROW = re.compile(r'\{\{([-\d.e]+)f, ([-\d.e]+)f, ([-\d.e]+)f\}, \{([-\d.e]+)f, ([-\d.e]+)f, ([-\d.e]+)f\}, '
                 r'([-\d.e]+)f, ([-\d.e]+)f, (\d+)\}')


def tables(path):
    out, name = {}, None
    rows = []
    for line in open(path):
        m = re.match(r'constexpr StockVertex (\w+)\[', line)
        if m:
            name, rows = m.group(1), []
            out[name] = rows
            continue
        r = ROW.search(line)
        if r and name:
            v = [float(x) for x in r.groups()]
            rows.append((round(v[0], 3), round(v[1], 3), round(v[2], 3), round(v[6], 3), round(v[7], 3), int(v[8])))
    return {k: Counter(tuple(sorted(rows[i:i + 3])) for i in range(0, len(rows) - 2, 3)) for k, rows in out.items()}


a, b = tables(sys.argv[1]), tables(sys.argv[2])
for name in a.keys() | b.keys():
    ta, tb = a.get(name, Counter()), b.get(name, Counter())
    only_a, only_b = ta - tb, tb - ta
    # the rest matched within a tolerance (positions 2 mm, texture positions 0.002), in any corner order
    rest_b = list(only_b.elements())
    worst, left_a = 0.0, Counter()
    for t in only_a.elements():
        best, bi = None, -1
        for i, u in enumerate(rest_b):
            d = max(abs(x - y) for p, q in zip(t, u) for x, y in zip(p[:5], q[:5]))
            if t[0][5] != u[0][5]:
                continue
            if best is None or d < best:
                best, bi = d, i
        if best is not None and best <= 0.002:
            worst = max(worst, best)
            rest_b.pop(bi)
        else:
            left_a[t] += 1
    only_a, only_b = left_a, Counter(rest_b)
    print('%-28s %6d vs %6d triangles: %5d only in first, %5d only in second (others within %.4f)' % (
        name, sum(ta.values()), sum(tb.values()), sum(only_a.values()), sum(only_b.values()), worst))
    for t in list(only_b)[:2] if len(sys.argv) > 3 else []:
        print('   second only:', t)
    for t in list(only_a)[:2] if len(sys.argv) > 3 else []:
        print('   first only:', t)
