// Linux implementations of the per-platform pieces EA's bWare core expects from each console.
#include <cstring>
#include <mutex>
#include <new>

#include "Speed/Indep/Libs/realcore/6.24.00/include/common/realcore/system.h"

// RealSystem::Mutex reserves 28 bytes for the platform lock. A pthread mutex is 40 bytes on
// x86-64, so the buffer holds a pointer to a heap-allocated recursive mutex instead.
static_assert(sizeof(RealSystem::Mutex) >= sizeof(std::recursive_mutex *));

static std::recursive_mutex *&Impl(RealSystem::Mutex *m)
{
    return *reinterpret_cast<std::recursive_mutex **>(m->mBuf);
}

namespace RealSystem {

Mutex::Mutex() { memset(mBuf, 0, sizeof(mBuf)); }

void Mutex::Create()
{
    if (!Impl(this))
        Impl(this) = new std::recursive_mutex();
}

void Mutex::Destroy()
{
    delete Impl(this);
    Impl(this) = nullptr;
}

void Mutex::Lock()
{
    Create(); // tolerate Lock before Create, as statics may be used early
    Impl(this)->lock();
}

void Mutex::Unlock() { Impl(this)->unlock(); }

} // namespace RealSystem

extern "C" void bMemSet(void *dest, unsigned char pattern, unsigned int size) { memset(dest, pattern, size); }

// GameCube auxiliary-RAM pool. There is no such memory here; report it as unavailable.
extern "C" int VMAlloc(uintptr_t, size_t) { return 0; }

// Synchronous variant of the dev-kit debug link (see bFunkStub.cpp).
int bFunkCallSync(const char *, int, const void *, int, void *, int) { return 0; }

// ---------------------------------------------------------------------------------------------
// Memory ownership. EA code allocates with bMalloc (directly or via operator new overloads such
// as FEng's FNEW) but frees with plain delete. The original games replace the global operator
// delete with bFree. Here delete routes by address: memory inside a registered bWare arena goes
// to bFree, everything else to the C library.
#include <cstdlib>

#include "Speed/Indep/bWare/Inc/bWare.hpp"
#include "platform/linux/PlatformMemory.h"

namespace {
struct Arena {
    const char *begin, *end;
};
Arena g_arenas[16];
int g_arenaCount = 0;

bool InArena(const void *p)
{
    for (int i = 0; i < g_arenaCount; ++i)
        if (p >= g_arenas[i].begin && p < g_arenas[i].end)
            return true;
    return false;
}

void Release(void *p)
{
    if (!p)
        return;
    if (InArena(p))
        bFree(p);
    else
        free(p);
}
} // namespace

namespace platform {
void RegisterMemoryArena(void *mem, size_t size)
{
    if (g_arenaCount < 16)
        g_arenas[g_arenaCount++] = {static_cast<char *>(mem), static_cast<char *>(mem) + size};
}

void InitMainMemoryPool(size_t size)
{
    void *mem = aligned_alloc(128, size);
    RegisterMemoryArena(mem, size);
    bInitMemoryPool(0, mem, static_cast<int>(size), "Main Pool");
}
} // namespace platform

void operator delete(void *p) noexcept { Release(p); }
void operator delete[](void *p) noexcept { Release(p); }
void operator delete(void *p, size_t) noexcept { Release(p); }
void operator delete[](void *p, size_t) noexcept { Release(p); }
