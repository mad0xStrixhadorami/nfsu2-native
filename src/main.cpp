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

#include <vector>

#include "game/bWare/bCompression.h"
#include "game/indep/File/ZipFileSystem.h"
#include "Speed/Indep/Src/FEng/FEngine.h"
#include "game/indep/Frontend/FENative.h"
#include "game/indep/Texture/TexturePack.h"
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

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // FEng screen space is 640x480; open at 2x.
    SDL_Window *win = SDL_CreateWindow("Need for Speed Underground 2 (native)", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1280, 960, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(ren, FENativeInterface::kScreenWidth, FENativeInterface::kScreenHeight);

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
    engine->SetInterface(&fe);
    // NFSU2_PACKAGE=name.fng shows another front-end package instead (development aid).
    const char *package = getenv("NFSU2_PACKAGE") ? getenv("NFSU2_PACKAGE") : "loading_boot.fng";
    if (!engine->PushPackage(package, 0, 0xff))
        fprintf(stderr, "could not push %s\n", package);

    Uint32 last = SDL_GetTicks();
    const Uint32 start = last;
    for (bool running = true; running;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                running = false;
        Uint32 now = SDL_GetTicks();
        engine->Update(static_cast<i32>(now - last), 0); // FEng ticks are milliseconds (cFEng::Service)
        last = now;

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        engine->Render();

        // NFSU2_SCREENSHOT=file.bmp: save the frame after NFSU2_SCREENSHOT_MS (default 1500) and quit.
        if (const char *shot = getenv("NFSU2_SCREENSHOT")) {
            const char *ms = getenv("NFSU2_SCREENSHOT_MS");
            if (now - start > Uint32(ms ? atoi(ms) : 1500)) {
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
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    fflush(stdout);
    // EA's static object pools are not safe to destroy at process exit (ISSUES.md #25).
    _Exit(0);
}
