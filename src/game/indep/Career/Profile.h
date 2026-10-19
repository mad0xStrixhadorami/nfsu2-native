#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Career {

// A car the player owns in career mode.
struct OwnedCar {
    std::string model; // GLOBALB car database folder, e.g. "SKYLINE"
    uint32_t paint = 0; // paint index (0 = the car's stock paint)
};

// The career profile: what UG2 keeps in the per-player save block at 0x004f1744 (0xac54 bytes per
// player, `&DAT_004f1744 + player * 0xac54` in 0x001e8d00 / 0x001e9ba8 / 0x0025ece0 / ...). The
// block is a serialized object graph (an item pool at +0x9bec reached through 0x00275a28 /
// 0x002759a0, holding owned cars and career state) and is written to the memory card by EA's save
// subsystem (the 0x00265xxx "save data" module around the 0x004e8710 manager + the memcard thread).
//
// That on-card byte layout is EA's own and is not decoded (no sample console save exists on this
// disc), so this struct is the port's own model of the same data, and the serializer below is a
// stand-in for EA's. Fields we can trace are marked with their source; the money/respect starting
// values are unconfirmed (see the comment on them).
struct Profile {
    static constexpr int kStartingMoney = 1000;  // stand-in; candidate 0x002ed830 (uGpffffedac = 1000)
    static constexpr int kStartingRespect = 200; // stand-in; candidate 0x002ed820 (uGpffffeda8 = 200)
    static constexpr int kMaxCars = 32;
    static constexpr int kSaveNameMax = 16;

    bool valid = false;
    std::string name = "PLAYER";
    int money = kStartingMoney;
    int respect = kStartingRespect;
    int stage = 0;               // career progress (completed events); 0x34a11 event table, #136
    std::string region = "L4RA"; // current region bundle
    int currentCar = 0;          // index into cars
    std::vector<OwnedCar> cars;
    uint32_t playTime = 0; // seconds

    const OwnedCar *CurrentCar() const
    {
        if (currentCar < 0 || currentCar >= int(cars.size()))
            return nullptr;
        return &cars[size_t(currentCar)];
    }
    bool Owns(const std::string &model) const
    {
        for (const OwnedCar &c : cars)
            if (c.model == model)
                return true;
        return false;
    }
};

// The port's save format (stand-in, see above).
std::vector<uint8_t> Serialize(const Profile &p);
bool Deserialize(const std::vector<uint8_t> &data, Profile &out);

// ---- memory-card helpers (Career/Memcard.h) ------------------------------------------------
// A career profile is one save file on the card, named "<profile name>.SAV" (UG2 keeps its career
// save in the same place, e.g. the CreateUIProfileManager / FEPkgMemcardList file list).
bool SaveCareer(const Profile &p);   // false if the card is missing/full
bool LoadCareer(Profile &out, const std::string &name = std::string()); // name empty = first save
bool DeleteCareer(const std::string &name);
bool CareerExists(); // any save on the card

} // namespace Career
