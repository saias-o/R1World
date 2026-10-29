#pragma once

#include "gen/osm.hpp"

#include <map>
#include <string>
#include <vector>

namespace r1 {

struct MiniRoad {
    P2 a, b;
    std::string name;
    int rank = 0;
};

struct MiniMapTile {
    Bounds bounds;
    std::vector<MiniRoad> roads;
    std::map<std::string, int> cities;
    std::map<std::string, int> countries;
};

// Called on the tile cooker thread, from the full observed road polylines.
MiniMapTile makeMiniMapTile(const std::vector<OsmWay>& roads, const OsmData& osm, const Bounds& bounds);

// North-up. View dimensions are shared with live markers in minimap_view.hpp.
std::string miniMapRoadRml(const std::vector<const MiniMapTile*>& tiles, double lon, double lat);
std::string miniMapPlace(const std::vector<const MiniMapTile*>& tiles,
                         const std::map<std::string, std::string>& zoneCountries,
                         const std::map<std::string, std::string>& countryNames,
                         const std::string& timezone, double lon, double lat);

}  // namespace r1
