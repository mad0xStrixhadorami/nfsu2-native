#include "game/indep/Perf.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
std::atomic<uint64_t> g_allocs{0}, g_allocBytes{0}, g_frees{0};
}

// ---- instrumented global allocator: counts every C++ allocation (the per-frame count is what must stay near zero) ----
void *operator new(size_t n)
{
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_allocBytes.fetch_add(n, std::memory_order_relaxed);
    if (void *p = malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n) { return operator new(n); }
// operator delete is the engine's own (PlatformSystem.cpp routes arena memory to bFree); only allocations are counted here.

namespace Perf {
Counters g;

namespace {
constexpr int kRing = 600;
struct Frame {
    float total, section[kSections], gpu;
    uint32_t allocs;
};
Frame g_ring[kRing];
int g_head = 0, g_count = 0;
uint64_t g_freq = 1, g_frameStart = 0, g_lap = 0, g_prevEnd = 0;
float g_sec[kSections];
bool g_overlay = false, g_summary = false;
double g_lastReport = 0;
uint64_t g_allocAtStart = 0;
// GPU timer queries (a small ring: results are read a few frames later, never stalling)
GLuint g_q[4];
int g_qHead = 0, g_qInFlight = 0;
bool g_gpuOk = false, g_gpuOpen = false;
float g_gpuMs = 0;
using GenQ = void (*)(GLsizei, GLuint *);
using BeginQ = void (*)(GLenum, GLuint);
using EndQ = void (*)(GLenum);
using GetQ = void (*)(GLuint, GLenum, uint64_t *);
GenQ pGen;
BeginQ pBegin;
EndQ pEnd;
GetQ pGet;
constexpr GLenum kTimeElapsed = 0x88BF, kResultAvailable = 0x8867, kResult = 0x8866;
uint64_t g_lastRss = 0;

uint64_t Rss()
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    unsigned long total = 0, res = 0;
    if (fscanf(f, "%lu %lu", &total, &res) != 2)
        res = 0;
    fclose(f);
    return uint64_t(res) * 4096ull;
}
float Ms(uint64_t ticks) { return float(double(ticks) * 1000.0 / double(g_freq)); }
} // namespace

void Init()
{
    g_freq = SDL_GetPerformanceFrequency();
    g_summary = getenv("NFSU2_PERF") != nullptr;
    pGen = reinterpret_cast<GenQ>(SDL_GL_GetProcAddress("glGenQueries"));
    pBegin = reinterpret_cast<BeginQ>(SDL_GL_GetProcAddress("glBeginQuery"));
    pEnd = reinterpret_cast<EndQ>(SDL_GL_GetProcAddress("glEndQuery"));
    pGet = reinterpret_cast<GetQ>(SDL_GL_GetProcAddress("glGetQueryObjectui64v"));
    if (pGen && pBegin && pEnd && pGet) {
        pGen(4, g_q);
        g_gpuOk = true;
    }
    printf("[Perf] profiler ready (GPU timer %s); F3 overlay%s\n", g_gpuOk ? "yes" : "no", g_summary ? ", summary every 2 s" : "");
}

void BeginFrame()
{
    g = Counters();
    std::memset(g_sec, 0, sizeof(g_sec));
    g_frameStart = g_lap = SDL_GetPerformanceCounter();
    g_allocAtStart = g_allocs.load(std::memory_order_relaxed);
}

void Lap(Section s)
{
    const uint64_t now = SDL_GetPerformanceCounter();
    g_sec[s] += Ms(now - g_lap);
    g_lap = now;
}

void GpuBegin()
{
    if (!g_gpuOk || g_gpuOpen || g_qInFlight >= 3)
        return;
    pBegin(kTimeElapsed, g_q[g_qHead]);
    g_gpuOpen = true;
}

void GpuEnd()
{
    if (!g_gpuOpen)
        return;
    pEnd(kTimeElapsed);
    g_gpuOpen = false;
    g_qHead = (g_qHead + 1) % 4;
    ++g_qInFlight;
}

void EndFrame()
{
    const uint64_t now = SDL_GetPerformanceCounter();
    // read back finished GPU queries (oldest first)
    while (g_gpuOk && g_qInFlight > 0) {
        const int idx = (g_qHead + 4 - g_qInFlight) % 4;
        uint64_t avail = 0;
        pGet(g_q[idx], kResultAvailable, &avail);
        if (!avail)
            break;
        uint64_t ns = 0;
        pGet(g_q[idx], kResult, &ns);
        g_gpuMs = float(double(ns) * 1e-6);
        --g_qInFlight;
    }
    Frame &fr = g_ring[g_head];
    fr.total = g_prevEnd ? Ms(now - g_prevEnd) : Ms(now - g_frameStart);
    g_prevEnd = now;
    std::memcpy(fr.section, g_sec, sizeof(g_sec));
    fr.gpu = g_gpuMs;
    fr.allocs = uint32_t(g_allocs.load(std::memory_order_relaxed) - g_allocAtStart);
    g_head = (g_head + 1) % kRing;
    g_count = std::min(g_count + 1, kRing);

    if (g_summary) {
        const double t = double(now) / double(g_freq);
        if (t - g_lastReport >= 2.0 && g_count >= 30) {
            g_lastReport = t;
            float sorted[kRing];
            double sum = 0, cpuSum = 0, gpuSum = 0, sec[kSections] = {};
            uint64_t allocSum = 0;
            int spikes = 0;
            const int n = std::min(g_count, 120);
            for (int i = 0; i < n; ++i) {
                const Frame &f = g_ring[(g_head - 1 - i + kRing) % kRing];
                sorted[i] = f.total;
                sum += f.total;
                gpuSum += f.gpu;
                for (int k = 0; k < kSections; ++k)
                    sec[k] += f.section[k];
                cpuSum += f.section[kInput] + f.section[kUpdate] + f.section[kWorld] + f.section[kUI];
                allocSum += f.allocs;
                spikes += f.total > 1.6 * (sum / double(i + 1)) && f.total > 20.0f;
            }
            std::sort(sorted, sorted + n);
            const float avg = float(sum / n), p99 = sorted[std::min(n - 1, int(n * 0.99f))], worst = sorted[n - 1];
            const float cpu = float(cpuSum / n), gpu = float(gpuSum / n), wait = float(sec[kPresent] / n);
            const char *verdict = gpu > 0.85f * avg ? "GPU-bound" : cpu > 0.85f * avg ? "CPU-bound" : wait > 0.4f * avg ? "sync/vsync-bound" : "mixed";
            printf("[Perf] %.1f fps  frame avg %.2f ms  99%% %.2f  worst %.2f | cpu %.2f (input %.2f update %.2f world %.2f ui %.2f) gpu %.2f present-wait %.2f -> %s | "
                   "draws %u (multi %u) tris %u texbinds %u progsw %u bufbinds %u cells %u | allocs/frame %.1f | rss %.0f MB | spikes %d\n",
                   1000.0f / avg, avg, p99, worst, cpu, sec[kInput] / n, sec[kUpdate] / n, sec[kWorld] / n, sec[kUI] / n, gpu, wait, verdict, g.drawCalls,
                   g.multiDraws, g.triangles, g.textureBinds, g.programSwitches, g.bufferBinds, g.cellsDrawn, double(allocSum) / n, double(Rss()) / 1048576.0, spikes);
            fflush(stdout);
        }
    }
}

bool OverlayOn() { return g_overlay; }
void ToggleOverlay() { g_overlay = !g_overlay; }
uint64_t Allocations() { return g_allocs.load(std::memory_order_relaxed); }

int OverlayLines(char lines[][96], int maxLines)
{
    if (g_count == 0 || maxLines < 8)
        return 0;
    const int n = std::min(g_count, 60);
    double sum = 0, cpu = 0, gpu = 0, wait = 0, allocs = 0;
    float worst = 0;
    for (int i = 0; i < n; ++i) {
        const Frame &f = g_ring[(g_head - 1 - i + kRing) % kRing];
        sum += f.total;
        gpu += f.gpu;
        cpu += f.section[kInput] + f.section[kUpdate] + f.section[kWorld] + f.section[kUI];
        wait += f.section[kPresent];
        allocs += f.allocs;
        worst = std::max(worst, f.total);
    }
    const float avg = float(sum / n), c = float(cpu / n), gp = float(gpu / n), w = float(wait / n);
    const char *verdict = gp > 0.85f * avg ? "GPU-bound" : c > 0.85f * avg ? "CPU-bound" : w > 0.4f * avg ? "sync/vsync-bound" : "mixed";
    int k = 0;
    snprintf(lines[k++], 96, "%.0f FPS   %.2f ms avg   %.1f ms worst", 1000.0f / avg, avg, worst);
    snprintf(lines[k++], 96, "CPU %.2f ms   GPU %.2f ms   wait %.2f ms", c, gp, w);
    snprintf(lines[k++], 96, "-> %s", verdict);
    snprintf(lines[k++], 96, "draws %u (multi %u)  tris %uk  cells %u", g.drawCalls, g.multiDraws, g.triangles / 1000, g.cellsDrawn);
    snprintf(lines[k++], 96, "tex binds %u  prog %u  buf %u", g.textureBinds, g.programSwitches, g.bufferBinds);
    snprintf(lines[k++], 96, "heap allocs/frame %.1f   RSS %.0f MB", allocs / n, double(Rss()) / 1048576.0);
    return k;
}
} // namespace Perf
