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
#include "Speed/Indep/Src/FEng/FEString.h"
#include "game/indep/Frontend/FENative.h"
#include "game/indep/Frontend/FEStartup.h"
#include "game/indep/Career/Events.h"
#include "game/indep/Texture/TexturePack.h"
#include "game/indep/Frontend/PostRaceResults.h"
#include "game/indep/Sound/CarAudioTable.h"
#include "game/indep/Sound/EngineSound.h"
#include "game/indep/Sound/MusicStream.h"
#include "game/indep/Sound/UiSound.h"
#include "game/indep/World/AiDriver.h"
#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/RaceRoute.h"
#include "game/indep/World/Traffic.h"
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
        Career::EventTable::Get().Load(globalB.data(), globalB.size());
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
        FEPackage *pkg = engine->PushPackage(package, 0, 0xff);
        if (!pkg)
            fprintf(stderr, "could not push %s\n", package);
        else if (getenv("NFSU2_FETREE")) {
            std::function<void(FEObject *, int, uint32_t)> dump = [&](FEObject *o, int depth, uint32_t parent) {
                for (; o; o = o->GetNext()) {
                    const FEObjData *d = o->GetObjData();
                    char label[32] = "";
                    if (o->Type == FE_String)
                        snprintf(label, sizeof(label), " label %08x", static_cast<FEString *>(o)->GetLabelHash());
                    printf("[FETree] %*s%08x type %d parent %08x pos (%.0f,%.0f) size (%.1f,%.1f) flags %08x c=%u,%u,%u,%u %s%s\n", depth * 2, "",
                           o->NameHash, int(o->Type), parent, d ? d->Pos.x : 0, d ? d->Pos.y : 0, d ? d->Size.x : 0,
                           d ? d->Size.y : 0, o->Flags, d ? unsigned(d->Col.r) : 0u, d ? unsigned(d->Col.g) : 0u,
                           d ? unsigned(d->Col.b) : 0u, d ? unsigned(d->Col.a) : 0u, o->pName ? o->pName : "", label);
                    if (o->Type == FE_Group)
                        dump(static_cast<FEGroup *>(o)->GetFirstChild(), depth + 1, o->NameHash);
                }
            };
            dump(pkg->GetFirstObject(), 0, 0);
        }
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
        Career::EventTable::Get().Load(globalB.data(), globalB.size());
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

    // NFSU2_EVENTS=1: dump the career event table (0x34a11).
    if (getenv("NFSU2_EVENTS")) {
        const Career::EventTable &t = Career::EventTable::Get();
        printf("[Career] %zu events\n", t.Count());
        for (size_t i = 0; i < t.Count(); ++i) {
            const Career::Event &e = t.At(i);
            printf("[Career] %3zu %08x stage %3u region %u kind %u purse %5u respect %3u%s%s races", i,
                   e.id, unsigned(e.stage), unsigned(e.region), unsigned(e.kind), e.purse, e.respect,
                   e.special ? " special" : "", e.listed ? " listed" : "");
            for (int16_t r : e.races)
                printf(" %d", r);
            printf("\n");
        }
        fflush(stdout);
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
    // NFSU2_DRIVE="from-to:button,..." (ms since start; up down left right): buttons held for a window,
    // so a test can drive the car by hand (throttle, steer) instead of using NFSU2_AUTODRIVE.
    struct HeldPress {
        unsigned from, to;
        u32 mask;
    };
    std::vector<HeldPress> held;
    if (const char *in = getenv("NFSU2_DRIVE")) {
        for (const char *p = in; *p;) {
            unsigned a = 0, b = 0;
            char name[16] = {};
            int used = 0;
            if (sscanf(p, "%u-%u:%15[a-z]%n", &a, &b, name, &used) != 3)
                break;
            u32 m = !strcmp(name, "up") ? FENativeInterface::kPadUp : !strcmp(name, "down") ? FENativeInterface::kPadDown
                  : !strcmp(name, "left") ? FENativeInterface::kPadLeft : !strcmp(name, "right") ? FENativeInterface::kPadRight : 0;
            if (m)
                held.push_back({a, b, m});
            p += used;
            if (*p == ',')
                ++p;
        }
    }
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
    TrafficNet trafficNet;
    std::vector<PlayerCar> trafficCars;
    Traffic traffic;
    std::vector<bool> aiOut; // URL knockout (ISSUES #117): opponents eliminated this race
    AiDriver playerAi; // NFSU2_AUTODRIVE with a route
    std::vector<float> aiStartS;
    std::vector<uint8_t> inGameCommon, inGameRace, inGameDrift, inGameFreeRoam;
    bool inWorld = false, flyCamera = false;
    FEPackage *pausePkg = nullptr; // UI_Pause.fng while the race is paused (ISSUES.md #122)
    // Career crib (UI_CareerCrib.fng): the career hub over a career free-roam session (ISSUES #141).
    FEPackage *cribPkg = nullptr;
    // Post-race results (UI_PostRaceResults.fng): opponents' finish times on a race clock.
    FEPackage *resultsPkg = nullptr;
    std::vector<std::string> oppNames;
    std::vector<float> aiFinish;
    float raceClock = 0, finishedFor = 0;
    std::string currentRegion;
    std::string currentHudName; // HUD package of the current world session (pushed on leaving the crib)
    int worldRaceNumber = 0; // the race the world session is running (0 = free roam)
    FEPackage *hud = nullptr;
    RaceSession race;
    u32 worldLastPad = 0;
    RaceSession::Config raceConfig;
    float chaseYaw = 0.0f, steer = 0.0f;
    const bool autoDrive = getenv("NFSU2_AUTODRIVE") != nullptr; // test aid: hold the throttle
    const bool dragAuto = getenv("NFSU2_DRAG_AUTO") != nullptr;  // test aid: hold throttle + auto-upshift
    // Music (ISSUES.md #123): starts once the boot movies are over (first main menu, or a race
    // entered directly); NFSU2_MUSIC_VOLUME scales it (0 = off,
    // which also mutes the engine).
    MusicStream music;
    EngineSound engineSound; // player car (ISSUES.md #124)
    int lastGear = 1;        // for gear-change sounds
    const bool physTest = getenv("NFSU2_PHYS_TEST") != nullptr;
    float physTestTime = 0;
    bool musicStarted = false;
    const float musicVolume = getenv("NFSU2_MUSIC_VOLUME") ? float(atof(getenv("NFSU2_MUSIC_VOLUME"))) : 1.0f;
    UiSound uiSound; // front-end clicks (ISSUES.md #126)
    if (musicVolume > 0 && uiSound.Load(ReadGameFile(*fs, "SOUND\\GLOBAL\\FE_COMMON_MB.abk")))
        fe.SetSoundHook([&uiSound](uint32_t msg) { uiSound.OnMessage(msg); });
    std::function<void(const std::string &)> enterWorld;
    enterWorld = [&](const std::string &regionIn) {
        // NFSU2_RACE selects a race by its race-database number; the track's own region then wins over
        // the region the front end asked for (career enters the city, but a forced race lives in the
        // region its race record names, e.g. 4602 -> L4RF).
        std::string region = regionIn;
        if (const char *n = getenv("NFSU2_RACE")) {
            const int want = atoi(n);
            for (const RaceInfo &r : fe.Races())
                if (r.number == want && r.region[0])
                    region = r.region;
        }
        currentRegion = region;
        pausePkg = nullptr;
        cribPkg = nullptr;
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
        // L4RF and L4RG also carry a few city solids referenced by L4RA.
        for (const char *other : {"L4RA", "L4RB", "L4RC", "L4RD", "L4RF", "L4RG"})
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
        worldRaceNumber = raceNumber;
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
        int opponents = freeRoam || grid.empty() || flyCamera ? 0 : std::min<int>(raceConfig.type == 8 ? 1 : startup.RaceOpponents(), int(grid.size()) - 1);
        // NFSU2_OPPONENTS overrides the field size (test aid: 0 clears the track for an automated
        // player, e.g. to exercise the career purse without a rival blocking the route).
        if (const char *n = getenv("NFSU2_OPPONENTS"))
            opponents = grid.empty() ? 0 : std::clamp(atoi(n), 0, int(grid.size()) - 1);
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
        // Car audio banks by car type and engine upgrade level (stock = 0) from UG2's table (ISSUES #128).
        if (musicVolume > 0) {
            const CarAudio::Level &audio =
                CarAudio::kRowLevels[CarAudio::RowForCarType(fe.CarTypeIndex(fe.PreviewCarName()))][0];
            if (getenv("NFSU2_SOUND_DEBUG"))
                printf("[Engine] %s: %s, %s\n", fe.PreviewCarName().c_str(), CarAudio::kEngineBanks[audio.engine],
                       CarAudio::kGearBanks[audio.gearbox]);
            engineSound.Load(ReadGameFile(*fs, (std::string("SOUND\\ENGINE\\") + CarAudio::kEngineBanks[audio.engine]).c_str()));
            engineSound.LoadSkid(ReadGameFile(*fs, "SOUND\\SKIDS\\SKID_PAV_MB.abk"));
            engineSound.LoadRoad(ReadGameFile(*fs, "SOUND\\IG_GLOBAL\\ROADNOISE_00_MB.abk"));
            engineSound.LoadShift(ReadGameFile(*fs, (std::string("SOUND\\SHIFTING\\") + CarAudio::kGearBanks[audio.gearbox]).c_str()));
        }
        PlayerCar::Wheel wheels[4];
        PlayerCar::Performance perf;
        car.SetPerformance(fe.CarPerformance(fe.PreviewCarName(), perf) ? perf : PlayerCar::Performance());
        // The ported UG2 vehicle physics (ISSUES #129) drive the player car; NFSU2_PHYSICS=arcade
        // selects the old stand-in model.
        if (const char *ph = getenv("NFSU2_PHYSICS"); !ph || strcmp(ph, "arcade")) {
            const uint8_t *rec = fe.CarRecordData(fe.PreviewCarName());
            printf("[Physics] UG2 vehicle model: %s\n", rec && car.UseUG2Physics(rec) ? "on" : "unavailable");
        }
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
                if (const char *ap = getenv("NFSU2_AI_PHYSICS"); ap && !strcmp(ap, "ug2"))
                    if (const uint8_t *rec = fe.CarRecordData(name))
                        opponentCars[i].UseUG2Physics(rec);
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
        // Ambient traffic (ISSUES #130): service vehicles on the free-roam lane links.
        trafficCars.clear();
        traffic = Traffic();
        if (!flyCamera && !getenv("NFSU2_NOTRAFFIC")) {
            static const int kCount[] = {0, 5, 10, 16};
            int want = kCount[std::clamp(startup.RaceTraffic(), 0, 3)];
            if (const char *t = getenv("NFSU2_TRAFFIC"))
                want = atoi(t);
            snprintf(pathsPath, sizeof(pathsPath), "TRACKS\\ROUTES%s\\PATHSFREEROAM.BIN", region.c_str());
            std::vector<std::string> pool = fe.TrafficCars();
            if (want > 0 && !pool.empty() && trafficNet.Load(ReadGameFile(*fs, pathsPath))) {
                static const SDL_Color kTrafficPaints[] = {{180, 180, 190, 255}, {30, 30, 40, 255}, {170, 30, 30, 255},
                                                           {200, 190, 120, 255}, {40, 70, 150, 255}};
                trafficCars.resize(size_t(want));
                std::vector<size_t> firstOf(pool.size(), size_t(-1));
                for (size_t i = 0; i < trafficCars.size(); ++i) {
                    size_t k = i % pool.size();
                    if (firstOf[k] == size_t(-1)) {
                        PlayerCar::Wheel w[4];
                        if (!trafficCars[i].LoadModel(ReadCarGeometry(*fs, pool[k]), fe.CarWheels(pool[k], w) ? w : nullptr))
                            continue;
                        firstOf[k] = i;
                        world.AddTexturePack(ReadGameFile(*fs, ("CARS\\" + pool[k] + "\\TEXTURES.BIN").c_str()));
                    } else {
                        trafficCars[i].SetModel(trafficCars[firstOf[k]]);
                    }
                    PlayerCar::Performance perf;
                    trafficCars[i].SetPerformance(fe.CarPerformance(pool[k], perf) ? perf : PlayerCar::Performance());
                }
                // Cars whose model failed to load stay empty; they are still driven but invisible.
                for (PlayerCar &c : trafficCars)
                    world.AddCar(&c.Model(), &c.TriangleTextures(), kTrafficPaints[(&c - trafficCars.data()) % 5]);
                traffic.Init(&trafficNet, &trafficCars, 0x2a5eu + unsigned(raceNumber));
                if (getenv("NFSU2_WORLD_DEBUG"))
                    for (const std::string &n : pool)
                        printf("[World]   traffic model %s: geometry %zu bytes\n", n.c_str(), ReadCarGeometry(*fs, n).size());
                printf("[World] traffic: %zu cars, %zu lane links\n", trafficCars.size(), trafficNet.Links().size());
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
        // The front end asks for the crib with the session: a career entry (Resume / new career) and
        // the return from a career event open in it, as UG2 does (0x00265970 / 0x0025e388 enter the
        // world and show UI_CareerCrib.fng over the crib scenery). The crib is a menu screen, so the
        // race HUD is pushed when the player returns to explore mode (below) instead.
        const bool cribSession = startup.TakeCribRequest();
        currentHudName = hudName;
        if (!cribSession) {
            hud = engine->PushPackage(hudName, 0, 0xff);
            if (!hud)
                hud = engine->PushPackage("HUD_SingleRace.fng", 0, 0xff);
            printf("[World] HUD %s\n", hud ? hudName : "missing");
        } else {
            hud = nullptr;
        }
        race.Begin(hud, raceConfig);
        if (cribSession) {
            cribPkg = engine->PushPackage("UI_CareerCrib.fng", 0, 0xff);
            printf("[Career] crib %s\n", cribPkg ? "shown" : "missing");
        }
        // Dev aid: NFSU2_ROUTE_CHECK=1 lists stretches of the race route (and +-2.5 m beside it) with no ground.
        if (getenv("NFSU2_ROUTE_CHECK") && route.Valid()) {
            float runStart = -1, runLast = 0;
            int runs = 0;
            for (float d = 0; d <= route.Length(); d += 1.0f) {
                float x, y, dx, dy, z;
                route.Sample(d, x, y, dx, dy);
                bool missing = false;
                for (float off : {-2.5f, 0.0f, 2.5f})
                    missing |= !world.GroundHeight(x - dy * off, y + dx * off, 80.0f, z);
                if (missing) {
                    if (runStart < 0)
                        runStart = d;
                    runLast = d;
                } else if (runStart >= 0) {
                    float rx, ry;
                    route.Sample(runStart, rx, ry, dx, dy);
                    printf("[RouteCheck] no ground s=%.0f..%.0f (%.0f m) at %.1f %.1f\n", runStart, runLast, runLast - runStart + 1, rx, ry);
                    ++runs;
                    runStart = -1;
                }
            }
            printf("[RouteCheck] %d gaps over %.0f m\n", runs, route.Length());
            // Compare ground heights of the render triangles and the collision mesh along the route.
            if (getenv("NFSU2_COLL_COMPARE")) {
                size_t both = 0, onlyR = 0, onlyC = 0, close = 0;
                float worst = 0;
                for (float d = 0; d <= route.Length(); d += 1.0f) {
                    float x, y, dx, dy, zr, zc;
                    route.Sample(d, x, y, dx, dy);
                    world.SetGroundSource(1);
                    bool r = world.GroundHeight(x, y, 80.0f, zr);
                    world.SetGroundSource(2);
                    bool c = world.GroundHeight(x, y, 80.0f, zc);
                    world.SetGroundSource(0);
                    if (r && c) {
                        ++both;
                        close += std::fabs(zr - zc) < 0.3f;
                        if (std::fabs(zr - zc) >= 0.3f && int(d) % 40 == 0)
                            printf("[RouteCheck]   s=%.0f at %.1f %.1f render %.2f coll %.2f\n", d, x, y, zr, zc);
                        worst = std::max(worst, std::fabs(zr - zc));
                    } else if (r)
                        ++onlyR;
                    else if (c)
                        ++onlyC;
                }
                printf("[RouteCheck] compare: both %zu (within 0.3 m: %zu, worst %.2f), render only %zu, collision only %zu\n",
                       both, close, worst, onlyR, onlyC);
            }
        }
        // Dev aid: NFSU2_GROUND_MAP="x,y,half,step" prints the ground height (m, '.' = none) on a grid.
        if (const char *gm = getenv("NFSU2_GROUND_MAP")) {
            float cx, cy, half, step;
            if (sscanf(gm, "%f,%f,%f,%f", &cx, &cy, &half, &step) == 4) {
                for (float y = cy + half; y >= cy - half; y -= step) {
                    printf("[Ground] y%7.1f:", y);
                    for (float x = cx - half; x <= cx + half; x += step) {
                        float z;
                        if (world.GroundHeight(x, y, 60.0f, z))
                            printf(" %4.0f", z);
                        else
                            printf("    .");
                    }
                    printf("\n");
                }
            }
        }
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
                    char label[32] = "";
                    if (o->Type == FE_String)
                        snprintf(label, sizeof(label), " label %08x", static_cast<FEString *>(o)->GetLabelHash());
                    printf("[FETree] %*s%08x type %d parent %08x pos (%.0f,%.0f) size (%.1f,%.1f) flags %08x c=%u,%u,%u,%u %s%s\n", depth * 2, "",
                           o->NameHash, int(o->Type), parent, d ? d->Pos.x : 0, d ? d->Pos.y : 0, d ? d->Size.x : 0,
                           d ? d->Size.y : 0, o->Flags, d ? unsigned(d->Col.r) : 0u, d ? unsigned(d->Col.g) : 0u,
                           d ? unsigned(d->Col.b) : 0u, d ? unsigned(d->Col.a) : 0u, o->pName ? o->pName : "", label);
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
        if (!musicStarted && musicVolume > 0 && (inWorld || startup.IsFinished())) {
            musicStarted = true;
            if (music.Open(*fs, "SOUND\\PFDATA\\MUSICSFX.MUS"))
                music.SetVolume(musicVolume);
        }
        music.SetPaused(pausePkg != nullptr);
        music.Update();
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
            // Paused: the simulation stands still (zero time step) while the world and HUD draw. The
            // career crib (ISSUES #141) also holds the simulation until the player returns to explore.
            const Uint32 frameStart = last;
            const float dt = (pausePkg || cribPkg) ? 0.0f : std::min(0.1f, (now - last) / 1000.0f);
            u32 pad = fe.GetJoyPadMask(0);
            for (const HeldPress &h : held)
                if (elapsed >= h.from && elapsed < h.to)
                    pad |= h.mask;
            const u32 pressed = pad & ~worldLastPad;
            worldLastPad = pad;
            last = now;
            // Start opens the pause menu (UI_Pause.fng); its choice comes back through FEStartup.
            FEStartupSequence::PauseAction pauseAction = startup.TakePauseAction();
            if (cribPkg && pauseAction == FEStartupSequence::kPauseResume) {
                engine->UnloadPackage(cribPkg);
                cribPkg = nullptr;
                // Explore mode needs the race HUD (the crib session was started without one).
                hud = engine->PushPackage(currentHudName.c_str(), 0, 0xff);
                if (!hud)
                    hud = engine->PushPackage("HUD_SingleRace.fng", 0, 0xff);
                race.Begin(hud, raceConfig); // free roam restarts with no countdown
                printf("[Career] explore mode (HUD %s)\n", hud ? currentHudName.c_str() : "missing");
            }
            if (!pausePkg && !cribPkg && race.GetPhase() != RaceSession::kFinished &&
                (pressed & FENativeInterface::kPadStart)) {
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
            // The crib and the world map swap in place (World Map opens from the crib, Back returns).
            if (cribPkg) {
                if (const char *menu = startup.TakeCareerMenuSwitch()) {
                    engine->UnloadPackage(cribPkg);
                    cribPkg = engine->PushPackage(menu, 0, 0xff);
                    printf("[Career] menu %s %s\n", menu, cribPkg ? "shown" : "missing");
                }
                // The world map started a career event: leave the crib session for the event's region.
                std::string eventRegion;
                if (startup.TakeRaceRequest(eventRegion)) {
                    engine->UnloadPackage(cribPkg);
                    cribPkg = nullptr;
                    if (hud)
                        engine->UnloadPackage(hud);
                    hud = nullptr;
                    enterWorld(eventRegion);
                    continue;
                }
            }
            if (pausePkg || cribPkg)
                startup.Tick(elapsed); // screen ticks for the pause menu / crib icon scroller
            // Quit Race from the pause menu (or Back to Main Menu from the crib) -- or Accept once the
            // race is over.
            if (((pausePkg || cribPkg) && pauseAction == FEStartupSequence::kPauseQuit) ||
                (race.GetPhase() == RaceSession::kFinished && (pressed & FENativeInterface::kPadAccept))) {
                if (pausePkg)
                    engine->UnloadPackage(pausePkg);
                pausePkg = nullptr;
                if (cribPkg)
                    engine->UnloadPackage(cribPkg);
                cribPkg = nullptr;
                if (resultsPkg)
                    engine->UnloadPackage(resultsPkg);
                resultsPkg = nullptr;
                if (hud)
                    engine->UnloadPackage(hud);
                hud = nullptr;
                inWorld = false;
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                engineSound.Set(0, 0, 0, 0, false);
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
                car.SetHold(race.GetPhase() == RaceSession::kCountdown);
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
                    // A career race pays the 0x34a11 event's purse/respect (ISSUES #140).
                    int place = 0;
                    for (size_t i = 0; i < rows.size(); ++i)
                        if (rows[i].name == "YOU")
                            place = int(i) + 1;
                    startup.AwardCareerResult(worldRaceNumber, place);
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
                engineSound.Set(car.Rpm(), car.GetPerformance().idleRpm, car.GetPerformance().redline,
                                std::max(throttle, inputThrottle), !pausePkg);
                engineSound.SetMotion(car.Speed(), car.Slip());
                if (car.Gear() != lastGear) {
                    if (lastGear > 0 && car.Gear() > 0)
                        engineSound.Shift(car.Gear() - lastGear);
                    lastGear = car.Gear();
                }
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
                } else if (physTest && go) {
                    // NFSU2_PHYS_TEST: straight-line full throttle for 12 s, then full braking.
                    physTestTime += dt;
                    const bool accel = physTestTime < 12.0f;
                    car.Update(world, accel ? 1.0f : 0.0f, accel ? 0.0f : 1.0f, 0.0f, dt, true, power);
                } else {
                    car.Update(world, throttle, brake, steer, dt, true, power);
                }
                // Drift race (ISSUES #116): score from the car's cornering, both for the player and
                // the route-following test aid.
                if (race.IsDrift())
                    race.DriftUpdate(car.Speed(), car.Yaw(), dt);
                for (size_t i = 0; i < ai.size(); ++i)
                    opponentCars[i].SetHold(!go);
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
                if (traffic.Size()) {
                    std::vector<const PlayerCar *> avoid(cars.begin(), cars.end());
                    traffic.Update(world, dt, avoid);
                }
                for (PlayerCar &t : trafficCars)
                    cars.push_back(&t);
                if (getenv("NFSU2_WORLD_DEBUG") && !trafficCars.empty())
                {
                    static int dbgFrame = 0;
                    if (++dbgFrame % 15 == 0) {
                        float nd = 1e9f;
                        for (PlayerCar &t : trafficCars)
                            nd = std::min(nd, std::hypot(t.X() - car.X(), t.Y() - car.Y()));
                        printf("[World]   player %.1f %.1f yaw %.2f speed %.1f nearest traffic %.1f m\n", car.X(), car.Y(),
                               car.Yaw(), car.Speed(), nd);
                    }
                }
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
                        if (behind->Speed() > ahead->Speed() + 1.0f) {
                            // Traffic (ISSUES #130) is solid: a hit sheds real speed and shoves the
                            // other car; racers only rub (0.98).
                            const size_t firstTraffic = 1 + opponentCars.size();
                            if (i >= firstTraffic || j >= firstTraffic) {
                                float closing = behind->Speed() - ahead->Speed();
                                behind->ScaleSpeed(std::max(0.7f, 1.0f - 0.03f * closing));
                                ahead->ScaleSpeed(1.0f + std::min(0.15f, 0.01f * closing));
                            } else
                                behind->ScaleSpeed(0.98f);
                        }
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
                printf("[World] %.1f fps  car %.1f %.1f %.1f speed %.1f m/s race %.1f m slip %.2f\n", 60000.0 / std::max<Uint32>(1, now - fpsStart),
                       car.X(), car.Y(), car.Z(), car.Speed(), race.PlayerDistance(), car.Slip());
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
            startup.PreRender(); // after the FEng script update, before the frame is drawn
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
