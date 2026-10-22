// Tune::Session test (ISSUES #214): a synthetic car and slider table, checking the range / value
// formulas of the ported engine (0x003201f0, 0x00320670, 0x00321118, 0x0031fe88, 0x0031f658).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/indep/Frontend/TuneSystem.h"

namespace {

int g_fail = 0;
void Check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_fail;
    }
}
bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

void PutF(std::vector<uint8_t> &b, size_t off, float v) { memcpy(&b[off], &v, 4); }
float GetF(const std::vector<uint8_t> &b, size_t off)
{
    float v;
    memcpy(&v, &b[off], 4);
    return v;
}

} // namespace

int main()
{
    // The record: mem offset m is at file offset m + 8.  Field 0 (front springs) bounds come from the
    // stock value 30 (0x284), the rear 40 (0x2a4) and the upgrade values 60 / 70 (0x6d4 / 0x6f4).
    std::vector<uint8_t> record(0x890, 0);
    PutF(record, 0x284 + 8, 30.0f);
    PutF(record, 0x2a4 + 8, 40.0f);
    PutF(record, 0x6d4 + 8, 60.0f);
    PutF(record, 0x6f4 + 8, 70.0f);
    // The working frame: field 0 lives at frame +0x174 (= mem 0x284); the part-tuned value is 50.
    std::vector<uint8_t> frame(0x2b0, 0);
    PutF(frame, 0x174, 50.0f);

    Tune::Table table;
    Tune::Entry e;
    e.part = 29, e.level = 1, e.field = 0, e.mode = 2;
    e.min = -0.2f, e.max = 0.1f, e.uiLo = 0.0f, e.uiHi = 10.0f;
    table.Add(e);
    Tune::Entry low = e; // a lower level of the same field must lose to the higher one
    low.part = 31, low.level = 2, low.min = -0.3f;
    table.Add(low);

    Check(table.Active(0, {29}) != nullptr && table.Active(0, {29})->level == 1, "active entry");
    Check(table.Active(0, {29, 31}) != nullptr && table.Active(0, {29, 31})->level == 2, "highest level wins");
    Check(table.Active(0, {}) == nullptr && table.Active(1, {29}) == nullptr, "no entry");

    Tune::Session session(table, record.data(), frame, {29});
    Check(session.FieldActive(0) && !session.FieldActive(4) && !session.FieldActive(10), "active fields");

    float lo, hi;
    // Bounds: min/max of {30,40,60,70} = [30,70], then mode 2 widens by a fraction of the span.
    Check(session.Bounds(0, 0, true, lo, hi) && Near(lo, 22.0f) && Near(hi, 74.0f), "bounds adjusted by the entry");
    // Slider range around the reference 50: [22, min(74, 50 + 0.1 * 52)].
    Check(session.SliderRange(0, 0, lo, hi) && Near(lo, 22.0f) && Near(hi, 55.2f), "slider range");
    Check(session.DisplayRange(0, 0, lo, hi) && Near(lo, 0.0f) && Near(hi, 10.0f), "display range from the table");
    Check(Near(session.Display(0, 0), (50.0f - 22.0f) / 52.0f * 10.0f), "neutral shows the reference");
    Check(session.Step(0) > 0.0f && session.Step(0) < 10.0f / 30.0f + 1e-3f, "step is at most a thirtieth of the display span");

    // Setting the display to 10 (the physical 74) clamps to the slider's top and stores a byte.
    session.SetDisplay(0, 0, 10.0f);
    Check(session.Byte(0, 0) > 100 && session.Byte(0, 0) <= 200, "byte moved up");
    session.Apply();
    Check(Near(GetF(session.Frame(), 0x174), 55.2f, 0.05f), "apply writes the clamped physical value");
    // Reset restores neutral and the part-tuned value.
    session.Reset(0, 0);
    session.Apply();
    Check(session.Byte(0, 0) == 100 && Near(GetF(session.Frame(), 0x174), 50.0f), "reset");
    // Position bytes: a low display value moves the byte below 100 and the value down.
    session.SetDisplay(0, 0, 0.0f);
    Check(session.Byte(0, 0) < 100, "byte moved down");
    session.Apply();
    Check(GetF(session.Frame(), 0x174) < 50.0f && GetF(session.Frame(), 0x174) >= 22.0f - 1e-3f, "value moved down within bounds");

    // Saved bytes round-trip.
    std::vector<uint8_t> bytes = session.Setting();
    Tune::Session other(table, record.data(), frame, {29});
    other.LoadSetting(bytes);
    other.Apply();
    Check(Near(GetF(other.Frame(), 0x174), GetF(session.Frame(), 0x174)), "setting round-trip");

    if (!g_fail)
        printf("tunesystem_test: ok\n");
    return g_fail ? 1 : 0;
}
