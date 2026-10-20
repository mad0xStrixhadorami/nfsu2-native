// 0x134011 solid-header transform (#175): the detached stock spoiler is authored around its own
// origin and mounted by the column-major matrix at header +0x40 (translation in [12..14]).
#include "game/indep/World/SolidMesh.h"

#include <cassert>
#include <cmath>

namespace {

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

} // namespace

int main()
{
    SolidMesh::NamedSolid solid;
    const SolidMesh::Point p{1.0f, 2.0f, 3.0f};
    SolidMesh::Point q = SolidMesh::TransformPoint(solid, p);
    assert(Near(q.x, 1.0f) && Near(q.y, 2.0f) && Near(q.z, 3.0f));

    // SKYLINE_KIT00_SPOILER_A's header translation: (-2.1749325, 0, 0.7869703).
    solid.transform[12] = -2.1749325f;
    solid.transform[14] = 0.7869703f;
    q = SolidMesh::TransformPoint(solid, p);
    assert(Near(q.x, -1.1749325f) && Near(q.y, 2.0f) && Near(q.z, 3.7869703f));

    // A Y mirror (right-hand mirror LOD) still follows the same row-vector convention.
    solid.transform[12] = solid.transform[14] = 0.0f;
    solid.transform[5] = -1.0f;
    q = SolidMesh::TransformPoint(solid, p);
    assert(Near(q.x, 1.0f) && Near(q.y, -2.0f) && Near(q.z, 3.0f));
    return 0;
}
