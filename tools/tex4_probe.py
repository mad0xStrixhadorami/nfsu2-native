#!/usr/bin/env python3
"""Probe how PS2 4-bit textures in a TPK were uploaded: try upload formats/widths and
render a contact sheet. Usage: tex4_probe.py BUNDLE TEXNAME OUT.png"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import gs_tables as T
from PIL import Image, ImageDraw

def find_chunk(d, cid, off=0, end=None):
    end = len(d) if end is None else end
    while off + 8 <= end:
        i, s = struct.unpack_from('<II', d, off)
        if i == cid: return off + 8, s
        if i & 0x80000000:
            r = find_chunk(d, cid, off + 8, off + 8 + s)
            if r: return r
        off += 8 + s
    return None

def a8(x,y,pw): return ((y//64)*pw + x//128)*8192 + T.BLOCKTABLE8[(y//16)%4][(x//16)%8]*256 + T.COLUMNTABLE8[y%16][x%16]
def a4(x,y,pw): return ((y//128)*pw + x//128)*16384 + T.BLOCKTABLE4[(y//16)%8][(x//32)%4]*512 + T.COLUMNTABLE4[y%16][x%32]
def a32(x,y,pw): return (((y//32)*pw + x//64)*2048 + T.BLOCKTABLE32[(y//8)%4][(x//8)%8]*64 + T.COLUMNTABLE32[y%8][x%8])*4

def main():
    d = open(sys.argv[1], 'rb').read()
    info, isz = find_chunk(d, 0x33310004)
    blob, bsz = find_chunk(d, 0x33320002)
    while d[blob] == 0x11: blob += 1
    E = {}
    for i in range(isz // 124):
        o = info + i*124; n = d[o+12:o+36].split(b'\0')[0].decode()
        _,_,_,doff,poff,dsz,psz,_ = struct.unpack_from('<8I', d, o+36); w,h = struct.unpack_from('<HH', d, o+68)
        E[n] = (doff,poff,dsz,psz,w,h)
    n = sys.argv[2]; doff,poff,dsz,psz,w,h = E[n]; data = d[blob+doff:blob+doff+dsz]
    pal = [tuple(d[blob+poff+4*i:blob+poff+4*i+4]) for i in range(16)]
    pal = [(r,g,b,min(255,a*2)) for r,g,b,a in pal]
    def decode(mode, ww, pw_w, pw_r):
        mem = bytearray(1 << 20)
        if mode == 'T8':
            for i,b in enumerate(data): mem[a8(i%ww, i//ww, pw_w)] = b
        elif mode == 'T32':
            for i in range(len(data)//4):
                a = a32(i%ww, i//ww, pw_w); mem[a:a+4] = data[4*i:4*i+4]
        else:
            mem[:len(data)] = data
        out = []
        for y in range(h):
            for x in range(w):
                q = a4(x,y,pw_r) if mode != 'LIN' else y*w + x
                b = mem[q >> 1]; out.append(pal[(b>>4) if q & 1 else (b & 15)])
        img = Image.new('RGBA',(w,h)); img.putdata(out)
        bg = Image.new('RGBA',(w,h),(60,60,60,255)); bg.alpha_composite(img); return bg
    combos = [('LIN',0,0,0)] + [('T8',ww,max(1,ww//128),pr) for ww in (64,128,256) for pr in (1,2)] + \
             [('T32',ww,max(1,ww//64),pr) for ww in (64,128) for pr in (1,2)]
    sheet = Image.new('RGBA',(w,(h+12)*len(combos)),(0,0,0,255)); dr = ImageDraw.Draw(sheet)
    for k,c in enumerate(combos):
        y0 = k*(h+12); dr.text((2,y0), str(c), fill=(255,255,0,255)); sheet.alpha_composite(decode(*c),(0,y0+12))
    sheet.resize((w*2, sheet.size[1]*2), Image.NEAREST).save(sys.argv[3])

if __name__ == "__main__":
    main()
