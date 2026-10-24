#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Cars that are not on the disc (ISSUES #295). A mod car is a folder name (e.g. PICANTO) with
//  - a model generated in code (a lofted body shell, glass, lamps with the game's material slots) plus the wheels and brakes of a
//    donor car,
//  - a database record cloned from the donor and patched (names, mass, engine torque, limiter),
// and every file read for CARS\<MOD>\ is answered here: GEOMETRY.BIN is the generated model, TEXTURES / VINYLS / PARTS_ANIMATIONS come
// from the donor, so the rest of the game treats it like any other car.
namespace ModCars {
struct Mod {
    const char *folder;       // upper-case folder / type name
    const char *donor;        // donor car folder
    const char *manufacturer; // shown in front of the folder name
    float massTonnes;         // kerb weight
    float peakKw;             // engine peak power
    float topSpeedKmh;        // the speed limiter
};
const std::vector<Mod> &All();
const Mod *Find(const std::string &folderUpper);
// Does `path` ("CARS\\PICANTO\\GEOMETRY.BIN") belong to a mod car?
bool IsModPath(const char *path);
// Reads `path` for a mod car: the generated geometry, or the donor's file. `base` reads from the disc.
std::vector<uint8_t> Read(const char *path, const std::function<std::vector<uint8_t>(const char *)> &base);
// A mod's database record: the donor's 0x890 bytes with names, mass, inertia, torque curve and limiter patched.
std::vector<uint8_t> MakeRecord(const std::vector<uint8_t> &donorRecord, const Mod &mod);
} // namespace ModCars
