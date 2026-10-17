#pragma once
#include <cstddef>

namespace platform {
// Create bWare memory pool 0 ("Main Pool") from host memory. Call before any EA code allocates.
void InitMainMemoryPool(size_t size);
// Tell operator delete that [mem, mem+size) belongs to bWare (see PlatformSystem.cpp).
void RegisterMemoryArena(void *mem, size_t size);
} // namespace platform
