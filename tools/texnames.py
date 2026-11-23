#!/usr/bin/env python3
"""Name the texture hashes of a log's "textures:" lines (Textures.cpp).

Usage: python tools/texnames.py <swbf-vr.log> [lvl dir] [log subsystem, default textures]
Hashes: textures FNV-1a over the name, each byte | 0x20 (the game's 0x3008F0); model nodes and bones the game's
HashName 0x3007F0 (CRC-32). Run from the repo root. Candidate names are the printable strings
(3-63 characters) in the game's .lvl files (default: GameData/Data/_lvl_pc, all subfolders) and the BFBuilder
assets' file names. Prints each entry / lookup line with its name, and a summary of unnamed hashes.
"""
import os, re, sys

def name_hash(name):
    value = 0x811C9DC5
    for byte in name.encode('latin-1'):
        value = ((value ^ (byte | 0x20)) * 0x01000193) & 0xFFFFFFFF
    return value

_CRC = None

def node_hash(name):
    """The game's HashName 0x3007F0 (model nodes, bones): CRC-32, MSB first, table at 0x48E300, lower-cased A-Z."""
    global _CRC
    if _CRC is None:
        import struct
        image = open('re/battlefront-image.bin', 'rb').read()
        _CRC = struct.unpack_from('<256I', image, 0x48E300)
    value = 0xFFFFFFFF
    for byte in name.encode('latin-1'):
        if 0x41 <= byte <= 0x5A:
            byte += 0x20
        value = ((value << 8) & 0xFFFFFFFF) ^ _CRC[(value >> 24) ^ byte]
    return value ^ 0xFFFFFFFF

def candidates(lvl_dir):
    names = set()
    for root, _, files in os.walk(lvl_dir):
        for file in files:
            if file.lower().endswith('.lvl'):
                data = open(os.path.join(root, file), 'rb').read()
                names.update(m.decode('latin-1') for m in re.findall(rb'[A-Za-z0-9_\-\.]{3,63}', data))
    for root, _, files in os.walk('C:/LucasArts/BFBuilder'):
        for file in files:
            names.add(os.path.splitext(file)[0])
    return names

def main():
    log = sys.argv[1]
    tag = (sys.argv[3] if len(sys.argv) > 3 else 'textures') + ':'
    lvl_dir = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != '-' else (
        'C:/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront (Classic 2004)/GameData/Data/_lvl_pc')
    table = {}
    for name in candidates(lvl_dir):
        table.setdefault(name_hash(name), name)
        table.setdefault(node_hash(name), name)
    unnamed = 0
    for line in open(log, encoding='latin-1'):
        if tag not in line:
            continue
        text = line.rstrip().split(tag, 1)[1].strip()
        names = []
        for found in re.findall(r'0x([0-9a-f]{8})', text):
            name = table.get(int(found, 16))
            unnamed += name is None
            names.append(name or '?')
        print(text + ('  = ' + ' '.join(names) if names else ''))
    print('unnamed:', unnamed, file=sys.stderr)

main()
