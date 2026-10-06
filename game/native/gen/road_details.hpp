#pragma once

#include "streets.hpp"

namespace r1 {
struct RoadAxis {
    int64_t id = 0;
    Tags tags;
    std::vector<P3> points;
    bool bridge = false;
};
struct RoadDetails {
    std::vector<MeshPart> parts;
    nlohmann::json stats;
    clip::Paths64 medians;
};
// Ground paint is clipped to actual asphalt and draped on its triangles.
// Raised paint uses the solved road profile. Rails yield to intersecting roads
// at the same height, never to a crossing underneath an overpass.
RoadDetails buildRoadDetails(const std::vector<RoadAxis>& roads, const clip::Paths64& tile,
                             const clip::Paths64& asphalt, const Drape* ground,
                             const std::string& country,
                             const std::vector<RoadAxis>& neighbours = {});
} // namespace r1
