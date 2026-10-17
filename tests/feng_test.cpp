// Load a real UG2 front-end package (loading_boot.fng from GLOBAL\GLOBALA.BUN) with the reused
// FEng engine and list its objects. Proves MW's FEng reader understands UG2's .fng files.
// Usage: feng_test GLOBALA.BUN
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "Speed/Indep/Src/FEng/FEGameInterface.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/Src/FEng/FEngine.h"
#include "Speed/Indep/bWare/Inc/bMemory.hpp"
#include "game/bWare/bChunk.h"
#include "platform/linux/PlatformMemory.h"

// Accepts everything and draws nothing: enough for the engine to parse and start a package.
struct NullInterface : FEGameInterface {
    u8 *GetPackageData(const char *, u8 **, bool &) override { return nullptr; }
    bool LoadResources(FEPackage *, i32, FEResourceRequest *) override { return true; }
    bool UnloadResources(FEPackage *, i32, FEResourceRequest *) override { return true; }
    void PackageWasLoaded(FEPackage *) override {}
    bool PackageWillUnload(FEPackage *) override { return true; }
    void NotificationMessage(u32, FEObject *, u32, u32) override {}
    void NotifySoundMessage(u32, FEObject *, u32, u32) override {}
    void BeginPackageRendering(FEPackage *) override {}
    void EndPackageRendering(FEPackage *) override {}
    void GenerateRenderContext(u16, FEObject *) override {}
    bool GetContextTransform(u16, FEMatrix4 &) override { return false; }
    void RenderObject(FEObject *) override {}
    void GetViewTransformation(FEMatrix4 *) override {}
    u32 GetJoyPadMask(u8) override { return 0; }
    void GetMouseInfo(FEMouseInfo &) override {}
    bool DoesPointTouchObject(float, float, FEObject *) override { return false; }
};

static const char *TypeName(int t)
{
    static const char *names[] = {"None", "Image", "String", "Model", "List", "Group", "CodeList",
                                  "Movie", "Effect", "ColoredImage", "AnimImage", "SimpleImage", "MultiImage"};
    return t >= 0 && t < 13 ? names[t] : "User";
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    std::vector<uint8_t> bundle;
    for (int c; (c = fgetc(f)) != EOF;)
        bundle.push_back(uint8_t(c));
    fclose(f);

    platform::InitMainMemoryPool(32 << 20);

    NullInterface iface;
    FEngine engine;
    engine.SetInterface(&iface);

    int loaded = 0, failed = 0;
    const uint8_t *end = bundle.data() + bundle.size();
    for (const uint8_t *p = bundle.data(); p + 8 <= end;) {
        const bChunk *c = reinterpret_cast<const bChunk *>(p);
        if (c->id == 0x00030203) { // BCHUNK_FENG_PACKAGE
            FEPackage *pkg = engine.LoadPackage(c->Data(), false);
            if (!pkg) {
                printf("package at %#zx: FAILED to load\n", size_t(p - bundle.data()));
                ++failed;
            } else {
                ++loaded;
                printf("package %s (%s): %u objects\n", pkg->GetName(), pkg->GetFilename(), pkg->GetNumParentObjects());
                for (FEObject *o = pkg->GetFirstObject(); o; o = o->GetNext())
                    printf("  %-12s %08x %s\n", TypeName(o->Type), o->NameHash, o->pName ? o->pName : "");
            }
        }
        p = c->Data() + c->size;
    }
    printf("feng_test: %d loaded, %d failed\n", loaded, failed);
    // EA's static object pools are not safe to destroy at process exit (ISSUES.md #25).
    _Exit(failed || !loaded ? 1 : 0);
}

// Test-only stand-ins for the game-side FEng wrapper (cFEng lives in MW's FEngInterface.cpp).
#include "Speed/Indep/Src/Frontend/FEngInterfaces/FEngInterface.hpp"
cFEng *cFEng::mInstance = nullptr;
void cFEng::QueuePackageMessage(uint32, const char *, FEObject *) {}
