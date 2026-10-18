// Native entry point. Mirrors the boot order of the PS2 main() at 0x002dbf00:
//   0x00442a88 global ctors (handled by the C++ runtime here)
//   0x002dbd88 system init(argc, argv)  -> todo
//   0x002dbef8                          -> todo
//   0x002e2fc8 startup path selection   -> todo
//   0x002dc298 main loop (run on the scratchpad stack on PS2) -> todo
// Currently: mounts the disc archive, then shows the real boot screen (loading_boot.fng) through
// the reused FEng engine and the native renderer.

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "game/bWare/bCompression.h"
#include "game/indep/File/ZipFileSystem.h"
#include "Speed/Indep/Src/FEng/FEGroup.h"
#include "Speed/Indep/Src/FEng/FEngine.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "game/indep/Frontend/FENative.h"
#include "game/indep/Frontend/FEStartup.h"
#include "game/indep/Texture/TexturePack.h"
#include "game/indep/Frontend/PostRaceResults.h"
#include "game/indep/World/AiDriver.h"
#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/RaceRoute.h"
#include "game/indep/World/RaceSession.h"
#include "game/indep/World/TrackWorld.h"
#include "platform/linux/PlatformMemory.h"
#include "platform/linux/PlatformFile.h"

static int SelfTest(const ZipFileSystem &fs)
{
    // Files referenced by name in the executable, with the first 4 bytes we expect.
    struct { const char *path; const char magic[5]; } checks[] = {
        {"GLOBAL\\GLOBALB.LZC", "JDLZ"},
        {"FRONTEND\\FRONTB.LZC", "JDLZ"},
        {"global/globalb.lzc", "JDLZ"}, // same file: case and slash insensitive
    };
    int failures = 0;
    for (auto &c : checks) {
        ZipFile f;
        char head[4] = {};
        bool ok = fs.Open(c.path, f) && platform::FileRead(f.archiveHandle, f.offset, head, 4) == 4 &&
                  memcmp(head, c.magic, 4) == 0;
        printf("  %-24s %s", c.path, ok ? "ok" : "FAIL");
        if (fs.Open(c.path, f))
            printf("  (ZZDATA%u sector %u, %lld bytes)", f.entry->archive, f.entry->sector, (long long)f.size);
        printf("\n");
        failures += !ok;
    }
    ZipFile f;
    bool missingOk = !fs.Open("NOT\\A\\REAL\\FILE.BIN", f);
    printf("  %-24s %s\n", "(missing file)", missingOk ? "ok" : "FAIL");
    return failures + !missingOk;
}

// Read a whole file from the disc archive, decompressing it if it is compressed.
static std::vector<uint8_t> ReadGameFile(const ZipFileSystem &fs, const char *gamePath)
{
    ZipFile f;
    if (!fs.Open(gamePath, f))
        return {};
    std::vector<uint8_t> raw(f.size);
    platform::FileRead(f.archiveHandle, f.offset, raw.data(), f.size);
    if (raw.size() < 18 || !bIsCompressed(raw.data()))
        return raw;
    std::vector<uint8_t> data(bGetDecompressedSize(raw.data()));
    if (bDecompress(raw.data(), data.data()) == 0)
        return {};
    return data;
}

// CARS\<name>\GEOMETRY.BIN, or the GEOMETRY.LZC (JDLZ) that each car record also names.
static std::vector<uint8_t> ReadCarGeometry(const ZipFileSystem &fs, const std::string &car)
{
    std::vector<uint8_t> data = ReadGameFile(fs, ("CARS\\" + car + "\\GEOMETRY.BIN").c_str());
    return data.empty() ? ReadGameFile(fs, ("CARS\\" + car + "\\GEOMETRY.LZC").c_str()) : data;
}

// --dump GAME_PATH OUT: write a file from the disc archive, decompressed if needed.
static int DumpFile(const ZipFileSystem &fs, const char *gamePath, const char *outPath)
{
    ZipFile f;
    if (!fs.Open(gamePath, f)) {
        fprintf(stderr, "not found: %s\n", gamePath);
        return 1;
    }
    std::vector<uint8_t> raw(f.size);
    platform::FileRead(f.archiveHandle, f.offset, raw.data(), f.size);
    std::vector<uint8_t> data = raw;
    if (raw.size() >= 18 && bIsCompressed(raw.data())) {
        data.assign(bGetDecompressedSize(raw.data()), 0);
        uint32_t n = bDecompress(raw.data(), data.data());
        printf("%s: %zu bytes compressed -> %u bytes\n", gamePath, raw.size(), n);
        if (n == 0)
            return 1;
    }
    FILE *o = fopen(outPath, "wb");
    if (!o)
        return 1;
    fwrite(data.data(), 1, data.size(), o);
    fclose(o);
    return 0;
}

int main(int argc, char **argv)
{
    platform::InitMainMemoryPool(64 << 20); // bWare "Main Pool" (InitPlatform 0x0032c7c0 on PS2)
    const char *root = getenv("NFSU2_DATA");
    platform::SetDataRoot(root ? root : ".");

    ZipFileSystem *fs = MountDiscArchive();
    if (!fs) {
        fprintf(stderr, "No NFSUNDER\\ZDIR.BIN under %s (set NFSU2_DATA to your disc folder)\n",
                platform::DataRoot().c_str());
        return 1;
    }
    printf("Mounted disc archive: %zu entries\n", fs->EntryCount());
    int failures = SelfTest(*fs);
    printf("File system self-test: %s\n", failures ? "FAILED" : "passed");
    if (argc > 1 && strcmp(argv[1], "--test") == 0)
        return failures ? 1 : 0;
    if (argc > 3 && strcmp(argv[1], "--dump") == 0)
        return DumpFile(*fs, argv[2], argv[3]);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
    }
    // FEng screen space is 640x480; open at 2x.
    // The world renderer draws with OpenGL inside SDL's OpenGL render backend; it needs a depth
    // buffer (ISSUES.md #108). The FEng screens keep using the SDL_Renderer API.
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window *win = SDL_CreateWindow("Need for Speed Underground 2 (native)", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1280, 960, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);

    // Helper to render a single frame during synchronous boot loading screens.
    auto renderFrame = [&](FEngine *eng) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        eng->Render();
        SDL_RenderPresent(ren);
    };

    // Boot screen, as InitializeEverything does it (0x002df948, LoadGlobalAChunks + BootLoadingScreen):
    // GLOBAL\GLOBALA.BUN holds loading_boot.fng and its textures.
    std::vector<uint8_t> globalA = ReadGameFile(*fs, "GLOBAL\\GLOBALA.BUN");
    // Language data, as the language init (0x00262840) loads it: font sheets, then the fonts and
    // strings of the current language.
    std::vector<uint8_t> langTextures = ReadGameFile(*fs, "LANGUAGES\\LANGUAGETEXTURES.BIN");
    std::vector<uint8_t> english = ReadGameFile(*fs, "LANGUAGES\\ENGLISH.BIN");
    FENativeInterface fe(ren);
    fe.AddBundle(globalA.data(), globalA.size());
    fe.AddBundle(langTextures.data(), langTextures.size());
    fe.AddBundle(english.data(), english.size());
    FEngine *engine = new FEngine();
    fe.AttachEngine(engine);
    fe.SetFileReader([fs](const char *path) { return ReadGameFile(*fs, path); });

    std::vector<uint8_t> globalB, frontB;
    FEStartupSequence startup(engine, &fe, /*isPal=*/true);

    if (const char *package = getenv("NFSU2_PACKAGE")) {
        // Show loading_boot.fng with a live progress bar while decompressing GLOBALB.LZC & FRONTB.LZC.
        FEPackage *bootPkg = engine->PushPackage("loading_boot.fng", 0, 0xff);
        if (bootPkg) {
            FENativeInterface::SetVisible(bootPkg, 0x855f83ba, false);
            engine->Update(16, 0);
            renderFrame(engine);
        }
        globalB = ReadGameFile(*fs, "GLOBAL\\GLOBALB.LZC");
        fe.AddBundle(globalB.data(), globalB.size(), "GLOBAL\\GLOBALB.LZC");
        frontB = ReadGameFile(*fs, "FRONTEND\\FRONTB.LZC");
        fe.AddBundle(frontB.data(), frontB.size(), "FRONTEND\\FRONTB.LZC");
        if (bootPkg)
            engine->UnloadPackage(bootPkg);
        if (getenv("NFSU2_LIST_PACKAGES")) {
            for (const std::string &n : fe.PackageNames())
                printf("%s\n", n.c_str());
            fflush(stdout);
            _Exit(0);
        }
        if (!engine->PushPackage(package, 0, 0xff))
            fprintf(stderr, "could not push %s\n", package);
    } else {
        // 1. BootLoadingScreen (0x002e01f0): push loading_boot.fng, hide Legal_blurb (0x855f83ba) on PAL.
        FEPackage *bootPkg = engine->PushPackage("loading_boot.fng", 0, 0xff);
        if (bootPkg) {
            FENativeInterface::SetVisible(bootPkg, 0x855f83ba, false);
            engine->Update(16, 0);
            renderFrame(engine);
        }
        // 2. LoadGlobalChunks (0x002df9a0): load GLOBAL\GLOBALB.LZC while loading_boot.fng is shown.
        globalB = ReadGameFile(*fs, "GLOBAL\\GLOBALB.LZC");
        fe.AddBundle(globalB.data(), globalB.size(), "GLOBAL\\GLOBALB.LZC");
        // 3. EndBootLoadingScreen (0x002e0258): unload loading_boot.fng.
        if (bootPkg)
            engine->UnloadPackage(bootPkg);

        // 4. ChangeGameFlowState(3) -> LoadingFrontEnd (0x002dfea8): push Loading.fng while loading FRONTB.LZC.
        FEPackage *loadPkg = engine->PushPackage("Loading.fng", 0, 0xff);
        if (loadPkg) {
            engine->Update(16, 0);
            renderFrame(engine);
        }
        frontB = ReadGameFile(*fs, "FRONTEND\\FRONTB.LZC");
        fe.AddBundle(frontB.data(), frontB.size(), "FRONTEND\\FRONTB.LZC");
        if (getenv("NFSU2_LIST_PACKAGES")) {
            for (const std::string &n : fe.PackageNames())
                printf("%s\n", n.c_str());
            fflush(stdout);
            _Exit(0);
        }
        // 5. EndGameFlowLoadingFrontEnd (0x002dfff8): unload Loading.fng and start FEStartupSequence (0x00292280).
        if (loadPkg)
            engine->UnloadPackage(loadPkg);
        startup.Start();
    }

    Uint32 last = SDL_GetTicks();
    const Uint32 start = last;
    const char *pressMsEnv = getenv("NFSU2_PRESS_MS");
    const Uint32 pressMs = pressMsEnv ? Uint32(atoi(pressMsEnv)) : 0;
    const char *padMaskEnv = getenv("NFSU2_PAD_MASK");
    const u32 padMaskToInject = padMaskEnv ? u32(strtoul(padMaskEnv, nullptr, 0))
                                           : (FENativeInterface::kPadAccept | FENativeInterface::kPadStart);
    const char *maxFramesEnv = getenv("NFSU2_FRAMES");
    const int maxFrames = maxFramesEnv ? atoi(maxFramesEnv) : 0;
    int frameCount = 0;
    bool pressSent = false;
    struct ScriptedPress {
        Uint32 ms;
        u32 mask;
        bool sent = false;
    };
    std::vector<ScriptedPress> script;
    if (const char *in = getenv("NFSU2_INPUT")) {
        static const struct { const char *name; u32 mask; } kButtons[] = {
            {"up", FENativeInterface::kPadUp},         {"down", FENativeInterface::kPadDown},
            {"left", FENativeInterface::kPadLeft},     {"right", FENativeInterface::kPadRight},
            {"accept", FENativeInterface::kPadAccept}, {"back", FENativeInterface::kPadBack},
            {"start", FENativeInterface::kPadStart},   {"ltrigger", FENativeInterface::kPadLTrigger},
            {"rtrigger", FENativeInterface::kPadRTrigger}};
        for (const char *p = in; *p;) {
            char name[16] = {};
            unsigned ms = 0;
            int used = 0;
            if (sscanf(p, "%u:%15[a-z]%n", &ms, name, &used) != 2)
                break;
            for (auto &b : kButtons)
                if (strcmp(b.name, name) == 0)
                    script.push_back({ms, b.mask});
            p += used;
            if (*p == ',')
                ++p;
        }
    }

    // In-game world (stand-in for the PS2 game flow after StartRace, ISSUES.md #103/#104).
    TrackWorld world;
    TrackWorld::Camera camera;
    PlayerCar car;
    RaceRoute route;
    std::vector<PlayerCar> opponentCars;
    std::vector<AiDriver> ai;
    std::vector<bool> aiOut; // URL knockout (ISSUES #117): opponents eliminated this race
    AiDriver playerAi; // NFSU2_AUTODRIVE with a route
    std::vector<float> aiStartS;
    std::vector<uint8_t> inGameCommon, inGameRace, inGameDrift, inGameFreeRoam;
    bool inWorld = false, flyCamera = false;
    FEPackage *pausePkg = nullptr; // UI_Pause.fng while the race is paused (ISSUES.md #122)
    // Post-race results (UI_PostRaceResults.fng): opponents' finish times on a race clock.
    FEPackage *resultsPkg = nullptr;
    std::vector<std::string> oppNames;
    std::vector<float> aiFinish;
    float raceClock = 0, finishedFor = 0;
    std::string currentRegion;
    FEPackage *hud = nullptr;
    RaceSession race;
    u32 worldLastPad = 0;
    RaceSession::Config raceConfig;
    float chaseYaw = 0.0f, steer = 0.0f;
    const bool autoDrive = getenv("NFSU2_AUTODRIVE") != nullptr; // test aid: hold the throttle
    const bool dragAuto = getenv("NFSU2_DRAG_AUTO") != nullptr;  // test aid: hold throttle + auto-upshift
    std::function<void(const std::string &)> enterWorld;
    enterWorld = [&](const std::string &region) {
        currentRegion = region;
        pausePkg = nullptr;
        resultsPkg = nullptr;
        oppNames.clear();
        aiFinish.clear();
        raceClock = finishedFor = 0;
        // LoadingRegion (0x002ddb40): show Loading.fng while the region streams in.
        if (engine->PushPackage("Loading.fng", 0, 0xff)) {
            engine->Update(16, 0);
            renderFrame(engine);
        }
        // RegionLoader stage 1 (0x002ddbc0): the in-game bundles with the HUD packages.
        if (inGameCommon.empty()) {
            inGameCommon = ReadGameFile(*fs, "GLOBAL\\INGAMECOMMON.LZC");
            fe.AddBundle(inGameCommon.data(), inGameCommon.size(), "GLOBAL\\INGAMECOMMON.LZC");
            inGameRace = ReadGameFile(*fs, "GLOBAL\\INGAMERACE.BUN");
            fe.AddBundle(inGameRace.data(), inGameRace.size(), "GLOBAL\\INGAMERACE.BUN");
            // Drift events use their own bundle (HUD_Drift.fng), ISSUES #116.
            inGameDrift = ReadGameFile(*fs, "GLOBAL\\INGAMEDRIFT.BUN");
            if (!inGameDrift.empty())
                fe.AddBundle(inGameDrift.data(), inGameDrift.size(), "GLOBAL\\INGAMEDRIFT.BUN");
            // Free run / outrun use the free-roam bundle (HUD_SingleRace.fng), ISSUES #118.
            inGameFreeRoam = ReadGameFile(*fs, "GLOBAL\\INGAMEFREEROAM.BUN");
            if (!inGameFreeRoam.empty())
                fe.AddBundle(inGameFreeRoam.data(), inGameFreeRoam.size(), "GLOBAL\\INGAMEFREEROAM.BUN");
            if (getenv("NFSU2_LIST_INGAME"))
                for (const std::string &n : fe.PackageNames())
                    printf("[World] package %s\n", n.c_str());
        }
        std::string path = "TRACKS\\STREAM" + region + ".BUN";
        std::vector<uint8_t> stream = ReadGameFile(*fs, path.c_str());
        printf("[World] %s: %zu bytes\n", path.c_str(), stream.size());
        // Neighbouring regions of the same location: some of this region's scenery names solids
        // that only they contain (ISSUES.md #121). Read for the load only.
        std::vector<std::vector<uint8_t>> neighbourData;
        std::vector<const std::vector<uint8_t> *> neighbours;
        for (const char *other : {"L4RA", "L4RB", "L4RC", "L4RD"})
            if (region != other && region.compare(0, 2, other, 2) == 0) {
                neighbourData.push_back(ReadGameFile(*fs, (std::string("TRACKS\\STREAM") + other + ".BUN").c_str()));
                if (neighbourData.back().empty())
                    neighbourData.pop_back();
            }
        for (const auto &n : neighbourData)
            neighbours.push_back(&n);
        const bool loaded = !stream.empty() && world.Load(stream, neighbours);
        std::vector<std::vector<uint8_t>>().swap(neighbourData);
        if (!loaded) {
            fprintf(stderr, "[World] could not load %s\n", path.c_str());
            return;
        }
        camera = world.StartCamera();
        flyCamera = getenv("NFSU2_WORLD_FLY") != nullptr;
        int raceNumber = startup.RaceNumber();
        if (const char *n = getenv("NFSU2_RACE"))
            raceNumber = atoi(n);
        raceConfig = RaceSession::Config();
        raceConfig.laps = startup.RaceLaps();
        raceConfig.type = startup.RaceType();
        if (const char *t = getenv("NFSU2_RACETYPE"))
            raceConfig.type = atoi(t);
        for (const RaceInfo &r : fe.Races())
            if (r.number == raceNumber)
                raceConfig.trackLength = float(r.length);
        if (getenv("NFSU2_LIST_RACES"))
            for (const RaceInfo &r : fe.Races())
                printf("[RaceDB] %d %s cat %d flags %08x%s len %.0f\n", r.number, r.region, r.category,
                       r.typeFlags, r.special ? " special" : "", double(r.length));
        // Free run (type 9): roam the region with no race layout -- no route, no barriers, no
        // opponents, no countdown (ISSUES #118).
        const bool freeRoam = raceConfig.type == 9;
        // The race route (Tracks\Routes<region>\Paths<race>.bin, ISSUES.md #107).
        char pathsPath[64];
        snprintf(pathsPath, sizeof(pathsPath), "TRACKS\\ROUTES%s\\PATHS%d.BIN", region.c_str(), raceNumber);
        route = RaceRoute();
        if (freeRoam)
            raceNumber = 0;
        else if (raceNumber && route.Load(ReadGameFile(*fs, pathsPath)))
            raceConfig.route = &route;
        // Barriers the route runs through belong to other races' layouts; keep the rest (side
        // streets closed). Without a route every barrier stays. Free run opens the whole region.
        world.EnableBarriers([&](const float *bb) {
            if (freeRoam)
                return false;
            if (!route.Valid())
                return true;
            float cx = (bb[0] + bb[3]) * 0.5f, cy = (bb[1] + bb[4]) * 0.5f, x, y, dx, dy;
            route.Sample(route.Project(cx, cy), x, y, dx, dy);
            return std::hypot(cx - x, cy - y) > 6.0f;
        });
        // Start grid: of the race's grids, the one heading along the route (the other is for the
        // reverse direction); the player starts at the back, opponents ahead.
        std::string markersPath = "TRACKS\\ROUTES" + region + "\\TRACKPOSMARKERSALL.BIN";
        std::vector<uint8_t> markers = ReadGameFile(*fs, markersPath.c_str());
        std::vector<TrackWorld::Camera> grid;
        if (raceNumber) {
            float best = -2.0f;
            for (auto &g : world.GridsFromMarkers(markers, raceNumber)) {
                float align = 1.0f;
                if (route.Valid()) {
                    float x, y, dx, dy;
                    route.Sample(route.Project(g[0].x, g[0].y), x, y, dx, dy);
                    align = std::cos(g[0].yaw) * dx + std::sin(g[0].yaw) * dy;
                }
                if (align > best) {
                    best = align;
                    grid = g;
                }
            }
        }
        const int opponents = freeRoam || grid.empty() || flyCamera ? 0 : std::min<int>(raceConfig.type == 8 ? 1 : startup.RaceOpponents(), int(grid.size()) - 1);
        if (!grid.empty()) {
            camera = grid[std::min<size_t>(opponents, grid.size() - 1)];
            printf("[World] race %d: grid of %zu, %d opponents\n", raceNumber, grid.size(), opponents);
            raceConfig.dirX = std::cos(camera.yaw);
            raceConfig.dirY = std::sin(camera.yaw);
            raceConfig.hasLine = world.LineFromMarkers(markers, raceNumber, raceConfig.lineX, raceConfig.lineY);
            if (route.Valid()) {
                raceConfig.lineS = route.Project(raceConfig.hasLine ? raceConfig.lineX : grid[0].x,
                                                 raceConfig.hasLine ? raceConfig.lineY : grid[0].y);
                if (getenv("NFSU2_WORLD_DEBUG"))
                    printf("[World] line %s %.1f,%.1f lineS %.1f grid0 %.1f,%.1f s %.1f\n",
                           raceConfig.hasLine ? "marker" : "grid", raceConfig.lineX, raceConfig.lineY,
                           raceConfig.lineS, grid[0].x, grid[0].y, route.Project(grid[0].x, grid[0].y));
            }
            camera.z += 2.0f;
        }
        // The player car (stand-in for the unported car loader / physics, ISSUES.md #105).
        world.ClearCars();
        opponentCars.clear();
        ai.clear();
        aiStartS.clear();
        aiOut.clear();
        // Car textures: the shared pack and the player's (opponents add theirs below).
        world.AddTexturePack(ReadGameFile(*fs, "CARS\\TEXTURES.BIN"));
        world.AddTexturePack(ReadGameFile(*fs, ("CARS\\" + fe.PreviewCarName() + "\\TEXTURES.BIN").c_str()));
        PlayerCar::Wheel wheels[4];
        PlayerCar::Performance perf;
        car.SetPerformance(fe.CarPerformance(fe.PreviewCarName(), perf) ? perf : PlayerCar::Performance());
        if (!flyCamera && car.LoadModel(ReadCarGeometry(*fs, fe.PreviewCarName()),
                                        fe.CarWheels(fe.PreviewCarName(), wheels) ? wheels : nullptr)) {
            world.AddCar(&car.Model(), &car.TriangleTextures(), SDL_Color{40, 90, 200, 255});
            float ground = camera.z - 2.0f;
            world.GroundHeight(camera.x, camera.y, camera.z, ground);
            car.Place(camera.x, camera.y, ground, camera.yaw);
            if (getenv("NFSU2_WORLD_DEBUG"))
                for (float zMax : {16.0f, 18.0f, 18.5f, 19.0f, 19.5f, 20.0f, 21.0f, 25.0f}) {
                    float h = -1;
                    bool ok = world.GroundHeight(camera.x, camera.y, zMax, h);
                    printf("[World] ground under start, zMax %.1f: %s %.2f\n", zMax, ok ? "found" : "none", h);
                }
            chaseYaw = camera.yaw;
        } else {
            flyCamera = true;
        }
        // Opponents (ISSUES.md #107): other database cars on the grid slots ahead of the player.
        if (!flyCamera && route.Valid() && opponents > 0) {
            static const SDL_Color kPaints[] = {{200, 40, 40, 255}, {230, 200, 40, 255}, {40, 170, 70, 255},
                                                {220, 220, 225, 255}, {150, 60, 190, 255}};
            std::vector<std::string> pool = fe.RaceableCars();
            pool.erase(std::remove(pool.begin(), pool.end(), fe.PreviewCarName()), pool.end());
            opponentCars.resize(opponents);
            ai.resize(opponents);
            aiOut.assign(opponents, false);
            for (int i = 0; i < opponents; ++i) {
                const TrackWorld::Camera &slot = grid[i];
                // Some database cars have no files on the PAL disc (CIVIC, RSX): take the next one.
                std::string name = fe.PreviewCarName();
                bool loaded = false;
                for (size_t attempt = 0; attempt < pool.size() && !loaded; ++attempt) {
                    name = pool[(i * 7 + raceNumber + attempt) % pool.size()];
                    PlayerCar::Wheel oppWheels[4];
                    loaded = opponentCars[i].LoadModel(ReadCarGeometry(*fs, name),
                                                       fe.CarWheels(name, oppWheels) ? oppWheels : nullptr);
                }
                if (!loaded) {
                    name = fe.PreviewCarName();
                    opponentCars[i].SetModel(car);
                }
                PlayerCar::Performance oppPerf;
                opponentCars[i].SetPerformance(fe.CarPerformance(name, oppPerf) ? oppPerf : PlayerCar::Performance());
                float ground = slot.z;
                world.GroundHeight(slot.x, slot.y, slot.z + 2.0f, ground);
                opponentCars[i].Place(slot.x, slot.y, ground, slot.yaw);
                world.AddCar(&opponentCars[i].Model(), &opponentCars[i].TriangleTextures(), kPaints[i % 5]);
                world.AddTexturePack(ReadGameFile(*fs, ("CARS\\" + name + "\\TEXTURES.BIN").c_str()));
                // Lane: keep each car on its own side of the route line; faster cars at the front.
                float x, y, dx, dy;
                route.Sample(route.Project(slot.x, slot.y), x, y, dx, dy);
                float lane = std::clamp(-(slot.x - x) * dy + (slot.y - y) * dx, -4.5f, 4.5f);
                // NFSU2_AI_SKILL scales the opponents' pace (test aid: set the field slower so an
                // automated player can lead, e.g. to exercise the URL knockout).
                float skill = 0.86f + 0.04f * float(opponents - i) / opponents;
                if (const char *s = getenv("NFSU2_AI_SKILL"))
                    skill *= std::clamp(float(atof(s)), 0.1f, 2.0f);
                ai[i].Init(&opponentCars[i], &route, lane, skill);
                aiStartS.push_back(route.Project(slot.x, slot.y));
                printf("[World] opponent %d: %s\n", i + 1, name.c_str());
                oppNames.push_back(name);
                aiFinish.push_back(0.0f);
            }
        }
        raceConfig.racers = 1 + int(ai.size());
        if (const char *cam = getenv("NFSU2_WORLD_CAM"))
            sscanf(cam, "%f,%f,%f,%f,%f", &camera.x, &camera.y, &camera.z, &camera.yaw, &camera.pitch);
        printf("[World] camera %.1f %.1f %.1f yaw %.2f\n", camera.x, camera.y, camera.z, camera.yaw);
        // The front end stays loaded but is no longer ticked or drawn (UnloadingFrontEnd 0x002e0098
        // is not ported); the world view uses the full output resolution.
        // UnloadingFrontEnd (0x002e0098): drop the front-end screens, then show the race HUD.
        std::vector<FEPackage *> fePackages;
        for (FEPackage *p = engine->GetPackageList()->GetFirstPackage(); p; p = p->GetNext())
            fePackages.push_back(p);
        for (FEPackage *p : fePackages)
            engine->UnloadPackage(p);
        const char *hudName = getenv("NFSU2_HUD")
                                  ? getenv("NFSU2_HUD")
                                  : (raceConfig.type == 3 ? "HUD_Drift.fng" : "HUD_SingleRace.fng");
        hud = engine->PushPackage(hudName, 0, 0xff);
        if (!hud)
            hud = engine->PushPackage("HUD_SingleRace.fng", 0, 0xff);
        printf("[World] HUD %s\n", hud ? hudName : "missing");
        race.Begin(hud, raceConfig);
        // Dev aid: NFSU2_WORLD_PUSH=x.fng pushes another in-game package over the HUD (its tree is
        // dumped with NFSU2_FETREE instead of the HUD's).
        FEPackage *treeRoot = hud;
        if (const char *extra = getenv("NFSU2_WORLD_PUSH"))
            if (FEPackage *p = engine->PushPackage(extra, 0, 0xff))
                treeRoot = p;
        if (treeRoot && getenv("NFSU2_FETREE")) {
            // Dev aid: the object tree (stripped packages carry only name hashes).
            std::function<void(FEObject *, int, uint32_t)> dump = [&](FEObject *o, int depth, uint32_t parent) {
                for (; o; o = o->GetNext()) {
                    const FEObjData *d = o->GetObjData();
                    printf("[FETree] %*s%08x type %d parent %08x pos (%.0f,%.0f) size (%.1f,%.1f) flags %08x\n", depth * 2, "",
                           o->NameHash, int(o->Type), parent, d ? d->Pos.x : 0, d ? d->Pos.y : 0, d ? d->Size.x : 0,
                           d ? d->Size.y : 0, o->Flags);
                    if (o->Type == FE_Group)
                        dump(static_cast<FEGroup *>(o)->GetFirstChild(), depth + 1, o->NameHash);
                }
            };
            dump(treeRoot->GetFirstObject(), 0, 0);
        }
        inWorld = true;
    };
    if (getenv("NFSU2_CAR_TEST")) // dev aid: drivetrain figures of every Quick Race car
        for (const std::string &name : fe.RaceableCars()) {
            PlayerCar::Performance p;
            PlayerCar::Wheel w[4];
            if (!fe.CarPerformance(name, p))
                continue;
            float t100, vmax;
            PlayerCar::Simulate(p, fe.CarWheels(name, w) ? w[0].radius : 0.32f, t100, vmax);
            printf("[CarTest] %-12s %.2f t  %d gears  %4.0f/%4.0f rpm  peak %.0f Nm  0-100 %.1f s  top %.0f km/h\n",
                   name.c_str(), p.mass, p.numGears, p.redline, p.maxRpm,
                   1000 * *std::max_element(p.torque, p.torque + 9), t100, vmax * 3.6f);
        }
    if (const char *region = getenv("NFSU2_WORLD"))
        enterWorld(region);

    for (bool running = true; running;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                running = false;
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - start;

        bool releaseInjectedPadAfterUpdate = false;
        // Optional one-update input tap for testing screen transitions (NFSU2_PRESS_MS / NFSU2_PAD_MASK).
        if (pressMs > 0 && !pressSent && elapsed >= pressMs) {
            fe.SetSimulatedPadMask(padMaskToInject);
            pressSent = true;
            releaseInjectedPadAfterUpdate = true;
        }
        // NFSU2_INPUT="ms:button,..." emits one-update taps so a held press cannot cascade through screens.
        // Buttons: up down left right accept back start.
        for (ScriptedPress &p : script) {
            if (!p.sent && elapsed >= p.ms) {
                fe.SetSimulatedPadMask(p.mask);
                p.sent = true;
                releaseInjectedPadAfterUpdate = true;
            }
        }

        if (inWorld) {
            // Paused: the simulation stands still (zero time step) while the world and HUD draw.
            const Uint32 frameStart = last;
            const float dt = pausePkg ? 0.0f : std::min(0.1f, (now - last) / 1000.0f);
            const u32 pad = fe.GetJoyPadMask(0);
            const u32 pressed = pad & ~worldLastPad;
            worldLastPad = pad;
            last = now;
            // Start opens the pause menu (UI_Pause.fng); its choice comes back through FEStartup.
            FEStartupSequence::PauseAction pauseAction = startup.TakePauseAction();
            if (!pausePkg && race.GetPhase() != RaceSession::kFinished && (pressed & FENativeInterface::kPadStart)) {
                pausePkg = engine->PushPackage("UI_Pause.fng", 0, 0xff);
                printf("[Race] paused\n");
                if (getenv("NFSU2_WORLD_DEBUG"))
                    for (FEPackage *p = engine->GetPackageList()->GetFirstPackage(); p; p = p->GetNext())
                        printf("[Race]   package %s mask %x\n", p->GetName(), p->GetControlMask());
            } else if (pausePkg && pauseAction == FEStartupSequence::kPauseResume) {
                engine->UnloadPackage(pausePkg);
                pausePkg = nullptr;
                printf("[Race] resumed\n");
            } else if (pausePkg && pauseAction == FEStartupSequence::kPauseRestart) {
                printf("[Race] restart\n");
                engine->UnloadPackage(pausePkg);
                pausePkg = nullptr;
                if (hud)
                    engine->UnloadPackage(hud);
                hud = nullptr;
                std::string region = currentRegion;
                enterWorld(region);
                continue;
            }
            if (pausePkg)
                startup.Tick(elapsed); // screen ticks for the pause menu's icon scroller
            // Quit Race from the pause menu -- or Accept once the race is over.
            if ((pausePkg && pauseAction == FEStartupSequence::kPauseQuit) ||
                (race.GetPhase() == RaceSession::kFinished && (pressed & FENativeInterface::kPadAccept))) {
                if (pausePkg)
                    engine->UnloadPackage(pausePkg);
                pausePkg = nullptr;
                if (resultsPkg)
                    engine->UnloadPackage(resultsPkg);
                resultsPkg = nullptr;
                if (hud)
                    engine->UnloadPackage(hud);
                hud = nullptr;
                inWorld = false;
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                startup.ReturnFromRace();
                continue;
            }
            if (flyCamera) {
                // Fly camera: Up/Down move, Left/Right turn, L1/R1 lower/raise.
                const float speed = 40.0f * dt, turn = 1.6f * dt;
                if (pad & FENativeInterface::kPadLeft)
                    camera.yaw += turn;
                if (pad & FENativeInterface::kPadRight)
                    camera.yaw -= turn;
                float move = (pad & FENativeInterface::kPadUp) ? speed : (pad & FENativeInterface::kPadDown) ? -speed : 0.0f;
                camera.x += std::cos(camera.yaw) * move;
                camera.y += std::sin(camera.yaw) * move;
                if (pad & FENativeInterface::kPadLTrigger)
                    camera.z -= speed * 0.5f;
                if (pad & FENativeInterface::kPadRTrigger)
                    camera.z += speed * 0.5f;
            } else {
                // Drive: Up throttle, Down brake/reverse, Left/Right steer (eased for digital input).
                float target = (pad & FENativeInterface::kPadLeft) ? 1.0f : (pad & FENativeInterface::kPadRight) ? -1.0f : 0.0f;
                steer += std::clamp(target - steer, -4.0f * dt, 4.0f * dt);
                // inputThrottle also revs the drag gearbox during the countdown (the car still gets
                // no throttle until green, below).
                const float inputThrottle = (autoDrive || dragAuto || (pad & FENativeInterface::kPadUp)) ? 1.0f : 0.0f;
                float throttle = inputThrottle;
                float brake = (pad & FENativeInterface::kPadDown) ? 1.0f : 0.0f;
                std::vector<float> rivals;
                for (size_t i = 0; i < ai.size(); ++i)
                    rivals.push_back(race.RaceDistance(aiStartS[i], ai[i].Progress()));
                if (!race.Update(car, rivals, dt)) {
                    // Countdown, or race over: no throttle; brake to a stop after the finish
                    // (braking at rest would drive the car backwards).
                    throttle = 0;
                    brake = race.GetPhase() == RaceSession::kFinished && car.Speed() > 0.5f ? 1.0f : 0.0f;
                }
                // Finish times for the results screen: opponents crossing the finish distance, on a
                // clock that runs from the green light (and stops while paused).
                if (race.GetPhase() != RaceSession::kCountdown)
                    raceClock += dt;
                for (size_t i = 0; i < rivals.size() && i < aiFinish.size(); ++i)
                    if (aiFinish[i] <= 0 && rivals[i] >= race.RaceLength())
                        aiFinish[i] = raceClock;
                if (race.GetPhase() == RaceSession::kFinished && !resultsPkg && (finishedFor += dt) > 3.0f) {
                    std::vector<PostRaceRow> rows;
                    rows.push_back({"YOU", fe.CarDisplayName(fe.PreviewCarName()), race.TotalTime()});
                    const float length = race.RaceLength();
                    for (size_t i = 0; i < oppNames.size(); ++i) {
                        float t = aiFinish[i];
                        if (t <= 0 && i < aiOut.size() && aiOut[i])
                            t = -1; // knocked out
                        else if (t <= 0 && i < rivals.size()) // still racing: estimate from its pace
                            t = raceClock + (length - rivals[i]) / std::max(5.0f, rivals[i] / std::max(1.0f, raceClock));
                        char label[16];
                        snprintf(label, sizeof(label), "RIVAL %zu", i + 1);
                        rows.push_back({label, fe.CarDisplayName(oppNames[i]), t});
                    }
                    std::stable_sort(rows.begin(), rows.end(), [](const PostRaceRow &a, const PostRaceRow &b) {
                        return (a.time >= 0) != (b.time >= 0) ? a.time >= 0 : a.time < b.time;
                    });
                    resultsPkg = engine->PushPackage("UI_PostRaceResults.fng", 0, 0xff);
                    FillPostRaceResults(resultsPkg, rows);
                    printf("[Race] results:");
                    for (const PostRaceRow &r : rows)
                        printf("  %s %.2f", r.name.c_str(), r.time);
                    printf("\n");
                }
                // URL knockout (ISSUES #117): park any opponent eliminated at the last line.
                for (int k = race.TakeKnockout(); k >= 0; k = race.TakeKnockout())
                    if (k < int(aiOut.size()))
                        aiOut[k] = true;
                const bool go = race.GetPhase() != RaceSession::kCountdown;
                // Drag race (ISSUES #115): the gearbox drives the engine torque; R2/Accept upshift.
                float power = 1.0f;
                if (race.IsDrag() && race.GetPhase() != RaceSession::kFinished) {
                    bool shift = (pressed & (FENativeInterface::kPadRTrigger | FENativeInterface::kPadAccept)) != 0;
                    if (dragAuto && race.NeedShift())
                        shift = true;
                    power = race.DragUpdate(car.Speed(), inputThrottle, shift, dt);
                }
                if ((autoDrive || dragAuto) && route.Valid() && race.GetPhase() != RaceSession::kFinished) {
                    // Test aid: the player car follows the route like an opponent.
                    if (playerAi.Car() != &car)
                        playerAi.Init(&car, &route, 0.0f, 1.0f);
                    playerAi.SetPower(power);
                    playerAi.Update(world, dt, go && race.GetPhase() == RaceSession::kRacing);
                } else {
                    car.Update(world, throttle, brake, steer, dt, true, power);
                }
                // Drift race (ISSUES #116): score from the car's cornering, both for the player and
                // the route-following test aid.
                if (race.IsDrift())
                    race.DriftUpdate(car.Speed(), car.Yaw(), dt);
                for (size_t i = 0; i < ai.size(); ++i)
                    if (!aiOut[i])
                        ai[i].Update(world, dt, go);
                // Knocked-out opponents stop driving and roll to a halt (ISSUES #117).
                for (size_t i = 0; i < aiOut.size(); ++i)
                    if (aiOut[i] && opponentCars[i].Speed() > 0.0f)
                        opponentCars[i].ScaleSpeed(0.97f);
                // Car-to-car contact: push overlapping cars apart (2 m radius each); a car running
                // into the back of another loses a little speed.
                std::vector<PlayerCar *> cars{&car};
                for (PlayerCar &o : opponentCars)
                    cars.push_back(&o);
                for (size_t i = 0; i < cars.size(); ++i)
                    for (size_t j = i + 1; j < cars.size(); ++j) {
                        float dx = cars[j]->X() - cars[i]->X(), dy = cars[j]->Y() - cars[i]->Y();
                        float d = std::hypot(dx, dy);
                        if (d >= 4.0f || d < 1e-3f || std::fabs(cars[j]->Z() - cars[i]->Z()) > 2.0f)
                            continue;
                        float push = (4.0f - d) * 0.5f / d;
                        cars[i]->Nudge(-dx * push, -dy * push);
                        cars[j]->Nudge(dx * push, dy * push);
                        // The one behind (moving towards the other) slows to the other's pace.
                        float iTowards = std::cos(cars[i]->Yaw()) * dx + std::sin(cars[i]->Yaw()) * dy;
                        PlayerCar *behind = iTowards > 0 ? cars[i] : cars[j];
                        PlayerCar *ahead = behind == cars[i] ? cars[j] : cars[i];
                        if (behind->Speed() > ahead->Speed() + 1.0f)
                            behind->ScaleSpeed(0.98f);
                    }
                for (size_t i = 0; i < cars.size(); ++i) {
                    float m[12];
                    cars[i]->Transform(m);
                    world.SetCarTransform(i, m);
                }
                // Chase camera: behind and above, lagging the car's heading.
                float diff = std::remainder(car.Yaw() - chaseYaw, 6.2831853f);
                chaseYaw += diff * std::min(1.0f, 5.0f * dt);
                camera.x = car.X() - std::cos(chaseYaw) * 6.5f;
                camera.y = car.Y() - std::sin(chaseYaw) * 6.5f;
                float camGround = car.Z();
                world.GroundHeight(camera.x, camera.y, car.Z() + 2.0f, camGround);
                camera.z = std::max(car.Z(), camGround) + 2.3f;
                camera.yaw = chaseYaw;
                camera.pitch = -0.17f;
            }
            // FEng runs on real time even while the race is paused (the pause menu animates).
            engine->Update(static_cast<i32>(std::min<Uint32>(100, now - frameStart)), 0);
            if (releaseInjectedPadAfterUpdate) // scripted taps reach FEng (pause menu) too
                fe.SetSimulatedPadMask(0);
            // The world uses the full output resolution; FEng (HUD) its 640x480 screen space.
            SDL_RenderSetLogicalSize(ren, 0, 0);
            int w, h;
            SDL_GetRendererOutputSize(ren, &w, &h);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (!world.RenderGL(ren, camera, w, h))
                world.Render(ren, camera, w, h);
            SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            engine->Render();
            static Uint32 fpsStart = now, fpsFrames = 0;
            if (++fpsFrames == 60) {
                for (size_t i = 0; i < ai.size() && getenv("NFSU2_WORLD_DEBUG"); ++i)
                    printf("[World]   opp %zu at %.1f %.1f %.1f speed %.1f race %.1f m\n", i + 1, opponentCars[i].X(),
                           opponentCars[i].Y(), opponentCars[i].Z(), opponentCars[i].Speed(),
                           race.RaceDistance(aiStartS[i], ai[i].Progress()));
                printf("[World] %.1f fps  car %.1f %.1f %.1f speed %.1f m/s race %.1f m\n", 60000.0 / std::max<Uint32>(1, now - fpsStart),
                       car.X(), car.Y(), car.Z(), car.Speed(), race.PlayerDistance());
                if (race.IsDrag() && getenv("NFSU2_WORLD_DEBUG"))
                    printf("[Drag] gear %d rpm %.0f\n", race.Gear(), race.Rpm());
                if (race.IsDrift() && getenv("NFSU2_WORLD_DEBUG"))
                    printf("[Drift] angle %.0f mult %d chain %.0f total %.0f\n", race.DriftAngle(),
                           race.DriftMultiplier(), race.DriftPoints(), race.DriftTotal());
                fpsStart = now;
                fpsFrames = 0;
            }
        } else {
            startup.Tick(elapsed);
            engine->Update(static_cast<i32>(now - last), 0); // FEng ticks are milliseconds (cFEng::Service)
            if (releaseInjectedPadAfterUpdate)
                fe.SetSimulatedPadMask(0);
            last = now;

            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            engine->Render();
            fe.RenderCarPreviewOverlay();
            std::string region;
            if (startup.TakeRaceRequest(region))
                enterWorld(region);
        }

        // NFSU2_SCREENSHOT=file.bmp: save the frame after NFSU2_SCREENSHOT_MS (default 1500) and quit.
        if (const char *shot = getenv("NFSU2_SCREENSHOT")) {
            const char *ms = getenv("NFSU2_SCREENSHOT_MS");
            if (elapsed > Uint32(ms ? atoi(ms) : 1500)) {
                int w, h;
                SDL_GetRendererOutputSize(ren, &w, &h);
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
                SDL_SaveBMP(s, shot);
                SDL_FreeSurface(s);
                running = false;
            }
        }
        SDL_RenderPresent(ren);
        if (maxFrames > 0 && ++frameCount >= maxFrames)
            running = false;
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    fflush(stdout);
    // EA's static object pools are not safe to destroy at process exit (ISSUES.md #25).
    _Exit(0);
}
