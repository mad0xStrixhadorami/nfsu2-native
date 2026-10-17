# nfsu2_decomp issue log

Earlier history (recompilation experiment, issues 1–14) is in `~/re/nfsu2_port/ISSUES.md`. That build is reference only.

Status key: OPEN · FIXED · WORKAROUND · NOTE

| # | Date | Area | Status | Issue | Resolution / next step |
|---|------|------|--------|-------|------------------------|
| 1 | 2026-10-17 | Design | NOTE | User chose full functional decompilation with no emulation (PS2Recomp scaffold dropped) | Rewrite game C++ by hand; replace PS2 SDK/hardware with SDL2/OpenGL/POSIX |
| 2 | 2026-10-17 | Versions | OPEN | Only the PS2 PAL disc is confirmed. The GameCube build reportedly ships with symbols, which would name functions for free | Ask the user again if naming becomes the bottleneck |
| 3 | 2026-10-17 | Reference | NOTE | Ghidra pseudocode for 16,856/16,857 functions dumped to `~/re/nfsu2_pal/reference/all` (1 failed, see index.tsv) | Local only, never commit |
| 4 | 2026-10-17 | File | FIXED | Disc archive system decompiled (0x2dbd40, 0x2eb3e0, 0x2eb568, 0x2eb138, 0x2eace8) with POSIX I/O; self-test passes on real data | — |
| 5 | 2026-10-17 | File | NOTE | `ZipFileSystem::Open` doesn't replicate the original fixed-size open-record pool (0x00155e90); no head-position update on read yet | Add when the streaming reader (0x0040c220 callers) is decompiled |
| 6 | 2026-10-17 | Boot | OPEN | main (0x2dbf00): system init 0x2dbd88, 0x2dbef8, path select 0x2e2fc8, main loop 0x2dc298 not done | Next: decompile 0x2dbd88 |
| 7 | 2026-10-17 | Data | OPEN | `.LZC` files are JDLZ-compressed (EA LZ). The decompressor is needed before any asset loads | Find the JDLZ decoder in the binary and decompile it |
