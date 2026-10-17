#pragma once
#include <cstdint>
#include <cstddef>

// EA compressed blocks. Every block starts with a 4-byte tag and a version byte.
// Function names are ours (no symbols in the PS2 build).
enum : uint32_t {
    kTagJDLZ = 0x5a4c444a, // "JDLZ", version 2
    kTagHUFF = 0x46465548, // "HUFF", version 1
    kTagCOMP = 0x504d4f43, // "COMP", version 1
    kTagRAWW = 0x57574152, // "RAWW", version 1
};

bool bIsCompressed(const void *src);                      // 0x002e9898
uint32_t bGetDecompressedSize(const void *src);            // header +8 (uncompressed size)
uint32_t bDecompress(const void *src, void *dst);          // 0x002e9910, returns bytes written or 0
uint32_t bDecompressJDLZ(const void *src, void *dst);      // 0x002e8590
