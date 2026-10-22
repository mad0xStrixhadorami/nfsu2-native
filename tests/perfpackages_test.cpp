// PerfPackages parser test (ISSUES #197): a synthetic 0x80034a10 container with a 0x34a15 chunk.
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/indep/Frontend/PerfPackages.h"

namespace {

void Put32(std::vector<uint8_t> &b, size_t off, uint32_t v)
{
    if (b.size() < off + 4)
        b.resize(off + 4, 0);
    memcpy(&b[off], &v, 4);
}
void PutF(std::vector<uint8_t> &b, size_t off, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    Put32(b, off, u);
}

int g_fail = 0;
void Check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_fail;
    }
}

} // namespace

int main()
{
    // Two packages: (cat 0, lvl 1) with two parts, (cat 3, lvl 2) with one.
    std::vector<uint8_t> chunk(0x17c * 2, 0);
    Put32(chunk, 0, 0);
    Put32(chunk, 4, 1);
    Put32(chunk, 8, 2);
    for (uint32_t i = 0; i < 2; ++i) {
        const size_t p = 0xc + i * 0x5c;
        Put32(chunk, p, i);
        Put32(chunk, p + 4, 0x1000 + i);
        Put32(chunk, p + 8, 250 * (i + 1));
        PutF(chunk, p + 0x30, 0.2f);
        for (int k = 1; k < 4; ++k)
            PutF(chunk, p + 0x30 + k * 4, -1.0f);
    }
    Put32(chunk, 0x17c, 3);
    Put32(chunk, 0x17c + 4, 2);
    Put32(chunk, 0x17c + 8, 1);
    Put32(chunk, 0x17c + 0xc, 2);
    Put32(chunk, 0x17c + 0xc + 8, 1000);
    Put32(chunk, 0x17c + 0xc + 0x10, 0x7a1f0f1d);

    std::vector<uint8_t> container;
    Put32(container, 0, 0x34a15);
    Put32(container, 4, uint32_t(chunk.size()));
    container.insert(container.end(), chunk.begin(), chunk.end());
    // An unrelated trailing chunk must be skipped.
    const size_t tail = container.size();
    Put32(container, tail, 0x34a1c);
    Put32(container, tail + 4, 0);

    PerfPackages pk;
    Check(pk.Parse(container.data(), container.size()), "parse");
    Check(pk.Count() == 2, "two packages");
    const PerfPackage *a = pk.Find(0, 1);
    Check(a && a->parts.size() == 2, "package (0,1) has two parts");
    Check(a && a->TotalPrice() == 750, "total price 250+500");
    Check(a && a->parts[0].stat[0] == 0.2f && a->parts[0].stat[1] == -1.0f, "stat floats");
    const PerfPackage *b = pk.Find(3, 2);
    Check(b && b->parts.size() == 1 && b->parts[0].tags[0] == 0x7a1f0f1d && b->parts[0].tags[1] == 0,
          "tags");
    Check(pk.FindByPart(2) == b, "find by part id");
    Check(!pk.Find(9, 9) && !pk.FindByPart(99), "misses");
    if (!g_fail)
        printf("perfpackages_test: ok\n");
    return g_fail ? 1 : 0;
}
