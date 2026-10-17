#!/usr/bin/env python3
"""Decode an EA PS2 solid mesh (chunk 0x134a02) into triangle topology and render it.

Topology (functional approximation, see ISSUES.md #65): each strip entry's VIF block
is one triangle strip of NumVerts vertices, and the recorded NumPolys is always *less*
than the (NumVerts-2) triangles a plain strip would produce. The difference is always
even: (NumVerts - NumPolys) / 2 is the number of strip runs R, so 2 "bridge" triangles
are skipped at each of the R-1 internal run seams. The seam positions are not stored in
the file (strip Flags take only three constant values and no field tracks R), so they are
approximated by dropping the (NumVerts-2)-NumPolys triangles with the largest longest
edge. 5812 of 7298 multi-run strips split cleanly this way.

Usage: solid_mesh.py GEOMETRY.BIN OUT.png [mesh_index|all]
"""
import math
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from solid_preview import strip_positions  # noqa: E402
from vif_dump import find_meshes  # noqa: E402


def strip_triangles(positions, numvert, numpoly):
    """Turn one decoded strip's vertex positions into a triangle list.

    Vertices are consumed as a plain triangle strip (winding alternates), then the
    (numvert-2)-numpoly largest-longest-edge triangles are dropped as run-seam bridges."""
    if numvert < 3 or len(positions) != numvert:
        return [], 0, 0
    tris = []
    for k in range(numvert - 2):
        a, b, c = (positions[k], positions[k + 1], positions[k + 2]) if k % 2 == 0 \
            else (positions[k + 1], positions[k], positions[k + 2])
        longest = max(math.dist(a, b), math.dist(b, c), math.dist(a, c))
        tris.append((longest, a, b, c))
    drop = (numvert - 2) - numpoly
    tris.sort(key=lambda t: t[0])
    kept = tris[:len(tris) - drop] if drop > 0 else tris
    dropped = tris[len(tris) - drop:] if drop > 0 else []
    clean = 1 if (not dropped or (kept and dropped[0][0] > kept[-1][0])) else 0
    return [t[1:] for t in kept], len(dropped), clean


def solid_triangles(d):
    """Triangulate every strip of every mesh in a solid file. Returns (tris, stats)."""
    out = []
    strips = polys = dropped = clean = messy = 0
    for m in find_meshes(d):
        b01, s01 = m[0x134a01]
        b02, _ = m[0x134a02]
        while d[b02] == 0x11:
            b02 += 1
        for i in range((s01 - 4) // 16):
            fl, nv, npl, dens, dq, nz, nt, dqs, pg, tex, lm = \
                struct.unpack_from('<HBBfHbbBBbb', d, b01 + 4 + i * 16)
            positions = strip_positions(d, b02 + dq * 16, dqs * 16)
            tris, ndrop, isclean = strip_triangles(positions, nv, npl)
            out.extend(tris)
            strips += 1
            polys += npl
            dropped += ndrop
            if ndrop:
                clean += isclean
                messy += 0 if isclean else 1
    return out, (strips, polys, dropped, clean, messy)


def main():
    d = open(sys.argv[1], 'rb').read()
    tris, (strips, polys, dropped, clean, messy) = solid_triangles(d)
    print(f'{strips} strips, {len(tris)} triangles ({polys} declared polys, '
          f'{dropped} bridge triangles dropped; {clean} clean / {messy} messy seams)')
    xs = [v[0] for t in tris for v in t]
    ys = [v[1] for t in tris for v in t]
    xmin, xmax, ymin, ymax = min(xs), max(xs), min(ys), max(ys)
    W, H = 1200, 700
    scale = (W - 80) / max(xmax - xmin, ymax - ymin, 1e-6)
    img = Image.new('RGB', (W, H), (15, 15, 20))
    dr = ImageDraw.Draw(img)
    for t in sorted(tris, key=lambda t: sum(v[2] for v in t)):
        dr.polygon([(40 + (v[0] - xmin) * scale, H - 60 - (v[1] - ymin) * scale) for v in t],
                   fill=(70, 110, 160))
    img.save(sys.argv[2])
    print(f'saved {sys.argv[2]}')


if __name__ == '__main__':
    main()
