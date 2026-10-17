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
| 8 | 2026-10-17 | Data | FIXED | JDLZ decompressor (0x2e8590) decompiled; output verified byte-identical to GLOBALB.BUN (the disc's uncompressed copy of GLOBALB.LZC, 1,616,384 bytes) | — |
| 9 | 2026-10-17 | Data | OPEN | HUFF (0x2e9640), COMP (0x2e96f8), RAWW (0x2e9698) decoders not done; FRONTB has HUFF-compressed sub-chunks | Decompile when first needed |
| 10 | 2026-10-17 | Texture | FIXED | 8-bit PS2 texture packs (TPK 0xb3300000) decode: 124-byte info records, GS-swizzled pixels, swizzled 256-colour CLUT. The EA GAMES logo renders correctly | Layout comes from the data; the game's TPK loader isn't located yet |
| 11 | 2026-10-17 | Texture | OPEN | 4-bit textures (e.g. U2_MENU_LOGO) use another GS layout. PSMCT32→PSMT4 with PCSX2's tables and upload widths 64/128/256 all look wrong. Info flags differ: 4bpp `10 00 01 00`, 8bpp `00 01 01 00` | Find how the game uploads 4bpp (GS register chunk 0x33310005, upload code), then convert accordingly |
| 12 | 2026-10-17 | Render | NOTE | Window shows a boot splash using SDL's 2D renderer. That's a stand-in, not the game's own boot flow | Replace with the decompiled front end (FEng) and an OpenGL renderer |
