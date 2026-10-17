#include "game/indep/File/ZipFileSystem.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Speed/Indep/bWare/Inc/Strings.hpp"

// 0x002eace8: upper-case ASCII letters, turn '/' into '\', then bStringHash.
uint32_t ZipPathHash(const char *path)
{
    char buf[160];
    size_t n = 0;
    for (; path[n] && n < sizeof(buf) - 1; ++n) {
        unsigned char c = static_cast<unsigned char>(path[n]);
        if (unsigned(c - 'a') < 26u)
            c &= 0x5f;
        buf[n] = c == '/' ? '\\' : static_cast<char>(c);
    }
    buf[n] = '\0';
    return bStringHash(buf);
}

// 0x002eb3e0
bool ZipFileSystem::Init(const char *zdirPath, const char *archivePrefix)
{
    if (!platform::FileExists(zdirPath))
        return false;
    platform::FileHandle dir = platform::FileOpen(zdirPath);
    int64_t size = platform::FileSize(dir);
    m_entries.resize(size / sizeof(ZDirEntry));
    platform::FileRead(dir, 0, m_entries.data(), m_entries.size() * sizeof(ZDirEntry));
    platform::FileClose(dir);

    for (int i = 0; i < kMaxArchives; ++i) {
        char name[0x40];
        snprintf(name, sizeof(name), "%s%d.BIN", archivePrefix, i); // format string at 0x0047c648
        m_archives[i] = platform::FileExists(name) ? platform::FileOpen(name) : -1;
    }
    return true;
}

// 0x002eb568: binary search for the first entry with this hash, then pick the copy whose
// globalSector is closest to the current head position (files are duplicated on the disc
// to cut seek time).
const ZDirEntry *ZipFileSystem::Find(const char *path) const
{
    if (m_entries.empty())
        return nullptr;
    const uint32_t hash = ZipPathHash(path);
    int lo = 0, hi = static_cast<int>(m_entries.size()) - 1, hit = -1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t h = m_entries[mid].nameHash;
        if (h == hash && (mid == 0 || m_entries[mid - 1].nameHash != hash)) {
            hit = mid;
            break;
        }
        if (h < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (hit < 0)
        return nullptr;

    int best = hit;
    int64_t bestDist = INT32_MAX;
    for (size_t i = hit; i < m_entries.size() && m_entries[i].nameHash == hash; ++i) {
        int64_t d = std::llabs(int64_t(m_entries[i].globalSector) - m_headSector);
        if (d < bestDist) {
            bestDist = d;
            best = static_cast<int>(i);
        }
    }
    return &m_entries[best];
}

// 0x002eb138: strip the "discu:" device prefix and leading slashes, look the file up and
// fill in the open record. The original allocates the record from a pool (0x00155e90).
bool ZipFileSystem::Open(const char *path, ZipFile &out) const
{
    if (strncmp(path, "discu:", 6) == 0) // string at 0x00568340
        path += 6;
    while (*path == '\\' || *path == '/')
        ++path;
    const ZDirEntry *e = Find(path);
    if (!e || e->archive >= kMaxArchives || m_archives[e->archive] == -1)
        return false;
    out.entry = e;
    out.archiveHandle = m_archives[e->archive];
    out.offset = int64_t(e->sector) * kSectorSize;
    out.size = e->size;
    return true;
}

// 0x002dbd40
ZipFileSystem *MountDiscArchive()
{
    static const char kZDir[] = "NFSUNDER\\ZDIR.BIN";      // 0x00479618
    static const char kArchives[] = "NFSUNDER\\ZZDATA";    // 0x00479630
    if (!platform::FileExists(kZDir))
        return nullptr;
    auto *fs = new ZipFileSystem();
    if (!fs->Init(kZDir, kArchives)) {
        delete fs;
        return nullptr;
    }
    return fs;
}
