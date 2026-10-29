#!/usr/bin/env python3
"""Decode CARS\\<car>\\PARTS_ANIMATIONS.BIN (ISSUES #358): the hood / door / boot animations the shops play.

Container: 21 chunks id 0x00e34010, each = {u32 id, u32 size, 8 filler bytes 0x11, an embedded relocatable MIPS ELF whose only section
is .data (an EAGL "AnimationBank")} followed by one chunk 0x00037100 (the instance mapping).  Bank .data layout (little endian):
  +0x00 12 bytes zero, 'SK' marker ...  +0x10 the bank name, e.g. SKYLINE_ZAM_HOOD_FRONT_t
  header u16/u8 fields:  +0x40 {1, 1, nkeys, 'cccc'..}  ->  nkeys keys of 16 bytes start at the data's key table (+0x80 in a one-channel bank)
  key = {f32 qx, f32 0, f32 qw_or_qz, f32 0}: a rotation about one axis as the pair (sin(a/2), cos(a/2)); keys are evenly spaced in time
Usage: car_part_anims.py <PARTS_ANIMATIONS.BIN>   (extract one with: build/nfsu2 --dump 'CARS\\SKYLINE\\PARTS_ANIMATIONS.BIN' out)
"""
import math, struct, sys

def banks(d):
    o = 0
    while o + 8 <= len(d):
        cid, sz = struct.unpack_from('<II', d, o)
        p = d[o + 8:o + 8 + sz]
        if cid == 0xe34010:
            i = p.find(b'\x7fELF'); e = p[i:]
            shoff, = struct.unpack_from('<I', e, 32); shes, shn, shstr = struct.unpack_from('<HHH', e, 46)
            shs = [struct.unpack_from('<10I', e, shoff + k * shes) for k in range(shn)]
            so = shs[shstr][4]
            sec = {e[so + s[0]:e.index(b'\0', so + s[0])].decode(): s for s in shs}
            s = sec['.data']
            yield e[s[4]:s[4] + s[5]]
        o += 8 + sz

def decode(dat):
    st = 0x0c
    while dat[st] == 0:
        st += 1
    name = dat[st:dat.index(b'\0', st)].decode()
    nk = struct.unpack_from('<H', dat, 0x42)[0] if False else None
    # the key count sits in the channel header right before the keys: find the first run of unit-length (sin, 0, cos, 0) records
    keys = []
    for off in range(0x40, len(dat) - 16, 4):
        run = []
        k = off
        while k + 16 <= len(dat):
            x, y, w, z = struct.unpack_from('<4f', dat, k)
            if abs(x * x + y * y + w * w + z * z - 1) < 2e-3 and not (x == 0 and y == 0 and w == 0):
                run.append((x, y, w, z)); k += 16
            else:
                break
        if len(run) >= 8:
            keys = run; break
    return name, keys

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    for dat in banks(d):
        name, keys = decode(dat)
        if keys:
            ang = lambda q: 2 * math.degrees(math.acos(max(-1, min(1, max(abs(q[0]), abs(q[1]), abs(q[2]), abs(q[3])) if False else 1.0))))
            def angle(q):
                v = math.sqrt(sum(c * c for c in q) - max(q, key=abs) ** 2) if False else None
                return None
            # which component is the scalar part: the one that is 1.0 in the first key
            wi = max(range(4), key=lambda i: abs(keys[0][i]))
            ax = [i for i in range(4) if i != wi]
            ends = 2 * math.degrees(math.atan2(math.sqrt(sum(keys[-1][i] ** 2 for i in ax)), abs(keys[-1][wi])))
            axis = max(ax, key=lambda i: max(abs(k[i]) for k in keys))
            print('%-34s %3d keys  end %6.1f deg  scalar@%d axis@%d' % (name, len(keys), ends, wi, axis))
        else:
            print('%-34s (no rotation keys: %d bytes)' % (name, len(dat)))
