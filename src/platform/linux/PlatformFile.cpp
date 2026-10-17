#include "platform/linux/PlatformFile.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <vector>

namespace platform {

static std::string g_root = ".";

void SetDataRoot(const std::string &root) { g_root = root; }
const std::string &DataRoot() { return g_root; }

// Resolve "NFSUNDER\ZDIR.BIN" against the data root, matching each component
// case-insensitively because the disc is ISO9660 (upper case) and Linux is not.
static bool Resolve(const char *gamePath, std::string &out)
{
    std::string path = g_root;
    std::string part;
    auto step = [&](const std::string &name) -> bool {
        std::string direct = path + "/" + name;
        struct stat st;
        if (stat(direct.c_str(), &st) == 0) {
            path = direct;
            return true;
        }
        DIR *d = opendir(path.c_str());
        if (!d)
            return false;
        bool found = false;
        while (dirent *e = readdir(d)) {
            if (strcasecmp(e->d_name, name.c_str()) == 0) {
                path += "/";
                path += e->d_name;
                found = true;
                break;
            }
        }
        closedir(d);
        return found;
    };
    for (const char *p = gamePath;; ++p) {
        if (*p == '\\' || *p == '/' || *p == '\0') {
            if (!part.empty() && !step(part))
                return false;
            part.clear();
            if (*p == '\0')
                break;
        } else if (*p != ';') {
            part += *p;
        } else {
            // ISO9660 version suffix ";1"
            while (p[1] && p[1] != '\\' && p[1] != '/')
                ++p;
        }
    }
    out = path;
    return true;
}

bool FileExists(const char *gamePath)
{
    std::string p;
    struct stat st;
    return Resolve(gamePath, p) && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

FileHandle FileOpen(const char *gamePath)
{
    std::string p;
    if (!Resolve(gamePath, p))
        return -1;
    return open(p.c_str(), O_RDONLY | O_CLOEXEC);
}

int64_t FileSize(FileHandle h)
{
    struct stat st;
    return fstat(h, &st) == 0 ? st.st_size : -1;
}

int64_t FileRead(FileHandle h, int64_t offset, void *dst, int64_t size)
{
    int64_t done = 0;
    while (done < size) {
        ssize_t n = pread(h, static_cast<char *>(dst) + done, size - done, offset + done);
        if (n <= 0)
            break;
        done += n;
    }
    return done;
}

void FileClose(FileHandle h)
{
    if (h >= 0)
        close(h);
}

} // namespace platform
