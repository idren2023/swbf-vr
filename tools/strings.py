"""Strings in the exe image that match a pattern, with their RVA (re/battlefront-image.bin, base 0x540000, file offset =
RVA). Usage: python -I tools/strings.py <regex> [--min 4] [--most 80] [--wide]
ASCII by default; --wide also searches UTF-16LE. Prints `rva  text`, at most --most lines. Follow a string to the code
that uses it with tools/refs.py <rva>."""
import re
import sys

IMAGE = 're/battlefront-image.bin'
args = sys.argv[1:]
if not args:
    print(__doc__)
    sys.exit(1)
pattern = re.compile(args[0].encode() if True else b'', re.I)
least = int(args[args.index('--min') + 1]) if '--min' in args else 4
most = int(args[args.index('--most') + 1]) if '--most' in args else 80
data = open(IMAGE, 'rb').read()
shown = 0
for m in re.finditer(rb'[\x20-\x7e]{%d,}' % least, data):
    text = m.group()
    if pattern.search(text):
        print('%08x  %s' % (m.start(), text.decode('ascii')))
        shown += 1
        if shown >= most:
            break
if '--wide' in args and shown < most:
    wide = re.compile(args[0], re.I)
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % least, data):
        text = m.group().decode('utf-16-le')
        if wide.search(text):
            print('%08x  (wide) %s' % (m.start(), text))
            shown += 1
            if shown >= most:
                break
print('%d shown' % shown)
