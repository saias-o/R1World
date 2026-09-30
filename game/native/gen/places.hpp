#pragma once

#include <string>
#include <vector>

namespace r1 {

struct PlaceChoice {
    std::string name;
    std::string label;
    double lon = 0;
    double lat = 0;
};

// Photon returns ranked GeoJSON results. Keep only finite point locations and
// enough context to distinguish communes with the same name.
std::vector<PlaceChoice> parsePlaceChoices(const std::string& body);
std::vector<PlaceChoice> searchPlaceChoices(const std::string& query);

}  // namespace r1
