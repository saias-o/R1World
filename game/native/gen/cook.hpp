// One tile, cooked: every generator run over one tile's observations.
// Nothing here touches a disk or a network; the
// observations come in, the geometry and the manifest go out, and the same
// input always gives the same tile (PLAN §3 I3).
#pragma once

#include "canopy.hpp"
#include "mesh.hpp"
#include "osm.hpp"
#include "peaks.hpp"
#include "seaice.hpp"
#include "../minimap.hpp"

#include <memory>

namespace r1 {

// §3 I4: the budget is a contract. A tile past it is refused, never truncated.
constexpr size_t kTileVertexBudget = 120000;

struct Observations {
    Tile tile;
    // Shared: one neighbourhood query is read once for its nine tiles.
    std::shared_ptr<const OsmData> osm;
    // The box the OSM answer was asked about, when known: past it, nothing
    // was observed, which is not the same as nothing being there.
    std::optional<Bounds> osmExtent;
    ElevationGrid elevations;
    // The relief of the tiles around, those already on disk: a bridge or its
    // ramps reaching past the tile are solved on the same ground from both
    // sides (gen/bridges.hpp). Missing ones fall back to this tile's own.
    std::vector<ElevationGrid> around;
    std::string elevationSource;
    bool offline = false;  // OSM missing: use Natural Earth's approximate coast
    bool groundPending = false;  // measured relief is still on its way
    size_t targetVertices = kTileVertexBudget;  // softer LOD target from the resident arena
    // Cooked from an answer older than the aero layer, which is on its way:
    // the manifest says so, and the service cooks the tile again when it lands.
    bool airportsPending = false;
    // Cooked before its OSM answer came. The available relief and Natural
    // Earth's coast appear first; streets follow as soon as OSM arrives.
    bool provisional = false;
    // The sea ice around a polar tile with sea-level ground; null elsewhere.
    std::shared_ptr<const SeaIce> seaIce;
    // The measured canopy (gen/canopy), when converted; on its way otherwise.
    std::optional<Canopy> canopy;
    bool canopyPending = false;
    // The surveyed summits around the tile (gen/peaks): the relief is raised
    // to them before anything stands on it. `peaksSource` says what answered
    // (ObservationStore::peaks).
    std::vector<Peak> peaks;
    std::string peaksSource;
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
    // Small display-only road/place extract from the same observations as the
    // geometry. It is transient, never written into the deterministic manifest.
    MiniMapTile minimap;
    // What the ice was read from, for what the game draws past the tiles.
    std::shared_ptr<const SeaIce> seaIce;
    double cookMs = 0;
};

// Throws on a tile that exceeds the budget, naming its parts.
CookedTile cookTile(const Observations& in);

}  // namespace r1
