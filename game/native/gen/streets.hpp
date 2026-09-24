// Terrain-following streets, tagged sidewalks and surveyed zebra crossings.
// Widths without tags are estimates, never survey data, and the manifest
// counts which is which.
#pragma once

#include "mesh.hpp"
#include "osm.hpp"
#include "polygons.hpp"

namespace r1 {

// OSM `highway` values a car drives on.
bool isMotorway(const std::string& highway);
// A length tag in metres, or `fallback` when absent or absurd (0 < n <= 50).
double lengthTag(const std::string* value, double fallback);
double roadWidth(const Tags& tags);

struct StreetOutput {
    std::vector<MeshPart> parts;
    nlohmann::json stats;
};

// `roads` are already clipped to the tile; `footprints` are the buildings'
// rings in engine (x, z), which no street surface may cover.
StreetOutput buildStreets(const std::vector<OsmWay>& roads, const std::vector<OsmNode>& features,
                          const ElevationGrid& elevations, const Anchor& anchor,
                          const std::vector<Ring>& footprints);

}  // namespace r1
