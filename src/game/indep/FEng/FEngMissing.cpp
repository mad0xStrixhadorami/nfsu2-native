// FEng pieces the nfsmw decompilation does not provide yet.
#include <cstdio>
#include <cstdlib>

#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/Src/FEng/FEngine.h"
#include "Speed/Indep/Src/Frontend/FEngInterfaces/FEngInterface.hpp"

// Global object GUID counter (declared in FEngine.h).
u32 FEngine::SysGUID = 0;

// TODO(ISSUES #24): decompile UG2's FEngine destructor (0x00288240). Only runs at shutdown.
FEngine::~FEngine() {}

// TODO(ISSUES #24): decompile from UG2. Fails loudly rather than returning a wrong position.
void FEngGetCenter(FEObject *, float &, float &)
{
    fprintf(stderr, "FEngGetCenter: not implemented yet (ISSUES.md #24)\n");
    abort();
}

// Game-side FEng wrapper hooks used inside FEng (MW's cFEng lives in FEngInterface.cpp, which
// drives the console renderer; the native host is FENativeInterface).
cFEng *cFEng::mInstance = nullptr;
static FEngine *g_activeFEngine = nullptr;

void SetActiveFEngineForWrapper(FEngine *engine)
{
    g_activeFEngine = engine;
}

// Mirrors cFEng::QueuePackageMessage (0x00292858 in UG2, FEngInterface.cpp in MW).
void cFEng::QueuePackageMessage(uint32 msg, const char *pkgName, FEObject *obj)
{
    if (!g_activeFEngine)
        return;
    FEObject *target = obj ? obj : reinterpret_cast<FEObject *>(0xFFFFFFFCu);
    if (pkgName) {
        if (FEPackage *pkg = g_activeFEngine->GetPackageList()->FindPackage(pkgName))
            g_activeFEngine->QueueMessage(msg, nullptr, pkg, target, 0xFF);
    } else {
        for (FEPackage *pkg = g_activeFEngine->GetPackageList()->GetFirstPackage(); pkg; pkg = pkg->GetNext())
            g_activeFEngine->QueueMessage(msg, nullptr, pkg, target, 0);
    }
}
