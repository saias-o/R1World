// Where grass grows on a tile, for the engine's grass blades (saida::GrassNode).
//
// The blades stand on the ground as drawn -- the tile's grid, triangle for
// triangle -- and grow where that ground is drawn as a grassy family, in that
// family's measured colour, so that where they thin out with distance the
// ground under them is the same colour (CLAUDE.md rule 2). Nothing grows on
// what is laid over the ground: streets and pavements, car parks, quays,
// runways, and nothing inside a building.
#pragma once

#include "palette.hpp"
#include "polygons.hpp"
#include "terrain.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace r1 {

// Texels a side of a tile's cover: about a metre each on a 0.005 degree tile.
constexpr int kGrassCoverSize = 512;
// Metres above the ground under which a surface is laid on it, not over it:
// a pavement is, a bridge deck is not.
constexpr double kLaidHeight = 1.0;

struct GrassCover {
    int groundSamples = 0;          // kTerrainMeshSize
    std::vector<float> heights;     // engine y of the drawn grid, row (south to north) major
    // (u, v) of an engine (x, z): u = a x + b z + c, v = d x + e z + f, with
    // u along the grid's columns and v along its rows, both 0 to 1.
    std::array<double, 6> uvFromEngine{};
    int coverSize = 0;
    std::vector<uint32_t> cover;    // RGBA8, r the low byte: sRGB colour, density
    bool empty() const { return cover.empty(); }
};

// The share of the ground a family grows blades over, 0 for none.
double grassDensity(const std::string& family);

// The cover of a tile drawn as `grid` (buildTerrain), its triangles' classes
// resolved to families by `familyOf` and to colours by `colourOf`. Nothing
// grows under `laid` -- the tile's other parts, wherever one of their faces
// looks up within kLaidHeight of the ground -- nor inside `footprints`
// (engine x, z). Empty where nothing grows at all.
GrassCover grassCover(const TerrainGrid& grid,
                      const std::function<std::optional<std::string>(const std::string&)>& familyOf,
                      const std::function<std::array<double, 3>(const std::string&)>& colourOf,
                      const std::vector<const MeshPart*>& laid, const std::vector<Ring>& footprints);

}  // namespace r1
