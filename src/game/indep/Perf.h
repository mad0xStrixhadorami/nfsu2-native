#pragma once
// A lightweight built-in profiler (ISSUES #282): per-frame CPU sections, GPU time (GL timer query), draw-call / state-change
// counters, heap allocations per frame, resident memory, and a CPU-bound / GPU-bound / sync-bound verdict.
// F3 toggles the on-screen overlay; NFSU2_PERF=1 prints a summary every 2 s (average, 1 % low, worst frames, spikes).
#include <cstdint>

namespace Perf {
enum Section { kInput, kUpdate, kWorld, kUI, kPresent, kSections };

struct Counters {
    uint32_t drawCalls = 0, multiDraws = 0, triangles = 0, textureBinds = 0, programSwitches = 0, bufferBinds = 0, cellsDrawn = 0;
};
extern Counters g; // per frame; reset by BeginFrame

void Init();                  // after the GL context exists
void BeginFrame();
void Lap(Section s);          // charges the time since the previous Lap / BeginFrame to s
void GpuBegin();              // brackets the 3D world's GL work
void GpuEnd();
void EndFrame();              // after SDL_RenderPresent
bool OverlayOn();
void ToggleOverlay();
// The overlay text lines (drawn by ModernUI): frame, sections, GPU, counters, memory, allocations.
int OverlayLines(char lines[][96], int maxLines);
// Global allocation counters (operator new / delete are instrumented).
uint64_t Allocations();
} // namespace Perf
