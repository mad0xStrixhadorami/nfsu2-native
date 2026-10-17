#pragma once
#include <cstdint>
#include <vector>

#include "platform/linux/PlatformFile.h"

// The hashed disc archive: NFSUNDER\ZDIR.BIN indexes files packed into NFSUNDER\ZZDATA<n>.BIN.
// Original object is 0x464 bytes with vtable 0x0047ced8; it is created by 0x002eb078.
// Class and member names are ours (the PS2 build has no symbols).

struct ZDirEntry {          // 24 bytes on disc
    uint32_t nameHash;      // ZipPathHash(path)
    uint32_t archive;       // n in ZZDATA<n>.BIN
    uint32_t sector;        // 2048-byte sector inside that archive
    uint32_t globalSector;  // sector across all archives back to back (used for seek distance)
    uint32_t size;          // bytes
    uint32_t contentHash;   // not used by the lookup
};
static_assert(sizeof(ZDirEntry) == 24);

struct ZipFile {            // the per-open record filled in by Open (0x002eb138)
    const ZDirEntry *entry;
    platform::FileHandle archiveHandle;
    int64_t offset;         // byte offset inside the archive
    int64_t size;
};

class ZipFileSystem {
public:
    static constexpr int kMaxArchives = 16; // the original loops 0x10 times
    static constexpr int kSectorSize = 2048;

    bool Init(const char *zdirPath, const char *archivePrefix); // 0x002eb3e0
    const ZDirEntry *Find(const char *path) const;               // 0x002eb568
    bool Open(const char *path, ZipFile &out) const;             // 0x002eb138
    void ResetHead() { m_headSector = 0; }                       // 0x002eb120 (clears +0x45c/+0x460)

    size_t EntryCount() const { return m_entries.size(); }
    const std::vector<ZDirEntry> &Entries() const { return m_entries; }

private:
    std::vector<ZDirEntry> m_entries;                     // +0x14 / +0x18
    platform::FileHandle m_archives[kMaxArchives] = {};   // +0x41c
    uint32_t m_headSector = 0;                            // +0x45c, last read position
};

uint32_t ZipPathHash(const char *path); // 0x002eace8

// 0x002dbd40: mount the archive if the disc has one. Returns nullptr when absent.
ZipFileSystem *MountDiscArchive();
