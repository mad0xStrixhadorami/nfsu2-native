# nfsu2_decomp

A functional decompilation of **Need for Speed: Underground 2** (PS2 PAL v2.00, `SLES_527.25`),
rewritten as readable C++ that builds and runs natively on Linux.

## Rules

- **No emulation.** No PS2 CPU, register file, hardware registers, IOP, GS or VU emulation. Game logic is
  rewritten as normal C++, and PS2 hardware use is replaced with Linux APIs: SDL2 for the window, input and
  audio, OpenGL for graphics, and POSIX for files and threads.
- **Functional, not matching.** Code must do what the original does. It doesn't have to compile to the same
  bytes.
- **Every function is traceable.** Each rewritten function carries a `// 0xADDRESS` comment pointing to the
  original, and `docs/function_map.tsv` tracks its status.
- **Bring your own disc.** The repository holds no game code or assets. At run time the game reads
  `NFSUNDER/ZDIR.BIN` and `ZZDATA*.BIN` from your own copy (`NFSU2_DATA=/path/to/disc`).
- `reference/` (Ghidra pseudocode) is derived from the game binary. Keep it local and never commit it.

## Layout

| Path | What |
|------|------|
| `src/game/bWare/` | EA Black Box engine core (`../indep/bWare/...` in the original) |
| `src/game/indep/` | Platform-independent game code (`../indep/src/...`) |
| `src/platform/linux/` | Linux replacements for PS2 hardware and SDK calls |
| `tools/` | Archive tools (`zdir_names.py`, `zz_extract.py`) and the progress window |
| `docs/function_map.tsv` | Address → name → status (`todo`, `wip`, `done`, `replaced`) |
| `ISSUES.md` | Issue log |

## Build

```sh
cmake -S . -B build -G Ninja && cmake --build build
NFSU2_DATA=~/re/nfsu2_game ./build/nfsu2
```
