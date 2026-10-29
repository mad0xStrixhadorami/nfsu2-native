#include "game/indep/World/MaterialLibrary.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL_opengl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace MaterialLibrary {
namespace {
const char *const kColourFiles[kColours] = {"asphalt031", "pavingstones128", "pavingstones146", "bricks084",
                                            "bricks093",  "plaster002",      "concrete048",     "concrete047a"};
const char *const kNormalFiles[kNormals] = {"asphalt031", "pavingstones128", "bricks084", "concrete048", "plaster002"};
unsigned g_colour[kColours], g_normal[kNormals];
int g_state = 0; // 0 untried, 1 loaded, -1 failed

std::string Dir()
{
    if (const char *d = getenv("NFSU2_ASSETS"))
        return d;
    for (const char *c : {"assets/israel", "../assets/israel", "../../assets/israel"}) {
        std::string probe = std::string(c) + "/LICENSE.txt";
        if (FILE *f = fopen(probe.c_str(), "r")) {
            fclose(f);
            return c;
        }
    }
    return "assets/israel";
}

unsigned Upload(const std::string &path)
{
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!px)
        return 0;
    unsigned id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, 16.0f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    stbi_image_free(px);
    return id;
}
} // namespace

bool Load()
{
    if (g_state)
        return g_state > 0;
    if (getenv("NFSU2_NO_MATERIALS")) {
        g_state = -1;
        return false;
    }
    const std::string dir = Dir();
    bool ok = true;
    for (int i = 0; i < kColours; ++i)
        ok &= (g_colour[i] = Upload(dir + "/" + kColourFiles[i] + "_color.jpg")) != 0;
    for (int i = 0; i < kNormals; ++i)
        ok &= (g_normal[i] = Upload(dir + "/" + kNormalFiles[i] + "_normal.jpg")) != 0;
    g_state = ok ? 1 : -1;
    printf("[Materials] %s (%s)\n", ok ? "photo materials loaded" : "FAILED, using procedural skins", dir.c_str());
    return ok;
}
unsigned Colour(int i) { return g_colour[i]; }
unsigned Normal(int i) { return g_normal[i]; }
} // namespace MaterialLibrary
