// Geographical placement of low vegetation; shared photographed assets stay
// scene instances, never unique geometry in a tile's vertex arena.
#pragma once
#include "scatter.hpp"

namespace r1 {
Scatter planUnderstory(const OsmData& osm, const Tile& tile, const Anchor& anchor,
                      const GroundAt& ground, const std::vector<OsmWay>& roadside,
                      const clip::Paths64& driveways, int budget = 512,
                      const std::function<bool(P2)>& permitted = {});
struct GrassCover;
void maskUnderstoryGrass(GrassCover& cover, const nlohmann::json& plants);
}
