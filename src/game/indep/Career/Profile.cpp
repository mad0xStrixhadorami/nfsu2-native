#include "game/indep/Career/Profile.h"

#include <cstdio>
#include <cstring>

#include "game/indep/Career/Memcard.h"

namespace Career {

namespace {

constexpr char kMagic[8] = {'U', '2', 'C', 'A', 'R', 'E', 'E', 'R'};
constexpr uint32_t kVersion = 1;

void PutU32(std::vector<uint8_t> &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(v >> (i * 8)));
}
void PutI32(std::vector<uint8_t> &b, int32_t v) { PutU32(b, uint32_t(v)); }
void PutStr(std::vector<uint8_t> &b, const std::string &s)
{
    size_t n = s.size() > 255 ? 255 : s.size();
    b.push_back(uint8_t(n));
    b.insert(b.end(), s.begin(), s.begin() + n);
}

bool GetU32(const std::vector<uint8_t> &b, size_t &o, uint32_t &v)
{
    if (o + 4 > b.size())
        return false;
    v = uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
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
bool GetStr(const std::vector<uint8_t> &b, size_t &o, std::string &s)
{
    if (o + 1 > b.size())
        return false;
    size_t n = b[o++];
    if (o + n > b.size())
        return false;
    s.assign(reinterpret_cast<const char *>(b.data() + o), n);
    o += n;
    return true;
}

} // namespace

std::vector<uint8_t> Serialize(const Profile &p)
{
    std::vector<uint8_t> payload;
    PutStr(payload, p.name);
    PutI32(payload, p.money);
    PutI32(payload, p.respect);
    PutI32(payload, p.stage);
    PutStr(payload, p.region);
    PutI32(payload, p.currentCar);
    PutU32(payload, p.playTime);
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars) {
        PutStr(payload, c.model);
        PutU32(payload, c.paint);
    }

    std::vector<uint8_t> out;
    out.insert(out.end(), kMagic, kMagic + 8);
    PutU32(out, kVersion);
    PutU32(out, uint32_t(payload.size()));
    uint32_t sum = 0;
    for (uint8_t c : payload)
        sum = sum * 31 + c;
    PutU32(out, sum);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool Deserialize(const std::vector<uint8_t> &data, Profile &out)
{
    if (data.size() < 20 || memcmp(data.data(), kMagic, 8) != 0)
        return false;
    size_t o = 8;
    uint32_t version, size, sum;
    if (!GetU32(data, o, version) || !GetU32(data, o, size) || !GetU32(data, o, sum))
        return false;
    if (version != kVersion || o + size > data.size())
        return false;
    std::vector<uint8_t> payload(data.begin() + o, data.begin() + o + size);
    uint32_t check = 0;
    for (uint8_t c : payload)
        check = check * 31 + c;
    if (check != sum)
        return false;

    Profile p;
    size_t q = 0;
    uint32_t count = 0;
    if (!GetStr(payload, q, p.name) || !GetI32(payload, q, p.money) || !GetI32(payload, q, p.respect) ||
        !GetI32(payload, q, p.stage) || !GetStr(payload, q, p.region) || !GetI32(payload, q, p.currentCar) ||
        !GetU32(payload, q, p.playTime) || !GetU32(payload, q, count))
        return false;
    if (count > uint32_t(Profile::kMaxCars))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        OwnedCar c;
        if (!GetStr(payload, q, c.model) || !GetU32(payload, q, c.paint))
            return false;
        p.cars.push_back(c);
    }
    if (p.currentCar < 0 || p.currentCar > int(p.cars.size()))
        p.currentCar = 0;
    p.valid = true;
    out = p;
    return true;
}

bool SaveCareer(const Profile &p)
{
    MemoryCard &card = MemoryCard::Get();
    if (!card.Present())
        return false;
    std::string name = MemoryCard::SanitizeName(p.name);
    std::vector<uint8_t> data = Serialize(p);
    bool ok = card.Write(name, data);
    printf("[Career] saved %s to %s (%d bytes)\n", name.c_str(), card.SlotDir().c_str(), int(data.size()));
    return ok;
}

bool LoadCareer(Profile &out)
{
    MemoryCard &card = MemoryCard::Get();
    std::vector<CardFile> files = card.List();
    if (files.empty())
        return false;
    std::vector<uint8_t> data;
    if (!card.Read(files.front().name, data))
        return false;
    if (!Deserialize(data, out)) {
        fprintf(stderr, "[Career] %s is not a readable save\n", files.front().name.c_str());
        return false;
    }
    printf("[Career] loaded %s from %s (money %d, respect %d, %zu cars)\n", out.name.c_str(),
           card.SlotDir().c_str(), out.money, out.respect, out.cars.size());
    return true;
}

bool DeleteCareer(const std::string &name)
{
    return MemoryCard::Get().Remove(MemoryCard::SanitizeName(name));
}

bool CareerExists()
{
    return !MemoryCard::Get().List().empty();
}

} // namespace Career
