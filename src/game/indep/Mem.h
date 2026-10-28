#pragma once
// Resident-memory telemetry (ISSUES #331).
//
// Every subsystem that keeps something resident tells this registry so: its size, a priority (how bad is it to
// evict: 4 pinned .. 1 first to go), whether the cost is VRAM or RAM, and the time it was last used.  The registry
// is the input of the budgeted residency manager that will later evict the oldest low-priority items first, and
// its reports show where the process's gigabytes actually are.
//
// The registry is single-threaded like the rest of the game: `SetTime` once a frame, `Put` when something is
// created / changes size, `Touch` whenever it is used, `Drop` when it goes away.  Ids are the caller's keys and
// must be unique across kinds (a GL texture name, a fixed constant for a singleton).
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Mem {
enum Kind : int { kTexture, kArena, kGeometry, kCars, kSound, kUi, kKinds };

inline constexpr int kPinned = 4; // never evict (the arenas, the white / skin textures)
inline constexpr int kHigh = 3;   // the resident region's own data
inline constexpr int kNormal = 2; // cars, UI, sound
inline constexpr int kLow = 1;    // leftovers of a closed room / a previous screen

void SetTime(double seconds); // once a frame; the ages in the reports are relative to it
// Adds or updates one resident item.  `used` is only meaningful for arenas (allocated `bytes`, `used` in use).
void Put(uint32_t id, Kind kind, const char *name, size_t bytes, int priority, bool gpu, size_t used = 0);
void Touch(uint32_t id);
void Drop(uint32_t id);
void DropKind(Kind kind);

size_t Bytes(Kind kind); // resident bytes of that kind (VRAM + RAM as recorded)
size_t Bytes();
int Count(Kind kind);
const char *KindName(Kind kind);

struct Row {
    uint32_t id;      // the registry key: a GL name for textures
    const char *name;
    size_t bytes, used;
    int priority;
    double age;  // seconds since the last Touch (negative: never touched)
    bool gpu, touched;
};
// The largest items of `kind` (or of every kind with kind < 0), biggest first.  Returns how many were written.
int Top(int kind, Row *out, int max);
// The eviction order of a kind (#332): the items that are not pinned and have not been used for `minAge`
// seconds, least recently used first (never-used items age from their creation).  Pinned items are skipped.
int Oldest(Kind kind, double minAge, Row *out, int max);

// The residency budget the manager trims to, and what it has trimmed this session (the reports show both).
void SetBudget(size_t bytes);
size_t Budget();
void NoteTrim(size_t bytes, int items);
size_t TrimmedBytes();
int TrimmedItems();
void Print(FILE *out); // the --mem-report body: totals per kind and the biggest items

// The live overlay (F4, or NFSU2_MEM_OVERLAY=1): totals and the biggest items, one line each.
bool OverlayOn();
void ToggleOverlay();
int OverlayLines(char lines[][96], int maxLines);
} // namespace Mem
