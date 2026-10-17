#include "game/bWare/bCompression.h"

#include <cstring>

uint32_t bHuffDecode(const uint8_t *src, uint8_t *dst); // bHuffman.cpp, 0x002e8720

static uint32_t ReadU32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// 0x002e8540: forward byte copy, so overlapping back-references repeat bytes.
static void CopyForward(uint8_t *dst, const uint8_t *src, int count)
{
    for (int i = 0; i < count; ++i)
        dst[i] = src[i];
}

// 0x002e9898
bool bIsCompressed(const void *src)
{
    const uint8_t *p = static_cast<const uint8_t *>(src);
    switch (ReadU32(p)) {
    case kTagJDLZ:
        return p[4] == 2;
    case kTagHUFF:
    case kTagCOMP:
    case kTagRAWW:
        return p[4] == 1;
    default:
        return false;
    }
}

uint32_t bGetDecompressedSize(const void *src)
{
    return ReadU32(static_cast<const uint8_t *>(src) + 8);
}

// 0x002e8590
// Header: +0 "JDLZ", +4 version 2, +8 uncompressed size, +12 compressed size (with header),
// +16 first literal/match flag byte, +17 first match-kind flag byte, data from +18.
uint32_t bDecompressJDLZ(const void *src, void *dstv)
{
    const uint8_t *hdr = static_cast<const uint8_t *>(src);
    if (ReadU32(hdr) != kTagJDLZ || hdr[4] != 2)
        return 0;
    const uint32_t outSize = ReadU32(hdr + 8);
    int remaining = static_cast<int>(ReadU32(hdr + 12)) - 0x12;
    const uint8_t *in = hdr + 0x12;
    uint8_t *out = static_cast<uint8_t *>(dstv);
    uint8_t *const outEnd = out + outSize;

    // Each flag word holds 8 flags plus a sentinel bit 0x100; it is reloaded when only the
    // sentinel is left.
    unsigned litFlags = hdr[16] | 0x100;   // bit 0: 0 = literal, 1 = back-reference
    unsigned kindFlags = hdr[17] | 0x100;  // bit 0: 0 = far reference, 1 = near/run reference

    while (remaining != 0) {
        if ((litFlags & 1) == 0) {
            --remaining;
            if (out < outEnd)
                *out++ = *in++;
        } else {
            int length;
            if ((kindFlags & 1) == 0) {
                length = (in[0] & 0x1f) + 3;
                int distance = (((in[0] >> 5) << 8) | in[1]) + 0x11;
                CopyForward(out, out - distance, length);
            } else {
                length = (((in[0] >> 4) << 8) | in[1]) + 3;
                int distance = (in[0] & 0x0f) + 1;
                CopyForward(out, out - distance, length);
            }
            out += length;
            in += 2;
            remaining -= 2;
            kindFlags >>= 1;
        }
        litFlags >>= 1;
        if (litFlags == 1) {
            litFlags = *in++ | 0x100;
            --remaining;
        }
        if (kindFlags == 1) {
            kindFlags = *in++ | 0x100;
            --remaining;
        }
    }
    return outSize;
}

// 0x002e9910
uint32_t bDecompress(const void *src, void *dst)
{
    if (!src || !dst)
        return 0;
    switch (ReadU32(static_cast<const uint8_t *>(src))) {
    case kTagJDLZ:
        return bDecompressJDLZ(src, dst);
    case kTagHUFF: // 0x002e9640: 16-byte header, then huffcodex data
        if (static_cast<const uint8_t *>(src)[4] != 1)
            return 0;
        bHuffDecode(static_cast<const uint8_t *>(src) + 16, static_cast<uint8_t *>(dst));
        return bGetDecompressedSize(src);
    case kTagCOMP: // 0x002e96f8, todo
    case kTagRAWW: // 0x002e9698, todo
    default:
        return 0;
    }
}
