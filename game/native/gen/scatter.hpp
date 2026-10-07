// What stands on the ground without being tile geometry: street furniture,
// vegetation, and the lane graph traffic drives on. Props and plants are
// scene nodes, never tile geometry: the engine's MeshCache indexes by asset,
// so six hundred nodes pointing at one bench upload it once (CLAUDE.md §5).
#pragma once

#include "canopy.hpp"
#include "palette.hpp"

#include <functional>

namespace r1 {

using GroundAt = std::function<P3(double, double)>;
using Segment2 = std::pair<P2, P2>;

struct Scatter {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json stats;
};

// Tagged OSM points turned into placed, oriented, scaled instances. Nothing is
// invented: every prop stands on a point somebody surveyed.
Scatter planProps(const std::vector<const OsmNode*>& features, const GroundAt& ground,
                  const RegionProfile& profile, const std::vector<Segment2>& roads, int budget = 260);

// Survey-first vegetation, plus inferred temperate rural roadside planting,
// bounded by OSM obstacles, tile ownership and a per-tile budget. With the
// measured canopy (gen/canopy), every inferred tree must stand where the
// canopy says a tree stands, and each tree cell the survey left empty gets
// one, at the canopy's measured height; low cells get shrubs.
Scatter planNature(const OsmData& osm, const Tile& tile, const Anchor& anchor, const GroundAt& ground,
                   int budget = 320, const Canopy* canopy = nullptr,
                   const std::vector<OsmWay>* roadside = nullptr);

// The tile's lane graph in engine metres, and how busy it should be.
nlohmann::json buildLaneGraph(const std::vector<OsmWay>& roads, const GroundAt& ground, int buildings,
                              double lon, double lat);

}  // namespace r1
