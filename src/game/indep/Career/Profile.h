#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Career {

// A car the player owns in career mode.
struct OwnedCar {
    std::string model;   // GLOBALB car database folder, e.g. "SKYLINE"
    uint32_t paint = 0;  // Base Paint: 1-based color index in `paintSet` (0 = stock)
    uint32_t paintSet = 0; // 0 = GLOSS (the only set v4 saves could store), else a 0xebb03e66 set
    // The other eight UI_Paint.fng cards (0x002bcff8 order): Spoiler, Roof Scoop, Mirrors, Rims,
    // Spinners, Brakes, Engine Accents, Muffler Tip.  Index 0 = the part's stock colour; a stored
    // set of 0 means the part's default set (PaintCatalog::SetForPart).  The original keeps one
    // colour record per installed part (0x002b3488 / 0x002b2680); this is the port's model of it.
    static constexpr int kPaintParts = 8;
    uint32_t partPaint[kPaintParts] = {};
    uint32_t partPaintSet[kPaintParts] = {};
    // The fitted spoiler (Body Shop, ISSUES #185): 1..40 = SPOILER_STYLE<NN>[_CF]_A, 0 = the stock
    // wing.  The original keeps every installed body part in the career item pool (0x002b5b00
    // tests ownership); the port only mounts spoilers so far, so one record per car covers it.
    uint8_t spoiler = 0;
    bool spoilerCarbon = false;
    // Roof scoop and exhaust tip fitted by the Body Shop (ISSUES #186).  roof 1..17 with the three
    // body flags (OFFSET / DUAL / carbon), exhaust 1..10; 0 keeps the stock part.
    uint8_t roof = 0;
    bool roofDual = false, roofOffset = false, roofCarbon = false;
    uint8_t exhaust = 0;
    // Hood / headlights / taillights fitted by the Body Shop (ISSUES #188): hood 1..28, lights
    // 1..14, 0 = stock.  The carbon hood ships no separate solid, so hoodCarbon only picks the
    // price band and the record key.
    uint8_t hood = 0;
    bool hoodCarbon = false;
    uint8_t headlight = 0;
    uint8_t taillight = 0;
    // Front/rear bumpers and side skirts fitted by the Body Shop (ISSUES #190).  These records name
    // their solid with the kit number ("_KIT<NN>") instead of "_STYLE<NN>", so the value is the
    // catalogue's model-table kit, 1..30; 0 keeps the stock panel.
    uint8_t frontBumper = 0;
    uint8_t rearBumper = 0;
    uint8_t skirt = 0;
    // Wide-body kit fitted by the Body Shop (ISSUES #193): the catalogue's KITW number, 1..4;
    // 0 keeps the stock KIT00 shell.  The kit replaces the whole shell and both door skins.
    uint8_t wideBody = 0;
    // The performance packages' parts fitted by the Performance Shop (ISSUES #198): global part ids
    // from the 0x34a15 table (PerfPackages).  The original keeps them in the car's installed-part
    // set (0x0031a548 on the shop manager's car, +0xc40).
    std::vector<uint16_t> perfParts;
    // The tuning slider bytes of the car (Performance Tuning, ISSUES #214): 41 bytes, 100 = neutral;
    // empty = never tuned.
    std::vector<uint8_t> tune;
    // The vinyl layers on the car's paint skin (Graphics Shop, ISSUES #223), bottom layer first,
    // at most kMaxVinylLayers.  `design` is the pack texture name without the car prefix
    // (e.g. "WILD_172B"); `color` the red / green / blue region colours.
    static constexpr int kMaxVinylLayers = 4;
    struct VinylLayer {
        std::string design;
        uint8_t color[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    };
    std::vector<VinylLayer> vinyls;
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
    // The career item pool (0x002b5b00 tests it): owned Body Shop parts, encoded by FEStartup's
    // PartKey (part id, style and the carbon/OFFSET/DUAL flags).  The original keeps a real item
    // pool in the save block; this is the port's flat stand-in (ISSUES #187).
    std::vector<uint32_t> ownedParts;
    // Completed 0x34a11 events, keyed by the record's +0x08 id - the same test 0x00268bd8 makes for
    // the world map (0x001ee590 stores the result at item index 7).
    std::vector<uint32_t> completedEvents;
    // The signed sponsor (name hash, 0 = none) of each of the 8 area objects (+0xe8, ISSUES #165).
    static constexpr int kAreas = 8;
    uint32_t signedSponsors[kAreas] = {};
    int SignedSponsorCount() const // 0x00267920
    {
        int n = 0;
        for (uint32_t h : signedSponsors)
            n += h != 0;
        return n;
    }
    uint32_t playTime = 0; // seconds
    // Best finishing times (the 0x4fb4ac best-time table, 0x00272e18: {race s16, direction u8, value}).
    struct BestTime {
        int race;
        int direction; // 0 forward / 1 reverse (the 0x4fb4ac key)
        int millis;
    };
    std::vector<BestTime> bestTimes;
    // Drift events use the same 0x4fb4ac entries but score them with 0x00272ee8 (largest wins)
    // instead of 0x00272e18 (smallest wins); the two kinds never share a race.
    struct BestScore {
        int race;
        int direction;
        int score;
    };
    std::vector<BestScore> bestScores;
    int BestTimeFor(int race, int direction = 0) const
    {
        for (const BestTime &b : bestTimes)
            if (b.race == race && b.direction == direction)
                return b.millis;
        return 0;
    }
    void RecordTime(int race, int direction, int millis)
    {
        for (BestTime &b : bestTimes)
            if (b.race == race && b.direction == direction) {
                if (millis < b.millis)
                    b.millis = millis;
                return;
            }
        bestTimes.push_back({race, direction, millis});
    }
    int BestScoreFor(int race, int direction = 0) const
    {
        for (const BestScore &b : bestScores)
            if (b.race == race && b.direction == direction)
                return b.score;
        return 0;
    }
    void RecordScore(int race, int direction, int score)
    {
        for (BestScore &b : bestScores)
            if (b.race == race && b.direction == direction) {
                if (score > b.score)
                    b.score = score;
                return;
            }
        bestScores.push_back({race, direction, score});
    }

    bool Completed(uint32_t id) const
    {
        for (uint32_t e : completedEvents)
            if (e == id)
                return true;
        return false;
    }
    void Complete(uint32_t id)
    {
        if (!Completed(id))
            completedEvents.push_back(id);
    }

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

// The 0x4fb4ac table exists even without a career save (Quick Race), but a console only keeps it in
// the player's save block. The port mirrors it to a sidecar on the same card (RECORDS.REC) so QR
// best laps survive a restart; Career/Records.cpp owns that file. The in-memory table is also seeded
// from the profile when one is loaded.
struct BestTable {
    std::vector<Profile::BestTime> times;
    std::vector<Profile::BestScore> scores;

    int BestTimeFor(int race, int direction = 0) const
    {
        for (const Profile::BestTime &b : times)
            if (b.race == race && b.direction == direction)
                return b.millis;
        return 0;
    }
    void RecordTime(int race, int direction, int millis)
    {
        for (Profile::BestTime &b : times)
            if (b.race == race && b.direction == direction) {
                if (millis < b.millis)
                    b.millis = millis;
                return;
            }
        times.push_back({race, direction, millis});
    }
    int BestScoreFor(int race, int direction = 0) const
    {
        for (const Profile::BestScore &b : scores)
            if (b.race == race && b.direction == direction)
                return b.score;
        return 0;
    }
    void RecordScore(int race, int direction, int score)
    {
        for (Profile::BestScore &b : scores)
            if (b.race == race && b.direction == direction) {
                if (score > b.score)
                    b.score = score;
                return;
            }
        scores.push_back({race, direction, score});
    }
    void MergeFrom(const Profile &p)
    {
        for (const Profile::BestTime &b : p.bestTimes)
            RecordTime(b.race, b.direction, b.millis);
        for (const Profile::BestScore &b : p.bestScores)
            RecordScore(b.race, b.direction, b.score);
    }
};

// The port's save format (stand-in, see above).
std::vector<uint8_t> Serialize(const Profile &p);
bool Deserialize(const std::vector<uint8_t> &data, Profile &out);

// QR sidecar (RECORDS.REC in the memory-card slot directory). Both use the port's own format like
// the career save; a missing/corrupt file just yields an empty table.
bool SaveRecords(const BestTable &t);
bool LoadRecords(BestTable &out);

// ---- memory-card helpers (Career/Memcard.h) ------------------------------------------------
// A career profile is one save file on the card, named "<profile name>.SAV" (UG2 keeps its career
// save in the same place, e.g. the CreateUIProfileManager / FEPkgMemcardList file list).
bool SaveCareer(const Profile &p);   // false if the card is missing/full
bool LoadCareer(Profile &out, const std::string &name = std::string()); // name empty = first save
bool DeleteCareer(const std::string &name);
bool CareerExists(); // any save on the card

} // namespace Career
