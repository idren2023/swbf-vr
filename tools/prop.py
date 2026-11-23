#!/usr/bin/env python3
"""ODF property name -> where the exe parses it.
Usage: tools/prop.py <name or regex> [...]      (case-insensitive, matches property name or source file)
Reads re/secretsociety/BF1_case_hex_lines.txt (source file, line, 32-bit name hash per property) and searches
re/battlefront-image.bin (.text only) for the hash as a 4-byte immediate. Prints image offsets (RVA)."""
import re, struct, sys, os
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're')
rows = []
for l in open(os.path.join(R, 'secretsociety', 'BF1_case_hex_lines.txt'), errors='replace'):
    m = re.match(r'\s*(\S+?)\((\d+)\):\s*case\s+0x([0-9a-fA-F]{1,8})\s*:?\s*/\*\s*"([^"]*)"', l)
    if m: rows.append((m.group(1), int(m.group(2)), int(m.group(3), 16), m.group(4)))
img = open(os.path.join(R, 'battlefront-image.bin'), 'rb').read()[:0x40d000]
def find(h):
    p = struct.pack('<I', h); out = []; i = 0
    while True:
        i = img.find(p, i)
        if i < 0: return out
        out.append(i); i += 1
if len(sys.argv) < 2: sys.exit(__doc__)
for a in sys.argv[1:]:
    rx = re.compile(a, re.I)
    for f, ln, h, n in rows:
        if rx.search(n) or rx.search(f):
            print('%-28s %-26s %5d  0x%08x  %s' % (n, f, ln, h, ' '.join('+0x%x' % x for x in find(h)) or '-'))
