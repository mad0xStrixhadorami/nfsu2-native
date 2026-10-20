// Career profile save format: direction-keyed best times/scores (0x4fb4ac) and v1/v2 compatibility.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game/indep/Career/Profile.h"

namespace fs = std::filesystem;

namespace {

void PutU32(std::vector<uint8_t> &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(v >> (i * 8)));
}
void PutStr(std::vector<uint8_t> &b, const std::string &s)
{
    b.push_back(uint8_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

// A save written by the older port (version 1, best times are {race, ms} pairs; here none).
std::vector<uint8_t> MakeV1Save()
{
    std::vector<uint8_t> payload;
    PutStr(payload, "OLD");
    PutU32(payload, 100);
    PutU32(payload, 10);
    PutU32(payload, 0);
    PutStr(payload, "L4RA");
    PutU32(payload, 0); // currentCar
    PutU32(payload, 0); // playTime
    PutU32(payload, 0); // cars
    PutU32(payload, 1); // completedEvents
    PutU32(payload, 0xb01c890b);
    // No best-time list: Deserialize stops after the completion list, as the old port wrote it.

    uint32_t sum = 0;
    for (uint8_t c : payload)
        sum = sum * 31 + c;
    std::vector<uint8_t> out = {'U', '2', 'C', 'A', 'R', 'E', 'E', 'R'};
    PutU32(out, 1);
    PutU32(out, uint32_t(payload.size()));
    PutU32(out, sum);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// A version-2 save (direction-keyed best times, no score list). The v3 loader must still read it.
std::vector<uint8_t> MakeV2Save()
{
    std::vector<uint8_t> payload;
    PutStr(payload, "V2");
    PutU32(payload, 500);
    PutU32(payload, 50);
    PutU32(payload, 0);
    PutStr(payload, "L4RA");
    PutU32(payload, 0); // currentCar
    PutU32(payload, 0); // playTime
    PutU32(payload, 0); // cars
    PutU32(payload, 0); // completedEvents
    PutU32(payload, 2); // bestTimes
    PutU32(payload, 4012);
    PutU32(payload, 0);
    PutU32(payload, 91000);
    PutU32(payload, 4012);
    PutU32(payload, 1);
    PutU32(payload, 92000);

    uint32_t sum = 0;
    for (uint8_t c : payload)
        sum = sum * 31 + c;
    std::vector<uint8_t> out = {'U', '2', 'C', 'A', 'R', 'E', 'E', 'R'};
    PutU32(out, 2);
    PutU32(out, uint32_t(payload.size()));
    PutU32(out, sum);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

int Fail(const char *what)
{
    std::fprintf(stderr, "profile_test: %s\n", what);
    return 1;
}

} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "nfsu2_profile_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    setenv("NFSU2_MEMCARD", dir.c_str(), 1);

    // Version 3 round-trip: forward and reverse best times are separate keys, the smaller time wins
    // within a key, and drift scores keep the larger value under the same direction key.
    Career::Profile p;
    p.name = "TEST";
    p.cars.push_back({"SKYLINE", 0});
    p.cars[0].paint = 17; // GLOBALB GLOSS colour index (ISSUES #167)
    p.completedEvents.push_back(0x12345678);
    p.RecordTime(4012, 0, 90000);
    p.RecordTime(4012, 1, 89000);
    p.RecordTime(4012, 1, 95000);
    p.RecordScore(4311, 0, 12345);
    p.RecordScore(4311, 1, 15000);
    p.RecordScore(4311, 1, 14000);
    p.signedSponsors[2] = 0x167a374f;
    p.signedSponsors[5] = 0x5e96e1be;
    if (!Career::SaveCareer(p))
        return Fail("v3 save failed");

    Career::Profile q;
    if (!Career::LoadCareer(q, "TEST"))
        return Fail("v3 load failed");
    if (q.BestTimeFor(4012, 0) != 90000)
        return Fail("forward key mismatch");
    if (q.BestTimeFor(4012, 1) != 89000)
        return Fail("reverse key must keep the smaller time");
    if (q.BestTimeFor(4012, 2) != 0)
        return Fail("unknown direction must not alias");
    if (q.BestScoreFor(4311, 0) != 12345)
        return Fail("forward score mismatch");
    if (q.BestScoreFor(4311, 1) != 15000)
        return Fail("reverse score must keep the larger value");
    if (q.BestScoreFor(4311, 2) != 0)
        return Fail("unknown score direction must not alias");
    if (q.signedSponsors[2] != 0x167a374f || q.signedSponsors[5] != 0x5e96e1be || q.SignedSponsorCount() != 2)
        return Fail("signed sponsors must round-trip (v4)");
    if (q.cars.size() != 1 || q.cars[0].paint != 17)
        return Fail("body paint index must round-trip");

    // Version 2: still loads its direction-keyed times and has no scores.
    {
        const std::vector<uint8_t> old = MakeV2Save();
        std::ofstream out((dir / "V2.SAV").string(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(old.data()), std::streamsize(old.size()));
    }
    Career::Profile v2;
    if (!Career::LoadCareer(v2, "V2"))
        return Fail("v2 load failed");
    if (v2.BestTimeFor(4012, 0) != 91000 || v2.BestTimeFor(4012, 1) != 92000)
        return Fail("v2 best times mismatch");
    if (v2.BestScoreFor(4311, 0) != 0)
        return Fail("v2 save must have no scores");

    // Version 1: the old save still loads; its best-time list is absent, so both keys are zero.
    {
        const std::vector<uint8_t> old = MakeV1Save();
        std::ofstream out((dir / "OLD.SAV").string(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(old.data()), std::streamsize(old.size()));
    }
    Career::Profile r;
    if (!Career::LoadCareer(r, "OLD"))
        return Fail("v1 load failed");
    if (r.completedEvents.size() != 1 || r.completedEvents[0] != 0xb01c890b)
        return Fail("v1 completion list mismatch");
    if (r.BestTimeFor(0xb01c890b, 0) != 0 || r.BestTimeFor(0xb01c890b, 1) != 0)
        return Fail("v1 save must have no best times");
    if (r.BestScoreFor(0xb01c890b, 0) != 0)
        return Fail("v1 save must have no best scores");

    // RECORDS.REC: the QR-only sidecar round-trips both tables and is independent of a .SAV.
    Career::BestTable rec;
    rec.RecordTime(4011, 0, 123456);
    rec.RecordTime(4011, 1, 120000);
    rec.RecordScore(4311, 0, 9000);
    if (!Career::SaveRecords(rec))
        return Fail("records save failed");
    Career::BestTable rec2;
    if (!Career::LoadRecords(rec2))
        return Fail("records load failed");
    if (rec2.BestTimeFor(4011, 0) != 123456 || rec2.BestTimeFor(4011, 1) != 120000)
        return Fail("records times mismatch");
    if (rec2.BestScoreFor(4311, 0) != 9000)
        return Fail("records score mismatch");
    if (fs::file_size(dir / "RECORDS.REC", ec) == 0)
        return Fail("records file missing");

    fs::remove_all(dir, ec);
    std::printf("profile_test: passed\n");
    return 0;
}
