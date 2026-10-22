#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// UG2's performance tuning (the sliders of UI_PerformanceTuning_*, ISSUES #207 / #210 / #211): a port
// of the range / value engine around 0x0031f2a0 .. 0x00322xxx for the fields whose pieces are
// decoded (0, 1, 2, 4, 5, 6, 8, 9, 0x12..0x16: suspension, brake balance, tyres, aerodynamics).
// A tuning session holds the car's stock record, the part-tuned working copy (PerfModel frame) and
// one byte per slider (100 = neutral, 0..200 = position -1..+1).  Offsets named "mem" are the
// executable's record offsets (file offset + 8); "frame" offsets are PerfModel's (record + 0x118).
namespace Tune {

constexpr int kFields = 25;       // 0x19 tuning fields
constexpr size_t kSettingSize = 41; // 0x29: 23 single fields, then field 23 (9) and field 24 (9)

// One entry of GLOBALB's 0x34a1a table: an installed part unlocks a field.
struct Entry {
    uint32_t part = 0;
    uint8_t level = 0, field = 0, mode = 0;
    float min = 0, max = 0;       // the physical modifier (meaning depends on `mode`)
    float uiLo = 0, uiHi = 0;     // the slider's display range (-99.99 = none)
};

class Table {
public:
    // `container` is the payload of the 0x80034a10 GLOBALB chunk.
    bool Parse(const uint8_t *container, size_t size);
    size_t Count() const { return m_entries.size(); }
    const Entry &At(size_t i) const { return m_entries[i]; }
    // 0x0031f2a0: the entry of the highest installed level for a field, or null.
    const Entry *Active(int field, const std::vector<uint16_t> &installed) const;
    void Add(const Entry &e) { m_entries.push_back(e); }

private:
    std::vector<Entry> m_entries;
};

class Session {
public:
    // record: the stock 0x890-byte car record; work: the 0x2b0-byte frame after PerfModel::Apply.
    Session(const Table &table, const uint8_t *record, const std::vector<uint8_t> &workFrame,
            const std::vector<uint16_t> &installed);

    // Supported and unlocked (0x0031faf8 for the fields this port handles).
    bool FieldActive(int field) const;
    static bool Supported(int field);
    // The unit flag of the tuning object (+4): 1 = metric (cm), else inches for the lowering fields.
    void SetMetric(bool metric) { m_metric = metric; }
    // The slider range in display units (0x003201f0 with the display conversion), and the step one
    // Left / Right press moves (0x002c1e30: a thirtieth of that span).
    bool SliderRangeDisplay(int field, int sub, float &lo, float &hi) const;
    float Step(int field) const;

    // 0x00320670: the physical bounds of a field (record extremes, adjusted by the table entry).
    bool Bounds(int field, int sub, bool average, float &lo, float &hi) const;
    // 0x003201f0 (without the display conversion): the slider's physical range around the reference.
    bool SliderRange(int field, int sub, float &lo, float &hi) const;
    // 0x00320030 with the display flag: the display range from the table (0..10 for plain sliders).
    bool DisplayRange(int field, int sub, float &lo, float &hi) const;

    // Slider byte (0..200) of a field; the value shown (0x0031f828 -> display) and set (0x0031f8d8).
    uint8_t &Byte(int field, int sub = 0) { return m_setting[Index(field, sub)]; }
    uint8_t Byte(int field, int sub = 0) const { return m_setting[Index(field, sub)]; }
    float Display(int field, int sub = 0) const;
    void SetDisplay(int field, int sub, float display);
    void Reset(int field, int sub = 0);
    void ResetAll();

    // Writes every active field's tuned physical value into the working frame (0x0031fbe0).
    void Apply();
    const std::vector<uint8_t> &Frame() const { return m_work; } // 0x2b0 bytes, ready to copy back

    const std::vector<uint8_t> &Setting() const { return m_setting; }
    void LoadSetting(const std::vector<uint8_t> &bytes);

private:
    static size_t Index(int field, int sub) { return field < 23 ? size_t(field) : (field == 23 ? 23 + size_t(sub) : 32 + size_t(sub)); }
    float RecM(size_t mem) const;                 // stock record float at a mem offset
    float *WorkPtr(int field, int sub);
    const float *WorkPtr(int field, int sub) const;
    float Ref(int field, int sub) const { return m_ref[Index(field, sub)]; }
    float ReadValue(int field, int sub) const;    // 0x00320e20: the working copy's current value
    float Base(int field, int sub) const;         // 0x00320500
    float Sign(int field) const { return (field == 3 || field == 7 || field == 9) ? -1.0f : 1.0f; }
    void AdjustByEntry(int field, float &lo, float &hi) const; // 0x00321118
    void WritePhysical(int field, int sub, float value);       // 0x003211f8
    float ToDisplay(float physical, int field) const;
    float ToPhysical(float display, int field) const;
    void CaptureRefs();

    const Table &m_table;
    std::vector<uint8_t> m_record;
    std::vector<uint8_t> m_work;
    std::vector<uint8_t> m_part; // the part-tuned frame (Apply restarts from it)
    std::vector<uint16_t> m_installed;
    std::vector<uint8_t> m_setting;
    std::vector<float> m_ref;
    bool m_metric = true;
};

} // namespace Tune
