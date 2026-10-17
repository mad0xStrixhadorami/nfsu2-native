// FEng pieces the nfsmw decompilation does not provide yet.
#include <cstdio>
#include <cstdlib>

#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEngine.h"

// Global object GUID counter (declared in FEngine.h).
u32 FEngine::SysGUID = 0;

// TODO(ISSUES #24): decompile UG2's FEngine destructor. Only runs at shutdown.
FEngine::~FEngine() {}

// TODO(ISSUES #24): decompile from UG2. Fails loudly rather than returning a wrong position.
void FEngGetCenter(FEObject *, float &, float &)
{
    fprintf(stderr, "FEngGetCenter: not implemented yet (ISSUES.md #24)\n");
    abort();
}
