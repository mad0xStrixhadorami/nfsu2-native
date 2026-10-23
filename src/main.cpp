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
#include "game/indep/Frontend/PerfModel.h"
#include "game/indep/Frontend/TuneSystem.h"
#include "game/indep/Career/Events.h"
#include "game/indep/Career/CarLot.h"
#include "game/indep/Career/Sms.h"
#include "game/indep/Career/Sponsors.h"
#include "game/indep/Texture/TexturePack.h"
#include "game/indep/Frontend/VinylCatalog.h"
#include "game/indep/Frontend/PostRaceResults.h"
#include "game/indep/Sound/CarAudioTable.h"
#include "game/indep/Sound/EngineSound.h"
#include "game/indep/Sound/MusicStream.h"
#include "game/indep/Sound/UiSound.h"
#include "game/indep/World/AiDriver.h"
#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/CarCollision.h"
#include "game/indep/World/IntroCamera.h"
#include "game/indep/World/Minimap.h"
#include "game/indep/World/RaceRoute.h"
#include "game/indep/World/Traffic.h"
#include "game/indep/World/RaceSession.h"
#include "game/indep/World/TrackWorld.h"
#include "platform/linux/PlatformMemory.h"
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
            renderFrame(engine);
        }
        globalB = ReadGameFile(*fs, "GLOBAL\\GLOBALB.LZC");
        fe.AddBundle(globalB.data(), globalB.size(), "GLOBAL\\GLOBALB.LZC");
        Career::EventTable::Get().Load(globalB.data(), globalB.size());
        Career::SponsorTable::Get().Load(globalB.data(), globalB.size());
        Career::SmsTable::Get().Load(globalB.data(), globalB.size());
        Career::CarLotTable::Get().Load(globalB.data(), globalB.size());
        startup.LoadBootProfile();
        frontB = ReadGameFile(*fs, "FRONTEND\\FRONTB.LZC");
        fe.AddBundle(frontB.data(), frontB.size(), "FRONTEND\\FRONTB.LZC");
        // The tuning / dyno screens (UI_PerformanceTuning_*, UI_PerformanceDyno_*) ship in the in-game
        // bundle the dyno scene loads (ISSUES #203); the port keeps them resident with the front end.
        perfTuningB = ReadGameFile(*fs, "GLOBAL\\INGAMEPERFORMANCETUNING.BUN");
        if (!perfTuningB.empty())
            fe.AddBundle(perfTuningB.data(), perfTuningB.size(), "GLOBAL\\INGAMEPERFORMANCETUNING.BUN");
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
            renderFrame(engine);
        }
        // 2. LoadGlobalChunks (0x002df9a0): load GLOBAL\GLOBALB.LZC while loading_boot.fng is shown.
        globalB = ReadGameFile(*fs, "GLOBAL\\GLOBALB.LZC");
        fe.AddBundle(globalB.data(), globalB.size(), "GLOBAL\\GLOBALB.LZC");
        Career::EventTable::Get().Load(globalB.data(), globalB.size());
        Career::SponsorTable::Get().Load(globalB.data(), globalB.size());
        Career::SmsTable::Get().Load(globalB.data(), globalB.size());
        Career::CarLotTable::Get().Load(globalB.data(), globalB.size());
        startup.LoadBootProfile();
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
        // The tuning / dyno screens (UI_PerformanceTuning_*, UI_PerformanceDyno_*) ship in the in-game
        // bundle the dyno scene loads (ISSUES #203); the port keeps them resident with the front end.
        perfTuningB = ReadGameFile(*fs, "GLOBAL\\INGAMEPERFORMANCETUNING.BUN");
        if (!perfTuningB.empty())
            fe.AddBundle(perfTuningB.data(), perfTuningB.size(), "GLOBAL\\INGAMEPERFORMANCETUNING.BUN");
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
                    printf("[Sponsors]   %-16s threshold %d bonus %d race purse %d\n", s->name.c_str(),
                           area->thresholds[size_t(i)], s->signingBonus, s->racePurse);
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
            {"rtrigger", FENativeInterface::kPadRTrigger}, {"square", FENativeInterface::kPadButton0}};
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
    std::vector<std::vector<uint8_t>> hudTexturePacks; // kept alive: the front end keeps pointers into them
    float introBars = 0.0f; // letterbox amount of the pre-race camera (#265)
    Minimap minimap; // the HUD's round map (#259)
    std::vector<uint32_t> minimapPixels;
    bool minimapBound = false;
    std::vector<PlayerCar> garageCars; // the career's other cars on the garage room's discs (#252)
    std::vector<AiDriver> ai;
    TrafficNet trafficNet;
    std::vector<PlayerCar> trafficCars;
    Traffic traffic;
    std::vector<bool> aiOut; // URL knockout (ISSUES #117): opponents eliminated this race
    AiDriver playerAi; // NFSU2_AUTODRIVE with a route
    std::vector<float> aiStartS;
    std::vector<uint8_t> inGameCommon, inGameRace, inGameDrift, inGameFreeRoam;
    bool inWorld = false, flyCamera = false, sceneMode = false;
    FEPackage *pausePkg = nullptr; // UI_Pause.fng while the race is paused (ISSUES.md #122)
    // The pause menu's package changes when Options opens and closes (a package switch), so look it up.
    auto pauseScreen = [&engine, &pausePkg]() -> FEPackage * {
        for (FEPackage *p = engine->GetPackageList()->GetFirstPackage(); p; p = p->GetNext())
            if (p->GetName() && (strncasecmp(p->GetName(), "UI_Pause", 8) == 0))
                return p;
        return pausePkg;
    };
    // Career crib (UI_CareerCrib.fng): the career hub over a career free-roam session (ISSUES #141).
    FEPackage *cribPkg = nullptr;
    int playerSkinIndex = -1;       // the player's car in the world (vinyl skin, ISSUES #223)
    std::string playerSkinKey;
    // Post-race results (UI_PostRaceResults.fng): opponents' finish times on a race clock.
    FEPackage *resultsPkg = nullptr;
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
        const bool loaded = !stream.empty() && world.Load(stream, neighbours);
        std::vector<std::vector<uint8_t>>().swap(neighbourData);
        if (!loaded) {
            fprintf(stderr, "[World] could not load %s\n", path.c_str());
            return;
        }
        {
            std::vector<uint8_t> regionBundle = ReadGameFile(*fs, ("TRACKS\\" + region + ".BUN").c_str());
            world.LoadBarrierGroups(regionBundle);
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
        raceConfig.showGauges = startup.ShowGauges();
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
        // The Body Shop preview selection (#188), falling back to the dev environment.
        PlayerCar::InstalledParts installedParts = fe.InstalledPartsForPreview();
        if (!flyCamera && car.LoadModel(ReadCarGeometry(*fs, fe.PreviewCarName()),
                                        fe.CarWheels(fe.PreviewCarName(), wheels) ? wheels : nullptr,
                                        &installedParts)) {
            // The career/QR paint (OwnedCar::paintSet/paint) selects a GLOBALB color (#167/#168).
            const size_t playerCarIndex = world.AddCar(&car.Model(), &car.TriangleTextures(),
                         fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(),
                                          SDL_Color{40, 90, 200, 255}),
                         &car.TriangleRims(),
                         fe.ColorForPaint(fe.PreviewRimSet(), fe.PreviewRimPaint(),
                                          SDL_Color{38, 38, 38, 255}),
                         &car.TriangleCalipers(),
                         fe.ColorForPaint(fe.PreviewCaliperSet(), fe.PreviewCaliperPaint(),
                                          SDL_Color{128, 128, 128, 255}),
                         &car.TriangleMirrors(),
                         fe.ColorForPaint(fe.PreviewMirrorSet(), fe.PreviewMirrorPaint(),
                                          fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(),
                                                           SDL_Color{40, 90, 200, 255})),
                         &car.TriangleSpoilers(),
                         fe.ColorForPaint(fe.PreviewSpoilerSet(), fe.PreviewSpoilerPaint(),
                                          fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(),
                                                           SDL_Color{40, 90, 200, 255})),
                         &car.TriangleRoofScoops(),
                         fe.ColorForPaint(fe.PreviewRoofSet(), fe.PreviewRoofPaint(),
                                          fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(),
                                                           SDL_Color{40, 90, 200, 255})),
                         &car.TriangleEngines(),
                         fe.ColorForPaint(fe.PreviewEngineSet(), fe.PreviewEnginePaint(),
                                          SDL_Color{128, 64, 48, 255}),
                         &car.TriangleExhausts(),
                         fe.ColorForPaint(fe.PreviewExhaustSet(), fe.PreviewExhaustPaint(),
                                          SDL_Color{150, 150, 150, 255}));
            playerSkinIndex = int(playerCarIndex);
            playerSkinKey.clear(); // the per-frame check below composes the skin
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
                const size_t oppWorldIndex = world.AddCar(&opponentCars[i].Model(), &opponentCars[i].TriangleTextures(), kPaints[i % 5],
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
                            const SDL_Color bc = kPaints[i % 5];
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
                                                                 SDL_Color{40, 90, 200, 255});
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


    for (bool running = true; running;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F10)) // Esc is Back
                running = false;
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
            // Vinyl skin of the player's car: recomposed when the previewed layers or paint change
            // (the crib's Graphics Shop previews live on the world car).
            if (playerSkinIndex >= 0) {
                const std::vector<CarSkin::Layer> layers = fe.PreviewVinyls().empty() ? fe.TestVinyls() : fe.PreviewVinyls();
                const SDL_Color body = fe.ColorForPaint(fe.PreviewPaintSet(), fe.PreviewPaint(), SDL_Color{40, 90, 200, 255});
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
            if (cribPkg && pauseAction == FEStartupSequence::kPauseResume && sceneMode) {
                // Leaving the garage room: stream the city region back in (the explore session, HUD and all).
                engine->UnloadPackage(cribPkg);
                cribPkg = nullptr;
                sceneMode = false;
                std::string region = currentRegion;
                enterWorld(region);
                continue;
            }
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
            if (((pausePkg || cribPkg) && pauseAction == FEStartupSequence::kPauseQuit) ||
                (race.GetPhase() == RaceSession::kFinished && (pressed & FENativeInterface::kPadAccept))) {
                if (pausePkg)
                    engine->UnloadPackage(pauseScreen());
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
                    const int driftScore = race.IsDrift() ? int(race.DriftTotal()) : 0;
                    startup.AwardCareerResult(worldRaceNumber, place, race.BestLap(), driftScore);
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
                for (size_t i = 0; i < cars.size(); ++i) {
                    float m[12];
                    cars[i]->Transform(m);
                    cars[i]->AdvanceWheels(dt); // the wheels roll with the speed (#257)
                    world.SetCarWheels(i, &cars[i]->TriangleWheels(), &cars[i]->TriangleBrakes(), cars[i]->WheelHubs(), cars[i]->WheelAngle(),
                                      cars[i]->WheelSteer());
                    if (sceneMode && i > 0)
                        m[3] = m[7] = m[11] = 1.0e5f; // the garage shows only the player's car (row-major 3x4: translation in 3/7/11)
                    world.SetCarTransform(i, m);
                }
                // Chase camera: behind and above, lagging the car's heading.
                float diff = std::remainder(car.Yaw() - chaseYaw, 6.2831853f);
                chaseYaw += diff * std::min(1.0f, 5.0f * dt);
                const float viewYaw = chaseYaw + camOrbit; // dev aid; 0 = the normal chase view
                // Options > Player > Camera: 0 Bumper / 1 Hood / 2 Close / 3 Far / 4 Super Far (distance, height).
                static const float kCamBack[5] = {0.5f, 1.2f, 5.0f, 6.5f, 9.0f}, kCamUp[5] = {0.9f, 1.3f, 1.9f, 2.3f, 3.2f};
                // 0x0025ddf0 / 0x0025de60 / 0x0025df38 demote Super Far to Far for races; only free roam (0x0025ded8) uses it.
                const int camSetting = std::clamp(startup.CameraSetting(), 0, race.IsFreeRun() ? 4 : 3);
                camera.x = car.X() - std::cos(viewYaw) * kCamBack[camSetting];
                camera.y = car.Y() - std::sin(viewYaw) * kCamBack[camSetting];
                float camGround = car.Z();
                world.GroundHeight(camera.x, camera.y, car.Z() + 2.0f, camGround);
                camera.z = std::max(car.Z(), camGround) + kCamUp[camSetting];
                camera.yaw = viewYaw;
                camera.pitch = camOrbit == 0.0f ? -0.17f : -0.08f;
                introBars = 0.0f;
                if (race.InIntro() && !sceneMode) {
                    const TrackWorld::Camera chaseCam = camera;
                    const IntroCamera::Result shot = IntroCamera::Evaluate(race.IntroElapsed(), race.IntroTotal(),
                                                                           {car.X(), car.Y(), car.Z(), car.Yaw()}, chaseCam);
                    camera = shot.camera;
                    introBars = shot.bars;
                    // Keep the lens out of the buildings: pull the camera in along the line to the car until the segment
                    // is clear of walls, and above the road.
                    {
                        const float ax = car.X(), ay = car.Y(), az = car.Z() + 0.7f;
                        const float dx = camera.x - ax, dy = camera.y - ay, dz = camera.z - az;
                        float k = 1.0f;
                        for (; k > 0.18f; k -= 0.06f)
                            if (!world.HitsWall(ax, ay, az, ax + dx * (k + 0.04f), ay + dy * (k + 0.04f), az + dz * (k + 0.04f)))
                                break;
                        static float smoothK = 1.0f; // eased so a passing pillar does not jerk the shot
                        smoothK += (k - smoothK) * std::min(1.0f, 12.0f * std::max(dt, 0.016f));
                        k = std::min(k, std::max(smoothK, k));
                        camera.x = ax + dx * k, camera.y = ay + dy * k, camera.z = az + dz * k;
                        float g = camera.z - 1.0f;
                        if (world.GroundHeight(camera.x, camera.y, camera.z + 1.5f, g))
                            camera.z = std::max(camera.z, g + 0.35f);
                    }
                    if ((pressed & FENativeInterface::kPadAccept) && race.IntroElapsed() > 0.8f)
                        race.SkipIntro(); // Accept skips the cinematic (not the press that started the race)
                } else {
                    camera.fov = 70.0f;
                }
                if (sceneMode && cribPkg) {
                    // Garage showroom: sway 7 m in front of a car on its disc. On the crib's own menu the focus
                    // visits each car in the garage in turn (16 s round: the player's car 9 s, the others 3.5 s
                    // each); the shop screens stay on the player's car.
                    static float fx = 0, fy = 0;
                    static bool fInit = false;
                    float tx = car.X(), ty = car.Y();
                    const bool topMenu = startup.LastMenu() && strcasecmp(startup.LastMenu(), "UI_CareerCrib.fng") == 0;
                    if (topMenu && !garageCars.empty()) {
                        const float phase = std::fmod(float(now) * 0.001f, 16.0f);
                        const size_t which = phase < 9.0f ? 0 : phase < 12.5f ? 1 : 2;
                        if (which >= 1 && which <= garageCars.size())
                            tx = garageCars[which - 1].X(), ty = garageCars[which - 1].Y();
                    }
                    if (!fInit) {
                        fx = tx, fy = ty, fInit = true;
                    }
                    const float k = std::min(1.0f, 2.2f * std::max(dt, 0.016f));
                    fx += (tx - fx) * k, fy += (ty - fy) * k;
                    float a = -1.5707963f + 0.6f * std::sin(float(now) * 0.00035f);
                    if (const char *ga = getenv("NFSU2_GARAGE_YAW")) // dev aid: a fixed camera angle (radians from the front view)
                        a = -1.5707963f + float(atof(ga));
                    camera.x = fx + std::cos(a) * 7.0f;
                    camera.y = fy + std::sin(a) * 7.0f;
                    camera.z = car.Z() + 1.7f;
                    camera.yaw = std::atan2(fy - camera.y, fx - camera.x);
                    camera.pitch = -0.1f;
                }
            }
            // The HUD minimap: a heading-up window of the road bitmap around the player with the route, the rivals and an
            // arrow (the original's TRACKMAP image is not available, ISSUES #259).
            if (hud && !sceneMode && !cribPkg && minimap.Valid() && !getenv("NFSU2_NO_MINIMAP")) {
                const int mode = race.IsFreeRun() ? startup.ExploreMapMode() : startup.RaceMapMode();
                constexpr uint32_t kMapTex = 0x4d494e49, kMapObj = 0x90fe80f3, kNorthObj = 0xf2a54430;
                if (mode != 0) {
                    std::vector<Minimap::Dot> dots;
                    for (const PlayerCar &o : opponentCars)
                        dots.push_back({o.X(), o.Y(), 255, 70, 70});
                    const float window = 260.0f + std::min(160.0f, std::fabs(car.Speed()) * 3.0f); // zooms out with speed
                    minimap.Render(car.X(), car.Y(), car.Yaw(), window, mode == 2, route.Valid() ? &route : nullptr, dots, 256, minimapPixels);
                    fe.SetStreamTexture(kMapTex, minimapPixels.data(), 256, 256);
                    if (!minimapBound) {
                        FENativeInterface::SetTextureHash(hud, kMapObj, kMapTex);
                        minimapBound = true;
                    }
                    // The north marker orbits the map (rotate mode) or stays on top (north up).
                    FEObject *mapObj = hud->FindObjectByHash(kMapObj), *north = hud->FindObjectByHash(kNorthObj);
                    if (mapObj && north && mapObj->GetObjData() && north->GetObjData()) {
                        const float a = mode == 2 ? car.Yaw() : 1.5707963f;
                        const float r = 70.0f;
                        north->GetObjData()->Pos.x = mapObj->GetObjData()->Pos.x - std::cos(a) * r;
                        north->GetObjData()->Pos.y = mapObj->GetObjData()->Pos.y - std::sin(a) * r;
                        north->Flags |= 0x400000u;
                    }
                }
            }
            // The full-screen maps (career World Map, in-game World Map): the live road map of the region replaces the
            // package's static outline, with the player / rivals / route, or the career events (ISSUES #261).
            if (cribPkg && cribPkg->GetName() && minimap.Valid() && !getenv("NFSU2_NO_MINIMAP")) {
                const bool careerMap = strcasecmp(cribPkg->GetName(), "UI_CareerWorldMap.fng") == 0;
                const bool gameMap = strcasecmp(cribPkg->GetName(), "UI_InGame_WorldMap.fng") == 0;
                if (careerMap || gameMap) {
                    constexpr uint32_t kFullTex = 0x4d415046, kMapObj = 0x90fe80f3;
                    static unsigned fullFrame = 0;
                    if (fullFrame++ % 6 == 0) {
                        std::vector<Minimap::Dot> dots;
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
                                if (it->second.empty() || !world.LineFromMarkers(it->second, race, x, y))
                                    continue;
                                if (i == startup.WorldMapSelected())
                                    marker = {x, y, 255, 255, 255}, haveMarker = true, markerYaw = 1.5707963f;
                                else
                                    dots.push_back({x, y, 255, 214, 60});
                            }
                        } else {
                            for (const PlayerCar &o : opponentCars)
                                dots.push_back({o.X(), o.Y(), 255, 70, 70});
                            marker = {car.X(), car.Y(), 255, 255, 255};
                            markerYaw = car.Yaw();
                            haveMarker = true;
                        }
                        minimap.RenderFull(gameMap && route.Valid() ? &route : nullptr, dots, 512, minimapPixels, haveMarker ? &marker : nullptr, markerYaw);
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
            if (!world.RenderGL(ren, camera, w, h))
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
            // The dialog is a package on top of the shop; the preview overlay is drawn outside the
            // package stack, so it would cover the prompt (ISSUES #189).
            if (!startup.IsMessageDialogOpen())
                fe.RenderCarPreviewOverlay();
            std::string region;
            if (startup.TakeRaceRequest(region))
                enterWorld(region);
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
