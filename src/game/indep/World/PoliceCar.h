#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/indep/World/SolidMesh.h"

// The Israeli police car (ISSUES #318): a mesh converted from the user's own GTA V assets by tools/convert_police.py into the cache
// file police/police.pcar beside the game data ('PCR1': textures, triangles with their normals and the wheel hubs).  The assets are not
// part of the repository.
struct PoliceMesh {
    struct Tex {
        uint32_t hash = 0;
        int w = 0, h = 0;
        uint8_t gloss = 0;
        std::string name;
        std::vector<uint32_t> rgba;
    };
    std::vector<Tex> textures;
    std::vector<SolidMesh::Triangle> model;
    std::vector<uint32_t> triTex;
    std::vector<int8_t> wheelOf;
    std::vector<uint8_t> rims;
    std::vector<float> normals; // 9 per triangle
    float hubs[12] = {};        // FL FR RR RL, car space
    float radius = 0.335f, width = 0.235f;
    float bar[6] = {};          // lightbar bounding box (min xyz, max xyz)
    bool Load(const std::vector<uint8_t> &file);
};
