#!/usr/bin/env python3
"""List the career hotspots (shops, crib, ...) of a region's PathsFreeRoam.bin: type-14 records of chunk 0x3414a.
Usage: hotspots.py PATHSFREEROAM.BIN GLOBALB.BUN   (ISSUES.md #269)
Record (100 bytes): u32 type 14 at -0x30 from the name hash, float x, y, dir x, y, width, ... then the u32 name hash (the
second hash of the GLOBALB 0x34a12 shop record, record+0x3c)."""
import struct, sys

def shops(g):
    o = 0x15ce1c
    out = {}
    # locate chunk 0x34a12 generically
    def walk(o, e, dep=0):
        while o + 8 <= e:
            c, s = struct.unpack_from('<II', g, o)
            if c == 0x34a12:
                return o + 8, s
            if c & 0x80000000 and dep < 3:
                r = walk(o + 8, o + 8 + s, dep + 1)
                if r:
                    return r
            o += 8 + s
    base, size = walk(0, len(g))
    for i in range(size // 160):
        r = g[base + i * 160: base + (i + 1) * 160]
        out[struct.unpack_from('<I', r, 0x3c)[0]] = (r[:0x20].split(b'\0')[0].decode(), r[0x40:0x58].split(b'\0')[0].decode(), r[0x9c])
    return out

def main():
    d = open(sys.argv[1], 'rb').read()
    g = open(sys.argv[2], 'rb').read()
    sh = shops(g)
    for j in range(0, len(d) - 4):
        k, = struct.unpack_from('<I', d, j)
        if k in sh and j >= 0x30 and struct.unpack_from('<I', d, j - 0x30)[0] == 14:
            x, y, dx, dy, w = struct.unpack_from('<5f', d, j - 0x2c)
            print('%-22s %-16s kind %d  x %.1f y %.1f dir (%.2f,%.2f) z %.1f' % (sh[k][0], sh[k][1], sh[k][2], x, y, dx, dy, w))

main()
