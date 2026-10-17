// Runtime checks for the reused bWare core on 64-bit Linux.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "Speed/Indep/bWare/Inc/bMemory.hpp"
#include "Speed/Indep/bWare/Inc/bPrintf.hpp"
#include "Speed/Indep/bWare/Inc/bSlotPool.hpp"
#include "Speed/Indep/bWare/Inc/bWare.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        bool ok_ = (cond);                                                                         \
        printf("  %-60s %s\n", #cond, ok_ ? "ok" : "FAIL");                                       \
        g_failures += !ok_;                                                                        \
    } while (0)

int main()
{
    puts("strings");
    // Known ZDIR hash: bStringHash of the upper-case path, from the disc index.
    CHECK(bStringHash("GLOBAL\\GLOBALB.LZC") == 0x4c939b36u);
    CHECK(bStrCmp("abc", "abc") == 0);
    CHECK(bStrICmp("ABC", "abc") == 0);
    CHECK(bStrLen("ZZDATA") == 6);
    char buf[64];
    bSPrintf(buf, "%s%d.BIN", "NFSUNDER\\ZZDATA", 3);
    CHECK(strcmp(buf, "NFSUNDER\\ZZDATA3.BIN") == 0);

    puts("memory pools");
    const int size = 64 << 20;
    void *arena = aligned_alloc(128, size);
    bInitMemoryPool(0, arena, size, "Main Pool");
    void *a = bWareMalloc(1000, "test", __LINE__, 0);
    void *b = bWareMalloc(4096, "test", __LINE__, 0);
    CHECK(a && b && a != b);
    CHECK((char *)a >= (char *)arena && (char *)b < (char *)arena + size);
    memset(a, 0xAB, 1000);
    memset(b, 0xCD, 4096);
    CHECK(((unsigned char *)a)[999] == 0xAB);
    bFree(a);
    bFree(b);
    CHECK(bCountFreeMemory(0) > size - 4096);

    puts("slot pools");
    SlotPool *pool = bNewSlotPool(24, 100, "test pool", 0);
    CHECK(pool != nullptr);
    void *s[100];
    for (auto &p : s)
        p = bOMalloc(pool);
    CHECK(s[0] && s[99] && s[0] != s[99]);
    CHECK(bIsSlotPoolFull(pool));
    for (auto &p : s)
        bFree(pool, p);
    CHECK(bCountFreeSlots(pool) == 100);

    printf("bware_test: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
