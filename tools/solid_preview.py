#!/usr/bin/env python3
"""Decode EA PS2 solid vertices and plot them for format/scale inspection.

Positions are V4-16 SoA groups of 4 at VU address 0x05. Each packet also
provides two candidate bounds vectors at addresses 0x02 and 0x03; signed
coordinates are mapped between them as Q15. Triangle topology is unknown, so
this tool deliberately plots points instead of inventing triangles.

Usage: solid_preview.py GEOMETRY.BIN OUT.png [mesh_index|all]"""
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from vif_dump import find_meshes  # noqa: E402


def strip_positions(d, start, size):
    """Collect UNPACK writes into a VU memory dict, then read SoA positions from addr 0x05."""
    vu = {}
    p, end = start, start + size
    while p + 4 <= end:
        imm, num, cmd = struct.unpack_from('<HBB', d, p)
        p += 4
        c = cmd & 0x7f
        if c >= 0x60:
            vn, vl = (c >> 2) & 3, c & 3
            comps = vn + 1
            bits = {0: 32, 1: 16, 2: 8, 3: 5}[vl]
            elem = bits * comps // 8 if bits != 5 else 2
            for k in range(num):
                o = p + k * elem
                if bits == 32:
                    v = struct.unpack_from('<%df' % comps, d, o)
                elif bits == 16:
                    v = struct.unpack_from('<%dh' % comps, d, o)
                elif bits == 8:
                    v = struct.unpack_from('<%db' % comps, d, o) if not imm & 0x4000 else struct.unpack_from('<%dB' % comps, d, o)
                else:
                    v = (0, 0, 0, 0)
                vu[(imm & 0x3ff) + k] = v
            p += (elem * num + 3) & ~3
        elif c == 0x14:
            break
    hdr = vu.get(0, (0,))
    nverts = hdr[0]
    pos = []
    addr = 5
    soa_vertices = nverts & ~3
    while len(pos) < soa_vertices:
        xs, ys, zs = vu.get(addr, ()), vu.get(addr + 1, ()), vu.get(addr + 2, ())
        for k in range(4):
            if k < len(xs) and k < len(ys) and k < len(zs):
                pos.append((xs[k], ys[k], zs[k]))
        addr += 3
    tail_addr = 5 + (soa_vertices // 4) * 3
    remainder = nverts - soa_vertices
    for k in range(remainder):
        if remainder == 1:
            xyz = tuple(vu.get(tail_addr + a, (0,))[0] for a in range(3))
        elif remainder == 2:
            xyz = tuple(vu.get(tail_addr + a, (0, 0))[k] for a in range(3))
        else:
            tail = vu.get(tail_addr + k, (0, 0, 0))
            xyz = tuple(tail[:3])
        pos.append(xyz)
    lo, hi = vu.get(2), vu.get(3)
    if len(lo or ()) < 3 or len(hi or ()) < 3:
        raise ValueError('strip is missing candidate position bounds at VU addresses 2 and 3')
    return [tuple(lo[a] + (p[a] / 32768.0) * (hi[a] - lo[a]) for a in range(3)) for p in pos]


def main():
    d = open(sys.argv[1], 'rb').read()
    meshes = find_meshes(d)
    select = sys.argv[3] if len(sys.argv) > 3 else 'all'
    if select == 'all':
        selected = enumerate(meshes)
    else:
        index = int(select)
        selected = [(index, meshes[index])]
    points = []
    declared = 0
    strips = 0
    for mesh_index, m in selected:
        b01, s01 = m[0x134a01]
        b02, s02 = m[0x134a02]
        while d[b02] == 0x11:
            b02 += 1
        n = (s01 - 4) // 16
        for i in range(n):
            fl, nv, npl, dens, dq, nz, nt, dqs, pg, tex, lm = struct.unpack_from('<HBBfHbbBBbb', d, b01 + 4 + i * 16)
            pos = strip_positions(d, b02 + dq * 16, dqs * 16)
            if len(pos) != nv:
                raise ValueError(f'mesh {mesh_index} strip {i}: decoded {len(pos)} vertices, expected {nv}')
            points.extend(pos)
            declared += nv
            strips += 1
    if not points:
        raise ValueError('no vertices decoded')
    print(f'{len(meshes)} meshes, {strips} strips, {len(points)}/{declared} vertices')
    xs = [v[0] for v in points]
    ys = [v[1] for v in points]
    zs = [v[2] for v in points]
    print('x', min(xs), max(xs), 'y', min(ys), max(ys), 'z', min(zs), max(zs))
    W, H = 1200, 600
    img = Image.new('RGB', (W, H), (20, 20, 25))
    # The decoded model has X as its long axis: side view is X/Y, top view is X/Z.
    span = max(max(zs) - min(zs), max(xs) - min(xs), max(ys) - min(ys)) or 1
    s = (W / 2 - 40) / span

    xmin, ymin = min(xs), min(ys)
    zmid = (min(zs) + max(zs)) / 2

    def side(v):
        return (20 + (v[0] - xmin) * s, H / 2 + 150 - (v[1] - ymin) * s)

    def top(v):
        return (W / 2 + 20 + (v[0] - xmin) * s, H / 2 - (v[2] - zmid) * s)
    px = img.load()
    dr = ImageDraw.Draw(img)
    dr.text((20, 12), 'SIDE  X / Y', fill=(200, 200, 210))
    dr.text((W / 2 + 20, 12), 'TOP  X / Z', fill=(200, 200, 210))
    for v in points:
        for (x, y), col in ((side(v), (90, 200, 255)), (top(v), (255, 200, 90))):
            if 0 <= int(x) < W and 0 <= int(y) < H:
                px[int(x), int(y)] = col
    img.save(sys.argv[2])


if __name__ == '__main__':
    main()
