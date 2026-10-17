// Native entry point. Mirrors the boot order of the PS2 main() at 0x002dbf00:
//   0x00442a88 global ctors (handled by the C++ runtime here)
//   0x002dbd88 system init(argc, argv)  -> todo
//   0x002dbef8                          -> todo
//   0x002e2fc8 startup path selection   -> todo
//   0x002dc298 main loop (run on the scratchpad stack on PS2) -> todo
// For now it mounts the disc archive with the decompiled file system, checks it against
// known files, and opens an SDL window.

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vector>

#include "game/bWare/bCompression.h"
#include "game/indep/File/ZipFileSystem.h"
#include "game/indep/Texture/TexturePack.h"
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

static SDL_Texture *UploadTexture(SDL_Renderer *ren, const TexturePack &pack, const char *name)
{
    const TextureInfo *info = pack.Find(name);
    DecodedTexture tex;
    if (!info || !pack.Decode(*info, tex)) {
        fprintf(stderr, "texture %s: not decoded\n", name);
        return nullptr;
    }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, tex.width, tex.height);
    SDL_UpdateTexture(t, nullptr, tex.rgba.data(), tex.width * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
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
    // PS2 PAL output is 640x512; scale 2x for a modern display.
    SDL_Window *win = SDL_CreateWindow("Need for Speed Underground 2 (native)", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1280, 1024, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, 640, 512);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    // Boot splash from the front-end bundle's texture pack.
    std::vector<uint8_t> frontend = ReadGameFile(*fs, "FRONTEND\\FRONTB.LZC");
    TexturePack pack;
    SDL_Texture *logo = nullptr, *ring = nullptr;
    if (pack.Parse(frontend.data(), frontend.size())) {
        printf("Front-end texture pack: %zu textures\n", pack.Count());
        logo = UploadTexture(ren, pack, "EA_GAMES_LOGO");
        ring = UploadTexture(ren, pack, "EA_GAMES_LOGO_RING");
    }

    const Uint32 start = SDL_GetTicks();
    for (bool running = true; running;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                running = false;
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        float t = (SDL_GetTicks() - start) / 1000.0f;
        SDL_Rect dst = {320 - 128, 256 - 128, 256, 256};
        if (ring) {
            // Pulse the glow ring behind the logo.
            Uint8 a = Uint8(128 + 127 * SDL_sinf(t * 3.0f));
            SDL_SetTextureAlphaMod(ring, a);
            SDL_Rect r = {320 - 160, 256 - 160, 320, 320};
            SDL_RenderCopy(ren, ring, nullptr, &r);
        }
        if (logo) {
            SDL_SetTextureAlphaMod(logo, Uint8(SDL_min(1.0f, t) * 255));
            SDL_RenderCopy(ren, logo, nullptr, &dst);
        }
        // NFSU2_SCREENSHOT=file.bmp: save the frame 1.5 s in and quit (for testing).
        if (const char *shot = getenv("NFSU2_SCREENSHOT"); shot && t > 1.5f) {
            int w, h;
            SDL_GetRendererOutputSize(ren, &w, &h);
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
            SDL_SaveBMP(s, shot);
            SDL_FreeSurface(s);
            running = false;
        }
        SDL_RenderPresent(ren);
    }
    if (logo)
        SDL_DestroyTexture(logo);
    if (ring)
        SDL_DestroyTexture(ring);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    delete fs;
    return 0;
}
