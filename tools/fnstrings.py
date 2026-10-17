#!/usr/bin/env python3
"""Show string literals referenced by a function (and its callees down to DEPTH), using the
Ghidra pseudocode dump and the ELF. Helps identify unnamed functions.
Usage: fnstrings.py REFDIR ELF DEPTH ADDR [ADDR...]"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from callgraph import load  # noqa: E402


def elf_loader(path):
    data = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', data, 0x1c)
    phnum, = struct.unpack_from('<H', data, 0x2c)
    segs = []
    for i in range(phnum):
        _, off, vaddr, _, filesz, _, _, _ = struct.unpack_from('<8I', data, phoff + i * 32)
        segs.append((vaddr, off, filesz))

    def string_at(va):
        for v, off, size in segs:
            if v <= va < v + size:
                o = off + va - v
                end = data.find(b'\0', o, o + 200)
                if end <= o:
                    return None
                s = data[o:end]
                if len(s) >= 3 and all(32 <= b < 127 for b in s):
                    return s.decode()
        return None
    return string_at


def main():
    refdir, elf, depth = sys.argv[1], sys.argv[2], int(sys.argv[3])
    names, sizes, calls = load(refdir)
    string_at = elf_loader(elf)
    for root in (int(a, 16) for a in sys.argv[4:]):
        seen, frontier, found = {root}, [root], []
        for _ in range(depth + 1):
            nxt = []
            for a in frontier:
                path = os.path.join(refdir, f'{a:08x}.c')
                if os.path.exists(path):
                    for m in re.finditer(r'0x([0-9a-f]{6})\b', open(path).read()):
                        s = string_at(int(m.group(1), 16))
                        if s and s not in found:
                            found.append(s)
                for c in calls.get(a, []):
                    if c not in seen:
                        seen.add(c)
                        nxt.append(c)
            frontier = nxt
        print(f'{root:08x} {names.get(root, "?")}: ' + ' | '.join(found[:8]))


if __name__ == '__main__':
    main()
