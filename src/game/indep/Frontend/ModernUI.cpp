#include "game/indep/Frontend/ModernUI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "stb_image.h"

namespace {
constexpr float kPi = 3.14159265f;
const SDL_Color kAccent{70, 190, 255, 255};   // the blue-and-white of the flag, brightened for a screen
const SDL_Color kWarm{255, 150, 40, 255};
const SDL_Color kRed{255, 60, 55, 255};
const SDL_Color kWhite{245, 248, 252, 255};

std::string FontPath()
{
    if (const char *d = getenv("NFSU2_ASSETS_UI"))
        return std::string(d) + "/Rajdhani-Bold.ttf";
    for (const char *c : {"assets/ui", "../assets/ui", "../../assets/ui"}) {
        std::string p = std::string(c) + "/Rajdhani-Bold.ttf";
        if (FILE *f = fopen(p.c_str(), "rb")) {
            fclose(f);
            return p;
        }
    }
    return "assets/ui/Rajdhani-Bold.ttf";
}

SDL_Color Mix(SDL_Color a, SDL_Color b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {Uint8(a.r + (b.r - a.r) * t), Uint8(a.g + (b.g - a.g) * t), Uint8(a.b + (b.b - a.b) * t), Uint8(a.a + (b.a - a.a) * t)};
}
SDL_Color Alpha(SDL_Color c, float a) { return {c.r, c.g, c.b, Uint8(std::clamp(a, 0.0f, 1.0f) * 255.0f)}; }
} // namespace

ModernUI::~ModernUI()
{
    if (m_map)
        SDL_DestroyTexture(m_map);
    if (m_loadArt)
        SDL_DestroyTexture(m_loadArt);
    if (m_capture)
        SDL_DestroyTexture(m_capture);
    for (auto &f : m_frames)
        if (f.second)
            SDL_DestroyTexture(f.second);
    if (m_font)
        SDL_DestroyTexture(m_font);
    if (m_glow)
        SDL_DestroyTexture(m_glow);
}

bool ModernUI::Init(SDL_Renderer *r)
{
    m_r = r;
    FILE *f = fopen(FontPath().c_str(), "rb");
    if (!f) {
        printf("[ModernUI] font not found (%s): classic UI only\n", FontPath().c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> ttf((size_t(n)));
    if (fread(ttf.data(), 1, size_t(n), f) != size_t(n)) {
        fclose(f);
        return false;
    }
    fclose(f);
    m_atlasW = 1024, m_atlasH = 1024;
    std::vector<unsigned char> alpha(size_t(m_atlasW) * m_atlasH);
    stbtt_bakedchar baked[96];
    if (stbtt_BakeFontBitmap(ttf.data(), 0, kBakePx, alpha.data(), m_atlasW, m_atlasH, 32, 96, baked) <= 0) {
        printf("[ModernUI] font bake failed\n");
        return false;
    }
    for (int i = 0; i < 96; ++i)
        m_glyphs[i] = {float(baked[i].x0), float(baked[i].y0), float(baked[i].x1), float(baked[i].y1), baked[i].xoff, baked[i].yoff, baked[i].xadvance};
    std::vector<Uint32> px(alpha.size());
    for (size_t i = 0; i < alpha.size(); ++i)
        px[i] = 0x00ffffffu | (Uint32(alpha[i]) << 24);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    m_font = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, m_atlasW, m_atlasH);
    SDL_UpdateTexture(m_font, nullptr, px.data(), m_atlasW * 4);
    SDL_SetTextureBlendMode(m_font, SDL_BLENDMODE_BLEND);
    // a soft round glow (radial alpha falloff)
    const int G = 128;
    std::vector<Uint32> g(G * G);
    for (int y = 0; y < G; ++y)
        for (int x = 0; x < G; ++x) {
            const float dx = (x + 0.5f) / G * 2 - 1, dy = (y + 0.5f) / G * 2 - 1;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float a = std::pow(std::max(0.0f, 1.0f - d), 2.2f);
            g[y * G + x] = 0x00ffffffu | (Uint32(a * 255.0f) << 24);
        }
    m_glow = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, G, G);
    SDL_UpdateTexture(m_glow, nullptr, g.data(), G * 4);
    SDL_SetTextureBlendMode(m_glow, SDL_BLENDMODE_ADD);
    // the loading-screen art (ISSUES #301), next to the font
    {
        std::string art = FontPath();
        art = art.substr(0, art.rfind('/') + 1) + "loading.jpg";
        int iw = 0, ih = 0, comp = 0;
        if (unsigned char *img = stbi_load(art.c_str(), &iw, &ih, &comp, 4)) {
            m_loadArt = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
            SDL_UpdateTexture(m_loadArt, nullptr, img, iw * 4);
            SDL_SetTextureBlendMode(m_loadArt, SDL_BLENDMODE_NONE);
            SDL_SetTextureScaleMode(m_loadArt, SDL_ScaleModeLinear);
            m_loadW = iw, m_loadH = ih;
            stbi_image_free(img);
        } else {
            printf("[ModernUI] loading art not found (%s)\n", art.c_str());
        }
    }
    {   // the animated backdrop: count the consecutive frames
        std::string dir = FontPath();
        dir = dir.substr(0, dir.rfind('/') + 1) + "loading/";
        m_frameDir = dir;
        char name[600];
        for (int i = 1; i < 2000; ++i) {
            snprintf(name, sizeof(name), "%sf%03d.jpg", dir.c_str(), i);
            FILE *f = fopen(name, "rb");
            if (!f)
                break;
            fclose(f);
            ++m_frameCount;
        }
        if (m_frameCount >= 2 && !getenv("NFSU2_STILL_LOADING")) {
            if (SDL_Texture *first = LoadingFrame(0)) {
                Uint32 fmt;
                int acc;
                SDL_QueryTexture(first, &fmt, &acc, &m_loadW, &m_loadH);
            }
            printf("[ModernUI] animated loading backdrop: %d frames\n", m_frameCount);
        } else {
            m_frameCount = 0;
        }
    }
    printf("[ModernUI] ready (%s)\n", FontPath().c_str());
    return true;
}

float ModernUI::TextWidth(std::string_view s, float px) const
{
    float w = 0;
    for (unsigned char c : s)
        if (c >= 32 && c < 128)
            w += m_glyphs[c - 32].xadv;
    return w * px / kBakePx;
}

void ModernUI::Text(float x, float y, float px, std::string_view s, SDL_Color col, int align)
{
    if (!m_font)
        return;
    const float k = px / kBakePx;
    const float w = TextWidth(s, px);
    float cx = align == 1 ? x - w * 0.5f : align == 2 ? x - w : x;
    const float base = y + px * 0.78f; // baseline from the top of the line
    SDL_SetTextureColorMod(m_font, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(m_font, col.a);
    for (unsigned char c : s) {
        if (c < 32 || c >= 128)
            continue;
        const Glyph &g = m_glyphs[c - 32];
        const SDL_Rect src{int(g.x0), int(g.y0), int(g.x1 - g.x0), int(g.y1 - g.y0)};
        const SDL_FRect dst{cx + g.xoff * k, base + g.yoff * k, (g.x1 - g.x0) * k, (g.y1 - g.y0) * k};
        SDL_RenderCopyF(m_r, m_font, &src, &dst);
        cx += g.xadv * k;
    }
}

void ModernUI::Quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_Color ca, SDL_Color cb, SDL_Color cc, SDL_Color cd)
{
    const SDL_Vertex v[4] = {{a, ca, {0, 0}}, {b, cb, {0, 0}}, {c, cc, {0, 0}}, {d, cd, {0, 0}}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(m_r, nullptr, v, 4, idx, 6);
}

void ModernUI::Rect(float x, float y, float w, float h, SDL_Color top, SDL_Color bottom)
{
    Quad({x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, top, top, bottom, bottom);
}

// A panel whose top-right and bottom-left corners are cut at 45 degrees (the "sport" silhouette), a vertical tint gradient
// and a bright 2 px rim on the cut edges.
void ModernUI::Glass(float x, float y, float w, float h, float cut, SDL_Color tint, SDL_Color edge)
{
    const SDL_Color top = tint, bot = Mix(tint, {0, 0, 0, tint.a}, 0.5f);
    const SDL_Vertex v[6] = {{{x, y}, top, {0, 0}},          {{x + w - cut, y}, top, {0, 0}}, {{x + w, y + cut}, Mix(top, bot, cut / h), {0, 0}},
                             {{x + w, y + h}, bot, {0, 0}},  {{x + cut, y + h}, bot, {0, 0}}, {{x, y + h - cut}, Mix(top, bot, 1 - cut / h), {0, 0}}};
    const int idx[12] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5};
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(m_r, nullptr, v, 6, idx, 12);
    SDL_SetRenderDrawColor(m_r, edge.r, edge.g, edge.b, edge.a);
    SDL_RenderDrawLineF(m_r, x, y, x + w - cut, y);
    SDL_RenderDrawLineF(m_r, x + w - cut, y, x + w, y + cut);
    SDL_RenderDrawLineF(m_r, x + w, y + cut, x + w, y + h);
    SDL_RenderDrawLineF(m_r, x + w, y + h, x + cut, y + h);
    SDL_RenderDrawLineF(m_r, x + cut, y + h, x, y + h - cut);
    SDL_RenderDrawLineF(m_r, x, y + h - cut, x, y);
}

void ModernUI::ArcSegment(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c)
{
    const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
    Quad({cx + c0 * r0, cy + s0 * r0}, {cx + c0 * r1, cy + s0 * r1}, {cx + c1 * r1, cy + s1 * r1}, {cx + c1 * r0, cy + s1 * r0}, c, c, c, c);
}

void ModernUI::Arc(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c0, SDL_Color c1)
{
    const int steps = std::max(2, int(std::ceil(std::fabs(a1 - a0) / 0.09f)));
    auto mix = [&](float t) {
        return SDL_Color{Uint8(c0.r + (c1.r - c0.r) * t), Uint8(c0.g + (c1.g - c0.g) * t), Uint8(c0.b + (c1.b - c0.b) * t), Uint8(c0.a + (c1.a - c0.a) * t)};
    };
    for (int i = 0; i < steps; ++i) {
        const float ta = float(i) / steps, tb = float(i + 1) / steps;
        const float aa = a0 + (a1 - a0) * ta, ab = a0 + (a1 - a0) * tb;
        const SDL_Color ca = mix(ta), cb = mix(tb);
        Quad({cx + std::cos(aa) * r0, cy + std::sin(aa) * r0}, {cx + std::cos(aa) * r1, cy + std::sin(aa) * r1},
             {cx + std::cos(ab) * r1, cy + std::sin(ab) * r1}, {cx + std::cos(ab) * r0, cy + std::sin(ab) * r0}, ca, ca, cb, cb);
    }
}

void ModernUI::Glow(float cx, float cy, float radius, SDL_Color c)
{
    if (!m_glow)
        return;
    SDL_SetTextureColorMod(m_glow, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(m_glow, c.a);
    const SDL_FRect dst{cx - radius, cy - radius, radius * 2, radius * 2};
    SDL_RenderCopyF(m_r, m_glow, nullptr, &dst);
}

void ModernUI::DrawBackdrop(int w, int h, float t)
{
    // Flat charcoal with a few hard diagonal cuts and one signal-yellow tab (ISSUES #357); the blue glow pools, the violet dusk, the
    // perspective grid and the speed streaks of the first redesign are gone.
    const float W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(0, 0, W, H, {20, 20, 22, 255}, {10, 10, 11, 255});
    const float drift = 12.0f * std::sin(t * 0.12f);
    const float c1 = W * 0.58f + drift, c2 = W * 0.82f - drift;
    Quad({c1, 0}, {c1 + W * 0.20f, 0}, {c1 + W * 0.20f - H * 0.32f, H}, {c1 - H * 0.32f, H}, {255, 255, 255, 9}, {255, 255, 255, 9}, {255, 255, 255, 9}, {255, 255, 255, 9});
    Quad({c2, 0}, {c2 + W * 0.05f, 0}, {c2 + W * 0.05f - H * 0.32f, H}, {c2 - H * 0.32f, H}, {255, 214, 10, 16}, {255, 214, 10, 16}, {255, 214, 10, 16}, {255, 214, 10, 16});
    Rect(0, H * 0.80f, W, H * 0.20f, {0, 0, 0, 0}, {0, 0, 0, 150});
    Rect(0, 0, W, H * 0.12f, {0, 0, 0, 110}, {0, 0, 0, 0});
    Quad({0, 0}, {W * 0.09f, 0}, {W * 0.09f - 10, 5}, {0, 5}, {255, 214, 10, 255}, {255, 214, 10, 255}, {255, 214, 10, 255}, {255, 214, 10, 255});
}

void ModernUI::DrawShotTitle(int w, int h, const char *title, const char *sub, float age, float dir, bool entry)
{
    if (!m_font || age < 0.0f)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    const SDL_Color ink{10, 10, 11, 255}, paper{244, 244, 240, 255}, signal{255, 214, 10, 255};
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    auto ease = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return 1.0f - std::pow(1.0f - x, 3.0f); };
    auto A = [](SDL_Color c, float a) { return SDL_Color{c.r, c.g, c.b, Uint8(std::clamp(a, 0.0f, 1.0f) * 255.0f)}; };
    const float slant = H * 0.30f;
    // the blade: its leading edge runs across in 0.32 s, its trailing edge follows 0.22 s later (a swept band, direction by `dir`)
    const float lead = ease(age / 0.32f), trail = ease((age - 0.22f) / 0.36f);
    auto edgeX = [&](float p) { return dir > 0 ? -slant + p * (W + 2 * slant) : W + slant - p * (W + 2 * slant); };
    if (trail < 1.0f) {
        const float x0 = edgeX(trail), x1 = edgeX(lead);
        const float xl = std::min(x0, x1), xr = std::max(x0, x1);
        Quad({xl + slant, 0}, {xr + slant, 0}, {xr, H}, {xl, H}, signal, signal, signal, signal);
        // a second, thinner blade riding behind it
        const float bw = 26 * s, tx = dir > 0 ? xl - bw * 2.2f : xr + bw * 1.2f;
        Quad({tx + slant, 0}, {tx + bw + slant, 0}, {tx + bw, H}, {tx, H}, A(paper, 0.85f), A(paper, 0.85f), A(paper, 0.85f), A(paper, 0.85f));
    }
    // the title cuts in behind the blade, holds, fades
    const float inP = ease((age - 0.30f) / 0.45f), out = 1.0f - ease((age - (entry ? 1.9f : 1.35f)) / 0.45f);
    if (inP > 0.0f && out > 0.0f) {
        const float lx = 72 * s + (1.0f - inP) * -90 * s * dir, ty = H * 0.70f;
        const float al = inP * out;
        Text(lx, ty - 34 * s, 20 * s, sub, A(signal, al), 0);
        Text(lx, ty - 6 * s, 92 * s, title, A(paper, al), 0);
        const float lw = std::min(1.0f, inP) * 260 * s;
        Quad({lx, ty + 96 * s}, {lx + lw, ty + 96 * s}, {lx + lw, ty + 100 * s}, {lx, ty + 100 * s}, A(signal, al), A(signal, al), A(signal, al), A(signal, al));
        // dark under-strip so the title reads on any scene
        Quad({0, ty - 60 * s}, {W, ty - 60 * s}, {W, ty + 118 * s}, {0, ty + 118 * s}, A(ink, 0.0f), A(ink, 0.0f), A(ink, 0.0f), A(ink, 0.0f));
    }
}

void ModernUI::DrawWind(int w, int h, float strength, float nitro, float t)
{
    if (strength <= 0.02f && nitro <= 0.02f)
        return;
    const float cx = float(w) * 0.5f, cy = float(h) * 0.5f, diag = std::hypot(cx, cy);
    const int lines = 34 + int(strength * 34.0f + nitro * 30.0f);
    for (int i = 0; i < lines; ++i) {
        // a deterministic hash per streak: angle, phase, speed, thickness
        uint32_t k = uint32_t(i) * 2654435761u + 12345u;
        auto rnd = [&k]() { k ^= k << 13; k ^= k >> 17; k ^= k << 5; return float(k & 0xffff) / 65535.0f; };
        const float ang = rnd() * 6.2831853f, ph = rnd(), sp = 0.55f + rnd() * 0.9f, th = 1.2f + rnd() * 2.2f;
        const float u = std::fmod(ph + t * sp * (0.8f + 1.4f * strength), 1.0f); // 0 (inner) .. 1 (off the edge)
        const float r0 = diag * (0.52f + 0.52f * u * u), len = diag * (0.05f + 0.16f * strength + 0.10f * nitro) * (0.4f + u);
        const float r1 = r0 + len;
        const float edge = std::clamp((u - 0.05f) * 3.0f, 0.0f, 1.0f) * std::clamp((1.0f - u) * 4.0f, 0.0f, 1.0f);
        const float alpha = (0.10f + 0.22f * strength + 0.16f * nitro) * edge;
        const SDL_Color c = nitro > 0.3f ? Mix({210, 235, 255, 255}, {110, 190, 255, 255}, nitro) : SDL_Color{225, 238, 250, 255};
        const float ca = std::cos(ang), sa = std::sin(ang), nx = -sa * th * 0.5f, ny = ca * th * 0.5f;
        Quad({cx + ca * r0 + nx, cy + sa * r0 + ny}, {cx + ca * r0 - nx, cy + sa * r0 - ny},
             {cx + ca * r1 - nx * 0.2f, cy + sa * r1 - ny * 0.2f}, {cx + ca * r1 + nx * 0.2f, cy + sa * r1 + ny * 0.2f},
             Alpha(c, 0.0f), Alpha(c, 0.0f), Alpha(c, alpha), Alpha(c, alpha));
    }
}

void ModernUI::DrawMinimap(int w, int h, const Hud &hud, float t)
{
    if (!m_font || !hud.mapPixels || hud.mapSize <= 0)
        return;
    if (!m_map || m_mapTexSize != hud.mapSize) {
        if (m_map)
            SDL_DestroyTexture(m_map);
        m_map = SDL_CreateTexture(m_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, hud.mapSize, hud.mapSize);
        m_mapTexSize = hud.mapSize;
        if (m_map)
            SDL_SetTextureBlendMode(m_map, SDL_BLENDMODE_BLEND);
    }
    if (!m_map)
        return;
    if (hud.mapNew)
        SDL_UpdateTexture(m_map, nullptr, hud.mapPixels, hud.mapSize * 4);
    // NFS Unbound's map: a frosted glass disc (the scene shows through), a thin white double ring with a colour fringe, a yellow arrow with
    // green rings, and the event name in a wide face underneath.
    const float s = float(h) / 720.0f;
    const float R = 92.0f * s, cx = 30.0f * s + R + 18.0f * s, cy = float(h) - 78.0f * s - R;
    const SDL_FRect dst{cx - R, cy - R, R * 2.0f, R * 2.0f};
    SDL_RenderCopyF(m_r, m_map, nullptr, &dst);
    // a soft inner shade towards the rim
    for (int i = 0; i < 4; ++i)
        Ring(cx, cy, R - float(i + 1) * 4.2f * s, R - float(i) * 4.2f * s, {0, 0, 0, Uint8(7 + (3 - i) * 2)});
    Ring(cx + 1.3f * s, cy, R - 0.6f * s, R + 0.9f * s, {255, 70, 90, 60});  // chromatic fringe
    Ring(cx - 1.3f * s, cy, R - 0.6f * s, R + 0.9f * s, {70, 220, 255, 60});
    Ring(cx, cy, R - 0.9f * s, R + 1.2f * s, {250, 252, 255, 238});
    Ring(cx, cy, R + 4.2f * s, R + 5.0f * s, {235, 240, 250, 70});
    // The player: a yellow arrow with a green outline and two green rings behind it (up when the map turns with the car).
    const float psi = hud.mapRotate ? 0.0f : 1.5707963f - hud.mapYaw; // clockwise from up
    auto rot = [&](float x, float y, float k) { return SDL_FPoint{cx + (x * std::cos(psi) - y * std::sin(psi)) * s * k, cy + (x * std::sin(psi) + y * std::cos(psi)) * s * k}; };
    Ring(cx, cy - 5.0f * s, 12.0f * s, 13.6f * s, {90, 255, 80, 215});
    Ring(cx, cy - 15.0f * s, 10.0f * s, 11.4f * s, {90, 255, 80, 170});
    auto arrow = [&](float k, SDL_Color c) {
        const SDL_FPoint tip = rot(0, -13, k), l = rot(-9.5f, 10, k), n = rot(0, 4.5f, k), r = rot(9.5f, 10, k);
        Quad(tip, l, n, tip, c, c, c, c);
        Quad(tip, n, r, tip, c, c, c, c);
    };
    arrow(1.28f, {70, 235, 60, 255});
    arrow(1.0f, {255, 226, 58, 255});
    // The label: the event type with the same colour fringe as the ring, and the distance (or the map scale) under it.
    const float ly = cy + R + 14.0f * s;
    const char *name = hud.eventName && hud.eventName[0] ? hud.eventName : "FREE ROAM";
    Text(cx - 1.5f * s, ly, 24.0f * s, name, {70, 220, 255, 120}, 1);
    Text(cx + 1.5f * s, ly, 24.0f * s, name, {255, 70, 110, 120}, 1);
    Text(cx, ly, 24.0f * s, name, {250, 252, 255, 250}, 1);
    char buf[48];
    if (hud.finishKm >= 0.0f)
        snprintf(buf, sizeof buf, "%.1f KM TO GO", hud.finishKm);
    else
        snprintf(buf, sizeof buf, "%d M", int(hud.mapMetres + 0.5f));
    Text(cx, ly + 26.0f * s, 15.0f * s, buf, hud.finishKm >= 0 ? SDL_Color{255, 226, 90, 235} : SDL_Color{205, 225, 245, 210}, 1);
    (void)t;
}

// NFS Unbound's dial: a dark glass disc with the speed in padded digits, "mph" and a gear tile; around it an rpm scale in thousands (a
// red zone at the end), a bright white arc outside showing the revs, and to the left a quarter-circle gauge with a needle for the nitrous.
void ModernUI::DrawSpeedo(int w, int h, const Hud &hud, float t)
{
    const float s = float(h) / 720.0f;
    const float Rout = 100.0f * s, Rdisc = 77.0f * s, cx = float(w) - 152.0f * s, cy = float(h) - 138.0f * s;
    constexpr float kPi = 3.14159265f;
    const float a0 = 145.0f * kPi / 180.0f, sweep = 240.0f * kPi / 180.0f; // 0 at the lower left, clockwise over the top to the lower right
    const float frac = std::clamp(hud.rpm / std::max(hud.rpmScale, 1000.0f), 0.0f, 1.0f);
    const float redStart = 0.82f;
    char buf[64];
    // while the nitrous burns the dial turns lime, as in NFS Unbound's boost
    const bool boost = hud.nitrousActive;
    const SDL_Color ink = boost ? SDL_Color{196, 255, 72, 255} : SDL_Color{250, 252, 255, 255};
    Glow(cx, cy, Rout * 1.7f, boost ? SDL_Color{110, 190, 30, 70} : SDL_Color{30, 50, 90, 46});
    // the disc and the scale band (a darker red band at the end)
    Arc(cx, cy, 0.0f, Rdisc, 0.0f, 6.2831853f, {14, 16, 22, 222}, {14, 16, 22, 222});
    Arc(cx, cy, Rdisc, Rout, a0, a0 + sweep * redStart, {44, 60, 88, 150}, {44, 60, 88, 150});
    Arc(cx, cy, Rdisc, Rout, a0 + sweep * redStart, a0 + sweep, {150, 26, 32, 215}, {150, 26, 32, 215});
    if (frac > 0.0f) // the part of the band the revs have reached is a little brighter
        Arc(cx, cy, Rdisc, Rout, a0, a0 + sweep * std::min(frac, redStart), {70, 96, 138, 120}, {88, 118, 170, 150});
    // rings with the colour fringe of the original's glass
    Ring(cx + 1.2f * s, cy, Rout - 0.6f * s, Rout + 0.8f * s, {255, 70, 90, 55});
    Ring(cx - 1.2f * s, cy, Rout - 0.6f * s, Rout + 0.8f * s, {70, 220, 255, 55});
    Ring(cx, cy, Rout - 0.8f * s, Rout + 1.0f * s, {ink.r, ink.g, ink.b, 225});
    Ring(cx, cy, Rdisc - 0.8f * s, Rdisc + 0.8f * s, {ink.r, ink.g, ink.b, 210});
    // the rpm numerals (thousands)
    const int majors = int(hud.rpmScale / 1000.0f);
    for (int m = 0; m <= majors; ++m) {
        const float u = float(m * 1000) / hud.rpmScale;
        if (u > 1.0f)
            break;
        const float a = a0 + sweep * u, r = (Rdisc + Rout) * 0.5f;
        snprintf(buf, sizeof buf, "%d", m);
        Text(cx + std::cos(a) * r, cy + std::sin(a) * r - 8.0f * s, 15.0f * s, buf, u > redStart ? SDL_Color{255, 215, 215, 235} : SDL_Color{225, 235, 248, 215}, 1);
    }
    // the bright white rev arc outside the scale, growing with the revs
    if (frac > 0.003f)
        Arc(cx, cy, Rout + 3.5f * s, Rout + 9.5f * s, a0, a0 + sweep * frac, {ink.r, ink.g, ink.b, 110}, ink);
    // the two small indicator rings under the digits: nitrous burst (cyan) and a drift in progress (amber)
    const bool nos = hud.nitrousActive, drift = hud.drifting;
    Ring(cx - 20.0f * s, cy - 47.0f * s, 9.6f * s, 11.2f * s, {235, 240, 250, 95});
    Ring(cx + 15.0f * s, cy - 54.0f * s, 10.6f * s, 12.2f * s, {235, 240, 250, 95});
    if (nos)
        Arc(cx - 20.0f * s, cy - 47.0f * s, 0.0f, 9.0f * s, 0.0f, 6.2831853f, {110, 235, 255, 150}, {110, 235, 255, 150});
    if (drift)
        Arc(cx + 15.0f * s, cy - 54.0f * s, 0.0f, 10.0f * s, 0.0f, 6.2831853f, {255, 180, 60, 150}, {255, 180, 60, 150});
    // speed: three padded digits in a heavy face (the same text drawn twice, a hair apart, for weight), the unit and the gear tile
    const float spd = hud.mph ? hud.speedKmh * 0.621371f : hud.speedKmh;
    snprintf(buf, sizeof buf, "%03d", std::min(999, int(std::fabs(spd) + 0.5f)));
    Text(cx - 0.8f * s, cy - 40.0f * s, 60.0f * s, buf, ink, 1);
    Text(cx + 0.8f * s, cy - 40.0f * s, 60.0f * s, buf, ink, 1);
    Text(cx, cy + 14.0f * s, 15.0f * s, hud.mph ? "mph" : "km/h", {215, 225, 240, 230}, 1);
    const char *g = hud.gear < 0 ? "R" : hud.gear == 0 ? "N" : nullptr;
    if (g)
        snprintf(buf, sizeof buf, "%s", g);
    else
        snprintf(buf, sizeof buf, "%d", hud.gear);
    const float tile = 31.0f * s;
    Glass(cx - tile * 0.5f, cy + 31.0f * s, tile, tile, 8.0f * s, {205, 208, 214, 235}, {240, 243, 248, 235});
    Text(cx, cy + 31.0f * s + 1.0f * s, 27.0f * s, buf, frac > 0.92f ? SDL_Color{190, 40, 40, 255} : SDL_Color{34, 38, 48, 255}, 1);

    // ---- the nitrous gauge: a quarter circle left of the dial, a needle on its pivot ----
    const float px = cx - 113.0f * s, py = cy + 66.0f * s, ro = 90.0f * s, ri = 70.0f * s;
    const float na0 = kPi, naSweep = 0.5f * kPi; // from the left, clockwise to straight up
    const float fill = hud.hasNitrous ? std::clamp(hud.nitrous, 0.0f, 1.0f) : 0.0f;
    const SDL_Color blue = hud.nitrousActive ? SDL_Color{130, 255, 255, 255} : SDL_Color{58, 150, 255, 245};
    Arc(px, py, ro, ro + 2.4f * s, na0, na0 + naSweep, blue, blue);
    const float gap = 0.05f, seg = (naSweep - 2 * gap) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float sa = na0 + gap + float(i) * (seg + gap), sb = sa + seg;
        const float lit = std::clamp(fill * 3.0f - float(i), 0.0f, 1.0f); // how much of this segment is filled
        Arc(px, py, ri, ro - 1.0f * s, sa, sb, {92, 98, 110, 150}, {92, 98, 110, 150});
        if (lit > 0.0f)
            Arc(px, py, ri, ro - 1.0f * s, sa, sa + (sb - sa) * lit, {blue.r, blue.g, blue.b, 230}, {blue.r, blue.g, blue.b, 230});
    }
    const float needle = na0 + naSweep * fill;
    {
        const float dx = std::cos(needle), dy = std::sin(needle), nx = -dy * 3.6f * s, ny = dx * 3.6f * s, len = 62.0f * s;
        Quad({px + dx * 9.0f * s + nx, py + dy * 9.0f * s + ny}, {px + dx * 9.0f * s - nx, py + dy * 9.0f * s - ny},
             {px + dx * len, py + dy * len}, {px + dx * len, py + dy * len}, {255, 255, 255, 235}, {255, 255, 255, 235}, {255, 255, 255, 0}, {255, 255, 255, 0});
    }
    Ring(px + 1.0f * s, py, 7.4f * s, 9.4f * s, {255, 70, 90, 60});
    Ring(px - 1.0f * s, py, 7.4f * s, 9.4f * s, {70, 220, 255, 60});
    Ring(px, py, 7.6f * s, 9.2f * s, {250, 252, 255, 240});
    // the battery with a bolt
    {
        const float bx = px - 36.0f * s, by = py - 52.0f * s, bw = 15.0f * s, bh = 23.0f * s;
        const SDL_Color bc = fill > 0.0f || hud.nitrousActive ? SDL_Color{60, 150, 255, 255} : SDL_Color{110, 125, 150, 200};
        Rect(bx + bw * 0.3f, by - 3.0f * s, bw * 0.4f, 3.0f * s, bc, bc);
        Rect(bx, by, bw, bh, bc, {bc.r, bc.g, Uint8(std::min(255, bc.b + 0)), bc.a});
        const SDL_Color wc{235, 245, 255, 255};
        Quad({bx + bw * 0.62f, by + 3.0f * s}, {bx + bw * 0.28f, by + bh * 0.55f}, {bx + bw * 0.52f, by + bh * 0.55f}, {bx + bw * 0.72f, by + 3.0f * s}, wc, wc, wc, wc);
        Quad({bx + bw * 0.38f, by + bh - 3.0f * s}, {bx + bw * 0.72f, by + bh * 0.45f}, {bx + bw * 0.48f, by + bh * 0.45f}, {bx + bw * 0.28f, by + bh - 3.0f * s}, wc, wc, wc, wc);
    }
    (void)t;
}

void ModernUI::DrawHud(int w, int h, const Hud &hud, float t)
{
    if (!m_font)
        return;
    DrawMinimap(w, h, hud, t);
    const float s = float(h) / 720.0f;
    const SDL_Color glass{8, 14, 28, 150}, edge{120, 190, 255, 120};
    char buf[64];

    DrawSpeedo(w, h, hud, t);

    // ---- drift, lower centre (ISSUES #291): the running chain with its multiplier and the slide angle; the banked points pop up ----
    if (hud.drifting || hud.driftChain > 0.5f || hud.driftBanner > 0.0f) {
        const float dx = float(w) * 0.5f, dy = float(h) * 0.70f;
        if (hud.driftChain > 0.5f) {
            const float pw = 250 * s;
            Glow(dx, dy + 30 * s, pw * 0.7f, {255, 140, 40, 40});
            Glass(dx - pw * 0.5f, dy, pw, 66 * s, 16 * s, {26, 16, 8, 170}, {255, 170, 80, 170});
            Text(dx - pw * 0.5f + 16 * s, dy + 4 * s, 20 * s, "DRIFT", {255, 190, 110, 240}, 0);
            snprintf(buf, sizeof(buf), "%d", int(hud.driftChain * float(hud.driftMult)));
            Text(dx - 4 * s, dy + 12 * s, 48 * s, buf, kWhite, 1);
            snprintf(buf, sizeof(buf), "x%d", hud.driftMult);
            Text(dx + pw * 0.5f - 14 * s, dy + 12 * s, 40 * s, buf, hud.driftMult >= 4 ? SDL_Color{255, 214, 80, 255} : kWarm, 2);
            const float f = std::clamp(std::fabs(hud.driftAngle) / 60.0f, 0.0f, 1.0f);
            Rect(dx - pw * 0.5f + 14 * s, dy + 58 * s, (pw - 28 * s) * f, 4 * s, kWarm, kRed);
        }
        if (hud.driftBanner > 0.0f) {
            const float a = std::min(1.0f, hud.driftBanner / 0.8f), rise = (2.5f - hud.driftBanner) * 18 * s;
            snprintf(buf, sizeof(buf), "+%d", int(hud.driftBank));
            Text(dx, dy - 150 * s - rise, 52 * s, buf, {255, 214, 80, Uint8(255 * a)}, 1);
        }
    }

    if (!hud.race)
        return;
    // ---- race info, top left: position and lap ----
    const float px = 28 * s, py = 24 * s;
    Glass(px, py, 190 * s, 74 * s, 16 * s, glass, edge);
    snprintf(buf, sizeof(buf), "%d", hud.position);
    Text(px + 18 * s, py + 2 * s, 66 * s, buf, hud.position == 1 ? SDL_Color{255, 214, 80, 255} : kWhite, 0);
    const char *ord = hud.position == 1 ? "ST" : hud.position == 2 ? "ND" : hud.position == 3 ? "RD" : "TH";
    Text(px + 18 * s + TextWidth(buf, 66 * s) + 2 * s, py + 8 * s, 24 * s, ord, {200, 225, 255, 230}, 0);
    snprintf(buf, sizeof(buf), "/ %d", hud.racers);
    Text(px + 170 * s, py + 40 * s, 22 * s, buf, {160, 205, 255, 220}, 2);
    Glass(px, py + 82 * s, 190 * s, 40 * s, 12 * s, glass, edge);
    snprintf(buf, sizeof(buf), "LAP %d / %d", std::min(hud.lap, hud.laps), hud.laps);
    Text(px + 16 * s, py + 86 * s, 28 * s, buf, kWhite, 0);

    // ---- timer, top centre ----
    const int ms = int(hud.lapTime * 1000.0f);
    snprintf(buf, sizeof(buf), "%02d:%02d.%02d", ms / 60000, (ms / 1000) % 60, (ms / 10) % 100);
    const float tw = 210 * s;
    Glass(float(w) * 0.5f - tw * 0.5f, py, tw, 54 * s, 14 * s, glass, edge);
    Text(float(w) * 0.5f, py + 4 * s, 44 * s, buf, kWhite, 1);
    if (hud.bestLap > 0.0f) {
        const int bm = int(hud.bestLap * 1000.0f);
        snprintf(buf, sizeof(buf), "BEST %02d:%02d.%02d", bm / 60000, (bm / 1000) % 60, (bm / 10) % 100);
        Text(float(w) * 0.5f, py + 58 * s, 18 * s, buf, {255, 214, 80, 235}, 1);
    }
    (void)t;
}

void ModernUI::DrawDialog(int w, int h, std::string_view title, const char *const *items, int itemCount, int selected, bool confirm,
                          std::string_view question, bool confirmYes, float t)
{
    (void)t;
    if (!m_font)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    const SDL_Color ink{10, 10, 11, 255}, paper{244, 244, 240, 255}, signal{255, 214, 10, 255};
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(0, 0, W, H, {0, 0, 0, 150}, {0, 0, 0, 190}); // dim the scene
    // flat panel on the left, like the main menu: a title, a column of caps, a yellow slanted highlight (ISSUES #357)
    const float pw = 430 * s, ph = (confirm ? 250.0f : 120.0f + 60.0f * float(itemCount) + 56.0f) * s;
    const float px = 90 * s, py = H * 0.5f - ph * 0.5f;
    Quad({px, py}, {px + pw, py}, {px + pw, py + ph}, {px, py + ph}, {10, 10, 11, 238}, {10, 10, 11, 238}, {10, 10, 11, 238}, {10, 10, 11, 238});
    Quad({px, py}, {px + 6 * s, py}, {px + 6 * s, py + ph}, {px, py + ph}, signal, signal, signal, signal);
    Text(px + 34 * s, py + 22 * s, 38 * s, title, paper, 0);
    if (!confirm) {
        for (int i = 0; i < itemCount; ++i) {
            const float ry = py + 92 * s + 60 * s * float(i);
            const bool on = i == selected;
            if (on) {
                const float sl = 14 * s;
                Quad({px + 22 * s + sl, ry}, {px + pw - 22 * s + sl, ry}, {px + pw - 22 * s, ry + 52 * s}, {px + 22 * s, ry + 52 * s}, signal, signal, signal, signal);
            }
            Text(px + 44 * s, ry + 6 * s, 32 * s, items[i], on ? ink : SDL_Color{244, 244, 240, 200}, 0);
        }
        Text(px + 34 * s, py + ph - 38 * s, 16 * s, "ARROWS move    ENTER select    ESC resume", {244, 244, 240, 130}, 0);
    } else {
        Text(px + 34 * s, py + 92 * s, 28 * s, question, paper, 0);
        Text(px + 34 * s, py + 128 * s, 18 * s, "Your race progress will be lost.", {244, 244, 240, 160}, 0);
        for (int i = 0; i < 2; ++i) {
            const bool on = (i == 0) == confirmYes;
            const float bx = px + 34 * s + float(i) * (pw * 0.5f - 24 * s), bw = pw * 0.5f - 44 * s, by = py + 168 * s;
            const SDL_Color fill = on ? signal : SDL_Color{40, 40, 42, 255};
            Quad({bx + 10 * s, by}, {bx + bw + 10 * s, by}, {bx + bw, by + 52 * s}, {bx, by + 52 * s}, fill, fill, fill, fill);
            Text(bx + bw * 0.5f + 4 * s, by + 8 * s, 32 * s, i == 0 ? "YES" : "NO", on ? ink : SDL_Color{244, 244, 240, 200}, 1);
        }
    }
}

void ModernUI::DrawMainMenu(int w, int h, const MainMenu &m, float t)
{
    if (!m_font || m.count <= 0)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    const SDL_Color ink{10, 10, 11, 255}, paper{244, 244, 240, 255}, signal{255, 214, 10, 255};
    auto A = [](SDL_Color c, int a) { return SDL_Color{c.r, c.g, c.b, Uint8(a)}; };
    // legibility scrim on the left and a thin shade top and bottom (the car and the street stay visible)
    Quad({0, 0}, {W * 0.52f, 0}, {W * 0.52f, H}, {0, H}, A(ink, 225), A(ink, 0), A(ink, 0), A(ink, 225));
    Rect(0, 0, W, 90 * s, A(ink, 150), A(ink, 0));
    Rect(0, H - 110 * s, W, 110 * s, A(ink, 0), A(ink, 190));
    // wordmark
    const float lx = 72 * s;
    Text(lx, 44 * s, 15 * s, m.kicker, A(paper, 170), 0);
    Text(lx, 60 * s, 34 * s, m.title, paper, 0);
    Quad({lx, 104 * s}, {lx + 46 * s, 104 * s}, {lx + 46 * s, 108 * s}, {lx, 108 * s}, signal, signal, signal, signal);
    // the list; the highlight glides to the selected row
    static float hy = -1.0f, last = -1.0f;
    const float top = 168 * s, pitch = std::min(60 * s, (H - top - 150 * s) / float(m.count));
    const float rowPx = std::min(44 * s, pitch * 0.74f);
    const float target = top + pitch * float(m.selected);
    if (hy < 0)
        hy = target;
    const float dt = last < 0 ? 0.016f : std::min(0.05f, t - last);
    last = t;
    hy += (target - hy) * std::min(1.0f, dt * 18.0f);
    const float slant = 16 * s, bw = 470 * s, bh = pitch * 0.9f;
    Quad({lx - 22 * s + slant, hy}, {lx - 22 * s + bw + slant, hy}, {lx - 22 * s + bw, hy + bh}, {lx - 22 * s, hy + bh}, signal, signal, signal, signal);
    for (int i = 0; i < m.count; ++i) {
        const float ry = top + pitch * float(i);
        const bool on = i == m.selected;
        const bool dead = m.disabled ? m.disabled[i] : (i == 3 && !m.enabledOnline);
        char idx[8];
        snprintf(idx, sizeof(idx), "%02d", i + 1);
        const SDL_Color txt = on ? ink : (dead ? A(paper, 70) : A(paper, 215));
        float rsz = rowPx;
        const float rw = TextWidth(m.items[i], rsz);
        if (rw > 390 * s)
            rsz *= 390 * s / rw; // long labels shrink to the column instead of running under the car
        Text(lx, ry + (pitch - rsz) * 0.12f + (rowPx - rsz) * 0.5f, rsz, m.items[i], txt, 0);
        Text(lx - 22 * s + bw - 14 * s, ry + pitch * 0.28f, 17 * s, idx, on ? A(ink, 170) : A(paper, 80), 2);
    }
    if (m.notes && m.selected >= 0 && m.selected < m.count && m.notes[m.selected])
        Text(lx, top + pitch * float(m.count) + 14 * s, 19 * s, m.notes[m.selected], A(paper, 190), 0);
    // bottom bar: the player on the left, the keys on the right
    const float by = H - 52 * s;
    Quad({lx, by - 10 * s}, {W - lx, by - 10 * s}, {W - lx, by - 9 * s}, {lx, by - 9 * s}, A(paper, 70), A(paper, 70), A(paper, 70), A(paper, 70));
    float x = lx;
    if (!m.player.empty()) {
        Text(x, by + 2 * s, 22 * s, m.player, paper, 0);
        x += TextWidth(m.player, 22 * s) + 28 * s;
    }
    char buf[48];
    if (m.money >= 0) {
        snprintf(buf, sizeof(buf), "$%d,%03d", m.money / 1000, m.money % 1000);
        if (m.money < 1000)
            snprintf(buf, sizeof(buf), "$%d", m.money);
        Text(x, by + 2 * s, 22 * s, buf, signal, 0);
        x += TextWidth(buf, 22 * s) + 28 * s;
    }
    if (m.reputation >= 0) {
        snprintf(buf, sizeof(buf), "REP %d", m.reputation);
        Text(x, by + 2 * s, 22 * s, buf, A(paper, 190), 0);
    }
    float kx = W - lx;
    auto key = [&](const char *cap, const char *what) {
        const float wl = TextWidth(what, 18 * s);
        Text(kx, by + 4 * s, 18 * s, what, A(paper, 200), 2);
        kx -= wl + 10 * s;
        const float cw = TextWidth(cap, 14 * s) + 16 * s;
        Quad({kx - cw, by + 3 * s}, {kx, by + 3 * s}, {kx, by + 25 * s}, {kx - cw, by + 25 * s}, A(paper, 235), A(paper, 235), A(paper, 235), A(paper, 235));
        Text(kx - cw * 0.5f, by + 6 * s, 14 * s, cap, ink, 1);
        kx -= cw + 26 * s;
    };
    key("ENTER", "SELECT");
    key("ARROWS", "MOVE");
    // the car
    if (!m.car.empty()) {
        const float cx = W - lx, cy = H - 190 * s;
        Text(cx, cy, 34 * s, m.car, paper, 2);
        if (!m.carSub.empty())
            Text(cx, cy + 38 * s, 17 * s, m.carSub, A(paper, 160), 2);
        Quad({cx - 46 * s, cy - 10 * s}, {cx, cy - 10 * s}, {cx, cy - 6 * s}, {cx - 46 * s, cy - 6 * s}, signal, signal, signal, signal);
    }
}

void ModernUI::DrawResults(int w, int h, const Results &r, float t)
{
    if (!m_font)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    const SDL_Color ink{10, 10, 11, 255}, paper{244, 244, 240, 255}, signal{255, 214, 10, 255};
    auto A = [](SDL_Color c, int a) { return SDL_Color{c.r, c.g, c.b, Uint8(a)}; };
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    // the scene is dimmed and the left two fifths go solid ink, cut on a diagonal
    Rect(0, 0, W, H, A(ink, 150), A(ink, 190));
    const float cut = W * 0.44f, sl = H * 0.22f;
    Quad({0, 0}, {cut + sl, 0}, {cut, H}, {0, H}, A(ink, 238), A(ink, 238), A(ink, 238), A(ink, 238));
    Quad({cut + sl, 0}, {cut + sl + 7 * s, 0}, {cut + 7 * s, H}, {cut, H}, signal, signal, signal, signal);
    const float lx = 72 * s;
    const float k = std::min(1.0f, t * 3.0f);
    static const char *const kOrd[] = {"", "1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH"};
    if (!r.reward) {
        Text(lx, 52 * s, 20 * s, r.heading, A(paper, 200), 0);
        const std::string pl = r.place >= 1 && r.place <= 8 ? kOrd[r.place] : (r.place > 8 ? std::to_string(r.place) + "TH" : "");
        if (!pl.empty())
            Text(lx - 8 * s + (1.0f - k) * -60 * s, 84 * s, 210 * s, pl, r.place == 1 ? signal : paper, 0);
        if (!r.detail.empty())
            Text(lx, 330 * s, 22 * s, r.detail, A(paper, 190), 0);
        // the table on the right
        const float tx = cut + sl + 70 * s, tw = W - tx - 64 * s;
        Text(tx, 60 * s, 18 * s, "POS", A(paper, 140), 0);
        Text(tx + 70 * s, 60 * s, 18 * s, "DRIVER", A(paper, 140), 0);
        Text(tx + tw, 60 * s, 18 * s, "TIME", A(paper, 140), 2);
        const float rowH = std::min(62 * s, (H - 190 * s) / std::max<float>(1.0f, float(r.rows.size())));
        for (size_t i = 0; i < r.rows.size(); ++i) {
            const ResultRow &row = r.rows[i];
            const float ry = 96 * s + rowH * float(i) + (1.0f - k) * 24 * s * float(i);
            const float al = k;
            if (row.player) {
                const float sk = 14 * s;
                Quad({tx - 18 * s + sk, ry}, {tx + tw + 14 * s + sk, ry}, {tx + tw + 14 * s, ry + rowH - 6 * s}, {tx - 18 * s, ry + rowH - 6 * s}, A(signal, int(255 * al)), A(signal, int(255 * al)),
                     A(signal, int(255 * al)), A(signal, int(255 * al)));
            }
            const SDL_Color c = row.player ? A(ink, int(255 * al)) : A(paper, int(225 * al));
            char pos[8];
            snprintf(pos, sizeof(pos), "%zu", i + 1);
            Text(tx, ry + 6 * s, rowH * 0.62f, pos, c, 0);
            Text(tx + 70 * s, ry + 3 * s, rowH * 0.46f, row.name, c, 0);
            Text(tx + 70 * s, ry + rowH * 0.50f, rowH * 0.27f, row.car, row.player ? A(ink, int(190 * al)) : A(paper, int(140 * al)), 0);
            Text(tx + tw, ry + 8 * s, rowH * 0.5f, row.time, c, 2);
        }
    } else {
        Text(lx, 52 * s, 20 * s, "EVENT COMPLETE", A(paper, 200), 0);
        Text(lx, 84 * s, 120 * s, "REWARD", paper, 0);
        char buf[48];
        snprintf(buf, sizeof(buf), "$%d", r.money);
        Text(lx, 270 * s, 26 * s, "BANK", A(paper, 160), 0);
        Text(lx + (1.0f - k) * -50 * s, 298 * s, 120 * s, buf, signal, 0);
        snprintf(buf, sizeof(buf), "+%d", r.reputation);
        Text(lx, 450 * s, 26 * s, "REPUTATION", A(paper, 160), 0);
        Text(lx + (1.0f - k) * -50 * s, 478 * s, 96 * s, buf, paper, 0);
        if (!r.player.empty())
            Text(W - 64 * s, 60 * s, 22 * s, r.player, A(paper, 200), 2);
    }
    // the key hint
    const float by = H - 52 * s;
    Quad({lx, by - 10 * s}, {W - 64 * s, by - 10 * s}, {W - 64 * s, by - 9 * s}, {lx, by - 9 * s}, A(paper, 70), A(paper, 70), A(paper, 70), A(paper, 70));
    const float cw = TextWidth("ENTER", 14 * s) + 16 * s;
    Quad({W - 64 * s - 110 * s - cw, by + 3 * s}, {W - 64 * s - 110 * s, by + 3 * s}, {W - 64 * s - 110 * s, by + 25 * s}, {W - 64 * s - 110 * s - cw, by + 25 * s}, A(paper, 235), A(paper, 235), A(paper, 235), A(paper, 235));
    Text(W - 64 * s - 110 * s - cw * 0.5f, by + 6 * s, 14 * s, "ENTER", ink, 1);
    Text(W - 64 * s, by + 4 * s, 18 * s, "CONTINUE", A(paper, 210), 2);
}

void ModernUI::DrawCountdown(int w, int h, float left, float goAge)
{
    if (!m_font || (left <= 0.0f && goAge < 0.0f))
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    const SDL_Color ink{10, 10, 11, 255}, paper{244, 244, 240, 255}, signal{255, 214, 10, 255};
    auto A = [](SDL_Color c, float a) { return SDL_Color{c.r, c.g, c.b, Uint8(std::clamp(a, 0.0f, 1.0f) * 255.0f)}; };
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    const float cx = W * 0.5f, cy = H * 0.34f;
    if (left > 0.0f) {
        const int n = int(std::ceil(left));
        const float f = left - std::floor(left - 1e-4f) - 0.0f; // 1 .. 0 within the second
        const float frac = std::clamp(1.0f - (left - float(n - 1)), 0.0f, 1.0f); // 0 at the start of this number, 1 at its end
        const float pop = 1.0f + 0.35f * std::pow(1.0f - std::min(1.0f, frac * 5.0f), 2.0f); // a quick overshoot as each number lands
        const float a = frac < 0.8f ? 1.0f : 1.0f - (frac - 0.8f) * 5.0f;
        (void)f;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", n);
        const float sz = 190 * s * pop;
        const float bw = 360 * s, bh = 14 * s, sk = 26 * s;
        // a slanted bar under the numeral that drains over the second
        Quad({cx - bw * 0.5f + sk, cy + 118 * s}, {cx - bw * 0.5f + sk + bw * (1.0f - frac), cy + 118 * s}, {cx - bw * 0.5f + bw * (1.0f - frac), cy + 118 * s + bh},
             {cx - bw * 0.5f, cy + 118 * s + bh}, A(signal, a), A(signal, a), A(signal, a), A(signal, a));
        Text(cx, cy - sz * 0.5f, sz, buf, A(paper, a), 1);
    } else {
        const float a = std::clamp(1.0f - goAge * 1.25f, 0.0f, 1.0f);
        const float pop = 1.0f + 0.9f * goAge; // GO flies towards the camera as it fades
        const float sz = 200 * s * pop;
        const float bw = W * (0.28f + 0.9f * goAge), sk = 40 * s;
        Quad({cx - bw * 0.5f + sk, cy - 6 * s}, {cx + bw * 0.5f + sk, cy - 6 * s}, {cx + bw * 0.5f - sk, cy + 6 * s}, {cx - bw * 0.5f - sk, cy + 6 * s}, A(signal, a), A(signal, a), A(signal, a), A(signal, a));
        Text(cx, cy - sz * 0.5f, sz, "GO", A(signal, a), 1);
    }
}

void ModernUI::DrawEngage(int w, int h, std::string_view kicker, std::string_view title, const char *const *lines, int lineCount, int selected,
                          float t)
{
    if (!m_font)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(0, 0, W, H, {2, 6, 16, 110}, {2, 6, 16, 170});
    const float pw = 520 * s, ph = (170.0f + 30.0f * float(lineCount) + 70.0f) * s;
    const float px = W * 0.5f - pw * 0.5f, py = H * 0.5f - ph * 0.5f;
    Glow(W * 0.5f, H * 0.5f, pw * 1.1f, {20, 80, 170, 60});
    Glass(px, py, pw, ph, 28 * s, {8, 14, 28, 230}, {120, 190, 255, 170});
    Quad({px, py}, {px + pw - 28 * s, py}, {px + pw - 28 * s, py + 3 * s}, {px, py + 3 * s}, kAccent, kWarm, kWarm, kAccent);
    Text(px + 30 * s, py + 18 * s, 17 * s, kicker, Alpha(kWarm, 0.95f), 0);
    Text(px + 30 * s, py + 38 * s, 44 * s, title, kWhite, 0);
    for (int i = 0; i < lineCount; ++i) { // "label|value": two columns
        const char *bar = std::strchr(lines[i], '|');
        const std::string label = bar ? std::string(lines[i], size_t(bar - lines[i])) : std::string(lines[i]);
        Text(px + 32 * s, py + 104 * s + 30 * s * float(i), 22 * s, label, {140, 175, 215, 235}, 0);
        if (bar)
            Text(px + 190 * s, py + 104 * s + 30 * s * float(i), 22 * s, bar + 1, kWhite, 0);
    }
    for (int i = 0; i < 2; ++i) {
        const bool on = i == selected;
        const float bw = pw * 0.5f - 45 * s, bx = px + 30 * s + float(i) * (bw + 30 * s), by = py + ph - 78 * s;
        const float pulse = 0.75f + 0.25f * std::sin(t * 6.0f);
        Glass(bx, by, bw, 54 * s, 14 * s, on ? SDL_Color{30, 100, 190, Uint8(170 + 50 * pulse)} : SDL_Color{12, 20, 40, 200},
              on ? SDL_Color{150, 215, 255, 230} : SDL_Color{80, 120, 170, 120});
        Text(bx + bw * 0.5f, by + 8 * s, 34 * s, i == 0 ? "ACCEPT" : "DECLINE", on ? kWhite : SDL_Color{175, 200, 230, 235}, 1);
    }
}

void ModernUI::DrawToast(int w, int h, std::string_view kicker, std::string_view title, std::string_view body, float age, float life)
{
    if (!m_font || age < 0.0f || age > life)
        return;
    const float s = float(h) / 720.0f, W = float(w);
    const float slide = std::clamp(age / 0.35f, 0.0f, 1.0f), out = std::clamp((life - age) / 0.35f, 0.0f, 1.0f);
    const float k = std::min(slide, out), ease = 1.0f - (1.0f - k) * (1.0f - k);
    const float pw = 560 * s, px = W * 0.5f - pw * 0.5f;
    // wrap the body to the card width
    std::vector<std::string> lines;
    {
        std::string cur;
        size_t i = 0;
        const float maxw = pw - 60 * s;
        while (i < body.size()) {
            size_t j = body.find(' ', i);
            if (j == std::string_view::npos)
                j = body.size();
            const std::string word(body.substr(i, j - i));
            const std::string tryLine = cur.empty() ? word : cur + " " + word;
            if (TextWidth(tryLine, 19 * s) > maxw && !cur.empty()) {
                lines.push_back(cur);
                cur = word;
            } else {
                cur = tryLine;
            }
            i = j + 1;
        }
        if (!cur.empty())
            lines.push_back(cur);
    }
    if (lines.size() > 5)
        lines.resize(5);
    const float ph = (82.0f + 25.0f * float(lines.size()) + 18.0f) * s;
    const float py = -ph + (ph + 22.0f * s) * ease;
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    const Uint8 a = Uint8(235.0f * ease);
    Glass(px, py, pw, ph, 22 * s, {8, 14, 28, a}, {120, 190, 255, Uint8(170.0f * ease)});
    Quad({px, py}, {px + pw - 22 * s, py}, {px + pw - 22 * s, py + 3 * s}, {px, py + 3 * s}, kAccent, kWarm, kWarm, kAccent);
    Text(px + 28 * s, py + 14 * s, 15 * s, kicker, Alpha(kWarm, ease), 0);
    Text(px + 28 * s, py + 32 * s, 30 * s, title, Alpha(kWhite, ease), 0);
    for (size_t i = 0; i < lines.size(); ++i)
        Text(px + 28 * s, py + (76.0f + 25.0f * float(i)) * s, 19 * s, lines[i], {190, 212, 238, Uint8(235.0f * ease)}, 0);
    // a thin timer line at the bottom
    const float f = std::clamp(1.0f - age / life, 0.0f, 1.0f);
    Quad({px + 28 * s, py + ph - 8 * s}, {px + 28 * s + (pw - 56 * s) * f, py + ph - 8 * s}, {px + 28 * s + (pw - 56 * s) * f, py + ph - 6 * s}, {px + 28 * s, py + ph - 6 * s},
         Alpha(kAccent, 0.8f * ease), Alpha(kAccent, 0.8f * ease), Alpha(kAccent, 0.8f * ease), Alpha(kAccent, 0.8f * ease));
}

void ModernUI::DrawCoverFrame(int w, int h, const SDL_Rect &page, std::string_view kicker, std::string_view title, std::string_view detail,
                              std::string_view hint, float t)
{
    if (!m_font)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(0, 0, W, H, {2, 6, 16, 175}, {2, 6, 16, 215});
    Glow(page.x + page.w * 0.5f, page.y + page.h * 0.5f, page.h * 0.95f, {30, 100, 210, Uint8(70 + 15 * std::sin(t * 1.5f))});
    // a soft shadow and a thin lit edge around the page
    for (int i = 5; i >= 1; --i) {
        const float g = float(i) * 5.0f * s;
        Rect(page.x - g + 8 * s, page.y - g + 10 * s, page.w + 2 * g, page.h + 2 * g, {0, 0, 0, Uint8(14)}, {0, 0, 0, Uint8(14)});
    }
    // captions on the left of the page
    const float lx = std::max(24.0f * s, float(page.x) - 360.0f * s);
    Text(lx, page.y + 40 * s, 17 * s, kicker, Alpha(kWarm, 0.95f), 0);
    Text(lx, page.y + 62 * s, 46 * s, title, kWhite, 0);
    Quad({lx, page.y + 122 * s}, {lx + 70 * s, page.y + 122 * s}, {lx + 70 * s, page.y + 125 * s}, {lx, page.y + 125 * s}, kWarm, kWarm, kWarm, kWarm);
    Text(lx, page.y + 142 * s, 22 * s, detail, {190, 212, 238, 240}, 0);
    Text(W * 0.5f, H - 40 * s, 18 * s, hint, {140, 175, 215, 220}, 1);
}

void ModernUI::CaptureFrame()
{
    if (!m_r)
        return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(m_r, &w, &h);
    if (w < 16 || h < 16)
        return;
    std::vector<uint32_t> px(size_t(w) * size_t(h));
    const SDL_Rect whole{0, 0, w, h};
    if (SDL_RenderReadPixels(m_r, &whole, SDL_PIXELFORMAT_ARGB8888, px.data(), w * 4) != 0)
        return;
    for (uint32_t &p : px)
        p |= 0xff000000u;
    if (!m_capture || m_capW != w || m_capH != h) {
        if (m_capture)
            SDL_DestroyTexture(m_capture);
        m_capture = SDL_CreateTexture(m_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        SDL_SetTextureBlendMode(m_capture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(m_capture, SDL_ScaleModeLinear);
        m_capW = w, m_capH = h;
    }
    SDL_UpdateTexture(m_capture, nullptr, px.data(), w * 4);
}

void ModernUI::DrawFade(int w, int h, float alpha)
{
    if (alpha <= 0.002f)
        return;
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_r, 0, 0, 0, Uint8(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
    const SDL_Rect all{0, 0, w, h};
    SDL_RenderFillRect(m_r, &all);
}

void ModernUI::DrawWipe(int w, int h, float t)
{
    if (!m_capture)
        return;
    const float W = float(w), H = float(h);
    t = std::clamp(t, 0.0f, 1.0f);
    const float e = t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f; // ease in-out
    const float slant = 0.20f * H;
    const float x = -slant * 1.5f + (W + slant * 3.0f) * e; // the edge at the vertical centre
    // the captured frame survives to the right of the slanted edge (its top leans right), and drifts a little as it leaves
    const SDL_Color white{255, 255, 255, 255};
    const float zoom = 1.0f + 0.07f * e;
    auto uv = [&](float px, float py) { return SDL_FPoint{0.5f + (px / W - 0.5f) / zoom, 0.5f + (py / H - 0.5f) / zoom}; };
    const float xt = x + slant, xb = x - slant; // top / bottom of the edge
    const float lt = std::clamp(xt, 0.0f, W), lb = std::clamp(xb, 0.0f, W);
    if (lt < W || lb < W) {
        const SDL_Vertex v[4] = {{{lt, 0}, white, uv(lt, 0)}, {{W, 0}, white, uv(W, 0)}, {{W, H}, white, uv(W, H)}, {{lb, H}, white, uv(lb, H)}};
        static const int idx[6] = {0, 1, 2, 0, 2, 3};
        SDL_SetTextureAlphaMod(m_capture, Uint8(255.0f * (1.0f - 0.35f * e)));
        SDL_RenderGeometry(m_r, m_capture, v, 4, idx, 6);
    }
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    // a dark shadow on the leaving frame, the soft accent glow on both sides of the edge and a hot white core
    const float fade = std::sin(std::clamp(t, 0.0f, 1.0f) * kPi); // the light is strongest mid-sweep
    const float sh = 70.0f * H / 720.0f, gl = 150.0f * H / 720.0f, core = 4.0f * H / 720.0f;
    Quad({xt, 0}, {xt + sh, 0}, {xb + sh, H}, {xb, H}, {0, 0, 0, Uint8(120.0f * fade)}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, Uint8(120.0f * fade)});
    Quad({xt - gl, 0}, {xt, 0}, {xb, H}, {xb - gl, H}, Alpha(kAccent, 0.0f), Alpha(kAccent, 0.55f * fade), Alpha(kAccent, 0.55f * fade), Alpha(kAccent, 0.0f));
    Quad({xt, 0}, {xt + gl * 0.35f, 0}, {xb + gl * 0.35f, H}, {xb, H}, Alpha(kWarm, 0.45f * fade), Alpha(kWarm, 0.0f), Alpha(kWarm, 0.0f), Alpha(kWarm, 0.45f * fade));
    Quad({xt - core, 0}, {xt + core, 0}, {xb + core, H}, {xb - core, H}, Alpha(white, 0.9f * fade), Alpha(white, 0.9f * fade), Alpha(white, 0.9f * fade), Alpha(white, 0.9f * fade));
}

void ModernUI::DrawOverlay(int w, int h, const char lines[][96], int count, bool left)
{
    if (!m_font || count <= 0)
        return;
    const float s = float(h) / 720.0f, panel = 400 * s, px = left ? 8 * s : float(w) - panel - 8 * s, py = 96 * s, lh = 21 * s; // top right by default, under the timer row
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(px - 8 * s, py - 6 * s, panel + 8 * s, lh * float(count) + 12 * s, {0, 0, 0, 175}, {0, 0, 0, 175});
    for (int i = 0; i < count; ++i) {
        SDL_Color c = i == 0 ? SDL_Color{120, 255, 160, 255} : SDL_Color{225, 235, 245, 255};
        if (std::strstr(lines[i], "GPU-bound") || std::strstr(lines[i], "CPU-bound"))
            c = {255, 190, 70, 255};
        Text(px, py + lh * float(i), 17 * s, lines[i], c, 0);
    }
}

SDL_Texture *ModernUI::LoadingFrame(int index)
{
    auto it = m_frames.find(index);
    if (it != m_frames.end())
        return it->second;
    if (m_frames.size() >= 6) { // keep only a few: drop the one furthest from the request
        auto far = m_frames.begin();
        for (auto k = m_frames.begin(); k != m_frames.end(); ++k)
            if (std::abs(k->first - index) > std::abs(far->first - index))
                far = k;
        if (far->second)
            SDL_DestroyTexture(far->second);
        m_frames.erase(far);
    }
    char name[600];
    snprintf(name, sizeof(name), "%sf%03d.jpg", m_frameDir.c_str(), index + 1);
    SDL_Texture *tex = nullptr;
    int iw = 0, ih = 0, comp = 0;
    if (unsigned char *img = stbi_load(name, &iw, &ih, &comp, 4)) {
        tex = SDL_CreateTexture(m_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
        SDL_UpdateTexture(tex, nullptr, img, iw * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        stbi_image_free(img);
    }
    m_frames[index] = tex;
    return tex;
}

bool ModernUI::DrawLoading(int w, int h, const char *stage, float fraction, float t)
{
    if (!m_font || (!m_loadArt && m_frameCount < 2))
        return false;
    const float W = float(w), H = float(h);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    SDL_SetRenderDrawColor(m_r, 0, 0, 0, 255);
    SDL_RenderClear(m_r);
    // the art, "cover" fitted with a slow push-in and a small sideways drift (Ken Burns), so the screen is never static
    const float zoom = 1.04f + 0.035f * (0.5f - 0.5f * std::cos(t * 0.22f));
    const float cover = std::max(W / m_loadW, H / m_loadH) * zoom;
    const float dw = m_loadW * cover, dh = m_loadH * cover;
    const float ox = (W - dw) * (0.5f + 0.35f * std::sin(t * 0.17f)), oy = (H - dh) * 0.5f;
    const SDL_FRect dst{ox, oy, dw, dh};
    if (m_frameCount >= 2) {
        // ping-pong playback of the frames (16 fps) with a cross-fade between neighbours, so the loop never jumps
        const float pos = t * 16.0f, period = float(2 * (m_frameCount - 1));
        float p = std::fmod(pos, period);
        if (p > float(m_frameCount - 1))
            p = period - p;
        const int i0 = std::clamp(int(p), 0, m_frameCount - 1), i1 = std::min(i0 + 1, m_frameCount - 1);
        const float k = p - float(i0);
        SDL_Texture *a = LoadingFrame(i0), *b = LoadingFrame(i1);
        if (a) {
            SDL_SetTextureBlendMode(a, SDL_BLENDMODE_NONE);
            SDL_RenderCopyF(m_r, a, nullptr, &dst);
        }
        if (b && i1 != i0 && k > 0.02f) {
            SDL_SetTextureBlendMode(b, SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(b, Uint8(std::clamp(k, 0.0f, 1.0f) * 255.0f));
            SDL_RenderCopyF(m_r, b, nullptr, &dst);
        }
    } else {
        SDL_RenderCopyF(m_r, m_loadArt, nullptr, &dst);
    }
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    // grading: darker top and a deep floor for the text, soft side vignette
    Rect(0, 0, W, H * 0.20f, {0, 4, 14, 150}, {0, 4, 14, 0});
    Rect(0, H * 0.58f, W, H * 0.42f, {0, 4, 14, 0}, {0, 4, 14, 235});
    Quad({0, 0}, {W * 0.10f, 0}, {W * 0.10f, H}, {0, H}, {0, 0, 0, 120}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 120});
    Quad({W * 0.90f, 0}, {W, 0}, {W, H}, {W * 0.90f, H}, {0, 0, 0, 0}, {0, 0, 0, 120}, {0, 0, 0, 120}, {0, 0, 0, 0});
    Quad({0, 0}, {W, 0}, {W, 3}, {0, 3}, kAccent, kWarm, kWarm, kAccent);

    const float u = H / 720.0f; // layout unit: everything scales with the window height
    const float mx = W * 0.055f;
    // title block, lower left
    Text(mx, H - 236 * u, 15 * u, "NEED FOR SPEED", Alpha(kAccent, 0.95f));
    Text(mx - 2 * u, H - 218 * u, 58 * u, "UNDERGROUND 2", kWhite);
    Quad({mx, H - 150 * u}, {mx + 70 * u, H - 150 * u}, {mx + 70 * u, H - 147 * u}, {mx, H - 147 * u}, kWarm, kWarm, kWarm, kWarm);
    // hint (rotates every 6 s, cross-fades)
    static const char *const kHints[] = {
        "Pull the handbrake at speed to start a drift - chain them for a bigger multiplier.",
        "Nitrous is best saved for the straights after a long corner.",
        "Tel Aviv, Jerusalem, Haifa, Be'er Sheva: every district has its own character.",
        "Visit a shop to tune your car: Performance, Body, Graphics and Specialties each tune your car differently.",
        "Fit better tyres before you chase more power - grip wins races.",
        "Press Esc at any time to pause, restart or change options.",
    };
    const int nh = int(sizeof(kHints) / sizeof(kHints[0]));
    const float hs = t / 6.0f;
    const int hi = int(hs) % nh;
    const float ph = hs - std::floor(hs);
    const float fade = std::min(std::min(ph * 8.0f, (1.0f - ph) * 8.0f), 1.0f);
    Text(mx, H - 128 * u, 12 * u, "HINT", Alpha(kWarm, 0.9f * fade));
    Text(mx + 46 * u, H - 132 * u, 18 * u, kHints[hi], Alpha({210, 222, 238, 255}, 0.92f * fade));

    // progress bar along the bottom: a glass track, a gradient fill with a bright head and a sweeping sheen
    const float bx = mx, bw = W - 2 * mx, by = H - 52 * u, bh = 6 * u;
    Rect(bx, by, bw, bh, {255, 255, 255, 38}, {255, 255, 255, 22});
    const float fw = std::max(bh, bw * fraction);
    Quad({bx, by}, {bx + fw, by}, {bx + fw, by + bh}, {bx, by + bh}, kAccent, Mix(kAccent, kWarm, 0.55f), Mix(kAccent, kWarm, 0.55f), kAccent);
    const float sheen = std::fmod(t * 0.55f, 1.6f) - 0.3f;
    {
        const float sx = bx + fw * sheen, sw = 90 * u;
        const float x0 = std::clamp(sx, bx, bx + fw), x1 = std::clamp(sx + sw, bx, bx + fw);
        if (x1 > x0 + 1)
            Quad({x0, by}, {x1, by}, {x1, by + bh}, {x0, by + bh}, {255, 255, 255, 0}, {255, 255, 255, 120}, {255, 255, 255, 120}, {255, 255, 255, 0});
    }
    Glow(bx + fw, by + bh * 0.5f, 34 * u, Alpha(kAccent, 0.85f));
    // stage label (left) and percentage (right)
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", int(fraction * 100.0f + 0.5f));
    Text(bx, by - 34 * u, 20 * u, stage && *stage ? stage : "Loading", Alpha(kWhite, 0.95f));
    Text(bx + bw, by - 40 * u, 30 * u, pct, kWhite, 2);
    // spinner next to the stage label's baseline: a short comet orbiting a ring
    {
        const float cx = bx + bw - 118 * u, cy = by - 24 * u, r = 9 * u;
        Ring(cx, cy, r - 1.4f * u, r, Alpha(kWhite, 0.18f));
        const float a = std::fmod(t * 4.2f, 2 * kPi);
        Arc(cx, cy, r - 1.8f * u, r + 0.4f * u, a - 1.5f, a, Alpha(kAccent, 0.0f), kAccent);
    }
    return true;
}

// ---- The police pursuit HUD (ISSUES #318) -------------------------------------------------------------------------------------------------
void ModernUI::DrawPursuit(int w, int h, const PursuitHud &p, float t)
{
    if (!m_font || p.state == 0)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    const SDL_Color red{255, 52, 48, 255}, blue{60, 120, 255, 255};
    const bool result = p.state >= 3;
    // red / blue flashing at the edges while the law is close
    if (!result && p.nearest < 140.0f) {
        const float k = std::clamp(1.0f - p.nearest / 140.0f, 0.0f, 1.0f);
        const float ph = std::fmod(t * 2.6f, 1.0f);
        const float lf = (ph < 0.5f ? std::sin(ph * 12.566f) : 0.0f), rf = (ph >= 0.5f ? std::sin((ph - 0.5f) * 12.566f) : 0.0f);
        const float ew = 150.0f * s * (0.6f + k);
        Quad({0, 0}, {ew, 0}, {ew, H}, {0, H}, Alpha(blue, 0.50f * k * std::max(0.0f, lf)), Alpha(blue, 0.0f), Alpha(blue, 0.0f), Alpha(blue, 0.50f * k * std::max(0.0f, lf)));
        Quad({W - ew, 0}, {W, 0}, {W, H}, {W - ew, H}, Alpha(red, 0.0f), Alpha(red, 0.50f * k * std::max(0.0f, rf)), Alpha(red, 0.50f * k * std::max(0.0f, rf)), Alpha(red, 0.0f));
    }
    if (!result) {
        // top: the heat panel
        const float pw = 430 * s, ph2 = 78 * s, px = W * 0.5f - pw * 0.5f, py = 16 * s;
        const bool cool = p.state == 2;
        Glass(px, py, pw, ph2, 20 * s, {6, 10, 22, 205}, cool ? SDL_Color{90, 220, 190, 190} : SDL_Color{255, 90, 80, 190});
        Quad({px, py}, {px + pw - 20 * s, py}, {px + pw - 20 * s, py + 3 * s}, {px, py + 3 * s}, blue, red, red, blue);
        for (int i = 0; i < 5; ++i) { // heat pips: badges that flash blue / red
            const float cx = px + 32 * s + float(i) * 30 * s, cy = py + 24 * s, r = 11 * s;
            const bool on = i < p.heat;
            const float fl = 0.6f + 0.4f * std::sin(t * 9.0f + float(i));
            const SDL_Color c = on ? ((i + int(t * 4.0f)) % 2 == 0 ? red : blue) : SDL_Color{50, 60, 80, 200};
            Quad({cx, cy - r}, {cx + r, cy}, {cx, cy + r}, {cx - r, cy}, Alpha(c, on ? fl : 0.8f), Alpha(c, on ? fl : 0.8f), Alpha(c, on ? fl : 0.8f), Alpha(c, on ? fl : 0.8f));
        }
        Text(px + 20 * s, py + 40 * s, 22 * s, cool ? "COOLING DOWN" : (p.nearest < 60.0f ? "PURSUIT  -  CLOSE" : "PURSUIT"), cool ? SDL_Color{120, 240, 205, 255} : kWhite, 0);
        const int secs = int(p.chaseTime);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
        Text(px + pw - 24 * s, py + 12 * s, 26 * s, buf, kWhite, 2);
        snprintf(buf, sizeof(buf), "UNITS %d", p.units);
        Text(px + pw - 24 * s, py + 44 * s, 18 * s, buf, {170, 195, 235, 235}, 2);
        snprintf(buf, sizeof(buf), "BOUNTY  $%d", int(p.bounty));
        Text(px + 190 * s, py + 12 * s, 24 * s, buf, kWarm, 0);
        snprintf(buf, sizeof(buf), "HEAT %d", p.heat);
        Text(px + 190 * s, py + 44 * s, 18 * s, buf, {170, 195, 235, 235}, 0);
        // bottom: the bars
        auto bar = [&](float y, const char *label, float v, SDL_Color c) {
            const float bw = 340 * s, bx = W * 0.5f - bw * 0.5f, bh = 12 * s;
            Glass(bx - 8 * s, y - 24 * s, bw + 16 * s, 40 * s, 10 * s, {6, 10, 22, 190}, Alpha(c, 0.7f));
            Rect(bx, y, bw, bh, {20, 26, 40, 230}, {12, 16, 28, 230});
            const float k = std::clamp(v, 0.0f, 1.0f);
            Rect(bx, y, bw * k, bh, Alpha(c, 1.0f), Alpha(c, 0.7f));
            Text(bx, y - 22 * s, 17 * s, label, Alpha(c, 1.0f), 0);
        };
        if (p.bust > 0.02f)
            bar(H - 150 * s, "BUSTED", p.bust, red);
        if (p.cooldown > 0.02f)
            bar(H - (p.bust > 0.02f ? 205.0f : 150.0f) * s, "COOLDOWN", p.cooldown, {90, 235, 190, 255});
    }
    // banner
    if (p.banner > 0.0f && !result) {
        const float a = std::clamp(std::min(p.banner / 0.25f, (3.2f - p.banner) / 0.5f), 0.0f, 1.0f);
        const float sc = 1.0f + (1.0f - std::clamp(p.banner / 0.3f, 0.0f, 1.0f)) * 0.5f;
        const float cy = H * 0.30f;
        Quad({0, cy - 46 * s}, {W, cy - 46 * s}, {W, cy + 52 * s}, {0, cy + 52 * s}, Alpha({10, 14, 28, 255}, 0.0f), Alpha({10, 14, 28, 255}, 0.0f), Alpha({10, 14, 28, 255}, 0.0f),
             Alpha({10, 14, 28, 255}, 0.0f));
        Rect(0, cy - 40 * s, W, 84 * s, Alpha({8, 12, 26, 255}, 0.72f * a), Alpha({8, 12, 26, 255}, 0.72f * a));
        Rect(0, cy - 40 * s, W, 3 * s, Alpha(red, a), Alpha(blue, a));
        Rect(0, cy + 41 * s, W, 3 * s, Alpha(blue, a), Alpha(red, a));
        Text(W * 0.5f, cy - 30 * s, 50 * s * sc, p.bannerText, Alpha(kWhite, a), 1);
        Text(W * 0.5f, cy + 20 * s, 20 * s, p.bannerSub, Alpha({170, 205, 255, 255}, a), 1);
    }
    // result
    if (result) {
        const float a = std::clamp(p.resultTimer / 0.5f, 0.0f, 1.0f);
        const bool busted = p.state == 3;
        const SDL_Color c = busted ? red : SDL_Color{90, 235, 150, 255};
        Rect(0, 0, W, H, Alpha({2, 4, 12, 255}, 0.55f * a), Alpha({2, 4, 12, 255}, 0.78f * a));
        const float pw = 560 * s, ph2 = 330 * s, px = W * 0.5f - pw * 0.5f, py = H * 0.5f - ph2 * 0.5f;
        Glow(W * 0.5f, H * 0.5f, pw * 0.9f, Alpha(c, 0.22f * a));
        Glass(px, py, pw, ph2, 30 * s, {8, 12, 26, Uint8(235 * a)}, Alpha(c, 0.8f * a));
        Text(W * 0.5f, py + 24 * s, 80 * s, busted ? "BUSTED" : "ESCAPED", Alpha(c, a), 1);
        char buf[96];
        const int secs = int(p.chaseTime);
        const float ly = py + 130 * s;
        snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
        Text(px + 50 * s, ly, 24 * s, "PURSUIT TIME", {140, 175, 215, 235}, 0), Text(px + pw - 50 * s, ly, 24 * s, buf, kWhite, 2);
        snprintf(buf, sizeof(buf), "%d", p.heat);
        Text(px + 50 * s, ly + 34 * s, 24 * s, "HEAT LEVEL", {140, 175, 215, 235}, 0), Text(px + pw - 50 * s, ly + 34 * s, 24 * s, buf, kWhite, 2);
        snprintf(buf, sizeof(buf), "$%d", int(p.bounty));
        Text(px + 50 * s, ly + 68 * s, 24 * s, "BOUNTY", {140, 175, 215, 235}, 0), Text(px + pw - 50 * s, ly + 68 * s, 24 * s, buf, kWarm, 2);
        snprintf(buf, sizeof(buf), busted ? "-$%d" : "+$%d", int(p.amount));
        Text(px + 50 * s, ly + 104 * s, 30 * s, busted ? "FINE" : "REWARD", Alpha(c, 1.0f), 0), Text(px + pw - 50 * s, ly + 104 * s, 30 * s, buf, Alpha(c, 1.0f), 2);
        const float pulse = 0.65f + 0.35f * std::sin(t * 5.0f);
        Text(W * 0.5f, py + ph2 - 36 * s, 18 * s, "PRESS ENTER", Alpha(kWhite, pulse * a), 1);
    }
}
