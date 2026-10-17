#include "Speed/Indep/Src/FEng/FEWideString.h"
#include "Speed/Indep/Src/FEng/FEngStandard.h"

u32 GetStringLength(const i16 *pString) {
    if (pString == nullptr) {
        return 0;
    }

    u32 i = 0;
    while (pString[i] != 0) {
        i++;
    }

    return i;
}

FEWideString::FEWideString() {
    mpsString = nullptr;
    mulBufferLength = 0;
    mulBufferLength = Length();
}

FEWideString::FEWideString(const FEWideString &string) {
    mpsString = nullptr;
    mulBufferLength = 0;
    *this = string;
    mulBufferLength = Length();
}

FEWideString::~FEWideString() {
    if (mpsString != nullptr) {
        delete[] mpsString;
    }
}

FEWideString &FEWideString::operator=(const FEWideString &string) {
    if (string.mpsString == nullptr) {
        return *this;
    }

    u32 ulStrLen = GetStringLength(string.mpsString);
    mpsString = AllocateString(ulStrLen + 1);
    CopyString(mpsString, string.mpsString);

    return *this;
}

FEWideString &FEWideString::operator=(const i16 *psString) {
    if (psString == nullptr) {
        return *this;
    }

    u32 ulStrLen = GetStringLength(psString);
    mpsString = AllocateString(ulStrLen + 1);
    CopyString(mpsString, psString);

    return *this;
}

// nfsu2 patch: ASCII assignment (declared upstream, not implemented). Widens each byte.
FEWideString &FEWideString::operator=(const char *pcString) {
    if (pcString == nullptr) {
        return *this;
    }

    u32 ulStrLen = 0;
    while (pcString[ulStrLen] != '\0') {
        ulStrLen++;
    }
    mpsString = AllocateString(ulStrLen + 1);
    for (u32 i = 0; i <= ulStrLen; i++) {
        mpsString[i] = static_cast<unsigned char>(pcString[i]);
    }

    return *this;
}

u32 FEWideString::Length() const {
    return GetStringLength(mpsString);
}

void FEWideString::SetLength(const u32 newLength) {
    if (newLength > Length()) {
        mulBufferLength = newLength;
        i16 *psNewString = FNEW i16[newLength + 1];
        CopyString(psNewString, mpsString);
        if (mpsString != nullptr) {
            delete[] mpsString;
        }
        mpsString = psNewString;
    }
}

i16 *FEWideString::AllocateString(const u32 newLength) {
    if (newLength > mulBufferLength) {
        SetLength(newLength);
    }

    return mpsString;
}
