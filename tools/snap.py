#!/usr/bin/env python3
"""Reader for swbf-vr-snap.bin (written by src/EngineSurvey.cpp).
load(path) -> list of (tick, frame, [ (start, depth, frm, fromOff, name, bytes) ... ])"""
import struct
def load(path):
    d = open(path, 'rb').read(); o = 0; out = []
    while o + 16 <= len(d) and d[o:o+4] == b'SNP1':
        tick, frame, cnt = struct.unpack_from('<III', d, o + 4); o += 16; objs = []
        for _ in range(cnt):
            st, dep, fr, fo, ln = struct.unpack_from('<IIIII', d, o)
            nm = d[o+20:o+68].split(b'\0')[0].decode(); objs.append((st, dep, fr, fo, nm, d[o+68:o+68+ln])); o += 68 + ln
        out.append((tick, frame, objs))
    return out
def marks(logpath):
    m = []
    for l in open(logpath, errors='replace'):
        p = l.split()
        if len(p) >= 3 and p[1] == 'MARK': m.append((int(p[0]), ' '.join(p[2:])))
    return m
