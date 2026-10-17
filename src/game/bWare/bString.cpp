#include "game/bWare/bString.h"

// 0x00156d78
uint32_t bStringHash(const char *str)
{
    return bStringHash(str, 0xFFFFFFFFu);
}

// 0x00156db8
uint32_t bStringHash(const char *str, uint32_t prefix)
{
    uint32_t h = prefix;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(str); *p; ++p)
        h = h * 33 + *p;
    return h;
}
