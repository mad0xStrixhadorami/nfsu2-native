#pragma once
#include <string>
#include <vector>

class FEPackage;

// UI_PostRaceResults.fng ("Event Results"; screen factory 0x00512d38: create 0x00229ca8, ctor
// 0x00229ce0, fill 0x00229e50 -> rows 0x0022a080 / 0x0022a9a8). Rows are the objects
// RANKING_DATA_n, PLAYER_DATA_n, CAR_DATA_n and TIME_DATA_n (n from 1, six in the package); the
// screen lists every racer in finishing order with the finish time as m:ss.cc.
struct PostRaceRow {
    std::string name, car;
    float time = -1; // seconds; < 0 = did not finish
};
void FillPostRaceResults(FEPackage *pkg, const std::vector<PostRaceRow> &rows);
