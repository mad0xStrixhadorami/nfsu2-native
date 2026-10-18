#!/usr/bin/env python3
"""Check the PS2 mesh strip-restart model against eStripEntry::NumPolys (ISSUES.md #110).

The mesh VU program (MSCAL 0) seeds the VU random-number register R from the four words unpacked
to VU address 1 and steps it once per vertex; bit 15 of each result is the GS ADC ("no kick") bit
of that vertex. Seeds: .x before the first call, then the delay slots of the JALRs at VU
0x1568 / 0x15a8 / 0x15e8 re-seed from .x / .z / .w. RNEXT runs 7 times on the first call and 4
times on every later call. Calls start at JALR 0x1618 - 0x10 * (groups - 1).
Usage: strip_topology.py GEOMETRY.BIN|STREAM.BUN"""
import collections
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from vif_dump import find_meshes  # noqa: E402


def seeds(d, start, size):
    p, end = start, start + size
    while p + 4 <= end:
        imm, num, cmd = struct.unpack_from('<HBB', d, p)
        p += 4
        c = cmd & 0x7f
        if c >= 0x60:
            vn, vl = (c >> 2) & 3, c & 3
            comps, bits = vn + 1, {0: 32, 1: 16, 2: 8, 3: 5}[vl]
            if (imm & 0x3ff) == 1 and bits == 32 and comps == 4:
                return struct.unpack_from('<4I', d, p)
            elem = bits * comps // 8 if bits != 5 else 2
            p += (elem * (num or 256) + 3) & ~3
        elif c == 0x14:
            break
    return (0, 0, 0, 0)


def restart_flags(seed, nverts):
    def rinit(w):
        return 0x3f800000 | (w & 0x7fffff)
    groups = (nverts + 3) // 4
    r, out = rinit(seed[0]), []
    reseed = {0x1568: 0, 0x15a8: 2, 0x15e8: 3}
    for call in range(groups):
        jalr = 0x1618 - 0x10 * (groups - 1) + 0x10 * call
        if jalr in reseed:
            r = rinit(seed[reseed[jalr]])
        for _ in range(7 if call == 0 else 4):
            x, y = (r >> 4) & 1, (r >> 22) & 1
            r = (((r << 1) ^ x ^ y) & 0x7fffff) | 0x3f800000
            out.append((r >> 15) & 1)
    return out[:nverts]


def main():
    d = open(sys.argv[1], 'rb').read()
    result = collections.Counter()
    for mesh in find_meshes(d):
        b01, s01 = mesh[0x134a01]
        b02, _ = mesh[0x134a02]
        while d[b02] == 0x11:
            b02 += 1
        for i in range((s01 - 4) // 16):
            _, nv, npl, _, dq, _, _, dqs, _, _, _ = struct.unpack_from('<HBBfHbbBBbb', d, b01 + 4 + i * 16)
            flags = restart_flags(seeds(d, b02 + dq * 16, dqs * 16), nv)
            result[sum(1 for k in range(2, nv) if not flags[k]) == npl] += 1
    print(f'strips whose kicks equal NumPolys: {result[True]} of {sum(result.values())}')


if __name__ == '__main__':
    main()
