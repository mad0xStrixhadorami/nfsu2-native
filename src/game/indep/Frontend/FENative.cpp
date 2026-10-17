#include "game/indep/Frontend/FENative.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "Speed/Indep/Src/FEng/FEImage.h"
#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/bWare/bChunk.h"
#include "game/indep/Texture/TexturePack.h"

namespace {
constexpr uint32_t kChunkFEngPackage = 0x00030203; // BCHUNK_FENG_PACKAGE
constexpr uint32_t kResourceImage = 1;             // FEResourceRequest::Type for textures

// Texture lookup key used by UG2: bStringHash of the upper-case file name without extension
// ("NFSU_logo_chrome.tga" -> bStringHash("NFSU_LOGO_CHROME")), which equals the TPK name hash.
uint32_t TextureHashFromFilename(const char *filename)
{
    const char *base = filename;
    for (const char *p = filename; *p; ++p)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    char upper[128];
    size_t n = 0;
    for (; base[n] && base[n] != '.' && n < sizeof(upper) - 1; ++n)
        upper[n] = static_cast<char>(toupper(static_cast<unsigned char>(base[n])));
    upper[n] = '\0';
    return bStringHash(upper);
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
    for (auto &[data, size] : m_bundles) {
        const uint8_t *end = data + size;
        for (const uint8_t *p = data; p + 8 <= end;) {
            const bChunk *c = reinterpret_cast<const bChunk *>(p);
            if (c->id == kChunkFEngPackage && strcasecmp(PackageShortName(c->Data()), base) == 0) {
                *pBlockStart = const_cast<u8 *>(c->Data());
                bDeleteBlock = false;
                return const_cast<u8 *>(c->Data());
            }
            p = c->Data() + c->size;
        }
    }
    fprintf(stderr, "FEng: package %s not found in loaded bundles\n", name);
    return nullptr;
}

bool FENativeInterface::LoadResources(FEPackage *, i32 count, FEResourceRequest *list)
{
    for (i32 i = 0; i < count; ++i)
        if (list[i].Type == kResourceImage && list[i].pFilename)
            list[i].Handle = TextureHashFromFilename(list[i].pFilename);
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
    if (!tex) {
        fprintf(stderr, "FEng: texture %08x not in any loaded pack\n", hash);
        tex = m_white;
    }
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

    const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    const float uv[4][2] = {{d->UpperLeft.x, d->UpperLeft.y},
                            {d->LowerRight.x, d->UpperLeft.y},
                            {d->LowerRight.x, d->LowerRight.y},
                            {d->UpperLeft.x, d->LowerRight.y}};
    SDL_Vertex v[4];
    for (int i = 0; i < 4; ++i) {
        xform.Apply(corners[i][0], corners[i][1], v[i].position.x, v[i].position.y);
        v[i].color = col;
        v[i].tex_coord = {uv[i][0], uv[i][1]};
    }
    const int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(m_renderer, TextureFor(obj->Handle), v, 4, indices, 6);
}

void FENativeInterface::RenderObject(FEObject *pObject)
{
    switch (pObject->Type) {
    case FE_Image:
        DrawImage(pObject);
        break;
    default:
        // Strings, movies, models etc. are not drawn yet (ISSUES.md).
        break;
    }
}
