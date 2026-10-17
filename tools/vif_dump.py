#!/usr/bin/env python3
"""Decode PS2 VIF command streams in an EA solid mesh (chunk 0x134a02) for inspection.
Usage: vif_dump.py GEOMETRY.BIN [mesh_index] [strips]"""
import struct
import sys

UNPACK_FMT = {0x0: ('S', 32, 1), 0x1: ('S', 16, 1), 0x2: ('S', 8, 1), 0x4: ('V2', 32, 2), 0x5: ('V2', 16, 2),
              0x6: ('V2', 8, 2), 0x8: ('V3', 32, 3), 0x9: ('V3', 16, 3), 0xa: ('V3', 8, 3), 0xc: ('V4', 32, 4),
              0xd: ('V4', 16, 4), 0xe: ('V4', 8, 4), 0xf: ('V4', 5, 4)}


def chunks(d, off, end):
    while off + 8 <= end:
        cid, sz = struct.unpack_from('<II', d, off)
        yield cid, off + 8, sz
        off += 8 + sz


def find_meshes(d):
    out = []

    def walk(off, end):
        for cid, body, sz in chunks(d, off, end):
            if cid == 0x80134100:
                parts = {c: (b, s) for c, b, s in chunks(d, body, body + sz)}
                out.append(parts)
            elif cid & 0x80000000:
                walk(body, body + sz)
    walk(0, len(d))
    return out


def decode(data, start, size, max_cmds=60):
    p, end, n = start, start + size, 0
    while p + 4 <= end and n < max_cmds:
        imm, num, cmd = struct.unpack_from('<HBB', data, p)
        p += 4
        n += 1
        c = cmd & 0x7f
        if c >= 0x60:
            vn, vl = (c >> 2) & 3, c & 3
            key = (vn << 2) | vl
            name, bits, comps = UNPACK_FMT.get(key, ('?', 0, 0))
            elem = bits * comps // 8 if bits != 5 else 2
            signed = not (imm & 0x4000)
            count = num or 256  # VIF NUM=0 means 256 elements
            nbytes = (elem * count + 3) & ~3
            vals = []
            for k in range(min(count, 6)):
                o = p + k * elem
                if bits == 32:
                    vals.append(tuple(round(x, 3) for x in struct.unpack_from('<%df' % comps, data, o)))
                elif bits == 16:
                    vals.append(struct.unpack_from('<%d%s' % (comps, 'h' if signed else 'H'), data, o))
                elif bits == 8:
                    vals.append(struct.unpack_from('<%d%s' % (comps, 'b' if signed else 'B'), data, o))
            usn = 's' if signed else 'u'
            print(f'  UNPACK {name}-{bits}{usn} num {num} addr {imm & 0x3ff:#x} {"+TOPS" if imm & 0x8000 else ""}: {vals}')
            p += nbytes
        else:
            names = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD',
                     0x06: 'MSKPATH3', 0x07: 'MARK', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL',
                     0x15: 'MSCALF', 0x17: 'MSCNT', 0x20: 'STMASK', 0x30: 'STROW', 0x31: 'STCOL', 0x4a: 'MPG',
                     0x50: 'DIRECT', 0x51: 'DIRECTHL'}
            print(f'  {names.get(c, hex(c))} imm {imm:#x} num {num}')
            if c == 0x20:
                p += 4
            elif c in (0x30, 0x31):
                p += 16
            elif c in (0x50, 0x51):
                p += (imm or 0x10000) * 16
            elif c == 0x4a:
                p += (num or 256) * 8  # MPG transfers 64-bit microcode words


def main():
    d = open(sys.argv[1], 'rb').read()
    meshes = find_meshes(d)
    m = meshes[int(sys.argv[2]) if len(sys.argv) > 2 else 0]
    b01, s01 = m[0x134a01]
    b02, s02 = m[0x134a02]
    while d[b02] == 0x11:
        b02 += 1
    for i in range(int(sys.argv[3]) if len(sys.argv) > 3 else 2):
        fl, nv, npl, dens, dq, nz, nt, dqs, pg, tex, lm = struct.unpack_from('<HBBfHbbBBbb', d, b01 + 4 + i * 16)
        print(f'strip {i}: verts {nv} polys {npl} flags {fl:#x} tex {tex}')
        decode(d, b02 + dq * 16, dqs * 16)


if __name__ == '__main__':
    main()
