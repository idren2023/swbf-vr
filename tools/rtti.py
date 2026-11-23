#!/usr/bin/env python3
"""List MSVC RTTI classes and their vtables from an in-memory image dump of Battlefront.exe.
Usage: tools/rtti.py re/battlefront-image.bin <exe base in hex, from swbf-vr.log> [name filter regex]
Output: class, vtable RVA, offset of that vtable within the object, method count, first method RVAs."""
import re, struct, sys
img = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16)
flt = re.compile(sys.argv[3], re.I) if len(sys.argv) > 3 else None
TEXT = (0x1000, 0x40BC05); RDATA = (0x40C000, 0x477000)
u32 = lambda o: struct.unpack_from('<I', img, o)[0]
tds = {m.start() - 8 + base: m.group(1).decode()
       for m in re.finditer(rb'\.\?AV([^@\0]+(?:@[^@\0]+)*)@@\0', img)}
cols = {}  # COL VA -> (class, offset in object)
for o in range(RDATA[0], RDATA[1] - 16, 4):
    if u32(o) == 0 and u32(o + 12) in tds:
        cols[o + base] = (tds[u32(o + 12)], u32(o + 4))
rows = []
for o in range(RDATA[0], RDATA[1] - 8, 4):
    if u32(o) in cols:
        name, off = cols[u32(o)]
        fns, p = [], o + 4
        while p < RDATA[1] and TEXT[0] <= u32(p) - base < TEXT[1] and (p == o + 4 or u32(p - 4) not in cols):
            fns.append(u32(p) - base); p += 4
        if fns: rows.append((name, o + 4, off, fns))
for name, vt, off, fns in sorted(rows):
    if flt and not flt.search(name): continue
    print(f"{name}\tvtbl=0x{vt:x}\tobjoff=0x{off:x}\tn={len(fns)}\t" + ' '.join(f'{f:x}' for f in fns[:6]))
