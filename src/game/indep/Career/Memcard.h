#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Career {

// One file on the card (a "save").
struct CardFile {
    std::string name; // save name (the career profile's name)
    uint32_t size = 0;
    uint32_t modified = 0;
};

// The native replacement for the PS2 memory card (mc0:/mc1:). UG2 reaches it through EA's memcard
// library (`FEPkgMemcard` create 0x00225f50, `FEPkgMemcardList` 0x00228348, `MemcardThread`,
// `MMSaveLoad`, `MemcardAllocator`); the port talks to the filesystem instead, exactly as it
// replaced file I/O with POSIX (ZipFileSystem, #4). A card is a directory, a save is a file in it.
//
// The PS2 save *contents* are EA's serialized object graph and are not decoded (no sample save
// exists on this disc); the port writes its own format - see Profile.cpp. The directory layout,
// however, mirrors the console: slot 1 = <NFSU2_DATA>/mc0, slot 2 = <NFSU2_DATA>/mc1.
class MemoryCard {
public:
    static MemoryCard &Get();

    // Slot directory (default "<NFSU2_DATA>/mc0", overridable with NFSU2_MEMCARD for tests).
    void SetSlotDir(const std::string &dir) { m_dir = dir; }
    const std::string &SlotDir() const { return m_dir; }

    bool Present();                       // usable card: the directory exists (created on demand)
    std::vector<CardFile> List() const;    // *.SAV files, name-sorted
    int Count() const { return int(List().size()); }
    int MaxFiles() const { return 8; }     // stand-in capacity, in save files (a PS2 card holds ~15)

    bool Exists(const std::string &name) const;
    bool Read(const std::string &name, std::vector<uint8_t> &out) const;
    bool Write(const std::string &name, const std::vector<uint8_t> &data);
    bool Remove(const std::string &name);

    // PS2 save-file names are upper case and short; keep the same shape.
    static std::string SanitizeName(const std::string &raw);

private:
    MemoryCard() = default;
    std::string PathOf(const std::string &name) const;

    std::string m_dir;
};

} // namespace Career
