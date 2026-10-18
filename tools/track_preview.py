#!/usr/bin/env python3
"""Plot a top-down point map of a UG2 PS2 track stream (TRACKS\\STREAM<name>.BUN).

The stream is a flat list of bChunks. Two kinds matter here:
  0x80134000  solid lists (same format as car GEOMETRY.BIN; each 0x80134010 solid has a
              0x134011 header with the name hash at +0x10, and 0x80134100 VIF meshes)
  0x80034100  scenery sections:
                0x34101 header
                0x34102 SceneryInfo, 68 bytes: char name[32]; u32 hash[2]; ...
                0x34103 SceneryInstance, 64 bytes after a 12-byte 0x11 pad:
                        float bbox[6]; u16 info; u16 flags; s16 preculler; s16 light;
                        float pos[3]; s16 rot[9] (1/8192); s16 pad
Instances place solids (by info name hash) in the world; Z is up.
Usage: track_preview.py STREAM.BUN OUT.png [max_sections]"""
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from solid_preview import strip_positions  # noqa: E402


def chunks(d, off, end):
    while off + 8 <= end:
        cid, sz = struct.unpack_from('<II', d, off)
        yield cid, off + 8, sz
        off += 8 + sz


def solid_points(d, body, size):
    """Decode every mesh of one 0x80134010 solid into solid-space points."""
    pts = []
    for cid, b, s in chunks(d, body, body + size):
        if cid != 0x80134100:
            continue
        parts = {c: (b2, s2) for c, b2, s2 in chunks(d, b, b + s)}
        if 0x134a01 not in parts or 0x134a02 not in parts:
            continue
        b01, s01 = parts[0x134a01]
        b02, _ = parts[0x134a02]
        while d[b02] == 0x11:
            b02 += 1
        for i in range((s01 - 4) // 16):
            _, nv, _, _, dq, _, _, dqs, _, _, _ = struct.unpack_from('<HBBfHbbBBbb', d, b01 + 4 + i * 16)
            try:
                pts.extend(strip_positions(d, b02 + dq * 16, dqs * 16))
            except (ValueError, struct.error, IndexError):
                pass
    return pts


def main():
    d = open(sys.argv[1], 'rb').read()
    max_sections = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
    solids = {}  # name hash -> (file offset, size); decoded lazily
    sections = []
    for cid, body, size in chunks(d, 0, len(d)):
        if cid == 0x80134000:
            for c2, b2, s2 in chunks(d, body, body + size):
                if c2 != 0x80134010:
                    continue
                for c3, b3, s3 in chunks(d, b2, b2 + s2):
                    if c3 == 0x134011:
                        solids[struct.unpack_from('<I', d, b3 + 0x10)[0]] = (b2, s2)
                        break
        elif cid == 0x80034100:
            sections.append({c: (b2, s2) for c, b2, s2 in chunks(d, body, body + size)})
    print(f'{len(solids)} solids, {len(sections)} scenery sections')

    cache = {}
    points = []
    placed = missing = 0
    for sec in sections[:max_sections]:
        if 0x34102 not in sec or 0x34103 not in sec:
            continue
        bi, si = sec[0x34102]
        hashes = [struct.unpack_from('<I', d, bi + k + 0x20)[0] for k in range(0, si - 67, 68)]
        b3, s3 = sec[0x34103]
        while d[b3] == 0x11:
            b3 += 1
        for k in range((s3 - (b3 - sec[0x34103][0])) // 64):
            o = b3 + k * 64
            info = struct.unpack_from('<H', d, o + 0x18)[0]
            pos = struct.unpack_from('<3f', d, o + 0x20)
            rot = [r / 8192.0 for r in struct.unpack_from('<9h', d, o + 0x2c)]
            if not 0 <= info < len(hashes) or hashes[info] not in solids:
                missing += 1
                continue
            h = hashes[info]
            if h not in cache:
                cache[h] = solid_points(d, *solids[h])
            placed += 1
            for v in cache[h][::4]:
                # Row vectors: world = v.x*row0 + v.y*row1 + v.z*row2 + pos
                points.append(tuple(v[0] * rot[a] + v[1] * rot[3 + a] + v[2] * rot[6 + a] + pos[a]
                                    for a in range(3)))
    print(f'{placed} instances placed, {missing} unresolved, {len(points)} points')
    if not points:
        return
    xs = sorted(p[0] for p in points)
    ys = sorted(p[1] for p in points)
    zs = [p[2] for p in points]
    # Robust extents (1st..99th percentile) so stray points do not shrink the map.
    lo_x, hi_x = xs[len(xs) // 100], xs[-len(xs) // 100 - 1]
    lo_y, hi_y = ys[len(ys) // 100], ys[-len(ys) // 100 - 1]
    zlo, zhi = min(zs), max(zs)
    print(f'x {lo_x:.0f}..{hi_x:.0f}  y {lo_y:.0f}..{hi_y:.0f}  z {zlo:.0f}..{zhi:.0f}')
    W = 1600
    span = max(hi_x - lo_x, hi_y - lo_y) or 1
    s = (W - 40) / span
    img = Image.new('RGB', (W, W), (12, 12, 16))
    px = img.load()
    for x, y, z in points:
        u, v = int(20 + (x - lo_x) * s), int(W - 20 - (y - lo_y) * s)
        if 0 <= u < W and 0 <= v < W:
            t = (z - zlo) / ((zhi - zlo) or 1)
            px[u, v] = (int(60 + 195 * t), int(120 + 100 * (1 - t)), int(255 - 155 * t))
    ImageDraw.Draw(img).text((20, 8), sys.argv[1].rsplit('/', 1)[-1] + '  top view (X right, Y up)',
                             fill=(220, 220, 230))
    img.save(sys.argv[2])


if __name__ == '__main__':
    main()
