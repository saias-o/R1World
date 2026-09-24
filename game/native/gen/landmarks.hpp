// The twenty places that must look like themselves.
//
// Their models are drawn by recipe (`tools/r1/landmark_recipes.py`), baked at
// authoring time into `assets/world/landmarks/` with the list that describes
// them, and shipped with the game like any other asset: the game only places
// them. Anchor and bearing are measured on the landmark's own OSM element, the
// height is the official one, and the manifest says which is which (§4 I5).
#pragma once

#include "polygons.hpp"
#include "osm.hpp"

#include <functional>

namespace r1 {

constexpr int kLandmarkRevision = 3;

struct LandmarkLevel { std::string path; double until = 0; size_t vertices = 0; };

struct Landmark {
    std::string slug, name, wikidata, osm, bearingSource;
    double lon = 0, lat = 0, bearing = 0, height = 0, groundAlt = 0;
    std::string groundSource;
    std::vector<Ring> clearance, solids;  // recipe frame (x, z)
    // Level 0 is the full model its tile carries; 1 and 2 are seen from afar.
    std::vector<LandmarkLevel> levels;
};

// Every landmark, from `assets/world/landmarks/landmarks.json`.
const std::vector<Landmark>& landmarks();
double landmarkFarRange();

struct LandmarkPlacement {
    std::vector<const OsmWay*> kept;  // the tile's buildings minus the landmarks' traces
    nlohmann::json nodes = nlohmann::json::array();
    std::vector<Ring> solids;
    size_t vertices = 0;
    nlohmann::json manifest = nlohmann::json::array();
    nlohmann::json replaced = nlohmann::json::array();
};

LandmarkPlacement placeLandmarks(const Bounds& bounds, const std::function<P3(double, double)>& ground,
                                 const std::vector<const OsmWay*>& buildings);

}  // namespace r1
