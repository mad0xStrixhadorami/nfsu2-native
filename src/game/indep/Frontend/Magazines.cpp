#include "game/indep/Frontend/Magazines.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "game/bWare/bChunk.h"

namespace {
uint32_t U32(const uint8_t *p, size_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}
} // namespace

bool Magazines::Load(const std::vector<uint8_t> &frontend, const std::vector<uint8_t> &showcase, std::vector<uint8_t> high, std::vector<uint8_t> low)
{
    m_covers.clear();
    m_high = std::move(high), m_low = std::move(low);
    if (m_high.empty() || !m_highPack.Parse(m_high.data(), m_high.size()))
        return false;
    if (!m_low.empty())
        m_lowPack.Parse(m_low.data(), m_low.size());
    auto known = [&](uint32_t h) { return h && (m_highPack.Count() ? true : false) && (m_cache.count(h) || true); };
    (void)known;
    auto inPack = [&](const TexturePack &p, uint32_t h) {
        for (size_t i = 0; i < p.Count(); ++i)
            if (p.At(i).nameHash == h)
                return true;
        return false;
    };
    auto codeOf = [&](uint32_t h) -> std::string {
        for (size_t i = 0; i < m_highPack.Count(); ++i)
            if (m_highPack.At(i).nameHash == h) {
                const std::string &n = m_highPack.At(i).name;
                const size_t k = n.find("_BG_");
                return k == std::string::npos ? std::string() : n.substr(0, k);
            }
        return std::string();
    };
    // FRONTEND chunk 0x30230: the cover table is the last table of the chunk, 16 records of 0x68 bytes (the texture hash words found at
    // offset 4396 of the payload on the PAL disc; located by scanning for the first background hash so the exact table offset is not hard coded)
    if (frontend.size() > 16 && U32(frontend.data(), 0) == 0x30230) {
        const uint8_t *d = frontend.data() + 8;
        const size_t n = U32(frontend.data(), 4);
        for (size_t off = 12; off + 0x68 <= n; off += 4) {
            const uint32_t w0 = U32(d, off);
            if (!w0 || !inPack(m_highPack, w0))
                continue;
            const std::string code = codeOf(w0);
            if (code.empty())
                continue;
            // a table of consecutive records of 0x68 bytes starts here
            for (size_t rec = off; rec + 0x68 <= n && inPack(m_highPack, U32(d, rec)); rec += 0x68) {
                Cover c;
                c.code = codeOf(U32(d, rec));
                for (int i = 0; i < 3; ++i)
                    c.bg[i] = U32(d, rec + size_t(i) * 4), c.model[i] = U32(d, rec + 0x14 + size_t(i) * 4), c.xt[i] = U32(d, rec + 0x28 + size_t(i) * 4);
                for (int i = 0; i < 2; ++i)
                    c.masthead[i] = U32(d, rec + 0x0c + size_t(i) * 4);
                if (!c.code.empty())
                    m_covers.push_back(c);
            }
            break;
        }
    }
    // SHOWCASE chunk 0x30231: 14 records of 56 bytes; word 3 = the label, words 9 / 10 = the two masthead textures
    if (showcase.size() > 16 && U32(showcase.data(), 0) == 0x30231) {
        const uint8_t *d = showcase.data() + 8;
        const size_t n = U32(showcase.data(), 4);
        for (size_t off = 12; off + 56 <= n; off += 4) {
            const uint32_t m0 = U32(d, off + 36), m1 = U32(d, off + 40), label = U32(d, off + 12);
            if (!label || !m0 || !(inPack(m_lowPack, m0) || inPack(m_highPack, m0)))
                continue;
            Cover c;
            c.full = false, c.label = label, c.masthead[0] = m0, c.masthead[1] = m1;
            std::string name;
            for (size_t i = 0; i < m_lowPack.Count(); ++i)
                if (m_lowPack.At(i).nameHash == m0)
                    name = m_lowPack.At(i).name;
            for (size_t i = 0; i < m_highPack.Count(); ++i)
                if (m_highPack.At(i).nameHash == m0)
                    name = m_highPack.At(i).name;
            const size_t k = name.find("_MAS");
            c.code = k == std::string::npos ? name : name.substr(0, k);
            m_covers.push_back(c);
            off += 52; // next record
        }
    }
    printf("[Magazines] %zu covers (%zu with full layers)\n", m_covers.size(),
           size_t(std::count_if(m_covers.begin(), m_covers.end(), [](const Cover &c) { return c.full; })));
    return !m_covers.empty();
}

const Magazines::Cover *Magazines::ForCode(const std::string &code) const
{
    const Cover *showcase = nullptr;
    for (const Cover &c : m_covers)
        if (c.code == code) {
            if (c.full)
                return &c;
            showcase = &c;
        }
    return showcase;
}

const Magazines::Cover *Magazines::ForLabel(uint32_t label) const
{
    for (const Cover &c : m_covers)
        if (!c.full && c.label == label)
            return &c;
    return nullptr;
}

SDL_Texture *Magazines::Texture(SDL_Renderer *r, uint32_t hash)
{
    if (!hash)
        return nullptr;
    auto it = m_cache.find(hash);
    if (it != m_cache.end())
        return it->second;
    SDL_Texture *tex = nullptr;
    for (const TexturePack *pack : {&m_highPack, &m_lowPack})
        for (size_t i = 0; i < pack->Count() && !tex; ++i)
            if (pack->At(i).nameHash == hash) {
                DecodedTexture dt;
                if (pack->Decode(pack->At(i), dt)) {
                    tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, dt.width, dt.height);
                    SDL_UpdateTexture(tex, nullptr, dt.rgba.data(), dt.width * 4);
                    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
                }
            }
    m_cache[hash] = tex;
    return tex;
}

void Magazines::Draw(SDL_Renderer *r, const Cover &c, const SDL_Rect &area, unsigned variant,
                     const std::function<void(const SDL_Rect &page)> &underModel)
{
    auto layer = [&](uint32_t h) {
        if (SDL_Texture *t = Texture(r, h))
            SDL_RenderCopy(r, t, nullptr, &area);
    };
    auto pick = [&](const uint32_t *arr, int n, unsigned v) -> uint32_t {
        int count = 0;
        uint32_t list[4];
        for (int i = 0; i < n; ++i)
            if (arr[i])
                list[count++] = arr[i];
        return count ? list[v % unsigned(count)] : 0;
    };
    if (c.full) {
        layer(pick(c.bg, 3, variant));
    } else { // a showcase magazine has no layers of its own: a brand-coloured page
        const SDL_Rect page = PageOf(area);
        const uint32_t h = variant * 2654435761u + uint32_t(c.code.size()) * 40503u + uint32_t(c.code.empty() ? 0 : c.code[0]) * 977u;
        const Uint8 r0 = Uint8(40 + (h & 0x7f)), g0 = Uint8(40 + ((h >> 8) & 0x7f)), b0 = Uint8(60 + ((h >> 16) & 0x7f));
        const SDL_Color top{r0, g0, b0, 255}, bottom{Uint8(r0 / 3), Uint8(g0 / 3), Uint8(b0 / 3), 255};
        const SDL_Vertex v[4] = {{{float(page.x), float(page.y)}, top, {0, 0}},
                                 {{float(page.x + page.w), float(page.y)}, top, {0, 0}},
                                 {{float(page.x + page.w), float(page.y + page.h)}, bottom, {0, 0}},
                                 {{float(page.x), float(page.y + page.h)}, bottom, {0, 0}}};
        static const int idx[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
    }
    if (underModel)
        underModel(PageOf(area));
    if (c.full) {
        layer(pick(c.model, 3, variant / 3));
    }
    layer(pick(c.masthead, 2, variant));
    if (c.full)
        layer(pick(c.xt, 3, variant));
}
