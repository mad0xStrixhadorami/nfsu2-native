// Race route lane selection: Paths files contain both carriageways and neighbouring events' roads.
// 0x00345418 links the 0x34149 segments at their endpoint lists; the race record's length picks the
// connected loop. This synthetic file has a direct 400 m loop and a 583 m detour sharing the same
// start/end, so the two lengths must select different branches.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/indep/World/RaceRoute.h"

namespace {

void Put32(std::vector<uint8_t> &v, size_t at, uint32_t x) { memcpy(v.data() + at, &x, 4); }
void Put16(std::vector<uint8_t> &v, size_t at, uint16_t x) { memcpy(v.data() + at, &x, 2); }
void PutF(std::vector<uint8_t> &v, size_t at, float x) { memcpy(v.data() + at, &x, 4); }

struct Pt {
    float x, y;
};

struct Seg {
    const char *name;
    uint32_t order;
    std::vector<Pt> pts;
    std::vector<uint16_t> front, back;
};

std::vector<uint8_t> Build()
{
    // S=(0,0) A=(100,0) B=(200,0) D=(150,50). Each segment owns a contiguous node range (the
    // road network stores per-segment point runs, so endpoints are repeated between segments).
    const std::vector<Seg> segs = {
        {"DIRECT_A", 0, {{0, 0}, {100, 0}}, {2}, {1, 3}},
        {"SHORT", 1, {{100, 0}, {200, 0}}, {0}, {2}},
        {"RETURN", 2, {{200, 0}, {0, 0}}, {1, 3}, {0}},
        {"DETOUR", 3, {{100, 0}, {150, 50}, {200, 0}}, {0}, {2}},
    };

    std::vector<uint8_t> nodeBody;
    std::vector<uint16_t> firstNode(segs.size()), lastNode(segs.size());
    for (size_t k = 0; k < segs.size(); ++k) {
        firstNode[k] = uint16_t(nodeBody.size() / 24);
        for (const Pt &p : segs[k].pts) {
            size_t at = nodeBody.size();
            nodeBody.resize(at + 24, 0);
            PutF(nodeBody, at + 0, p.x);
            PutF(nodeBody, at + 4, p.y);
        }
        lastNode[k] = uint16_t(nodeBody.size() / 24);
    }
    std::vector<uint8_t> segBody(segs.size() * 220, 0);
    for (size_t k = 0; k < segs.size(); ++k) {
        uint8_t *s = segBody.data() + k * 220;
        memcpy(s, segs[k].name, std::min<size_t>(15, strlen(segs[k].name)));
        Put32(segBody, k * 220 + 0x14, segs[k].order);
        Put16(segBody, k * 220 + 0x1c, firstNode[k]);
        Put16(segBody, k * 220 + 0x1e, lastNode[k]);
        PutF(segBody, k * 220 + 0x64, 1.0f);
        s[0x22] = uint8_t(segs[k].front.size());
        s[0x23] = uint8_t(segs[k].back.size());
        for (size_t i = 0; i < segs[k].front.size(); ++i)
            Put16(segBody, k * 220 + 0x24 + i * 8, segs[k].front[i]);
        for (size_t i = 0; i < segs[k].back.size(); ++i)
            Put16(segBody, k * 220 + 0x44 + i * 8, segs[k].back[i]);
    }

    std::vector<uint8_t> out(8, 0);
    Put32(out, 0, 0x80034147);
    auto chunk = [&](uint32_t id, const std::vector<uint8_t> &body) {
        size_t at = out.size();
        out.resize(at + 8 + body.size());
        Put32(out, at, id);
        Put32(out, at + 4, uint32_t(body.size()));
        memcpy(out.data() + at + 8, body.data(), body.size());
    };
    chunk(0x34148, nodeBody);
    chunk(0x34149, segBody);
    Put32(out, 4, uint32_t(out.size() - 8));
    return out;
}

int Check(bool ok, const char *what)
{
    if (!ok)
        fprintf(stderr, "FAIL: %s\n", what);
    return ok ? 0 : 1;
}

} // namespace

int main()
{
    const std::vector<uint8_t> data = Build();
    int failures = 0;

    RaceRoute direct;
    failures += Check(direct.Load(data, 400.0f), "direct load");
    failures += Check(direct.Closed(), "direct closed");
    failures += Check(std::fabs(direct.Length() - 400.0f) < 1.0f, "direct length 400 m");

    RaceRoute detour;
    failures += Check(detour.Load(data, 441.4f), "detour load");
    failures += Check(detour.Closed(), "detour closed");
    failures += Check(std::fabs(detour.Length() - 441.42f) < 1.0f, "detour length 441 m");

    // The direct branch must not contain the detour's apex.
    float x = 0, y = 0, dx = 0, dy = 0;
    bool apex = false;
    for (float s = 0; s <= direct.Length(); s += 5.0f) {
        direct.Sample(s, x, y, dx, dy);
        if (y > 10.0f)
            apex = true;
    }
    failures += Check(!apex, "direct branch has no detour apex");

    // Reversing keeps the lap length and closedness (race 4001 reverse route).
    direct.Reverse();
    failures += Check(direct.Closed(), "reversed closed");
    failures += Check(std::fabs(direct.Length() - 400.0f) < 1.0f, "reversed length 400 m");
    failures += Check(std::fabs(direct.Delta(0.0f, direct.Length() - 1.0f) + 1.0f) < 0.01f,
                      "reversed delta wraps");

    if (!failures)
        printf("route_test: all checks passed\n");
    return failures ? 1 : 0;
}
