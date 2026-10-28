#include "game/indep/Mem.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/indep/Perf.h"

namespace {

struct Item {
    Mem::Kind kind = Mem::kTexture;
    std::string name;
    size_t bytes = 0, used = 0;
    int priority = Mem::kNormal;
    bool gpu = true;
    double lastUse = -1.0;
    bool touched = false; // Touch() was called at least once (static data is never touched)
};

std::unordered_map<uint32_t, Item> g_items;
double g_time = 0.0;
bool g_overlay = false;

const char *kKindNames[Mem::kKinds] = {"textures", "vertex arenas", "geometry", "cars", "sound", "UI"};

std::vector<const Item *> Sorted(int kind)
{
    std::vector<const Item *> v;
    v.reserve(g_items.size());
    for (const auto &kv : g_items)
        if (kind < 0 || int(kv.second.kind) == kind)
            v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const Item *a, const Item *b) { return a->bytes > b->bytes; });
    return v;
}

void Mb(char *dst, size_t n, size_t bytes) // 1 decimal, MB (or kB below 1 MB)
{
    if (bytes >= 1048576)
        snprintf(dst, n, "%.1f MB", double(bytes) / 1048576.0);
    else
        snprintf(dst, n, "%.0f kB", double(bytes) / 1024.0);
}

} // namespace

namespace Mem {

void SetTime(double seconds) { g_time = seconds; }

void Put(uint32_t id, Kind kind, const char *name, size_t bytes, int priority, bool gpu, size_t used)
{
    Item &it = g_items[id];
    it.kind = kind;
    if (name && *name)
        it.name = name;
    it.bytes = bytes, it.used = used, it.priority = priority, it.gpu = gpu;
}

void Touch(uint32_t id)
{
    auto it = g_items.find(id);
    if (it != g_items.end()) {
        it->second.lastUse = g_time;
        it->second.touched = true;
    }
}

void Drop(uint32_t id) { g_items.erase(id); }

void DropKind(Kind kind)
{
    for (auto it = g_items.begin(); it != g_items.end();)
        it = it->second.kind == kind ? g_items.erase(it) : std::next(it);
}

size_t Bytes(Kind kind)
{
    size_t sum = 0;
    for (const auto &kv : g_items)
        if (kv.second.kind == kind)
            sum += kv.second.bytes;
    return sum;
}

size_t Bytes()
{
    size_t sum = 0;
    for (const auto &kv : g_items)
        sum += kv.second.bytes;
    return sum;
}

int Count(Kind kind)
{
    int n = 0;
    for (const auto &kv : g_items)
        n += kv.second.kind == kind;
    return n;
}

const char *KindName(Kind kind) { return kind >= 0 && kind < kKinds ? kKindNames[kind] : "?"; }

int Top(int kind, Row *out, int max)
{
    const std::vector<const Item *> v = Sorted(kind);
    const int n = std::min<int>(int(v.size()), max);
    for (int i = 0; i < n; ++i) {
        out[i].name = v[i]->name.empty() ? "(unnamed)" : v[i]->name.c_str();
        out[i].bytes = v[i]->bytes, out[i].used = v[i]->used;
        out[i].priority = v[i]->priority, out[i].gpu = v[i]->gpu, out[i].touched = v[i]->touched;
        out[i].age = v[i]->lastUse < 0.0 ? -1.0 : g_time - v[i]->lastUse;
    }
    return n;
}

void Print(FILE *f)
{
    char b[32], u[32], total[32];
    size_t vram = 0, ram = 0;
    for (const auto &kv : g_items)
        (kv.second.gpu ? vram : ram) += kv.second.bytes;
    Mb(total, sizeof total, Bytes());
    Mb(b, sizeof b, vram);
    Mb(u, sizeof u, ram);
    fprintf(f, "[Mem] resident at %.1f s: %s in %zu items (VRAM %s, RAM %s) | process RSS %.0f MB\n", g_time,
            total, g_items.size(), b, u, double(Perf::RssBytes()) / 1048576.0);
    for (int k = 0; k < kKinds; ++k) {
        Mb(b, sizeof b, Bytes(Kind(k)));
        fprintf(f, "[Mem]   %-15s %6d items  %9s\n", KindName(Kind(k)), Count(Kind(k)), b);
    }
    Row rows[24];
    const int n = Top(-1, rows, 24);
    fprintf(f, "[Mem] biggest %d:\n", n);
    for (int i = 0; i < n; ++i) {
        Mb(b, sizeof b, rows[i].bytes);
        char extra[32] = "", age[32] = "static";
        if (rows[i].used)
            snprintf(extra, sizeof extra, " (%.0f%% used)", 100.0 * double(rows[i].used) / std::max<size_t>(1, rows[i].bytes));
        if (!rows[i].touched)
            snprintf(age, sizeof age, "not used yet");
        else if (rows[i].age >= 0.0)
            snprintf(age, sizeof age, "used %5.1f s ago", rows[i].age);
        fprintf(f, "[Mem]   %9s  %s %-28.28s pri %d  %s%s\n", b, rows[i].gpu ? "VRAM" : "RAM ", rows[i].name,
                rows[i].priority, age, extra);
    }
    // The eviction candidates the budgeted manager will look at first: low priority and unused for a while.
    fprintf(f, "[Mem] eviction candidates (priority <= %d, unused > 5 s): ", kNormal);
    size_t cand = 0;
    int count = 0;
    for (const auto &kv : g_items) {
        const Item &it = kv.second;
        if (it.priority <= kNormal && it.touched && g_time - it.lastUse > 5.0) {
            cand += it.bytes;
            ++count;
        }
    }
    Mb(b, sizeof b, cand);
    fprintf(f, "%d items, %s\n", count, b);
}

bool OverlayOn() { return g_overlay; }
void ToggleOverlay() { g_overlay = !g_overlay; }

int OverlayLines(char lines[][96], int maxLines)
{
    if (maxLines < 4)
        return 0;
    int k = 0;
    char b[32];
    Mb(b, sizeof b, Bytes());
    snprintf(lines[k++], 96, "Residency  %s in %zu items   RSS %.0f MB", b, g_items.size(), double(Perf::RssBytes()) / 1048576.0);
    for (int kind = 0; kind < kKinds && k < maxLines; ++kind) {
        Mb(b, sizeof b, Bytes(Kind(kind)));
        snprintf(lines[k++], 96, "%-14s %5d  %8s", KindName(Kind(kind)), Count(Kind(kind)), b);
    }
    Row rows[16];
    const int n = Top(-1, rows, std::min(16, maxLines - k));
    for (int i = 0; i < n; ++i) {
        Mb(b, sizeof b, rows[i].bytes);
        char age[16] = "  -  ";
        if (rows[i].touched && rows[i].age >= 0.0)
            snprintf(age, sizeof age, "%4.1fs", rows[i].age);
        snprintf(lines[k++], 96, "%8s %-18.18s p%d %s", b, rows[i].name, rows[i].priority, age);
    }
    return k;
}

} // namespace Mem
