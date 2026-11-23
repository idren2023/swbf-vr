#!/usr/bin/env python3
"""Find code that mentions an address as a 32-bit constant (globals, tables, vtables) and list direct callers of a function.
Usage: tools/refs.py <rva hex> ...        (run from the repo root; image: re/battlefront-image.bin, base 0x540000)
For each RVA: "refs" = places in .text holding the absolute address (location and enclosing function start, heuristic);
"callers" = call sites reaching it directly or through a jump thunk (from re/sym-cache.pkl if tools/sym.py has built it)."""
import os, pickle, struct, sys
IMG = 're/battlefront-image.bin'; BASE = 0x540000; TEXT = (0x1000, 0x40BC05)
img = open(IMG, 'rb').read()
def fstart(p):
    while p > TEXT[0] + 2:
        if img[p-1] == 0xCC and img[p] != 0xCC and img[p-2] in (0xCC, 0xC3): return p
        p -= 1
    return p
calls = pickle.load(open('re/sym-cache.pkl', 'rb'))[0] if os.path.exists('re/sym-cache.pkl') else {}
if len(sys.argv) < 2: sys.exit(__doc__)
for arg in sys.argv[1:]:
    rva = int(arg, 16); pat = struct.pack('<I', BASE + rva); p = TEXT[0]; out = []
    while True:
        p = img.find(pat, p, TEXT[1])
        if p < 0: break
        out.append('%x(in %x)' % (p, fstart(p))); p += 1
    print('%x refs: %s' % (rva, ' '.join(out) or '-'))
    if rva in calls: print('%x callers: %s' % (rva, ' '.join('%x' % c for c in calls[rva])))
