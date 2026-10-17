#include "game/indep/Frontend/FENative.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "Speed/Indep/Src/FEng/FEImage.h"
#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/Src/FEng/FEString.h"
#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/bWare/bChunk.h"
#include "game/bWare/bCompression.h"
#include "game/indep/Texture/TexturePack.h"

namespace {
constexpr uint32_t kChunkFEngPackage = 0x00030203; // BCHUNK_FENG_PACKAGE
constexpr uint32_t kChunkFEngPackageLZ = 0x00030210; // BCHUNK_FENG_COMPRESSED_PACKAGE: u32 hash + JDLZ
constexpr uint32_t kResourceImage = 1;             // FEResourceRequest::Type for textures
constexpr uint32_t kResourceFont = 2;              // FEResourceRequest::Type for fonts
constexpr uint32_t kChunkFEngFont = 0x00030201;    // BCHUNK_FENG_FONT
constexpr uint32_t kChunkLanguage = 0x00039000;    // BCHUNK_LANGUAGE

std::string LowerName(const char *s)
{
    std::string r(s);
    for (char &c : r)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return r;
}

// FEng package short name lives in the PkHd chunk: u32[6] = version, ?, resources, objects,
// name length, file name length, then the short name.
const char *PackageShortName(const uint8_t *feng)
{
    return reinterpret_cast<const char *>(feng + 8 + 8 + 24);
}
} // namespace

FENativeInterface::Affine FENativeInterface::Affine::operator*(const Affine &o) const
{
    Affine r;
    r.a = a * o.a + c * o.b;
    r.b = b * o.a + d * o.b;
    r.c = a * o.c + c * o.d;
    r.d = b * o.c + d * o.d;
    r.tx = a * o.tx + c * o.ty + tx;
    r.ty = b * o.tx + d * o.ty + ty;
    return r;
}

void FENativeInterface::Affine::Apply(float x, float y, float &ox, float &oy) const
{
    ox = a * x + c * y + tx;
    oy = b * x + d * y + ty;
}

FENativeInterface::FENativeInterface(SDL_Renderer *renderer) : m_renderer(renderer)
{
    m_white = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 1, 1);
    uint32_t px = 0xffffffff;
    SDL_UpdateTexture(m_white, nullptr, &px, 4);
    SDL_SetTextureBlendMode(m_white, SDL_BLENDMODE_BLEND);
}

FENativeInterface::~FENativeInterface()
{
    for (auto &t : m_textures)
        if (t.second && t.second != m_white)
            SDL_DestroyTexture(t.second);
    SDL_DestroyTexture(m_white);
    for (TexturePack *p : m_packs)
        delete p;
}

void FENativeInterface::AddBundle(const uint8_t *data, size_t size)
{
    m_bundles.emplace_back(data, size);
    for (const uint8_t *p = data, *end = data + size; p + 8 <= end;) {
        const bChunk *c = reinterpret_cast<const bChunk *>(p);
        if (c->Data() + c->size > end)
            break;
        if (c->id == kChunkFEngPackage) {
            m_packages[LowerName(PackageShortName(c->Data()))] = c->Data();
        } else if (c->id == kChunkFEngPackageLZ && c->size > 4 + 18 && bIsCompressed(c->Data() + 4)) {
            auto buf = std::make_unique<std::vector<uint8_t>>(bGetDecompressedSize(c->Data() + 4));
            if (bDecompress(c->Data() + 4, buf->data()) == buf->size()) {
                m_packages[LowerName(PackageShortName(buf->data()))] = buf->data();
                m_unpacked.push_back(std::move(buf));
            }
        } else if (c->id == kChunkFEngFont) {
            FEFont font;
            if (font.Parse(c->Data(), c->size))
                m_fonts[FEResourceHash(font.Name().c_str())] = font;
        } else if (c->id == kChunkLanguage) {
            FELanguage lang;
            if (lang.Parse(c->Data(), c->size))
                m_languages.push_back(lang);
        }
        p = c->Data() + c->size;
    }
    auto *pack = new TexturePack();
    if (pack->Parse(data, size))
        m_packs.push_back(pack);
    else
        delete pack;
}

u8 *FENativeInterface::GetPackageData(const char *name, u8 **pBlockStart, bool &bDeleteBlock)
{
    const char *base = name;
    for (const char *p = name; *p; ++p)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    auto it = m_packages.find(LowerName(base));
    if (it == m_packages.end()) {
        fprintf(stderr, "FEng: package %s not found in loaded bundles\n", name);
        return nullptr;
    }
    *pBlockStart = const_cast<u8 *>(it->second);
    bDeleteBlock = false;
    return const_cast<u8 *>(it->second);
}

std::vector<std::string> FENativeInterface::PackageNames() const
{
    std::vector<std::string> names;
    for (auto &p : m_packages)
        names.push_back(p.first);
    return names;
}

bool FENativeInterface::LoadResources(FEPackage *, i32 count, FEResourceRequest *list)
{
    for (i32 i = 0; i < count; ++i)
        if ((list[i].Type == kResourceImage || list[i].Type == kResourceFont) && list[i].pFilename)
            list[i].Handle = FEResourceHash(list[i].pFilename);
    return true;
}

SDL_Texture *FENativeInterface::TextureFor(uint32_t hash)
{
    auto it = m_textures.find(hash);
    if (it != m_textures.end())
        return it->second;
    SDL_Texture *tex = nullptr;
    for (TexturePack *pack : m_packs) {
        for (size_t i = 0; i < pack->Count() && !tex; ++i) {
            const TextureInfo &info = pack->At(i);
            if (info.nameHash != hash)
                continue;
            DecodedTexture img;
            if (pack->Decode(info, img)) {
                tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.width,
                                        img.height);
                SDL_UpdateTexture(tex, nullptr, img.rgba.data(), img.width * 4);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            } else {
                fprintf(stderr, "FEng: texture %s (%d-bit) not decodable yet, drawing flat colour\n",
                        info.name.c_str(), info.bitsPerPixel);
                tex = m_white;
            }
        }
    }
    if (!tex)
        fprintf(stderr, "FEng: texture %08x not in any loaded pack\n", hash);
    m_textures[hash] = tex;
    return tex;
}

// Object transform as in MW cFEngRender::MakeRenderMatrix: scale by Size, rotate about Pivot,
// translate by Pos. Only rotation about the screen axis is used by 2D front-end screens.
FENativeInterface::Affine FENativeInterface::LocalTransform(const FEObject *obj) const
{
    const FEObjData *d = obj->GetObjData();
    Affine scale;
    scale.a = d->Size.x;
    scale.d = d->Size.y;
    float angle = 2.0f * std::atan2(d->Rot.z, d->Rot.w);
    Affine rot;
    rot.a = std::cos(angle);
    rot.b = std::sin(angle);
    rot.c = -rot.b;
    rot.d = rot.a;
    Affine toPivot, fromPivot, pos;
    toPivot.tx = -d->Pivot.x;
    toPivot.ty = -d->Pivot.y;
    fromPivot.tx = d->Pivot.x;
    fromPivot.ty = d->Pivot.y;
    pos.tx = d->Pos.x;
    pos.ty = d->Pos.y;
    return pos * fromPivot * rot * toPivot * scale;
}

void FENativeInterface::ObjectColor(const FEObject *obj, int &r, int &g, int &b, int &a) const
{
    const FEObjData *d = obj->GetObjData();
    r = std::clamp(int(d->Col.r), 0, 255);
    g = std::clamp(int(d->Col.g), 0, 255);
    b = std::clamp(int(d->Col.b), 0, 255);
    a = std::clamp(int(d->Col.a), 0, 255);
    if (obj->RenderContext != 0) {
        const Context &con = m_contexts[obj->RenderContext];
        r = (con.r * r + 128) >> 8;
        g = (con.g * g + 128) >> 8;
        b = (con.b * b + 128) >> 8;
        a = (con.a * a + 128) >> 8;
    }
}

void FENativeInterface::GenerateRenderContext(u16 uContext, FEObject *pObject)
{
    Context &con = m_contexts[uContext & 0xff];
    Affine local = LocalTransform(pObject);
    con.xform = pObject->RenderContext != 0 ? m_contexts[pObject->RenderContext].xform * local : local;
    ObjectColor(pObject, con.r, con.g, con.b, con.a);
}

bool FENativeInterface::GetContextTransform(u16 uContext, FEMatrix4 &m)
{
    m.Identify();
    if (uContext == 0)
        return true;
    const Affine &x = m_contexts[uContext & 0xff].xform;
    m.m11 = x.a;
    m.m12 = x.b;
    m.m21 = x.c;
    m.m22 = x.d;
    m.m41 = x.tx;
    m.m42 = x.ty;
    return true;
}

void FENativeInterface::GetViewTransformation(FEMatrix4 *pView)
{
    pView->Identify();
}

void FENativeInterface::DrawImage(FEObject *obj)
{
    const FEImageData *d = reinterpret_cast<const FEImageData *>(obj->pData);
    if (getenv("NFSU2_FEDEBUG"))
        fprintf(stderr, "FEImage %08x pos (%.1f,%.1f,%.1f) size (%.1f,%.1f) ctx %u flags %08x\n", obj->NameHash, d->Pos.x,
                d->Pos.y, d->Pos.z, d->Size.x, d->Size.y, obj->RenderContext, obj->Flags);
    Affine xform = LocalTransform(obj);
    if (obj->RenderContext != 0)
        xform = m_contexts[obj->RenderContext].xform * xform;
    Affine screen;
    screen.tx = kScreenWidth / 2.0f;
    screen.ty = kScreenHeight / 2.0f;
    xform = screen * xform;

    int r, g, b, a;
    ObjectColor(obj, r, g, b, a);
    if (a == 0)
        return;
    SDL_Color col = {Uint8(r), Uint8(g), Uint8(b), Uint8(a)};

    SDL_Texture *tex = obj->Handle ? TextureFor(obj->Handle) : m_white;
    if (!tex)
        return; // texture not in any loaded pack: draw nothing rather than a placeholder
    DrawQuad(xform, -0.5f, -0.5f, 0.5f, 0.5f, d->UpperLeft.x, d->UpperLeft.y, d->LowerRight.x, d->LowerRight.y, col, tex);
}

void FENativeInterface::DrawQuad(const Affine &xform, float x0, float y0, float x1, float y1, float u0, float v0,
                                 float u1, float v1, SDL_Color col, SDL_Texture *tex)
{
    const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    SDL_Vertex v[4];
    for (int i = 0; i < 4; ++i) {
        xform.Apply(corners[i][0], corners[i][1], v[i].position.x, v[i].position.y);
        v[i].color = col;
        v[i].tex_coord = {uv[i][0], uv[i][1]};
    }
    const int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(m_renderer, tex, v, 4, indices, 6);
}

// Text, following nfsmw FEngFont::RenderString: lines break on '\n' or '^', justification from
// FEString::Format, glyph quads at x + kern + offsetX, advancing by the glyph's advance.
void FENativeInterface::DrawString(FEObject *obj)
{
    FEString *str = static_cast<FEString *>(obj);
    auto font = m_fonts.find(obj->Handle);
    if (font == m_fonts.end()) {
        static uint32_t warned = 0;
        if (warned != obj->Handle)
            fprintf(stderr, "FEng: font %08x not loaded\n", obj->Handle);
        warned = obj->Handle;
        return;
    }
    const FEFont &f = font->second;

    // Text: the object's own string, else its label from the language tables.
    std::vector<uint16_t> text;
    if (const i16 *w = str->GetString(); w && *w) {
        for (; *w; ++w)
            text.push_back(static_cast<uint16_t>(*w));
    } else if (str->GetLabelHash() != 0xFFFFFFFF) {
        for (const FELanguage &lang : m_languages)
            if (const char *s8 = lang.Find(str->GetLabelHash())) {
                for (; *s8; ++s8)
                    text.push_back(static_cast<unsigned char>(*s8));
                break;
            }
    }
    if (getenv("NFSU2_FEDEBUG")) {
        const FEObjData *dd = obj->GetObjData();
        fprintf(stderr, "FEString %08x label %08x len %zu pos (%.1f,%.1f,%.1f) size (%.2f,%.2f) pivot (%.0f,%.0f) flags %08x ctx %u fmt %x a=%d text:",
                obj->NameHash, str->GetLabelHash(), text.size(), dd->Pos.x, dd->Pos.y, dd->Pos.z, dd->Size.x, dd->Size.y,
                dd->Pivot.x, dd->Pivot.y, obj->Flags, obj->RenderContext, str->Format, int(dd->Col.a));
        for (size_t i = 0; i < text.size() && i < 60; ++i)
            fputc(text[i] < 128 ? int(text[i]) : '?', stderr);
        fputc('\n', stderr);
    }
    if (text.empty())
        return;

    auto isNewline = [](uint16_t c) { return c == '\n' || c == '^'; };
    auto lineWidth = [&](size_t from) {
        float w = 0;
        for (size_t i = from; i < text.size() && !isNewline(text[i]); ++i)
            if (const FontGlyph *g = f.Glyph(text[i]))
                w += g->advanceX;
        return w;
    };
    int lines = 1;
    for (uint16_t c : text)
        lines += isNewline(c);
    const float leading = static_cast<float>(str->Leading);
    const float textHeight = lines * f.Height() + (lines - 1) * leading;
    const u32 format = str->Format;
    auto xOffset = [&](float w) {
        return (format & FESTRING_JUSTIFY_HCENTER) ? -0.5f * w : (format & FESTRING_JUSTIFY_HRIGHT) ? -w : 0.0f;
    };
    float y = (format & FESTRING_JUSTIFY_VCENTER) ? -0.5f * textHeight : (format & FESTRING_JUSTIFY_VBOTTOM) ? -textHeight : 0.0f;
    float x = xOffset(lineWidth(0));

    Affine xform = LocalTransform(obj);
    // Glyphs are laid out in pixels; drop the unit-quad scale and keep Size as a multiplier.
    if (obj->RenderContext != 0)
        xform = m_contexts[obj->RenderContext].xform * xform;
    Affine screen;
    screen.tx = kScreenWidth / 2.0f;
    screen.ty = kScreenHeight / 2.0f;
    xform = screen * xform;

    int r, g, b, a;
    ObjectColor(obj, r, g, b, a);
    if (a == 0)
        return;
    SDL_Color col = {Uint8(r), Uint8(g), Uint8(b), Uint8(a)};
    SDL_Texture *tex = TextureFor(f.TextureHash());
    int tw = 1, th = 1;
    SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);

    uint16_t prev = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        uint16_t c = text[i];
        if (isNewline(c)) {
            y += f.Height() + leading;
            x = xOffset(lineWidth(i + 1));
            prev = 0;
            continue;
        }
        const FontGlyph *gl = f.Glyph(c);
        if (!gl)
            continue;
        float kern = prev ? static_cast<float>(f.Kern(gl, prev)) : 0.0f;
        float gw = std::max<float>(gl->width, 4.0f);
        float x0 = x + kern + gl->offsetX, y0 = y + gl->offsetY;
        DrawQuad(xform, x0, y0, x0 + gw, y0 + gl->height, float(gl->u) / tw, float(gl->v) / th,
                 float(gl->u + gl->width + 1) / tw, float(gl->v + gl->height) / th, col, tex);
        x += gl->advanceX + kern;
        prev = c;
    }
}

void FENativeInterface::RenderObject(FEObject *pObject)
{
    switch (pObject->Type) {
    case FE_Image:
        DrawImage(pObject);
        break;
    case FE_String:
        DrawString(pObject);
        break;
    default:
        // Movies, models, multi-images etc. are not drawn yet (ISSUES.md).
        break;
    }
}
