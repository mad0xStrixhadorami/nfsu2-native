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
// fitted roof scoop and exhaust tip (#186), v9 the owned Body Shop parts (#187), v10 the fitted
// hood, headlights and taillights (#188), v11 the fitted front/rear bumpers and side skirts (#190),
// v12 the fitted wide-body kit (#193), v13 the fitted performance parts (#198), v14 the tuning slider bytes (#214).
constexpr uint32_t kVersion = 15;

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
    // v10: the fitted hood, headlights and taillights (#188).
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars) {
        PutU32(payload, c.hood);
        PutU32(payload, c.hoodCarbon ? 1 : 0);
        PutU32(payload, c.headlight);
        PutU32(payload, c.taillight);
    }
    // v11: the fitted front/rear bumpers and side skirts (#190), stored as the catalogue's kit
    // number (the same value the part list shows), 0 = stock.
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars) {
        PutU32(payload, c.frontBumper);
        PutU32(payload, c.rearBumper);
        PutU32(payload, c.skirt);
    }
    // v12: the fitted wide-body kit (#193), 0 = stock KIT00 shell.
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars)
        PutU32(payload, c.wideBody);
    // v13: the fitted performance parts (#198): a count and the 0x34a15 part ids per car.
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars) {
        PutU32(payload, uint32_t(c.perfParts.size()));
        for (uint16_t id : c.perfParts)
            PutU32(payload, id);
    }
    // v14: the tuning slider bytes (#214): a length (0 or 41) and the bytes per car.
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars) {
        PutU32(payload, uint32_t(c.tune.size()));
        for (uint8_t b : c.tune)
            PutU32(payload, b);
    }
    // v15: the vinyl layers (#223): per car a count, then per layer the design name and 9 colour bytes.
    PutU32(payload, uint32_t(p.cars.size()));
    for (const OwnedCar &c : p.cars) {
        PutU32(payload, uint32_t(c.vinyls.size()));
        for (const OwnedCar::VinylLayer &l : c.vinyls) {
            PutU32(payload, uint32_t(l.design.size()));
            for (char ch : l.design)
                PutU32(payload, uint8_t(ch));
            for (const auto &col : l.color)
                for (uint8_t v : col)
                    PutU32(payload, v);
        }
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
    if ((version < 1 || version > 15) || o + size > data.size())
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
    if (version >= 10 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars) {
            uint32_t hood = 0, carbon = 0, headlight = 0, taillight = 0;
            if (!GetU32(payload, q, hood) || !GetU32(payload, q, carbon) ||
                !GetU32(payload, q, headlight) || !GetU32(payload, q, taillight))
                return false;
            c.hood = uint8_t(hood);
            c.hoodCarbon = carbon != 0;
            c.headlight = uint8_t(headlight);
            c.taillight = uint8_t(taillight);
        }
    }
    if (version >= 11 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars) {
            uint32_t front = 0, rear = 0, skirt = 0;
            if (!GetU32(payload, q, front) || !GetU32(payload, q, rear) || !GetU32(payload, q, skirt))
                return false;
            c.frontBumper = uint8_t(front);
            c.rearBumper = uint8_t(rear);
            c.skirt = uint8_t(skirt);
        }
    }
    if (version >= 12 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars) {
            uint32_t wide = 0;
            if (!GetU32(payload, q, wide))
                return false;
            c.wideBody = uint8_t(wide);
        }
    }
    if (version >= 13 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars) {
            uint32_t count = 0;
            if (!GetU32(payload, q, count) || count > 64)
                return false;
            c.perfParts.clear();
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t id = 0;
                if (!GetU32(payload, q, id))
                    return false;
                c.perfParts.push_back(uint16_t(id));
            }
        }
    }
    if (version >= 14 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars) {
            uint32_t count = 0;
            if (!GetU32(payload, q, count) || count > 64)
                return false;
            c.tune.clear();
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t b = 0;
                if (!GetU32(payload, q, b))
                    return false;
                c.tune.push_back(uint8_t(b));
            }
        }
    }
    if (version >= 15 && q + 4 <= payload.size()) {
        uint32_t n = 0;
        if (!GetU32(payload, q, n) || n != p.cars.size())
            return false;
        for (OwnedCar &c : p.cars) {
            uint32_t count = 0;
            if (!GetU32(payload, q, count) || count > uint32_t(OwnedCar::kMaxVinylLayers))
                return false;
            c.vinyls.clear();
            for (uint32_t i = 0; i < count; ++i) {
                OwnedCar::VinylLayer l;
                uint32_t len = 0;
                if (!GetU32(payload, q, len) || len > 40)
                    return false;
                for (uint32_t k = 0; k < len; ++k) {
                    uint32_t ch = 0;
                    if (!GetU32(payload, q, ch))
                        return false;
                    l.design.push_back(char(ch));
                }
                for (auto &col : l.color)
                    for (uint8_t &v : col) {
                        uint32_t x = 0;
                        if (!GetU32(payload, q, x))
                            return false;
                        v = uint8_t(x);
                    }
                c.vinyls.push_back(std::move(l));
            }
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
