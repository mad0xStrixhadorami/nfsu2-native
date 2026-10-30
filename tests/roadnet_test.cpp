// RoadNet parser: a synthetic chunk 0x34121 of two sections (header 0x8c bytes + count * 0x38 waypoints), ISSUES #388.
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/indep/World/RoadNet.h"

static void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) { memcpy(&b[at], &v, 4); }
static void putf(std::vector<uint8_t> &b, size_t at, float v) { memcpy(&b[at], &v, 4); }

int main()
{
    std::vector<uint8_t> d(8, 0);
    put32(d, 0, 0x34121);
    auto section = [&](uint16_t id, int count, float x0, float y0, float dx) {
        const size_t at = d.size();
        d.resize(at + 0x8c + size_t(count) * 0x38, 0);
        memcpy(&d[at + 10], &id, 2);
        int16_t c = int16_t(count);
        memcpy(&d[at + 0x34], &c, 2);
        putf(d, at + 0x40, dx * float(count - 1));
        for (int k = 0; k < count; ++k) {
            putf(d, at + 0x8c + size_t(k) * 0x38, x0 + dx * float(k));
            putf(d, at + 0x8c + size_t(k) * 0x38 + 4, y0);
        }
    };
    section(101, 4, 0.0f, 0.0f, 10.0f);  // runs to (30, 0)
    section(102, 3, 40.0f, 0.0f, 10.0f); // starts 10 m after it
    section(103, 2, 500.0f, 0.0f, 10.0f); // far away
    put32(d, 4, uint32_t(d.size() - 8));
    RoadNet net;
    int fail = 0;
    auto check = [&](bool ok, const char *what) { printf("  %-50s %s\n", what, ok ? "ok" : "FAIL"), fail += !ok; };
    check(net.Load(d) && net.Sections().size() == 3, "three sections parsed");
    check(net.Sections()[0].id == 101 && net.Sections()[0].x.size() == 4 && net.Sections()[0].x[3] == 30.0f, "ids, counts and positions");
    check(net.Sections()[0].next.size() == 1 && net.Sections()[0].next[0] == 1, "section 101 continues into 102");
    check(net.Sections()[2].next.empty(), "the far section has no successor");
    std::vector<uint8_t> bad = d;
    put32(bad, 0, 0x34122);
    check(!net.Load(bad), "another chunk id is refused");
    printf("roadnet_test: %s\n", fail ? "FAILED" : "passed");
    return fail ? 1 : 0;
}
