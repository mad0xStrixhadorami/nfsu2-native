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

Built as **native 32-bit x86** (like the original PC release): EA's data and engine code assume
4-byte pointers. Needs `g++-multilib` and `libsdl2-dev:i386`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake
cmake --build build
./build/bware_test                                  # engine core self-test
NFSU2_DATA=~/re/nfsu2_game ./build/nfsu2 --test     # disc file system self-test
NFSU2_DATA=~/re/nfsu2_game ./build/nfsu2            # run
```

Engine code reused from the NFS Most Wanted decompilation lives in `third_party/nfsmw` (CC0);
local changes are listed in `third_party/nfsmw/PATCHES.md`.
