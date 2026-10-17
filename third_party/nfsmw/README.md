# nfsmw (vendored)

Platform-independent code from the Need for Speed: Most Wanted decompilation,
https://github.com/dbalatoni13/nfsmw (CC0 1.0), at the commit in `UPSTREAM_COMMIT`.

Most Wanted (2005) uses the same EA Black Box engine as Underground 2 (2004). We reuse its
decompiled engine code, built for Linux with `compat/` (`-DEA_PLATFORM_WIN32`, our `types.h`,
`nfsu2_prelude.h`). Each piece used by nfsu2 is checked against the UG2 PS2 binary; where UG2
differs, the UG2 version lives in `src/` and the difference is logged in `ISSUES.md`.
Files here are kept unmodified so they can be updated from upstream.
