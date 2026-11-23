#!/usr/bin/env python3
"""Find rotation matrices, positions and other changing floats inside snapshotted objects.
Usage: tools/find_matrices.py <run dir> [class name filter]"""
import sys, struct, math, collections
sys.path.insert(0, 'tools'); import snap
run = sys.argv[1]; flt = sys.argv[2] if len(sys.argv) > 2 else 'Camera'
S = snap.load(run + '/swbf-vr-snap.bin'); M = snap.marks(run + '/swbf-vr.log')
series = collections.defaultdict(list)
for t, f, objs in S:
    for st, dep, fr, fo, nm, b in objs:
        if flt in nm: series[(nm, st)].append((t, b))
def fl(b, o, n): return struct.unpack_from('<%df' % n, b, o)
def ortho(v, stride):
    r = [v[i*stride:i*stride+3] for i in range(3)]
    for i in range(3):
        if abs(sum(x*x for x in r[i]) - 1) > 2e-3: return False
    return all(abs(sum(a*b for a, b in zip(r[i], r[j]))) < 2e-3 for i, j in ((0,1),(0,2),(1,2)))
for (nm, st), ser in sorted(series.items(), key=lambda k: -len(k[1])):
    if len(ser) < 50: continue
    n = min(len(b) for t, b in ser)
    print(f'== {nm} @{st:#x} samples={len(ser)}')
    for stride in (3, 4):
        for off in range(4, n - 4*(2*stride+3), 4):
            ok = 0; vals = set()
            for t, b in ser[::7]:
                v = fl(b, off, 2*stride+3)
                if all(math.isfinite(x) and abs(x) <= 1.001 for x in v) and ortho(v, stride): ok += 1; vals.add(v[:3])
            if ok >= 0.9 * len(ser[::7]):
                print(f'  rot3x3 at +{off:#x} stride {stride}: distinct={len(vals)}')
