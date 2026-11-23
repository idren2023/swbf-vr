#!/usr/bin/env python3
"""Replays src/GameBuild.cpp's address lookup over the Steam image dump (re/battlefront-image.bin, base 0x540000,
offset = RVA), with the patterns read from that file, so the DLL and this check use the same signatures.

For every address: the signature is searched in .text (as for an unknown build) and must match exactly once, at the
site in STEAM_ADDRESSES, and lead to the RVA there; the known-build check (match at the table's site) is replayed too.
Relative addresses are checked as base + offset. Exit code 1 if anything differs.
Usage: python tools/signatures.py   (from the repo root)"""
import re, struct, sys

IMG = 're/battlefront-image.bin'
SOURCE = 'src/GameBuild.cpp'
BASE = 0x540000
MAX_THUNKS = 3

img = open(IMG, 'rb').read()
src = open(SOURCE, encoding='utf-8').read()


def sections():
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    count = struct.unpack_from('<H', img, pe + 6)[0]
    optional = struct.unpack_from('<H', img, pe + 20)[0]
    size = struct.unpack_from('<I', img, pe + 24 + 56)[0]
    found, at = {}, pe + 24 + optional
    for _ in range(count):
        name = img[at:at + 8].rstrip(b'\0').decode()
        vsize, va = struct.unpack_from('<II', img, at + 8)
        found[name] = (va, va + vsize)
        at += 40
    return found, size


SECTIONS, IMAGE_SIZE = sections()
TEXT = SECTIONS['.text']
text = img[TEXT[0]:TEXT[1]]


def number(value):
    return int(value, 0)


definitions = []
for m in re.finditer(r'(Signature|Relative)\(Address::(\w+),\s*"(\w+)",\s*([^,]+),\s*(?:Find::(\w+)|Address::(\w+)),'
                     r'\s*(-?(?:0x[0-9A-Fa-f]+|\d+))(?:,\s*"([0-9A-F? ]+)")?\)', src):
    kind, enum, name, features, find, base, offset, pattern = m.groups()
    definitions.append(dict(enum=enum, name=name, features=features.strip(), find=find or 'Relative', base=base,
                            offset=number(offset), pattern=pattern))
steam = {m.group(1): (int(m.group(2), 16), int(m.group(3), 16))
         for m in re.finditer(r'\{Address::(\w+),\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)\}', src)}
enum_names = re.search(r'enum class Address\s*\{(.*?)\};', open('src/GameBuild.h').read(), re.S).group(1)
enum_names = [n for n in re.findall(r'^\s*(\w+),', enum_names, re.M) if n != 'Count']


def compile_pattern(pattern):
    tokens = pattern.split()
    # A lookahead finds overlapping matches too, as the DLL's byte-by-byte search does.
    body = b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in tokens)
    return re.compile(b'(?=' + body + b')', re.S), tokens


def matches_at(tokens, site):
    if site < TEXT[0] or site + len(tokens) > TEXT[1]:
        return False
    return all(t == '??' or img[site + i] == int(t, 16) for i, t in enumerate(tokens))


def resolve(find, site, offset):
    at = site + offset
    if find == 'Code':
        return at if TEXT[0] <= at < TEXT[1] else None
    if find == 'Pointer':
        value = struct.unpack_from('<I', img, at)[0]
        return value - BASE if BASE <= value < BASE + IMAGE_SIZE else None
    if find == 'CallTarget':
        if img[at] != 0xE8:
            return None
        target = at
        for step in range(MAX_THUNKS + 1):
            opcode = img[target]
            if (step == 0 and opcode != 0xE8) or (step > 0 and opcode != 0xE9):
                break
            target = (target + 5 + struct.unpack_from('<i', img, target + 1)[0]) & 0xFFFFFFFF
            if not TEXT[0] <= target < TEXT[1]:
                return None
        return target
    return None


def main():
    failures = 0
    found = {}
    order = [d['enum'] for d in definitions]
    if order != enum_names or sorted(steam) != sorted(enum_names):
        print('table order or names differ from enum Address')
        failures += 1
    print('%-26s %-10s %9s %9s %5s  %s' % ('address', 'find', 'rva', 'site', 'bytes', 'result'))
    for d in definitions:
        rva, site = steam.get(d['enum'], (None, None))
        if d['find'] == 'Relative':
            base = found.get(d['base'])
            got = base + d['offset'] if base is not None else None
            ok = got == rva
            print('%-26s %-10s %9x %9s %5s  %s' % (d['name'], 'Relative', rva, '-', '-',
                                                    'ok' if ok else 'FAIL: got %s' % (got and hex(got))))
            found[d['enum']] = got
            failures += not ok
            continue
        regex, tokens = compile_pattern(d['pattern'])
        hits = [TEXT[0] + x.start() for x in regex.finditer(text)]
        problems = []
        if len(hits) != 1:
            problems.append('%d matches' % len(hits))
        if hits and hits[0] != site:
            problems.append('match at %x, table site %x' % (hits[0], site))
        got = resolve(d['find'], hits[0], d['offset']) if len(hits) == 1 else None
        if got != rva:
            problems.append('resolves to %s, table %x' % (got and hex(got), rva))
        if not matches_at(tokens, site) or resolve(d['find'], site, d['offset']) != rva:
            problems.append('known-build check fails')
        if tokens[0] == '??':
            problems.append('first byte must be fixed')
        if len(tokens) > 64:
            problems.append('longer than 64 bytes')
        found[d['enum']] = got
        failures += bool(problems)
        print('%-26s %-10s %9x %9x %5d  %s' % (d['name'], d['find'], rva, site, len(tokens),
                                                'ok (unique)' if not problems else 'FAIL: ' + '; '.join(problems)))
    print('%d addresses, %d failures' % (len(definitions), failures))
    sys.exit(1 if failures else 0)


main()
