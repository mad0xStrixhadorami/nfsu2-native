#ifndef FEOBJECTSORTER_H_
#define FEOBJECTSORTER_H_

#include <types.h>
#include <algorithm>
#include "FEObject.h"
#include "FEngStandard.h"

// total size: 0x8
// Decl: 24
struct SFERadixKey {
    FEObject *pobObject; // offset 0x0, size 0x4, Decl: 25
    u32 ulKey;           // offset 0x4, size 0x4, Decl: 26
};

// total size: 0x4004
// Decl: 34
template <int N> class FEObjectSorter {
  private:
    u32 mulNumObjects;              // offset 0x0, size 0x4, Decl: 36
    SFERadixKey mastFinalList[N];   // offset 0x4, size 0x2000, Decl: 37
    SFERadixKey mastScratchList[N]; // offset 0x2004, size 0x2000, Decl: 38

  public:
    FEObjectSorter() { // Decl: 41
        Zero();
    }

    void Zero() { // Decl: 46
        mulNumObjects = 0;
    }

    void AddObject(FEObject *pobObject, float fZValue) { // Decl: 51
        mastFinalList[mulNumObjects].pobObject = pobObject;
        mastFinalList[mulNumObjects].ulKey = *reinterpret_cast<u32 *>(&fZValue);
        mulNumObjects++;
    }

    u32 GetNumObjects() { // Decl: 59
        return mulNumObjects;
    }

    SFERadixKey *GetListPtr() { // Decl: 70
        return mastFinalList;
    }

    void SortObjects();
};

// nfsu2 patch: the decompiled radix sort is unsolved upstream (it counts into the wrong array and
// reads key bytes in big-endian order). Same result intended: stable ascending sort by the float
// depth key (non-negative floats order like their bit patterns).
template <int N> void FEObjectSorter<N>::SortObjects() { // Decl: 81
    std::stable_sort(mastFinalList, mastFinalList + mulNumObjects,
                     [](const SFERadixKey &a, const SFERadixKey &b) { return a.ulKey < b.ulKey; });
}

#endif
