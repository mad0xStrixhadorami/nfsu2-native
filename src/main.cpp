// Native entry point. Mirrors the boot order of the PS2 main() at 0x002dbf00:
//   0x00442a88 global ctors (handled by the C++ runtime here)
//   0x002dbd88 system init(argc, argv)  -> todo
//   0x002dbef8                          -> todo
//   0x002e2fc8 startup path selection   -> todo
//   0x002dc298 main loop (run on the scratchpad stack on PS2) -> todo
// Currently: mounts the disc archive, then shows the real boot screen (loading_boot.fng) through
// the reused FEng engine and the native renderer.

#include <unistd.h>
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <algorithm>
#include <sys/stat.h>
#include <memory>
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
#include "game/indep/Frontend/PerfModel.h"
#include "game/indep/Frontend/TuneSystem.h"
#include "game/indep/Career/Events.h"
#include "game/indep/Career/CarLot.h"
#include "game/indep/Career/Hotspots.h"
#include "game/indep/Frontend/Magazines.h"
#include "game/indep/Frontend/ModernUI.h"
#include "game/indep/Mem.h"
#include "game/indep/Perf.h"
#include "game/indep/World/TimeOfDay.h"
#include "game/indep/Career/Sms.h"
#include "game/indep/Career/Sponsors.h"
#include "game/indep/Texture/TexturePack.h"
#include "game/indep/Frontend/RimCatalog.h"
#include "game/indep/Frontend/VinylCatalog.h"
#include "game/indep/Frontend/PostRaceResults.h"
#include "game/indep/Sound/CarAudioTable.h"
#include "game/indep/Sound/EngineSound.h"
#include "game/indep/Sound/MusicStream.h"
#include "game/indep/Sound/UiSound.h"
#include "game/indep/World/AiDriver.h"
#include "game/indep/World/Pursuit.h"
#include "game/indep/Sound/SirenSound.h"
#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/CarCollision.h"
#include "game/indep/World/CameraRig.h"
#include "game/indep/World/ModCars.h"
#include "game/indep/World/IntroCamera.h"
#include "game/indep/World/Minimap.h"
#include "game/indep/World/RaceRoute.h"
#include "game/indep/World/Traffic.h"
#include "game/indep/World/RaceSession.h"
#include "game/indep/World/TrackWorld.h"
#include "platform/linux/PlatformMemory.h"
#include "game/indep/World/CarPartAnim.h"
#include "platform/linux/PlatformFile.h"

static std::vector<uint8_t> ReadGameFile(const ZipFileSystem &fs, const char *gamePath);

// The performance model on the real disc data (ISSUES #200): the stock Skyline record and the packages
// must give the figures recorded there (peak torque 299 Nm stock, rising with parts, mass falling).
static int PerfModelDiscTest(const ZipFileSystem &fs)
{
    std::vector<uint8_t> globalB = ReadGameFile(fs, "GLOBAL\\GLOBALB.LZC");
    if (globalB.empty())
        return 1;
    const uint8_t *cars = nullptr;
    size_t carsSize = 0;
    PerfPackages packages;
    for (size_t off = 0; off + 8 <= globalB.size();) {
        uint32_t id, size;
        memcpy(&id, &globalB[off], 4);
        memcpy(&size, &globalB[off + 4], 4);
        if (off + 8 + size > globalB.size())
            break;
        if (id == 0x34600) {
            cars = &globalB[off + 8];
            carsSize = size;
        } else if (id == 0x80034a10) {
            packages.Parse(&globalB[off + 8], size);
        }
        off += 8 + size;
    }
    int failures = 0;
    auto check = [&](bool ok, const char *what) {
        printf("  %-24s %s\n", what, ok ? "ok" : "FAIL");
        failures += !ok;
    };
    check(packages.Count() == 30, "(30 perf packages)");
    const uint8_t *skyline = nullptr;
    for (size_t k = 0; cars && (k + 1) * PerfModel::kRecordSize <= carsSize; ++k)
        if (strcmp(reinterpret_cast<const char *>(cars + k * PerfModel::kRecordSize + 8), "SKYLINE") == 0)
            skyline = cars + k * PerfModel::kRecordSize;
    check(skyline != nullptr, "(Skyline record)");
    if (!skyline)
        return failures;
    auto peak = [](const std::vector<uint8_t> &rec) {
        PlayerCar::Performance p;
        FENativeInterface::ReadCarPerformance(rec.data(), p);
        float best = 0;
        for (float t : p.torque)
            best = std::max(best, t);
        return best * 1000.0f; // N*m
    };
    std::vector<uint8_t> stock(skyline, skyline + PerfModel::kRecordSize);
    PerfModel::Apply(stock.data(), stock.size(), PerfPackages(), {});
    check(std::fabs(peak(stock) - 299.0f) < 1.5f, "(stock Skyline 299 Nm)");
    std::vector<uint16_t> level3;
    for (size_t i = 0; i < packages.Count(); ++i)
        if (packages.At(i).level == 3)
            for (const PerfPart &part : packages.At(i).parts)
                level3.push_back(uint16_t(part.id));
    std::vector<uint8_t> tuned(skyline, skyline + PerfModel::kRecordSize);
    PerfModel::Apply(tuned.data(), tuned.size(), packages, level3);
    check(std::fabs(peak(tuned) - 409.0f) < 2.0f, "(level 3 Skyline 409 Nm)");
    float mass0, mass1;
    memcpy(&mass0, stock.data() + 0x228, 4);
    memcpy(&mass1, tuned.data() + 0x228, 4);
    check(mass1 < mass0, "(parts lighten the car)");
    // The tuning engine on the real data (ISSUES #207/#210/#211).
    Tune::Table tuneTable;
    for (size_t off = 0; off + 8 <= globalB.size();) {
        uint32_t id, size;
        memcpy(&id, &globalB[off], 4);
        memcpy(&size, &globalB[off + 4], 4);
        if (off + 8 + size > globalB.size())
            break;
        if (id == 0x80034a10)
            tuneTable.Parse(&globalB[off + 8], size);
        off += 8 + size;
    }
    check(tuneTable.Count() == 62, "(62 tuning entries)");
    {
        std::vector<uint16_t> parts = level3;
        std::vector<uint8_t> frame(tuned.begin() + PerfModel::kFrameOffset,
                                   tuned.begin() + PerfModel::kFrameOffset + PerfModel::kFrameSize);
        Tune::Session session(tuneTable, skyline, frame, parts);
        for (int f = 0; f < Tune::kFields; ++f) {
            if (!session.FieldActive(f))
                continue;
            float bl, bh, sl, sh, dl, dh;
            session.Bounds(f, 0, true, bl, bh);
            session.SliderRange(f, 0, sl, sh);
            session.DisplayRange(f, 0, dl, dh);
            if (getenv("NFSU2_TUNE_DEBUG"))
                printf("  [tune] field %2d bounds %.4f..%.4f slider %.4f..%.4f display %.2f..%.2f default shows %.3f\n", f,
                       bl, bh, sl, sh, dl, dh, session.Display(f, 0));
            check(sl <= sh && bl <= bh, "(tune ranges ordered)");
            if (getenv("NFSU2_TUNE_DEBUG") && (f == 0x17 || f == 0x18)) {
                float lo2, hi2;
                printf("  [tune]   curve %d:", f);
                for (int i = 0; i < 9; ++i) {
                    session.SliderRangeDisplay(f, i, lo2, hi2);
                    printf(" %.0f[%.0f..%.0f]", session.Display(f, i), lo2, hi2);
                }
                printf("\n");
                session.SetDisplay(f, 4, session.Display(f, 4) + 20.0f);
                printf("  [tune]   after +20 at point 4:");
                float sum = 0;
                for (int i = 0; i < 9; ++i) {
                    printf(" %.0f", session.Display(f, i));
                    sum += session.Display(f, i);
                }
                printf("  (sum %.0f)\n", sum);
                session.ResetAll();
            }
            if (getenv("NFSU2_TUNE_DEBUG") && (f == 3 || f == 0 || f == 10 || f == 11 || f == 16 || f == 17)) {
                float lo2, hi2;
                session.SliderRangeDisplay(f, 0, lo2, hi2);
                printf("  [tune]   field %d display slider %.3f..%.3f step %.4f\n", f, lo2, hi2, session.Step(f));
                for (int i = 0; i < 4; ++i) {
                    session.SetDisplay(f, 0, session.Display(f, 0) + session.Step(f));
                    printf("  [tune]   +step -> byte %d shows %.3f\n", session.Byte(f, 0), session.Display(f, 0));
                }
                session.Reset(f, 0);
            }
        }
    }
    return failures;
}

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
    return failures + !missingOk + PerfModelDiscTest(fs);
}

// Read a whole file from the disc archive, decompressing it if it is compressed.
static std::vector<uint8_t> ReadGameFile(const ZipFileSystem &fs, const char *gamePath)
{
    if (ModCars::IsModPath(gamePath)) // cars that are not on the disc (ISSUES #295): generated model, the donor's textures
        return ModCars::Read(gamePath, [&fs](const char *p) { return ReadGameFile(fs, p); });
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
    if (argc > 3 && strcmp(argv[1], "--texpack") == 0) { // dev aid: --texpack <game file path> <out dir>: every texture of a 0xb3300000 pack as BMP
        std::vector<uint8_t> data = ReadGameFile(*fs, argv[2]);
        TexturePack pack;
        if (data.empty() || !pack.Parse(data.data(), data.size())) {
            fprintf(stderr, "no texture pack in %s\n", argv[2]);
            return 1;
        }
        std::string dir = argv[3];
        mkdir(dir.c_str(), 0755);
        for (size_t i = 0; i < pack.Count(); ++i) {
            DecodedTexture t;
            if (!pack.Decode(pack.At(i), t))
                continue;
            if (SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(t.rgba.data(), t.width, t.height, 32, t.width * 4, SDL_PIXELFORMAT_ABGR8888)) {
                char name[600];
                snprintf(name, sizeof name, "%s/%03zu_%08x_%dx%d.bmp", dir.c_str(), i, pack.At(i).nameHash, t.width, t.height);
                SDL_SaveBMP(surf, name);
                SDL_FreeSurface(surf);
            }
            printf("%03zu %08x %-40s %dx%d bpp %d\n", i, pack.At(i).nameHash, pack.At(i).name.c_str(), pack.At(i).width, pack.At(i).height, pack.At(i).bitsPerPixel);
        }
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--export-shikun") == 0) // dev aid: --export-shikun <base> [floors W D seed]  (ISSUES #306)
        return TrackWorld::ExportShikun(argv[2], argc > 3 ? atoi(argv[3]) : 5, argc > 4 ? float(atof(argv[4])) : 24.0f,
                                        argc > 5 ? float(atof(argv[5])) : 11.0f, argc > 6 ? unsigned(atoi(argv[6])) : 3u) ? 0 : 1;
    if (argc > 3 && strcmp(argv[1], "--dump") == 0)
        return DumpFile(*fs, argv[2], argv[3]);
    if (argc > 2 && strcmp(argv[1], "--solids") == 0) { // dev aid: --solids <GEOMETRY.BIN> [name-prefix]
        FILE *f = fopen(argv[2], "rb");
        if (!f)
            return 1;
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
            data.insert(data.end(), buf, buf + n);
        fclose(f);
        const std::vector<SolidMesh::NamedSolid> solids = SolidMesh::DecodeNamedSolids(data.data(), data.size());
        for (const SolidMesh::NamedSolid &s : solids) {
            if (argc > 3 && s.name.compare(0, strlen(argv[3]), argv[3]) != 0)
                continue;
            printf("%-34s tris %5zu tex %zu bbox (%.3f %.3f %.3f)-(%.3f %.3f %.3f) mk %zu\n", s.name.c_str(), s.tris.size(),
                   s.textures.size(), s.bboxMin[0], s.bboxMin[1], s.bboxMin[2], s.bboxMax[0], s.bboxMax[1], s.bboxMax[2],
                   s.markers.size());
        }
        printf("%zu solids\n", solids.size());
        return 0;
    }
    if (argc > 3 && strcmp(argv[1], "--vinyls") == 0) { // dev aid: --vinyls <VINYLS.BIN> <CAR> lists the shop catalogue
        FILE *f = fopen(argv[2], "rb");
        if (!f)
            return 1;
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
            data.insert(data.end(), buf, buf + n);
        fclose(f);
        TexturePack pack;
        VinylCatalog catalog;
        if (!pack.Parse(data.data(), data.size()) || !catalog.Build(pack, argv[3])) {
            printf("no vinyl catalogue\n");
            return 1;
        }
        for (int g : catalog.UsedGroups()) {
            const auto items = catalog.InGroup(g);
            printf("%-14s %3zu:", VinylCatalog::Groups()[size_t(g)].debug, items.size());
            for (size_t i = 0; i < items.size() && i < 6; ++i)
                printf(" %s", items[i]->name.c_str());
            printf("\n");
        }
        printf("%zu designs\n", catalog.Designs().size());
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--tpk") == 0) { // dev aid: list the textures of an extracted pack file
        FILE *f = fopen(argv[2], "rb");
        if (!f)
            return 1;
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
            data.insert(data.end(), buf, buf + n);
        fclose(f);
        TexturePack pack;
        if (!pack.Parse(data.data(), data.size())) {
            printf("not a texture pack\n");
            return 1;
        }
        if (argc > 3) { // --tpk <file> <hash-hex> <out.ppm>: decode one texture
            const uint32_t want = uint32_t(strtoul(argv[3], nullptr, 16));
            for (size_t i = 0; i < pack.Count(); ++i)
                if (pack.At(i).nameHash == want) {
                    DecodedTexture img;
                    const bool ok = pack.Decode(pack.At(i), img);
                    printf("%s %dx%d decode %d\n", pack.At(i).name.c_str(), img.width, img.height, ok);
                    if (ok && argc > 4)
                        if (FILE *o = fopen(argv[4], "wb")) {
                            fprintf(o, "P6\n%d %d\n255\n", img.width, img.height);
                            for (uint32_t px : img.rgba) {
                                const unsigned a = (px >> 24) & 0xff;
                                const unsigned char rgb[3] = {uint8_t((px & 0xff) * a / 255), uint8_t(((px >> 8) & 0xff) * a / 255),
                                                              uint8_t(((px >> 16) & 0xff) * a / 255)};
                                fwrite(rgb, 1, 3, o);
                            }
                            fclose(o);
                        }
                    return ok ? 0 : 1;
                }
            return 1;
        }
        for (size_t i = 0; i < pack.Count(); ++i) {
            const TextureInfo &t = pack.At(i);
            printf("%4zu %08x %-32s %ux%u %u-bit pal %u data %u\n", i, t.nameHash, t.name.c_str(), t.width, t.height,
                   t.bitsPerPixel, t.paletteSize, t.dataSize);
        }
        return 0;
    }

    Sky::Init(); // time of day, before anything can draw: NFSU2_TIME / NFSU2_TIME_PRESET / NFSU2_TIME_SPEED (ISSUES #333)
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
    // 4x multisampling for the world's edges (NFSU2_MSAA=0 turns it off, =2 / =8 pick another count; the GL renderer
    // falls back to no multisampling when the driver cannot provide the buffer).
    const int msaa = getenv("NFSU2_MSAA") ? atoi(getenv("NFSU2_MSAA")) : 4;
    if (msaa > 1) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa);
    }
    SDL_Window *win = SDL_CreateWindow("Need for Speed Underground 2 (native)", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE); // 16:9; FEng keeps its 4:3 box (pillarboxed, never stretched)
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
    // Language strings and fonts: NFSU2_LANGUAGE=FRENCH (or its index) / the saved option; English when missing.
    const std::string languageName = FEStartupSequence::SelectedLanguage();
    std::vector<uint8_t> english = ReadGameFile(*fs, ("LANGUAGES\\" + languageName + ".BIN").c_str());
    if (english.empty() && languageName != "ENGLISH")
        english = ReadGameFile(*fs, "LANGUAGES\\ENGLISH.BIN");
    printf("[Lang] language file %s\n", languageName.c_str());
    FENativeInterface fe(ren);
    ModernUI modernUi; // ISSUES #276: NFSU2_CLASSIC_UI=1 keeps the original look
    if (!getenv("NFSU2_CLASSIC_UI"))
        modernUi.Init(ren);
    // The loading screen (ISSUES #301): ModernUI::DrawLoading over the whole window. The overall bar is monotonic: each load phase owns a
    // slice [loadBase, loadBase + loadSpan] and reports its own 0..1 progress inside it.
    const Uint32 loadStart = SDL_GetTicks();
    // Transitions (ISSUES #311): a captured menu frame is wiped away at the start of a load, and the finished loading frame is wiped away over the
    // game when it starts; a load that begins without a capture fades in from black.
    bool transActive = false, transFromCapture = false, revealCaptured = false, pendingReveal = false;
    Uint32 transStart = 0, transLast = 0, lastLoadingDraw = 0, sessionStart = 0;
    float transAccum = 0.0f;
    constexpr float kTransSeconds = 0.9f;
    const bool noTransitions = getenv("NFSU2_NO_TRANSITIONS") != nullptr;
    auto transitionDraw = [&](int tw, int th) { // logical size must be reset by the caller
        if (!transActive)
            return;
        // progress advances by at most 33 ms per drawn frame, so a hitch (the first game frames stall) cannot swallow the wipe
        const Uint32 nowT = SDL_GetTicks();
        if (transStart == transLast)
            transAccum = 0.0f;
        transAccum += std::min(float(nowT - transLast) * 0.001f, 0.033f);
        transLast = nowT;
        const float t = transAccum / kTransSeconds;
        static int transFrames = 0;
        ++transFrames;
        if (t >= 1.0f) {
            transActive = false;
            if (getenv("NFSU2_TRANS_DEBUG"))
                printf("[Transition] done after %d frames, %u ms\n", transFrames, nowT - transStart);
            transFrames = 0;
        } else {
            modernUi.DrawWipe(tw, th, t);
        }
    };
    float loadBase = 0.0f, loadSpan = 1.0f, loadShown = 0.0f;
    const char *loadStage = nullptr;
    auto loadingFrame = [&](const char *label, float f) -> bool {
        if (!modernUi.Ready() || getenv("NFSU2_CLASSIC_LOADING"))
            return false;
        loadShown = std::max(loadShown, loadBase + loadSpan * std::clamp(f, 0.0f, 1.0f));
        const char *stage = loadStage;
        if (!stage && label) {
            static const struct { const char *key, *text; } kNames[] = {
                {"GLOBALB", "Loading game data"},       {"FRONTB", "Loading menus"},           {"PERFORMANCETUNING", "Loading the tuning shop"},
                {"INGAMECOMMON", "Loading the race interface"}, {"INGAME", "Loading the race interface"}, {"STREAM", "Streaming the city"},
            };
            for (const auto &n : kNames)
                if (strstr(label, n.key)) {
                    stage = n.text;
                    break;
                }
        }
        SDL_RenderSetLogicalSize(ren, 0, 0);
        int lw, lh;
        SDL_GetRendererOutputSize(ren, &lw, &lh);
        const Uint32 nowMs = SDL_GetTicks();
        if (nowMs - lastLoadingDraw > 1500) { // a new load begins
            sessionStart = nowMs;
            revealCaptured = false;
            transStart = nowMs, transLast = nowMs;
            transActive = transFromCapture; // wipe the captured menu frame away, else fade in from black
            transFromCapture = false;
        }
        lastLoadingDraw = nowMs;
        const bool ok = modernUi.DrawLoading(lw, lh, stage ? stage : "Loading", loadShown, float(nowMs - sessionStart) * 0.001f);
        if (ok && !noTransitions && loadShown >= 0.999f && !revealCaptured) { // the finished loading frame is kept to wipe away over the game
            modernUi.CaptureFrame();
            revealCaptured = true;
            pendingReveal = true;
        }
        if (ok && !noTransitions) {
            if (transActive)
                transitionDraw(lw, lh);
            else if (nowMs - sessionStart < 450)
                modernUi.DrawFade(lw, lh, 1.0f - float(nowMs - sessionStart) / 450.0f);
        }
        // NFSU2_LOADING_SHOT=<prefix>: save the loading screen as <prefix>_<percent>.bmp at 10 % steps (dev aid)
        if (ok)
            if (const char *shot = getenv("NFSU2_LOADING_SHOT")) {
                static int lastStep = -1;
                const int step = int(loadShown * 10.0f);
                if (step != lastStep) {
                    lastStep = step;
                    if (SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, lw, lh, 32, SDL_PIXELFORMAT_ARGB8888)) {
                        SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                        char name[512];
                        snprintf(name, sizeof(name), "%s_%02d.bmp", shot, step * 10);
                        SDL_SaveBMP(surf, name);
                        SDL_FreeSurface(surf);
                    }
                }
            }
        if (ok)
            SDL_RenderPresent(ren);
        SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
        return ok;
    };
    fe.SetLoadingHook(loadingFrame);
    fe.AddBundle(globalA.data(), globalA.size());
    fe.AddBundle(langTextures.data(), langTextures.size());
    fe.AddBundle(english.data(), english.size());
    if (const char *lang = getenv("NFSU2_LANG")) {
        // Dev aid: resolve comma-separated language label hashes (hex) to their text.
        std::string list(lang);
        size_t pos = 0;
        while (pos < list.size()) {
            size_t comma = list.find(',', pos);
            std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!item.empty()) {
                uint32_t h = uint32_t(strtoul(item.c_str(), nullptr, 16));
                printf("[Lang] %08x = %s\n", h, fe.LanguageText(h).c_str());
            }
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
        fflush(stdout);
    }
    FEngine *engine = new FEngine();
    fe.AttachEngine(engine);
    SDL_StartTextInput();
    fe.SetFileReader([fs](const char *path) { return ReadGameFile(*fs, path); });

    std::vector<uint8_t> globalB, frontB, perfTuningB;
    FEStartupSequence startup(engine, &fe, /*isPal=*/true);

    if (const char *package = getenv("NFSU2_PACKAGE")) {
        // Show loading_boot.fng with a live progress bar while decompressing GLOBALB.LZC & FRONTB.LZC.
        FEPackage *bootPkg = engine->PushPackage("loading_boot.fng", 0, 0xff);
        if (bootPkg) {
            FENativeInterface::SetVisible(bootPkg, 0x855f83ba, false);
            engine->Update(16, 0);
            if (!loadingFrame("GLOBALB", 0.0f))
                renderFrame(engine);
        }
        loadBase = 0.0f, loadSpan = 0.45f;
        globalB = ReadGameFile(*fs, "GLOBAL\\GLOBALB.LZC");
        fe.AddBundle(globalB.data(), globalB.size(), "GLOBAL\\GLOBALB.LZC");
        Career::EventTable::Get().Load(globalB.data(), globalB.size());
        Career::SponsorTable::Get().Load(globalB.data(), globalB.size());
        Career::SmsTable::Get().Load(globalB.data(), globalB.size());
        Career::CarLotTable::Get().Load(globalB.data(), globalB.size());
        Career::HotspotTable::Get().LoadShops(globalB.data(), globalB.size());
        startup.LoadBootProfile();
        loadBase = 0.45f, loadSpan = 0.45f;
        frontB = ReadGameFile(*fs, "FRONTEND\\FRONTB.LZC");
        fe.AddBundle(frontB.data(), frontB.size(), "FRONTEND\\FRONTB.LZC");
        // The tuning / dyno screens (UI_PerformanceTuning_*, UI_PerformanceDyno_*) ship in the in-game
        // bundle the dyno scene loads (ISSUES #203); the port keeps them resident with the front end.
        loadBase = 0.9f, loadSpan = 0.1f;
        perfTuningB = ReadGameFile(*fs, "GLOBAL\\INGAMEPERFORMANCETUNING.BUN");
        if (!perfTuningB.empty())
            fe.AddBundle(perfTuningB.data(), perfTuningB.size(), "GLOBAL\\INGAMEPERFORMANCETUNING.BUN");
        loadingFrame("", 1.0f);
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
        if (pkg && getenv("NFSU2_FE_SHOWALL")) {
            // Dev aid: reveal every object in a directly pushed package (no screen handler).
            std::function<void(FEObject *)> show = [&](FEObject *o) {
                for (; o; o = o->GetNext()) {
                    if (FEObjData *d = o->GetObjData())
                        d->Col.a = 255;
                    o->Flags &= ~1u;
                    o->Flags |= 0x400000u;
                    if (o->Type == FE_Group)
                        show(static_cast<FEGroup *>(o)->GetFirstChild());
                }
            };
            show(pkg->GetFirstObject());
        }
        if (pkg && getenv("NFSU2_FETREE")) {
            std::function<void(FEObject *, int, uint32_t)> dump = [&](FEObject *o, int depth, uint32_t parent) {
                for (; o; o = o->GetNext()) {
                    const FEObjData *d = o->GetObjData();
                    char label[64] = "";
                    if (o->Type == FE_String)
                        snprintf(label, sizeof(label), " label %08x", static_cast<FEString *>(o)->GetLabelHash());
                    else if (o->Type == FE_Image)
                        snprintf(label, sizeof(label), " tex %08x", o->Handle);
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
            if (!loadingFrame("GLOBALB", 0.0f))
                renderFrame(engine);
        }
        // 2. LoadGlobalChunks (0x002df9a0): load GLOBAL\GLOBALB.LZC while loading_boot.fng is shown.
        loadBase = 0.0f, loadSpan = 0.45f;
        globalB = ReadGameFile(*fs, "GLOBAL\\GLOBALB.LZC");
        fe.AddBundle(globalB.data(), globalB.size(), "GLOBAL\\GLOBALB.LZC");
        Career::EventTable::Get().Load(globalB.data(), globalB.size());
        Career::SponsorTable::Get().Load(globalB.data(), globalB.size());
        Career::SmsTable::Get().Load(globalB.data(), globalB.size());
        Career::CarLotTable::Get().Load(globalB.data(), globalB.size());
        Career::HotspotTable::Get().LoadShops(globalB.data(), globalB.size());
        startup.LoadBootProfile();
        // 3. EndBootLoadingScreen (0x002e0258): unload loading_boot.fng.
        if (bootPkg)
            engine->UnloadPackage(bootPkg);

        // 4. ChangeGameFlowState(3) -> LoadingFrontEnd (0x002dfea8): push Loading.fng while loading FRONTB.LZC.
        FEPackage *loadPkg = engine->PushPackage("Loading.fng", 0, 0xff);
        if (loadPkg) {
            engine->Update(16, 0);
            if (!loadingFrame("FRONTB", 0.0f))
                renderFrame(engine);
        }
        loadBase = 0.45f, loadSpan = 0.45f;
        frontB = ReadGameFile(*fs, "FRONTEND\\FRONTB.LZC");
        fe.AddBundle(frontB.data(), frontB.size(), "FRONTEND\\FRONTB.LZC");
        // The tuning / dyno screens (UI_PerformanceTuning_*, UI_PerformanceDyno_*) ship in the in-game
        // bundle the dyno scene loads (ISSUES #203); the port keeps them resident with the front end.
        loadBase = 0.9f, loadSpan = 0.1f;
        perfTuningB = ReadGameFile(*fs, "GLOBAL\\INGAMEPERFORMANCETUNING.BUN");
        if (!perfTuningB.empty())
            fe.AddBundle(perfTuningB.data(), perfTuningB.size(), "GLOBAL\\INGAMEPERFORMANCETUNING.BUN");
        loadingFrame("", 1.0f);
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

    // NFSU2_SPONSORS=1: dump the sponsor records and each area's offers (ISSUES #165).
    if (getenv("NFSU2_SPONSORS")) {
        const Career::SponsorTable &t = Career::SponsorTable::Get();
        for (int a = 0; a < 6; ++a) {
            const Career::SponsorArea *area = t.Area(a);
            if (!area)
                continue;
            printf("[Sponsors] area %d: %d sponsors\n", a, area->count);
            for (int i = 0; i < area->count; ++i)
                if (const Career::Sponsor *s = t.Find(area->hashes[size_t(i)]))
                    printf("[Sponsors]   %-16s threshold %d bonus %d race purse %d rating needed %d\n", s->name.c_str(),
                           area->thresholds[size_t(i)], s->signingBonus, s->racePurse, s->extra);
        }
        fflush(stdout);
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
            for (size_t r = 0; r < e.races.size(); ++r)
                printf(" %d/%02x", e.races[r], r < e.raceFlags.size() ? e.raceFlags[r] : 0);
            printf("\n");
            if (getenv("NFSU2_EVENTS_RAW")) {
                printf("[Raw] %3zu", i);
                for (int b = 0; b < 0x88; ++b)
                    printf("%s%02x", b % 4 == 0 ? " " : "", e.raw[b]);
                printf("\n");
            }
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
        bool menuRelative = false; // "m<ms>:button": ms after the main menu is reached (the intro length varies)
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
                  : !strcmp(name, "left") ? FENativeInterface::kPadLeft : !strcmp(name, "right") ? FENativeInterface::kPadRight
                  : !strcmp(name, "hand") ? FENativeInterface::kPadButton0 : 0; // hand = the handbrake (X / Left Shift)
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
            {"rtrigger", FENativeInterface::kPadRTrigger}, {"square", FENativeInterface::kPadButton0}};
        for (const char *p = in; *p;) {
            char name[16] = {};
            unsigned ms = 0;
            int used = 0;
            const bool rel = *p == 'm';
            if (rel)
                ++p;
            if (sscanf(p, "%u:%15[a-z]%n", &ms, name, &used) != 2)
                break;
            for (auto &b : kButtons)
                if (strcmp(b.name, name) == 0)
                    script.push_back({ms, b.mask, false, rel});
            p += used;
            if (*p == ',')
                ++p;
        }
    }

    // In-game world (stand-in for the PS2 game flow after StartRace, ISSUES.md #103/#104).
    TrackWorld world;
    TrackWorld::Camera camera;
    PlayerCar car;

    // The menu showroom stage (ISSUES #304): the preview car on a dark studio floor, drawn with the race's car shader.
    std::unique_ptr<TrackWorld> stageWorld;
    std::string stageName;
    std::vector<std::string> stagePacks;
    size_t stageSig = 0;
    size_t stageCarIdx = size_t(-1);
    CarPartAnims stageAnims;
    std::string stageAnimsCar;
    float stageFlourishT0 = -100.0f;
    float stageDoors = 0.0f; // seconds into the doors' opening clip (the showroom's flourish, ISSUES #364)
    float stageSwitch = -10.0f; // seconds (frontend clock) of the last car change, for the fade-in
    // The magazine covers (ISSUES #309), loaded on first use.
    Magazines magazines;
    bool magazinesTried = false;
    auto magazinesReady = [&]() {
        if (!magazinesTried) {
            magazinesTried = true;
            magazines.Load(ReadGameFile(*fs, "FRONTEND\\MAGAZINES_FRONTEND.BIN"), ReadGameFile(*fs, "FRONTEND\\MAGAZINES_SHOWCASE.BIN"),
                           ReadGameFile(*fs, "FRONTEND\\MAGAZINES_STREAMING_HIGH.BIN"), ReadGameFile(*fs, "FRONTEND\\MAGAZINES_STREAMING_LOW.BIN"));
        }
        return magazines.Ready();
    };
    // Builds / refreshes the stage car (the front end's assembled preview car); false when there is none.
    auto syncStageCar = [&](float seconds) -> bool {
        FENativeInterface::StageCar sc;
        if (!fe.PreviewStageCar(sc))
            return false;
        if (!stageWorld)
            stageWorld = std::make_unique<TrackWorld>();
        // a signature of everything the car is built from: rebuild the stage car when any of it changes
        auto mix = [](size_t h, size_t v) { return h * 1000003u ^ (v + 0x9e3779b9u + (h << 6) + (h >> 2)); };
        auto col = [&](size_t h, SDL_Color c) { return mix(h, size_t(c.r) | size_t(c.g) << 8 | size_t(c.b) << 16); };
        size_t sig = mix(reinterpret_cast<size_t>(sc.model->data()), sc.model->size());
        for (SDL_Color c : {sc.body, sc.rim, sc.caliper, sc.mirror, sc.spoiler, sc.roof, sc.engine, sc.exhaust})
            sig = col(sig, c);
        // the vinyl livery (ISSUES #352): the layers' designs and colours are part of what the stage car is built from
        const std::vector<CarSkin::Layer> stageVinyls = fe.PreviewVinyls().empty() ? fe.TestVinyls() : fe.PreviewVinyls();
        for (const CarSkin::Layer &l : stageVinyls) {
            sig = mix(sig, std::hash<std::string>()(l.design));
            for (auto &c4 : l.color)
                for (uint8_t v : c4)
                    sig = mix(sig, v);
        }
        // the doors swing open for a moment when the showroom shows (the original's door curve, #358) and close again
        if (stageAnimsCar != sc.name) {
            stageAnimsCar = sc.name;
            stageAnims.Load(ReadGameFile(*fs, ("CARS\\" + sc.name + "\\PARTS_ANIMATIONS.BIN").c_str()));
        }
        float stageAng[5] = {0, 0, 0, 0, 0};
        if (const CarPartAnims::Clip *dl = stageAnims.Find("LEFT_DOOR_REG"))
            if (const CarPartAnims::Clip *dr = stageAnims.Find("RIGHT_DOOR_REG")) {
                const float since = seconds - stageFlourishT0; // seconds since the menu (or this car) appeared
                const bool open = since > 0.7f && since < 4.2f && !getenv("NFSU2_NO_STAGE_DOORS");
                static float lastSec = 0.0f;
                const float dts = std::clamp(seconds - lastSec, 0.0f, 0.1f);
                lastSec = seconds;
                stageDoors = std::clamp(stageDoors + (open ? 1.0f : -1.0f) * dts * 0.7f, 0.0f, dl->Duration());
                stageAng[3] = std::fabs(dl->At(stageDoors)), stageAng[4] = std::fabs(dr->At(stageDoors));
            }
        sig = mix(sig, size_t(stageAng[3] * 90.0f));
        if (sig != stageSig || sc.name != stageName) {
            stageWorld->ClearCars();
            for (const char *shared : {"CARS\\TEXTURES.BIN", "CARS\\WHEELS\\TEXTURES.BIN"})
                if (std::find(stagePacks.begin(), stagePacks.end(), shared) == stagePacks.end()) {
                    stageWorld->AddTexturePack(ReadGameFile(*fs, shared));
                    stagePacks.push_back(shared);
                }
            const std::string carPack = "CARS\\" + sc.name + "\\TEXTURES.BIN";
            if (std::find(stagePacks.begin(), stagePacks.end(), carPack) == stagePacks.end()) {
                stageWorld->AddTexturePack(ReadGameFile(*fs, carPack.c_str()));
                stagePacks.push_back(carPack);
            }
            const size_t idx = stageWorld->AddCar(sc.model, sc.slots, sc.body, sc.rims, sc.rim, sc.calipers, sc.caliper, sc.mirrors, sc.mirror,
                                                  sc.spoilers, sc.spoiler, sc.roofs, sc.roof, sc.engines, sc.engine, sc.exhausts, sc.exhaust);
            // stand the car on z = 0, centred on the origin, nose to +X
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const SolidMesh::Triangle &t : *sc.model)
                for (const SolidMesh::Point &pt : t.v) {
                    const float c3[3] = {pt.x, pt.y, pt.z};
                    for (int a = 0; a < 3; ++a)
                        lo[a] = std::min(lo[a], c3[a]), hi[a] = std::max(hi[a], c3[a]);
                }
            const float m[12] = {1, 0, 0, -(lo[0] + hi[0]) * 0.5f, 0, 1, 0, -(lo[1] + hi[1]) * 0.5f, 0, 0, 1, -lo[2]};
            if (!stageVinyls.empty()) { // the livery goes on before the transform builds the car's triangles (ISSUES #352)
                std::vector<uint32_t> skin;
                const bool shop = fe.EnsureVinylShop();
                const bool composed = shop && fe.ComposeVinylSkin(sc.body, stageVinyls, skin);
                const uint32_t slot = CarSkin::DetectBodySlot(*sc.model, *sc.slots, sc.name);
                if (getenv("NFSU2_STAGE_DEBUG"))
                    printf("[Stage] vinyl: %zu layers, shop %d, composed %d, body slot %08x\n", stageVinyls.size(), int(shop), int(composed), slot);
                if (composed)
                    stageWorld->SetCarSkin(idx, slot, std::move(skin));
            }
            stageCarIdx = idx;
            if (sc.parts) {
                float hinge[5][3];
                memcpy(hinge, sc.hinge, sizeof(hinge));
                for (int pi : {1, 3, 4})
                    stageAnims.Hinge(pi, hinge[pi]);
                stageWorld->SetCarParts(idx, sc.parts, hinge);
            }
            stageWorld->SetCarPartAngles(idx, stageAng);
            stageWorld->SetCarTransform(idx, m);
            if (sc.name != stageName)
                stageSwitch = seconds, stageFlourishT0 = seconds;
            stageName = sc.name, stageSig = sig;
            printf("[Stage] car %s on the showroom stage\n", sc.name.c_str());
        }
        return true;
    };
    // The icon-carousel screens (Career, mode select, options, customize ...) as the flat native list (ISSUES #361); true when one was drawn.
    bool nativeListVertical = false; // the list just drawn is a carousel (Up / Down map to Left / Right); settings lists are not
    auto drawNativeList = [&](int lw, int lh, float tsec) -> bool {
        nativeListVertical = false;
        fe.SetHideCarLabel(false);
        if (!modernUi.Ready() || getenv("NFSU2_CLASSIC_MENU") || startup.IsMessageDialogOpen())
            return false;
        {
            ModernUI::CarSelect cs;
            int gSlot = 0, gCount = 0;
            const bool garagePick = startup.GarageCarSelectState(gSlot, gCount);
            if ((fe.PreviewScreenOpen() || garagePick) && (garagePick || startup.CarSelectState(cs.kicker, cs.title, cs.customize))) { // the car select (ISSUES #369, #376)
                if (garagePick)
                    cs.kicker = "GARAGE", cs.title = "YOUR CARS", cs.customize = false;
                const std::string folder = fe.PreviewCarName();
                cs.make = fe.CarManufacturer(folder);
                std::string full = fe.CarDisplayName(folder);
                cs.model = !cs.make.empty() && full.compare(0, cs.make.size(), cs.make) == 0 && full.size() > cs.make.size() + 1 ? full.substr(cs.make.size() + 1) : folder;
                cs.group = garagePick ? "" : "STOCK CAR";
                for (int k = 0; k < 3; ++k)
                    cs.stats[k] = fe.StatValues()[k];
                cs.index = int(fe.PreviewCarIndex()), cs.count = int(fe.CarRosterSize());
                if (garagePick) {
                    cs.index = gSlot, cs.count = gCount;
                    char grp[24];
                    snprintf(grp, sizeof(grp), "GARAGE SLOT %03d", gSlot + 1);
                    cs.group = grp;
                }
                size_t ms = 0;
                int mon = -1, rep = -1;
                startup.MainMenuState(ms, cs.player, mon, rep);
                for (char &ch : cs.make)
                    ch = char(toupper(static_cast<unsigned char>(ch)));
                SDL_RenderSetLogicalSize(ren, 0, 0);
                fe.SetHideCarLabel(true);
                modernUi.DrawCarSelect(lw, lh, cs, tsec);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                nativeListVertical = false;
                return true;
            }
        }
        {
            FEStartupSequence::PaintShopInfo pa;
            if (startup.PaintShopState(pa)) { // the paint shop (ISSUES #372)
                ModernUI::PaintShop ps;
                ps.parts = pa.parts, ps.colours = pa.colours, ps.part = int(pa.part), ps.selected = int(pa.selected), ps.money = pa.money;
                ps.finish = pa.finish, ps.price = pa.price, ps.grid = pa.grid;
                SDL_RenderSetLogicalSize(ren, 0, 0);
                modernUi.DrawPaintShop(lw, lh, ps, tsec);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                nativeListVertical = false;
                return true;
            }
        }
        {
            FEStartupSequence::PartsShopInfo pi;
            if (startup.PartsShopState(pi)) { // the Body Shop (ISSUES #371)
                ModernUI::Shop sh;
                sh.kicker = "GARAGE", sh.title = pi.title, sh.name = pi.name, sh.price = pi.price, sh.total = pi.total;
                sh.categories = pi.categories, sh.category = int(pi.category), sh.index = int(pi.index), sh.count = int(pi.count), sh.money = pi.money;
                sh.fitted = pi.fitted;
                for (char &ch : sh.name)
                    if (ch == '?')
                        ch = ' ';
                SDL_RenderSetLogicalSize(ren, 0, 0);
                modernUi.DrawShop(lw, lh, sh, tsec);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                nativeListVertical = false;
                return true;
            }
        }
        {
            ModernUI::Settings st;
            size_t ssel = 0;
            if (startup.SettingsMenuState(st.kicker, st.title, st.labels, st.values, ssel)) { // Options / Quick Race options (ISSUES #368)
                st.selected = int(ssel);
                for (std::string &v : st.labels)
                    v.erase(std::remove(v.begin(), v.end(), '?'), v.end());
                for (std::string &v : st.values)
                    v.erase(std::remove(v.begin(), v.end(), '?'), v.end());
                size_t ms = 0;
                int mon = -1, rep = -1;
                startup.MainMenuState(ms, st.player, mon, rep);
                SDL_RenderSetLogicalSize(ren, 0, 0);
                modernUi.DrawSettings(lw, lh, st, tsec);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                return true;
            }
        }
        {
            FEStartupSequence::TrackSelectInfo ti;
            if (startup.TrackSelectState(ti)) { // the Quick Race track select (ISSUES #367)
                ModernUI::TrackSelect ts;
                ts.names = ti.names, ts.locked = ti.locked, ts.selected = int(ti.selected), ts.title = ti.title, ts.type = ti.type;
                ts.distance = ti.distance, ts.bestLabel = ti.bestLabel, ts.best = ti.best;
                for (std::string &nm : ts.names)
                    nm.erase(std::remove(nm.begin(), nm.end(), '?'), nm.end());
                SDL_RenderSetLogicalSize(ren, 0, 0);
                nativeListVertical = true;
                modernUi.DrawTrackSelect(lw, lh, ts, tsec);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                return true;
            }
        }
        std::string listPkg;
        std::vector<std::string> listLabels;
        std::vector<char> listDis;
        size_t listSel = 0;
        if (!startup.ScrollerMenuState(listPkg, listLabels, listDis, listSel))
            return false;
        static const struct { const char *pkg, *kicker, *title; } kTitles[] = {
            {"UI_StartCareer.fng", "NEED FOR SPEED", "CAREER"}, {"UI_QRModeSelect.fng", "QUICK RACE", "MODE SELECT"},
            {"UI_OptionsMain.fng", "NEED FOR SPEED", "OPTIONS"}, {"UI_ChooseCustomizeCategory.fng", "GARAGE", "CUSTOMIZE"},
            {"UI_ChoosePerformanceCategory.fng", "GARAGE", "PERFORMANCE"}, {"UI_ProfileManager.fng", "NEED FOR SPEED", "PROFILE"},
            {"UI_PauseOptionsMain.fng", "PAUSED", "OPTIONS"}, {"UI_ChoosePaintCategory.fng", "GARAGE", "PAINT"}};
        ModernUI::MainMenu lm;
        for (const auto &t : kTitles)
            if (strcasecmp(t.pkg, listPkg.c_str()) == 0)
                lm.kicker = t.kicker, lm.title = t.title;
        for (std::string &lb : listLabels) // the font has no TM / R glyphs: the original text shows them as '?'
            lb.erase(std::remove(lb.begin(), lb.end(), '?'), lb.end());
        std::vector<const char *> items;
        std::unique_ptr<bool[]> deadB(new bool[listLabels.size()]);
        for (size_t k = 0; k < listLabels.size(); ++k)
            items.push_back(listLabels[k].c_str()), deadB[k] = listDis[k] != 0;
        lm.items = items.data(), lm.count = int(items.size()), lm.selected = int(listSel), lm.disabled = deadB.get();
        size_t ms = 0;
        startup.MainMenuState(ms, lm.player, lm.money, lm.reputation);
        SDL_RenderSetLogicalSize(ren, 0, 0);
        nativeListVertical = true;
        modernUi.DrawMainMenu(lw, lh, lm, tsec);
        SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
        return true;
    };
    auto drawStage = [&](float seconds, bool carScreen) -> bool {
        static bool wasMenuStage = false;
        if (!carScreen && !wasMenuStage)
            stageFlourishT0 = seconds; // the main menu just came up: restart the doors' flourish
        wasMenuStage = !carScreen;
        if (!syncStageCar(seconds))
            return false;
        TrackWorld::StageParams sp;
        if (carScreen) { // car select / shops: a slow turn around the car, the car a little right of centre
            sp.orbit = 0.5f + seconds * 0.18f;
            sp.shiftX = 0.0f;
            sp.distance = 9.6f;
        } else { // main menu: a three-quarter view with a gentle sway, the car on the right
            sp.orbit = 0.62f + 0.10f * std::sin(seconds * 0.23f);
            sp.shiftX = 0.36f;
            sp.distance = 9.0f;
            sp.lookZ = 1.0f;
            // a warm concrete-and-sodium studio instead of the default blue wash (the menu itself carries the one accent colour)
        }
        sp.accent[0] = 0.42f, sp.accent[1] = 0.36f, sp.accent[2] = 0.28f; // one warm concrete studio for every menu car scene
        sp.accent2[0] = 0.20f, sp.accent2[1] = 0.17f, sp.accent2[2] = 0.08f;
        sp.time = seconds;
        SDL_RenderSetLogicalSize(ren, 0, 0);
        int bw, bh;
        SDL_GetRendererOutputSize(ren, &bw, &bh);
        const bool ok = stageWorld->RenderStage(ren, bw, bh, sp);
        SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
        return ok;
    };
    // The magazine cover screen (ISSUES #309): a freshly won picture (modernPause 5) or the collection (6).
    std::vector<FEStartupSequence::ShowInfo> coverList;
    size_t coverSel = 0;
    auto drawCoverScreen = [&](int w, int h, float seconds, bool reveal) {
        if (coverList.empty()) {
            modernUi.DrawCoverFrame(w, h, SDL_Rect{w / 2 - 150, h / 2 - 150, 300, 300}, "MAGAZINES", "No covers yet",
                                    "Attend the car shows marked on the map to get your car into print.", "ESC  back", seconds);
            return;
        }
        const FEStartupSequence::ShowInfo &si = coverList[coverSel % coverList.size()];
        const Magazines::Cover *cv = nullptr;
        if (magazinesReady())
            cv = magazines.ForCode(si.code) ? magazines.ForCode(si.code) : magazines.ForLabel(si.label);
        if (!cv && magazinesReady()) { // a magazine without layers of its own wears one of the full covers
            std::vector<const Magazines::Cover *> full;
            for (const Magazines::Cover &c : magazines.Covers())
                if (c.full)
                    full.push_back(&c);
            if (!full.empty())
                cv = full[(si.media ? si.media->id : 0u) % full.size()];
        }
        const int sz = int(float(h) * 0.84f);
        const SDL_Rect area{(w - int(float(sz) * 0.655f)) / 2, (h - sz) / 2, sz, sz};
        char detail[160];
        snprintf(detail, sizeof(detail), "%s", si.media ? si.media->name : "");
        for (char *c = detail; *c; ++c)
            if (*c == '_')
                *c = ' ';
        char hint[96];
        if (reveal)
            snprintf(hint, sizeof(hint), "ENTER  continue");
        else
            snprintf(hint, sizeof(hint), "LEFT / RIGHT  browse   (%zu of %zu)     ESC  back", coverSel % coverList.size() + 1, coverList.size());
        modernUi.DrawCoverFrame(w, h, Magazines::PageOf(area), reveal ? "YOU MADE THE COVER" : "YOUR MAGAZINE COVERS", si.code, detail, hint, seconds);
        if (!cv)
            return;
        const unsigned variant = si.media ? si.media->id * 2654435761u >> 7 : 0;
        magazines.Draw(ren, *cv, area, variant, [&](const SDL_Rect &page) {
            if (!syncStageCar(seconds) || !stageWorld)
                return;
            TrackWorld::StageParams sp;
            sp.overlay = true;
            if (cv && !cv->full) { // a masthead-only magazine: the car fills the middle of the page
                sp.vx = page.x + int(float(page.w) * 0.02f), sp.vy = page.y + int(float(page.h) * 0.22f);
                sp.vw = int(float(page.w) * 0.96f), sp.vh = int(float(page.h) * 0.50f);
            } else { // a full cover: lower right, between the model and the headlines
                sp.vx = page.x + int(float(page.w) * 0.10f), sp.vy = page.y + int(float(page.h) * 0.50f);
                sp.vw = int(float(page.w) * 0.92f), sp.vh = int(float(page.h) * 0.42f);
            }
            sp.orbit = 0.95f + 0.12f * std::sin(seconds * 0.5f), sp.distance = 7.6f, sp.lookZ = 0.55f, sp.height = 1.15f, sp.fov = 34.0f;
            sp.time = seconds;
            int ow, oh;
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            stageWorld->RenderStage(ren, ow, oh, sp);
        });
    };
    RaceRoute route;
    std::vector<PlayerCar> opponentCars;
    std::vector<std::vector<uint8_t>> hudTexturePacks; // kept alive: the front end keeps pointers into them
    float introBars = 0.0f; // letterbox amount of the pre-race camera (#265)
    Minimap minimap; // the HUD's round map (#259)
    bool mapNative = false, mapRotate = true; // the Unbound-style native map is being drawn (#296)
    Uint32 lastFxTicks = 0;
    unsigned mapFrame = 0;
    bool mapFresh = true;
    float mapWindow = 260.0f;
    std::vector<uint32_t> minimapPixels;
    bool minimapBound = false;
    std::vector<PlayerCar> garageCars; // the career's other cars on the garage room's discs (#252)
    std::vector<AiDriver> ai;
    TrafficNet trafficNet;
    // The free-roam police (ISSUES #318): the imported car, the pursuit and the siren.
    PoliceMesh policeMesh, policeLodMesh;
    bool policeLodOk = false;
    bool policeMeshTried = false, policeMeshOk = false, sirenOpen = false;
    Pursuit pursuit;
    Uint32 pursuitResultStart = 0;
    SirenSound sirenSound;
    std::vector<PlayerCar> trafficCars;
    Traffic traffic;
    std::vector<bool> aiOut; // URL knockout (ISSUES #117): opponents eliminated this race
    AiDriver playerAi; // NFSU2_AUTODRIVE with a route
    std::vector<float> aiStartS;
    std::vector<uint8_t> inGameCommon, inGameRace, inGameDrift, inGameFreeRoam;
    bool inWorld = false, flyCamera = false, sceneMode = false;
    // The garage's 3D transitions (ISSUES #359): when the shot changes the camera swoops in (orbit, dolly, height and a lens punch) with a
    // radial blur, and a title card sweeps across.
    struct GarageTrans {
        float start = -100.0f, dur = 1.3f, dir = 1.0f;
        int shot = -1;
        bool entry = true;
        const char *title = "", *sub = "";
    } garageTrans;
    ModernUI::Results resultsView;   // the native results / reward screen's data (ISSUES #362)
    float resultsShownAt = 0.0f;
    float garageFx = 0.0f;
    int garageBayIndex = -1;       // the world car that holds the player's engine
    float garagePlayerM[12] = {};  // the player car's last transform
    float garageHoodOpen = 0.0f;   // the hood's opening angle, for the engine bay // the blur strength the transition asks of the speed effect
    FEPackage *pausePkg = nullptr; // UI_Pause.fng while the race is paused (ISSUES.md #122)
    // The modern pause dialog (ISSUES #281): 0 closed, 1 menu, 2 confirm 'back to the main menu', 3 confirm 'quit the game'.
    int modernPause = 0, modernSel = 0;
    bool modernYes = false, modernLeave = false;
    FEStartupSequence::UniqueInfo uniqueInfo; // the visual Unique being offered (modernPause 8, ISSUES #319)
    int uniqueSel = 0;
    std::vector<Minimap::Dot> poiDots; // free-roam event / car show / Unique spots for the HUD minimap (filled with the beacons)
    bool optionsPush = false;
    bool optionsFromModern = false; // the options carousel was opened from the dialog: Back returns to the dialog, not to UI_Pause
    int optionsGuard = 0; // frames after the dialog's OPTIONS opens UI_Pause.fng: its Continue also hears that Accept press and would resume at once
    // The pause menu's package changes when Options opens and closes (a package switch), so look it up.
    auto pauseScreen = [&engine, &pausePkg]() -> FEPackage * {
        for (FEPackage *p = engine->GetPackageList()->GetFirstPackage(); p; p = p->GetNext())
            if (p->GetName() && (strncasecmp(p->GetName(), "UI_Pause", 8) == 0))
                return p;
        return pausePkg;
    };
    // Career crib (UI_CareerCrib.fng): the career hub over a career free-roam session (ISSUES #141).
    FEPackage *cribPkg = nullptr;
    bool shopFromWorld = false;       // the open shop screen was entered from a free-roam hot spot (#269)
    bool shopScene = false;           // the shop's room (TrackWorld::LoadScene keepWorld) draws over the resident city (#330)
    struct StreetPose {               // where the car and the camera stood when the shop opened
        float x, y, z, yaw, camX, camY, camZ, camYaw, camPitch;
    } streetPose{};
    const Career::Trigger *engageArmed = nullptr; // an event locator stays disarmed until the car leaves its zone (ISSUES #303)
    FEStartupSequence::EngageInfo engage;
    const Career::Trigger *tipArmed = nullptr; // a tip spot delivers its message once per visit (ISSUES #308)
    std::string tipTitle, tipBody;
    Uint32 tipStart = 0;
    std::string seriesTitle, seriesBody; // a career series race start's toast (ISSUES #334)
    Uint32 seriesToastStart = 0;
    int engageSel = 0;
    const Career::Hotspot *hotArmed = nullptr; // a hot spot stays disarmed until the car leaves its circle

    int playerSkinIndex = -1;       // the player's car in the world (vinyl skin, ISSUES #223)
    std::string playerSkinKey;
    // Post-race results (UI_PostRaceResults.fng): opponents' finish times on a race clock.
    FEPackage *resultsPkg = nullptr, *rewardPkg = nullptr;
    std::vector<std::string> oppNames;
    std::vector<float> aiFinish;
    float raceClock = 0, finishedFor = 0;
    std::string currentRegion;
    std::string currentHudName; // HUD package of the current world session (pushed on leaving the crib)
    int worldRaceNumber = 0; // the race the world session is running (0 = free roam)
    bool worldRaceReverse = false; // direction the route was built with (QR setting / career slot)
    FEPackage *hud = nullptr;
    RaceSession race;
    u32 worldLastPad = 0;
    RaceSession::Config raceConfig;
    float chaseYaw = 0.0f, steer = 0.0f;
    CameraRig::Chase chaseRig;       // ISSUES #285
    CameraRig::Garage garageRig;
    CameraRig::Clearance introClear;
    const bool autoDrive = getenv("NFSU2_AUTODRIVE") != nullptr; // test aid: hold the throttle
    const bool dragAuto = getenv("NFSU2_DRAG_AUTO") != nullptr;  // test aid: hold throttle + auto-upshift
    // Dev aid: NFSU2_AI_LANE offsets the autodrive player's racing line (metres, + = left).
    const float autoLane = getenv("NFSU2_AI_LANE") ? float(atof(getenv("NFSU2_AI_LANE"))) : 0.0f;
    // Dev aid: NFSU2_CAM_ORBIT=<degrees> swings the chase camera around the car (0 = behind,
    // 90 = right side); for inspecting the car model in the world (ISSUES #161).
    const float camOrbit = getenv("NFSU2_CAM_ORBIT") ? float(atof(getenv("NFSU2_CAM_ORBIT"))) * 0.0174532925f : 0.0f;
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
    if (const char *typeName = getenv("NFSU2_PART_DUMP")) { // dev aid: records of a car type, e.g. SKYLINE
        const std::vector<CarPart> &ps = fe.Parts().Parts(bStringHash(typeName));
        printf("[PartDump] %s: %zu records\n", typeName, ps.size());
        std::map<int, int> byId;
        for (const CarPart &cp : ps)
            ++byId[cp.partId];
        for (auto &kv : byId)
            printf("[PartDump]   part id 0x%x: %d\n", kv.first, kv.second);
        for (const CarPart &cp : ps)
            if ((cp.partId >= 0x40 && cp.partId <= 0x43) || getenv("NFSU2_PART_DUMP_ALL"))
                printf("[PartDump]   id 0x%x group %u level %u %s\n", cp.partId, cp.group, cp.level, cp.name.c_str());
    }
    if (const char *ids = getenv("NFSU2_PERF_PARTINFO")) // dev aid: category / level of part ids, e.g. 15,19,24
        for (const char *s = ids; *s;) {
            const uint32_t id = uint32_t(strtoul(s, nullptr, 10));
            const PerfPackage *pk = fe.PerfParts().FindByPart(id);
            printf("[PerfPart] id %u -> %s %u level %u\n", id, pk ? "category" : "none", pk ? pk->category : 0, pk ? pk->level : 0);
            s = strchr(s, ',') ? strchr(s, ',') + 1 : s + strlen(s);
        }
    UiSound uiSound; // front-end clicks (ISSUES.md #126)
    if (musicVolume > 0 && uiSound.Load(ReadGameFile(*fs, "SOUND\\GLOBAL\\FE_COMMON_MB.abk")))
        fe.SetSoundHook([&uiSound](uint32_t msg) { uiSound.OnMessage(msg); });
    std::function<void(const std::string &)> enterWorld;
    enterWorld = [&](const std::string &regionIn) {
        // NFSU2_RACE selects a race by its race-database number; the track's own region then wins over
        // the region the front end asked for (career enters the city, but a forced race lives in the
        // region its race record names, e.g. 4602 -> L4RF).
        std::string region = regionIn;
        if (const char *pc = getenv("NFSU2_PLAY_CAR")) // dev aid: drive this car (any roster car, e.g. PICANTO)
            fe.SetPreviewCar(pc);
        if (const char *n = getenv("NFSU2_RACE")) {
            const int want = atoi(n);
            for (const RaceInfo &r : fe.Races())
                if (r.number == want && r.region[0])
                    region = r.region;
        }
        currentRegion = region;
        if (const char *vc = getenv("NFSU2_VINYL_DUMP")) { // dev aid: every texture name of a car's vinyl pack (ISSUES #323)
            TexturePack vp;
            const std::vector<uint8_t> vd = fe.ReadGameFile((std::string("CARS\\") + vc + "\\VINYLS.BIN").c_str());
            if (vp.Parse(vd.data(), vd.size()))
                for (size_t i = 0; i < vp.Count(); ++i)
                    printf("[Vinyl] %s\n", vp.At(i).name.c_str());
        }
        if (getenv("NFSU2_RIM_DUMP")) { // dev aid: the styles every rim brand ships (ISSUES #321)
            TexturePack rimPack;
            const std::vector<uint8_t> rimData = fe.ReadGameFile("CARS\\WHEELS\\TEXTURES.BIN");
            RimCatalog rimCat;
            if (rimPack.Parse(rimData.data(), rimData.size()))
                rimCat.Build(rimPack);
        }
        pausePkg = nullptr;
        cribPkg = nullptr;
        resultsPkg = nullptr;
        rewardPkg = nullptr;
        oppNames.clear();
        aiFinish.clear();
        raceClock = finishedFor = 0;
        // LoadingRegion (0x002ddb40): show Loading.fng while the region streams in.
        loadBase = 0.0f, loadSpan = 0.2f, loadShown = 0.0f, loadStage = "Loading the race interface";
        if (engine->PushPackage("Loading.fng", 0, 0xff)) {
            engine->Update(16, 0);
            if (!loadingFrame("INGAMECOMMON", 0.0f))
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
            // Global texture packs the HUD draws from (the tachometer face 0x40fe164f; ISSUES #260).
            for (const char *tp : {"GLOBAL\\DYNTEX.BIN", "GLOBAL\\HUD_CUSTOMTEXTURES_ALL.BIN"}) {
                std::vector<uint8_t> data = ReadGameFile(*fs, tp);
                if (!data.empty()) {
                    hudTexturePacks.push_back(std::move(data));
                    fe.AddBundle(hudTexturePacks.back().data(), hudTexturePacks.back().size(), tp);
                }
            }
            if (getenv("NFSU2_LIST_INGAME"))
                for (const std::string &n : fe.PackageNames())
                    printf("[World] package %s\n", n.c_str());
        }
        std::string path = "TRACKS\\STREAM" + region + ".BUN";
        loadBase = 0.2f, loadSpan = 0.1f, loadStage = "Streaming the city";
        loadingFrame("", 0.0f);
        std::vector<uint8_t> stream = ReadGameFile(*fs, path.c_str());
        loadingFrame("", 1.0f);
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
        printf("[World] %zu neighbouring stream(s) read for borrowed solids\n", neighbours.size());
        world.SetExtraPack({});
        // The location's dynamic textures (TRACKS\\LOC<n>DYNTEX.BIN, an 0xb3300000 pack the region streams do not
        // carry): the tree canopies and other foliage name textures that only live here (ISSUES #254).
        if (region.size() >= 2 && region[0] == 'L' && isdigit(static_cast<unsigned char>(region[1])) &&
            !getenv("NFSU2_NO_DYNTEX"))
            world.SetExtraPack(ReadGameFile(*fs, (std::string("TRACKS\\LOC") + region[1] + "DYNTEX.BIN").c_str()));
        // The global packs the world draws from too: the PAL GLOBALTEXTURESROW.BIN (shop ads and shared signs) and the
        // textures of INGAMECOMMON (the light beams, the start / finish graphics ...).
        if (!getenv("NFSU2_NO_GLOBALTEX")) {
            world.AddExtraPack(ReadGameFile(*fs, "GLOBAL\\GLOBALTEXTURESROW.BIN"));
            world.AddExtraPack(inGameCommon);
            world.AddExtraPack(globalB); // GLOBALB also holds shared object textures (OBJ_*)
        }
        loadBase = 0.3f, loadSpan = 0.55f, loadStage = "Building the city";
        world.SetProgress([&](float f) { loadingFrame("", f); });
        { // the street props stay off the race route (ISSUES #344): sample it before the world builds them
            int rn = startup.RaceNumber(), rtype = startup.RaceType();
            if (const char *n = getenv("NFSU2_RACE"))
                rn = atoi(n);
            if (const char *t = getenv("NFSU2_RACETYPE"))
                rtype = atoi(t);
            std::vector<float> xy;
            if (rn && rtype != 9) {
                float trackLen = 0.0f;
                for (const RaceInfo &r : fe.Races())
                    if (r.number == rn)
                        trackLen = float(r.length);
                char pp[64];
                snprintf(pp, sizeof(pp), "TRACKS\\ROUTES%s\\PATHS%d.BIN", region.c_str(), rn);
                RaceRoute propRoute;
                if (propRoute.Load(ReadGameFile(*fs, pp), trackLen))
                    for (float s = 0.0f; s < propRoute.Length(); s += 3.0f) {
                        float x, y, dx, dy;
                        propRoute.Sample(s, x, y, dx, dy);
                        xy.push_back(x), xy.push_back(y);
                    }
            }
            world.SetPropAvoid(std::move(xy));
        }
        const bool loaded = !stream.empty() && world.Load(stream, neighbours);
        world.SetProgress(nullptr);
        loadStage = nullptr;
        std::vector<std::vector<uint8_t>>().swap(neighbourData);
        if (!loaded) {
            fprintf(stderr, "[World] could not load %s\n", path.c_str());
            return;
        }
        {
            std::vector<uint8_t> regionBundle = ReadGameFile(*fs, ("TRACKS\\" + region + ".BUN").c_str());
            world.LoadBarrierGroups(regionBundle);
        }
        // The first RenderGL uploads every cell to the GPU (~1.3 s). Done here in 24 ms slices under the loading screen, so its animation and the
        // bar keep moving instead of freezing at 100 % (ISSUES #311).
        if (!getenv("NFSU2_NO_WARMUP") && modernUi.Ready()) {
            loadBase = 0.85f, loadSpan = 0.15f, loadStage = "Preparing graphics";
            world.SetWarmBudget(24);
            for (int guard = 0; world.Warming() && guard < 3000; ++guard) {
                int ww, wh;
                SDL_RenderSetLogicalSize(ren, 0, 0);
                SDL_GetRendererOutputSize(ren, &ww, &wh);
                world.RenderGL(ren, camera, ww, wh);
                if (!loadingFrame("", world.WarmFraction()))
                    break;
            }
            world.SetWarmBudget(0);
            loadingFrame("", 1.0f);
            loadStage = nullptr;
            SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
        }
        minimap.Build(world.RoadTriangles());
        minimapBound = false; // the new HUD package gets the map image again
        camera = world.StartCamera();
        flyCamera = getenv("NFSU2_WORLD_FLY") != nullptr;
        int raceNumber = startup.RaceNumber();
        if (const char *n = getenv("NFSU2_RACE"))
            raceNumber = atoi(n);
        raceConfig = RaceSession::Config();
        raceConfig.laps = startup.RaceLaps();
        if (const char *lp = getenv("NFSU2_LAPS")) // dev aid: override the lap count
            raceConfig.laps = std::max(1, atoi(lp));
        raceConfig.showGauges = startup.ShowGauges();
        raceConfig.modernHud = !getenv("NFSU2_CLASSIC_HUD");
        raceConfig.showPosition = startup.ShowRacePosition();
        raceConfig.showLapInfo = startup.ShowLapInfo();
        raceConfig.showLeaderboard = startup.ShowLeaderboard();
        raceConfig.raceMapMode = startup.RaceMapMode();
        raceConfig.exploreMapMode = startup.ExploreMapMode();
        raceConfig.type = startup.RaceType();
        if (const char *t = getenv("NFSU2_RACETYPE"))
            raceConfig.type = atoi(t);
        // The cinematic pre-race camera (ISSUES #265): 7 s before the countdown for real races. Scripted test runs
        // (autodrive, injected drive / input) keep the old 3 s start; NFSU2_INTRO=1 forces it, NFSU2_NO_INTRO=1 removes it.
        raceConfig.introSeconds = (getenv("NFSU2_INTRO") || (!getenv("NFSU2_NO_INTRO") && !getenv("NFSU2_AUTODRIVE") &&
                                                            !getenv("NFSU2_DRIVE") && !getenv("NFSU2_NOINPUT") && raceConfig.type != 2))
                                      ? 7.0f
                                      : 0.0f;
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
        // Track Direction (race settings +4, 0x0025c7b0): traverse the same route backwards. The
        // original keeps the race number and only flips the direction global (0x0053e404), which
        // selects the other start grid (+0xec tables) and the reverse route run.
        const bool reverse = !freeRoam && (startup.RaceReverse() || getenv("NFSU2_REVERSE") != nullptr);
        const int freeSpawnRace = raceNumber; // a free run on a track starts on that track's grid (ISSUES #349)
        if (freeRoam)
            raceNumber = 0;
        else if (raceNumber && route.Load(ReadGameFile(*fs, pathsPath), raceConfig.trackLength)) {
            if (reverse) {
                route.Reverse();
                printf("[World] race %d: reverse direction\n", raceNumber);
            }
            raceConfig.route = &route;
        }
        worldRaceNumber = raceNumber;
        worldRaceReverse = reverse;
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
        // Collision barrier groups (ISSUES #147): the groups named for this race
        // (PLAYER_BARRIERS_<race>, BARRIERS_<race>) are on, every other race's are off -- each race's
        // own groups never cross its route, the others' do. Free roam has none.
        for (size_t g = 1; g < world.BarrierGroupCount(); ++g) {
            const std::string &name = world.BarrierGroupName(g);
            const std::string suffix = "_" + std::to_string(raceNumber);
            const bool on = raceNumber != 0 && name.size() > suffix.size() &&
                            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
            world.SetBarrierGroupEnabled(g, on);
        }
        // Start grid: of the race's grids, the one heading along the route (the other is for the
        // reverse direction); the player starts at the back, opponents ahead.
        std::string markersPath = "TRACKS\\ROUTES" + region + "\\TRACKPOSMARKERSALL.BIN";
        std::vector<uint8_t> markers = ReadGameFile(*fs, markersPath.c_str());
        std::vector<TrackWorld::Camera> grid;
        if (raceNumber) {
            // Each race lists two start setups (a grid of 8 and its line marker), one per direction, and they can lie far
            // apart.  Take the setup on the route (a grid hundreds of metres off it is another layout's); if the route runs
            // against the grid's heading the Paths file is stored the other way round, so run it backwards (ISSUES #345).
            float best = -1e9f;
            bool flip = false;
            for (auto &g : world.GridsFromMarkers(markers, raceNumber)) {
                float align = 1.0f, off = 0.0f;
                if (route.Valid()) {
                    float x, y, dx, dy;
                    const float gs = route.Project(g[0].x, g[0].y);
                    route.Sample(gs, x, y, dx, dy);
                    off = std::hypot(g[0].x - x, g[0].y - y);
                    // At the end of an open route the sampled direction is a clamp artifact: use the last 5 m's chord.
                    if (!route.Closed() && (gs < 5.0f || gs > route.Length() - 5.0f)) {
                        float x0, y0, x1, y1, t0, t1;
                        const float e = gs < 5.0f ? 0.0f : route.Length() - 5.0f;
                        route.Sample(e, x0, y0, t0, t1);
                        route.Sample(e + 5.0f, x1, y1, t0, t1);
                        const float cl = std::max(1e-3f, std::hypot(x1 - x0, y1 - y0));
                        dx = (x1 - x0) / cl, dy = (y1 - y0) / cl;
                    }
                    align = std::cos(g[0].yaw) * dx + std::sin(g[0].yaw) * dy;
                    if (getenv("NFSU2_ROUTE_DUMP"))
                        printf("[Route] grid of %zu at %.1f %.1f yaw %.2f: route %.1f %.1f (%.0f m off) align %.2f\n", g.size(), g[0].x, g[0].y,
                               g[0].yaw, x, y, off, align);
                }
                const float base = off < 30.0f ? 0.0f : -off;
                for (int rev = 0; rev < (route.Valid() && off < 60.0f ? 2 : 1); ++rev) { // 60 m: an open route can start a lead-in away from its grid
                    const float score = base + (rev ? -align - 0.1f : align); // an aligned grid wins over reversing the route
                    if (score > best) {
                        best = score;
                        grid = g;
                        flip = rev != 0;
                    }
                }
            }
            if (flip && route.Valid()) {
                route.Reverse();
                printf("[World] race %d: the route runs against its start grid, reversed\n", raceNumber);
            }
        }
        // Free run on a race track region: the region's default camera is not on any road (L4RC: under the map, L4RF: on a girder),
        // so start on the start grid of the selected track, or of the region's first track. The city (L4RA) keeps its own start.
        if (freeRoam && region != "L4RA") {
            int spawnRace = freeSpawnRace;
            if (!spawnRace)
                for (const RaceInfo &r : fe.Races())
                    if (!spawnRace && region == r.region && r.length > 0 && !r.special)
                        spawnRace = r.number;
            const auto spawnGrids = spawnRace ? world.GridsFromMarkers(markers, spawnRace) : std::vector<std::vector<TrackWorld::Camera>>();
            if (!spawnGrids.empty()) {
                camera = spawnGrids[0][0];
                printf("[World] free run: start at the grid of race %d (%.1f, %.1f)\n", spawnRace, camera.x, camera.y);
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
            raceConfig.hasLine = world.LineFromMarkers(markers, raceNumber, raceConfig.lineX, raceConfig.lineY, grid[0].x, grid[0].y);
            if (route.Valid()) {
                // Anchor the line to the front grid slot: on a multi-lap Paths file the line marker's
                // nearest copy and the player's back slot can be one lap apart, while the front slot
                // is on the grid's lap (ISSUES #166).
                const float gridS = route.Project(grid[0].x, grid[0].y);
                raceConfig.lineS = route.Project(raceConfig.hasLine ? raceConfig.lineX : camera.x,
                                                 raceConfig.hasLine ? raceConfig.lineY : camera.y,
                                                 raceConfig.hasLine ? gridS : -1.0f);
                // The grid is on the same lap as the line; resolve the player's slot against lineS
                // instead of projecting freely (which jumps to the next lap and starts the race at
                // the finish).
                raceConfig.startS = route.Project(camera.x, camera.y, raceConfig.lineS);
                if (getenv("NFSU2_ROUTE_DUMP")) // dev aid: the route every 10 m around the start grid (ISSUES #345)
                    for (float d = -20.0f; d <= 140.0f; d += 10.0f) {
                        float rx, ry, rdx, rdy;
                        route.Sample(raceConfig.startS + d, rx, ry, rdx, rdy);
                        printf("[Route] start%+4.0f m: %.1f %.1f dir %.2f %.2f\n", d, rx, ry, rdx, rdy);
                    }
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
        playerAi = AiDriver(); // a new world: forget the previous route's progress (autodrive aid)
        aiStartS.clear();
        aiOut.clear();
        // Car textures: the shared pack and the player's (opponents add theirs below).
        world.AddTexturePack(ReadGameFile(*fs, "CARS\\TEXTURES.BIN"));
        if (fe.PreviewRimStyle() > 0 || getenv("NFSU2_PART_RIM"))
            world.AddTexturePack(ReadGameFile(*fs, "CARS\\WHEELS\\TEXTURES.BIN")); // fitted rims (#226)
        world.AddTexturePack(ReadGameFile(*fs, ("CARS\\" + fe.PreviewCarName() + "\\TEXTURES.BIN").c_str()));
        // Dev aid: NFSU2_TEX_DUMP=<hex name hash> saves that texture of the player's car pack as BMP.
        if (const char *dump = getenv("NFSU2_TEX_DUMP")) {
            const uint32_t want = uint32_t(strtoul(dump, nullptr, 16));
            std::vector<uint8_t> data = ReadGameFile(*fs, ("CARS\\" + fe.PreviewCarName() + "\\TEXTURES.BIN").c_str());
            TexturePack pack;
            if (pack.Parse(data.data(), data.size()))
                for (size_t i = 0; i < pack.Count(); ++i)
                    if (pack.At(i).nameHash == want) {
                        DecodedTexture t;
                        if (pack.Decode(pack.At(i), t)) {
                            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(t.rgba.data(), t.width, t.height, 32,
                                                                                    t.width * 4, SDL_PIXELFORMAT_ABGR8888);
                            SDL_SaveBMP(surf, "/tmp/tex_dump.bmp");
                            SDL_FreeSurface(surf);
                            printf("[Tex] dumped %s %dx%d bpp %d alpha usage %d blend %d\n", pack.At(i).name.c_str(), t.width,
                                   t.height, pack.At(i).bitsPerPixel, pack.At(i).alphaUsage, pack.At(i).alphaBlend);
                        }
                    }
        }
        // Car audio banks by car type and engine upgrade level (stock = 0) from UG2's table (ISSUES #128).
        if (musicVolume > 0) {
            // The engine level is the highest engine package part fitted (0x0017a118: part ids 0x0f / 0x13 / 0x18).
            int audioLevel = 0;
            for (uint16_t id : fe.PreviewPerfParts())
                audioLevel = std::max(audioLevel, id == 0x18 ? 3 : id == 0x13 ? 2 : id == 0xf ? 1 : 0);
            const CarAudio::Level &audio =
                CarAudio::kRowLevels[CarAudio::RowForCarType(fe.CarTypeIndex(fe.PreviewCarName()))][audioLevel];
            if (getenv("NFSU2_SOUND_DEBUG"))
                printf("[Engine] %s: %s, %s\n", fe.PreviewCarName().c_str(), CarAudio::kEngineBanks[audio.engine],
                       CarAudio::kGearBanks[audio.gearbox]);
            engineSound.Load(ReadGameFile(*fs, (std::string("SOUND\\ENGINE\\") + CarAudio::kEngineBanks[audio.engine]).c_str()));
            // Engine records 9 and 38 have the turbo-sound flag (+0x10 of 0x004ab640) clear.
            if (audio.engine != 9 && audio.engine != 38)
                engineSound.LoadTurbo(ReadGameFile(*fs, (std::string("SOUND\\TURBO\\") + CarAudio::kTurboBanks[audio.turbo]).c_str()));
            if (getenv("NFSU2_SOUND_DEBUG"))
                printf("[Engine] level %d turbo bank %s\n", audioLevel, CarAudio::kTurboBanks[audio.turbo]);
            engineSound.LoadSkid(ReadGameFile(*fs, "SOUND\\SKIDS\\SKID_PAV_MB.abk"));
            engineSound.LoadRoad(ReadGameFile(*fs, "SOUND\\IG_GLOBAL\\ROADNOISE_00_MB.abk"));
            engineSound.LoadShift(ReadGameFile(*fs, (std::string("SOUND\\SHIFTING\\") + CarAudio::kGearBanks[audio.gearbox]).c_str()));
            engineSound.LoadCollision(ReadGameFile(*fs, "SOUND\\IG_GLOBAL\\MB_Stich_Collision.abk"));
            engineSound.LoadNitrous(ReadGameFile(*fs, "SOUND\\NOS\\Nitrous_00_MB.abk"));
        }
        PlayerCar::Wheel wheels[4];
        PlayerCar::Performance perf;
        // The career car's fitted performance parts tune a copy of the record (ISSUES #200).
        std::vector<uint8_t> tunedRecord;
        if (!fe.PreviewPerfParts().empty())
            tunedRecord = fe.TunedCarRecord(fe.PreviewCarName(), fe.PreviewPerfParts(), fe.PreviewTune());
        if (!tunedRecord.empty()) {
            FENativeInterface::ReadCarPerformance(tunedRecord.data(), perf);
            car.SetPerformance(perf.Valid() ? perf : PlayerCar::Performance());
            PlayerCar::Performance stock;
            fe.CarPerformance(fe.PreviewCarName(), stock);
            float t0, v0, t1, v1;
            PlayerCar::Simulate(stock, 0.32f, t0, v0);
            PlayerCar::Simulate(perf, 0.32f, t1, v1);
            printf("[PerfShop] %zu parts fitted: 0-100 %.2f -> %.2f s, top speed %.0f -> %.0f km/h, mass %.3f -> %.3f\n",
                   fe.PreviewPerfParts().size(), t0, t1, v0 * 3.6f, v1 * 3.6f, stock.mass, perf.mass);
        } else {
            car.SetPerformance(fe.CarPerformance(fe.PreviewCarName(), perf) ? perf : PlayerCar::Performance());
        }
        // The ported UG2 vehicle physics (ISSUES #129) drive the player car; NFSU2_PHYSICS=arcade
        // selects the old stand-in model.
        if (const char *ph = getenv("NFSU2_PHYSICS"); !ph || strcmp(ph, "arcade")) {
            const uint8_t *rec = tunedRecord.empty() ? fe.CarRecordData(fe.PreviewCarName()) : tunedRecord.data();
            printf("[Physics] UG2 vehicle model: %s\n", rec && car.UseUG2Physics(rec) ? "on" : "unavailable");
        }
        const SDL_Color stockPaint = fe.StockPaintColor(fe.PreviewCarName(), 0, SDL_Color{40, 90, 200, 255}); // #288
        // The Body Shop preview selection (#188), falling back to the dev environment.
        PlayerCar::InstalledParts installedParts = fe.InstalledPartsForPreview();
        if (!flyCamera && car.LoadModel(ReadCarGeometry(*fs, fe.PreviewCarName()),
                                        fe.CarWheels(fe.PreviewCarName(), wheels) ? wheels : nullptr,
                                        &installedParts)) {
            // The career/QR paint (OwnedCar::paintSet/paint) selects a GLOBALB color (#167/#168).
            const size_t playerCarIndex = world.AddCar(&car.Model(), &car.TriangleTextures(),
                         fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(),
                                          stockPaint),
                         &car.TriangleRims(),
                         fe.ColorForPaint(fe.PreviewRimSet(), fe.PreviewRimPaint(),
                                          SDL_Color{38, 38, 38, 255}),
                         &car.TriangleCalipers(),
                         fe.ColorForPaint(fe.PreviewCaliperSet(), fe.PreviewCaliperPaint(),
                                          SDL_Color{128, 128, 128, 255}),
                         &car.TriangleMirrors(),
                         fe.ColorForPaint(fe.PreviewMirrorSet(), fe.PreviewMirrorPaint(),
                                          fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(),
                                                           stockPaint)),
                         &car.TriangleSpoilers(),
                         fe.ColorForPaint(fe.PreviewSpoilerSet(), fe.PreviewSpoilerPaint(),
                                          fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(),
                                                           stockPaint)),
                         &car.TriangleRoofScoops(),
                         fe.ColorForPaint(fe.PreviewRoofSet(), fe.PreviewRoofPaint(),
                                          fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(),
                                                           stockPaint)),
                         &car.TriangleEngines(),
                         fe.ColorForPaint(fe.PreviewEngineSet(), fe.PreviewEnginePaint(),
                                          SDL_Color{128, 64, 48, 255}),
                         &car.TriangleExhausts(),
                         fe.ColorForPaint(fe.PreviewExhaustSet(), fe.PreviewExhaustPaint(),
                                          SDL_Color{150, 150, 150, 255}));
            playerSkinIndex = int(playerCarIndex);
            garageBayIndex = -1;
            if (!car.EngineBayModel().empty() && !getenv("NFSU2_NO_ENGINE_BAY")) // the engine under the garage's open hood (ISSUES #360)
                garageBayIndex = int(world.AddCar(&car.EngineBayModel(), &car.EngineBayTextures(), SDL_Color{255, 255, 255, 255})); // white: the engine's own textures show
            playerSkinKey.clear(); // the per-frame check below composes the skin
            if (const char *spawn = getenv("NFSU2_SPAWN")) // dev aid: x,y,yaw (rad) -- put the player there (any grid)
                sscanf(spawn, "%f,%f,%f", &camera.x, &camera.y, &camera.yaw), camera.z = getenv("NFSU2_SPAWN_Z") ? float(atof(getenv("NFSU2_SPAWN_Z"))) : 20.0f;
            float ground = camera.z - 2.0f;
            // Some grids (L4RG 4701/4703) carry a marker z far below the surface: nothing is found under
            // it and the car would start underground. Look again from above.
            if (!world.GroundHeight(camera.x, camera.y, camera.z, ground)) {
                const bool above = world.GroundHeight(camera.x, camera.y, camera.z + 120.0f, ground);
                printf("[World] start ground not under marker z %.1f: %s %.1f\n", camera.z, above ? "from above" : "none", ground);
            }
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
                if (!world.GroundHeight(slot.x, slot.y, slot.z + 2.0f, ground))
                    world.GroundHeight(slot.x, slot.y, slot.z + 120.0f, ground);
                opponentCars[i].Place(slot.x, slot.y, ground, slot.yaw);
                // Rivals wear their own model's stock colours (#288): the default paint, or the second default when it
                // would match the player's car.
                SDL_Color rivalPaint = fe.StockPaintColor(name, 0, kPaints[i % 5]);
                if (rivalPaint.r == stockPaint.r && rivalPaint.g == stockPaint.g && rivalPaint.b == stockPaint.b)
                    rivalPaint = fe.StockPaintColor(name, 1, kPaints[i % 5]);
                const size_t oppWorldIndex = world.AddCar(&opponentCars[i].Model(), &opponentCars[i].TriangleTextures(), rivalPaint,
                             nullptr, SDL_Color{38, 38, 38, 255}, &opponentCars[i].TriangleCalipers(),
                             SDL_Color{128, 128, 128, 255});
                // Opponent vinyls: two of every three rivals carry one design from a few body-style groups
                // (stand-in for the original's per-driver customization, ISSUES #231).
                if (i % 3 != 2 && !getenv("NFSU2_NO_OPP_VINYLS")) {
                    CarSkin::Vinyls vin;
                    VinylCatalog cat;
                    if (vin.Load(ReadGameFile(*fs, ("CARS\\" + name + "\\VINYLS.BIN").c_str()), name) && vin.Valid() &&
                        cat.Build(*vin.Pack(), name)) {
                        std::vector<const VinylDesign *> pick;
                        for (const char *g : {"STRIPES", "FLAMES", "TRIBAL", "LIGHTNING", "MODERN"}) {
                            const int gi = VinylCatalog::GroupIndexOf(g);
                            if (gi >= 0)
                                for (const VinylDesign *d : cat.InGroup(gi))
                                    if (!d->set)
                                        pick.push_back(d);
                        }
                        if (!pick.empty()) {
                            CarSkin::Layer layer;
                            layer.design = pick[(size_t(i) * 7 + name.size() * 13 + 5) % pick.size()]->name;
                            const size_t count = fe.Paint().Count(PaintCatalog::kVinyl);
                            if (count >= 3)
                                for (int k = 0; k < 3; ++k) {
                                    const PaintColor &pc = fe.Paint().At(PaintCatalog::kVinyl, (size_t(i) * 11 + 4 + size_t(k) * 13) % count);
                                    layer.color[k][0] = pc.r, layer.color[k][1] = pc.g, layer.color[k][2] = pc.b;
                                }
                            std::vector<uint32_t> skin;
                            const SDL_Color bc = rivalPaint;
                            if (vin.Compose(bc.r, bc.g, bc.b, {layer}, skin)) {
                                world.SetCarSkin(oppWorldIndex,
                                                 CarSkin::DetectBodySlot(opponentCars[i].Model(), opponentCars[i].TriangleTextures(), name),
                                                 std::move(skin));
                                printf("[World] opponent %d vinyl %s\n", i + 1, layer.design.c_str());
                            }
                        }
                    }
                }
                world.AddTexturePack(ReadGameFile(*fs, ("CARS\\" + name + "\\TEXTURES.BIN").c_str()));
                // Lane: keep each car on its own side of the route line; faster cars at the front.
                // Resolve against lineS so a multi-lap Paths file does not place the grid on the
                // next lap's copy of the same tarmac (ISSUES #166).
                const float slotS = route.Project(slot.x, slot.y, raceConfig.lineS);
                float x, y, dx, dy;
                route.Sample(slotS, x, y, dx, dy);
                float lane = std::clamp(-(slot.x - x) * dy + (slot.y - y) * dx, -4.5f, 4.5f);
                // NFSU2_AI_SKILL scales the opponents' pace (test aid: set the field slower so an
                // automated player can lead, e.g. to exercise the URL knockout).
                float skill = 0.86f + 0.04f * float(opponents - i) / opponents;
                skill *= startup.CareerDifficulty() == 0 ? 0.94f : startup.CareerDifficulty() == 2 ? 1.06f : 1.0f; // Options > Gameplay
                if (const char *s = getenv("NFSU2_AI_SKILL"))
                    skill *= std::clamp(float(atof(s)), 0.1f, 2.0f);
                ai[i].Init(&opponentCars[i], &route, lane, skill, slotS);
                aiStartS.push_back(slotS);
                printf("[World] opponent %d: %s\n", i + 1, name.c_str());
                oppNames.push_back(name);
                aiFinish.push_back(0.0f);
            }
        }
        // Ambient traffic (ISSUES #130): service vehicles on the free-roam lane links.
        trafficCars.clear();
        traffic = Traffic();
        // Career hot spots (ISSUES #269): the shops, car lots and the crib stand as light columns in free roam.
        {
            std::vector<TrackWorld::Beacon> beacons;
            poiDots.clear();
            if (raceConfig.type == 9 || getenv("NFSU2_HOTSPOTS")) {
                char hotPath[96];
                snprintf(hotPath, sizeof(hotPath), "TRACKS\\ROUTES%s\\PATHSFREEROAM.BIN", region.c_str());
                Career::HotspotTable &hot = Career::HotspotTable::Get();
                printf("[Hotspots] %zu placed in %s\n", hot.LoadPositions(ReadGameFile(*fs, hotPath)), hotPath);
                for (const Career::Hotspot &h : hot.All()) {
                    TrackWorld::Beacon b;
                    b.x = h.x, b.y = h.y, b.radius = 5.0f;
                    switch (h.kind()) {
                    case Career::Hotspot::kCrib: b.r = 80, b.g = 255, b.b = 120; break;
                    case Career::Hotspot::kCarLot: b.r = 255, b.g = 220, b.b = 60; break;
                    case Career::Hotspot::kPerformance: b.r = 255, b.g = 70, b.b = 60; break;
                    default: b.r = 70, b.g = 190, b.b = 255; break;
                    }
                    beacons.push_back(b);
                }
                // Career event locators (ISSUES #303): a white-gold column where an open event starts.
                std::vector<uint8_t> hotPaths = ReadGameFile(*fs, hotPath);
                Career::TriggerTable &trig = Career::TriggerTable::Get();
                printf("[Triggers] %zu placed in %s\n", trig.Load(globalB.data(), globalB.size(), hotPaths), hotPath);
                size_t openEvents = 0;
                for (const Career::Trigger &t : trig.All())
                    if (t.kind == Career::Trigger::kEvent && startup.FreeRoamEventOpen(t.hash)) {
                        TrackWorld::Beacon b;
                        b.x = t.x, b.y = t.y, b.radius = 4.0f, b.r = 255, b.g = 235, b.b = 170;
                        beacons.push_back(b);
                        poiDots.push_back({t.x, t.y, 255, 235, 170});
                        ++openEvents;
                    }
                printf("[Triggers] %zu event locators open\n", openEvents);
                size_t openShows = 0;
                for (const Career::Trigger &t : trig.All()) {
                    FEStartupSequence::ShowInfo si;
                    if (t.kind == Career::Trigger::kShow && startup.ShowFor(t.name, si) && si.open && !si.won) {
                        TrackWorld::Beacon b;
                        b.x = t.x, b.y = t.y, b.radius = 4.0f, b.r = 190, b.g = 90, b.b = 255;
                        beacons.push_back(b);
                        poiDots.push_back({t.x, t.y, 190, 90, 255});
                        ++openShows;
                    }
                }
                printf("[Triggers] %zu car shows open\n", openShows);
                size_t openUniques = 0;
                for (const Career::Trigger &t : trig.All()) {
                    FEStartupSequence::UniqueInfo ui;
                    if ((t.kind == Career::Trigger::kUniqueVisual || t.kind == Career::Trigger::kUniquePerf) && startup.UniqueFor(t.name, ui) && ui.open && !ui.claimed && ui.supported) {
                        TrackWorld::Beacon b;
                        b.x = t.x, b.y = t.y, b.radius = 4.0f, b.r = 255, b.g = 150, b.b = 40;
                        beacons.push_back(b);
                        poiDots.push_back({t.x, t.y, 255, 150, 40});
                        printf("[Triggers] unique %s at %.1f %.1f z %.1f (%zu parts)\n", t.name.c_str(), t.x, t.y, t.z, ui.options.size());
                        ++openUniques;
                    }
                }
                printf("[Triggers] %zu visual uniques open\n", openUniques);
            }
            world.SetBeacons(beacons);
        }
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
                    world.AddCar(&c.Model(), &c.TriangleTextures(), kTrafficPaints[(&c - trafficCars.data()) % 5],
                                 nullptr, SDL_Color{38, 38, 38, 255}, &c.TriangleCalipers(),
                                 SDL_Color{128, 128, 128, 255});
                traffic.Init(&trafficNet, &trafficCars, 0x2a5eu + unsigned(raceNumber));
                if (getenv("NFSU2_WORLD_DEBUG"))
                    for (const std::string &n : pool)
                        printf("[World]   traffic model %s: geometry %zu bytes\n", n.c_str(), ReadCarGeometry(*fs, n).size());
                printf("[World] traffic: %zu cars, %zu lane links\n", trafficCars.size(), trafficNet.Links().size());
            }
        }
        // The police (ISSUES #318): the Skoda converted by tools/convert_police.py into <data>/police/police.pcar, on the lane network.
        pursuit = Pursuit();
        if (!flyCamera && raceConfig.type == 9 && !getenv("NFSU2_NO_POLICE")) {
            if (!policeMeshTried) {
                policeMeshTried = true;
                const char *dir = getenv("NFSU2_DATA");
                const std::string path = std::string(dir ? dir : ".") + "/police/police.pcar";
                if (FILE *pf = fopen(path.c_str(), "rb")) {
                    std::vector<uint8_t> bytes;
                    fseek(pf, 0, SEEK_END);
                    bytes.resize(size_t(ftell(pf)));
                    fseek(pf, 0, SEEK_SET);
                    if (fread(bytes.data(), 1, bytes.size(), pf) != bytes.size())
                        bytes.clear();
                    fclose(pf);
                    policeMeshOk = policeMesh.Load(bytes);
                    if (policeMeshOk) { // the far model beside it
                        const std::string lodPath = std::string(dir ? dir : ".") + "/police/police_lo.pcar";
                        if (FILE *lf = fopen(lodPath.c_str(), "rb")) {
                            std::vector<uint8_t> lb;
                            fseek(lf, 0, SEEK_END);
                            lb.resize(size_t(ftell(lf)));
                            fseek(lf, 0, SEEK_SET);
                            if (fread(lb.data(), 1, lb.size(), lf) != lb.size())
                                lb.clear();
                            fclose(lf);
                            policeLodOk = policeLodMesh.Load(lb);
                        }
                    }
                    printf("[Police] %s: %s (%zu triangles)\n", path.c_str(), policeMeshOk ? "loaded" : "unreadable", policeMesh.model.size());
                } else {
                    printf("[Police] no %s (run tools/convert_police.py); the police stay off\n", path.c_str());
                }
            }
            if (policeMeshOk) {
                if (!trafficNet.Valid())
                    trafficNet.Load(ReadGameFile(*fs, pathsPath));
                std::string baseName = fe.PreviewCarName();
                for (const std::string &n : fe.RaceableCars())
                    if (n == "SKYLINE" || n == "LANCER" || n == "350Z") {
                        baseName = n;
                        break;
                    }
                const uint8_t *rec = fe.CarRecordData(baseName);
                if (rec && pursuit.Init(world, policeMesh, policeLodOk ? &policeLodMesh : nullptr, rec, &trafficNet, getenv("NFSU2_POLICE_MAX") ? atoi(getenv("NFSU2_POLICE_MAX")) : 7))
                    printf("[Police] ready (physics of %s)\n", baseName.c_str());
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
        // Dev aid / crib environment (ISSUES #251): NFSU2_SCENE=<name> replaces the world with FRONTEND\\PLATFORMS\\<name>.BIN.
        // The career crib stands in the garage room (CRIB01.BIN: three display discs and the room around them); the
        // player's car sits on the front disc and the camera sways in front of it (ISSUES #251). NFSU2_SCENE=off
        // keeps the old crib over the city.
        sceneMode = false;
        {
            const char *sceneEnv = getenv("NFSU2_SCENE");
            const std::string scene = sceneEnv ? sceneEnv : (cribSession ? "CRIB01" : "");
            if (!scene.empty() && scene != "off" &&
                world.LoadScene(ReadGameFile(*fs, ("FRONTEND\\PLATFORMS\\" + scene + ".BIN").c_str()))) {
                sceneMode = true;
                car.Place(0.0f, 5.4f, 0.15f, -1.5707963f); // nose toward the entrance
                camera = TrackWorld::Camera();
                camera.x = 0.0f, camera.y = -2.0f, camera.z = 1.8f, camera.yaw = 1.5707963f, camera.pitch = -0.1f;
                // The other two discs hold the career's other garage cars (stock look plus their paint).
                garageCars.clear();
                garageCars.reserve(2); // the world keeps pointers to the models
                if (cribSession || getenv("NFSU2_GARAGE_CARS")) {
                    static const float kDisc[2][3] = {{4.7f, -2.7f, -1.15f}, {-4.7f, -2.7f, -2.0f}}; // x, y, yaw
                    const Career::Profile &prof = startup.CareerProfile();
                    for (size_t i = 0; i < prof.cars.size() && garageCars.size() < 2; ++i) {
                        if (int(i) == prof.currentCar)
                            continue;
                        const Career::OwnedCar &oc = prof.cars[i];
                        garageCars.emplace_back();
                        PlayerCar &g = garageCars.back();
                        PlayerCar::Wheel gw[4];
                        if (!g.LoadModel(ReadCarGeometry(*fs, oc.model), fe.CarWheels(oc.model, gw) ? gw : nullptr)) {
                            garageCars.pop_back();
                            continue;
                        }
                        const SDL_Color paint = fe.ColorForPaint(oc.paintSet ? oc.paintSet : PaintCatalog::kGloss, oc.paint,
                                                                 fe.StockPaintColor(oc.model, 0, SDL_Color{40, 90, 200, 255}));
                        const size_t idx = world.AddCar(&g.Model(), &g.TriangleTextures(), paint, &g.TriangleRims());
                        world.AddTexturePack(ReadGameFile(*fs, ("CARS\\" + oc.model + "\\TEXTURES.BIN").c_str()));
                        const float *slot = kDisc[garageCars.size() - 1];
                        g.Place(slot[0], slot[1], 0.15f, slot[2]);
                        float m[12];
                        g.Transform(m);
                        world.SetCarWheels(idx, &g.TriangleWheels(), &g.TriangleBrakes(), g.WheelHubs(), 0.0f, 0.0f);
                        world.SetCarTransform(idx, m);
                        printf("[Garage] %s on disc %zu\n", oc.model.c_str(), garageCars.size());
                    }
                }
            }
        }
        if (const char *devPkg = getenv("NFSU2_WORLD_PKG")) { // dev aid: show an in-game bundle screen over the world
            cribPkg = engine->PushPackage(devPkg, 0, 0xff);
            printf("[World] dev package %s %s\n", devPkg, cribPkg ? "shown" : "missing");
        }
        // Dev aid: NFSU2_ROUTE_CHECK=1 lists stretches of the race route (and +-2.5 m beside it) with no ground.
        // Dev aid: NFSU2_WALLS_AT="x,y,r" lists the collision wall faces within r metres (ISSUES #155).
        if (const char *at = getenv("NFSU2_WALLS_AT")) {
            float wx = 0, wy = 0, wr = 8;
            sscanf(at, "%f,%f,%f", &wx, &wy, &wr);
            world.DumpWalls(wx, wy, wr);
        }
        // Dev aid: which barrier groups' walls does the race route run through (ISSUES #147)?
        if (getenv("NFSU2_GROUP_DUMP") && route.Valid()) {
            for (size_t g = 1; g < world.BarrierGroupCount(); ++g) {
                int hits = 0, samples = 0;
                for (float d = 0; d + 2.0f <= route.Length(); d += 2.0f) {
                    float x0, y0, x1, y1, dx, dy, z0 = 0, z1 = 0;
                    route.Sample(d, x0, y0, dx, dy);
                    route.Sample(d + 2.0f, x1, y1, dx, dy);
                    world.GroundHeight(x0, y0, 80.0f, z0);
                    world.GroundHeight(x1, y1, 80.0f, z1);
                    const float a[3] = {x0, y0, z0 + 0.8f}, b[3] = {x1, y1, z1 + 0.8f};
                    ++samples;
                    hits += world.CollisionWall(a, b, nullptr, int(g)) ? 1 : 0;
                }
                printf("[Group] %zu %s crosses the route %d / %d\n", g, world.BarrierGroupName(g).c_str(), hits, samples);
            }
        }
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
                    char label[64] = "";
                    if (o->Type == FE_String)
                        snprintf(label, sizeof(label), " label %08x", static_cast<FEString *>(o)->GetLabelHash());
                    else if (o->Type == FE_Image)
                        snprintf(label, sizeof(label), " tex %08x", o->Handle);
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
        world.UpdateMemory(); // the residency report has the world's numbers from the first frame (#331)
        inWorld = true;
    };
    if (getenv("NFSU2_DYNO_BENCH")) { // dev aid (ISSUES #293): every car flat out on a bare plane: times vs the dyno model
        std::vector<std::string> all = fe.RaceableCars();
        if (const char *only = getenv("NFSU2_BENCH_CAR"))
            all = {only};
        printf("[Bench] %-11s %7s %7s %7s %7s %7s | %7s %7s %7s | %s\n", "car", "0-60mph", "0-100km", "0-100mp", "1/4mile", "top km/h", "model60", "model100", "modelTop", "notes");
        for (const std::string &name : all) {
            PlayerCar::Performance perf;
            PlayerCar::Wheel w[4];
            const uint8_t *rec = fe.CarRecordData(name);
            std::vector<uint8_t> geometry = ReadCarGeometry(*fs, name);
            if (!rec || !fe.CarPerformance(name, perf) || geometry.empty())
                continue;
            PlayerCar bc;
            if (!bc.LoadModel(geometry, fe.CarWheels(name, w) ? w : nullptr))
                continue;
            bc.SetPerformance(perf);
            if (!bc.UseUG2Physics(rec))
                continue;
            TrackWorld flat;
            flat.SetFlatGround(0.0f);
            bc.Place(0, 0, 0, 0);
            float t = 0, t60 = 0, t100k = 0, t100m = 0, tq = 0, top = 0;
            const float dt = 1.0f / 120.0f;
            for (int i = 0; i < 120 * 90; ++i, t += dt) { // 90 s: enough to reach the limiter
                bc.Update(flat, 1.0f, 0.0f, 0.0f, dt, true, 1.0f);
                const float v = bc.Speed();
                top = std::max(top, v);
                if (!t60 && v >= 26.82f) t60 = t;
                if (!t100k && v >= 27.78f) t100k = t;
                if (!t100m && v >= 44.70f) t100m = t;
                if (!tq && std::hypot(bc.X(), bc.Y()) >= 402.3f) tq = t;
            }
            // Braking: 100 km/h to a standstill, full brake, straight. Grip: 22 m/s on a part-throttle skidpad turn at half lock.
            float brakeDist = 0, latG = 0;
            {
                PlayerCar b2;
                if (b2.LoadModel(geometry, fe.CarWheels(name, w) ? w : nullptr)) {
                    b2.SetPerformance(perf), b2.UseUG2Physics(rec), b2.Place(0, 0, 0, 0);
                    for (int i = 0; i < 120 * 60 && b2.Speed() < 27.78f; ++i)
                        b2.Update(flat, 1.0f, 0.0f, 0.0f, dt, true, 1.0f);
                    const float x0 = b2.X(), y0 = b2.Y();
                    for (int i = 0; i < 120 * 12 && b2.Speed() > 0.3f; ++i)
                        b2.Update(flat, 0.0f, 1.0f, 0.0f, dt, true, 1.0f);
                    brakeDist = std::hypot(b2.X() - x0, b2.Y() - y0);
                }
                PlayerCar c2;
                if (c2.LoadModel(geometry, fe.CarWheels(name, w) ? w : nullptr)) {
                    c2.SetPerformance(perf), c2.UseUG2Physics(rec), c2.Place(0, 0, 0, 0);
                    float lastYaw = 0, yawRate = 0;
                    for (int i = 0; i < 120 * 40; ++i) {
                        const float thr = c2.Speed() < 22.0f ? 1.0f : 0.25f;
                        c2.Update(flat, thr, 0.0f, i > 120 * 12 ? 0.55f : 0.0f, dt, true, 1.0f);
                        yawRate = std::remainder(c2.Yaw() - lastYaw, 6.2831853f) / dt, lastYaw = c2.Yaw();
                        if (i > 120 * 25)
                            latG = std::max(latG, std::fabs(yawRate) * c2.Speed() / 9.81f);
                    }
                }
            }
            PlayerCar::RunTimes rt = PlayerCar::SimulateRun(perf, w[0].radius > 0 ? w[0].radius : 0.32f);
            printf("[Bench] %-11s %7.2f %7.2f %7.2f %7.2f %7.0f | %7.2f %7.2f %7.0f | limiter %.0f km/h | 100-0 %.1f m, lateral %.2f g\n", name.c_str(), t60,
                   t100k, t100m, tq, top * 3.6f, rt.zeroTo60mph, rt.zeroTo100mph, rt.topSpeed * 3.6f, perf.limiter * 1.609f, brakeDist, latG);
        }
        fflush(stdout);
        _exit(0); // skip the static destructors (the FEng pools are not torn down in order)
    }
    if (getenv("NFSU2_STRAIGHT_TEST")) { // dev aid: the longest straight lane chain of L4RA (a strip for physics benchmarks)
        TrafficNet net;
        const std::vector<uint8_t> data = ReadGameFile(*fs, "TRACKS\\ROUTESL4RA\\PATHSFREEROAM.BIN");
        if (net.Load(data)) {
            const auto &L = net.Links();
            float best = 0, bx = 0, by = 0, bdx = 0, bdy = 0;
            for (size_t i = 0; i < L.size(); ++i) {
                float len = 0, dx0 = L[i].x1 - L[i].x0, dy0 = L[i].y1 - L[i].y0;
                const float l0 = std::hypot(dx0, dy0);
                if (l0 < 1.0f)
                    continue;
                dx0 /= l0, dy0 /= l0;
                int k = int(i);
                int guard = 0;
                while (k >= 0 && guard++ < 200) {
                    float dx = L[k].x1 - L[k].x0, dy = L[k].y1 - L[k].y0;
                    const float l = std::hypot(dx, dy);
                    if (l < 0.5f || (dx * dx0 + dy * dy0) / l < 0.9995f)
                        break;
                    len += l;
                    k = L[k].next;
                }
                if (len > best)
                    best = len, bx = L[i].x0, by = L[i].y0, bdx = dx0, bdy = dy0;
            }
            printf("[Straight] longest %.0f m from (%.1f, %.1f) heading %.4f rad\n", best, bx, by, std::atan2(bdy, bdx));
        }
    }
    if (getenv("NFSU2_CITYMAP_TEST")) { // dev aid: extents of every region's free-roam road network (ISSUES #293)
        for (const char *reg : {"L4RA", "L4RB", "L4RC", "L4RD", "L4RE", "L4RF", "L4RG"}) {
            TrafficNet net;
            const std::vector<uint8_t> data = ReadGameFile(*fs, (std::string("TRACKS\\ROUTES") + reg + "\\PATHSFREEROAM.BIN").c_str());
            if (data.empty() || !net.Load(data)) {
                printf("[CityMap] %s: no free-roam network\n", reg);
                continue;
            }
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (const TrafficNet::Link &l : net.Links())
                x0 = std::min({x0, l.x0, l.x1}), y0 = std::min({y0, l.y0, l.y1}), x1 = std::max({x1, l.x0, l.x1}), y1 = std::max({y1, l.y0, l.y1});
            printf("[CityMap] %s: %zu links, x %.0f..%.0f y %.0f..%.0f\n", reg, net.Links().size(), x0, x1, y0, y1);
        }
    }
    if (getenv("NFSU2_WHEEL_AUDIT")) { // dev aid: does every car get four wheels, and where is each hub (ISSUES #289)?
        std::vector<std::string> all = fe.RaceableCars();
        for (const std::string &n : fe.TrafficCars())
            all.push_back(n);
        for (const std::string &name : all) {
            PlayerCar probe;
            PlayerCar::Wheel w[4];
            const bool haveDb = fe.CarWheels(name, w);
            const bool ok = probe.LoadModel(ReadCarGeometry(*fs, name), haveDb ? w : nullptr);
            int count[4] = {};
            for (int8_t k : probe.TriangleWheels())
                if (k >= 0 && k < 4)
                    ++count[k];
            const float *hub = probe.WheelHubs();
            // Where is the axle really? The centroid of the rim-face vertices of wheel 0 against the pivot the wheel spins about.
            float ax = 0, az = 0, nAx = 0, tx = 0, tz = 0, nT = 0;
            for (size_t t = 0; t < probe.Model().size(); ++t)
                if (probe.TriangleWheels()[t] == 0) {
                    for (const SolidMesh::Point &pt : probe.Model()[t].v) {
                        if (probe.TriangleRims()[t])
                            ax += pt.x, az += pt.z, nAx += 1;
                        tx += pt.x, tz += pt.z, nT += 1;
                    }
                }
            if (nAx > 0 && hub)
                printf("[WheelAxle] %-12s rim-face centre (%.3f, %.3f) spin pivot (%.3f, %.3f) -> off by %.3f m; all-vertex centroid (%.3f, %.3f)\n", name.c_str(),
                       ax / nAx, az / nAx, hub[0], hub[2], std::hypot(ax / nAx - hub[0], az / nAx - hub[2]), tx / nT, tz / nT);
            printf("[WheelAudit] %-12s load %s db %s wheel tris %d %d %d %d  hubs x %.2f %.2f %.2f %.2f y %.2f z %.2f\n", name.c_str(),
                   ok ? "ok " : "FAIL", haveDb ? "yes" : "NO ", count[0], count[1], count[2], count[3], hub[0], hub[3], hub[6], hub[9],
                   hub[1], hub[2]);
        }
    }
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
            // NFSU2_CAR_TEST_LEVEL=n also prints the car with every level-n performance package fitted (#200).
            if (const char *lv = getenv("NFSU2_CAR_TEST_LEVEL")) {
                std::vector<uint16_t> parts;
                for (size_t i = 0; i < fe.PerfParts().Count(); ++i)
                    if (fe.PerfParts().At(i).level == uint32_t(atoi(lv)) &&
                        (!getenv("NFSU2_CAR_TEST_CAT") || fe.PerfParts().At(i).category == uint32_t(atoi(getenv("NFSU2_CAR_TEST_CAT")))))
                        for (const PerfPart &pp : fe.PerfParts().At(i).parts)
                            parts.push_back(uint16_t(pp.id));
                const std::vector<uint8_t> tuned = fe.TunedCarRecord(name, parts);
                PlayerCar::Performance tp;
                FENativeInterface::ReadCarPerformance(tuned.data(), tp);
                float tt100, tvmax;
                PlayerCar::Simulate(tp, fe.CarWheels(name, w) ? w[0].radius : 0.32f, tt100, tvmax);
                printf("[CarTest]   +L%s parts: %.2f t  %d gears  %4.0f/%4.0f rpm  peak %.0f Nm  0-100 %.1f s  top %.0f km/h\n",
                       lv, tp.mass, tp.numGears, tp.redline, tp.maxRpm,
                       1000 * *std::max_element(tp.torque, tp.torque + 9), tt100, tvmax * 3.6f);
            }
        }
    if (const char *region = getenv("NFSU2_WORLD"))
        enterWorld(region);


    Perf::Init();
    if (getenv("NFSU2_MEM_OVERLAY")) // start with the residency overlay on (screenshots, #331)
        Mem::ToggleOverlay();
    // --mem-report: boot the normal flow, then print the residency report of the loaded world and quit.
    const bool memReport = argc > 1 && strcmp(argv[1], "--mem-report") == 0;
    Uint32 lastMemScan = 0;
    // The texture budget of the residency manager (#332): 192 MB by default, 0 = never trim.
    const int texBudgetMB = getenv("NFSU2_TEX_BUDGET_MB") ? atoi(getenv("NFSU2_TEX_BUDGET_MB")) : 192;
    const double texIdle = getenv("NFSU2_TEX_IDLE") ? atof(getenv("NFSU2_TEX_IDLE")) : 10.0;
    for (bool running = true; running;) {
        Perf::BeginFrame();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F10)) // Esc is Back
                running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F3 && !ev.key.repeat)
                Perf::ToggleOverlay();
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F4 && !ev.key.repeat)
                Mem::ToggleOverlay();
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F5 && !ev.key.repeat)
                Sky::NextPreset(); // day -> afternoon -> sunset -> night (ISSUES #333)
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F6 && !ev.key.repeat)
                Sky::ToggleRunning(); // freeze / run the clock
            if (ev.type == SDL_TEXTINPUT && startup.KeyboardActive()) // the name-entry keyboard (#204)
                startup.OnTextInput(ev.text.text);
        }
        if (!musicStarted && musicVolume > 0 && (inWorld || startup.IsFinished())) {
            musicStarted = true;
            if (music.Open(*fs, "SOUND\\PFDATA\\MUSICSFX.MUS"))
                music.SetVolume(musicVolume);
        }
        {   // Options volumes (the Audio list), relative to the 80% default so the default mix is unchanged.
            auto vol = [&startup](int i) { return startup.AudioVolume(i) / 0.8f; };
            const float master = vol(0);
            music.SetVolume(musicVolume * master * vol(inWorld ? 2 : 1));
            uiSound.SetVolume(0.5f * master * vol(4));
            engineSound.SetVolumes(master * vol(6), master * vol(5), master * vol(4));
        }
        race.SetMph(startup.SpeedoMph());
        music.SetPaused(pausePkg != nullptr || modernPause != 0);
        music.Update();
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - start;
        Mem::SetTime(double(elapsed) * 0.001);
        { // the time of day keeps running through the menus too (#333)
            static Uint32 lastSky = start;
            Sky::Update(double(now - lastSky) * 0.001);
            lastSky = now;
        }
        if ((inWorld || sceneMode) && elapsed - lastMemScan >= 1000) { // a cheap once-a-second refresh (ISSUES #331)
            lastMemScan = elapsed;
            world.UpdateMemory();
            // Texture budget (#332): page the least recently used textures out while over it; the cells decode
            // them again when they are next in view.  NFSU2_TEX_BUDGET_MB=0 disables the manager.
            if (texBudgetMB >= 0) {
                Mem::SetBudget(size_t(texBudgetMB) * 1048576);
                world.TrimTextures(size_t(texBudgetMB) * 1048576, texIdle);
            }
        }
        static Uint32 worldAt = 0; // when the world (or the dev scene) first became drawable
        if ((inWorld || sceneMode) && !worldAt)
            worldAt = elapsed;
        const Uint32 reportAfter = Uint32(getenv("NFSU2_MEM_REPORT_MS") ? atoi(getenv("NFSU2_MEM_REPORT_MS")) : 3000);
        if (memReport && worldAt && elapsed - worldAt >= reportAfter) {
            world.UpdateMemory();
            Mem::Print(stdout);
            fflush(stdout);
            running = false;
        }

        bool releaseInjectedPadAfterUpdate = false;
        // Optional one-update input tap for testing screen transitions (NFSU2_PRESS_MS / NFSU2_PAD_MASK).
        if (pressMs > 0 && !pressSent && elapsed >= pressMs) {
            fe.SetSimulatedPadMask(padMaskToInject);
            pressSent = true;
            releaseInjectedPadAfterUpdate = true;
        }
        // NFSU2_INPUT="ms:button,..." emits one-update taps so a held press cannot cascade through screens.
        // Buttons: up down left right accept back start.
        static Uint32 menuReachedMs = 0; // when UI_Main.fng first showed (for the menu-relative taps)
        if (!menuReachedMs && startup.CurrentPackage() && strcasecmp(startup.CurrentPackage(), "UI_Main.fng") == 0)
            menuReachedMs = std::max<Uint32>(1, elapsed);
        for (ScriptedPress &p : script) {
            if (!p.sent && (p.menuRelative ? (menuReachedMs && elapsed >= menuReachedMs + p.ms) : elapsed >= p.ms)) {
                fe.SetSimulatedPadMask(p.mask);
                p.sent = true;
                releaseInjectedPadAfterUpdate = true;
            }
        }

        if (inWorld) {
            // Paused: the simulation stands still (zero time step) while the world and HUD draw. The
            // career crib (ISSUES #141) also holds the simulation until the player returns to explore.
            const Uint32 frameStart = last;
            // Vinyl skin of the player's car: recomposed when the previewed layers or paint change
            // (the crib's Graphics Shop previews live on the world car).
            if (playerSkinIndex >= 0) {
                const std::vector<CarSkin::Layer> layers = fe.PreviewVinyls().empty() ? fe.TestVinyls() : fe.PreviewVinyls();
                const SDL_Color body = fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(), fe.StockPaintColor(fe.PreviewCarName(), 0, SDL_Color{40, 90, 200, 255}));
                std::string key = std::to_string(body.r) + "," + std::to_string(body.g) + "," + std::to_string(body.b);
                for (const CarSkin::Layer &l : layers) {
                    key += "|" + l.design;
                    for (auto &col : l.color)
                        for (uint8_t v : col)
                            key += "," + std::to_string(v);
                }
                if (key != playerSkinKey) {
                    playerSkinKey = key;
                    if (layers.empty()) {
                        world.SetCarSkin(size_t(playerSkinIndex), 0, {});
                    } else if (fe.EnsureVinylShop()) {
                        std::vector<uint32_t> skin;
                        fe.ComposeVinylSkin(body, layers, skin);
                        world.SetCarSkin(size_t(playerSkinIndex), CarSkin::DetectBodySlot(car.Model(), car.TriangleTextures(), fe.PreviewCarName()),
                                         std::move(skin));
                    }
                }
            }
            const float dt = (pausePkg || cribPkg || modernPause) ? 0.0f : std::min(0.1f, (now - last) / 1000.0f);
            u32 pad = fe.GetJoyPadMask(0);
            for (const HeldPress &h : held)
                if (elapsed >= h.from && elapsed < h.to)
                    pad |= h.mask;
            const u32 pressed = pad & ~worldLastPad;
            worldLastPad = pad;
            last = now;
            // Start opens the pause menu (UI_Pause.fng); its choice comes back through FEStartup.
            FEStartupSequence::PauseAction pauseAction = startup.TakePauseAction();
            if (optionsGuard > 0) {
                --optionsGuard;
                pauseAction = FEStartupSequence::kPauseNone;
            }
            if (optionsPush && !(pad & FENativeInterface::kPadAccept)) {
                optionsPush = false;
                pausePkg = engine->PushPackage("UI_PauseOptionsMain.fng", 0, 0xff); // the original in-race options carousel
                optionsGuard = 6, optionsFromModern = true;
            }
            if (optionsFromModern && pausePkg) {
                FEPackage *cur = pauseScreen();
                if (cur && cur->GetName() && strncasecmp(cur->GetName(), "UI_Pause.fng", 12) == 0) { // Back from the options carousel
                    engine->UnloadPackage(cur);
                    pausePkg = nullptr;
                    optionsFromModern = false;
                    modernPause = 1, modernSel = 0;
                    printf("[Pause] options closed -> dialog\n");
                }
            } else if (!pausePkg) {
                optionsFromModern = false;
            }
            if (cribPkg && pauseAction == FEStartupSequence::kPauseResume && sceneMode && !shopScene) {
                // Leaving the garage room: stream the city region back in (the explore session, HUD and all).
                engine->UnloadPackage(cribPkg);
                cribPkg = nullptr;
                sceneMode = false;
                std::string region = currentRegion;
                enterWorld(region);
                continue;
            }
            if (cribPkg && shopFromWorld && startup.PeekCareerMenuSwitchIs("UI_CareerCrib.fng")) {
                startup.TakeCareerMenuSwitch(); // Back out of a hot-spot shop returns to the street, not the crib
                pauseAction = FEStartupSequence::kPauseResume;
            }
            if (cribPkg && pauseAction == FEStartupSequence::kPauseResume) {
                shopFromWorld = false;
                engine->UnloadPackage(cribPkg);
                cribPkg = nullptr;
                if (shopScene) { // a street shop's room: drop it and put the car back on the street, no region reload (#330)
                    shopScene = false;
                    sceneMode = false;
                    world.UnloadScene();
                    world.UpdateMemory();
                    car.Place(streetPose.x, streetPose.y, streetPose.z, streetPose.yaw);
                    camera.x = streetPose.camX, camera.y = streetPose.camY, camera.z = streetPose.camZ;
                    camera.yaw = streetPose.camYaw, camera.pitch = streetPose.camPitch;
                    printf("[Hotspot] left the shop room, back at %.1f %.1f\n", streetPose.x, streetPose.y);
                }
                // Explore mode needs the race HUD (the crib session was started without one).
                hud = engine->PushPackage(currentHudName.c_str(), 0, 0xff);
                if (!hud)
                    hud = engine->PushPackage("HUD_SingleRace.fng", 0, 0xff);
                race.Begin(hud, raceConfig); // free roam restarts with no countdown
                printf("[Career] explore mode (HUD %s)\n", hud ? currentHudName.c_str() : "missing");
            }
            if (getenv("NFSU2_HOTSPOT_DEBUG"))
                printf("[Hotspot] free %d valid %d fly %d pause %d crib %d car %.1f %.1f speed %.1f\n", race.IsFreeRun(), startup.CareerProfile().valid, flyCamera, !!pausePkg, !!cribPkg, car.X(), car.Y(), car.Speed());
            // Free-roam hot spots (#269): roll into a shop's circle slowly and its screen opens over the street.
            if (!pausePkg && !cribPkg && race.IsFreeRun() && startup.CareerProfile().valid && !flyCamera) {
                const Career::Hotspot *inside = nullptr;
                for (const Career::Hotspot &h : Career::HotspotTable::Get().All())
                    if (h.Contains(car.X(), car.Y()))
                        inside = &h;
                if (!inside)
                    hotArmed = nullptr;
                else if (inside != hotArmed && std::fabs(car.Speed()) < 9.0f) {
                    hotArmed = inside;
                    const char *screen = startup.EnterShop(int(inside->kind()));
                    if (!screen)
                        printf("[Hotspot] %s: nothing to open (profile has %zu cars)\n", inside->name.c_str(), startup.CareerProfile().cars.size());
                    if (screen) {
                        printf("[Hotspot] entered %s -> %s\n", inside->name.c_str(), screen);
                        // The street shops open their own 3D room (the hotspot's scene file, e.g.
                        // PERFSHOP01.BIN) over the city still resident: with keepWorld the exit
                        // needs no region reload (ISSUES #330). The crib's separate career-start
                        // session and the car lot keep their old interactions.
                        const Career::Hotspot::Kind k = inside->kind();
                        if (k != Career::Hotspot::kCrib && k != Career::Hotspot::kCarLot) {
                            const std::string file = "FRONTEND\\PLATFORMS\\" + inside->scene;
                            if (world.LoadScene(ReadGameFile(*fs, file.c_str()), true)) {
                                shopScene = true;
                                sceneMode = true;
                                streetPose = {car.X(), car.Y(), car.Z(), car.Yaw(),
                                              camera.x, camera.y, camera.z, camera.yaw, camera.pitch};
                                car.Place(0.0f, 5.4f, 0.15f, -1.5707963f); // on the front disc, nose toward the entrance
                                world.UpdateMemory(); // the room's cells / arena join the residency report now (#331)
                                camera = TrackWorld::Camera(); // the garage rig takes over while the screen is up
                                camera.x = 0.0f, camera.y = -2.0f, camera.z = 1.8f, camera.yaw = 1.5707963f, camera.pitch = -0.1f;
                            } else {
                                printf("[Hotspot] %s: scene %s not available, the screen opens over the street\n",
                                       inside->name.c_str(), inside->scene.c_str());
                            }
                        }
                        if (hud)
                            engine->UnloadPackage(hud);
                        hud = nullptr;
                        cribPkg = engine->PushPackage(screen, 0, 0xff);
                        shopFromWorld = cribPkg != nullptr;
                    }
                }
            }
            // Free-roam career events (#303): driving into an event locator offers its event (Accept starts the race).
            if (!pausePkg && !cribPkg && !modernPause && race.IsFreeRun() && startup.CareerProfile().valid && !flyCamera) {
                const Career::Trigger *inside = nullptr;
                for (const Career::Trigger &t : Career::TriggerTable::Get().All())
                    if (t.kind == Career::Trigger::kEvent && t.Contains(car.X(), car.Y()))
                        inside = &t;
                if (!inside)
                    engageArmed = nullptr;
                else if (inside != engageArmed) {
                    engageArmed = inside;
                    if (startup.FreeRoamEventAt(inside->hash, engage)) {
                        modernPause = 4, engageSel = 0;
                        printf("[Engage] %s: %s (%s)\n", inside->name.c_str(), engage.name.c_str(), engage.type.c_str());
                    }
                }
            }
            // Tutorial-tip spots (#308): driving over one files its Game Tips message in the inbox and shows it as a toast.
            if (!pausePkg && !cribPkg && race.IsFreeRun() && startup.CareerProfile().valid && !flyCamera) {
                const Career::Trigger *inside = nullptr;
                for (const Career::Trigger &t : Career::TriggerTable::Get().All())
                    if (t.kind == Career::Trigger::kTip && t.Contains(car.X(), car.Y()))
                        inside = &t;
                if (!inside)
                    tipArmed = nullptr;
                else if (inside != tipArmed) {
                    tipArmed = inside;
                    if (startup.DeliverTip(inside->hash, tipTitle, tipBody))
                        tipStart = now;
                }
            }
            // Car-show spots (#309): attending an open show you have not won yet wins its magazine picture and reveals the cover.
            {
                static const Career::Trigger *showArmed = nullptr;
                if (!pausePkg && !cribPkg && !modernPause && race.IsFreeRun() && startup.CareerProfile().valid && !flyCamera) {
                    const Career::Trigger *inside = nullptr;
                    for (const Career::Trigger &t : Career::TriggerTable::Get().All())
                        if (t.kind == Career::Trigger::kShow && t.Contains(car.X(), car.Y()))
                            inside = &t;
                    if (!inside)
                        showArmed = nullptr;
                    else if (inside != showArmed) {
                        showArmed = inside;
                        FEStartupSequence::ShowInfo si;
                        if (startup.ShowFor(inside->name, si) && si.open && !si.won) {
                            startup.WinShow(si);
                            si.won = true;
                            coverList.assign(1, si);
                            coverSel = 0;
                            modernPause = 5;
                        }
                    }
                }
            }
            // Visual Unique spots (#319): driving into an open one offers its level-4 parts; Enter fits the highlighted part.
            {
                static const Career::Trigger *uniqueArmed = nullptr;
                if (!pausePkg && !cribPkg && !modernPause && race.IsFreeRun() && startup.CareerProfile().valid && !flyCamera) {
                    const Career::Trigger *inside = nullptr;
                    for (const Career::Trigger &t : Career::TriggerTable::Get().All())
                        if ((t.kind == Career::Trigger::kUniqueVisual || t.kind == Career::Trigger::kUniquePerf) && t.Contains(car.X(), car.Y()))
                            inside = &t;
                    if (!inside)
                        uniqueArmed = nullptr;
                    else if (inside != uniqueArmed) {
                        uniqueArmed = inside;
                        FEStartupSequence::UniqueInfo ui;
                        if (startup.UniqueFor(inside->name, ui) && ui.open && !ui.claimed) {
                            if (ui.options.empty())
                                printf("[Unique] %s: nothing to offer (category %s)\n", inside->name.c_str(),
                                       ui.supported ? "has no level-4 part for this car" : "not ported");
                            else
                                uniqueInfo = ui, uniqueSel = 0, modernPause = 8;
                        }
                    }
                }
            }
            // The modern pause dialog: Esc opens it (Resume / Restart / Options / Main Menu / Quit Game).
            const bool useModernPause = modernUi.Ready() && !getenv("NFSU2_CLASSIC_PAUSE");
            if (useModernPause) {
                // the menu entries live in fixed arrays: building a vector of strings every frame allocated
                static const char *const kFreeItems[] = {"RESUME", "MAGAZINES", "OPTIONS", "MAIN MENU", "QUIT GAME"};
                static const char *const kRaceItems[] = {"RESUME", "RESTART RACE", "OPTIONS", "MAIN MENU", "QUIT GAME"};
                const bool restartable = !race.IsFreeRun();
                const char *const *items = restartable ? kRaceItems : kFreeItems;
                const int itemCount = 5;
                if (!modernPause && !pausePkg && !cribPkg && race.GetPhase() != RaceSession::kFinished && !race.InIntro() &&
                    (pressed & (FENativeInterface::kPadStart | FENativeInterface::kPadBack))) {
                    modernPause = 1, modernSel = 0, modernYes = false;
                    printf("[Race] paused (dialog)\n");
                } else if (modernPause == 5 || modernPause == 6) { // a cover: Enter / Esc close it, Left / Right browse the collection
                    if (modernPause == 6 && !coverList.empty()) {
                        if (pressed & FENativeInterface::kPadRight)
                            coverSel = (coverSel + 1) % coverList.size();
                        if (pressed & FENativeInterface::kPadLeft)
                            coverSel = (coverSel + coverList.size() - 1) % coverList.size();
                    }
                    if (pressed & (FENativeInterface::kPadBack | FENativeInterface::kPadStart | (modernPause == 5 ? FENativeInterface::kPadAccept : 0u)))
                        modernPause = modernPause == 6 ? 1 : 0;
                } else if (modernPause == 8) { // a Unique: Up / Down choose a part, Enter fits it, Esc leaves it for later
                    const int n = int(uniqueInfo.options.size());
                    if (pressed & FENativeInterface::kPadUp)
                        uniqueSel = (uniqueSel + n - 1) % n;
                    if (pressed & FENativeInterface::kPadDown)
                        uniqueSel = (uniqueSel + 1) % n;
                    if (pressed & (FENativeInterface::kPadBack | FENativeInterface::kPadStart))
                        modernPause = 0;
                    else if (pressed & FENativeInterface::kPadAccept) {
                        startup.ClaimUnique(uniqueInfo, size_t(uniqueSel));
                        modernPause = 0;
                    }
                } else if (modernPause == 7) { // BUSTED / ESCAPED: Enter closes the result
                    if (pressed & (FENativeInterface::kPadAccept | FENativeInterface::kPadStart | FENativeInterface::kPadBack)) {
                        pursuit.Dismiss();
                        modernPause = 0;
                    }
                } else if (modernPause == 4) { // the event prompt: Left / Right choose, Enter answers, Esc declines
                    if (pressed & (FENativeInterface::kPadLeft | FENativeInterface::kPadRight | FENativeInterface::kPadUp | FENativeInterface::kPadDown))
                        engageSel ^= 1;
                    if (pressed & (FENativeInterface::kPadBack | FENativeInterface::kPadStart)) {
                        modernPause = 0;
                    } else if (pressed & FENativeInterface::kPadAccept) {
                        modernPause = 0;
                        if (engageSel == 0 && engage.event) {
                            startup.AcceptFreeRoamEvent(*engage.event);
                            std::string eventRegion;
                            if (startup.TakeRaceRequest(eventRegion)) {
                                if (hud)
                                    engine->UnloadPackage(hud);
                                hud = nullptr;
                                enterWorld(eventRegion);
                                continue;
                            }
                        }
                    }
                } else if (modernPause) {
                    const int n = itemCount;
                    if (modernPause == 1) {
                        if (pressed & FENativeInterface::kPadUp)
                            modernSel = (modernSel + n - 1) % n;
                        if (pressed & FENativeInterface::kPadDown)
                            modernSel = (modernSel + 1) % n;
                        if (pressed & (FENativeInterface::kPadBack | FENativeInterface::kPadStart)) {
                            modernPause = 0;
                            printf("[Race] resumed\n");
                        } else if (pressed & FENativeInterface::kPadAccept) {
                            const char *it = items[modernSel];
                            printf("[Pause] accept '%s' (sel %d)\n", it, modernSel);
                            if (!strcmp(it, "RESUME")) {
                                modernPause = 0;
                            } else if (!strcmp(it, "RESTART RACE")) {
                                modernPause = 0;
                                if (hud)
                                    engine->UnloadPackage(hud);
                                hud = nullptr;
                                std::string region = currentRegion;
                                enterWorld(region);
                                continue;
                            } else if (!strcmp(it, "MAGAZINES")) {
                                coverList = startup.WonShows();
                                coverSel = 0;
                                modernPause = 6;
                            } else if (!strcmp(it, "OPTIONS")) {
                                modernPause = 0;
                                optionsPush = true; // pushed once Accept is released, or the carousel would hear that press too
                            } else if (!strcmp(it, "MAIN MENU")) {
                                modernPause = 2, modernYes = false;
                            } else {
                                modernPause = 3, modernYes = false;
                            }
                        }
                    } else { // a confirmation: Left / Right choose, Enter answers, Esc cancels
                        if (pressed & (FENativeInterface::kPadLeft | FENativeInterface::kPadRight | FENativeInterface::kPadUp | FENativeInterface::kPadDown))
                            modernYes = !modernYes;
                        if (pressed & (FENativeInterface::kPadBack | FENativeInterface::kPadStart))
                            modernPause = 1;
                        else if (pressed & FENativeInterface::kPadAccept) {
                            if (modernYes && modernPause == 2)
                                modernLeave = true, modernPause = 0;
                            else if (modernYes && modernPause == 3)
                                running = false;
                            else
                                modernPause = 1;
                        }
                    }
                }
            }
            if (!pausePkg && !cribPkg && !useModernPause && race.GetPhase() != RaceSession::kFinished &&
                (pressed & (FENativeInterface::kPadStart | FENativeInterface::kPadBack))) { // Esc pauses too
                pausePkg = engine->PushPackage("UI_Pause.fng", 0, 0xff);
                printf("[Race] paused\n");
                if (getenv("NFSU2_WORLD_DEBUG"))
                    for (FEPackage *p = engine->GetPackageList()->GetFirstPackage(); p; p = p->GetNext())
                        printf("[Race]   package %s mask %x\n", p->GetName(), p->GetControlMask());
            } else if (pausePkg && pauseAction == FEStartupSequence::kPauseResume) {
                engine->UnloadPackage(pauseScreen());
                pausePkg = nullptr;
                printf("[Race] resumed\n");
            } else if (pausePkg && pauseAction == FEStartupSequence::kPauseRestart) {
                printf("[Race] restart\n");
                engine->UnloadPackage(pauseScreen());
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
                // The menu may have switched packages itself (shop screens navigate): resolve the live one.
                if (FEPackage *live = engine->GetPackageList()->FindPackage(startup.LastMenu()))
                    cribPkg = live;
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
            // A career event that paid something shows UI_PostRaceReward.fng (Bank / Reputation won) before the world is left (ISSUES #313).
            bool finishAccept = race.GetPhase() == RaceSession::kFinished && (pressed & FENativeInterface::kPadAccept);
            if (finishAccept && resultsPkg && !rewardPkg && startup.HasRaceReward()) {
                const FEStartupSequence::RaceReward rw = startup.TakeRaceReward();
                engine->UnloadPackage(resultsPkg);
                resultsPkg = nullptr;
                rewardPkg = engine->PushPackage("UI_PostRaceReward.fng", 0, 0xff);
                if (rewardPkg) {
                    char text[48];
                    snprintf(text, sizeof(text), "$%d", rw.money);
                    FENativeInterface::SetString(rewardPkg, 0x18d97a24, text);
                    snprintf(text, sizeof(text), "%d", rw.respect);
                    FENativeInterface::SetString(rewardPkg, 0x18d97a23, text);
                    FENativeInterface::SetString(rewardPkg, 0xeb406fec, rw.player.c_str());
                    resultsPkg = rewardPkg; // the screen takes the results' place until Accept
                    resultsView.reward = true, resultsView.money = rw.money, resultsView.reputation = rw.respect, resultsView.player = rw.player;
                    resultsShownAt = float(now) * 0.001f;
                    printf("[Reward] %s won $%d and %d reputation (P%d)\n", rw.player.c_str(), rw.money, rw.respect, rw.place);
                    finishAccept = false;
                } else {
                    rewardPkg = nullptr;
                }
            }
            if (((pausePkg || cribPkg) && pauseAction == FEStartupSequence::kPauseQuit) || modernLeave || finishAccept) {
                if (pausePkg)
                    engine->UnloadPackage(pauseScreen());
                pausePkg = nullptr;
                if (cribPkg)
                    engine->UnloadPackage(cribPkg);
                cribPkg = nullptr;
                if (resultsPkg)
                    engine->UnloadPackage(resultsPkg);
                resultsPkg = nullptr;
                rewardPkg = nullptr;
                if (hud)
                    engine->UnloadPackage(hud);
                hud = nullptr;
                inWorld = false;
                if (modernLeave) { // "Main Menu": drop every screen left from the flow that started the race, then show UI_Main
                    std::vector<FEPackage *> stale;
                    for (FEPackage *p = engine->GetPackageList()->GetFirstPackage(); p; p = p->GetNext())
                        stale.push_back(p);
                    for (FEPackage *p : stale)
                        engine->UnloadPackage(p);
                }
                modernPause = 0, modernLeave = false;
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                engineSound.Set(0, 0, 0, 0, false);
                startup.ReturnFromRace();
                startup.IgnoreHeldPad(pad); // the Enter that answered "Yes" must not also press Quick Race on the main menu
                if (releaseInjectedPadAfterUpdate)
                    fe.SetSimulatedPadMask(0);
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
                // Dev aids for scripted captures: NFSU2_FLY_SPIN=<rad/s> turns the camera on the spot, NFSU2_FLY_ORBIT="cx,cy,radius,zLook,rad/s"
                // circles a point (the camera keeps its own height and looks at (cx, cy, zLook)); an optional sixth value is the start angle.
                if (const char *spin = getenv("NFSU2_FLY_SPIN"))
                    camera.yaw += float(atof(spin)) * dt;
                if (const char *orb = getenv("NFSU2_FLY_ORBIT")) {
                    float ocx = 0, ocy = 0, orad = 30, olook = 0, ospd = 0.3f;
                    float oa0 = 0.0f;
                    if (sscanf(orb, "%f,%f,%f,%f,%f,%f", &ocx, &ocy, &orad, &olook, &ospd, &oa0) >= 5) {
                        static float orbitA = oa0;
                        orbitA += ospd * dt;
                        camera.x = ocx + std::cos(orbitA) * orad, camera.y = ocy + std::sin(orbitA) * orad;
                        camera.yaw = std::atan2(ocy - camera.y, ocx - camera.x);
                        camera.pitch = std::atan2(olook - camera.z, orad);
                    }
                }
            } else {
                // Drive: Up throttle, Down brake/reverse, Left/Right steer (eased for digital input).
                float target = (pad & FENativeInterface::kPadLeft) ? 1.0f : (pad & FENativeInterface::kPadRight) ? -1.0f : 0.0f;
                steer += std::clamp(target - steer, -4.0f * dt, 4.0f * dt);
                // inputThrottle also revs the drag gearbox during the countdown (the car still gets
                // no throttle until green, below).
                const float inputThrottle = (autoDrive || dragAuto || (pad & FENativeInterface::kPadUp)) ? 1.0f : 0.0f;
                float throttle = inputThrottle;
                float brake = (pad & FENativeInterface::kPadDown) ? 1.0f : 0.0f;
                static std::vector<float> rivals; // reused every frame: no heap allocation in the loop (ISSUES #282)
                rivals.clear();
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
                    const int driftScore = race.IsDrift() ? int(race.DriftTotal()) : 0;
                    if (const char *forced = getenv("NFSU2_CAREER_TEST")) // dev aid: NFSU2_CAREER_TEST=<place> pays the race as a career event finished there
                        place = std::max(1, atoi(forced));
                    // A career event's race series (ISSUES #334): each opponent slot's finishing place
                    // earns its championship points.
                    std::vector<int> rivalPlaces(oppNames.size(), 0);
                    for (size_t i = 0; i < rows.size(); ++i)
                        if (rows[i].name.rfind("RIVAL ", 0) == 0) {
                            const int slot = atoi(rows[i].name.c_str() + 6) - 1;
                            if (slot >= 0 && slot < int(rivalPlaces.size()))
                                rivalPlaces[size_t(slot)] = int(i) + 1;
                        }
                    // 0x00269f28 for a single race: (1 - player time / best rival time) * 100 (drift: score / rival score - 1).
                    float marginPct = -1.0f;
                    if (!race.IsDrift())
                        for (const PostRaceRow &r : rows)
                            if (r.name == "YOU")
                                for (const PostRaceRow &o : rows)
                                    if (o.name != "YOU" && o.time > 0 && r.time > 0) {
                                        marginPct = (1.0f - r.time / o.time) * 100.0f; // vs the best rival
                                        break;
                                    }
                    startup.AwardCareerResult(worldRaceNumber, place, race.BestLap(), driftScore, &rivalPlaces, marginPct);
                    if (race.IsDrift())
                        startup.RecordQuickRaceScore(worldRaceNumber, worldRaceReverse, driftScore);
                    else
                        startup.RecordQuickRaceTime(worldRaceNumber, worldRaceReverse, race.BestLap());
                    race.HideHud(); // the results panel replaces the HUD (its leftovers showed through)
                    if (hud)
                        engine->UnloadPackage(hud);
                    hud = nullptr;
                    resultsPkg = engine->PushPackage("UI_PostRaceResults.fng", 0, 0xff);
                    FillPostRaceResults(resultsPkg, rows);
                    resultsView = ModernUI::Results();
                    resultsShownAt = float(now) * 0.001f;
                    for (size_t ri = 0; ri < rows.size(); ++ri) {
                        ModernUI::ResultRow rr;
                        rr.name = rows[ri].name == "YOU" ? (startup.PlayerName().empty() ? "YOU" : startup.PlayerName()) : rows[ri].name;
                        rr.car = rows[ri].car;
                        rr.player = rows[ri].name == "YOU";
                        if (rows[ri].time >= 0) {
                            const int cs = int(rows[ri].time * 100.0f + 0.5f);
                            char tb[24];
                            snprintf(tb, sizeof(tb), "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
                            rr.time = tb;
                        } else
                            rr.time = "DNF";
                        if (rr.player)
                            resultsView.place = int(ri) + 1;
                        resultsView.rows.push_back(rr);
                    }
                    if (race.IsDrift())
                        resultsView.detail = "DRIFT SCORE " + std::to_string(driftScore);
                    else if (race.BestLap() > 0) {
                        const int cs = int(race.BestLap() * 100.0f + 0.5f);
                        char tb[40];
                        snprintf(tb, sizeof(tb), "BEST LAP  %d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
                        resultsView.detail = tb;
                    }
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
                                std::max(throttle, inputThrottle), !pausePkg && !modernPause);
                engineSound.SetMotion(car.Speed(), car.Slip());
                engineSound.SetWindBoost(car.NitrousActive() ? 1.0f : 0.0f);
                if (car.Gear() != lastGear) {
                    if (lastGear > 0 && car.Gear() > 0)
                        engineSound.Shift(car.Gear() - lastGear);
                    lastGear = car.Gear();
                }
                // Manual transmission (Options > Player): E / page-down shifts up, F down (pad R-trigger shifts up).
                {
                    static bool shiftDownWas = false;
                    const bool down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F] != 0;
                    car.SetManual(startup.ManualTransmission() && !race.IsDrag());
                    if (startup.ManualTransmission() && !race.IsDrag() && race.GetPhase() == RaceSession::kRacing) {
                        if (pressed & FENativeInterface::kPadRTrigger)
                            car.ManualShift(+1);
                        if (down && !shiftDownWas)
                            car.ManualShift(-1);
                    }
                    shiftDownWas = down;
                }
                // Drag race (ISSUES #115): the gearbox drives the engine torque; R2/Accept upshift.
                float power = 1.0f;
                if (race.IsDrag() && race.GetPhase() != RaceSession::kFinished) {
                    bool shift = (pressed & (FENativeInterface::kPadRTrigger | FENativeInterface::kPadAccept)) != 0;
                    if (dragAuto && race.NeedShift())
                        shift = true;
                    power = race.DragUpdate(car.Speed(), inputThrottle, shift, dt);
                }
                // Nitrous (ISSUES #217): Space / Accept held (Q in drag races, where Accept shifts); the
                // tank refills while the car slides.  NFSU2_NOS_AUTO=1 holds the button for tests.
                {
                    static const bool nosAuto = getenv("NFSU2_NOS_AUTO") != nullptr;
                    const bool held = (pad & FENativeInterface::kPadLTrigger) ||
                                      (!race.IsDrag() && (pad & FENativeInterface::kPadAccept)) || nosAuto;
                    static bool nosWasActive = false; // a burst that started in the last update plays its sound
                    if (car.NitrousActive() && !nosWasActive)
                        engineSound.NitrousBurst(true);
                    nosWasActive = car.NitrousActive();
                    car.SetNitrousButton(held && race.GetPhase() == RaceSession::kRacing);
                    // Handbrake (ISSUES #291): X / Left Shift (pad square); the tail steps out, see Vehicle::UpdateDrift.
                    car.SetHandbrake((pad & FENativeInterface::kPadButton0) || SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_LSHIFT]);
                    // The refill of 0x0036a9e8 -> 0x003054a8, once a second: the race progress gained
                    // (percent of a lap, +0x918 * 100) x 0.01 x 0.5 of the tank; drift races use the
                    // drift score gained x 0.000667; drag races (the 0.0 factor modes) get none.
                    static float nosClock = 0, nosLastDist = 0, nosLastScore = 0;
                    nosClock += dt;
                    if (race.GetPhase() != RaceSession::kRacing) {
                        nosClock = 0;
                        nosLastDist = race.PlayerDistance();
                        nosLastScore = race.DriftTotal();
                    } else if (nosClock >= 1.0f) {
                        nosClock = 0;
                        const float dist = race.PlayerDistance(), score = race.DriftTotal();
                        if (car.HasNitrous()) {
                            if (race.IsDrift())
                                car.AddNitrous(std::max(0.0f, score - nosLastScore) * 0.000667f);
                            else if (!race.IsDrag() && race.LapLength() > 0.0f)
                                car.AddNitrous(std::max(0.0f, (dist - nosLastDist) / race.LapLength() * 100.0f) * 0.01f * 0.5f);
                        }
                        nosLastDist = dist;
                        nosLastScore = score;
                    }
                }
                if ((autoDrive || dragAuto) && route.Valid() && race.GetPhase() != RaceSession::kFinished) {
                    // Test aid: the player car follows the route like an opponent.
                    if (playerAi.Car() != &car)
                        playerAi.Init(&car, &route, autoLane, 1.0f, raceConfig.startS);
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
                // Wall hits (ISSUES #221): the swept wall test's closing speed drives the crash sound.
                {
                    static Uint32 lastWallCrash = 0;
                    const float wall = car.TakeWallImpact();
                    if (wall > 2.0f && SDL_GetTicks() - lastWallCrash > 350) {
                        engineSound.Crash(wall);
                        lastWallCrash = SDL_GetTicks();
                        if (getenv("NFSU2_COLLISION_DEBUG"))
                            printf("[Collision] wall hit %.1f m/s\n", wall);
                    }
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
                // The police and the pursuit (ISSUES #318).
                if (pursuit.Ready()) {
                    static bool roadPlaced = false;
                    if (!roadPlaced && getenv("NFSU2_PURSUIT_ROAD")) { // dev aid: start on a lane of the road network
                        roadPlaced = true;
                        pursuit.DevPlacePlayer(world, car, atoi(getenv("NFSU2_PURSUIT_ROAD")));
                    }
                    pursuit.Update(world, dt, car, race.IsFreeRun() && !flyCamera && !sceneMode && !cribPkg);
                    bool bustedNow;
                    float amount;
                    if (pursuit.TakeResult(bustedNow, amount)) {
                        const int bal = startup.AdjustMoney(bustedNow ? -int(amount) : int(amount));
                        printf("[Pursuit] %s %.0f -> balance %d\n", bustedNow ? "fine" : "reward", amount, bal);
                        modernPause = 7;
                        pursuitResultStart = SDL_GetTicks();
                    }
                    if (!sirenOpen) {
                        sirenOpen = true;
                        sirenSound.Open();
                    }
                    sirenSound.SetState(dt > 0.0f ? pursuit.NearestSiren() : -1.0f, pursuit.GetHud().heat >= 3, 1.0f);
                }
                static std::vector<PlayerCar *> cars;
                cars.clear();
                cars.push_back(&car);
                for (PlayerCar &o : opponentCars)
                    cars.push_back(&o);
                if (traffic.Size()) {
                    static std::vector<const PlayerCar *> avoid;
                    avoid.assign(cars.begin(), cars.end());
                    traffic.Update(world, dt, avoid);
                }
                for (PlayerCar &t : trafficCars)
                    cars.push_back(&t);
                for (PlayerCar *p : pursuit.Cars())
                    cars.push_back(p);
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
                // Dev aid NFSU2_COLLISION_TEST: the first opponent is held parked 45 m ahead of the
                // player's start, off-centre by 0.6 m, so a straight run hits it.
                if (getenv("NFSU2_COLLISION_TEST") && !opponentCars.empty() && race.GetPhase() == RaceSession::kRacing) {
                    static bool set = false;
                    static float px, py, pz, pyaw;
                    if (!set) {
                        set = true;
                        pyaw = car.Yaw();
                        px = car.X() + std::cos(pyaw) * 45.0f - std::sin(pyaw) * 0.6f;
                        py = car.Y() + std::sin(pyaw) * 45.0f + std::cos(pyaw) * 0.6f;
                        pz = car.Z();
                    }
                    opponentCars[0].Place(px, py, pz, pyaw);
                }
                for (size_t i = 0; i < cars.size(); ++i)
                    for (size_t j = i + 1; j < cars.size(); ++j) {
                        // Rigid-body contacts when both cars have UG2 bodies (ISSUES #219); the arcade
                        // fallback below handles cars without one.
                        const float closing = ResolveCarContact(*cars[i], *cars[j]);
                        if (getenv("NFSU2_COLLISION_DEBUG") && closing < 0.0f && i == 0 && j == 1) {
                            static bool once = false;
                            if (!once)
                                once = true, printf("[Collision] cars 0/1 have no rigid body: %d %d\n", cars[0]->Body() != nullptr, cars[1]->Body() != nullptr);
                        }
                        if (closing >= 0.0f) {
                            static Uint32 lastCrash = 0;
                            if (i == 0 && closing > 1.5f && SDL_GetTicks() - lastCrash > 350) { // the player's hit
                                engineSound.Crash(closing);
                                lastCrash = SDL_GetTicks();
                            }
                            if (closing > 0.0f && getenv("NFSU2_COLLISION_DEBUG"))
                                printf("[Collision] cars %zu/%zu closing %.1f m/s\n", i, j, closing);
                            continue;
                        }
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
                world.UpdateEffects(dt);
                for (size_t i = 0; i < cars.size(); ++i) {
                    float m[12];
                    cars[i]->Transform(m);
                    cars[i]->AdvanceWheels(dt); // the wheels roll with the speed (#257)
                    world.SetCarWheels(i, &cars[i]->TriangleWheels(), &cars[i]->TriangleBrakes(), cars[i]->WheelHubs(), cars[i]->WheelAngle(),
                                      cars[i]->WheelSteer(), cars[i]->WheelRate());
                    world.SetCarLamps(i, true, cars[i]->BrakeLamp(), cars[i]->ReverseLamp());
                    if (pursuit.Ready() && i >= pursuit.FirstWorldIndex() && i < pursuit.FirstWorldIndex() + pursuit.Count())
                        world.SetCarSiren(i, pursuit.UnitSiren(i - pursuit.FirstWorldIndex()), pursuit.Mesh()->bar);
                    // the far model follows the same pose
                    if (pursuit.Ready() && pursuit.HasLod() && i >= pursuit.FirstWorldIndex() && i < pursuit.FirstWorldIndex() + pursuit.Count()) {
                        const size_t k = i - pursuit.FirstWorldIndex(), lo = pursuit.LodWorldIndex(k);
                        PlayerCar *lc = pursuit.LodCar(k);
                        world.SetCarWheels(lo, &lc->TriangleWheels(), &lc->TriangleBrakes(), lc->WheelHubs(), cars[i]->WheelAngle(), cars[i]->WheelSteer(), cars[i]->WheelRate());
                        world.SetCarLamps(lo, true, cars[i]->BrakeLamp(), cars[i]->ReverseLamp());
                        world.SetCarSiren(lo, pursuit.UnitSiren(k), pursuit.Mesh()->bar);
                        float lm[12];
                        cars[i]->Transform(lm);
                        world.SetCarTransform(lo, lm);
                    }
                    world.SetCarNitrous(i, (cars[i]->NitrousActive() && std::fabs(cars[i]->Speed()) > 1.0f) || (i == 0 && getenv("NFSU2_FORCE_FLAMES"))); // the env is a dev aid: flames without a tank
                    // Tyre smoke and skid marks from the sliding tyres of the UG2-physics cars (ISSUES #294).
                    if (i == 0 && cars[i]->UsingUG2Physics() && dt > 0.0f && !sceneMode) {
                        static float lastX[4], lastY[4], lastZ[4];
                        static bool lastOn[4] = {};
                        static float smokeAcc[4] = {};
                        const float speed = std::fabs(cars[i]->Speed());
                        for (int wi = 0; wi < 4; ++wi) {
                            const bool on = cars[i]->WheelSliding(wi) && speed > 3.0f;
                            float cx, cy, cz;
                            cars[i]->WheelContact(wi, cx, cy, cz);
                            if (on) {
                                const float sat = cars[i]->WheelSaturation(wi);
                                const float strength = std::clamp(0.4f + sat * 0.6f, 0.3f, 1.0f);
                                if (lastOn[wi])
                                    world.AddSkid(lastX[wi], lastY[wi], lastZ[wi], cx, cy, cz, 0.24f, strength);
                                smokeAcc[wi] += dt * (9.0f + 14.0f * cars[i]->DriftAmount()) * strength;
                                while (smokeAcc[wi] >= 1.0f) {
                                    smokeAcc[wi] -= 1.0f;
                                    world.EmitSmoke(cx, cy, cz, -std::cos(cars[i]->Yaw()) * speed * 0.4f, -std::sin(cars[i]->Yaw()) * speed * 0.4f, strength);
                                }
                            }
                            lastOn[wi] = on, lastX[wi] = cx, lastY[wi] = cy, lastZ[wi] = cz;
                        }
                    }
                    if (sceneMode && i > 0)
                        m[3] = m[7] = m[11] = 1.0e5f; // the garage shows only the player's car (row-major 3x4: translation in 3/7/11)
                    world.SetCarTransform(i, m);
                    if (i == 0)
                        memcpy(garagePlayerM, m, sizeof(garagePlayerM));
                }
                if (garageBayIndex >= 0) { // the engine follows the player's car (after the loop, which hides every car past the player in the garage)
                    float bm[12];
                    memcpy(bm, garagePlayerM, sizeof(bm));
                    if (!(sceneMode && cribPkg && garageHoodOpen > 0.05f))
                        bm[3] = bm[7] = bm[11] = 1.0e5f;
                    world.SetCarTransform(size_t(garageBayIndex), bm);
                }
                // Chase camera (ISSUES #285): a spring rig behind the car; see World/CameraRig.h.
                // Options > Player > Camera: 0 Bumper / 1 Hood / 2 Close / 3 Far / 4 Super Far (distance, height).
                static const float kCamBack[5] = {0.5f, 1.2f, 5.0f, 6.5f, 9.0f}, kCamUp[5] = {0.9f, 1.3f, 1.9f, 2.3f, 3.2f};
                // 0x0025ddf0 / 0x0025de60 / 0x0025df38 demote Super Far to Far for races; only free roam (0x0025ded8) uses it.
                const int camSetting = std::clamp(startup.CameraSetting(), 0, race.IsFreeRun() ? 4 : 3);
                {
                    static int rigSetting = -1;
                    if (rigSetting != camSetting)
                        chaseRig.Reset(), rigSetting = camSetting;
                    CameraRig::Chase::In in{};
                    in.x = car.X(), in.y = car.Y(), in.z = car.Z(), in.yaw = car.Yaw(), in.speed = car.Speed();
                    in.back = kCamBack[camSetting], in.up = kCamUp[camSetting];
                    const float bx = car.X() - std::cos(car.Yaw()) * in.back, by = car.Y() - std::sin(car.Yaw()) * in.back;
                    in.groundZ = car.Z();
                    world.GroundHeight(bx, by, car.Z() + 2.0f, in.groundZ);
                    in.nitrous = car.NitrousActive();
                    in.rigid = camSetting < 2;
                    in.orbit = camOrbit;
                    in.timeS = float(now) * 0.001f;
                    camera = chaseRig.Update(world, in, dt > 0.0f ? dt : 0.016f);
                    chaseYaw = camera.yaw - camOrbit;
                }
                introBars = 0.0f;
                if (race.InIntro() && !sceneMode) {
                    const TrackWorld::Camera chaseCam = camera;
                    const IntroCamera::Result shot = IntroCamera::Evaluate(race.IntroElapsed(), race.IntroTotal(),
                                                                           {car.X(), car.Y(), car.Z(), car.Yaw()}, chaseCam);
                    camera = shot.camera;
                    introBars = shot.bars;
                    // Keep the lens out of the buildings: pull in along the line to the car (snapping in, easing out), above the road.
                    {
                        if (race.IntroElapsed() < 0.05f)
                            introClear.Reset();
                        introClear.Apply(world, car.X(), car.Y(), car.Z() + 0.7f, camera.x, camera.y, camera.z, dt > 0.0f ? dt : 0.016f);
                        float g = camera.z - 1.0f;
                        if (world.GroundHeight(camera.x, camera.y, camera.z + 1.5f, g))
                            camera.z = std::max(camera.z, g + 0.35f);
                    }
                    if ((pressed & FENativeInterface::kPadAccept) && race.IntroElapsed() > 0.8f)
                        race.SkipIntro(); // Accept skips the cinematic (not the press that started the race)
                }
                if (sceneMode && cribPkg) {
                    // Garage showroom (ISSUES #251 / #285). On the crib's own menu the focus visits each car in the garage in
                    // turn (16 s round: the player's car 9 s, the others 3.5 s each); the shop screens stay on the player's
                    // car and the camera moves to the side of it that the browsed part lives on (CameraRig::Garage).
                    const PlayerCar *shown = &car;
                    const bool topMenu = startup.LastMenu() && strcasecmp(startup.LastMenu(), "UI_CareerCrib.fng") == 0;
                    if (topMenu && !garageCars.empty()) {
                        const float phase = std::fmod(float(now) * 0.001f, 16.0f);
                        const size_t which = phase < 9.0f ? 0 : phase < 12.5f ? 1 : 2;
                        if (which >= 1 && which <= garageCars.size())
                            shown = &garageCars[which - 1];
                    }
                    GarageShot shot = topMenu ? GarageShot::Overview : startup.GarageShotNow();
                    if (const char *gs = getenv("NFSU2_GARAGE_SHOT")) // dev aid: force a shot number
                        shot = GarageShot(std::clamp(atoi(gs), 0, int(GarageShot::Count) - 1));
                    if (getenv("NFSU2_GARAGE_DEBUG")) {
                        static GarageShot lastShot = GarageShot::Count;
                        if (shot != lastShot)
                            printf("[Garage] camera shot %d (%s)\n", int(shot), startup.LastMenu()), lastShot = shot;
                    }
                    float halfLen = 2.1f;
                    {
                        static const std::vector<SolidMesh::Triangle> *sizedFor = nullptr;
                        static float sized = 2.1f;
                        if (sizedFor != &shown->Model()) {
                            float lx = 0;
                            for (const SolidMesh::Triangle &t : shown->Model())
                                for (const SolidMesh::Point &p : t.v)
                                    lx = std::max(lx, std::fabs(p.x));
                            sized = std::clamp(lx, 1.6f, 3.2f), sizedFor = &shown->Model();
                        }
                        halfLen = sized;
                    }
                    if (getenv("NFSU2_GARAGE_TOUR")) // dev aid: cycle through the shots (one every 3.2 s), for recordings
                        shot = GarageShot(int(float(now) * 0.001f / 3.2f) % int(GarageShot::Count));
                    camera = garageRig.Update(world, shot, shown->X(), shown->Y(), car.Z(), shown->Yaw(), halfLen / 2.1f,
                                              float(now) * 0.001f, dt > 0.0f ? dt : 0.016f);
                    {
                        static const char *const kTitle[] = {"THE GARAGE", "FRONT BUMPER", "REAR BUMPER", "EXHAUST", "SIDE SKIRTS", "SPOILER", "HOOD & ENGINE",
                                                             "ROOF", "HEADLIGHTS", "TAILLIGHTS", "WHEELS", "UNDERGLOW", "PAINT & LIVERY"};
                        static const char *const kSub[] = {"YOUR CAR", "STYLE", "STYLE", "SOUND AND LOOK", "AERO", "AERO", "PERFORMANCE", "STYLE", "LIGHTING", "LIGHTING",
                                                           "RIMS AND BRAKES", "NEON", "COLOUR"};
                        const float tnow = float(now) * 0.001f;
                        if (int(shot) != garageTrans.shot) {
                            const bool first = garageTrans.shot < 0 || tnow - garageTrans.start > 600.0f;
                            garageTrans.entry = first;
                            garageTrans.shot = int(shot);
                            garageTrans.start = tnow;
                            garageTrans.dur = first ? 2.2f : 1.25f;
                            garageTrans.dir = (int(shot) & 1) ? 1.0f : -1.0f;
                            garageTrans.title = kTitle[int(shot)], garageTrans.sub = kSub[int(shot)];
                        }
                        const float u = std::clamp((tnow - garageTrans.start) / garageTrans.dur, 0.0f, 1.0f);
                        garageFx = 0.0f;
                        if (u < 1.0f && !getenv("NFSU2_NO_GARAGE_TRANSITIONS")) {
                            // easeOutBack: arrives a touch past the mark and settles
                            const float c1 = 1.35f, c3 = c1 + 1.0f, q = u - 1.0f;
                            const float e = 1.0f + c3 * q * q * q + c1 * q * q;
                            const float rem = 1.0f - e;
                            const float fwdX = std::cos(camera.yaw) * std::cos(camera.pitch), fwdY = std::sin(camera.yaw) * std::cos(camera.pitch),
                                        fwdZ = std::sin(camera.pitch);
                            const float cx = shown->X(), cy = shown->Y(), cz = car.Z() + 0.7f;
                            // the point the rig is looking at: the closest point of its view ray to the car
                            const float along = std::max(1.0f, (cx - camera.x) * fwdX + (cy - camera.y) * fwdY + (cz - camera.z) * fwdZ);
                            const float fx = camera.x + fwdX * along, fy = camera.y + fwdY * along, fz = camera.z + fwdZ * along;
                            const float ent = garageTrans.entry ? 1.0f : 0.0f;
                            const float orbit = rem * garageTrans.dir * (1.25f + 0.9f * ent);
                            const float dist = 1.0f + rem * (0.85f + 1.4f * ent);
                            const float co = std::cos(orbit), so = std::sin(orbit);
                            const float rx = camera.x - cx, ry = camera.y - cy;
                            camera.x = cx + (rx * co - ry * so) * dist;
                            camera.y = cy + (rx * so + ry * co) * dist;
                            camera.z += rem * (1.5f + 3.5f * ent);
                            camera.fov += rem * (26.0f + 14.0f * ent);
                            const float ddx = fx - camera.x, ddy = fy - camera.y, ddz = fz - camera.z;
                            camera.yaw = std::atan2(ddy, ddx);
                            camera.pitch = std::atan2(ddz, std::hypot(ddx, ddy));
                            garageFx = std::clamp(rem * 1.3f, 0.0f, 1.0f);
                        }
                    }
                    // The shops open the car (ISSUES #358): the hood, boot and doors play the original's key curves from
                    // CARS\\<car>\\PARTS_ANIMATIONS.BIN forwards to open and backwards to close.
                    if (playerSkinIndex >= 0) {
                        static CarPartAnims partAnims;
                        static std::string partAnimsCar;
                        static float partT[5] = {0, 0, 0, 0, 0};
                        const std::string cn = fe.PreviewCarName();
                        if (cn != partAnimsCar) {
                            partAnimsCar = cn;
                            partAnims.Load(ReadGameFile(*fs, ("CARS\\" + cn + "\\PARTS_ANIMATIONS.BIN").c_str()));
                            for (float &v : partT)
                                v = 0.0f;
                        }
                        bool want[5] = {false, false, false, false, false};
                        if (shown == &car) {
                            switch (shot) {
                            case GarageShot::Overview: want[3] = want[4] = true; break; // the doors swing open for the showroom
                            case GarageShot::Hood: want[1] = true; break;
                            case GarageShot::RearBumper: case GarageShot::Exhaust: case GarageShot::Spoiler: case GarageShot::Taillight: want[2] = true; break;
                            default: break;
                            }
                            if (const char *op = getenv("NFSU2_GARAGE_OPEN")) { // dev aid: "hood,trunk,doors" forces panels open
                                want[1] = strstr(op, "hood") != nullptr, want[2] = strstr(op, "trunk") != nullptr;
                                want[3] = want[4] = strstr(op, "doors") != nullptr;
                            }
                        }
                        static const char *const kClip[5] = {"", "HOOD_REG", "TRUNK_REG", "LEFT_DOOR_REG", "RIGHT_DOOR_REG"};
                        float ang[5] = {0, 0, 0, 0, 0};
                        for (int p = 1; p <= 4; ++p)
                            if (const CarPartAnims::Clip *cl = partAnims.Find(kClip[p])) {
                                partT[p] = std::clamp(partT[p] + (want[p] ? 1.0f : -1.0f) * (dt > 0.0f ? dt : 0.016f) * 0.8f, 0.0f, cl->Duration());
                                ang[p] = std::fabs(cl->At(partT[p]));
                            }
                        float hinge[5][3];
                        memcpy(hinge, car.PartHinge(0), sizeof(hinge));
                        for (int pi : {1, 3, 4}) // the original's hinge points (#374) replace the geometry estimate
                            partAnims.Hinge(pi, hinge[pi]);
                        world.SetCarParts(size_t(playerSkinIndex), &car.TrianglePart(), hinge);
                        world.SetCarPartAngles(size_t(playerSkinIndex), ang);
                        garageHoodOpen = ang[1];
                    }
                    if (getenv("NFSU2_GARAGE_DEBUG")) {
                        static float lastCam = -1e9f;
                        if (float(now) * 0.001f - lastCam > 2.0f) {
                            lastCam = float(now) * 0.001f;
                            printf("[Garage] shot %d car %.2f %.2f %.2f yaw %.2f cam %.2f %.2f %.2f dist %.2f frac %.2f\n", int(shot),
                                   shown->X(), shown->Y(), car.Z(), shown->Yaw(), camera.x, camera.y, camera.z,
                                   std::hypot(camera.x - shown->X(), camera.y - shown->Y()), garageRig.clear.frac);
                        }
                    }
                    if (const char *ga = getenv("NFSU2_GARAGE_YAW")) { // dev aid: a fixed camera angle (radians from the front view)
                        const float a = -1.5707963f + float(atof(ga));
                        camera.x = shown->X() + std::cos(a) * 7.0f, camera.y = shown->Y() + std::sin(a) * 7.0f;
                        camera.z = car.Z() + 1.7f, camera.yaw = std::atan2(shown->Y() - camera.y, shown->X() - camera.x), camera.pitch = -0.1f;
                    }
                }
            }
            // The HUD minimap: a heading-up window of the road bitmap around the player with the route, the rivals and an
            // arrow (the original's TRACKMAP image is not available, ISSUES #259).
            if (hud && !sceneMode && !cribPkg && minimap.Valid() && !getenv("NFSU2_NO_MINIMAP")) {
                const int mode = race.IsFreeRun() ? startup.ExploreMapMode() : startup.RaceMapMode();
                constexpr uint32_t kMapTex = 0x4d494e49, kMapObj = 0x90fe80f3, kNorthObj = 0xf2a54430;
                if (mode != 0) {
                    static std::vector<Minimap::Dot> dots;
                    dots.clear();
                    for (const PlayerCar &o : opponentCars)
                        dots.push_back({o.X(), o.Y(), 255, 70, 70});
                    static float windowSm = 260.0f;
                    const float windowWant = 240.0f + std::min(190.0f, std::fabs(car.Speed()) * 3.4f); // zooms out with speed
                    windowSm += (windowWant - windowSm) * std::min(1.0f, 3.0f * std::max(dt, 0.001f));
                    const float window = windowSm;
                    if (race.IsFreeRun()) { // shops and open event / show / Unique spots inside the map window (#327)
                        const float reach = window * 0.46f;
                        auto near = [&](float x, float y) { return std::fabs(x - car.X()) < reach && std::fabs(y - car.Y()) < reach; };
                        for (const Career::Hotspot &h : Career::HotspotTable::Get().All())
                            if (near(h.x, h.y))
                                switch (h.kind()) {
                                case Career::Hotspot::kCrib: dots.push_back({h.x, h.y, 80, 255, 120}); break;
                                case Career::Hotspot::kCarLot: dots.push_back({h.x, h.y, 255, 220, 60}); break;
                                case Career::Hotspot::kPerformance: dots.push_back({h.x, h.y, 255, 70, 60}); break;
                                default: dots.push_back({h.x, h.y, 70, 190, 255}); break;
                                }
                        for (const Minimap::Dot &d : poiDots)
                            if (near(d.x, d.y))
                                dots.push_back(d);
                    }
                    // The Unbound-style map (ISSUES #296): drawn natively by ModernUI with the modern HUD; the FEng map is hidden then.
                    mapNative = modernUi.Ready() && race.ModernHud() && !getenv("NFSU2_CLASSIC_MAP");
                    mapWindow = window, mapRotate = mode == 2;
                    if (mapNative) {
                        mapFrame++;
                        mapFresh = (mapFrame & 1) == 0 || minimapPixels.size() != 256 * 256; // the image refreshes at 30 Hz: the arrow is live
                        if (mapFresh)
                            minimap.RenderModern(car.X(), car.Y(), car.Yaw(), window, mapRotate, route.Valid() ? &route : nullptr, dots, 256, minimapPixels);
                        if (FEObject *mo = hud->FindObjectByHash(kMapObj))
                            mo->Flags |= 1u;
                        if (FEObject *no = hud->FindObjectByHash(kNorthObj))
                            no->Flags |= 1u;
                        if (FEObject *bg = hud->FindObjectByHash(0x40cdc515)) // the original's black backing disc
                            bg->Flags |= 1u;
                    } else {
                        minimap.Render(car.X(), car.Y(), car.Yaw(), window, mode == 2, route.Valid() ? &route : nullptr, dots, 256, minimapPixels);
                        fe.SetStreamTexture(kMapTex, minimapPixels.data(), 256, 256);
                    }
                    if (!mapNative && !minimapBound) {
                        FENativeInterface::SetTextureHash(hud, kMapObj, kMapTex);
                        minimapBound = true;
                    }
                    // The north marker orbits the map (rotate mode) or stays on top (north up).
                    if (!mapNative) {
                    FEObject *mapObj = hud->FindObjectByHash(kMapObj), *north = hud->FindObjectByHash(kNorthObj);
                    if (mapObj && north && mapObj->GetObjData() && north->GetObjData()) {
                        const float a = mode == 2 ? car.Yaw() : 1.5707963f;
                        const float r = 70.0f;
                        north->GetObjData()->Pos.x = mapObj->GetObjData()->Pos.x - std::cos(a) * r;
                        north->GetObjData()->Pos.y = mapObj->GetObjData()->Pos.y - std::sin(a) * r;
                        north->Flags |= 0x400000u;
                    }
                    } // !mapNative
                } else {
                    mapNative = false; // the player turned the map off in the options
                }
            }
            // The full-screen maps (career World Map, in-game World Map): the live road map of the region replaces the
            // package's static outline, with the player / rivals / route, or the career events (ISSUES #261).
            if (cribPkg && cribPkg->GetName() && minimap.Valid() && !getenv("NFSU2_NO_MINIMAP")) {
                const bool careerMap = strcasecmp(cribPkg->GetName(), "UI_CareerWorldMap.fng") == 0;
                const bool gameMap = strcasecmp(cribPkg->GetName(), "UI_InGame_WorldMap.fng") == 0;
                // The career World Map shows the whole city (every region's free-roam road network), not just the loaded region.
                static Minimap cityMap;
                static bool cityBuilt = false;
                if (careerMap && !cityBuilt) {
                    // Load L4RA's road geometry in a throwaway world (the career garage session runs in a small region, but the events
                    // are spread over the whole city), build its map once; fall back to the lane-link network if that fails.
                    cityBuilt = true;
                    const Uint32 t0 = SDL_GetTicks();
                    bool ok = false;
                    {
                        std::vector<uint8_t> stream = ReadGameFile(*fs, "TRACKS\\STREAML4RA.BUN");
                        std::vector<std::vector<uint8_t>> nd;
                        for (const char *other : {"L4RB", "L4RC", "L4RD", "L4RF", "L4RG"}) {
                            nd.push_back(ReadGameFile(*fs, (std::string("TRACKS\\STREAM") + other + ".BUN").c_str()));
                            if (nd.back().empty())
                                nd.pop_back();
                        }
                        std::vector<const std::vector<uint8_t> *> nb;
                        for (const auto &n : nd)
                            nb.push_back(&n);
                        if (!stream.empty()) {
                            TrackWorld cityWorld;
                            if (cityWorld.Load(stream, nb)) {
                                cityMap.Build(cityWorld.RoadTriangles());
                                ok = cityMap.Valid();
                            }
                        }
                    }
                    if (!ok) {
                        std::vector<float> segs;
                        for (const char *reg : {"L4RA", "L4RB", "L4RC", "L4RD", "L4RF", "L4RG"}) {
                            TrafficNet net;
                            const std::vector<uint8_t> data = ReadGameFile(*fs, (std::string("TRACKS\\ROUTES") + reg + "\\PATHSFREEROAM.BIN").c_str());
                            if (data.empty() || !net.Load(data))
                                continue;
                            for (const TrafficNet::Link &l : net.Links())
                                segs.insert(segs.end(), {l.x0, l.y0, l.x1, l.y1});
                        }
                        cityMap.BuildFromLinks(segs, 14.0f);
                    }
                    printf("[WorldMap] city map %s in %u ms\n", ok ? "built from L4RA road geometry" : "from lane links", SDL_GetTicks() - t0);
                }
                if (careerMap || gameMap) {
                    constexpr uint32_t kFullTex = 0x4d415046, kMapObj = 0x90fe80f3;
                    static unsigned fullFrame = 0;
                    if (fullFrame++ % 6 == 0) {
                        static std::vector<Minimap::Dot> dots;
                        dots.clear();
                        Minimap::Dot marker{0, 0, 255, 255, 255};
                        bool haveMarker = false;
                        float markerYaw = 0.0f;
                        if (careerMap) {
                            static std::map<std::string, std::vector<uint8_t>> markerCache;
                            const auto &events = startup.WorldMapEvents();
                            for (size_t i = 0; i < events.size(); ++i) {
                                if (events[i]->races.empty())
                                    continue;
                                const int race = events[i]->races[0];
                                std::string reg;
                                for (const RaceInfo &r : fe.Races())
                                    if (r.number == race)
                                        reg = r.region;
                                if (reg.empty())
                                    continue;
                                auto it = markerCache.find(reg);
                                if (it == markerCache.end())
                                    it = markerCache.emplace(reg, ReadGameFile(*fs, ("TRACKS\\ROUTES" + reg + "\\TRACKPOSMARKERSALL.BIN").c_str())).first;
                                float x = 0, y = 0;
                                const bool okMarker = !it->second.empty() && world.LineFromMarkers(it->second, race, x, y);
                                if (getenv("NFSU2_WM_DEBUG") && fullFrame == 1)
                                    printf("[WorldMap] event %zu race %d region %s marker %s (%.0f, %.0f)\n", i, race, reg.c_str(), okMarker ? "ok" : "none", x, y);
                                if (!okMarker)
                                    continue;
                                if (i == startup.WorldMapSelected())
                                    marker = {x, y, 255, 255, 255}, haveMarker = true, markerYaw = 1.5707963f;
                                else
                                    dots.push_back({x, y, 255, 214, 60});
                            }
                        } else {
                            for (const PlayerCar &o : opponentCars)
                                dots.push_back({o.X(), o.Y(), 255, 150, 40}); // the legend's orange "Opponent Car"
                            // The shops and the garage (the same colours as their light columns): the legend lists them (#293).
                            {
                                for (const Career::Hotspot &h : Career::HotspotTable::Get().All()) {
                                    switch (h.kind()) {
                                    case Career::Hotspot::kCrib: dots.push_back({h.x, h.y, 80, 255, 120}); break;
                                    case Career::Hotspot::kCarLot: dots.push_back({h.x, h.y, 255, 220, 60}); break;
                                    case Career::Hotspot::kPerformance: dots.push_back({h.x, h.y, 255, 70, 60}); break;
                                    default: dots.push_back({h.x, h.y, 70, 190, 255}); break;
                                    }
                                }
                            }
                            // The open event locators, car shows and Unique spots the HUD minimap draws (#327) belong on the full map too.
                            for (const Minimap::Dot &d : poiDots)
                                dots.push_back(d);
                            marker = {car.X(), car.Y(), 255, 255, 255};
                            markerYaw = car.Yaw();
                            haveMarker = true;
                        }
                        (careerMap && cityMap.Valid() ? cityMap : minimap).RenderFull(gameMap && route.Valid() ? &route : nullptr, dots, 512, minimapPixels, haveMarker ? &marker : nullptr, markerYaw);
                        fe.SetStreamTexture(kFullTex, minimapPixels.data(), 512, 512);
                    }
                    FENativeInterface::SetTextureHash(cribPkg, kMapObj, kFullTex);
                    if (FEObject *mo = cribPkg->FindObjectByHash(kMapObj))
                        if (mo->GetObjData()) {
                            mo->GetObjData()->Col.a = 255;
                            mo->Flags &= ~1u;
                            mo->Flags |= 0x400000u;
                        }
                }
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
            Perf::Lap(Perf::kUpdate);
            Perf::GpuBegin();
            if (getenv("NFSU2_CAM_LOG"))
                printf("[Cam] %u %.3f %.3f %.3f %.4f %.4f %.2f\n", unsigned(now), camera.x, camera.y, camera.z, camera.yaw, camera.pitch, camera.fov);
            {
                // The speed effect (ISSUES #298): from ~30 km/h, full by ~140; nitrous and the intro / menus change it.
                static float fxSm = 0, nosSm = 0;
                const bool racing = inWorld && !sceneMode && !race.InIntro() && !flyCamera && !cribPkg;
                const float want = racing ? std::clamp((std::fabs(car.Speed()) - 8.0f) / 30.0f, 0.0f, 1.0f) : (cribPkg && sceneMode ? garageFx * 0.75f : 0.0f);
                const float wantNos = racing && car.NitrousActive() ? 1.0f : 0.0f;
                const float k = std::min(1.0f, 4.0f * std::max(0.001f, float(now - lastFxTicks) * 0.001f));
                lastFxTicks = now;
                fxSm += (want - fxSm) * k, nosSm += (wantNos - nosSm) * k;
                const bool rigid = startup.CameraSetting() < 2; // bumper / hood: no car in view
                world.SetSpeedFx(std::min(1.0f, fxSm * (0.78f + 0.3f * nosSm)), nosSm, 0.5f, 0.34f, rigid ? 0.0f : 0.16f, rigid ? 0.0f : 0.19f);
            }
            const bool glWorldOk = world.RenderGL(ren, camera, w, h);
            Perf::GpuEnd();
            Perf::Lap(Perf::kWorld);
            if (!glWorldOk)
                world.Render(ren, camera, w, h);
            else if (startup.RearviewMirror() && inWorld && !cribPkg && !flyCamera) {
                // Options > Gameplay > Rearview Mirror: a mirrored view from the car's rear in a strip at the top
                // centre of the screen (the original's mirror is a HUD element, ISSUES #245).
                const int iw = w * 30 / 100, ih = h * 15 / 100, ix = (w - iw) / 2, iy = h * 3 / 100;
                TrackWorld::Camera rear = camera;
                rear.x = car.X() - std::cos(car.Yaw()) * 2.4f;
                rear.y = car.Y() - std::sin(car.Yaw()) * 2.4f;
                rear.z = car.Z() + 1.25f;
                rear.yaw = car.Yaw() + 3.14159265f;
                rear.pitch = -0.03f;
                const TrackWorld::Inset inset{ix, iy, iw, ih, true};
                world.RenderGL(ren, rear, w, h, &inset);
                SDL_SetRenderDrawColor(ren, 10, 10, 10, 255);
                for (int k = 0; k < 3; ++k) {
                    const SDL_Rect border{ix - k, iy - k, iw + 2 * k, ih + 2 * k};
                    SDL_RenderDrawRect(ren, &border);
                }
            }
            if (introBars > 0.01f) { // the letterbox of the pre-race cinematic
                const int bh = int(float(h) * 0.105f * introBars);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
                const SDL_Rect top{0, 0, w, bh}, bottom{0, h - bh, w, bh};
                SDL_RenderFillRect(ren, &top);
                SDL_RenderFillRect(ren, &bottom);
            }
            SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            // The garage menu (ISSUES #361): the crib's FEng carousel stays the logic, the flat native list draws it.
            size_t garageSel = 0;
            std::vector<std::string> garageLabels;
            std::vector<char> garageDis;
            const bool nativeGarage = cribPkg && sceneMode && modernUi.Ready() && !pausePkg && !modernPause && !getenv("NFSU2_CLASSIC_MENU") &&
                                      startup.LastMenu() && strcasecmp(startup.LastMenu(), "UI_CareerCrib.fng") == 0 &&
                                      cribPkg->GetName() && strcasecmp(cribPkg->GetName(), "UI_CareerCrib.fng") == 0 &&
                                      startup.CribMenuState(garageSel, garageLabels, garageDis);
            bool nativeWorldMap = false;
            {
                FEStartupSequence::WorldMapInfo wi;
                if (cribPkg && modernUi.Ready() && !getenv("NFSU2_CLASSIC_MENU") && !pausePkg && !modernPause && minimapPixels.size() == 512 * 512 &&
                    startup.WorldMapState(wi)) { // the career World Map (ISSUES #375)
                    ModernUI::WorldMap wm;
                    wm.area = "REGION " + std::to_string(wi.area);
                    wm.canPrev = wi.canPrev, wm.canNext = wi.canNext, wm.selected = int(wi.selected), wm.purse = wi.purse;
                    for (const auto &r : wi.rows)
                        wm.rows.push_back({r.name, r.type, r.done});
                    wm.status = wi.status, wm.type = wi.type, wm.distance = wi.distance, wm.bestLabel = wi.bestLabel, wm.best = wi.best;
                    size_t ms = 0;
                    int wrep = -1;
                    startup.MainMenuState(ms, wm.player, wm.money, wrep);
                    wm.pixels = minimapPixels.data(), wm.mapSize = 512;
                    int mw, mh;
                    SDL_GetRendererOutputSize(ren, &mw, &mh);
                    SDL_RenderSetLogicalSize(ren, 0, 0);
                    modernUi.DrawWorldMap(mw, mh, wm, float(now) * 0.001f);
                    SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                    nativeWorldMap = true;
                }
            }
            bool nativeGarageList = false;
            if (!nativeGarage && !nativeWorldMap && ((cribPkg && sceneMode && !pausePkg && !modernPause) || (pausePkg && !modernPause))) {
                int glw, glh;
                SDL_GetRendererOutputSize(ren, &glw, &glh);
                if (pausePkg) { // the road behind a pause screen is dimmed so the list reads (ISSUES #370)
                    SDL_RenderSetLogicalSize(ren, 0, 0);
                    modernUi.DrawFade(glw, glh, 0.6f);
                    SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                }
                nativeGarageList = drawNativeList(glw, glh, float(now) * 0.001f);
            }
            fe.SetVerticalNav(nativeGarage || (nativeGarageList && nativeListVertical));
            if (nativeGarage) {
                std::vector<const char *> items;
                std::vector<char> dead = garageDis;
                std::unique_ptr<bool[]> deadB(new bool[dead.size()]);
                for (size_t k = 0; k < garageLabels.size(); ++k)
                    items.push_back(garageLabels[k].c_str()), deadB[k] = dead[k] != 0;
                ModernUI::MainMenu gm;
                gm.items = items.data(), gm.count = int(items.size()), gm.selected = int(garageSel), gm.disabled = deadB.get();
                gm.kicker = "CAREER", gm.title = "THE GARAGE";
                size_t ms = 0;
                gm.car = fe.CarDisplayName(fe.PreviewCarName());
                gm.carSub = "ON THE TURNTABLE";
                startup.MainMenuState(ms, gm.player, gm.money, gm.reputation);
                SDL_RenderSetLogicalSize(ren, 0, 0);
                modernUi.DrawMainMenu(w, h, gm, float(now) * 0.001f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            } else if (nativeWorldMap) {
                // drawn above
            } else if (nativeGarageList) {
                // drawn above
            } else if (resultsPkg && modernUi.Ready() && !getenv("NFSU2_CLASSIC_MENU") && (!resultsView.rows.empty() || resultsView.reward)) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                modernUi.DrawResults(w, h, resultsView, float(now) * 0.001f - resultsShownAt);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            } else
                engine->Render();
            if (modernUi.Ready() && hud && !pausePkg && !cribPkg && !flyCamera && race.ModernHud() && !race.InIntro() && !race.IsFreeRun() &&
                !getenv("NFSU2_CLASSIC_HUD") && (race.CountdownLeft() > 0.0f || race.GoAge() >= 0.0f)) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                modernUi.DrawCountdown(w, h, race.CountdownLeft(), race.GoAge());
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            }
            if (modernUi.Ready() && hud && !pausePkg && !cribPkg && !flyCamera && race.ModernHud() && !race.InIntro() &&
                race.GetPhase() != RaceSession::kFinished) { // the redesigned race HUD (ISSUES #276)
                SDL_RenderSetLogicalSize(ren, 0, 0);
                ModernUI::Hud mh;
                mh.speedKmh = car.Speed() * 3.6f;
                mh.rpm = car.Rpm();
                mh.rpmScale = float(race.GaugeScale());
                mh.gear = car.Gear();
                mh.hasNitrous = car.HasNitrous();
                mh.nitrous = car.NitrousFraction();
                mh.nitrousActive = car.NitrousActive();
                mh.race = !race.IsFreeRun();
                mh.position = race.Position(), mh.racers = race.Racers(), mh.lap = race.Lap(), mh.laps = race.Laps();
                mh.lapTime = race.LapTime(), mh.bestLap = race.BestLap();
                mh.mph = startup.SpeedoMph();
                mh.drifting = car.Drifting(), mh.driftAngle = car.DriftAngleDeg(), mh.driftChain = car.DriftChain();
                mh.driftMult = car.DriftMultiplier(), mh.driftBanner = car.DriftBanner(), mh.driftBank = car.DriftLastBank();
                mh.driftTotal = car.DriftTotal();
                std::string evName; // owns the HUD's event label until the draw call
                if (mapNative) {
                    static const char *const kEventNames[] = {"CIRCUIT", "SPRINT", "DRAG", "DRIFT", "U.R.L.", "RACE", "RACE", "RACE", "RACE", "FREE ROAM", "STREET X"};
                    const int rt = race.RaceType();
                    evName = race.IsFreeRun() ? "FREE ROAM" : (rt >= 0 && rt <= 10 ? kEventNames[rt] : "RACE");
                    evName += startup.SeriesRaceTag(); // " - RACE 2/3" while a career series runs
                    mh.eventName = evName.c_str();
                    mh.mapNew = mapFresh, mh.mapPixels = minimapPixels.data(), mh.mapSize = 256, mh.mapRotate = mapRotate, mh.mapYaw = car.Yaw(), mh.mapMetres = mapWindow;
                    mh.finishKm = (race.IsFreeRun() || race.RaceLength() < 50.0f || !route.Valid()) ? -1.0f : std::max(0.0f, (race.RaceLength() - race.PlayerDistance()) * 0.001f);
                }
                // Wind (ISSUES #292): streaks from ~90 km/h, full by ~200; nitrous adds a blue tunnel.
                static float windSm = 0, nitroSm = 0;
                const float frameDt = 1.0f / 60.0f;
                windSm += (std::clamp((std::fabs(car.Speed()) - 25.0f) / 30.0f, 0.0f, 1.0f) - windSm) * std::min(1.0f, frameDt * 4.0f);
                nitroSm += ((car.NitrousActive() ? 1.0f : 0.0f) - nitroSm) * std::min(1.0f, frameDt * 5.0f);
                if (!getenv("NFSU2_NO_WIND"))
                    modernUi.DrawWind(w, h, windSm, nitroSm, float(now) * 0.001f);
                modernUi.DrawHud(w, h, mh, float(now) * 0.001f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            }
            if (pursuit.Ready() && modernUi.Ready() && !flyCamera && !pausePkg && !cribPkg && pursuit.GetHud().state != Pursuit::kIdle) { // (ISSUES #318)
                SDL_RenderSetLogicalSize(ren, 0, 0);
                const Pursuit::Hud &ph = pursuit.GetHud();
                ModernUI::PursuitHud pu;
                pu.state = ph.state == Pursuit::kChase ? 1 : ph.state == Pursuit::kCooldown ? 2 : ph.state == Pursuit::kBusted ? 3 : 4;
                pu.heat = ph.heat, pu.units = ph.units, pu.bounty = ph.bounty, pu.chaseTime = ph.chaseTime, pu.bust = ph.bust, pu.cooldown = ph.cooldown;
                pu.nearest = ph.nearest, pu.banner = ph.banner, pu.resultTimer = float(now - pursuitResultStart) * 0.001f, pu.amount = ph.state == Pursuit::kBusted ? ph.fine : ph.reward;
                pu.bannerText = ph.bannerText.c_str(), pu.bannerSub = ph.bannerSub.c_str();
                modernUi.DrawPursuit(w, h, pu, float(now) * 0.001f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            }
            // A career series race start (ISSUES #334): the toast comes up once the race world is in.
            if (inWorld && modernUi.Ready() && !pausePkg && !cribPkg && (!seriesToastStart || now - seriesToastStart >= 6000)) {
                std::string t, b;
                if (startup.TakeSeriesToast(t, b)) {
                    seriesTitle = t;
                    seriesBody = b;
                    seriesToastStart = now ? now : 1;
                }
            }
            if (seriesToastStart && modernUi.Ready() && !pausePkg && !cribPkg && now - seriesToastStart < 6000) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                int tw, th;
                SDL_GetRendererOutputSize(ren, &tw, &th);
                modernUi.DrawToast(tw, th, "CAREER SERIES", seriesTitle, seriesBody,
                                   float(now - seriesToastStart) * 0.001f, 6.0f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            }
            if (tipStart && modernUi.Ready() && !pausePkg && !cribPkg && now - tipStart < 12000) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                int tw, th;
                SDL_GetRendererOutputSize(ren, &tw, &th);
                modernUi.DrawToast(tw, th, "NEW TIP  -  SAVED TO YOUR INBOX", tipTitle, tipBody, float(now - tipStart) * 0.001f, 12.0f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            }
            if ((modernPause == 5 || modernPause == 6) && modernUi.Ready()) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                int cw, ch;
                SDL_GetRendererOutputSize(ren, &cw, &ch);
                drawCoverScreen(cw, ch, float(now) * 0.001f, modernPause == 5);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            } else if (modernPause == 4 && modernUi.Ready()) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                int w, h;
                SDL_GetRendererOutputSize(ren, &w, &h);
                char l0[96], l1[96], l2[96], l3[96];
                snprintf(l0, sizeof(l0), "Race type|%s", engage.type.c_str());
                snprintf(l1, sizeof(l1), "Distance|%s", engage.distance.c_str());
                snprintf(l2, sizeof(l2), "Status|%s", engage.status.c_str());
                snprintf(l3, sizeof(l3), "Purse|$%u", engage.purse);
                const char *lines[] = {l0, l1, l2, l3};
                modernUi.DrawEngage(w, h, "EVENT", engage.name, lines, 4, engageSel, float(now) * 0.001f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            } else if (modernPause == 8 && modernUi.Ready()) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                int w, h;
                SDL_GetRendererOutputSize(ren, &w, &h);
                std::vector<const char *> names;
                for (const FEStartupSequence::UniqueInfo::Option &o : uniqueInfo.options)
                    names.push_back(o.name.c_str());
                modernUi.DrawDialog(w, h, "UNIQUE PART", names.data(), int(names.size()), uniqueSel, false, "", false, float(now) * 0.001f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            } else if (cribPkg && sceneMode && modernUi.Ready() && garageTrans.shot >= 0 && !getenv("NFSU2_NO_GARAGE_TRANSITIONS") &&
                       float(now) * 0.001f - garageTrans.start < 2.4f) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                modernUi.DrawShotTitle(w, h, garageTrans.title, garageTrans.sub, float(now) * 0.001f - garageTrans.start, garageTrans.dir, garageTrans.entry);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            } else if (modernPause == 7) {
                // the pursuit result is drawn with the pursuit HUD above
            } else if (modernPause && modernUi.Ready()) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
                static const char *const kFreeItems[] = {"RESUME", "MAGAZINES", "OPTIONS", "MAIN MENU", "QUIT GAME"};
                static const char *const kRaceItems[] = {"RESUME", "RESTART RACE", "OPTIONS", "MAIN MENU", "QUIT GAME"};
                modernUi.DrawDialog(w, h, "PAUSED", race.IsFreeRun() ? kFreeItems : kRaceItems, 5, modernSel, modernPause != 1,
                                    modernPause == 3 ? "Quit the game?" : "Back to the main menu?", modernYes, float(now) * 0.001f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            }
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
            const bool staged = modernUi.Ready() && !getenv("NFSU2_NO_STAGE") && (fe.MainMenuOpen() || fe.PreviewScreenOpen()) &&
                                drawStage(float(elapsed) * 0.001f, fe.PreviewScreenOpen());
            fe.SetPreviewSuppressed(staged);
            if (modernUi.Ready() && !staged) { // the animated full-window backdrop under the 4:3 FEng box
                SDL_RenderSetLogicalSize(ren, 0, 0);
                int bw, bh;
                SDL_GetRendererOutputSize(ren, &bw, &bh);
                modernUi.DrawBackdrop(bw, bh, float(elapsed) * 0.001f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            }
            startup.PreRender(); // after the FEng script update, before the frame is drawn
            // The main menu (ISSUES #357): the FEng carousel keeps running as the logic (input, selection, navigation) but the native menu
            // draws it.  NFSU2_CLASSIC_MENU=1 shows the original package instead.
            size_t menuSel = 0;
            std::string menuPlayer;
            int menuMoney = -1, menuRep = -1;
            const bool nativeMenu = staged && !getenv("NFSU2_CLASSIC_MENU") && !fe.PreviewScreenOpen() && fe.MainMenuOpen() && startup.CurrentPackage() &&
                                    strcasecmp(startup.CurrentPackage(), "UI_Main.fng") == 0 && !startup.IsMessageDialogOpen() &&
                                    startup.MainMenuState(menuSel, menuPlayer, menuMoney, menuRep);
            fe.SetVerticalNav(nativeMenu);
            // The other icon-carousel screens as the same flat list (ISSUES #361), over the flat backdrop.
            int nlw = 0, nlh = 0;
            SDL_GetRendererOutputSize(ren, &nlw, &nlh);
            const bool nativeList = !nativeMenu && drawNativeList(nlw, nlh, float(elapsed) * 0.001f);
            fe.SetVerticalNav(nativeMenu || (nativeList && nativeListVertical));
            if (nativeMenu) {
                static const char *const kItems[] = {"CAREER", "QUICK RACE", "2 PLAYER", "ONLINE", "CUSTOMIZE", "PROFILE", "OPTIONS"};
                static const char *const kNotes[] = {"Build your reputation across five districts.", "Jump straight into a single event.",
                                                     "Head to head on one screen.", "Not available in this build.",
                                                     "Paint, vinyls, parts and performance.", "Create, save and manage profiles.",
                                                     "Controls, video, audio and gameplay."};
                ModernUI::MainMenu mm;
                mm.items = kItems, mm.notes = kNotes, mm.count = 7, mm.selected = int(menuSel);
                mm.player = menuPlayer, mm.money = menuMoney, mm.reputation = menuRep;
                mm.car = fe.CarDisplayName(fe.PreviewCarName());
                mm.carSub = "CURRENT CAR";
                SDL_RenderSetLogicalSize(ren, 0, 0);
                int mw, mh;
                SDL_GetRendererOutputSize(ren, &mw, &mh);
                modernUi.DrawMainMenu(mw, mh, mm, float(elapsed) * 0.001f);
                SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
            } else if (!nativeList)
                engine->Render();
            // The dialog is a package on top of the shop; the preview overlay is drawn outside the
            // package stack, so it would cover the prompt (ISSUES #189).
            if (!startup.IsMessageDialogOpen())
                fe.RenderCarPreviewOverlay();
            std::string region;
            if (startup.TakeRaceRequest(region)) {
                if (modernUi.Ready() && !noTransitions) { // keep the menu frame: it wipes away at the start of the load
                    SDL_RenderSetLogicalSize(ren, 0, 0);
                    modernUi.CaptureFrame();
                    SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
                    transFromCapture = true;
                }
                enterWorld(region);
            }
        }

        // Options > Video > Brightness / Contrast (-15..15): a translucent wash over the finished frame.
        // Brightness lightens (white) or darkens (black); contrast uses mid-grey, flattening the image when
        // negative and darkening the shadows a little when positive (a stand-in for the console's gamma ramp).
        {
            const int b = startup.Brightness(), c = startup.Contrast();
            auto wash = [&](Uint8 r, Uint8 g, Uint8 bl, int alpha) {
                if (alpha <= 0)
                    return;
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, r, g, bl, Uint8(std::min(alpha, 160)));
                SDL_RenderFillRect(ren, nullptr);
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
            };
            if (b > 0)
                wash(255, 255, 255, b * 6);
            else if (b < 0)
                wash(0, 0, 0, -b * 8);
            if (c < 0)
                wash(128, 128, 128, -c * 5);
            else if (c > 0)
                wash(0, 0, 0, c * 3);
        }

        if ((Perf::OverlayOn() || Mem::OverlayOn()) && modernUi.Ready()) {
            SDL_RenderSetLogicalSize(ren, 0, 0);
            int ow, oh;
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            if (Perf::OverlayOn()) {
                char lines[10][96];
                const int n = Perf::OverlayLines(lines, 10);
                modernUi.DrawOverlay(ow, oh, lines, n);
            }
            if (Mem::OverlayOn()) { // the residency panel, top left (#331)
                char lines[20][96];
                const int n = Mem::OverlayLines(lines, 20);
                modernUi.DrawOverlay(ow, oh, lines, n, true);
            }
        }
        // NFSU2_SCREENSHOT=file.bmp: save the frame after NFSU2_SCREENSHOT_MS (default 1500) and quit.
        if (const char *shot = getenv("NFSU2_SCREENSHOT")) {
            const char *ms = getenv("NFSU2_SCREENSHOT_MS");
            if (elapsed > Uint32(ms ? atoi(ms) : 1500)) {
                int w, h;
                SDL_GetRendererOutputSize(ren, &w, &h);
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                SDL_Rect whole{0, 0, w, h}; // the whole window, not just the 4:3 box of the logical size
                SDL_RenderSetLogicalSize(ren, 0, 0);
                SDL_RenderReadPixels(ren, &whole, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
                SDL_SaveBMP(s, shot);
                SDL_FreeSurface(s);
                running = false;
            }
        }
        // Screen transitions (#311): the finished loading frame wipes away over the first frames of the game
        if (pendingReveal && modernUi.Ready()) {
            pendingReveal = false;
            transActive = true;
            transStart = transLast = SDL_GetTicks();
        }
        if (transActive && modernUi.Ready()) {
            SDL_RenderSetLogicalSize(ren, 0, 0);
            int tw, th;
            SDL_GetRendererOutputSize(ren, &tw, &th);
            transitionDraw(tw, th);
            SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);
        }
        Perf::Lap(Perf::kUI);
        SDL_RenderPresent(ren);
        Perf::Lap(Perf::kPresent);
        Perf::EndFrame();
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
