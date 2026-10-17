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
