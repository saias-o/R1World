#include "minimap.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace r1 {
namespace {
constexpr double kEarthMetres = 6371008.8;
constexpr double kMapWidth = 296.0, kMapHeight = 256.0;
constexpr double kViewWidthMetres = 900.0, kViewHeightMetres = 900.0 * kMapHeight / kMapWidth;
constexpr size_t kRoadBudget = 240;
constexpr size_t kLabelBudget = 4;

bool inside(P2 p, const Bounds& b) {
    return p.x >= b.west && p.x <= b.east && p.y >= b.south && p.y <= b.north;
}

int roadRank(const Tags& tags) {
    const std::string kind = tagOr(tags, "highway");
    if (kind == "motorway" || kind == "trunk" || kind == "primary") return 3;
    if (kind == "secondary" || kind == "tertiary") return 2;
    if (kind == "residential" || kind == "living_street" || kind == "unclassified") return 1;
    return 0;
}

std::string escapeRml(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

double wrappedDegrees(double delta) {
    while (delta > 180.0) delta -= 360.0;
    while (delta < -180.0) delta += 360.0;
    return delta;
}

struct DrawRoad {
    double x0, y0, x1, y1, distance;
    int rank;
    std::string name;
};

bool clip(double& x0, double& y0, double& x1, double& y1) {
    const double dx = x1 - x0, dy = y1 - y0;
    double lo = 0.0, hi = 1.0;
    const double pq[4][2] = {{-dx, x0}, {dx, kMapWidth - x0}, {-dy, y0}, {dy, kMapHeight - y0}};
    for (const auto& pair : pq) {
        const double p = pair[0], q = pair[1];
        if (std::abs(p) < 1e-12) { if (q < 0.0) return false; }
        else if (p < 0.0) lo = std::max(lo, q / p);
        else hi = std::min(hi, q / p);
    }
    if (lo >= hi) return false;
    x1 = x0 + hi * dx; y1 = y0 + hi * dy;
    x0 += lo * dx; y0 += lo * dy;
    return true;
}

std::string popular(const std::vector<const MiniMapTile*>& tiles, bool city, double lon, double lat) {
    std::map<std::string, int> votes;
    auto countVotes = [&](bool currentOnly) {
        for (const MiniMapTile* tile : tiles) {
            if (currentOnly && !inside({lon, lat}, tile->bounds)) continue;
            for (const auto& [name, n] : city ? tile->cities : tile->countries)
                votes[name] += n;
        }
    };
    countVotes(true);
    if (votes.empty()) countVotes(false);
    std::string best;
    int count = 0;
    for (const auto& [name, n] : votes)
        if (n > count) { best = name; count = n; }
    return best;
}
}  // namespace

MiniMapTile makeMiniMapTile(const std::vector<OsmWay>& roads, const OsmData& osm, const Bounds& bounds) {
    MiniMapTile out;
    out.bounds = bounds;
    out.roads.reserve(roads.size());
    for (const OsmWay& road : roads) {
        if (road.points.size() != 2) continue;
        out.roads.push_back({road.points[0], road.points[1], tagOr(road.tags, "name"), roadRank(road.tags)});
    }
    auto collect = [&](P2 position, const Tags& tags) {
        if (!inside(position, bounds)) return;
        if (auto* city = tag(tags, "addr:city")) ++out.cities[*city];
        if (auto* country = tag(tags, "addr:country")) ++out.countries[*country];
    };
    for (const OsmWay& way : osm.buildings)
        if (!way.points.empty()) collect(way.points.front(), way.tags);
    for (const OsmNode& feature : osm.features) collect({feature.lon, feature.lat}, feature.tags);
    return out;
}

std::string miniMapRoadRml(const std::vector<const MiniMapTile*>& tiles, double lon, double lat) {
    std::vector<DrawRoad> candidates;
    const double cosLat = std::max(0.00001, std::cos(lat * 3.141592653589793 / 180.0));
    for (const MiniMapTile* tile : tiles) for (const MiniRoad& road : tile->roads) {
        double x0 = kMapWidth * (0.5 + kEarthMetres * cosLat * wrappedDegrees(road.a.x - lon)
                                         * 3.141592653589793 / 180.0 / kViewWidthMetres);
        double y0 = kMapHeight * (0.5 - kEarthMetres * (road.a.y - lat)
                                         * 3.141592653589793 / 180.0 / kViewHeightMetres);
        double x1 = kMapWidth * (0.5 + kEarthMetres * cosLat * wrappedDegrees(road.b.x - lon)
                                         * 3.141592653589793 / 180.0 / kViewWidthMetres);
        double y1 = kMapHeight * (0.5 - kEarthMetres * (road.b.y - lat)
                                         * 3.141592653589793 / 180.0 / kViewHeightMetres);
        if (!clip(x0, y0, x1, y1) || std::hypot(x1 - x0, y1 - y0) < 1.0) continue;
        const double distance = std::hypot((x0 + x1) * 0.5 - kMapWidth * 0.5,
                                           (y0 + y1) * 0.5 - kMapHeight * 0.5);
        candidates.push_back({x0, y0, x1, y1, distance, road.rank, road.name});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const DrawRoad& a, const DrawRoad& b) {
        const double sa = a.distance - a.rank * 24.0, sb = b.distance - b.rank * 24.0;
        return sa < sb;
    });
    if (candidates.size() > kRoadBudget) candidates.resize(kRoadBudget);
    std::stable_sort(candidates.begin(), candidates.end(), [](const DrawRoad& a, const DrawRoad& b) {
        return a.rank < b.rank;
    });
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    for (const DrawRoad& road : candidates) {
        const double length = std::hypot(road.x1 - road.x0, road.y1 - road.y0);
        const double thickness = road.rank == 3 ? 5.0 : road.rank == 2 ? 3.5 : road.rank == 1 ? 2.5 : 1.5;
        const double degrees = std::atan2(road.y1 - road.y0, road.x1 - road.x0) * 180.0 / 3.141592653589793;
        out << "<div class='mini-road rank-" << road.rank << "' style='left:"
            << (road.x0 + road.x1 - length) * 0.5 << "px;top:"
            << (road.y0 + road.y1 - thickness) * 0.5 << "px;width:"
            << length << "px;height:" << thickness << "px;transform:rotate(" << degrees << "deg);'></div>";
    }
    std::set<std::string> seen;
    std::vector<P2> labels;
    std::stable_sort(candidates.begin(), candidates.end(), [](const DrawRoad& a, const DrawRoad& b) {
        return a.distance - a.rank * 15.0 < b.distance - b.rank * 15.0;
    });
    for (const DrawRoad& road : candidates) {
        if (road.name.empty() || std::hypot(road.x1 - road.x0, road.y1 - road.y0) < 18.0 || !seen.insert(road.name).second) continue;
        const P2 at{(road.x0 + road.x1) * 0.5, (road.y0 + road.y1) * 0.5};
        if (at.x < 55 || at.x > kMapWidth - 55 || at.y < 15 || at.y > kMapHeight - 15) continue;
        bool overlap = false;
        for (const P2& other : labels) overlap |= std::hypot(at.x - other.x, at.y - other.y) < 78.0;
        if (overlap) continue;
        out << "<div class='mini-street' style='left:" << at.x - 55.0 << "px;top:"
            << at.y - 8.0 << "px;'>" << escapeRml(road.name) << "</div>";
        labels.push_back(at);
        if (labels.size() == kLabelBudget) break;
    }
    return out.str();
}

std::string miniMapPlace(const std::vector<const MiniMapTile*>& tiles,
                         const std::map<std::string, std::string>& zoneCountries,
                         const std::map<std::string, std::string>& countryNames,
                         const std::string& timezone, double lon, double lat) {
    std::string city = popular(tiles, true, lon, lat);
    std::string code = popular(tiles, false, lon, lat);
    if (code.empty()) {
        auto found = zoneCountries.find(timezone);
        if (found != zoneCountries.end()) code = found->second;
    }
    std::string country;
    auto found = countryNames.find(code);
    if (found != countryNames.end()) country = found->second;
    else if (!code.empty()) country = code;
    return (city.empty() ? "Ville non renseignée" : city) + " · " +
           (country.empty() ? "Pays non renseigné" : country);
}

}  // namespace r1
