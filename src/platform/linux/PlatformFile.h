#pragma once
#include <cstdint>
#include <string>

// Native replacements for the game's low-level disc file layer (0x0040c1e8..0x0040c308).
// The PS2 version goes through sceCdRead/IOP; here it is plain POSIX I/O on the user's
// extracted disc, rooted at $NFSU2_DATA. Game paths use '\' and are case-insensitive.
namespace platform {

using FileHandle = int; // -1 = invalid

void SetDataRoot(const std::string &root);
const std::string &DataRoot();

bool FileExists(const char *gamePath);                                   // replaces 0x0040c308
FileHandle FileOpen(const char *gamePath);                               // replaces 0x0040c1e8
int64_t FileSize(FileHandle h);                                          // replaces 0x0040c2d0
int64_t FileRead(FileHandle h, int64_t offset, void *dst, int64_t size); // replaces 0x0040c220
void FileClose(FileHandle h);                                            // replaces 0x0040c290

} // namespace platform
