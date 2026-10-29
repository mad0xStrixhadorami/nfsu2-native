// CARS\<car>\PARTS_ANIMATIONS.BIN decoder (ISSUES #358): a synthetic bank must decode to its name, its key count and its end angle.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/indep/World/CarPartAnim.h"

static int Fail(const char *m)
{
    fprintf(stderr, "partanim_test: %s\n", m);
    return 1;
}

int main()
{
    // one chunk: 8 filler bytes, the name, padding, then 31 keys (sin(a/2), 0, cos(a/2), 0) opening to 60 degrees
    std::vector<uint8_t> payload(8, 0x11);
    const char *name = "350Z_ZAM_HOOD_FRONT_q";
    payload.insert(payload.end(), name, name + strlen(name) + 1);
    while (payload.size() % 16 != 0)
        payload.push_back(0);
    const int n = 31;
    for (int i = 0; i < n; ++i) {
        const float a = (60.0f * 3.14159265f / 180.0f) * float(i) / float(n - 1);
        const float q[4] = {std::sin(a * 0.5f), 0.0f, std::cos(a * 0.5f), 0.0f};
        const uint8_t *b = reinterpret_cast<const uint8_t *>(q);
        payload.insert(payload.end(), b, b + 16);
    }
    std::vector<uint8_t> file(8);
    const uint32_t id = 0xe34010, sz = uint32_t(payload.size());
    memcpy(file.data(), &id, 4), memcpy(file.data() + 4, &sz, 4);
    file.insert(file.end(), payload.begin(), payload.end());

    CarPartAnims anims;
    if (!anims.Load(file))
        return Fail("no clips");
    const CarPartAnims::Clip *c = anims.Find("HOOD_FRONT");
    if (!c)
        return Fail("HOOD_FRONT not found");
    if (c->name != name)
        return Fail("name");
    if (c->angle.size() != size_t(n))
        return Fail("key count");
    if (std::fabs(c->At(c->Duration()) - 60.0f * 3.14159265f / 180.0f) > 0.01f)
        return Fail("end angle");
    if (std::fabs(c->At(0.0f)) > 1e-4f || std::fabs(c->At(c->Duration() * 0.5f) - 30.0f * 3.14159265f / 180.0f) > 0.01f)
        return Fail("curve");
    if (std::fabs(c->Duration() - 1.0f) > 1e-4f)
        return Fail("duration");
    printf("partanim_test: ok\n");
    return 0;
}
