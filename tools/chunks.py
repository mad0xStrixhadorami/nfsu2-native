#!/usr/bin/env python3
"""List EA bChunk trees: u32 id, u32 size, payload. Ids with bit 31 set contain child chunks.
Usage: chunks.py FILE [max_depth]"""
import struct
import sys


def walk(data, off, end, depth, maxd):
    while off + 8 <= end:
        cid, size = struct.unpack_from('<II', data, off)
        body = off + 8
        if body + size > end:
            print('  ' * depth + f'!! bad chunk {cid:08x} size {size} at {off:#x}')
            return
        if cid != 0:
            text = bytes(b for b in data[body:body + 64] if 32 <= b < 127)[:24].decode()
            print('  ' * depth + f'{cid:08x} @{off:#09x} size {size:>9}  {text}')
            if cid & 0x80000000 and depth < maxd:
                walk(data, body, body + size, depth + 1, maxd)
        off = body + size


def main():
    data = open(sys.argv[1], 'rb').read()
    walk(data, 0, len(data), 0, int(sys.argv[2]) if len(sys.argv) > 2 else 3)


if __name__ == '__main__':
    main()
