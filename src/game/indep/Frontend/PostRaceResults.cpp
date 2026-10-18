#include "game/indep/Frontend/PostRaceResults.h"

#include <cstdio>

#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/indep/Frontend/FENative.h"

void FillPostRaceResults(FEPackage *pkg, const std::vector<PostRaceRow> &rows)
{
    if (!pkg)
        return;
    constexpr size_t kRows = 6;
    char name[32], text[32];
    for (size_t i = 0; i < kRows; ++i) {
        const bool used = i < rows.size();
        for (const char *prefix : {"RANKING_DATA_%zu", "PLAYER_DATA_%zu", "CAR_DATA_%zu", "TIME_DATA_%zu"}) {
            snprintf(name, sizeof(name), prefix, i + 1);
            FENativeInterface::SetVisible(pkg, bStringHash(name), used);
        }
        if (!used)
            continue;
        const PostRaceRow &r = rows[i];
        snprintf(name, sizeof(name), "RANKING_DATA_%zu", i + 1);
        snprintf(text, sizeof(text), "%zu", i + 1);
        FENativeInterface::SetString(pkg, bStringHash(name), text);
        snprintf(name, sizeof(name), "PLAYER_DATA_%zu", i + 1);
        FENativeInterface::SetString(pkg, bStringHash(name), r.name.c_str());
        snprintf(name, sizeof(name), "CAR_DATA_%zu", i + 1);
        FENativeInterface::SetString(pkg, bStringHash(name), r.car.c_str());
        snprintf(name, sizeof(name), "TIME_DATA_%zu", i + 1);
        if (r.time < 0)
            snprintf(text, sizeof(text), "DNF");
        else
            snprintf(text, sizeof(text), "%d:%05.2f", int(r.time / 60.0f), r.time - int(r.time / 60.0f) * 60.0f);
        FENativeInterface::SetString(pkg, bStringHash(name), text);
    }
}
