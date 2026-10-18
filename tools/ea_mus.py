#!/usr/bin/env python3
"""Decode EA SCHl audio streams (UG2 PS2 SOUND\\PFDATA\\MUSICSFX.MUS) to WAV for checking.

Stream: SCHl header ("GSTR" variant: tag/value pairs, 0x82 channels, 0x84 sample rate, 0x85
sample count, 0xFF end), SCCl (block count), SCDl blocks, SCEl end. SCDl body: u32 BE samples in
this block, u32 BE offset of each channel's data (from after the offsets), then per channel EA-XA
v2 frames: 0xEE + 2 BE history samples + 28 BE PCM samples (61 bytes), or a coefficient/shift byte
+ 14 bytes of 4-bit samples, high nibble first (15 bytes) -- 28 samples per frame.
Usage: ea_mus.py FILE.MUS OUT.wav [stream_index] [max_seconds]"""
import struct
import sys
import wave

EA_XA = [0, 240, 460, 392, 0, 0, -208, -220]


def header(d, o):
    """SCHl at o -> (channels, rate, samples)."""
    size = struct.unpack_from('<I', d, o + 4)[0]
    p, end = o + 8, o + size
    if d[p:p + 4] == b'GSTR':
        p += 8
    tags = {}
    while p < end:
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
    return tags.get(0x82, 1), tags.get(0x84, 22050), tags.get(0x85, 0)


def decode_channel(data, count, hist):
    out, p = [], 0
    h1, h2 = hist
    while len(out) < count and p < len(data):
        info = data[p]
        if info == 0xEE:
            h1, h2 = struct.unpack_from('>hh', data, p + 1)
            frame = list(struct.unpack_from('>28h', data, p + 5))
            out += frame
            h1, h2 = frame[27], frame[26]
            p += 61
        else:
            c1, c2 = EA_XA[info >> 4], EA_XA[(info >> 4) + 4]
            shift = (info & 0x0F) + 8
            for i in range(28):
                b = data[p + 1 + i // 2]
                nib = (b >> 4) if i % 2 == 0 else (b & 0x0F)
                s = ((nib << 28) - ((nib & 8) << 29)) >> shift  # sign-extended nibble << 28, >> shift
                s = (s + h1 * c1 + h2 * c2 + 128) >> 8
                s = max(-32768, min(32767, s))
                out.append(s)
                h2, h1 = h1, s
            p += 15
    return out[:count], (h1, h2), p


def streams(d):
    o = d.find(b'SCHl')
    while o >= 0:
        yield o
        o = d.find(b'SCHl', o + 4)


def main():
    d = open(sys.argv[1], 'rb').read()
    which = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    limit = float(sys.argv[4]) if len(sys.argv) > 4 else 30
    starts = list(streams(d))
    print(len(starts), 'streams')
    o = starts[which]
    ch, rate, total = header(d, o)
    print('channels', ch, 'rate', rate, 'samples', total, '%.1f s' % (total / rate))
    o += struct.unpack_from('<I', d, o + 4)[0]
    hist = [(0, 0)] * ch
    pcm = [[] for _ in range(ch)]
    used_ok = 0
    while o + 8 <= len(d) and len(pcm[0]) < limit * rate:
        tag, size = d[o:o + 4], struct.unpack_from('<I', d, o + 4)[0]
        if tag == b'SCEl' or size == 0:
            break
        if tag == b'SCDl':
            body = d[o + 8:o + size]
            n = struct.unpack_from('>I', body, 0)[0]
            offs = struct.unpack_from('>%dI' % ch, body, 4)
            base = 4 + 4 * ch
            for c in range(ch):
                endc = base + (offs[c + 1] if c + 1 < ch else len(body) - base)
                s, hist[c], used = decode_channel(body[base + offs[c]:endc], n, hist[c])
                used_ok += used == (endc - base - offs[c]) or abs(used - (endc - base - offs[c])) < 4
                pcm[c] += s
        o += size
    print('decoded', len(pcm[0]), 'samples; blocks whose frames fill the channel data:', used_ok)
    w = wave.open(sys.argv[2], 'wb')
    w.setnchannels(ch)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(b''.join(struct.pack('<%dh' % ch, *[pcm[c][i] for c in range(ch)]) for i in range(len(pcm[0]))))
    w.close()


if __name__ == '__main__':
    main()
