// Decompress every HUFF block in a file and check the output sizes; optionally dump one block.
// Usage: huff_test FILE [INDEX OUT]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/bWare/bCompression.h"

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    std::vector<uint8_t> d;
    for (int c; (c = fgetc(f)) != EOF;)
        d.push_back(uint8_t(c));
    fclose(f);
    int n = 0, ok = 0;
    for (size_t i = 0; i + 16 <= d.size(); i += 4) {
        if (memcmp(&d[i], "HUFF", 4) != 0 || d[i + 4] != 1)
            continue;
        uint32_t size = bGetDecompressedSize(&d[i]);
        std::vector<uint8_t> out(size + 64, 0xCD);
        uint32_t got = bDecompress(&d[i], out.data());
        bool good = got == size && out[size] == 0xCD; // no overrun past the declared size
        printf("HUFF #%d at %#zx: %u bytes %s\n", n, i, size, good ? "ok" : "BAD");
        if (argc > 3 && atoi(argv[2]) == n) {
            FILE *o = fopen(argv[3], "wb");
            fwrite(out.data(), 1, size, o);
            fclose(o);
        }
        ok += good;
        ++n;
    }
    printf("huff_test: %d/%d ok\n", ok, n);
    return ok == n && n ? 0 : 1;
}
