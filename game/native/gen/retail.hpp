#pragma once
#include "interiors.hpp"
#include "streets.hpp"
namespace r1 {
// Copy observations before enrichment; never mutate a shared OSM answer.
std::vector<OsmWay> retailBuildings(const std::vector<const OsmWay*>& ways,
                                  const OsmData& osm,const std::function<P3(double,double)>& ground);
struct ParkingOutput { std::vector<MeshPart> parts; nlohmann::json manifest=nlohmann::json::array(); };
ParkingOutput buildRetailParking(const OsmData& osm,const std::vector<InteriorPlan>& shops,
    const std::vector<Ring>& footprints,const ElevationGrid& elevations,const Anchor& anchor);
} // namespace r1
