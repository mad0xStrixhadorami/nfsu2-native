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
