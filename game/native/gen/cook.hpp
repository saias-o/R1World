// One tile, cooked: every generator run over one tile's observations.
// Nothing here touches a disk or a network; the
// observations come in, the geometry and the manifest go out, and the same
// input always gives the same tile (PLAN §4 I3).
#pragma once

#include "mesh.hpp"
#include "osm.hpp"

#include <memory>

namespace r1 {

// §4 I4: the budget is a contract. A tile past it is refused, never truncated.
constexpr size_t kTileVertexBudget = 120000;

struct Observations {
    Tile tile;
    // Shared: one neighbourhood query is read once for its nine tiles.
    std::shared_ptr<const OsmData> osm;
    ElevationGrid elevations;
    std::string elevationSource;
    bool offline = false;  // Natural Earth's coast and flat ground, nothing surveyed
};

struct CookedTile {
    Tile tile;
    // The tile's own ground, streets, buildings and works: uploaded as meshes.
    std::vector<MeshPart> parts;
    // For a tile that is all ocean, the sea node that replaces the ground.
    nlohmann::json ocean;
    // Scene nodes on shared assets: landmarks, plants, props, boats, containers.
    nlohmann::json props = nlohmann::json::array();
    // What `ready.json` said: the frame, the collision data, what was inferred.
    nlohmann::json manifest;
    double cookMs = 0;
};

// Throws on a tile that exceeds the budget, naming its parts.
CookedTile cookTile(const Observations& in);

}  // namespace r1
