#!/usr/bin/env python3
"""Extract files from NFSU2 PS2 ZZDATA*.BIN using ZDIR.BIN and a names TSV.

Usage: zz_extract.py NFSUNDER_DIR names.tsv OUT_DIR [--list]
Unnamed entries go to OUT_DIR/_unnamed/<hash>_<n>.<magic-ext>.
Entries over 64 MiB (disc padding / streams) are skipped unless --big.
"""
import os
import sys

SECTOR = 2048
MAGIC_EXT = {b'\x00\x41\x03\x00': 'chunk', b'SCHl': 'asf', b'SHEN': 'asf', b'MVhd': 'vp6', b'\x00\x00\x01\xba': 'pss',
             b'JDLZ': 'lzc', b'COMP': 'lzc', b'BKHD': 'abk', b'ABKC': 'abk'}


def guess_ext(head):
    for m, e in MAGIC_EXT.items():
        if head.startswith(m):
            return e
    return 'bin'


def main():
    root, tsv, out = sys.argv[1:4]
    big = '--big' in sys.argv
    listing = '--list' in sys.argv
    zz = [open(os.path.join(root, f'ZZDATA{i}.BIN'), 'rb') for i in range(4)]
    seen = {}
    for line in open(tsv):
        h, a, sec, size, chk, name = line.rstrip('\n').split('\t')
        a, sec, size = int(a), int(sec), int(size)
        f = zz[a]
        f.seek(sec * SECTOR)
        head = f.read(16)
        if listing:
            print(f'{h} zz{a} {sec:>7} {size:>10} {head[:4].hex()} {name}')
            continue
        if size == 0 or (size > 64 << 20 and not big):
            continue
        n = seen.get(h, 0)
        seen[h] = n + 1
        if name:
            rel = name.replace('\\', '/')
            if n:
                rel += f'.dup{n}'
        else:
            rel = f'_unnamed/{h}_{n}.{guess_ext(head)}'
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        f.seek(sec * SECTOR)
        with open(dst, 'wb') as o:
            left = size
            while left:
                buf = f.read(min(left, 1 << 20))
                o.write(buf)
                left -= len(buf)


if __name__ == '__main__':
    main()
