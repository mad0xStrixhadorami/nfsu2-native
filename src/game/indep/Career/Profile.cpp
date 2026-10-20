#include "game/indep/Career/Profile.h"

#include <cstdio>
#include <cstring>

#include "game/indep/Career/Memcard.h"

namespace Career {

namespace {

constexpr char kMagic[8] = {'U', '2', 'C', 'A', 'R', 'E', 'E', 'R'};
// v2 adds the best-time direction key, v3 the drift scores, v4 the signed sponsors, v5 the paint
// finish set of each owned car (GLOSS/METAL/PEARL, ISSUES #168), v6 the other eight UI_Paint parts
// (Spoiler .. Muffler Tip, ISSUES #169), v7 the fitted spoiler (Body Shop, ISSUES #185), v8 the
// fitted roof scoop and exhaust tip (#186), v9 the owned Body Shop parts (#187).
constexpr uint32_t kVersion = 9;

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
    // Appended after the cars so a save written by the older port (no completion list) still parses.
    PutU32(payload, uint32_t(p.completedEvents.size()));
    for (uint32_t id : p.completedEvents)
        PutU32(payload, id);
    // Best times follow the completion list (also optional on load). v2 stores the direction key of
    // the 0x4fb4ac table ({race s16, direction u8, value s32}), so forward and reverse runs differ.
    PutU32(payload, uint32_t(p.bestTimes.size()));
    for (const Profile::BestTime &b : p.bestTimes) {
        PutI32(payload, b.race);
        PutI32(payload, b.direction);
        PutI32(payload, b.millis);
    }
    // v3: drift best scores, appended so v2 saves still parse (they simply stop after the times).
    PutU32(payload, uint32_t(p.bestScores.size()));
    for (const Profile::BestScore &b : p.bestScores) {
        PutI32(payload, b.race);
        PutI32(payload, b.direction);
        PutI32(payload, b.score);
    }
    // v4: the signed sponsor of each career area (0 = none), the +0xe8 slot of the area objects.
    for (uint32_t h : p.signedSponsors)
        PutU32(payload, h);
    // v5: the finish set of each car.  Appended (instead of widening the car records above) so a
    // v4 save still parses; 0 means GLOSS, which is what every v4 paint index referred to.
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars)
        PutU32(payload, c.paintSet);
    // v6: the per-part colours of the other eight UI_Paint.fng cards, in card order.
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars)
        for (int i = 0; i < OwnedCar::kPaintParts; ++i) {
            PutU32(payload, c.partPaint[i]);
            PutU32(payload, c.partPaintSet[i]);
        }
    // v7: the fitted spoiler of each car (Body Shop, #185): style 1..40 (+carbon flag), 0 = stock.
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars) {
        PutU32(payload, c.spoiler);
        PutU32(payload, c.spoilerCarbon ? 1 : 0);
    }
    // v8: the fitted roof scoop and exhaust tip (#186).
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars) {
        PutU32(payload, c.roof);
        PutU32(payload, c.roofDual ? 1 : 0);
        PutU32(payload, c.roofOffset ? 1 : 0);
        PutU32(payload, c.roofCarbon ? 1 : 0);
        PutU32(payload, c.exhaust);
    }
    // v9: the owned Body Shop parts (#187).
    PutU32(payload, uint32_t(p.ownedParts.size()));
    for (uint32_t key : p.ownedParts)
        PutU32(payload, key);

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
    if ((version < 1 || version > 9) || o + size > data.size())
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
    if (q + 4 <= payload.size()) { // the completion list is optional (see Serialize)
        uint32_t done = 0;
        if (!GetU32(payload, q, done) || done > 4096)
            return false;
        for (uint32_t i = 0; i < done; ++i) {
            uint32_t id;
            if (!GetU32(payload, q, id))
                return false;
            p.completedEvents.push_back(id);
        }
    }
    if (q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n > 4096)
            return false;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t race, millis;
            int32_t direction = 0;
            if (!GetI32(payload, q, race))
                return false;
            if (version >= 2 && !GetI32(payload, q, direction))
                return false;
            if (!GetI32(payload, q, millis))
                return false;
            p.bestTimes.push_back({race, direction, millis});
        }
    }
    if (version >= 3 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n > 4096)
            return false;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t race, direction, score;
            if (!GetI32(payload, q, race) || !GetI32(payload, q, direction) ||
                !GetI32(payload, q, score))
                return false;
            p.bestScores.push_back({race, direction, score});
        }
    }
    if (version >= 4)
        for (uint32_t &h : p.signedSponsors)
            if (!GetU32(payload, q, h))
                return false;
    if (version >= 5 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars)
            if (!GetU32(payload, q, c.paintSet))
                return false;
    }
    if (version >= 6 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars)
            for (int i = 0; i < OwnedCar::kPaintParts; ++i)
                if (!GetU32(payload, q, c.partPaint[i]) ||
                    !GetU32(payload, q, c.partPaintSet[i]))
                    return false;
    }
    if (version >= 7 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars) {
            uint32_t spoiler = 0, carbon = 0;
            if (!GetU32(payload, q, spoiler) || !GetU32(payload, q, carbon))
                return false;
            c.spoiler = uint8_t(spoiler);
            c.spoilerCarbon = carbon != 0;
        }
    }
    if (version >= 8 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars) {
            uint32_t roof = 0, dual = 0, offset = 0, carbon = 0, exhaust = 0;
            if (!GetU32(payload, q, roof) || !GetU32(payload, q, dual) || !GetU32(payload, q, offset) ||
                !GetU32(payload, q, carbon) || !GetU32(payload, q, exhaust))
                return false;
            c.roof = uint8_t(roof);
            c.roofDual = dual != 0;
            c.roofOffset = offset != 0;
            c.roofCarbon = carbon != 0;
            c.exhaust = uint8_t(exhaust);
        }
    }
    if (version >= 9 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n > 4096)
            return false;
        p.ownedParts.clear();
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t key = 0;
            if (!GetU32(payload, q, key))
                return false;
            p.ownedParts.push_back(key);
        }
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

bool LoadCareer(Profile &out, const std::string &name)
{
    MemoryCard &card = MemoryCard::Get();
    std::vector<CardFile> files = card.List();
    if (files.empty())
        return false;
    std::string pick = name.empty() ? files.front().name : MemoryCard::SanitizeName(name);
    bool known = false;
    for (const CardFile &f : files)
        if (f.name == pick)
            known = true;
    if (!known)
        return false;
    std::vector<uint8_t> data;
    if (!card.Read(pick, data))
        return false;
    if (!Deserialize(data, out)) {
        fprintf(stderr, "[Career] %s is not a readable save\n", pick.c_str());
        return false;
    }
    printf("[Career] loaded %s from %s (money %d, respect %d, %zu cars, %zu events done)\n",
           out.name.c_str(), card.SlotDir().c_str(), out.money, out.respect, out.cars.size(),
           out.completedEvents.size());
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
