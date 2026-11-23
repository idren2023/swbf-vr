#!/usr/bin/env python3
"""Describe code addresses in the exe image dump: enclosing function, vtable membership, strings it references.
Usage: tools/sym.py <rva hex> [...]        (image: re/battlefront-image.bin, dumped at base 0x540000)
Function boundaries are heuristic: the build pads between functions with int3 (CC) bytes."""
import struct, sys, re, os, pickle
IMG = 're/battlefront-image.bin'; BASE = 0x540000
TEXT = (0x1000, 0x40BC05); RDATA = (0x40C000, 0x476CAD); DATA = (0x477000, 0x21BBB60)
img = open(IMG, 'rb').read()
u32 = lambda o: struct.unpack_from('<I', img, o)[0]
def thunk_target(rva):
    """If rva is an incremental-link jump thunk (jmp rel32), return its destination, else rva."""
    seen = 0
    while TEXT[0] <= rva < TEXT[1] and img[rva] == 0xE9 and seen < 3:
        rva = (rva + 5 + struct.unpack_from('<i', img, rva + 1)[0]) & 0xFFFFFFFF; seen += 1
    return rva
def func_start(rva):
    p = rva
    while p > TEXT[0] + 2:
        if img[p-1] == 0xCC and img[p-2] == 0xCC and img[p] != 0xCC: return p
        if img[p-1] == 0xCC and img[p] != 0xCC and img[p-2] in (0xC3,) : return p
        p -= 1
    return TEXT[0]
def func_end(start):
    p = img.find(b'\xCC\xCC', start + 1)
    return p if p > 0 else TEXT[1]
def load_index():
    cache = 're/sym-cache.pkl'
    if os.path.exists(cache): return pickle.load(open(cache, 'rb'))
    calls = {}
    p = img.find(b'\xE8', TEXT[0])
    while 0 < p < TEXT[1] - 5:
        t = (p + 5 + struct.unpack_from('<i', img, p + 1)[0]) & 0xFFFFFFFF
        if TEXT[0] <= t < TEXT[1]: calls.setdefault(thunk_target(t), []).append(p)
        p = img.find(b'\xE8', p + 1)
    vt = {}
    for a in range(RDATA[0], RDATA[1] - 8, 4):
        col = u32(a) - BASE
        if not (RDATA[0] <= col <= RDATA[1] - 20) or col & 3 or u32(col) != 0 or u32(col + 4) >= 0x10000: continue
        td = u32(col + 12) - BASE
        if not (0 <= td < len(img) - 64) or img[td+8:td+11] != b'.?A': continue
        name = img[td+12:td+12+60].split(b'@@')[0].decode('latin1'); off = u32(col + 4)
        s = a + 4; i = 0
        while s + 4 <= RDATA[1] and TEXT[0] <= u32(s) - BASE < TEXT[1] and (i == 0 or not (RDATA[0] <= u32(s-4) - BASE < RDATA[1])):
            vt.setdefault(thunk_target(u32(s) - BASE), []).append('%s%s[%d]' % (name, '+0x%x' % off if off else '', i)); s += 4; i += 1
    pickle.dump((calls, vt), open(cache, 'wb')); return calls, vt
def strings_in(start, end):
    out = []
    for m in re.finditer(rb'[\x68\xB8-\xBF\x05\x0D](....)', img[start:end], re.S):
        v = struct.unpack('<I', m.group(1))[0] - BASE
        if RDATA[0] <= v < DATA[0] + 0x27a00:
            s = img[v:v+80].split(b'\0')[0]
            if len(s) >= 5 and all(32 <= c < 127 for c in s) and s.decode() not in out: out.append(s.decode())
    return out
def describe(rva, calls, vt):
    f = func_start(rva); e = func_end(f)
    names = vt.get(f, [])
    cs = calls.get(f, [])
    return f, e, names, cs, strings_in(f, e)
if __name__ == '__main__':
    calls, vt = load_index()
    for a in sys.argv[1:]:
        rva = int(a, 16); f, e, names, cs, st = describe(rva, calls, vt)
        print('%#x in func %#x..%#x (%d bytes) callers=%d %s' % (rva, f, e, e - f, len(cs), ' '.join(names[:4])))
        if st: print('    strings: ' + ' | '.join(st[:6]))
