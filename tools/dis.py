#!/usr/bin/env python3
"""Disassemble game code from the exe image dump, with call targets resolved through the incremental-link jump thunks.
Usage: tools/dis.py <rva hex> [length]     (run from the repo root; image: re/battlefront-image.bin, base 0x540000)
Without a length it stops at the int3 padding that follows the function.
Absolute addresses inside the image are annotated {rva ...}.
Needs objdump: GNU objdump (present in the device shell) or llvm-objdump from the build toolchain (set TOOLCHAIN=<dir>/bin)."""
import os, re, shutil, struct, subprocess, sys, tempfile
IMG = 're/battlefront-image.bin'; BASE = 0x540000; TEXT = (0x1000, 0x40BC05)
img = open(IMG, 'rb').read()
def thunk(r):
    n = 0
    while TEXT[0] <= r < TEXT[1] and img[r] == 0xE9 and n < 3:
        r = (r + 5 + struct.unpack_from('<i', img, r + 1)[0]) & 0xFFFFFFFF; n += 1
    return r
def objdump(raw, a):
    tmp = tempfile.mkdtemp()
    b = os.path.join(tmp, 'c.bin'); open(b, 'wb').write(raw)
    tc = os.environ.get('TOOLCHAIN')
    llvm = tc and next((os.path.join(tc, n) for n in ('llvm-objdump', 'llvm-objdump.exe') if os.path.exists(os.path.join(tc, n))), None)
    if llvm:
        o = os.path.join(tmp, 'c.o')
        subprocess.check_call([os.path.join(tc, 'llvm-objcopy'), '-I', 'binary', '-O', 'elf32-i386', b, o])
        out = subprocess.check_output([llvm, '-D', '--no-show-raw-insn', '-M', 'intel', '--adjust-vma=%#x' % a, o]).decode()
    else:
        exe = shutil.which('objdump') or sys.exit('no objdump found (set TOOLCHAIN or install binutils)')
        out = subprocess.check_output([exe, '-D', '-b', 'binary', '-m', 'i386', '-M', 'intel', '--no-show-raw-insn', '--adjust-vma=%#x' % a, b]).decode()
    shutil.rmtree(tmp)
    return out
def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    a = int(sys.argv[1], 16)
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else img.find(b'\xCC\xCC', a + 1) - a
    for line in objdump(img[a:a + n], a).split('\n'):
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
        if not m: continue
        addr = int(m.group(1), 16); ins = re.sub(r'\s*<.*?>', '', m.group(2).strip())
        if not (a <= addr < a + n): continue
        ins = re.sub(r'\s+', ' ', ins)
        op = img[addr]
        if op in (0xE8, 0xE9):   # call / jmp rel32: recompute the target and follow thunks
            t = (addr + 5 + struct.unpack_from('<i', img, addr + 1)[0]) & 0xFFFFFFFF
            t2 = thunk(t)
            ins = '%s 0x%x%s' % ('call' if op == 0xE8 else 'jmp', t2, ' (via thunk)' if t2 != t else '')
        ins = re.sub(r'0x([0-9a-f]{6,8})\b', lambda mm: mm.group(0) + ('{rva %x}' % (int(mm.group(1), 16) - BASE) if BASE + 0x1000 <= int(mm.group(1), 16) < BASE + len(img) and op not in (0xE8, 0xE9) else ''), ins)
        print('%06x %s' % (addr, ins))
try:
    main()
except BrokenPipeError:
    pass
