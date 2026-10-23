#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/CarMaterial.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <cstring>

#include "game/indep/World/TrackWorld.h"

namespace {
bool EndsWith(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool Contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

// The Body Shop's hoods and lights are solids in the car's own GEOMETRY.BIN, named
// <CAR>_STYLE<NN>_HOOD[_LEFT|_RIGHT]_A, <CAR>_STYLE<NN>_HEADLIGHT_LEFT_A / _RIGHT_A and
// <CAR>_STYLE<NN>_BRAKELIGHT_A (ISSUES #188).  Names are cut to 27 characters on disc, so a hood's
// right half loses its LOD letter ("<CAR>_STYLE21_HOOD_RIGHT_"); the first solid with that name in
// the file is the A level.  Matching the "_STYLE<NN>_HOOD" prefix covers the one-piece, left-half
// and right-half hoods without a per-style table.
bool IsInstalledPanel(const std::string &n, const PlayerCar::InstalledParts &parts,
                      const SolidMesh::BodyStyleSet &styles)
{
    // A catalogue style the car does not ship is treated as stock (#192): the shop hides those
    // records, and an old save that still names one must not leave a hole in the body.
    const int hood = styles.Has(9, parts.hood) ? parts.hood : 0;
    const int headlight = styles.Has(0xe, parts.headlight) ? parts.headlight : 0;
    const int taillight = styles.Has(0xf, parts.taillight) ? parts.taillight : 0;
    const int frontBumper = styles.Has(0x1, parts.frontBumper) ? parts.frontBumper : 0;
    const int rearBumper = styles.Has(0x2, parts.rearBumper) ? parts.rearBumper : 0;
    const int skirt = styles.Has(0xb, parts.skirt) ? parts.skirt : 0;
    char tag[32];
    if (hood > 0) {
        snprintf(tag, sizeof(tag), "_STYLE%02d_HOOD", hood);
        if (Contains(n, tag))
            return true;
        // The split hoods (styles 21-25) ship their own underside; the rest reuse KIT00's.
        snprintf(tag, sizeof(tag), "_KIT%02d_HOOD_UNDER", hood);
        if (Contains(n, tag))
            return true;
    }
    if (headlight > 0) {
        snprintf(tag, sizeof(tag), "_STYLE%02d_HEADLIGHT", headlight);
        if (Contains(n, tag))
            return true;
    }
    if (taillight > 0) {
        snprintf(tag, sizeof(tag), "_STYLE%02d_BRAKELIGHT", taillight);
        if (Contains(n, tag))
            return true;
    }
    // The bumpers and skirts are kit panels (#190), e.g. SKYLINE_KIT17_FRONT_BUMPER_A.
    if (frontBumper > 0) {
        snprintf(tag, sizeof(tag), "_KIT%02d_FRONT_BUMPER", frontBumper);
        if (Contains(n, tag))
            return true;
    }
    if (rearBumper > 0) {
        snprintf(tag, sizeof(tag), "_KIT%02d_REAR_BUMPER", rearBumper);
        if (Contains(n, tag))
            return true;
    }
    if (skirt > 0) {
        snprintf(tag, sizeof(tag), "_KIT%02d_SKIRT", skirt);
        if (Contains(n, tag))
            return true;
    }
    return false;
}

// A wide-body kit (#193) replaces the whole KIT00 shell, not one panel.  The KITW BODY_A..D and
// DOOR_LEFT/RIGHT_A..C names follow the same LOD convention as every other car solid (the triangle
// counts fall A > B > C > D), so only the A level is drawn.  The door panel/sill names are cut to
// 27 characters and have no LOD letter; their single solid is the outer skin.
bool IsWideBodyPanel(const std::string &n, int kit)
{
    char tag[32];
    snprintf(tag, sizeof(tag), "_KITW%02d_", kit);
    if (!Contains(n, tag))
        return false;
    if (Contains(n, "_BODY_"))
        return EndsWith(n, "_A");
    if (Contains(n, "_DOOR_LEFT_") || Contains(n, "_DOOR_RIGHT_"))
        return EndsWith(n, "_A");
    return Contains(n, "_DOOR_PANEL_") || Contains(n, "_DOOR_SILL_");
}

bool IsReplacedStockWideBody(const std::string &n)
{
    return Contains(n, "_KIT00_BODY_") || Contains(n, "_KIT00_DOOR_");
}

// The stock panel that an installed style replaces.  The hood's underside (KIT00_HOOD_UNDER) is a
// separate panel: one-piece hoods keep it, while the split styles 21-25 bring KIT<NN>_HOOD_UNDER.
bool IsReplacedStockPanel(const std::string &n, const PlayerCar::InstalledParts &parts, bool splitHood,
                          const SolidMesh::BodyStyleSet &styles)
{
    if (styles.Has(9, parts.hood) && Contains(n, "_KIT00_HOOD") &&
        (splitHood || !Contains(n, "HOOD_UNDER")))
        return true;
    if (styles.Has(0xe, parts.headlight) && Contains(n, "_KIT00_HEADLIGHT"))
        return true;
    if (styles.Has(0xf, parts.taillight) && Contains(n, "_KIT00_BRAKELIGHT"))
        return true;
    if (styles.Has(0x1, parts.frontBumper) && Contains(n, "_KIT00_FRONT_BUMPER"))
        return true;
    if (styles.Has(0x2, parts.rearBumper) && Contains(n, "_KIT00_REAR_BUMPER"))
        return true;
    if (styles.Has(0xb, parts.skirt) && Contains(n, "_KIT00_SKIRT"))
        return true;
    return false;
}

// 0x13401a marker name hashes (bStringHash of the marker names): the base solid carries the roof
// scoop mount and each rear bumper one exhaust-tip mount (ISSUES.md #178).
constexpr uint32_t kRoofScoopMarker = 0x90c81258;    // ROOF_SCOOP
constexpr uint32_t kSpoilerMarker = 0xc93b73fd;     // SPOILER, base solid (-2.17 0.00 0.79)
constexpr uint32_t kLeftExhaustMount = 0x69fdf59d;  // bumper exhaust-tip mount, left (-2.16, +0.25, 0.10)
constexpr uint32_t kRightExhaustMount = 0xe2527054; // bumper exhaust-tip mount, right (-2.16, -0.25, 0.10)

// A solid from a universal part file (CARS\ROOF / CARS\EXHAUST) is modelled around its own origin;
// its header translation is the shop display line-up, so the car's position marker is the mount.
void AddMounted(const SolidMesh::NamedSolid &s, const SolidMesh::PositionMarker &marker,
                std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex)
{
    for (const SolidMesh::Triangle &t : s.tris) {
        SolidMesh::Triangle o = t;
        for (int k = 0; k < 3; ++k)
            o.v[k] = SolidMesh::TransformPoint(marker.matrix, t.v[k]);
        out.push_back(o);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < s.textures.size() ? s.textures[t.tex] : 0);
    }
}

// Exhaust tips are authored along +X with the rolled outlet lip at bbox min and the pipe end at bbox
// max (the universal file is a shop line-up, so the header translations are not mounts).  The rear
// bumper carries the tip mounts: 0x69fdf59d on the left and 0xe2527054 on the right, both with an
// identity basis at (-2.16, +-0.25, 0.10).  The stock bumper has no cut-out, so the port seats the
// lip just behind the bumper's rearmost face to keep the tip visible; the original's exact offset
// comes from the car attribute data, which is not decoded (ISSUES.md #180).
void AddExhaustTip(const SolidMesh::NamedSolid &s, const SolidMesh::PositionMarker &mount, float rearFace,
                   std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex)
{
    const float lipX = rearFace - 0.06f;
    for (const SolidMesh::Triangle &t : s.tris) {
        SolidMesh::Triangle o = t;
        for (int k = 0; k < 3; ++k) {
            const SolidMesh::Point &p = t.v[k];
            o.v[k] = p;
            o.v[k].x = p.x - s.bboxMin[0] + lipX;
            o.v[k].y = p.y + mount.matrix[13];
            o.v[k].z = p.z + mount.matrix[14];
        }
        out.push_back(o);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < s.textures.size() ? s.textures[t.tex] : 0);
    }
}

// Wheel solids are modelled around their own hub (the attachment points come from the unported
// car attribute data), so they are placed at estimated axle positions from the body bounds.
void AddWheel(const SolidMesh::NamedSolid &wheel, float x, float y, float z, bool mirror,
              std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex, std::vector<uint8_t> &rim)
{
    // The wheel solid is a single face at local y ~= 0 (tyre sidewall plus rim) backed by a quad at
    // y ~= width.  Only the inner radial part of that face is the rim; the outer ring is the tyre
    // sidewall and must stay dark.  The same 0.68 radius split is used for the debug dump (#161).
    const float cx = (wheel.bboxMin[0] + wheel.bboxMax[0]) * 0.5f;
    const float cz = (wheel.bboxMin[2] + wheel.bboxMax[2]) * 0.5f;
    const float radius = 0.5f * std::max(wheel.bboxMax[0] - wheel.bboxMin[0], wheel.bboxMax[2] - wheel.bboxMin[2]);
    const float rimRadius = radius * 0.68f;
    for (const SolidMesh::Triangle &t : wheel.tris) {
        SolidMesh::Triangle w = t;
        for (int k = 0; k < 3; ++k) {
            w.v[k].x += x;
            w.v[k].y = (mirror ? -t.v[k].y : t.v[k].y) + y;
            w.v[k].z += z;
        }
        const float ty = (t.v[0].y + t.v[1].y + t.v[2].y) / 3.0f;
        const float tx = (t.v[0].x + t.v[1].x + t.v[2].x) / 3.0f - cx;
        const float tz = (t.v[0].z + t.v[1].z + t.v[2].z) / 3.0f - cz;
        const bool isRim = ty <= 0.02f && std::sqrt(tx * tx + tz * tz) <= rimRadius;
        out.push_back(w);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < wheel.textures.size() ? wheel.textures[t.tex] : 0);
        rim.push_back(isRim ? 1 : 0);
    }
}
// Brake solids are authored around their own hub like the wheels; place a copy at each wheel
// centre, just inboard of the rim face.  Material slot 0 is the caliper, slot 1 the disc, so only
// slot 0 gets the Brakes paint (#173).
void AddBrake(const SolidMesh::NamedSolid &brake, float x, float y, float z, bool mirror,
              std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex,
              std::vector<uint8_t> &rim, std::vector<uint8_t> &caliper)
{
    for (const SolidMesh::Triangle &t : brake.tris) {
        SolidMesh::Triangle b = t;
        for (int k = 0; k < 3; ++k) {
            b.v[k].x += x;
            b.v[k].y = (mirror ? -t.v[k].y : t.v[k].y) + y;
            b.v[k].z += z;
        }
        out.push_back(b);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < brake.textures.size() ? brake.textures[t.tex] : 0);
        rim.push_back(0);
        caliper.push_back(t.tex == 0 ? 1 : 0);
    }
}
// Drop triangles reaching outside the part's header bounds (a guard against mis-decoded vertices;
// with the exact strip topology, ISSUES.md #110, none are expected).
void AddFiltered(const SolidMesh::NamedSolid &s, std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex)
{
    auto inside = [&s](const SolidMesh::Point &p) {
        constexpr float m = 0.05f;
        return p.x >= s.bboxMin[0] - m && p.x <= s.bboxMax[0] + m && p.y >= s.bboxMin[1] - m &&
               p.y <= s.bboxMax[1] + m && p.z >= s.bboxMin[2] - m && p.z <= s.bboxMax[2] + m;
    };
    for (const SolidMesh::Triangle &t : s.tris)
        if (inside(t.v[0]) && inside(t.v[1]) && inside(t.v[2])) {
            out.push_back(t);
            tex.push_back(t.tex >= 0 && size_t(t.tex) < s.textures.size() ? s.textures[t.tex] : 0);
        }
}
// Detached car parts (the stock spoiler, #175) are modelled around their own origin and carry the
// mount transform in the 0x134011 header.  Apply it as the game's solid loader does.
void AddTransformed(const SolidMesh::NamedSolid &s, std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex)
{
    for (const SolidMesh::Triangle &t : s.tris) {
        SolidMesh::Triangle o = t;
        for (int k = 0; k < 3; ++k)
            o.v[k] = SolidMesh::TransformPoint(s, t.v[k]);
        out.push_back(o);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < s.textures.size() ? s.textures[t.tex] : 0);
    }
}
} // namespace

PlayerCar::InstalledParts PlayerCar::InstalledPartsFromEnvironment(
    const std::function<std::vector<uint8_t>(const char *)> &read)
{
    InstalledParts parts;
    if (const char *roof = getenv("NFSU2_PART_ROOF")) {
        parts.roof = atoi(roof);
        parts.roofDual = strstr(roof, "DUAL") != nullptr;
        parts.roofOffset = strstr(roof, "OFFSET") != nullptr;
        parts.roofCarbon = strstr(roof, "CF") != nullptr;
        if (parts.roof > 0 && read)
            parts.roofGeometry = read("CARS\\ROOF\\GEOMETRY.BIN");
        if (parts.roof > 0 && parts.roofGeometry.empty()) {
            fprintf(stderr, "[Parts] roof scoop %d: CARS\\ROOF\\GEOMETRY.BIN is missing\n", parts.roof);
            parts.roof = 0;
        }
    }
    if (const char *exhaust = getenv("NFSU2_PART_EXHAUST")) {
        parts.exhaust = atoi(exhaust);
        if (parts.exhaust > 0 && read)
            parts.exhaustGeometry = read("CARS\\EXHAUST\\GEOMETRY.BIN");
        if (parts.exhaust > 0 && parts.exhaustGeometry.empty()) {
            fprintf(stderr, "[Parts] exhaust tip %d: CARS\\EXHAUST\\GEOMETRY.BIN is missing\n", parts.exhaust);
            parts.exhaust = 0;
        }
    }
    if (const char *engine = getenv("NFSU2_PART_ENGINE"))
        parts.engine = atoi(engine);
    if (const char *spoiler = getenv("NFSU2_PART_SPOILER")) {
        parts.spoiler = atoi(spoiler);
        parts.spoilerCarbon = strstr(spoiler, "CF") != nullptr;
        if (parts.spoiler > 0 && read)
            parts.spoilerGeometry = read("CARS\\SPOILER\\GEOMETRY.BIN");
        if (parts.spoiler > 0 && parts.spoilerGeometry.empty()) {
            fprintf(stderr, "[Parts] spoiler %d: CARS\\SPOILER\\GEOMETRY.BIN is missing\n", parts.spoiler);
            parts.spoiler = 0;
        }
    }
    // Body panels live in the car file, so only the style/finish selection is needed here (#188).
    if (const char *hood = getenv("NFSU2_PART_HOOD")) {
        parts.hood = atoi(hood);
        parts.hoodCarbon = strstr(hood, "CF") != nullptr;
    }
    if (const char *headlight = getenv("NFSU2_PART_HEADLIGHT"))
        parts.headlight = atoi(headlight);
    if (const char *taillight = getenv("NFSU2_PART_TAILLIGHT"))
        parts.taillight = atoi(taillight);
    // Kit-number body panels (#190): bumpers and side skirts are car-file solids too.
    if (const char *bumper = getenv("NFSU2_PART_FRONT_BUMPER"))
        parts.frontBumper = atoi(bumper);
    if (const char *bumper = getenv("NFSU2_PART_REAR_BUMPER"))
        parts.rearBumper = atoi(bumper);
    if (const char *skirt = getenv("NFSU2_PART_SKIRT"))
        parts.skirt = atoi(skirt);
    // NFSU2_PART_RIM=<BRAND>:<style>, e.g. ENKEI:4 (brand file CARS\\WHEELS\\GEOMETRY_<geometry>.BIN; OZ is P9).
    if (const char *rim = getenv("NFSU2_PART_RIM")) {
        const char *colon = strchr(rim, ':');
        if (colon) {
            parts.rimBrand.assign(rim, colon - rim);
            parts.rimStyle = atoi(colon + 1);
            parts.rimGeometry = read(("CARS\\WHEELS\\GEOMETRY_" + std::string(parts.rimBrand == "OZ" ? "P9" : parts.rimBrand) + ".BIN").c_str());
        }
    }
    if (const char *neon = getenv("NFSU2_PART_NEON")) // 1..10
        parts.neon = atoi(neon);
    if (const char *tint = getenv("NFSU2_PART_TINT")) // 1..3
        parts.tint = atoi(tint);
    if (const char *wide = getenv("NFSU2_PART_WIDEBODY"))
        parts.wideBody = atoi(wide);
    if (parts.roof || parts.exhaust || parts.engine || parts.spoiler)
        printf("[Parts] installed roof %d%s%s%s exhaust %d engine %d\n", parts.roof,
               parts.roofDual ? " dual" : "", parts.roofOffset ? " offset" : "", parts.roofCarbon ? " carbon" : "",
               parts.exhaust, parts.engine);
    if (parts.spoiler)
        printf("[Parts] installed spoiler %d%s\n", parts.spoiler, parts.spoilerCarbon ? " carbon" : "");
    if (parts.hood || parts.headlight || parts.taillight)
        printf("[Parts] installed hood %d%s headlight %d taillight %d\n", parts.hood,
               parts.hoodCarbon ? " carbon" : "", parts.headlight, parts.taillight);
    if (parts.frontBumper || parts.rearBumper || parts.skirt || parts.wideBody)
        printf("[Parts] installed front bumper %d rear bumper %d skirt %d wide body %d\n",
               parts.frontBumper, parts.rearBumper, parts.skirt, parts.wideBody);
    return parts;
}

bool PlayerCar::LoadModel(const std::vector<uint8_t> &geometry, const Wheel *wheels, const InstalledParts *parts)
{
    m_model.clear();
    m_triTex.clear();
    m_triRim.clear();
    m_triCaliper.clear();
    m_triMirror.clear();
    m_triSpoiler.clear();
    m_triRoof.clear();
    m_triEngine.clear();
    m_triExhaust.clear();
    std::vector<SolidMesh::NamedSolid> solids = SolidMesh::DecodeNamedSolids(geometry.data(), geometry.size());
    m_bodyStyles = SolidMesh::FindBodyStyles(solids);
    // A split hood (style 21-25 on the roster cars) has its own KIT<NN>_HOOD_UNDER_LE/RI pair, so
    // its stock KIT00 underside must go too (#188).  Detect it from the file rather than hard-coding
    // the style range.
    bool splitHood = false;
    if (parts && m_bodyStyles.Has(9, parts->hood)) {
        char underTag[32];
        snprintf(underTag, sizeof(underTag), "_KIT%02d_HOOD_UNDER", parts->hood);
        for (const SolidMesh::NamedSolid &s : solids)
            if (Contains(s.name, underTag)) {
                splitHood = true;
                break;
            }
    }
    if (parts && getenv("NFSU2_PART_DEBUG")) {
        auto missing = [&](int partId, int style, const char *name) {
            if (style > 0 && !m_bodyStyles.Has(partId, style))
                printf("[Parts] %s style %d is not in this car's geometry; stock panel kept (#192)\n",
                       name, style);
        };
        missing(9, parts->hood, "hood");
        missing(0xe, parts->headlight, "headlight");
        missing(0xf, parts->taillight, "taillight");
        missing(0x1, parts->frontBumper, "front bumper");
        missing(0x2, parts->rearBumper, "rear bumper");
        missing(0xb, parts->skirt, "side skirt");
        missing(6, parts->wideBody, "wide body");
    }
    // Dev aid: NFSU2_SOLID_DUMP=1 lists every named solid with its header transform and bounds,
    // for finding parts the port has not mounted yet (ISSUES #178).
    if (getenv("NFSU2_SOLID_DUMP")) {
        for (const SolidMesh::NamedSolid &s : solids) {
            printf("[Solid] %-30s tris %4zu tex %zu bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f) mount (%.2f %.2f %.2f)\n",
                   s.name.c_str(), s.tris.size(), s.textures.size(), s.bboxMin[0], s.bboxMin[1], s.bboxMin[2],
                   s.bboxMax[0], s.bboxMax[1], s.bboxMax[2], s.transform[12], s.transform[13], s.transform[14]);
            // 0x13401a part/effect markers (#178): the hashes the car code looks up by name.
            for (const SolidMesh::PositionMarker &m : s.markers)
                printf("[Solid]   marker 0x%08x at (%.2f %.2f %.2f) x (%.2f %.2f %.2f) y (%.2f %.2f %.2f) z (%.2f %.2f %.2f)\n",
                       m.hash, m.matrix[12], m.matrix[13], m.matrix[14], m.matrix[0], m.matrix[1], m.matrix[2],
                       m.matrix[4], m.matrix[5], m.matrix[6], m.matrix[8], m.matrix[9], m.matrix[10]);
            // NFSU2_MARKER_MATRIX=1 adds each marker's 3x4 basis (row-vector, translation omitted):
            // part mounts are identity, while the effect markers (exhaust plume) are rotated.
            if (getenv("NFSU2_MARKER_MATRIX") && !s.markers.empty())
                for (const SolidMesh::PositionMarker &m : s.markers)
                    printf("[Solid]     %08x [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]\n",
                           m.hash, m.matrix[0], m.matrix[1], m.matrix[2], m.matrix[3], m.matrix[4], m.matrix[5],
                           m.matrix[6], m.matrix[7], m.matrix[8], m.matrix[9], m.matrix[10], m.matrix[11]);
        }
    }
    std::set<std::string> seen;
    const SolidMesh::NamedSolid *frontWheel = nullptr, *rearWheel = nullptr, *body = nullptr;
    const SolidMesh::NamedSolid *frontBrake = nullptr, *rearBrake = nullptr;
    const SolidMesh::PositionMarker *roofMarker = nullptr, *exhaustMarker = nullptr;
    const SolidMesh::PositionMarker *spoilerMarker = nullptr;
    float bumperRear = 0.0f; // rear bumper bbox min x (car space)
    // Service/traffic vehicles (TAXI, BUS, ...) have no tuning kits: their parts are <NAME>_BODY_A and
    // <NAME>_TIRE_FRONT_A (ISSUES #130), so accept those when there is no KIT00 part at all.
    bool traffic = true;
    for (const SolidMesh::NamedSolid &s : solids)
        if (Contains(s.name, "_KIT00_"))
            traffic = false;
    for (const SolidMesh::NamedSolid &s : solids) {
        const std::string &n = s.name;
        // Engine accents are car-specific solids in car space (<CAR>_STYLE<NN>_ENGINE_A), so the
        // installed style is selected here rather than mounted from a universal file (#178).
        if (parts && parts->engine > 0) {
            char tag[32];
            snprintf(tag, sizeof(tag), "_STYLE%02d_ENGINE", parts->engine);
            if (Contains(n, tag) && EndsWith(n, "_A")) {
                const size_t before = m_model.size();
                AddFiltered(s, m_model, m_triTex);
                m_triEngine.resize(m_model.size(), 0);
                for (size_t i = before; i < m_model.size(); ++i)
                    m_triEngine[i] = 1;
                if (getenv("NFSU2_PART_DEBUG"))
                    printf("[Parts] engine accent %s: %zu triangles\n", n.c_str(), m_model.size() - before);
                continue;
            }
        }
        // A fitted wide-body kit replaces the KIT00 shell and door skins as a set (#193).  Only the
        // A LOD of each KITW panel is drawn, exactly like the stock KIT00 shell.
        if (parts && m_bodyStyles.Has(6, parts->wideBody) && IsWideBodyPanel(n, parts->wideBody)) {
            const size_t before = m_model.size();
            AddFiltered(s, m_model, m_triTex);
            if (Contains(n, "_BODY_"))
                body = &s;
            if (getenv("NFSU2_PART_DEBUG"))
                printf("[Parts] wide body panel %s: %zu triangles\n", n.c_str(), m_model.size() - before);
            continue;
        }
        if (parts && m_bodyStyles.Has(6, parts->wideBody) && IsReplacedStockWideBody(n))
            continue;
        // Body panels from this car's own file (#188/#190): the shop's hoods, headlights,
        // taillights, bumpers and skirts are all present as <CAR>_... solids, so select the
        // installed style here.
        if (parts && (parts->hood || parts->headlight || parts->taillight || parts->frontBumper ||
                      parts->rearBumper || parts->skirt) &&
            IsInstalledPanel(n, *parts, m_bodyStyles)) {
            if (EndsWith(n, "_B") || EndsWith(n, "_C") || EndsWith(n, "_D") || !seen.insert(n).second)
                continue;
            AddFiltered(s, m_model, m_triTex);
            if (getenv("NFSU2_PART_DEBUG"))
                printf("[Parts] body panel %s: %zu triangles\n", n.c_str(), s.tris.size());
            continue;
        }
        // The installed style replaces its stock panel; the hood underside stays (see the helper).
        if (parts && IsReplacedStockPanel(n, *parts, splitHood, m_bodyStyles))
            continue;
        if (traffic) {
            if (!Contains(n, "_BODY_") && !Contains(n, "_TIRE_"))
                continue;
        } else if (!Contains(n, "_KIT00_") && !Contains(n, "_BASE_"))
            continue;
        // LOD suffix _A.._D; names are cut to 27 characters, and the A level comes first.
        if (EndsWith(n, "_B") || EndsWith(n, "_C") || EndsWith(n, "_D") || !seen.insert(n).second)
            continue;
        if (Contains(n, "FRONT_WHEEL") || Contains(n, "TIRE_FRONT")) { frontWheel = &s; continue; }
        if (Contains(n, "REAR_WHEEL") || Contains(n, "TIRE_REAR")) { rearWheel = &s; continue; }
        // Parts modelled around their own origin, or hidden inside the body.
        if (Contains(n, "BRAKE_")) {
            if (Contains(n, "FRONT_BRAKE"))
                frontBrake = &s;
            else if (Contains(n, "REAR_BRAKE"))
                rearBrake = &s;
            continue;
        }
        if (Contains(n, "SPOILER")) {
            // The stock KIT00 spoiler is a detached solid whose header transform mounts it on the
            // trunk (#175).  An installed aftermarket wing replaces it (ISSUES #181).
            if (parts && parts->spoiler > 0)
                continue;
            const size_t before = m_model.size();
            AddTransformed(s, m_model, m_triTex);
            m_triSpoiler.resize(m_model.size(), 0);
            for (size_t i = before; i < m_model.size(); ++i)
                m_triSpoiler[i] = 1;
            continue;
        }
        if (Contains(n, "EXHAUST") || Contains(n, "UNDER") ||
            Contains(n, "ENGINE") || Contains(n, "AUDIO") || Contains(n, "UNLIT"))
            continue;
        if (getenv("NFSU2_NO_HOOD") && Contains(n, "HOOD"))
            continue; // dev aid: expose the engine-bay solids (engine accents, #178)
        if (Contains(n, "_BODY_"))
            body = &s;
        const size_t before = m_model.size();
        AddFiltered(s, m_model, m_triTex);
        // Mount points for the installed aftermarket parts: the base solid carries ROOF_SCOOP and
        // the rear bumper the exhaust-tip mounts (left, else right).  The bumper's rearmost face is
        // the reference for seating the tip (ISSUES #180).
        if (Contains(n, "_BASE_")) {
            roofMarker = s.FindMarker(kRoofScoopMarker);
            spoilerMarker = s.FindMarker(kSpoilerMarker);
        }
        if (Contains(n, "REAR_BUMPER")) {
            exhaustMarker = s.FindMarker(kLeftExhaustMount) ? s.FindMarker(kLeftExhaustMount)
                                                            : s.FindMarker(kRightExhaustMount);
            bumperRear = s.bboxMin[0];
        }
        if (Contains(n, "SIDE_MI")) {
            // The mirror solid has a 9-triangle glass in slot 0 and the painted housing in slot 1.
            m_triMirror.resize(m_model.size(), 0);
            for (size_t i = before; i < m_model.size(); ++i)
                m_triMirror[i] = (m_model[i].tex == 1) ? 1 : 0;
        }
    }
    // Installed rims (ISSUES #226): pick the brand style's solid (highest LOD) whose outer radius is
    // nearest the car's tyre and use it on both axles.
    std::vector<SolidMesh::NamedSolid> rimSolids;
    const SolidMesh::NamedSolid *spinSolid = nullptr;
    if (parts && parts->rimStyle > 0 && !parts->rimGeometry.empty() && !parts->rimBrand.empty()) {
        rimSolids = SolidMesh::DecodeNamedSolids(parts->rimGeometry.data(), parts->rimGeometry.size());
        char prefix[48];
        snprintf(prefix, sizeof prefix, "%s_STYLE%02d_", parts->rimBrand.c_str(), parts->rimStyle);
        const float target = wheels ? wheels[0].radius : (frontWheel ? (frontWheel->bboxMax[2] - frontWheel->bboxMin[2]) * 0.5f : 0.32f);
        const SolidMesh::NamedSolid *best = nullptr;
        float bestDiff = 1e9f;
        for (const SolidMesh::NamedSolid &s : rimSolids) {
            if (s.name.compare(0, strlen(prefix), prefix) != 0 || !EndsWith(s.name, "_A") || s.tris.empty())
                continue;
            const float radius = 0.5f * std::max(s.bboxMax[0] - s.bboxMin[0], s.bboxMax[2] - s.bboxMin[2]);
            if (std::fabs(radius - target) < bestDiff)
                bestDiff = std::fabs(radius - target), best = &s;
        }
        if (best) {
            frontWheel = rearWheel = best;
            // Spinners: a separate hub piece (<stem>_SPIN_, first of its three LODs) rides on the wheel.
            const std::string stem = best->name.substr(0, best->name.size() - 1) + "SPIN_";
            for (const SolidMesh::NamedSolid &s : rimSolids)
                if (s.name == stem && !s.tris.empty()) {
                    spinSolid = &s;
                    break;
                }
            printf("[Rims] %s (radius %.3f for a %.3f tyre)\n", best->name.c_str(),
                   0.5f * std::max(best->bboxMax[0] - best->bboxMin[0], best->bboxMax[2] - best->bboxMin[2]), target);
        } else {
            printf("[Rims] no %s solid in the brand file\n", prefix);
        }
    }
    if (frontWheel && getenv("NFSU2_WHEEL_DEBUG"))
        printf("[Wheel] %s: %zu tris bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f) textures %zu\n", frontWheel->name.c_str(),
               frontWheel->tris.size(), frontWheel->bboxMin[0], frontWheel->bboxMin[1], frontWheel->bboxMin[2],
               frontWheel->bboxMax[0], frontWheel->bboxMax[1], frontWheel->bboxMax[2], frontWheel->textures.size());
    if (frontWheel && getenv("NFSU2_WHEEL_DEBUG")) {
        float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        for (const SolidMesh::Triangle &t : frontWheel->tris)
            for (int k = 0; k < 3; ++k)
                u0 = std::min(u0, t.v[k].u), u1 = std::max(u1, t.v[k].u), v0 = std::min(v0, t.v[k].v), v1 = std::max(v1, t.v[k].v);
        printf("[Wheel] uv range u %.3f..%.3f v %.3f..%.3f\n", u0, u1, v0, v1);
        // Detailed rim face (y <= 0.02), flat backing quad (y >= 0.2) and the tread in between.
        for (int region = 0; region < 3; ++region) {
            float a0 = 1e9f, a1 = -1e9f, b0 = 1e9f, b1 = -1e9f;
            int n = 0;
            for (const SolidMesh::Triangle &t : frontWheel->tris) {
                const float y = (t.v[0].y + t.v[1].y + t.v[2].y) / 3.0f;
                const int r = y >= 0.2f ? 0 : y <= 0.02f ? 1 : 2;
                if (r != region)
                    continue;
                ++n;
                for (int k = 0; k < 3; ++k)
                    a0 = std::min(a0, t.v[k].u), a1 = std::max(a1, t.v[k].u), b0 = std::min(b0, t.v[k].v), b1 = std::max(b1, t.v[k].v);
            }
            printf("[Wheel]  %s: %d tris u %.3f..%.3f v %.3f..%.3f\n", region == 0 ? "back" : region == 1 ? "rim" : "tread", n, a0, a1, b0, b1);
        }
        // Dev aid: NFSU2_WHEEL_DUMP=1 prints the rim/backing triangles, =2 every wheel triangle
        // (position, UV, material slot), to check which side of the solid carries the rim face.
        if (const char *wd = getenv("NFSU2_WHEEL_DUMP")) {
            const int mode = atoi(wd);
            for (const SolidMesh::Triangle &t : frontWheel->tris) {
                const float y = (t.v[0].y + t.v[1].y + t.v[2].y) / 3.0f;
                if (mode == 1 && y < 0.2f)
                    continue;
                printf("[Wheel] tri y %.3f tex %d", y, int(t.tex));
                for (int k = 0; k < 3; ++k)
                    printf(" (%.3f %.3f %.3f uv %.3f %.3f)", t.v[k].x, t.v[k].y, t.v[k].z, t.v[k].u, t.v[k].v);
                printf("\n");
            }
        }
    }
    if (frontWheel && getenv("NFSU2_WHEEL_DEBUG"))
        for (uint32_t t : frontWheel->textures)
            printf("[Wheel] texture slot hash %08x\n", t);
    if (!rearWheel)
        rearWheel = frontWheel;
    if (wheels && getenv("NFSU2_WHEEL_DEBUG"))
        for (int i = 0; i < 4; ++i)
            printf("[Wheel] slot %d x %.3f y %.3f z %.3f width %.3f radius %.3f\n", i, wheels[i].x, wheels[i].y,
                   wheels[i].z, wheels[i].width, wheels[i].radius);
    if (body && getenv("NFSU2_WHEEL_DEBUG"))
        printf("[Wheel] body bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", body->bboxMin[0], body->bboxMin[1],
               body->bboxMin[2], body->bboxMax[0], body->bboxMax[1], body->bboxMax[2]);
    // Specialties (#227): tint retags the glass slots; the underglow is a flat ring of emissive
    // triangles just above the road around the body.
    if (parts && parts->tint > 0 && parts->tint <= 3) {
        char slotName[24];
        snprintf(slotName, sizeof slotName, "WINDOW_TINT%d", parts->tint);
        const uint32_t tinted = CarMaterial::NameHash(slotName);
        const uint32_t windows[] = {CarMaterial::NameHash("WINDOW_FRONT"), CarMaterial::NameHash("WINDOW_REAR"),
                                    CarMaterial::NameHash("WINDOW_LEFT_FRONT"), CarMaterial::NameHash("WINDOW_RIGHT_FRONT"),
                                    CarMaterial::NameHash("WINDOW_LEFT_REAR"), CarMaterial::NameHash("WINDOW_RIGHT_REAR")};
        for (uint32_t &t : m_triTex)
            for (uint32_t w : windows)
                if (t == w)
                    t = tinted;
    }
    if (parts && parts->neon > 0 && parts->neon <= 10 && body) {
        char slotName[24];
        snprintf(slotName, sizeof slotName, "NEON_GLOW_%d", parts->neon);
        const uint32_t glow = CarMaterial::NameHash(slotName);
        const float x0 = body->bboxMin[0] + 0.25f, x1 = body->bboxMax[0] - 0.25f;
        const float yo = std::max(std::fabs(body->bboxMin[1]), std::fabs(body->bboxMax[1])) + 0.18f, yi = yo - 0.32f;
        const float z = 0.07f;
        auto vert = [](float x, float y, float zz) { SolidMesh::Point p; p.x = x; p.y = y; p.z = zz; p.u = p.v = 0; return p; };
        auto quad = [&](float xa, float ya, float xb, float yb) {
            SolidMesh::Triangle t;
            t.tex = -1;
            t.v[0] = vert(xa, ya, z); t.v[1] = vert(xb, ya, z); t.v[2] = vert(xb, yb, z);
            m_model.push_back(t); m_triTex.push_back(glow);
            t.v[0] = vert(xa, ya, z); t.v[1] = vert(xb, yb, z); t.v[2] = vert(xa, yb, z);
            m_model.push_back(t); m_triTex.push_back(glow);
        };
        quad(x0, yi, x1, yo);   // left strip
        quad(x0, -yo, x1, -yi); // right strip
        quad(x1, -yo, x1 + 0.3f, yo); // front
        quad(x0 - 0.3f, -yo, x0, yo); // rear
    }
    m_triRim.resize(m_model.size(), 0);
    m_triCaliper.resize(m_model.size(), 0);
    m_rideHeight = 0;
    m_wheelStart = m_model.size();
    if (frontWheel && wheels) {
        // Database placement: the wheel solid's detailed rim face is at y ~= 0 (normal -y) and a
        // flat backing quad at y ~= width, so the solid is a right-hand wheel (outboard -y) as
        // authored. Mirror it on the left so the rim face stays outboard on both sides; the old
        // code mirrored the right side instead, which pointed the rim face into the car and left
        // the flat backing quad visible as a grey/black square (ISSUES #161). Place the wheel's
        // outboard edge at the database track position + half the tyre width using the solid's
        // own bounds, since its authored width differs slightly from the database figure.
        for (int i = 0; i < 4; ++i) {
            const Wheel &w = wheels[i];
            const bool left = w.y > 0;
            const SolidMesh::NamedSolid &solid = (i < 2 || !rearWheel) ? *frontWheel : *rearWheel;
            const float outboard = std::fabs(w.y) + w.width * 0.5f;
            const float yMin = solid.bboxMin[1];
            AddWheel(solid, w.x, left ? outboard + yMin : -outboard - yMin, w.z, left, m_model, m_triTex, m_triRim);
            if (spinSolid)
                AddWheel(*spinSolid, w.x, left ? outboard + yMin : -outboard - yMin, w.z, left, m_model, m_triTex, m_triRim);
        }
        m_rideHeight = std::max(0.0f, wheels[0].radius - wheels[0].z);
        m_wheelRadius = wheels[0].radius;
    } else if (body && frontWheel) {
        float radius = (frontWheel->bboxMax[2] - frontWheel->bboxMin[2]) * 0.5f;
        float front = body->bboxMax[0] - 0.85f, rear = body->bboxMin[0] + 0.95f;
        // No database wheel positions: put the tyres just inside the widest body point and use the
        // same mirror/placement rule as above (rim face outboard on both sides).
        float outboard = std::max(0.3f, body->bboxMax[1] - 0.05f);
        float z = radius;
        AddWheel(*frontWheel, front, outboard + frontWheel->bboxMin[1], z, true, m_model, m_triTex, m_triRim);
        AddWheel(*frontWheel, front, -outboard - frontWheel->bboxMin[1], z, false, m_model, m_triTex, m_triRim);
        AddWheel(*rearWheel, rear, outboard + rearWheel->bboxMin[1], z, true, m_model, m_triTex, m_triRim);
        AddWheel(*rearWheel, rear, -outboard - rearWheel->bboxMin[1], z, false, m_model, m_triTex, m_triRim);
    }
    // AddWheel only appends the rim mask; pad the caliper mask with the wheel triangles before the
    // brake triangles are appended, otherwise the caliper flags shift onto the first wheel faces.
    m_triCaliper.resize(m_model.size(), 0);
    if (wheels && (frontBrake || rearBrake)) {
        // The wheel face sits at its placement y; brake solids span local y -0.05..0.03, so push
        // them 0.055 m inboard (opposite the rim face) to keep them behind the spokes.
        for (int i = 0; i < 4; ++i) {
            const SolidMesh::NamedSolid *brake = (i < 2 ? frontBrake : rearBrake);
            const SolidMesh::NamedSolid *wheelSolid = (i < 2) ? frontWheel : rearWheel;
            if (!brake || !wheelSolid)
                continue;
            const bool left = (i == 0 || i == 3);
            const PlayerCar::Wheel &w = wheels[i];
            const float outboard = std::fabs(w.y) + w.width * 0.5f;
            const float wheelY = left ? outboard + wheelSolid->bboxMin[1] : -outboard - wheelSolid->bboxMin[1];
            const float brakeY = left ? wheelY - 0.055f : wheelY + 0.055f;
            AddBrake(*brake, w.x, brakeY, w.z, left, m_model, m_triTex, m_triRim, m_triCaliper);
        }
    }
    // Installed aftermarket parts (#178).  Universal part files hold every style as a shop display
    // line-up, so ignore their header translations and mount the chosen solid at the car marker.
    if (parts && parts->roof > 0 && !parts->roofGeometry.empty()) {
        char name[48];
        snprintf(name, sizeof(name), "ROOF_STYLE%02d%s%s%s_A", parts->roof, parts->roofDual ? "_DUAL" : "",
                 parts->roofOffset ? "_OFFSET" : "", parts->roofCarbon ? "_CF" : "");
        std::vector<SolidMesh::NamedSolid> roof =
            SolidMesh::DecodeNamedSolids(parts->roofGeometry.data(), parts->roofGeometry.size());
        const auto it = std::find_if(roof.begin(), roof.end(), [name](const SolidMesh::NamedSolid &s) {
            return s.name == name;
        });
        if (it != roof.end() && roofMarker) {
            const size_t before = m_model.size();
            AddMounted(*it, *roofMarker, m_model, m_triTex);
            m_triRoof.resize(m_model.size(), 0);
            for (size_t i = before; i < m_model.size(); ++i)
                m_triRoof[i] = 1;
            printf("[Parts] roof scoop %s: %zu triangles at (%.2f %.2f %.2f)\n", name, m_model.size() - before,
                   roofMarker->matrix[12], roofMarker->matrix[13], roofMarker->matrix[14]);
        } else {
            fprintf(stderr, "[Parts] roof scoop %s: %s\n", name,
                    it == roof.end() ? "solid not found" : "no ROOF_SCOOP marker on the body");
        }
    }
    if (parts && parts->exhaust > 0 && !parts->exhaustGeometry.empty()) {
        char name[48];
        snprintf(name, sizeof(name), "EXHAUST_STYLE%02d_LEVEL1_A", parts->exhaust);
        std::vector<SolidMesh::NamedSolid> exhaust =
            SolidMesh::DecodeNamedSolids(parts->exhaustGeometry.data(), parts->exhaustGeometry.size());
        const auto it = std::find_if(exhaust.begin(), exhaust.end(), [name](const SolidMesh::NamedSolid &s) {
            return s.name == name;
        });
        if (it != exhaust.end() && exhaustMarker) {
            const size_t before = m_model.size();
            AddExhaustTip(*it, *exhaustMarker, bumperRear, m_model, m_triTex);
            m_triExhaust.resize(m_model.size(), 0);
            for (size_t i = before; i < m_model.size(); ++i)
                m_triExhaust[i] = 1;
            printf("[Parts] exhaust tip %s: %zu triangles at (%.2f %.2f %.2f)\n", name, m_model.size() - before,
                   exhaustMarker->matrix[12], exhaustMarker->matrix[13], exhaustMarker->matrix[14]);
        } else {
            fprintf(stderr, "[Parts] exhaust tip %s: %s\n", name,
                    it == exhaust.end() ? "solid not found" : "no LEFT/RIGHT_EXHAUST marker on the bumper");
        }
    }
    // Installed aftermarket spoiler (#182): the universal file's 40 styles are a display line-up
    // (header translations step along x/y), so the chosen solid is mounted at the base solid's
    // SPOILER marker, the same point the stock KIT00 spoiler's header transform uses.
    if (parts && parts->spoiler > 0 && !parts->spoilerGeometry.empty()) {
        char name[48];
        snprintf(name, sizeof(name), "SPOILER_STYLE%02d%s_A", parts->spoiler, parts->spoilerCarbon ? "_CF" : "");
        std::vector<SolidMesh::NamedSolid> wings =
            SolidMesh::DecodeNamedSolids(parts->spoilerGeometry.data(), parts->spoilerGeometry.size());
        const auto it = std::find_if(wings.begin(), wings.end(), [name](const SolidMesh::NamedSolid &s) {
            return s.name == name;
        });
        if (it != wings.end() && spoilerMarker) {
            const size_t before = m_model.size();
            AddMounted(*it, *spoilerMarker, m_model, m_triTex);
            m_triSpoiler.resize(m_model.size(), 0);
            for (size_t i = before; i < m_model.size(); ++i)
                m_triSpoiler[i] = 1;
            printf("[Parts] spoiler %s: %zu triangles at (%.2f %.2f %.2f)\n", name, m_model.size() - before,
                   spoilerMarker->matrix[12], spoilerMarker->matrix[13], spoilerMarker->matrix[14]);
        } else {
            fprintf(stderr, "[Parts] spoiler %s: %s\n", name,
                    it == wings.end() ? "solid not found" : "no SPOILER marker on the body");
        }
    }
    m_triRim.resize(m_model.size(), 0);
    m_triCaliper.resize(m_model.size(), 0);
    m_triMirror.resize(m_model.size(), 0);
    m_triSpoiler.resize(m_model.size(), 0);
    m_triRoof.resize(m_model.size(), 0);
    m_triEngine.resize(m_model.size(), 0);
    m_triExhaust.resize(m_model.size(), 0);
    printf("[Car] %zu solids, %zu model triangles\n", solids.size(), m_model.size());
    return !m_model.empty();
}

float PlayerCar::Torque(const Performance &p, float rpm)
{
    // 9 curve points spread evenly over idle..max rpm (the curve's shape -- rising to a peak and
    // falling at the top -- matches each car's real figures, e.g. 350Z peak 0.37 kN*m).
    float t = (rpm - p.idleRpm) / (p.maxRpm - p.idleRpm) * 8.0f;
    if (t <= 0)
        return p.torque[0];
    if (t >= 8)
        return rpm > p.maxRpm ? 0.0f : p.torque[8];
    int i = int(t);
    return p.torque[i] + (p.torque[i + 1] - p.torque[i]) * (t - i);
}

// Stand-in drivetrain from the database figures (UG2's own model, 0x0031d570 / 0x0031e0d0, is not
// ported): engine torque through gear, final drive and drivetrain efficiency to the wheels, limited
// by tyre grip; aero drag and rolling resistance from the record; an automatic gearbox shifting at
// the redline. power scales the engine (the drag-race gearbox, ISSUES.md #115).
void PlayerCar::UpdateEngine(float throttle, float brake, float dt, float power)
{
    const Performance &p = m_perf;
    const float mass = p.mass * 1000.0f, g = 9.81f;
    const float wheelRpm = std::fabs(m_speed) / (2.0f * 3.14159265f * m_wheelRadius) * 60.0f;
    // Gear: reverse when braking at rest, otherwise automatic between redline and the drop below it.
    if (m_speed < -0.5f || (brake > 0 && throttle <= 0 && m_speed < 0.5f))
        m_gear = -1;
    else if (m_gear < 1 && m_speed >= -0.1f && throttle > 0)
        m_gear = 1;
    if (m_gear >= 1) {
        float rpm = wheelRpm * p.gears[m_gear - 1] * p.finalDrive;
        if (rpm > p.redline && m_gear < p.numGears)
            ++m_gear;
        else if (m_gear > 1 && wheelRpm * p.gears[m_gear - 2] * p.finalDrive < p.redline * 0.62f)
            --m_gear;
    }
    const float ratio = (m_gear >= 1 ? p.gears[m_gear - 1] : std::fabs(p.reverse)) * p.finalDrive;
    m_rpm = std::clamp(wheelRpm * ratio, p.idleRpm, p.maxRpm);
    // Clutch slip at launch: the engine runs up to 70% of the redline while the wheels are slow.
    if (throttle > 0)
        m_rpm = std::max(m_rpm, p.idleRpm + (p.redline * 0.7f - p.idleRpm) * throttle * (m_rpm < p.redline * 0.7f));
    float drive = Torque(p, m_rpm) * 1000.0f * ratio * p.efficiency / m_wheelRadius * throttle * power;
    drive = std::min(drive, p.grip * mass * g * 0.55f); // traction (driven axle carries ~55%)
    if (m_gear == -1)
        drive = -std::min(drive, mass * 4.0f);
    float force = drive - p.aero * 1000.0f * m_speed * std::fabs(m_speed) -
                  (m_speed > 0 ? 1.0f : m_speed < 0 ? -1.0f : 0.0f) * p.rolling * mass * g;
    if (brake > 0 && m_gear != -1 && m_speed > 0.1f)
        force -= p.grip * mass * g * 0.9f * brake;
    float v = m_speed + force / mass * dt;
    if (throttle <= 0 && (m_speed > 0) != (v > 0) && std::fabs(m_speed) > 0)
        v = 0; // brakes and resistance stop the car rather than reverse it
    if (p.limiter > 0 && v > p.limiter * 0.44704f)
        v = std::max(m_speed - 2.0f * dt, p.limiter * 0.44704f); // speed limiter cuts the drive
    m_speed = v;
}

void PlayerCar::Simulate(const Performance &p, float wheelRadius, float &zeroTo100, float &topSpeed)
{
    const RunTimes run = SimulateRun(p, wheelRadius);
    zeroTo100 = run.zeroTo100kmh;
    topSpeed = run.topSpeed;
}

PlayerCar::RunTimes PlayerCar::SimulateRun(const Performance &p, float wheelRadius)
{
    PlayerCar car;
    car.SetPerformance(p);
    car.m_wheelRadius = wheelRadius;
    RunTimes run;
    const float dt = 1.0f / 60.0f;
    for (int step = 0; step < 60 * 120; ++step) {
        float before = car.m_speed;
        car.UpdateEngine(1.0f, 0.0f, dt, 1.0f);
        const float t = (step + 1) * dt;
        if (!run.zeroTo100kmh && car.m_speed >= 100.0f / 3.6f)
            run.zeroTo100kmh = t;
        if (!run.zeroTo60mph && car.m_speed >= 60.0f * 0.44704f)
            run.zeroTo60mph = t;
        if (!run.zeroTo100mph && car.m_speed >= 100.0f * 0.44704f)
            run.zeroTo100mph = t;
        if (step > 600 && car.m_speed - before < 1e-4f)
            break;
    }
    run.topSpeed = car.m_speed;
    return run;
}

void PlayerCar::Place(float x, float y, float z, float yaw)
{
    m_x = x, m_y = y, m_z = z, m_yaw = yaw;
    m_speed = m_vz = m_pitch = m_roll = 0;
    if (m_ug2.Valid())
        m_ug2.Place(x, y, z + m_rideHeight, yaw);
    m_gear = 1;
    m_rpm = m_perf.idleRpm;
}

void PlayerCar::Update(const TrackWorld &world, float throttle, float brake, float steer, float dt, bool walls,
                       float power)
{
    constexpr float kAccel = 9.0f, kBrake = 18.0f, kReverseMax = -12.0f, kTopSpeed = 62.0f;
    constexpr float kDrag = 0.0009f, kRolling = 0.6f, kWheelbase = 2.6f, kGravity = 19.6f;

    if (m_ug2.Valid()) {
        UG2Physics::Controls in;
        in.throttle = std::clamp(throttle * power, 0.0f, 1.0f);
        in.brake = std::clamp(brake, 0.0f, 1.0f);
        in.steer = std::clamp(steer, -1.0f, 1.0f);
        in.handbrake = m_hold ? 1.0f : 0.0f;
        in.nitrous = m_nitrousButton;
        m_ug2.Update(world, in, dt);
        const UG2Physics::Vec3 o = m_ug2.Origin();
        m_x = o.x, m_y = o.y, m_z = o.z - m_rideHeight, m_yaw = m_ug2.Yaw();
        m_speed = m_ug2.ForwardSpeed();
        m_rpm = m_ug2.Rpm();
        m_gear = m_ug2.Gear();
        m_slip += (m_ug2.Slip() - m_slip) * std::min(1.0f, dt * 8.0f);
        (void)walls;
        return;
    }

    if (m_perf.Valid()) {
        UpdateEngine(throttle, brake, dt, power);
    } else {
        // Longitudinal (no database record): engine, brakes (reverse once stopped), drag, rolling.
        if (throttle > 0)
            m_speed += kAccel * power * throttle * (1.0f - std::max(0.0f, m_speed) / kTopSpeed) * dt;
        if (brake > 0)
            m_speed -= (m_speed > 0.5f ? kBrake : kAccel * 0.5f) * brake * dt;
        m_speed -= (kDrag * m_speed * std::fabs(m_speed) + (m_speed > 0 ? kRolling : m_speed < 0 ? -kRolling : 0)) * dt;
        if (throttle <= 0 && brake <= 0 && std::fabs(m_speed) < 0.3f)
            m_speed = 0;
        m_speed = std::clamp(m_speed, kReverseMax, kTopSpeed);
    }

    // Steering: bicycle model with less lock at speed.
    float lock = 0.55f / (1.0f + std::fabs(m_speed) * 0.05f);
    const float yawRate = m_speed / kWheelbase * std::tan(steer * lock);
    m_yaw += yawRate * dt;
    // Tyre slip for sound (0..1): cornering force against the grip limit, or hard braking at speed.
    if (dt > 0) {
        const float grip = (m_perf.Valid() ? m_perf.grip : 1.0f) * 9.81f;
        float slip = std::clamp((std::fabs(m_speed * yawRate) / grip - 0.55f) / 0.45f, 0.0f, 1.0f);
        if (brake > 0.5f && m_speed > 8.0f)
            slip = std::max(slip, std::min(1.0f, (m_speed - 8.0f) / 20.0f));
        m_slip += (slip - m_slip) * std::min(1.0f, dt * 8.0f);
    }

    float fx = std::cos(m_yaw), fy = std::sin(m_yaw);
    float nx = m_x + fx * m_speed * dt, ny = m_y + fy * m_speed * dt;

    // Walls: probe the move at 0.9 m against steep world triangles (lower kerbs and road-seam
    // skirts are stepped over by the ground follow).
    float probe = m_speed >= 0 ? 2.2f : -2.3f;
    if (walls && std::fabs(m_speed) > 0.01f &&
        world.HitsWall(m_x, m_y, m_z + 0.9f, nx + fx * probe, ny + fy * probe, m_z + 0.9f)) {
        m_speed = -m_speed * 0.2f;
        return;
    }
    // Ground: the highest surface not far above the car; a big step up counts as a wall.
    float ground;
    if (world.GroundHeight(nx, ny, m_z + 1.2f, ground)) {
        m_x = nx, m_y = ny;
        if (ground >= m_z - 0.05f || (m_vz < 0 && ground > m_z + m_vz * dt)) {
            m_z = ground;
            m_vz = 0;
        } else {
            m_vz -= kGravity * dt; // off a ledge: fall
            m_z = std::max(ground, m_z + m_vz * dt);
        }
    } else {
        // No surface: a hole left by the strip topology approximation (ISSUES.md #90), not the
        // edge of the world -- keep rolling at the current height.
        m_x = nx, m_y = ny;
    }
    // Body attitude from the surface under the axles.
    float hf, hr, hl, hrt;
    const float lx = -fy, ly = fx;
    if (world.GroundHeight(m_x + fx * 1.3f, m_y + fy * 1.3f, m_z + 1.2f, hf) &&
        world.GroundHeight(m_x - fx * 1.3f, m_y - fy * 1.3f, m_z + 1.2f, hr))
        m_pitch = std::atan2(hf - hr, 2.6f);
    if (world.GroundHeight(m_x + lx * 0.8f, m_y + ly * 0.8f, m_z + 1.2f, hl) &&
        world.GroundHeight(m_x - lx * 0.8f, m_y - ly * 0.8f, m_z + 1.2f, hrt))
        m_roll = std::atan2(hl - hrt, 1.6f);
}

void PlayerCar::Transform(float m[12]) const
{
    if (m_ug2.Valid()) {
        m_ug2.Transform(m);
        return;
    }
    const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float cr = std::cos(m_roll), sr = std::sin(m_roll);
    // forward (pitched up by m_pitch), left (rolled by m_roll), up = forward x left
    float f[3] = {cy * cp, sy * cp, sp};
    float l0[3] = {-sy, cy, 0};
    float u0[3] = {f[1] * l0[2] - f[2] * l0[1], f[2] * l0[0] - f[0] * l0[2], f[0] * l0[1] - f[1] * l0[0]};
    float l[3], u[3];
    for (int a = 0; a < 3; ++a) {
        l[a] = l0[a] * cr - u0[a] * sr;
        u[a] = u0[a] * cr + l0[a] * sr;
    }
    for (int a = 0; a < 3; ++a) {
        m[a] = f[a];
        m[4 + a] = l[a];
        m[8 + a] = u[a];
    }
    // m_z is the ground contact; the model origin sits RideHeight() above it.
    m[3] = m_x + u[0] * m_rideHeight, m[7] = m_y + u[1] * m_rideHeight, m[11] = m_z + u[2] * m_rideHeight;
}
