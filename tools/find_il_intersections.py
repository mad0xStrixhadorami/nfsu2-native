#!/usr/bin/env python3
"""Find the places in the road mesh where an imported intersection asset (ISSUES #336) fits.

usage: find_il_intersections.py <surf.bin> <out.txt>

surf.bin comes from `NFSU2_DUMP_ROADS=surf.bin NFSU2_WORLD=L4RA ./build/nfsu2` (8 floats per road-surface triangle:
three xy pairs, the centroid height and a class, 1 = asphalt carriageway, 2 = sidewalk).

A crossing is a template match on the asphalt raster: four straight arms (asphalt, 52 m long, the arm's full width) and four
corner blocks that are NOT asphalt.  A T-junction is the same with one arm and its two corners missing.  The candidates are
refined (orientation to 1 degree, centre to 1.5 m), measured (the real half-width of the carriageway along every arm) and kept only when
the surface is a single layer (no deck above or below) and flat enough.

Output, one line per site:  kind x y yaw_deg halfwidth z      kind = X (four arms) or T (stem towards +yaw+90... see below)
  yaw_deg: direction of the first arm; for T the direction of the STEM (the arm without a partner).
"""
import json, math, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy.signal import fftconvolve

R = 1.5  # raster resolution, metres
ARM = 52.0  # straight arm length that must be asphalt
K = int(62 / R)


def raster(a):
    xs, ys, z, cls = a[:, [0, 2, 4]], a[:, [1, 3, 5]], a[:, 6], a[:, 7]
    x0, y0 = xs.min() - 10, ys.min() - 10
    W, H = int((xs.max() - x0) / R) + 20, int((ys.max() - y0) / R) + 20
    asph = Image.new('L', (W, H), 0)
    side = Image.new('L', (W, H), 0)
    zlo = Image.new('F', (W, H), 1e5)
    zhi = Image.new('F', (W, H), -1e5)
    da, ds, dl, dh = (ImageDraw.Draw(i) for i in (asph, side, zlo, zhi))
    order = np.argsort(-z)
    for i in order:  # high first so the lowest ends up on top
        p = [((xs[i, k] - x0) / R, (ys[i, k] - y0) / R) for k in range(3)]
        if cls[i] == 1:
            da.polygon(p, fill=255)
            dl.polygon(p, fill=float(z[i]))
        elif cls[i] == 2:
            ds.polygon(p, fill=255)
    for i in order[::-1]:
        if cls[i] == 1:
            dh.polygon([((xs[i, k] - x0) / R, (ys[i, k] - y0) / R) for k in range(3)], fill=float(z[i]))
    return ((np.asarray(asph) > 0).astype(np.float32), (np.asarray(side) > 0).astype(np.float32),
            np.asarray(zlo), np.asarray(zhi), x0, y0)


def kernels(w, th, kind):
    yy, xx = np.mgrid[-K:K + 1, -K:K + 1] * R
    t = math.radians(th)
    u = xx * math.cos(t) + yy * math.sin(t)
    v = -xx * math.sin(t) + yy * math.cos(t)
    if kind == 'X':
        arm = ((abs(u) < w - 0.5) & (abs(v) < ARM)) | ((abs(v) < w - 0.5) & (abs(u) < ARM))
        non = (abs(u) > w + 2) & (abs(u) < w + 20) & (abs(v) > w + 2) & (abs(v) < w + 20)
    else:  # T: the stem runs along +v, the bar along u; nothing beyond the bar on the -v side
        arm = ((abs(u) < ARM) & (abs(v) < w - 0.5)) | ((v > 0) & (abs(u) < w - 0.5) & (v < ARM))
        non = ((abs(u) > w + 2) & (abs(u) < w + 20) & (v > w + 2) & (v < w + 20)) | \
              ((abs(u) < w + 20) & (v < -(w + 2)) & (v > -(w + 20)))
    return arm.astype(np.float32), non.astype(np.float32)


def score(M, w, th, kind):
    arm, non = kernels(w, th, kind)
    return (fftconvolve(M, arm[::-1, ::-1], mode='same') + fftconvolve(1 - M, non[::-1, ::-1], mode='same')) / (arm.sum() + non.sum())


def arm_width(M, x0, y0, X, Y, ang_deg):
    """Median half width of the asphalt run across one arm at 26..44 m from the centre."""
    ang = math.radians(ang_deg)
    dx, dy = math.cos(ang), math.sin(ang)
    out = []
    for d in (26, 32, 38, 44):
        c = np.array([X + dx * d, Y + dy * d])
        run = 0.0
        for sgn in (-1, 1):
            k = 0.0
            while k < 20:
                p = c + sgn * k * np.array([-dy, dx])
                i, j = int((p[0] - x0) / R), int((p[1] - y0) / R)
                if not (0 <= j < M.shape[0] and 0 <= i < M.shape[1]) or M[j, i] < 0.5:
                    break
                k += 0.5
            run += k
        out.append(run / 2.0)
    return float(np.median(out)), float(max(out) - min(out))


def half_widths(M, x0, y0, X, Y, th, kind):
    """(hwU, hwV, worst): the half width of the arms that run along u (direction th) and along v (th + 90)."""
    u = [arm_width(M, x0, y0, X, Y, th + a) for a in (0, 180)]
    v = [arm_width(M, x0, y0, X, Y, th + 90)] + ([arm_width(M, x0, y0, X, Y, th + 270)] if kind == 'X' else [])
    return (float(np.mean([w for w, _ in u])), float(np.mean([w for w, _ in v])), max(sp for _, sp in u + v))


def main():
    a = np.fromfile(sys.argv[1], dtype=np.float32).reshape(-1, 8)
    M, S, Zlo, Zhi, x0, y0 = raster(a)
    print('raster', M.shape, 'asphalt px', int(M.sum()))
    found = []
    for kind, thetas in (('X', range(0, 90, 6)), ('T', range(0, 360, 6))):
        cands = []
        for w in (7, 9, 11, 13):
            for th in thetas:
                sc = score(M, w, th, kind)
                for j, i in np.argwhere(sc > 0.93)[::5]:
                    cands.append((float(sc[j, i]), w, th, i, j))
        cands.sort(reverse=True)
        for sc, w, th, i, j in cands:
            X, Y = x0 + i * R, y0 + j * R
            if any(math.hypot(X - o['x'], Y - o['y']) < 60 for o in found):
                continue
            # refine yaw / centre / width on a local window
            best = (sc, w, th, i, j)
            for dth in range(-5, 6, 1):
                arm, non = kernels(best[1], th + dth, kind)
                sub = M[j - K - 3:j + K + 4, i - K - 3:i + K + 4]
                if sub.shape[0] < 2 * K + 7 or sub.shape[1] < 2 * K + 7:
                    continue
                c1 = fftconvolve(sub, arm[::-1, ::-1], mode='valid') + fftconvolve(1 - sub, non[::-1, ::-1], mode='valid')
                c1 /= (arm.sum() + non.sum())
                jj, ii = np.unravel_index(np.argmax(c1), c1.shape)
                if c1[jj, ii] > best[0]:
                    best = (float(c1[jj, ii]), best[1], th + dth, i - 3 + ii, j - 3 + jj)
            sc, w, th, i, j = best
            X, Y = x0 + i * R, y0 + j * R
            hwU, hwV, spread = half_widths(M, x0, y0, X, Y, th, kind)
            win = (slice(max(0, j - K), j + K + 1), slice(max(0, i - K), i + K + 1))
            m = M[win] > 0
            if not m.any():
                continue
            layer = float(np.percentile((Zhi[win] - Zlo[win])[m], 98))
            zspread = float(np.percentile(Zhi[win][m], 98) - np.percentile(Zlo[win][m], 2))
            zc = float(np.median(Zlo[win][m]))
            ok = layer < 1.0 and zspread < 8.0 and spread < 3.5 and 8.5 <= hwU <= 15.0 and 8.5 <= hwV <= 15.0
            found.append(dict(kind=kind, x=X, y=Y, yaw=th % 360, hwU=hwU, hwV=hwV, z=zc, score=sc, layer=layer, zspread=zspread, spread=spread, ok=ok))
    for k in found:
        print('  cand %s %8.1f %8.1f yaw %5.1f hwU %5.2f hwV %5.2f spread %.1f z %5.1f score %.3f layer %.2f dz %.1f %s' % (k['kind'], k['x'], k['y'], k['yaw'], k['hwU'], k['hwV'], k['spread'], k['z'], k['score'], k['layer'], k['zspread'], 'OK' if k['ok'] else ''))
    keep = [f for f in found if f['ok']]
    print(len(found), 'candidates,', len(keep), 'usable')
    with open(sys.argv[2], 'w') as f:
        f.write('# kind x y yaw_deg halfwidthU halfwidthV z   (u = direction yaw, v = yaw + 90; the stem of a T runs along +v)  (tools/find_il_intersections.py)\n')
        for k in sorted(keep, key=lambda q: -q['score']):
            f.write('%s %.1f %.1f %.1f %.2f %.2f %.2f\n' % (k['kind'], k['x'], k['y'], k['yaw'], k['hwU'], k['hwV'], k['z']))
            print('%s %8.1f %8.1f yaw %5.1f hwU %5.2f hwV %5.2f z %5.1f score %.3f layer %.2f dz %.1f' % (
                k['kind'], k['x'], k['y'], k['yaw'], k['hwU'], k['hwV'], k['z'], k['score'], k['layer'], k['zspread']))


if __name__ == '__main__':
    main()
