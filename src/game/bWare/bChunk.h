#pragma once
#include <cstdint>
#include <cstring>

// EA bChunk: u32 id, u32 size, payload. Ids with bit 31 set hold child chunks.
struct bChunk {
    uint32_t id;
    uint32_t size;

    const uint8_t *Data() const { return reinterpret_cast<const uint8_t *>(this) + 8; }
    const bChunk *Next() const { return reinterpret_cast<const bChunk *>(Data() + size); }
    bool IsContainer() const { return (id & 0x80000000u) != 0; }
};

// Find the first chunk with this id in [begin, end), descending into containers.
inline const bChunk *bFindChunk(const uint8_t *begin, const uint8_t *end, uint32_t id)
{
    for (const uint8_t *p = begin; p + 8 <= end;) {
        bChunk c;
        memcpy(&c, p, sizeof(c));
        const bChunk *chunk = reinterpret_cast<const bChunk *>(p);
        if (p + 8 + c.size > end)
            return nullptr;
        if (c.id == id)
            return chunk;
        if (c.IsContainer())
            if (const bChunk *inner = bFindChunk(chunk->Data(), chunk->Data() + c.size, id))
                return inner;
        p = chunk->Data() + c.size;
    }
    return nullptr;
}
