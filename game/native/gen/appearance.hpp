// Choose the crowd's visual variety from the country containing a tile.
// The country map is bundled, so this also works before any online service.
#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace r1 {

struct CrowdAppearance {
    char sex = 'm';
    std::string skinTone = "light";
};

struct Country {
    std::string code, subregion;
};

class CountryCrowd {
public:
    void load(const nlohmann::json& geojson);
    Country at(double lon, double lat) const;
    size_t choose(const Country& country, const std::vector<CrowdAppearance>& avatars,
                  uint32_t seed, size_t slot) const;
private:
    struct Point { double lon, lat; };
    using Ring = std::vector<Point>;
    struct Polygon { std::vector<Ring> rings; double west=180, east=-180, south=90, north=-90; };
    struct Area { Country country; std::vector<Polygon> polygons; };
    std::vector<Area> areas_;
};

}  // namespace r1
