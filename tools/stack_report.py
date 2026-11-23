#!/usr/bin/env python3
"""Symbolise STACK lines of a swbf-vr.log. Usage: tools/stack_report.py <log> [name filter regex]"""
import sys, re, importlib.util
spec = importlib.util.spec_from_file_location('sym', 'tools/sym.py'); sym = importlib.util.module_from_spec(spec); spec.loader.exec_module(sym)
calls, vt = sym.load_index(); flt = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None; seen = set()
for l in open(sys.argv[1], errors='replace'):
    if ' STACK ' not in l: continue
    head, _, rest = l.partition(' STACK ')[2].partition(':')
    if flt and not flt.search(head): continue
    key = head.split()[0]
    if key in seen: continue
    seen.add(key); out = []
    for a in rest.split():
        r = int(a, 16); f = sym.func_start(r); n = vt.get(f, [])
        s = '%x%s' % (f, '{' + n[0] + '}' if n else '')
        if not out or out[-1] != s: out.append(s)
    print(head.strip()[:110]); print('    ' + ' < '.join(out))
