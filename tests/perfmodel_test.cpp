// PerfModel test (ISSUES #200): synthetic car record + package table, checking the formulas the
// modifiers of 0x0031b520 apply.  Frame offsets are relative to record + 0x118.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/indep/Frontend/PerfModel.h"

namespace {

int g_fail = 0;
void Check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_fail;
    }
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

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
float GetF(const std::vector<uint8_t> &b, size_t off)
{
    float v;
    memcpy(&v, &b[off], 4);
    return v;
}
size_t Frame(size_t off) { return PerfModel::kFrameOffset + off; }

// A package table: (category, level, parts{id, stat0, stat1}).
struct P {
    uint32_t cat, level;
    std::vector<std::vector<float>> parts; // {id, stat0, stat1}
};
PerfPackages MakePackages(const std::vector<P> &list)
{
    std::vector<uint8_t> chunk(list.size() * 0x17c, 0);
    for (size_t i = 0; i < list.size(); ++i) {
        const size_t r = i * 0x17c;
        Put32(chunk, r, list[i].cat);
        Put32(chunk, r + 4, list[i].level);
        Put32(chunk, r + 8, uint32_t(list[i].parts.size()));
        for (size_t k = 0; k < list[i].parts.size(); ++k) {
            const size_t p = r + 0xc + k * 0x5c;
            Put32(chunk, p, uint32_t(list[i].parts[k][0]));
            PutF(chunk, p + 0x30, list[i].parts[k][1]);
            PutF(chunk, p + 0x34, list[i].parts[k].size() > 2 ? list[i].parts[k][2] : -1.0f);
            PutF(chunk, p + 0x38, -1.0f);
            PutF(chunk, p + 0x3c, -1.0f);
        }
    }
    std::vector<uint8_t> container;
    Put32(container, 0, 0x34a15);
    Put32(container, 4, uint32_t(chunk.size()));
    container.insert(container.end(), chunk.begin(), chunk.end());
    PerfPackages pk;
    pk.Parse(container.data(), container.size());
    return pk;
}

} // namespace

int main()
{
    std::vector<uint8_t> rec(PerfModel::kRecordSize, 0);
    PutF(rec, Frame(0x110), 1.5f);   // mass
    PutF(rec, Frame(0x340), 0.9f);   // category 0: mass factor at full strength
    PutF(rec, Frame(0x200), 0.2f);   // torque curve point 0
    PutF(rec, Frame(0x224), 0.05f);  // the stock extra curve (+0x33c)
    PutF(rec, Frame(0x420), 0.1f);   // category 3 curve delta, point 0

    const PerfPackages pk = MakePackages({{0, 1, {{7, 0.5f}}},
                                          {3, 1, {{8, 0.3f}, {9, 0.4f}}},
                                          {3, 2, {{10, 0.6f}}}});

    // Strength: sums the installed parts' stats; clamps when the mean leaves 0..1.
    float out[3];
    Check(Near(PerfModel::Strength(pk, {7}, 0, 1, out), 0.5f) && Near(out[0], 0.5f), "single part strength");
    Check(Near(PerfModel::Strength(pk, {8, 9}, 3, 1, out), 0.7f), "two parts add");
    Check(Near(PerfModel::Strength(pk, {8, 9, 10}, 3, 1, out), 1.0f) && Near(out[0], 1.0f), "clamped to 1");
    Check(PerfModel::Strength(pk, {}, 3, 1, out) == 0.0f, "nothing installed");

    // A stock car still gains the extra curve (0x0031be80 runs unconditionally).
    {
        std::vector<uint8_t> r = rec;
        PerfModel::Apply(r.data(), r.size(), PerfPackages(), {});
        Check(Near(GetF(r, Frame(0x200)), 0.25f), "stock curve gains the extra curve");
        Check(Near(GetF(r, Frame(0x110)), 1.5f), "stock mass unchanged");
    }
    // Weight reduction scales the mass by (factor - 1) * strength + 1.
    {
        std::vector<uint8_t> r = rec;
        PerfModel::Apply(r.data(), r.size(), pk, {7});
        Check(Near(GetF(r, Frame(0x110)), 1.5f * 0.95f), "weight reduction mass");
    }
    // Category 3 adds the delta table scaled by strength to the curve.
    {
        std::vector<uint8_t> r = rec;
        PerfModel::Apply(r.data(), r.size(), pk, {8, 9});
        Check(Near(GetF(r, Frame(0x200)), 0.25f + 0.1f * 0.7f), "torque curve delta");
    }
    // The input record is the base: applying twice from the base is not cumulative.
    {
        std::vector<uint8_t> r = rec, r2 = rec;
        PerfModel::Apply(r.data(), r.size(), pk, {8});
        PerfModel::Apply(r2.data(), r2.size(), pk, {8});
        Check(GetF(r, Frame(0x200)) == GetF(r2, Frame(0x200)), "deterministic");
    }
    if (!g_fail)
        printf("perfmodel_test: ok\n");
    return g_fail ? 1 : 0;
}
