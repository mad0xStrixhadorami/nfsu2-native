#include "game/indep/Frontend/TuneSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Tune {

namespace {

constexpr uint32_t kTuneChunk = 0x34a1a;
constexpr size_t kEntrySize = 24;
constexpr float kNoUi = -99.99f;       // DAT_005637b4 / b8: "no display range in the table"
constexpr float kMinSpan = 0.01f;      // DAT_005637bc / c0
constexpr float kBig = 100000.0f;      // DAT_00568c18 / 1c: the initial {min, max} = {+100000, -100000}
constexpr float kBigBase = 10000000.0f; // DAT_005637c4 / c8

uint32_t U32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
float F32(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}
float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A field's reference partner for 0x00320500: the mirrored axle fields.
void Pair(int field, int &a, int &b)
{
    switch (field) {
    case 0:
    case 4:
        a = 0, b = 4;
        return;
    case 2:
    case 6:
        a = 2, b = 6;
        return;
    case 3:
    case 7:
        a = 3, b = 7;
        return;
    case 0x15:
    case 0x16:
        a = 0x15, b = 0x16;
        return;
    default:
        a = field, b = -1;
    }
}

// 0x00321cb8 (the part that matters for the supported fields): the lowering is shown in cm or
// inches (metres in the data).
float UnitScale(int field, bool metric, bool toDisplay)
{
    if (field == 0x17 || field == 0x18) // 0x00321cb8: kN*m -> N*m (x1000) or ft-lbs (x737.56)
        return metric ? (toDisplay ? 1000.0f : 0.001f) : (toDisplay ? 737.5621f : 1.0f / 737.5621f);
    if (field != 3 && field != 7)
        return 1.0f;
    if (metric)
        return toDisplay ? 100.0f : 0.01f;
    return toDisplay ? 39.37f : 0.0254f;
}

} // namespace

bool Table::Parse(const uint8_t *container, size_t size)
{
    m_entries.clear();
    for (size_t off = 0; off + 8 <= size;) {
        const uint32_t id = U32(container + off);
        const size_t len = U32(container + off + 4);
        if (off + 8 + len > size)
            break;
        if (id == kTuneChunk) {
            const uint8_t *data = container + off + 8;
            for (size_t r = 0; r + kEntrySize <= len; r += kEntrySize) {
                Entry e;
                e.part = U32(data + r);
                e.level = data[r + 4];
                e.field = data[r + 5];
                e.mode = data[r + 6];
                e.min = F32(data + r + 8);
                e.max = F32(data + r + 12);
                e.uiLo = F32(data + r + 16);
                e.uiHi = F32(data + r + 20);
                m_entries.push_back(e);
            }
        }
        off += 8 + len;
    }
    return !m_entries.empty();
}

const Entry *Table::Active(int field, const std::vector<uint16_t> &installed) const
{
    const Entry *best = nullptr;
    for (const Entry &e : m_entries) {
        if (e.field != field)
            continue;
        if (std::find(installed.begin(), installed.end(), uint16_t(e.part)) == installed.end())
            continue;
        if (!best || best->level < e.level)
            best = &e;
    }
    return best;
}

Session::Session(const Table &table, const uint8_t *record, const std::vector<uint8_t> &workFrame,
                 const std::vector<uint16_t> &installed)
    : m_table(table), m_record(record, record + 0x890), m_work(workFrame), m_installed(installed),
      m_setting(kSettingSize, 100), m_ref(kSettingSize, 0.0f)
{
    m_work.resize(0x2b0, 0);
    m_part = m_work; // the part-tuned frame every Apply starts from
    CaptureRefs();
}

bool Session::Supported(int field)
{
    switch (field) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
    case 10: case 11: case 12: case 13: case 14: case 15:
    case 0x10: case 0x11:
    case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
        return true;
    default:
        return false;
    }
}

bool Session::FieldActive(int field) const
{
    if (!Supported(field) || m_table.Active(field, m_installed) == nullptr)
        return false;
    if (field >= 10 && field <= 15) { // 0x0031faf8: only the gears the car has
        int count;
        memcpy(&count, m_part.data() + 0x1c8, 4);
        return field - 8 < count + 2;
    }
    return true;
}

float Session::RecM(size_t mem) const
{
    return F32(m_record.data() + mem + 8);
}

float *Session::WorkPtr(int field, int sub)
{
    size_t off = 0;
    switch (field) {
    case 3: off = 0x18 + size_t(sub) * 0x30; break;
    case 7: off = 0x78 + size_t(sub) * 0x30; break;
    case 0: off = 0x174; break;
    case 1: off = 0x178 + size_t(sub) * 4; break;
    case 2: off = 0x180; break;
    case 4: off = 0x194; break;
    case 5: off = 0x198 + size_t(sub) * 4; break;
    case 6: off = 0x1a0; break;
    case 8: off = 0x270; break;
    case 9: off = 0x1b8 + size_t(sub) * 4; break;
    case 10: case 11: case 12: case 13: case 14: case 15: off = 0x1b0 + size_t(field) * 4; break;
    case 0x17: off = 0x200 + size_t(sub) * 4; break;
    case 0x18: off = 0x224 + size_t(sub) * 4; break;
    case 0x10: off = 0x2a0; break;
    case 0x11: off = 0x2ac; break;
    case 0x12: off = 0x27c; break;
    case 0x13: off = sub == 0 ? 0xdc : 0xe0; break;
    case 0x14: off = sub == 0 ? 0xfc : 0x100; break;
    case 0x15: off = 0x264; break;
    case 0x16: off = 0x268; break;
    default: return nullptr;
    }
    return reinterpret_cast<float *>(m_work.data() + off);
}

const float *Session::WorkPtr(int field, int sub) const
{
    return const_cast<Session *>(this)->WorkPtr(field, sub);
}

// 0x00320e20: the working copy's value (the mean of the two axle entries for the mirrored fields).
float Session::ReadValue(int field, int sub) const
{
    switch (field) {
    case 1: case 5: case 0x13: case 0x14:
        return (*WorkPtr(field, 0) + *WorkPtr(field, 1)) * 0.5f;
    case 0x17: // 0x00320e20: the engine curve without the extra (turbo) curve
        return *WorkPtr(0x17, sub) - *WorkPtr(0x18, sub);
    case 3: case 7: {
        // 0x00320e20: the stock wheel height minus the working copy's (the lowering).
        const size_t off = field == 3 ? 0x18 : 0x78;
        return F32(m_record.data() + 0x118 + off + 0 * 0x30) - *WorkPtr(field, 0);
    }
    default:
        return *WorkPtr(field, sub);
    }
}

void Session::CaptureRefs()
{
    std::fill(m_ref.begin(), m_ref.end(), 0.0f);
    for (int f = 0; f < 0x17; ++f)
        if (FieldActive(f))
            m_ref[size_t(f)] = ReadValue(f, 0);
    for (int f = 0x17; f <= 0x18; ++f)
        if (FieldActive(f))
            for (int i = 0; i < 9; ++i)
                m_ref[Index(f, i)] = ReadValue(f, i);
}

// 0x00321118: the active table entry moves the bounds (mode 0/1 absolute, 2/3 fraction of the span,
// 4 clamp).
void Session::AdjustByEntry(int field, float &lo, float &hi) const
{
    const Entry *e = m_table.Active(field, m_installed);
    if (!e)
        return;
    const float span = hi - lo;
    if (e->mode == 2 || e->mode == 3) {
        const float nlo = lo + e->min * span;
        hi = hi + e->max * span;
        lo = nlo;
    } else if (e->mode < 2) {
        lo = lo + e->min;
        hi = hi + e->max;
    } else if (e->mode == 4) {
        lo = std::max(lo, e->min);
        hi = std::max(e->max, hi);
    }
}

// 0x00320670 (and its axle helpers 0x00320c40 / 0x00320a88).
bool Session::Bounds(int field, int sub, bool average, float &lo, float &hi) const
{
    float l = kBig, h = -kBig;
    auto take = [&](float v) {
        l = std::min(l, v);
        h = std::max(h, v);
    };
    switch (field) {
    case 0: case 4:
        for (size_t m : {0x284u, 0x2a4u, 0x6d4u, 0x6f4u})
            take(RecM(m));
        break;
    case 2: case 6:
        for (size_t m : {0x290u, 0x2b0u, 0x6e0u, 0x700u})
            take(RecM(m));
        break;
    case 8:
        take(RecM(0x380));
        take(RecM(0x6cc));
        break;
    case 9:
        take(RecM(0x2c8));
        take(RecM(0x4e8));
        break;
    case 0x15: case 0x16:
        take(RecM(0x374));
        take(RecM(0x374) + RecM(0x6b0) + RecM(0x6a0));
        take(RecM(0x378));
        take(RecM(0x378) + RecM(0x6b4) + RecM(0x6a4));
        break;
    case 0x12:
        l = 0.0f;
        h = 1.0f;
        break;
    case 0x17: case 0x18:
        // 0x00320670: over all nine points, the curve value and the value with the upgrade deltas
        // (field 0x17: stock + the 0x530 and 0x5f0 tables; field 0x18: the extra curve and the
        // 0x620 table), then moved by the entry and kept non-negative.
        for (size_t i = 0; i < 9; ++i) {
            if (field == 0x17) {
                const float a = RecM(0x310 + i * 4);
                take(a);
                take(a + RecM(0x530 + i * 4) + RecM(0x5f0 + i * 4));
            } else {
                take(RecM(0x334 + i * 4));
                take(RecM(0x620 + i * 4));
            }
        }
        AdjustByEntry(field, l, h);
        lo = l >= 0.0f ? l : 0.0f;
        hi = h >= 0.0f ? h : 0.0f;
        return true;
    case 0x10: // the nitrous depletion rate: 1 .. 11, no table adjust
        lo = 1.0f;
        hi = 11.0f;
        return true;
    case 0x11: // the nitrous torque mix: 0.1 .. 0.9, adjusted by the entry
        l = 0.1f;
        h = 0.9f;
        break;
    case 10: case 11: case 12: case 13: case 14: case 15:
        // 0x00320670: the ratios for 250 mph and 2.5 mph (111.7568 / 1.1175681 m/s); no table adjust.
        lo = RatioToSpeed(111.7568f);
        hi = RatioToSpeed(1.1175681f);
        return true;
    case 3: case 7: {
        // The pair of lowering limits at 0x714 / 0x718, negated, always including zero.
        const float a = -RecM(0x714), b = -RecM(0x718);
        l = std::min(std::min(a, b), 0.0f);
        h = std::max(std::max(a, b), 0.0f);
        break;
    }
    case 1: case 5: {
        // 0x00320c40: per axle {stock, upgrade} pairs for field 1 at 0x288 / 0x6d8 and for field 5
        // at 0x2a8 / 0x6f8, each moved by its own table entry.
        float e[8];
        for (int i = 0; i < 2; ++i) {
            e[i] = RecM(0x288 + size_t(i) * 4);
            e[4 + i] = RecM(0x6d8 + size_t(i) * 4);
            AdjustByEntry(1, e[i], e[4 + i]);
            e[2 + i] = RecM(0x2a8 + size_t(i) * 4);
            e[6 + i] = RecM(0x6f8 + size_t(i) * 4);
            AdjustByEntry(5, e[2 + i], e[6 + i]);
        }
        const int u = field == 1 ? 0 : 1;
        if (!average) {
            lo = e[u * 2 + sub];
            hi = e[4 + u * 2 + sub];
        } else {
            lo = (e[u * 2] + e[u * 2 + 1]) * 0.5f;
            hi = (e[4 + u * 2] + e[4 + u * 2 + 1]) * 0.5f;
        }
        return true;
    }
    case 0x13: case 0x14: {
        // 0x00320a88: tyre pairs at 0x1ec / 0x65c, 0x1f0 / 0x660 (field 0x13) and 0x20c / 0x67c,
        // 0x210 / 0x680 (field 0x14).
        float d[8];
        d[0] = RecM(0x1ec); d[4] = RecM(0x65c);
        AdjustByEntry(0x13, d[0], d[4]);
        d[1] = RecM(0x1f0); d[5] = RecM(0x660);
        AdjustByEntry(0x13, d[1], d[5]);
        d[2] = RecM(0x20c); d[6] = RecM(0x67c);
        AdjustByEntry(0x14, d[2], d[6]);
        d[3] = RecM(0x210); d[7] = RecM(0x680);
        AdjustByEntry(0x14, d[3], d[7]);
        const int u = field != 0x13;
        if (!average) {
            lo = d[u * 2 + sub];
            hi = d[4 + u * 2 + sub];
        } else {
            lo = (d[u * 2] + d[u * 2 + 1]) * 0.5f;
            hi = (d[4 + u * 2] + d[4 + u * 2 + 1]) * 0.5f;
        }
        return true;
    }
    default:
        return false;
    }
    AdjustByEntry(field, l, h);
    lo = l;
    hi = h;
    return true;
}

// 0x00320500: the reference a slider range is built around - the larger of a mirrored pair's
// references (the smaller for the sign-flipped fields).
float Session::Base(int field, int sub) const
{
    int a, b;
    Pair(field, a, b);
    const float sign = Sign(field);
    float v = sign <= 0.0f ? kBigBase : -kBigBase;
    for (int f : {a, b}) {
        if (f == -1)
            continue;
        const float r = m_ref[Index(f, f >= 0x17 ? sub : 0)];
        v = sign <= 0.0f ? std::min(v, r) : std::max(v, r);
    }
    return v;
}

// 0x003201f0 (physical, no display conversion).
bool Session::SliderRange(int field, int sub, float &lo, float &hi) const
{
    const Entry *e = m_table.Active(field, m_installed);
    if (!e)
        return false;
    float b0, b1;
    if (!Bounds(field, sub, true, b0, b1))
        return false;
    const float base = Base(field, sub);
    const float span = b1 - b0;
    switch (e->mode) {
    case 2:
        if (Sign(field) > 0.0f) {
            lo = b0;
            hi = std::min(b1, base + e->max * span);
        } else {
            lo = std::max(b0, base - e->max * span);
            hi = b1;
        }
        break;
    case 3:
        lo = std::max(b0, base + e->min * span);
        hi = std::min(b1, base + e->max * span);
        break;
    case 0:
        if (Sign(field) > 0.0f) {
            lo = b0;
            hi = std::min(b1, base + e->max);
        } else {
            lo = std::max(b0, base - e->max);
            hi = b1;
        }
        break;
    case 1:
        lo = std::max(b0, base + e->min);
        hi = std::min(b1, base + e->max);
        break;
    case 4:
        lo = e->min;
        hi = e->max;
        break;
    default:
        lo = b0;
        hi = b1;
    }
    if (lo == hi)
        hi = lo + kMinSpan;
    return true;
}

// 0x00320030: flag 1 gives the display range (the table's when it has one).
bool Session::DisplayRange(int field, int sub, float &lo, float &hi) const
{
    const Entry *e = m_table.Active(field, m_installed);
    if (!e)
        return false;
    if (!Bounds(field, sub, true, lo, hi))
        return false;
    if (field >= 10 && field <= 15) { // the speeds of the bounds' ratios
        const float a = RatioToSpeed(lo) * SpeedScale(), b = RatioToSpeed(hi) * SpeedScale();
        lo = std::min(a, b);
        hi = std::max(a, b);
        return true;
    }
    lo *= UnitScale(field, m_metric, true);
    hi *= UnitScale(field, m_metric, true);
    if (e->uiLo != kNoUi)
        lo = e->uiLo;
    if (e->uiHi != kNoUi)
        hi = e->uiHi;
    if (hi < lo)
        std::swap(lo, hi);
    if (lo == hi)
        hi = lo + kMinSpan;
    return true;
}

// 0x00321de8: ratio <-> speed at the redline for the car's drive split and final drive.
float Session::RatioToSpeed(float x) const
{
    auto work = [&](size_t off) { return F32(m_work.data() + off); };
    const float split = work(0x1c0);
    float radius = 0.0f;
    if (0.2f <= split) {
        radius = split <= 0.8f ? split * work(0x80) + (1.0f - split) * work(0x50) : work(0x80);
    } else {
        radius = work(0x50);
    }
    const float omega = work(0x1f4) * 0.10471968f;
    if (x == 0.0f)
        return 0.0f;
    return (omega / work(0x1b8)) * (radius / x);
}

// 0x003218e0: set a gear ratio inside its slider range and push the neighbours so the ratios stay
// at least 0.05 apart; returns the part of the move that could not be done.
float Session::Reorder(float value, int field, float arr[8])
{
    float lo, hi;
    SliderRange(field, 0, lo, hi);
    float clamped = std::max(value, lo);
    clamped = std::min(clamped, hi);
    float delta = value - arr[field - 8];
    if (delta == 0.0f)
        return 0.0f;
    int dir = 0;
    if (delta <= 0.0f || field < 0xb) {
        if (delta < 0.0f)
            dir = field < 0xf ? 1 : 0;
    } else {
        dir = -1;
    }
    float excess = value - clamped;
    if (dir != 0) {
        const int nf = field + dir;
        float want = clamped - 0.05f;
        const float neighbour = arr[nf - 8];
        if (neighbour <= want || nf <= field) {
            const float above = clamped + 0.05f;
            want = neighbour;
            if (neighbour < above && nf < field)
                want = std::max(neighbour, above);
        } else {
            want = std::min(neighbour, want);
        }
        if (want == neighbour) {
            arr[field - 8] = value - excess;
            return excess;
        }
        excess += Reorder(want, nf, arr);
    }
    arr[field - 8] = value - excess;
    return excess;
}

float Session::ToDisplay(float physical, int field) const
{
    if (field >= 10 && field <= 15) // the gears show the speed at the redline
        return RatioToSpeed(physical) * SpeedScale();

    float r0, r1, r2, r3;
    if (!Bounds(field, 0, false, r0, r1) || !DisplayRange(field, 0, r2, r3))
        return physical;
    if (r0 == r1)
        r1 = r0 + kMinSpan;
    const float c = Clamp(physical, std::min(r0, r1), std::max(r0, r1));
    return r2 + (c - r0) / (r1 - r0) * (r3 - r2);
}

bool Session::SliderRangeDisplay(int field, int sub, float &lo, float &hi) const
{
    float pl, ph;
    if (!SliderRange(field, sub, pl, ph))
        return false;
    lo = ToDisplay(pl, field);
    hi = ToDisplay(ph, field);
    if (hi < lo)
        std::swap(lo, hi);
    if (lo == hi)
        hi = lo + kMinSpan;
    return true;
}

float Session::Step(int field) const
{
    float lo, hi;
    return SliderRangeDisplay(field, 0, lo, hi) ? (hi - lo) / 30.0f : 0.0f;
}

float Session::ToPhysical(float display, int field) const
{
    if (field >= 10 && field <= 15) {
        float lo, hi;
        Bounds(field, 0, false, lo, hi);
        return Clamp(RatioToSpeed(display / SpeedScale()), std::min(lo, hi), std::max(lo, hi));
    }
    float r0, r1, r2, r3;
    if (!Bounds(field, 0, false, r0, r1) || !DisplayRange(field, 0, r2, r3))
        return display;
    if (r0 == r1)
        r1 = r0 + kMinSpan;
    const float c = Clamp(display, std::min(r2, r3), std::max(r2, r3));
    return r0 + (c - r2) / (r3 - r2) * (r1 - r0);
}

// 0x003211f8: the physical value goes into the working copy.
void Session::WritePhysical(int field, int sub, float value)
{
    switch (field) {
    case 1: case 5: case 0x13: case 0x14: {
        float al, ah;
        Bounds(field, 0, true, al, ah);
        if (ah == al)
            ah = al + kMinSpan;
        const float v = Clamp(value, std::min(al, ah), std::max(al, ah));
        for (int i = 0; i < 2; ++i) {
            float lo, hi;
            Bounds(field, i, false, lo, hi);
            *WorkPtr(field, i) = lo + (v - al) / (ah - al) * (hi - lo);
        }
        break;
    }
    case 9:
        *WorkPtr(9, 0) = value;
        *WorkPtr(9, 1) = value;
        break;
    case 0x17: case 0x18:
        WriteCurve(field, sub, value, false);
        break;
    case 0x10: case 0x11: {
        // 0x00321b70: the nitrous rate and torque are coupled - raising one lowers the other in
        // proportion to their slider spans.
        const int other = field == 0x11 ? 0x10 : 0x11;
        float lo, hi, lo2, hi2;
        SliderRange(field, 0, lo, hi);
        SliderRange(other, 0, lo2, hi2);
        const float newOther = Ref(other, 0) - (value - Ref(field, 0)) / (hi - lo) * (hi2 - lo2);
        *WorkPtr(field, 0) = value;
        *WorkPtr(other, 0) = newOther;
        break;
    }
    case 10: case 11: case 12: case 13: case 14: case 15: {
        // 0x00321a60: reorder the six ratios around the changed one, zero the gears the car lacks.
        float arr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int f = 10; f < 16; ++f)
            arr[f - 8] = *WorkPtr(f, 0);
        Reorder(value, field, arr);
        int count;
        memcpy(&count, m_work.data() + 0x1c8, 4);
        for (int i = count + 2; i < 8; ++i)
            if (i >= 0)
                arr[i] = 0.0f;
        for (int f = 10; f < 16; ++f)
            *WorkPtr(f, 0) = arr[f - 8];
        break;
    }
    case 3: case 7: {
        // 0x003211f8: the two wheels of the axle = stock height minus the lowering.
        const size_t off = field == 3 ? 0x18 : 0x78;
        for (int i = 0; i < 2; ++i)
            *WorkPtr(field, i) = F32(m_record.data() + 0x118 + off + size_t(i) * 0x30) - value;
        break;
    }
    default:
        if (float *p = WorkPtr(field, sub))
            *p = value;
    }
}

// 0x00321548: set one point of a curve field inside its range; with `spread` the change is paid back
// by the neighbours (weights 0x0051dec0: +0.25 for the first two, -0.25 for the next six, then
// whatever is left) so the curve's area stays put.  The main curve always holds value + extra curve.
void Session::WriteCurve(int field, int sub, float value, bool spread)
{
    static const float kWeights[9] = {0.25f, 0.25f, -0.25f, -0.25f, -0.25f, -0.25f, -0.25f, -0.25f, 0.0f};
    float cur[9];
    for (int i = 0; i < 9; ++i)
        cur[i] = ReadValue(field, i);
    const float change = value - cur[sub];
    float lo, hi;
    if (!SliderRange(field, sub, lo, hi))
        return;
    cur[sub] = std::min(hi, std::max(lo, value));
    if (change != 0.0f) {
        float left = change;
        int dir = 0, steps = 0, weight = 0;
        while (std::fabs(left) > 1.0e-7f && steps < 0x14 && spread) {
            float move = -left;
            if (steps < 9) {
                move = change * kWeights[steps];
                move = left <= 0.0f ? std::max(move, left) : std::min(move, left);
            }
            dir = -dir;
            if (dir > -1) {
                ++dir;
                if (dir > 8)
                    dir = 1;
            }
            const int at = sub + dir;
            if (at >= 0 && at < 9) {
                float nlo, nhi;
                SliderRange(field, at, nlo, nhi);
                ++weight;
                const float before = cur[at];
                float after = before + move;
                after = std::min(nhi, std::max(nlo, after));
                cur[at] = after;
                ++steps;
                left += after - before;
            }
        }
    }
    for (int i = 0; i < 9; ++i) {
        if (field == 0x17) {
            *WorkPtr(0x17, i) = cur[i] + *WorkPtr(0x18, i);
        } else {
            const float ecu = ReadValue(0x17, i);
            *WorkPtr(0x18, i) = cur[i];
            *WorkPtr(0x17, i) = ecu + cur[i];
        }
    }
}

float Session::Display(int field, int sub) const
{
    float lo, hi;
    if (!SliderRange(field, sub, lo, hi))
        return 0.0f;
    const float pos = (float(Byte(field, sub)) - 100.0f) * 0.01f; // 0x0031f5e0
    const float physical = Ref(field, sub) + Clamp(pos, -1.0f, 1.0f) * (hi - lo);
    return ToDisplay(physical, field);
}

void Session::SetDisplay(int field, int sub, float display)
{
    float lo, hi;
    if (!SliderRange(field, sub, lo, hi))
        return;
    const float physical = ToPhysical(display, field);
    const float v = Clamp(physical, lo, hi);               // 0x0031fa50
    const float pos = (v - Ref(field, sub)) / (hi - lo);
    const int stored = int(pos * 100.0f + 100.5f);         // 0x0031f658
    Byte(field, sub) = uint8_t(std::max(0, std::min(200, stored)));
    if (field == 0x17 || field == 0x18) {
        WriteCurve(field, sub, v, true);
        // 0x0031f8d8: every point's slider follows the redistributed values
        for (int i = 0; i < 9; ++i) {
            float plo, phi;
            if (!SliderRange(field, i, plo, phi))
                continue;
            const float actual = Clamp(ReadValue(field, i), plo, phi);
            const int b = int(((actual - Ref(field, i)) / (phi - plo)) * 100.0f + 100.5f);
            Byte(field, i) = uint8_t(std::max(0, std::min(200, b)));
        }
    } else {
        WritePhysical(field, sub, v);
    }
    if (field == 0x10 || field == 0x11) { // 0x0031f8d8: the coupled partner's slider follows
        const int other = field == 0x11 ? 0x10 : 0x11;
        float olo, ohi;
        if (FieldActive(other) && SliderRange(other, 0, olo, ohi)) {
            const float actual = Clamp(ReadValue(other, 0), olo, ohi);
            const int b = int(((actual - Ref(other, 0)) / (ohi - olo)) * 100.0f + 100.5f);
            Byte(other, 0) = uint8_t(std::max(0, std::min(200, b)));
        }
    }
    if (field >= 10 && field <= 15) { // 0x0031f8d8: every gear's slider follows the ratios as written
        for (int g = 10; g < 16; ++g) {
            float glo, ghi;
            if (!FieldActive(g) || !SliderRange(g, 0, glo, ghi))
                continue;
            const float actual = Clamp(ReadValue(g, 0), glo, ghi);
            const int b = int(((actual - Ref(g, 0)) / (ghi - glo)) * 100.0f + 100.5f);
            Byte(g, 0) = uint8_t(std::max(0, std::min(200, b)));
        }
    }
}

void Session::Reset(int field, int sub)
{
    Byte(field, sub) = 100;
}

void Session::ResetAll()
{
    std::fill(m_setting.begin(), m_setting.end(), uint8_t(100));
}

void Session::LoadSetting(const std::vector<uint8_t> &bytes)
{
    if (bytes.size() == kSettingSize)
        m_setting = bytes;
}

// 0x0031fbe0: every active field's value = clamp(reference + position * span).
void Session::Apply()
{
    m_work = m_part;
    for (int f = 0; f < 0x17; ++f) {
        if (!FieldActive(f))
            continue;
        float lo, hi;
        if (!SliderRange(f, 0, lo, hi))
            continue;
        const float pos = Clamp((float(Byte(f, 0)) - 100.0f) * 0.01f, -1.0f, 1.0f);
        const float v = Clamp(Ref(f, 0) + pos * (hi - lo), lo, hi);
        WritePhysical(f, 0, v);
    }
    // 0x0031fbe0 third loop: each curve point from its slider (no spreading), then the curve's
    // total is put back to what the parts left (0x00321850).
    float before = 0.0f;
    for (int i = 0; i < 9; ++i)
        before += F32(m_part.data() + 0x200 + size_t(i) * 4);
    bool curves = false;
    for (int f = 0x17; f <= 0x18; ++f) {
        if (!FieldActive(f))
            continue;
        curves = true;
        for (int i = 0; i < 9; ++i) {
            float lo, hi;
            if (!SliderRange(f, i, lo, hi))
                continue;
            const float pos = Clamp((float(Byte(f, i)) - 100.0f) * 0.01f, -1.0f, 1.0f);
            const float v = Clamp(Ref(f, i) + pos * (hi - lo), lo, hi);
            WriteCurve(f, i, v, false);
        }
    }
    if (curves) {
        float after = 0.0f;
        for (int i = 0; i < 9; ++i)
            after += F32(m_work.data() + 0x200 + size_t(i) * 4);
        if (std::fabs(before - after) > 0.001f) // 0x00321850
            for (int i = 0; i < 9; ++i)
                *WorkPtr(0x17, i) += (before - after) * (1.0f / 9.0f);
    }
}

} // namespace Tune
