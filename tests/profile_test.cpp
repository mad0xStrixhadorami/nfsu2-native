// Career profile save format: direction key of the best-time table (0x4fb4ac) and v1 compatibility.
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

    // Version 2 round-trip: forward and reverse best times are separate keys, and the smaller time
    // wins within a key.
    Career::Profile p;
    p.name = "TEST";
    p.cars.push_back({"SKYLINE", 0});
    p.completedEvents.push_back(0x12345678);
    p.RecordTime(4012, 0, 90000);
    p.RecordTime(4012, 1, 89000);
    p.RecordTime(4012, 1, 95000);
    if (!Career::SaveCareer(p))
        return Fail("v2 save failed");

    Career::Profile q;
    if (!Career::LoadCareer(q, "TEST"))
        return Fail("v2 load failed");
    if (q.BestTimeFor(4012, 0) != 90000)
        return Fail("forward key mismatch");
    if (q.BestTimeFor(4012, 1) != 89000)
        return Fail("reverse key must keep the smaller time");
    if (q.BestTimeFor(4012, 2) != 0)
        return Fail("unknown direction must not alias");

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

    fs::remove_all(dir, ec);
    std::printf("profile_test: passed\n");
    return 0;
}
