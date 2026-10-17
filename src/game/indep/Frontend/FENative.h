#pragma once
#include <SDL.h>

#include <cstdint>
#include <map>
#include <vector>

#include "Speed/Indep/Src/FEng/FEGameInterface.h"
#include "Speed/Indep/Src/FEng/FEngine.h"
#include "game/indep/Frontend/FEText.h"

class TexturePack;

// Native front-end host: the game side of EA's FEng engine, replacing MW's cFEng/cFEngRender
// (which drive the console renderer). Draws FEng objects with SDL's geometry API.
class FENativeInterface : public FEGameInterface {
public:
    static constexpr int kScreenWidth = 640;  // FEng screen space (SCREEN_WIDTH in MW)
    static constexpr int kScreenHeight = 480;

    explicit FENativeInterface(SDL_Renderer *renderer);
    virtual ~FENativeInterface();

    // Packages and textures come from loaded bundles (decompressed .BUN/.LZC data).
    void AddBundle(const uint8_t *data, size_t size);

    u8 *GetPackageData(const char *pPackageName, u8 **pBlockStart, bool &bDeleteBlock) override;
    bool LoadResources(FEPackage *pPackage, i32 Count, FEResourceRequest *pList) override;
    bool UnloadResources(FEPackage *, i32, FEResourceRequest *) override { return true; }
    void PackageWasLoaded(FEPackage *) override {}
    bool PackageWillUnload(FEPackage *) override { return true; }
    void NotificationMessage(u32, FEObject *, u32, u32) override {}
    void NotifySoundMessage(u32, FEObject *, u32, u32) override {}
    void BeginPackageRendering(FEPackage *) override {}
    void EndPackageRendering(FEPackage *) override {}
    void GenerateRenderContext(u16 uContext, FEObject *pObject) override;
    bool GetContextTransform(u16 uContext, FEMatrix4 &Matrix) override;
    void RenderObject(FEObject *pObject) override;
    void GetViewTransformation(FEMatrix4 *pView) override;
    u32 GetJoyPadMask(u8) override { return 0; }
    void GetMouseInfo(FEMouseInfo &) override {}
    bool DoesPointTouchObject(float, float, FEObject *) override { return false; }

private:
    // 2D affine transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty
    struct Affine {
        float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
        Affine operator*(const Affine &o) const; // apply o first, then this
        void Apply(float x, float y, float &ox, float &oy) const;
    };
    struct Context {
        Affine xform;
        int r = 255, g = 255, b = 255, a = 255;
    };

    Affine LocalTransform(const FEObject *obj) const;
    void ObjectColor(const FEObject *obj, int &r, int &g, int &b, int &a) const;
    SDL_Texture *TextureFor(uint32_t hash);
    void DrawImage(FEObject *obj);
    void DrawString(FEObject *obj);
    void DrawQuad(const Affine &xform, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                  SDL_Color col, SDL_Texture *tex);

    SDL_Renderer *m_renderer;
    std::vector<std::pair<const uint8_t *, size_t>> m_bundles;
    std::vector<TexturePack *> m_packs;
    std::map<uint32_t, SDL_Texture *> m_textures; // by texture name hash
    SDL_Texture *m_white = nullptr;               // stand-in for textures we cannot decode yet
    std::map<uint32_t, FEFont> m_fonts;           // by font name hash
    std::vector<FELanguage> m_languages;          // string tables, searched in load order
    Context m_contexts[256];
};
