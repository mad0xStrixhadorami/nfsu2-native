#pragma once
#include <cstdint>

// EA bWare string hashing. h = h * 33 + c, seeded with 0xFFFFFFFF.
uint32_t bStringHash(const char *str);                  // 0x00156d78
uint32_t bStringHash(const char *str, uint32_t prefix); // 0x00156db8, continues from an existing hash
