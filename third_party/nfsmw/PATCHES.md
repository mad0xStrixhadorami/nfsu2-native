# Local changes to vendored nfsmw code

Every edit to files under `third_party/nfsmw` is listed here and marked `// nfsu2 patch` in the source.

| File | Change | Why |
|------|--------|-----|
| `src/Speed/Indep/bWare/Src/bPrintf.cpp` | `bSPrintf`, `bSNPrintf` return the length from `bV(S/SN)Printf` | The matching decomp omits the return. Under GCC that is UB and crashed |

Functions upstream left as empty stubs (`{}` in a non-void function) are not patched. The `bware`
library is built with `-fsanitize=return`, so calling one aborts with its file and line.
Implement it from the UG2 binary when that happens, in `src/`, and log it in `ISSUES.md`.
| `src/Speed/Indep/bWare/Src/bSlotPool.cpp` | `SlotPool::Malloc` zeroes `SlotSize` bytes with `bMemSet` | Original zeroes `SlotSize/4` pointer-sized entries, which overran into neighbouring free slots on 64-bit and corrupted the free list |
| `src/Speed/Indep/bWare/Src/bSlotPool.cpp` | `ExpandSlotPool` finds the last slot by byte offset | Original index assumed 4-byte pointers |
| `src/Speed/Indep/Libs/Support/Utility/UStandard.h` | `UTL::Std::map` allocator is for `pair<const Key, T>` | GCC 16's libstdc++ static-asserts on an allocator for `T` (old compilers rebound it silently); 229 files failed |
| `src/Speed/Indep/Src/FEng/FETypes.h` | `FEngGet*` byte swaps only on `EA_PLATFORM_GAMECUBE` | FEng data is little-endian; the decomp targets big-endian GameCube and always swapped, so x86 couldn't read any .fng |
| `src/Speed/Indep/Src/FEng/FEObjectSorter.h` | `SortObjects` is a stable ascending sort by depth key | Upstream marks it UNSOLVED: it counts into the wrong array and assumes big-endian keys, producing null objects at render time |
| `src/Speed/Indep/Src/FEng/FEKeyInterpLinear.cpp` | Key values read from `GetKeyData()->Val` | Upstream UNSOLVED: read values from `GetKeyData()` (which points at `tTime`), shifting every component by one (dot colours showed the timeline in the blue channel) |
| `src/Speed/Indep/Src/FEng/FEObject.cpp` | `SetupMoveToTracks` dereferences `pDataPtr` | Upstream UNSOLVED: cast `u8 *pDataPtr` to `T &`, reading the stack address of the pointer instead of the object's current property |
| `src/Speed/Indep/Src/FEng/FEWideString.cpp` | Add `operator=(const char *)` | Declared upstream but never implemented; needed by FEString::SetString |
