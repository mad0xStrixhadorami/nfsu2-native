#!/usr/bin/env python3
"""Dump UG2's car part database (GLOBALB chunk 0x80034602, same pack format as MW, version 6).
Usage: carparts.py GLOBALB.bin SLES_527.25 [CARTYPE [filter]]   (ISSUES.md #134)
Parts are 14-byte records {u16 hashBot, u16 hashTop, i8 partId, u8 group(5)|level(3)<<5, i8 modelSel,
u8 carTypeIdx, u16 nameOff(*4 into strings), u16 attrTable, u16 modelTable}."""
import re, struct, sys

def bhash(s, upper=False):
    h = 0xFFFFFFFF
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h

def main():
    d = open(sys.argv[1], 'rb').read()
    elf = open(sys.argv[2], 'rb').read()
    ch = {}
    o = 0
    while o + 8 <= len(d):
        c, s = struct.unpack_from('<II', d, o)
        if c == 0x80034602:
            q = o + 8
            while q + 8 <= o + 8 + s:
                c2, s2 = struct.unpack_from('<II', d, q)
                ch[c2] = d[q + 8:q + 8 + s2]
                q += 8 + s2
        o += 8 + s
    pack = ch[0x34603]
    version, = struct.unpack_from('<I', pack, 8)
    numAttr, = struct.unpack_from('<I', pack, 0x20)
    numTypes, = struct.unpack_from('<I', pack, 0x28)
    numModels, = struct.unpack_from('<I', pack, 0x30)
    numParts, = struct.unpack_from('<I', pack, 0x38)
    parts, attrs, strs, types, tables = ch[0x34604], ch[0x34605], ch[0x34606], ch[0x3460b], ch[0x3460c]
    names = {}
    for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_ ]{3,40}', elf):
        names[bhash(m.group().decode())] = m.group().decode()
    def cstr(off):
        e = strs.index(0, off * 4)
        return strs[off * 4:e].decode('latin1')
    ctype = [struct.unpack_from('<I', types, 4 * i)[0] for i in range(numTypes)]
    # Car type names: the folder names of the car database (GLOBALB chunk 0x34600) hashed with bStringHash.
    o = 0
    while o + 8 <= len(d):
        c, s = struct.unpack_from('<II', d, o)
        if c == 0x34600:
            for k in range(s // 0x890):
                folder = d[o + 8 + k * 0x890 + 8:o + 8 + k * 0x890 + 24].split(b'\0')[0].decode()
                names[bhash(folder)] = folder
        o += 8 + s
    print('version', version, 'parts', numParts, 'attributes', numAttr, 'typenames', numTypes, file=sys.stderr)
    want = sys.argv[3].upper() if len(sys.argv) > 3 else None
    flt = sys.argv[4].upper() if len(sys.argv) > 4 else None
    for i in range(numParts):
        hb, ht, pid, gl, sel, ti, no, ao, mo = struct.unpack_from('<HHbBbBHHH', parts, 14 * i)
        tname = names.get(ctype[ti], '%08x' % ctype[ti]) if ti < numTypes else '?'
        if want and tname.upper() != want:
            continue
        pname = cstr(no)
        if flt and flt not in pname.upper():
            continue
        line = '%5d %-14s id %3d grp %2d lvl %d  %s' % (i, tname, pid, gl & 31, gl >> 5, pname)
        if ao != 0xFFFF:
            n, = struct.unpack_from('<h', tables, ao * 2) if ao * 2 + 2 <= len(tables) else (0,)
            offs = struct.unpack_from('<%dh' % n, tables, ao * 2 + 2) if 0 <= n < 64 else ()
            a = []
            for off in offs:
                if off * 8 + 8 <= len(attrs):
                    nh, pv = struct.unpack_from('<II', attrs, off * 8)
                    a.append('%s=%s' % (names.get(nh, '%08x' % nh), '%g' % struct.unpack('<f', struct.pack('<I', pv))[0] if 0x30000000 < pv < 0x50000000 else pv))
            line += '  [' + ', '.join(a) + ']'
        print(line)

main()
