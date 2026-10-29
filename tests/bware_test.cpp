// Runtime checks for the reused bWare core on 64-bit Linux.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "Speed/Indep/bWare/Inc/bMemory.hpp"
#include "Speed/Indep/bWare/Inc/bPrintf.hpp"
#include "Speed/Indep/bWare/Inc/bSlotPool.hpp"
#include "Speed/Indep/bWare/Inc/bChunk.hpp"
#include "Speed/Indep/bWare/Inc/bList.hpp"
#include "Speed/Indep/bWare/Inc/bMath.hpp"
#include "Speed/Indep/bWare/Inc/bWare.hpp"

// the formerly stripped functions have no declaration in the upstream headers
bFix bInverse(bFix a);
uint32 bSqrt32(uint32 a);
bAngle bOldATan(float x, float y);
bAngle bFastATan(float x, float y);
bFix bFixSin(bAngle angle);
void bFixSinCos(bFix *result_sin, bFix *result_cos, bAngle angle);
bPolar *bToPolar(bPolar *dest, bVector2 *cartesian);
bool bValidateChunks(bChunk *first_chunk, int sizeof_chunks);
char *bStrChr(const char *s1, int c);
int bStrCmp(uint16 *s1, uint16 *s2);
int bStrNCmp(uint16 *s1, uint16 *s2, int n);
uint16 *bStrCat(uint16 *to, uint16 *s1, uint16 *s2);
void bExpandBoundingBox(bVector2 *bbox_min, bVector2 *bbox_max, const bVector2 *point, float extra_width);
float bBoundingBoxDistOutside(const bVector2 *bbox_min, const bVector2 *bbox_max, const bVector2 *point);

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

    puts("formerly stubbed functions");
    CHECK(bSqrt32(0) == 0 && bSqrt32(15) == 3 && bSqrt32(16) == 4 && bSqrt32(0xffffffffu) == 65535);
    CHECK(bAbs(float(bInverse(0x20000) - 0x8000)) < 8.0f); // 16.16: 1 / 2 = 0.5, within the format's rounding
    CHECK(bFastATan(1.0f, 1.0f) == bDegToAng(45.0f) && bFastATan(0.0f, 1.0f) == bDegToAng(90.0f) && bFastATan(-1.0f, 0.0f) == bDegToAng(180.0f));
    CHECK(bOldATan(0.0f, -1.0f) == bFastATan(0.0f, -1.0f));
    {
        bFix si, co;
        bFixSinCos(&si, &co, bDegToAng(90.0f));
        CHECK(si > 65530 && si <= 65536 && bAbs(float(co)) < 8.0f);
        CHECK(bFixSin(bDegToAng(30.0f)) > 32700 && bFixSin(bDegToAng(30.0f)) < 32830);
        bVector2 c = {3.0f, 4.0f};
        bPolar pol;
        bToPolar(&pol, &c);
        CHECK(bAbs(pol.r - 5.0f) < 1e-4f);
    }
    {
        struct N : bNode { int key; };
        N n[5];
        bList a, b;
        a.InitList(), b.InitList();
        const int keys[5] = {30, 10, 20, 50, 40};
        auto flip = [](bNode *x, bNode *y) { return static_cast<N *>(x)->key < static_cast<N *>(y)->key ? 1 : 0; };
        for (int i = 0; i < 3; ++i) n[i].key = keys[i], a.AddSorted(flip, &n[i]);
        CHECK(static_cast<N *>(a.GetHead())->key == 10 && static_cast<N *>(a.GetTail())->key == 30);
        for (int i = 3; i < 5; ++i) n[i].key = keys[i], b.AddTail(&n[i]);
        a.AddTail(&b);
        CHECK(b.IsEmpty() && static_cast<N *>(a.GetTail())->key == 40 && a.CountElements() == 5);
        bList c;
        c.InitList();
        c.AddHead(&a);
        CHECK(a.IsEmpty() && c.CountElements() == 5 && static_cast<N *>(c.GetHead())->key == 10);
    }
    {
        // two chunks (ids 0x1, 0x2) and an unloader that rejects id 2
        alignas(16) unsigned char buf[64] = {};
        auto *c0 = reinterpret_cast<bChunk *>(buf);
        c0->ID = 1, c0->Size = 8;
        auto *c1 = reinterpret_cast<bChunk *>(buf + 16);
        c1->ID = 2, c1->Size = 8;
        CHECK(bValidateChunks(c0, 32) && !bValidateChunks(c0, 31));
        static int unloaded = 0;
        static bChunkLoader l1(1, [](bChunk *) { return 1; }, [](bChunk *) { unloaded++; return 1; });
        static bChunkLoader l2(2, [](bChunk *) { return 1; }, [](bChunk *) { return 0; });
        CHECK(bChunkLoader::CallUnloaders(c0, 32, true) == 2 && unloaded == 1);
        CHECK(bChunkLoader::CallUnloaders(c0, 32, false) == 0);
        CHECK(bChunkLoader::CallLoaders(c0, 32, true) == 0);
    }
    {
        bVector2 lo, hi, p = {5.0f, 7.0f}, q = {-1.0f, 2.0f};
        bInitializeBoundingBox(&lo, &hi, &p);
        bExpandBoundingBox(&lo, &hi, &q, 1.0f);
        CHECK(lo.x == -2.0f && lo.y == 1.0f && hi.x == 5.0f && hi.y == 7.0f);
        bVector2 out = {9.0f, 8.0f};
        CHECK(bAbs(bBoundingBoxDistOutside(&lo, &hi, &out) - 4.1231055f) < 1e-4f);
        bVector3 a3 = {1, 2, 3}, b3 = {1, 2, 3.0005f};
        CHECK(bEqual(&a3, &b3, 0.001f) && !bEqual(&a3, &b3, 0.0001f));
    }
    {
        char cat[16] = "ab";
        CHECK(bStrChr("hello", 'l') && bStrChr("hello", 'l')[1] == 'l' && !bStrChr("hello", 'z'));
        uint16 w1[] = {'a', 'b', 0}, w2[] = {'a', 'c', 0}, out[8];
        CHECK(bStrCmp(w1, w2) < 0 && bStrNCmp(w1, w2, 1) == 0);
        bStrCat(out, w1, w2);
        CHECK(out[0] == 'a' && out[3] == 'c' && out[4] == 0);
        (void)cat;
        bMemoryAllocator allocator;
        void *m = allocator.Alloc(128);
        CHECK(m != nullptr);
        bFree(m);
    }
    printf("bware_test: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
