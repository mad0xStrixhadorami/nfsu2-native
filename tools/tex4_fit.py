#!/usr/bin/env python3
"""Find the PS2 4-bit texture layout by scoring candidate GS upload layouts against the font
glyph rectangles (ink inside glyph boxes, transparent outside). Usage: tex4_fit.py GLOBALA.BUN"""
import itertools, os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import gs_tables as T
from tex4_probe import find_chunk  # noqa: E402

def addr_map(fmt, w, h, pw):
    """GS byte (or nibble for T4) address of each pixel (x, y) for a format and pages-per-row."""
    ys, xs = np.mgrid[0:h, 0:w]
    BT32, CT32 = np.array(T.BLOCKTABLE32), np.array(T.COLUMNTABLE32)
    BT8, CT8 = np.array(T.BLOCKTABLE8), np.array(T.COLUMNTABLE8)
    BT4, CT4 = np.array(T.BLOCKTABLE4), np.array(T.COLUMNTABLE4)
    if fmt == 'CT32':
        return (((ys//32)*pw + xs//64)*2048 + BT32[(ys//8)%4, (xs//8)%8]*64 + CT32[ys%8, xs%8])*4
    if fmt == 'T8':
        return ((ys//64)*pw + xs//128)*8192 + BT8[(ys//16)%4, (xs//16)%8]*256 + CT8[ys%16, xs%16]
    if fmt == 'T4':
        return ((ys//128)*pw + xs//128)*16384 + BT4[(ys//16)%8, (xs//32)%4]*512 + CT4[ys%16, xs%32]
    raise ValueError(fmt)

def main():
    d = open(sys.argv[1], 'rb').read()
    info, isz = find_chunk(d, 0x33310004)
    blob, _ = find_chunk(d, 0x33320002)
    while d[blob] == 0x11: blob += 1
    tex = None
    for i in range(isz // 124):
        o = info + i*124
        if d[o+12:o+36].split(b'\0')[0] == b'CONDUITMDITC_TT21I':
            _,_,_,doff,poff,dsz,psz,_ = struct.unpack_from('<8I', d, o+36)
            tex = (doff, poff, dsz, psz) + struct.unpack_from('<HH', d, o+68)
    doff, poff, dsz, psz, W, H = tex
    data = np.frombuffer(d, np.uint8, dsz, blob + doff)
    pal_a = np.frombuffer(d, np.uint8, psz, blob + poff).reshape(-1, 4)[:, 3]
    # glyph mask from the font chunk
    fnt = None
    p = 0
    while p + 8 <= len(d):
        cid, sz = struct.unpack_from('<II', d, p)
        if cid == 0x00030201: fnt = p + 8 + 0x200; break
        p += 8 + sz
    num = struct.unpack_from('<H', d, fnt + 0xa)[0]; gt = struct.unpack_from('<i', d, fnt + 0x14)[0]
    inside = np.zeros((H, W), bool)
    for i in range(num):
        u, gw, gh, U, V = struct.unpack_from('<HBBHH', d, fnt + gt + i*16)
        inside[V:V+gh, U:U+gw] = True

    results = []
    def score(idx):
        ink = pal_a[idx] >= 0x20
        return ink[inside].mean() - ink[~inside].mean()
    # nibble buffer from raw bytes, low nibble first or high first
    for nib_order in ('lo', 'hi'):
        nib = np.empty(dsz*2, np.uint8)
        nib[0::2], nib[1::2] = (data & 15, data >> 4) if nib_order == 'lo' else (data >> 4, data & 15)
        results.append((score(nib[:W*H].reshape(H, W)), 'linear', nib_order))
        for fmt, ww in itertools.product(('CT32', 'T8'), (32, 64, 128, 256, 512)):
            unit = 4 if fmt == 'CT32' else 1
            n_units = dsz // unit
            hh = n_units // ww
            if hh == 0 or ww*hh*unit != dsz: continue
            for pw_w in {max(1, ww // (64 if fmt == 'CT32' else 128)), 1, 2, 4}:
                for pw_r in (1, 2, 4):
                    mem = np.zeros(1 << 21, np.uint8)
                    a = addr_map(fmt, ww, hh, pw_w).ravel()
                    if fmt == 'CT32':
                        words = data.reshape(-1, 4)
                        for k in range(4): mem[a + k] = words[:, k]
                    else:
                        mem[a] = data
                    q = addr_map('T4', W, H, pw_r)
                    b = mem[q >> 1]
                    idx = np.where(q & 1, b >> 4, b & 15) if nib_order == 'lo' else np.where(q & 1, b & 15, b >> 4)
                    results.append((score(idx), f'{fmt} w={ww} h={hh} pw_w={pw_w} pw_r={pw_r}', nib_order))
    for r in sorted(results, reverse=True)[:8]:
        print(f'{r[0]:+.3f}  {r[1]}  nibbles={r[2]}')

main()
