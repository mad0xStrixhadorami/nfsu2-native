#!/usr/bin/env python3
"""GTA V (RSC7) fragment / texture dictionary reader for the Israeli police car import (ISSUES #318).

Everything of this file's YFT lives in one 'system' segment: a pointer 0x50000000 + n is byte n of the inflated data.
Only what is needed to get the triangle meshes, their diffuse texture names and the textures out is decoded.
"""
import struct
import zlib


def joaat(s):
    h = 0
    for c in s.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h


def inflate(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'RSC7', 'not an RSC7 resource'
    return zlib.decompress(d[16:], -15)


class Res:
    def __init__(self, raw):
        self.raw = raw

    def off(self, p):
        if p == 0:
            return None
        assert (p >> 28) in (5, 6), hex(p)
        return p & 0x0FFFFFFF

    def u8(self, o): return self.raw[o]
    def u16(self, o): return struct.unpack_from('<H', self.raw, o)[0]
    def u32(self, o): return struct.unpack_from('<I', self.raw, o)[0]
    def u64(self, o): return struct.unpack_from('<Q', self.raw, o)[0]
    def f32(self, o): return struct.unpack_from('<f', self.raw, o)[0]
    def ptr(self, o): return self.off(self.u64(o))

    def cstr(self, o):
        e = self.raw.index(b'\0', o)
        return self.raw[o:e].decode('latin1')


# vertex component types of the declaration (4 bits each) -> (struct format, bytes)
COMP = {1: ('2e', 4), 2: ('f', 4), 3: ('4e', 8), 5: ('2f', 8), 6: ('3f', 12), 7: ('4f', 16), 8: ('4B', 4), 9: ('4B', 4), 10: ('I', 4)}
SEM = ['pos', 'bw', 'bi', 'nrm', 'col0', 'col1', 'uv0', 'uv1', 'uv2', 'uv3', 'uv4', 'uv5', 'uv6', 'uv7', 'tan', 'bin']


class Geometry:
    pass


def read_decl(r, o):
    flags = r.u32(o)
    stride = r.u16(o + 4)
    types = r.u64(o + 8)
    layout, pos = [], 0
    for i in range(16):
        if flags & (1 << i):
            t = (types >> (4 * i)) & 0xF
            fmt, size = COMP[t]
            layout.append((SEM[i], t, pos, size))
            pos += size
    return stride, layout


def read_geometry(r, gp):
    g = Geometry()
    vb = r.ptr(gp + 0x18)
    ib = r.ptr(gp + 0x38)
    g.index_count = r.u32(gp + 0x58)
    stride = r.u16(vb + 8)
    vdata = r.ptr(vb + 0x10)
    nv = r.u32(vb + 0x18)
    decl = r.ptr(vb + 0x30)
    dstride, layout = read_decl(r, decl)
    assert dstride == stride, (dstride, stride)
    idata = r.ptr(ib + 0x10)
    ni = r.u32(ib + 8)
    g.indices = list(struct.unpack_from('<%dH' % ni, r.raw, idata))
    g.pos, g.nrm, g.uv, g.col = [], [], [], []
    comp = {s: (t, p, sz) for s, t, p, sz in layout}
    for i in range(nv):
        base = vdata + i * stride
        g.pos.append(struct.unpack_from('<3f', r.raw, base + comp['pos'][1]))
        if 'nrm' in comp:
            g.nrm.append(struct.unpack_from('<3f', r.raw, base + comp['nrm'][1]))
        if 'uv0' in comp:
            t, p, sz = comp['uv0']
            if t == 5:
                g.uv.append(struct.unpack_from('<2f', r.raw, base + p))
            else:
                g.uv.append(struct.unpack_from('<2e', r.raw, base + p))
        if 'col0' in comp:
            g.col.append(struct.unpack_from('<4B', r.raw, base + comp['col0'][1]))
    return g


def read_models(r, listptr):
    """DrawableModel pointer list -> models; each model = (geometries, shader mapping)."""
    out = []
    lp = r.ptr(listptr)
    if lp is None:
        return out
    arr = r.ptr(lp)
    cnt = r.u16(lp + 8)
    for i in range(cnt):
        mp = r.ptr(arr + 8 * i)
        gptr = r.ptr(mp + 8)
        gcount = r.u16(mp + 0x10)
        smap = r.ptr(mp + 0x20)
        geoms = []
        for k in range(gcount):
            g = read_geometry(r, r.ptr(gptr + 8 * k))
            g.shader = r.u16(smap + 2 * k)
            geoms.append(g)
        out.append(geoms)
    return out


def read_shaders(r, groupptr):
    """Shader group -> list of (shader hash, {param hash: texture name or vector})."""
    arr = r.ptr(groupptr + 0x10)
    cnt = r.u16(groupptr + 0x18)
    shaders = []
    for i in range(cnt):
        sp = r.ptr(arr + 8 * i)
        params = r.ptr(sp)
        shash = r.u32(sp + 8)
        n = r.u8(sp + 0x10)
        psize = r.u16(sp + 0x14)
        hashes = struct.unpack_from('<%dI' % n, r.raw, params + psize)
        d = {}
        for k in range(n):
            row = params + 16 * k
            typ = r.u8(row)
            dp = r.u64(row + 8)
            if dp == 0:
                d[hashes[k]] = None
                continue
            do = r.off(dp)
            if typ == 0:
                if r.raw[do:do + 4] == b'TXEX':
                    nm = r.ptr(do + 0x28)
                    d[hashes[k]] = r.cstr(nm)
                else:
                    d[hashes[k]] = None
            else:
                d[hashes[k]] = struct.unpack_from('<%df' % (4 * typ), r.raw, do)
        shaders.append((shash, d))
    return shaders


def dds_bytes(w, h, fourcc, data, levels=1):
    hdr = struct.pack('<4sIIIIIII44sII4sIIIII', b'DDS ', 124, 0x1007 | (0x20000 if levels > 1 else 0), h, w, 0, 0, levels, b'\0' * 44,
                      32, 4, fourcc, 0, 0, 0, 0, 0)
    hdr += struct.pack('<IIIII', 0x1000 | (0x400008 if levels > 1 else 0), 0, 0, 0, 0)
    return hdr + data


def read_ytd(path):
    """Texture dictionary -> {name: (width, height, fourcc, levels, data)}; data = the whole mip chain."""
    raw = inflate(path)
    r = Res(raw)
    # the system segment is 0x2000 bytes here; graphics pointers (0x6...) are relative to the graphics segment that follows it
    sysflags = struct.unpack_from('<I', open(path, 'rb').read(16), 8)[0]
    ss = sysflags & 0xF
    base = 0x200 << ss
    s = [(sysflags >> 4) & 1, (sysflags >> 5) & 3, (sysflags >> 7) & 0xF, (sysflags >> 11) & 0x3F, (sysflags >> 17) & 0x7F,
         (sysflags >> 24) & 1, (sysflags >> 25) & 1, (sysflags >> 26) & 1, (sysflags >> 27) & 1]
    sys_size = sum(c << (8 - i) for i, c in enumerate(s)) * base
    out = {}
    tl = r.ptr(0x30)
    n = r.u16(0x38)
    for i in range(n):
        t = r.ptr(tl + 8 * i)
        name = r.cstr(r.ptr(t + 0x28))
        w, h = r.u16(t + 0x50), r.u16(t + 0x52)
        fcc = raw[t + 0x58:t + 0x5C]
        levels = raw[t + 0x5D]
        dp = r.u64(t + 0x70)
        do = (dp & 0x0FFFFFFF) + sys_size if (dp >> 28) == 6 else dp & 0x0FFFFFFF
        bs = 8 if fcc == b'DXT1' else 16
        size = 0
        mw, mh = w, h
        for l in range(max(levels, 1)):
            if fcc in (b'DXT1', b'DXT3', b'DXT5'):
                size += max(1, (mw + 3) // 4) * max(1, (mh + 3) // 4) * bs
            else:
                size += mw * mh * 4
            mw, mh = max(1, mw // 2), max(1, mh // 2)
        out[name] = (w, h, fcc, levels, raw[do:do + size])
    return out
