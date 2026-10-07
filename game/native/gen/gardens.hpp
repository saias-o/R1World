// Country/region priors for detached homes, and their inferred front gardens.
// No cadastral boundaries are available: every inferred enclosure is labelled
// as a prediction. Dense blocks, public/commercial uses and mapped barriers
// silence these predictions. Surveyed barriers render outside dense cores,
// independently of the country used for detached-home predictions.
#pragma once

#include "scatter.hpp"
#include "streets.hpp"

namespace r1 {
struct ResidentialHome {
    int64_t id = 0;
    const ResidentialStyle* style = nullptr;
    Ring ring;
    P2 centre, frontage, along, inward;
    double halfWidth = 0, front = 0, back = 0;
    bool gardenEligible = false;
};
struct ResidentialPlan {
    std::vector<ResidentialHome> homes;
    nlohmann::json stats;
};
ResidentialPlan planResidential(const OsmData& osm, const Anchor& anchor);
// Classify small untyped footprints beside rural bus stops before the building
// chain runs. Explicit shelters need no country prior; dense urban cooking
// keeps its existing shelter rendering. A power/service hut never becomes one.
nlohmann::json classifyBusShelters(std::vector<OsmWay>& buildings, const OsmData& osm,
                                  const Anchor& anchor, bool infer);

struct GardenOutput {
    std::vector<MeshPart> parts;
    nlohmann::json stats;
    // Inferred driveway strips: no vegetation may block a home's access.
    clip::Paths64 drives;
};
GardenOutput buildGardens(const OsmData& osm, const ResidentialPlan& homes,
                         const Tile& tile, const Anchor& anchor,
                         const ElevationGrid& elevations, const clip::Paths64& paved,
                         const clip::Paths64& water, size_t vertexBudget = 18000);
} // namespace r1
