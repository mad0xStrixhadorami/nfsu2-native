#pragma once
// Photographic material textures (CC0, ambientCG; assets/israel, ISSUES #274) for the world shader.
namespace MaterialLibrary {
constexpr int kColours = 8; // asphalt, pavers (grey), pavers (cream), Jerusalem stone, stone cladding, plaster, concrete, stucco
constexpr int kNormals = 5; // asphalt, pavers, Jerusalem stone, concrete, plaster
// Loads every texture once (GL context current); false when the files are missing.
bool Load();
unsigned Colour(int i);
unsigned Normal(int i);
} // namespace MaterialLibrary
