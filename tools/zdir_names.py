#!/usr/bin/env python3
"""Recover ZDIR.BIN entry names by hashing candidate paths with bStringHash.

ZDIR.BIN record (24 bytes, little endian, sorted by hash):
  u32 name_hash     bStringHash(upper(path))
  u32 archive       ZZDATA<n>.BIN
  u32 sector        sector offset inside ZZDATA<n>
  u32 global_sector sector offset across ZZDATA0..3 concatenated
  u32 size          bytes
  u32 checksum      unknown hash of contents (not a name hash)
Usage: zdir_names.py ZDIR.BIN SLES_527.25 [extra_names.txt] > names.tsv
"""
import re
import struct
import sys


def bstringhash(s):
    h = 0xFFFFFFFF
    for c in s.upper().encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def elf_strings(path, minlen=2):
    data = open(path, 'rb').read()
    return [m.group().decode() for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, data)]


def candidates(strs):
    tokens = {s for s in strs if re.fullmatch(r'[A-Za-z0-9_\-]{1,40}', s)}
    # Pieces of longer identifiers too, e.g. "350Z" from "350Z_BODY"
    for s in list(tokens):
        tokens.update(p for p in re.split(r'[_\-]', s) if p)
    nums = [str(i) for i in range(1000)] + ['%02d' % i for i in range(100)] + ['%03d' % i for i in range(1000)]
    small = tokens | set(nums) | {''}
    for s in strs:
        if '\\' not in s and '/' not in s:
            continue
        if '%' not in s:
            yield s
            continue
        n = len(re.findall(r'%[0-9]*[sd]', s))
        if n == 1:
            pool = nums if re.search(r'%[0-9]*d', s) else small
            for t in pool:
                yield re.sub(r'%[0-9]*[sd]', lambda _: t, s, count=1)
        elif n == 2:
            # Two-slot templates: cross every token with a tiny suffix set only.
            short = [''] + [str(i) for i in range(20)] + ['_' + t for t in ('EN', 'FR', 'GE', 'IT', 'SP', 'DU', 'SW', 'DA', 'PAL', 'NTSC')]
            for a in small:
                for b in short:
                    it = iter((a, b))
                    yield re.sub(r'%[0-9]*[sd]', lambda _: next(it), s)


def dir_cross(strs, named):
    """Pair every bare file name in the ELF with every directory seen so far."""
    dirs = {n.rsplit('\\', 1)[0] for n in named if '\\' in n}
    dirs |= {d.rsplit('\\', 1)[0] for d in dirs if '\\' in d}
    files = {s for s in strs if re.fullmatch(r'[A-Za-z0-9_\-]+\.[A-Za-z0-9]{2,4}', s)}
    files |= {n.rsplit('\\', 1)[1] for n in named if '\\' in n}
    for d in dirs:
        for f in files:
            yield d + '\\' + f


def main():
    zdir, elf = sys.argv[1], sys.argv[2]
    raw = open(zdir, 'rb').read()
    entries = [struct.unpack_from('<6I', raw, i) for i in range(0, len(raw), 24)]
    want = {e[0] for e in entries}
    strs = elf_strings(elf)
    if len(sys.argv) > 3:
        strs += [l.strip() for l in open(sys.argv[3]) if l.strip()]
    found = {}
    for c in candidates(strs):
        h = bstringhash(c)
        if h in want and h not in found:
            found[h] = c.upper()
    while True:
        before = len(found)
        for c in dir_cross(strs, list(found.values())):
            h = bstringhash(c)
            if h in want and h not in found:
                found[h] = c.upper()
        if len(found) == before:
            break
    for h, a, sec, gsec, size, chk in entries:
        print(f'{h:08x}\t{a}\t{sec}\t{size}\t{chk:08x}\t{found.get(h, "")}')
    print(f'# named {len(found)}/{len(want)} unique hashes', file=sys.stderr)


if __name__ == '__main__':
    main()
