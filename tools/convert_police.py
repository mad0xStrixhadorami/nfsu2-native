#!/usr/bin/env python3
"""Converts the GTA V police fragment (police2.yft + police2.ytd) to the game's cache format 'PCR1' (ISSUES #318).

The assets are the user's own downloaded files (see Credits.txt in their folder: no redistribution), so the output goes to a cache
directory outside the repository and is read by the game at run time.

usage: convert_police.py <folder with police2.yft/.ytd> <out.pcar> [livery, default car_sign_2 = the black traffic print] [cell m, default 0.035] [uv cell]
       [lo]
the game wants <NFSU2_DATA>/police/police.pcar (cell 0.14, uv 0.1: ~25k triangles) and police_lo.pcar (0.5, 0.3, "lo": ~10k, the far model)
"""
import io
import math
import zlib
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
from yft import *  # noqa

CELL = float(sys.argv[4]) if len(sys.argv) > 4 else 0.035
UVCELL = float(sys.argv[5]) if len(sys.argv) > 5 else 0.02
NOCULL = True  # the hidden-surface cull removed 1% only
LOW = len(sys.argv) > 6 and sys.argv[6] == 'lo'          # the far LOD: no textures in the file, rough wheels
WHEELSEGS = 10 if LOW else 24
SKIP_TEX = {'FZ-G1', 'computer_Screen'}                         # the laptop on the dash


def to_ug(p):  # GTA x right, y forward -> UG2 x forward, y left
    return (p[1], -p[0], p[2])


def decode_texture(w, h, fcc, levels, data, maxdim):
    mip0 = data
    if fcc in (b'DXT1', b'DXT3', b'DXT5'):
        bs = 8 if fcc == b'DXT1' else 16
        n = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bs
        im = Image.open(io.BytesIO(dds_bytes(w, h, fcc, data[:n], 1))).convert('RGBA')
    else:  # D3DFMT_A8R8G8B8 (21): B G R A bytes
        a = np.frombuffer(data[:w * h * 4], np.uint8).reshape(h, w, 4)
        im = Image.fromarray(a[:, :, [2, 1, 0, 3]].copy(), 'RGBA')
    if max(w, h) > maxdim:
        im = im.resize((max(1, w * maxdim // max(w, h)), max(1, h * maxdim // max(w, h))), Image.LANCZOS)
    return im


def cluster(g, cell, uvcell):
    """Vertex clustering of one geometry: merges vertices that share a cell of position and of UV (so UV seams survive)."""
    pos = np.array(g.pos, np.float32)
    nrm = np.array(g.nrm, np.float32) if g.nrm else np.zeros_like(pos)
    uv = np.array(g.uv, np.float32) if g.uv else np.zeros((len(pos), 2), np.float32)
    uv = np.nan_to_num(uv)
    key = np.concatenate([np.floor(pos / cell), np.floor(uv / uvcell)], axis=1).astype(np.int64)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    inv = inv.reshape(-1)
    n = len(cnt)
    P = np.zeros((n, 3), np.float64); N = np.zeros((n, 3), np.float64); U = np.zeros((n, 2), np.float64)
    np.add.at(P, inv, pos); np.add.at(N, inv, nrm); np.add.at(U, inv, uv)
    P /= cnt[:, None]; U /= cnt[:, None]
    nl = np.linalg.norm(N, axis=1, keepdims=True)
    N = np.where(nl > 1e-6, N / np.maximum(nl, 1e-9), np.array([[0, 0, 1.0]]))
    idx = np.array(g.indices, np.int64).reshape(-1, 3)
    t = inv[idx]
    ok = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    t = t[ok]
    # drop duplicate triangles
    s = np.sort(t, axis=1)
    _, first = np.unique(s, axis=0, return_index=True)
    t = t[np.sort(first)]
    return P, N, U, t


def wheel_mesh(cx, cy, cz, radius, width, side, hubidx, tyretex, segs=24):
    """A tyre (rounded profile) plus a five-spoke rim at the outboard face, around the hub (cx, cy, cz); axle along y."""
    tris = []  # (verts[3] of (p, n, uv), tex, rim)
    sgn = 1.0 if side > 0 else -1.0  # +y = left side
    prof = []  # (radial offset from the rim edge, lateral, normal radial, normal lateral)
    r_rim = radius * 0.64
    w2 = width * 0.5
    pts = [(r_rim, -w2 * 0.98, -1, 0), (r_rim + (radius - r_rim) * 0.35, -w2, -0.8, -0.6), (radius * 0.97, -w2 * 0.85, -0.5, -0.85), (radius, -w2 * 0.5, 0, -1),
           (radius, w2 * 0.5, 0, 1), (radius * 0.97, w2 * 0.85, 0.5, 0.85), (r_rim + (radius - r_rim) * 0.35, w2, 0.8, 0.6), (r_rim, w2 * 0.98, 1, 0)]
    # (lateral sign: +lateral = outboard)
    for i in range(segs):
        a0 = 2 * math.pi * i / segs
        a1 = 2 * math.pi * (i + 1) / segs
        for k in range(len(pts) - 1):
            def v(a, pt, k2):
                r, lat, nr, nl = pt
                # wheel in the car's x (forward) / z (up) plane, the axle along y
                px = cx + r * math.cos(a)
                pz = cz + r * math.sin(a)
                py = cy + sgn * lat
                nx = nr * math.cos(a)
                nz = nr * math.sin(a)
                ny = sgn * nl
                # tread U runs round the tyre, V across it
                u = a / (2 * math.pi) * 4.0
                vv = 0.5 + (lat / width) * 0.9
                return ((px, py, pz), (nx, ny, nz), (u, vv))
            a = v(a0, pts[k], k); b = v(a1, pts[k], k); c = v(a1, pts[k + 1], k); d = v(a0, pts[k + 1], k)
            if sgn > 0:
                tris.append(((a, b, c), tyretex, 0)); tris.append(((a, c, d), tyretex, 0))
            else:
                tris.append(((a, c, b), tyretex, 0)); tris.append(((a, d, c), tyretex, 0))
    # sidewall disc (inboard and outboard) and the rim: a dished face with five double spokes (dark gaps between them)
    for side_out in (1, -1):
        lat = side_out * w2 * 0.98
        n_l = side_out * sgn
        for i in range(segs):
            a0 = 2 * math.pi * i / segs
            a1 = 2 * math.pi * (i + 1) / segs
            spoke = ((i * 5) // segs + (i * 5 % segs) / segs) if False else None
            def vert(a, r):
                return ((cx + r * math.cos(a), cy + sgn * lat, cz + r * math.sin(a)), (0, n_l, 0), (0.5 + 0.5 * math.cos(a) * r / radius, 0.5 + 0.5 * math.sin(a) * r / radius))
            o = side_out == 1
            # the outboard face is the rim (spokes via alternating segments), the inboard one stays dark
            ring_in = r_rim * 0.30
            for (ra, rb) in ((ring_in, r_rim),):
                p00, p01, p10, p11 = vert(a0, ra), vert(a1, ra), vert(a0, rb), vert(a1, rb)
                rim_here = o and ((i * 5 // segs) * 2 != (i * 10 // segs) - 0) or o
                # five spokes: segments whose index falls in the lower half of each fifth are a gap (not drawn)
                frac = (i * 5 / segs) % 1.0
                if o and frac > 0.55:
                    continue
                tex = -2 if o else tyretex  # -2: the rim colour (flagged rim)
                if (sgn * side_out) > 0:
                    tris.append(((p00, p01, p11), tex, 1 if o else 0)); tris.append(((p00, p11, p10), tex, 1 if o else 0))
                else:
                    tris.append(((p00, p11, p01), tex, 1 if o else 0)); tris.append(((p00, p10, p11), tex, 1 if o else 0))
            # the dark disc behind the spokes
            if o:
                p00, p01 = vert(a0, 0.0), vert(a1, 0.0)
                p10, p11 = vert(a0, r_rim), vert(a1, r_rim)
                lat_back = lat - sgn * 0.0
                if (sgn * side_out) > 0:
                    tris.append(((p00, p01, p11), tyretex, 0)); tris.append(((p00, p11, p10), tyretex, 0))
                else:
                    tris.append(((p00, p11, p01), tyretex, 0)); tris.append(((p00, p10, p11), tyretex, 0))
    return tris


def main():
    folder, out = sys.argv[1], sys.argv[2]
    livery = sys.argv[3] if len(sys.argv) > 3 else 'car_sign_2'
    raw = inflate(folder + '/police2.yft')
    r = Res(raw)
    dr = r.ptr(0x30)
    shaders = read_shaders(r, r.ptr(dr + 0x10))
    D = joaat('diffusesampler'); D2 = 0x0ad3a268
    models = read_models(r, dr + 0x50)[0]
    ytd = read_ytd(folder + '/police2.ytd')
    # skeleton: wheel hubs
    bo = r.ptr(0xdb60); nb = r.u16(0xdb58)
    bones = {}
    for i in range(nb):
        b = bo + i * 0x50
        try:
            bones[r.cstr(r.ptr(b + 0x38))] = struct.unpack_from('<3f', raw, b + 0x10)
        except Exception:
            pass
    hubs = {k: bones[k] for k in ('wheel_lf', 'wheel_rf', 'wheel_lr', 'wheel_rr')}
    print('hubs (GTA):', hubs)

    texlist = []   # (name, hash, w, h, rgba bytes, gloss)
    texindex = {}

    def want_tex(name, gloss, maxdim=512, solid=None):
        if name in texindex:
            return texindex[name]
        h = 0xC0DE0000 | (zlib.crc32(name.encode()) & 0xFFFF)
        assert all(t[1] != h for t in texlist), 'hash clash'
        if solid is not None:
            im = Image.new('RGBA', (4, 4), solid)
        else:
            w, hh, fcc, lv, data = ytd[name]
            im = decode_texture(w, hh, fcc, lv, data, 2048 if name == livery else maxdim)
        texlist.append((name, h, im.size[0], im.size[1], im.tobytes(), gloss))
        texindex[name] = len(texlist) - 1
        return texindex[name]

    # material of each shader: (texture index, gloss class)
    dummy = {'vehicle_generic_smallspecmap', 'dinghyblack_spec'}
    glass_names = {'vehicle_generic_glasswindows2', 'wind_temp'}
    tris = []  # (verts, texidx, wheel, rim)
    shader_name = {joaat(n): n for n in ['vehicle_paint3', 'vehicle_mesh', 'vehicle_lightsemissive', 'vehicle_vehglass', 'vehicle_badges', 'vehicle_tire']}
    for g in models:
        sh = shaders[g.shader]
        sname = shader_name.get(sh[0], '')
        diffuse = sh[1].get(D)
        if sname == 'vehicle_paint3':
            tname, gloss = livery, 1
        elif diffuse in SKIP_TEX:
            continue
        elif sname == 'vehicle_vehglass' and diffuse in glass_names:
            tname, gloss = '__glass', 2
        elif diffuse in ('red', 'red2'):
            tname, gloss = diffuse, 7
        elif diffuse is None or diffuse in dummy:
            tname, gloss = '__black', 0
        elif diffuse in ('Vison', 'Vison_ems', 'whelen_ion', 'fascia_emis', 'limo_lights_glass', 'jr_rglass'):
            tname, gloss = diffuse, 0
        else:
            tname, gloss = diffuse, 0
        if tname == '__glass':
            ti = want_tex('__glass', 2, solid=(10, 12, 16, 255))
        elif tname == '__black':
            ti = want_tex('__black', 0, solid=(18, 18, 20, 255))
        else:
            ti = want_tex(tname, gloss)
        # decimate: finer on the lightbar / lamps, coarser on the plain trim
        cell = CELL * (1.0 if tname in ('Vison', 'Vison_ems', 'whelen_ion', 'fascia_emis', 'red', 'red2') else 1.0)
        P, N, U, T = cluster(g, cell, UVCELL)
        for t in T:
            vs = []
            for k in t:
                vs.append((to_ug(P[k]), to_ug(N[k]), (float(U[k][0]), float(U[k][1]))))
            tris.append((tuple(vs), ti, 255, 0))
    shell = len(tris)
    print('shell triangles:', shell)
    # hidden-surface cull: the interior, the engine bay and the underbody are never seen (the windows are dark): keep a triangle only if it is
    # the nearest surface from some direction (an orthographic depth map from 42 directions, 3 cm cells, 8 cm slack)
    if not NOCULL:
        P = np.array([[v[0] for v in t[0]] for t in tris[:shell]], np.float32)  # (n, 3 verts, 3)
        bary = [(1/3, 1/3, 1/3), (1, 0, 0), (0, 1, 0), (0, 0, 1), (.5, .5, 0), (0, .5, .5), (.5, 0, .5), (.7, .15, .15), (.15, .7, .15), (.15, .15, .7)]
        pts = np.concatenate([P[:, 0] * w[0] + P[:, 1] * w[1] + P[:, 2] * w[2] for w in bary], axis=0)
        ids = np.concatenate([np.arange(shell)] * len(bary))
        dirs = []
        for ix in (-1, 0, 1):
            for iy in (-1, 0, 1):
                for iz in (-1, 0, 1):
                    if (ix, iy, iz) != (0, 0, 0):
                        dirs.append(np.array([ix, iy, iz], np.float32))
        extra = [np.array(d, np.float32) for d in ((2, 1, 1), (2, -1, 1), (-2, 1, 1), (-2, -1, 1), (1, 2, 1), (1, -2, 1), (-1, 2, 1), (-1, -2, 1),
                                                    (2, 1, 2), (2, -1, 2), (-2, 1, 2), (-2, -1, 2), (1, 2, 2), (1, -2, 2), (-1, 2, 2), (-1, -2, 2))]
        dirs += extra
        dirs = [d for d in dirs if d[2] >= -0.01]  # nobody looks at the underbody
        visible = np.zeros(shell, bool)
        cellsz = 0.035
        for d in dirs:
            d = d / np.linalg.norm(d)
            up = np.array([0, 0, 1], np.float32) if abs(d[2]) < 0.9 else np.array([1, 0, 0], np.float32)
            r = np.cross(up, d); r /= np.linalg.norm(r)
            u = np.cross(d, r)
            a = pts @ r; b = pts @ u; depth = pts @ d  # larger depth = nearer to a viewer far along +d
            ia = np.floor((a - a.min()) / cellsz).astype(np.int64); ib = np.floor((b - b.min()) / cellsz).astype(np.int64)
            key = ia * 100003 + ib
            order = np.argsort(key, kind='stable')
            ks = key[order]
            first = np.concatenate([[True], ks[1:] != ks[:-1]])
            grp = np.cumsum(first) - 1
            mx = np.maximum.reduceat(depth[order], np.nonzero(first)[0])
            near = depth[order] >= mx[grp] - 0.08
            visible[ids[order][near]] = True
        keep = [i for i in range(shell) if visible[i]]
        print('visible shell triangles:', len(keep), 'of', shell)
        tris = [tris[i] for i in keep] + tris[shell:]
        shell = len(keep)

    # wheels
    ug_hubs = {}
    for k, nm in ((0, 'wheel_lf'), (1, 'wheel_rf'), (2, 'wheel_rr'), (3, 'wheel_lr')):  # UG2 order FL FR RR RL
        ug_hubs[k] = to_ug(hubs[nm])
    radius, width = 0.335, 0.235
    tyre = want_tex('vehicle_generic_tyrewallblack', 4)
    for k in range(4):
        cx, cy, cz = ug_hubs[k]
        side = 1 if cy > 0 else -1
        # inset the wheel a little so the tyre sits inside the arch
        for verts, tex, rim in wheel_mesh(cx, cy - side * 0.0, cz, radius, width, side, k, tyre, WHEELSEGS):
            tris.append((tuple(verts), tyre if tex < 0 else tex, k, 1 if rim else 0))
            if tex == -2:
                pass
    print('total triangles:', len(tris))

    # lights for the siren flashers: the lightbar's extent
    lb = [(to_ug(p)) for g in models if shaders[g.shader][1].get(D) in ('Vison', 'Vison_ems') for p in g.pos]
    lb = np.array(lb)
    lo, hi = lb.min(0), lb.max(0)
    print('lightbar', lo, hi)
    with open(out, 'wb') as f:
        f.write(b'PCR1')
        f.write(struct.pack('<I', len(texlist)))
        for name, h, w, hh, rgba, gloss in texlist:
            nb_ = name.encode()
            if LOW:
                w = hh = 0
            f.write(struct.pack('<IHHBB', h, w, hh, gloss, len(nb_)))
            f.write(nb_)
            if not LOW:
                f.write(rgba)
        f.write(struct.pack('<I', len(tris)))
        for verts, ti, wheel, rim in tris:
            for p, n, uv in verts:
                f.write(struct.pack('<8f', p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1]))
            f.write(struct.pack('<IBBBB', texlist[ti][1], wheel, rim, 0, 0))
        for k in range(4):
            f.write(struct.pack('<3f', *ug_hubs[k]))
        f.write(struct.pack('<2f', radius, width))
        f.write(struct.pack('<6f', *lo, *hi))
    print('wrote', out)


main()
