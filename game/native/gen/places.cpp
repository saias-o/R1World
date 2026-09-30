#include "places.hpp"

#include "net.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace r1 {

std::vector<PlaceChoice> parsePlaceChoices(const std::string& body) {
    const auto doc = nlohmann::json::parse(body);
    if (!doc.is_object() || !doc.contains("features") || !doc["features"].is_array())
        throw std::runtime_error("place search returned no feature collection");
    std::vector<PlaceChoice> choices;
    for (const auto& feature : doc["features"]) {
        if (!feature.is_object() || !feature.contains("properties") || !feature["properties"].is_object() ||
            !feature.contains("geometry") || !feature["geometry"].is_object()) continue;
        const auto& properties = feature["properties"];
        const auto& geometry = feature["geometry"];
        if (geometry.value("type", std::string()) != "Point" || !geometry.contains("coordinates") ||
            !geometry["coordinates"].is_array() || geometry["coordinates"].size() < 2 ||
            !geometry["coordinates"][0].is_number() || !geometry["coordinates"][1].is_number()) continue;
        const double lon = geometry["coordinates"][0].get<double>();
        const double lat = geometry["coordinates"][1].get<double>();
        if (!std::isfinite(lon) || !std::isfinite(lat) || lon < -180 || lon > 180 || lat < -90 || lat > 90) continue;
        auto stringProperty = [&](const char* key) -> std::string {
            auto it = properties.find(key);
            return it != properties.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        const std::string name = stringProperty("name");
        if (name.empty()) continue;
        const std::string county = stringProperty("county");
        const std::string state = stringProperty("state");
        const std::string country = stringProperty("country");
        std::string label = name;
        if (!county.empty() && county != name) label += " · " + county;
        else if (!state.empty() && state != name) label += " · " + state;
        if (!country.empty() && country != name) label += " · " + country;
        bool duplicate = false;
        for (const auto& previous : choices)
            if (previous.label == label) duplicate = true;
        if (!duplicate) choices.push_back({name, label, lon, lat});
        if (choices.size() == 5) break;
    }
    return choices;
}

std::vector<PlaceChoice> searchPlaceChoices(const std::string& query) {
    // The public Photon instance supports search-as-you-type. Debouncing and
    // one in-flight request are enforced by the menu, never by this worker.
    const std::string url = "https://photon.komoot.io/api/?q=" + net::urlEncode(query) +
                            "&lang=fr&limit=10&layer=city&layer=locality";
    const net::Response response = net::request("GET", url, {}, {}, 8.0);
    if (response.status < 200 || response.status >= 300)
        throw std::runtime_error("place search HTTP " + std::to_string(response.status));
    return parsePlaceChoices(response.body);
}

}  // namespace r1
