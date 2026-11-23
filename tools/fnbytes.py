# Prints the bytes of functions of the Steam image dump (re/battlefront-image.bin, base 0x540000, offset = RVA) as
# base64, one line per function: "<rva hex> <base64>". The end is the first run of 0xCC padding after a ret.
# Usage: python3 tools/fnbytes.py <rva hex> [...]   (output is disassembled with tools/dis.sh in the build environment)
import sys, base64, re
d = open('re/battlefront-image.bin', 'rb').read()
for a in sys.argv[1:]:
    a = int(a, 16)
    m = re.search(rb'(\xc3|\xc2..)\xcc{2,}', d[a:a + 0x3000], re.S)
    n = m.end(1) if m else 0x400
    print('%x %s' % (a, base64.b64encode(d[a:a + n]).decode()))
