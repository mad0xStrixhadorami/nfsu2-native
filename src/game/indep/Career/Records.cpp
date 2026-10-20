#include "game/indep/Career/Profile.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "game/indep/Career/Memcard.h"

namespace Career {

namespace {

namespace fs = std::filesystem;

constexpr char kMagic[8] = {'U', '2', 'R', 'E', 'C', 'O', 'R', 'D'};
constexpr uint32_t kVersion = 1;
constexpr const char *kFileName = "RECORDS.REC";

void PutU32(std::vector<uint8_t> &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(v >> (i * 8)));
}
void PutI32(std::vector<uint8_t> &b, int32_t v) { PutU32(b, uint32_t(v)); }

bool GetU32(const std::vector<uint8_t> &b, size_t &o, uint32_t &v)
{
    if (o + 4 > b.size())
        return false;
    v = uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) |
        (uint32_t(b[o + 3]) << 24);
    o += 4;
    return true;
}
bool GetI32(const std::vector<uint8_t> &b, size_t &o, int32_t &v)
{
    uint32_t u;
    if (!GetU32(b, o, u))
        return false;
    v = int32_t(u);
    return true;
}

fs::path RecordsPath()
{
    MemoryCard &card = MemoryCard::Get();
    card.Present();
    return fs::path(card.SlotDir()) / kFileName;
}

} // namespace

bool SaveRecords(const BestTable &t)
{
    std::vector<uint8_t> payload;
    PutU32(payload, uint32_t(t.times.size()));
    for (const Profile::BestTime &b : t.times) {
        PutI32(payload, b.race);
        PutI32(payload, b.direction);
        PutI32(payload, b.millis);
    }
    PutU32(payload, uint32_t(t.scores.size()));
    for (const Profile::BestScore &b : t.scores) {
        PutI32(payload, b.race);
        PutI32(payload, b.direction);
        PutI32(payload, b.score);
    }

    uint32_t sum = 0;
    for (uint8_t c : payload)
        sum = sum * 31 + c;

    std::vector<uint8_t> out(kMagic, kMagic + sizeof(kMagic));
    PutU32(out, kVersion);
    PutU32(out, uint32_t(payload.size()));
    PutU32(out, sum);
    out.insert(out.end(), payload.begin(), payload.end());

    std::ofstream f(RecordsPath(), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

bool LoadRecords(BestTable &out)
{
    out = BestTable{};
    std::ifstream f(RecordsPath(), std::ios::binary);
    if (!f)
        return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 24 || std::equal(kMagic, kMagic + sizeof(kMagic), data.begin()) == false)
        return false;

    size_t o = sizeof(kMagic);
    uint32_t version = 0, size = 0, sum = 0;
    if (!GetU32(data, o, version) || !GetU32(data, o, size) || !GetU32(data, o, sum))
        return false;
    if (version != kVersion || o + size > data.size())
        return false;

    uint32_t check = 0;
    for (uint32_t i = 0; i < size; ++i)
        check = check * 31 + data[o + i];
    if (check != sum)
        return false;

    data.resize(o + size);
    uint32_t n = 0;
    if (!GetU32(data, o, n) || n > 4096)
        return false;
    for (uint32_t i = 0; i < n; ++i) {
        int32_t race, direction, millis;
        if (!GetI32(data, o, race) || !GetI32(data, o, direction) || !GetI32(data, o, millis))
            return false;
        out.times.push_back({race, direction, millis});
    }
    if (!GetU32(data, o, n) || n > 4096)
        return false;
    for (uint32_t i = 0; i < n; ++i) {
        int32_t race, direction, score;
        if (!GetI32(data, o, race) || !GetI32(data, o, direction) || !GetI32(data, o, score))
            return false;
        out.scores.push_back({race, direction, score});
    }
    return true;
}

} // namespace Career
