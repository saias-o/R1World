#include "appearance.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace r1 {
namespace {

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

bool subSaharan(const std::string& region) {
    return region == "Western Africa" || region == "Middle Africa" ||
           region == "Eastern Africa" || region == "Southern Africa";
}

// Approximate visual mix for gameplay, not a population census. There is no
// exclusive appearance assigned to a nationality: every band keeps variety.
std::array<int,3> weights(const Country& country) {
    const std::string& r = country.subregion;
    if (subSaharan(r))
        return {1, 2, 97};  // light, medium, dark
    if (r == "Northern Africa") return {10, 60, 30};
    if (r == "Southern Asia") return {12, 63, 25};
    if (r == "Eastern Asia" || r == "South-eastern Asia") return {42, 48, 10};
    if (r == "Western Asia" || r == "Central Asia") return {20, 62, 18};
    if (r == "Northern America") return {50, 25, 25};
    if (r == "Central America" || r == "South America" || r == "Caribbean") return {28, 47, 25};
    if (r == "Melanesia" || r == "Micronesia" || r == "Polynesia") return {30, 35, 35};
    if (r == "Australia and New Zealand") return {55, 25, 20};
    if (r == "Northern Europe" || r == "Western Europe" || r == "Southern Europe" || r == "Eastern Europe")
        return {65, 20, 15};
    return {35, 35, 30};
}

}  // namespace

void CountryCrowd::load(const nlohmann::json& geojson) {
    areas_.clear();
    for (const auto& feature : geojson.at("features")) {
        const auto& properties = feature.at("properties");
        Area area;
        area.country.code = properties.value("ISO_A2_EH", std::string());
        area.country.subregion = properties.value("SUBREGION", std::string());
        const auto& geometry = feature.at("geometry");
        if (geometry.is_null()) continue;
        const std::string type = geometry.at("type").get<std::string>();
        auto add = [&](const nlohmann::json& raw) {
            Polygon polygon;
            for (const auto& sourceRing : raw) {
                Ring ring;
                ring.reserve(sourceRing.size());
                for (const auto& point : sourceRing) {
                    const double lon = point.at(0).get<double>(), lat = point.at(1).get<double>();
                    ring.push_back({lon,lat});
                    polygon.west = std::min(polygon.west, lon);
                    polygon.east = std::max(polygon.east, lon);
                    polygon.south = std::min(polygon.south, lat);
                    polygon.north = std::max(polygon.north, lat);
                }
                if (ring.size() >= 3) polygon.rings.push_back(std::move(ring));
            }
            if (!polygon.rings.empty()) area.polygons.push_back(std::move(polygon));
        };
        if (type == "Polygon") add(geometry.at("coordinates"));
        else if (type == "MultiPolygon")
            for (const auto& polygon : geometry.at("coordinates")) add(polygon);
        if (!area.polygons.empty()) areas_.push_back(std::move(area));
    }
    if (areas_.empty()) throw std::runtime_error("Country crowd map has no polygons");
}

Country CountryCrowd::at(double lon, double lat) const {
    for (const Area& area : areas_) for (const Polygon& polygon : area.polygons) {
        if (lon < polygon.west || lon > polygon.east || lat < polygon.south || lat > polygon.north) continue;
        auto contains = [&](const Ring& source) {
            bool inside = false;
            for (size_t i=0, j=source.size()-1; i<source.size(); j=i++) {
                const Point& a=source[i], &b=source[j];
                if ((a.lat>lat)!=(b.lat>lat) &&
                    lon < (b.lon-a.lon)*(lat-a.lat)/(b.lat-a.lat)+a.lon) inside=!inside;
            }
            return inside;
        };
        if (!contains(polygon.rings.front())) continue;
        bool hole = false;
        for (size_t i=1; i<polygon.rings.size(); ++i) if (contains(polygon.rings[i])) { hole=true; break; }
        if (!hole) return area.country;
    }
    return {};
}

size_t CountryCrowd::choose(const Country& country, const std::vector<CrowdAppearance>& avatars,
                            uint32_t seed, size_t slot) const {
    if (avatars.empty()) throw std::invalid_argument("No crowd avatars");
    const uint64_t choice = mix((uint64_t(seed)<<32) ^ uint64_t(slot));
    const char sex = (choice & 1) ? 'f' : 'm';
    const auto w = weights(country);
    const int roll = int(mix(choice) % 100);
    const char* tone = roll < w[0] ? "light" : roll < w[0]+w[1] ? "medium" : "dark";
    if (subSaharan(country.subregion)) {
        // A small street scene uses the first few walker slots. Give every
        // tile a mostly dark-skinned crowd even when only a handful appear:
        // one lighter slot per 32, never among the first twelve.
        const size_t rareSlot=12+size_t(mix(seed)%20);
        tone = slot%32==rareSlot ? (mix(choice+2)%3==0 ? "light" : "medium") : "dark";
    }
    std::vector<size_t> matches;
    for (size_t i=0; i<avatars.size(); ++i)
        if (avatars[i].sex == sex && avatars[i].skinTone == tone) matches.push_back(i);
    // Keep the intended gender if that band has no suitable scan yet.
    if (matches.empty()) for (size_t i=0; i<avatars.size(); ++i)
        if (avatars[i].sex == sex) matches.push_back(i);
    if (matches.empty()) for (size_t i=0; i<avatars.size(); ++i) matches.push_back(i);
    return matches[size_t(mix(choice+1) % matches.size())];
}

}  // namespace r1
