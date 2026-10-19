#include "game/indep/Career/Memcard.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace Career {

namespace fs = std::filesystem;

namespace {

// "<NFSU2_DATA>/mc0" unless NFSU2_MEMCARD names a directory outright.
std::string DefaultSlotDir()
{
    if (const char *dir = getenv("NFSU2_MEMCARD"))
        if (*dir)
            return dir;
    if (const char *root = getenv("NFSU2_DATA"))
        if (*root)
            return std::string(root) + "/mc0";
    return "mc0";
}

bool HasSavSuffix(const std::string &name)
{
    if (name.size() < 5)
        return false;
    std::string tail = name.substr(name.size() - 4);
    for (char &c : tail)
        c = char(toupper((unsigned char)c));
    return tail == ".SAV";
}

} // namespace

MemoryCard &MemoryCard::Get()
{
    static MemoryCard card;
    if (card.m_dir.empty())
        card.m_dir = DefaultSlotDir();
    return card;
}

bool MemoryCard::Present()
{
    std::error_code ec;
    if (fs::is_directory(m_dir, ec))
        return true;
    // A PS2 with a card inserted always has one; create the directory on first use.
    fs::create_directories(m_dir, ec);
    return fs::is_directory(m_dir, ec);
}

std::string MemoryCard::SanitizeName(const std::string &raw)
{
    std::string out;
    for (char c : raw) {
        unsigned char u = (unsigned char)toupper((unsigned char)c);
        if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u == '-')
            out += char(u);
        if (out.size() >= 16)
            break;
    }
    if (out.empty())
        out = "PLAYER";
    return out;
}

std::string MemoryCard::PathOf(const std::string &name) const
{
    return m_dir + "/" + SanitizeName(name) + ".SAV";
}

std::vector<CardFile> MemoryCard::List() const
{
    std::vector<CardFile> files;
    std::error_code ec;
    if (!fs::is_directory(m_dir, ec))
        return files;
    for (const fs::directory_entry &e : fs::directory_iterator(m_dir, ec)) {
        if (!e.is_regular_file(ec))
            continue;
        std::string name = e.path().filename().string();
        if (!HasSavSuffix(name))
            continue;
        CardFile f;
        f.name = name.substr(0, name.size() - 4);
        f.size = uint32_t(e.file_size(ec));
        f.modified = uint32_t(e.last_write_time(ec).time_since_epoch().count());
        files.push_back(f);
    }
    std::sort(files.begin(), files.end(), [](const CardFile &a, const CardFile &b) { return a.name < b.name; });
    return files;
}

bool MemoryCard::Exists(const std::string &name) const
{
    std::error_code ec;
    return fs::is_regular_file(PathOf(name), ec);
}

bool MemoryCard::Read(const std::string &name, std::vector<uint8_t> &out) const
{
    std::ifstream in(PathOf(name), std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.assign(size_t(size), 0);
    if (size > 0)
        in.read(reinterpret_cast<char *>(out.data()), size);
    return bool(in) || in.eof();
}

bool MemoryCard::Write(const std::string &name, const std::vector<uint8_t> &data)
{
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    // Capacity check (stand-in for the console's free-block count): refuse a new file when full.
    if (!Exists(name) && Count() >= MaxFiles()) {
        fprintf(stderr, "[Memcard] card full (%d files)\n", Count());
        return false;
    }
    std::ofstream out(PathOf(name), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (!data.empty())
        out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
    return bool(out);
}

bool MemoryCard::Remove(const std::string &name)
{
    std::error_code ec;
    return fs::remove(PathOf(name), ec) && !ec;
}

} // namespace Career
