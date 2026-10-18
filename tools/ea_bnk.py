#!/usr/bin/env python3
"""List / decode the sounds of an EA BNKl bank inside a UG2 ABKC bank (SOUND\\ENGINE\\*_SPU.abk).

BNKl (at the ABK's +0x20 offset): 'BNKl', u16 version, u16 sound count, u32 header size,
u32 total size, 4 bytes, then one u32 per sound: offset (from its own slot) of a "PT" header:
tag/length/value records (0x80 version, 0x84 rate, 0x85 samples, 0x86 loop start, 0x87 loop end,
0x88 data start from the bank, 0xFF end). PS2 default codec: SPU ADPCM ("VAG", 16-byte frames).
Usage: ea_bnk.py BANK.abk [OUT_PREFIX]"""
import struct
import sys
import wave

VAG = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def pt(d, o):
    p, tags = o + 4, {}
    while p < len(d):
        tag = d[p]
        p += 1
        if tag == 0xFF:
            break
        if tag in (0xFC, 0xFD, 0xFE):
            continue
        n = d[p]
        p += 1
        tags[tag] = int.from_bytes(d[p:p + n], 'big')
        p += n
    return tags


def vag(data, count):
    out, h1, h2 = [], 0, 0
    for f in range(0, len(data) - 15, 16):
        shift, pred = data[f] & 0x0f, min(data[f] >> 4, 4)
        c1, c2 = VAG[pred]
        for i in range(28):
            b = data[f + 2 + i // 2]
            nib = (b & 0x0f) if i % 2 == 0 else (b >> 4)
            s = ((nib << 12) - ((nib & 8) << 13)) >> shift
            s = s + (h1 * c1 + h2 * c2 + 32) // 64
            s = max(-32768, min(32767, s))
            out.append(s)
            h2, h1 = h1, s
        if len(out) >= count:
            break
    return out[:count]


def main():
    d = open(sys.argv[1], 'rb').read()
    bank = d.find(b'BNKl')
    ver, count = struct.unpack_from('<HH', d, bank + 4)
    print('BNKl v%d, %d sounds' % (ver, count))
    for i in range(count):
        slot = bank + 0x18 + 4 * i
        rel = struct.unpack_from('<I', d, slot)[0]
        if not rel or d[slot + rel:slot + rel + 2] != b'PT':
            print('%d: (empty)' % i)
            continue
        tags = pt(d, slot + rel)
        n, rate = tags.get(0x85, 0), tags.get(0x84, 22050)
        start = tags.get(0x88, 0)
        print('%d: %6d samples @ %5d Hz (%.2f s) loop %d..%d data +0x%x' %
              (i, n, rate, n / rate, tags.get(0x86, 0), tags.get(0x87, 0), start))
        if len(sys.argv) > 2:
            nbytes = (n + 27) // 28 * 16
            pcm = vag(d[bank + start:bank + start + nbytes], n)
            w = wave.open('%s%d.wav' % (sys.argv[2], i), 'wb')
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))
            w.close()


if __name__ == '__main__':
    main()
