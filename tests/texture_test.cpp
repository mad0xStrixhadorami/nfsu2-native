// Decode every texture in a bundle's texture pack and write a few to BMP for inspection.
// Usage: texture_test BUNDLE OUT_DIR NAME...
#include <SDL.h>

#include <cstdio>
#include <string>
#include <vector>

#include "game/indep/Texture/TexturePack.h"

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    std::vector<uint8_t> data;
    for (int c; (c = fgetc(f)) != EOF;)
        data.push_back(uint8_t(c));
    fclose(f);
    TexturePack pack;
    if (!pack.Parse(data.data(), data.size()))
        return 1;
    int ok = 0, failed = 0;
    for (size_t i = 0; i < pack.Count(); ++i) {
        DecodedTexture t;
        if (pack.Decode(pack.At(i), t))
            ++ok;
        else {
            ++failed;
            printf("cannot decode %s (%d bpp)\n", pack.At(i).name.c_str(), pack.At(i).bitsPerPixel);
        }
    }
    for (int a = 3; a < argc; ++a) {
        const TextureInfo *info = pack.Find(argv[a]);
        DecodedTexture t;
        if (!info || !pack.Decode(*info, t))
            continue;
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(t.rgba.data(), t.width, t.height, 32, t.width * 4,
                                                            SDL_PIXELFORMAT_ABGR8888);
        SDL_SaveBMP(s, (std::string(argv[2]) + "/" + argv[a] + ".bmp").c_str());
        SDL_FreeSurface(s);
    }
    printf("texture_test: %d decoded, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}
