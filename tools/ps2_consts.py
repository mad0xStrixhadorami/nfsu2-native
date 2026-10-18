#!/usr/bin/env python3
"""Resolves the constants a Ghidra pseudocode file references (fGp/uGp gp-relative and DAT_ absolute)
to their values in SLES_527.25 (gp = 0x5695f0). Usage: ps2_consts.py ELF file.c [...]"""
import re, struct, sys

GP = 0x5695F0
elf = open(sys.argv[1], 'rb').read()
ph = struct.unpack_from('<I', elf, 0x1c)[0]
_, off, base, _, filesz, memsz = struct.unpack_from('<IIIIII', elf, ph)

def word(a):
    if base <= a < base + filesz - 3:
        return struct.unpack_from('<I', elf, off + a - base)[0]
    return None  # bss: set at run time

for path in sys.argv[2:]:
    src = open(path).read()
    seen = set()
    for m in re.finditer(r'\b([fu])Gp(f{3,}[0-9a-f]+)|\bDAT_([0-9a-f]{8})|_DAT_([0-9a-f]{8})', src):
        if m.group(2):
            a = (GP + (int(m.group(2), 16) - (1 << 32))) & 0xffffffff
            name = m.group(0)
        else:
            a = int(m.group(3) or m.group(4), 16)
            name = 'DAT_%08x' % a
        if name in seen:
            continue
        seen.add(name)
        w = word(a)
        if w is None:
            print('%s  %-16s @%08x  (bss)' % (path.split('/')[-1], name, a))
        else:
            f = struct.unpack('<f', struct.pack('<I', w))[0]
            print('%s  %-16s @%08x  0x%08x  %g' % (path.split('/')[-1], name, a, w, f))
