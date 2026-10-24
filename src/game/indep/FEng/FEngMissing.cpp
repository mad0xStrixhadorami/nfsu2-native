// FEng pieces the nfsmw decompilation does not provide yet.
#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/Src/FEng/FEngine.h"
#include "Speed/Indep/Src/Frontend/FEngInterfaces/FEngInterface.hpp"

// Global object GUID counter (declared in FEngine.h).
u32 FEngine::SysGUID = 0;

// TODO(ISSUES #24): decompile UG2's FEngine destructor (0x00288240). Only runs at shutdown.
FEngine::~FEngine() {}

// 0x00293b90: the object's centre is its position (ObjData +0x1c/+0x20). UG2 special-cases type 2 (strings) by adding
// half the laid-out text extent; the string metrics are not ported, so strings report their anchor position.
void FEngGetCenter(FEObject *obj, float &x, float &y)
{
    if (!obj || !obj->GetObjData())
        return;
    x = obj->GetObjData()->Pos.x;
    y = obj->GetObjData()->Pos.y;
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
