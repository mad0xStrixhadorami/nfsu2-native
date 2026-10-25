#include "game/indep/World/PoliceCar.h"

#include <cstring>

bool PoliceMesh::Load(const std::vector<uint8_t> &file)
{
    const uint8_t *p = file.data(), *end = p + file.size();
    auto need = [&](size_t n) { return size_t(end - p) >= n; };
    if (!need(8) || memcmp(p, "PCR1", 4) != 0)
        return false;
    p += 4;
    uint32_t nt;
    memcpy(&nt, p, 4), p += 4;
    textures.resize(nt);
    for (Tex &t : textures) {
        if (!need(10))
            return false;
        uint16_t w, h;
        memcpy(&t.hash, p, 4), memcpy(&w, p + 4, 2), memcpy(&h, p + 6, 2);
        t.gloss = p[8];
        const uint8_t nameLen = p[9];
        p += 10;
        if (!need(nameLen + size_t(w) * h * 4))
            return false;
        t.name.assign(reinterpret_cast<const char *>(p), nameLen), p += nameLen;
        t.w = w, t.h = h;
        t.rgba.resize(size_t(w) * h);
        if (w && h)
            memcpy(t.rgba.data(), p, size_t(w) * h * 4), p += size_t(w) * h * 4;
    }
    uint32_t n;
    if (!need(4))
        return false;
    memcpy(&n, p, 4), p += 4;
    model.resize(n), triTex.resize(n), wheelOf.resize(n), rims.resize(n), normals.resize(size_t(n) * 9);
    for (uint32_t i = 0; i < n; ++i) {
        if (!need(96 + 8))
            return false;
        SolidMesh::Triangle &t = model[i];
        for (int k = 0; k < 3; ++k) {
            float f[8];
            memcpy(f, p, 32), p += 32;
            t.v[k].x = f[0], t.v[k].y = f[1], t.v[k].z = f[2], t.v[k].u = f[6], t.v[k].v = f[7];
            normals[size_t(i) * 9 + size_t(k) * 3] = f[3], normals[size_t(i) * 9 + size_t(k) * 3 + 1] = f[4], normals[size_t(i) * 9 + size_t(k) * 3 + 2] = f[5];
        }
        t.tex = -1;
        memcpy(&triTex[i], p, 4);
        wheelOf[i] = p[4] == 255 ? int8_t(-1) : int8_t(p[4]);
        rims[i] = p[5];
        p += 8;
    }
    if (!need(12 * 4 + 8 + 24))
        return false;
    memcpy(hubs, p, 48), p += 48;
    memcpy(&radius, p, 4), memcpy(&width, p + 4, 4), p += 8;
    memcpy(bar, p, 24);
    return true;
}
