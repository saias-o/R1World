#include "peaks.hpp"

#include "terrain.hpp"

#include <algorithm>
#include <cmath>

namespace r1 {
namespace {
// The summit's shape where the relief lost it: a dome whose crown has this
// radius of curvature (metres). It falls 1.7 m at 10 m, 15 m at 30 m, 60 m at
// 60 m: a rounded top steepening into the flanks the model did keep.
constexpr double kCrown = 30.0;
// Closer to the relief than this (metres), a summit is already there.
constexpr double kAgrees = 0.5;

bool inside(const Bounds& b, double lon, double lat) {
    return b.west <= lon && lon <= b.east && b.south <= lat && lat <= b.north;
}
}  // namespace

std::vector<PeakCell> peakCells(const Bounds& b) {
    const double mid = (b.south + b.north) / 2;
    const double dLat = kPeakReach / kMetresPerDegree;
    const double dLon = kPeakReach / (kMetresPerDegree * std::max(0.01, std::cos(radians(mid))));
    const int south = int(std::floor(std::max(-90.0, b.south - dLat)));
    const int north = int(std::floor(std::min(89.999999, b.north + dLat)));
    const int west = int(std::floor(std::max(-180.0, b.west - dLon)));
    const int east = int(std::floor(std::min(179.999999, b.east + dLon)));
    std::vector<PeakCell> cells;
    for (int lat = south; lat <= north; ++lat)
        for (int lon = west; lon <= east; ++lon) cells.push_back({lat, lon});
    return cells;
}

std::optional<double> parseElevation(const std::string& text) {
    size_t at = 0;
    while (at < text.size() && text[at] == ' ') ++at;
    const size_t start = at;
    if (at < text.size() && (text[at] == '-' || text[at] == '+')) ++at;
    bool digits = false, point = false;
    for (; at < text.size(); ++at) {
        if (std::isdigit((unsigned char)text[at])) digits = true;
        else if (text[at] == '.' && !point) point = true;
        else break;
    }
    if (!digits) return std::nullopt;
    const double value = std::stod(text.substr(start, at - start));
    while (at < text.size() && text[at] == ' ') ++at;
    if (at < text.size() && text[at] == 'm') ++at;
    while (at < text.size() && text[at] == ' ') ++at;
    // Nothing may follow: "6234 ft", "1200-1300" and "ca. 900" are not
    // heights in metres.
    if (at != text.size() || !std::isfinite(value) || value < -500.0 || value > 9000.0) return std::nullopt;
    return value;
}

std::vector<Peak> peaksFromFeatures(const std::vector<OsmNode>& features, int* refused) {
    std::vector<Peak> peaks;
    int doubted = 0;
    for (const OsmNode& n : features) {
        const std::string natural = tagOr(n.tags, "natural");
        if ((natural != "peak" && natural != "volcano") || !has(n.tags, "ele")) continue;
        const auto ele = parseElevation(n.tags.at("ele"));
        if (!ele) { ++doubted; continue; }
        peaks.push_back({n.id, n.lon, n.lat, *ele, tagOr(n.tags, "name")});
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.id < b.id; });
    if (refused) *refused = doubted;
    return peaks;
}

std::vector<Peak> peaksFromDocument(const nlohmann::json& document) {
    std::vector<Peak> peaks;
    for (const auto& r : document.at("peaks"))
        peaks.push_back({r.at(0).get<int64_t>(), r.at(1).get<double>(), r.at(2).get<double>(), r.at(3).get<double>(),
                         r.at(4).get<std::string>()});
    return peaks;
}

ElevationGrid raiseToPeaks(const ElevationGrid& grid, const std::vector<Peak>& peaks,
                           const std::vector<const ElevationGrid*>& known, nlohmann::json* report) {
    const Bounds& b = grid.bounds;
    const int n = kTerrainMeshSize;
    ElevationGrid out{b, n, {}};
    out.southEdge = grid.southEdge; out.northEdge = grid.northEdge;
    out.values.reserve(size_t(n) * size_t(n));
    for (int row = 0; row < n; ++row)
        for (int col = 0; col < n; ++col)
            out.values.push_back(grid.sample(b.west + (b.east - b.west) * col / (n - 1),
                                             b.south + (b.north - b.south) * row / (n - 1)));
    nlohmann::json raised = nlohmann::json::array(), doubted = nlohmann::json::array();
    int agreed = 0, unread = 0;
    const double dLat = kPeakReach / kMetresPerDegree;
    const std::vector<double> base = out.values;
    for (const Peak& p : peaks) {
        const double metresLon = kMetresPerDegree * std::cos(radians(p.lat));
        const double dLon = kPeakReach / std::max(1.0, metresLon);
        if (p.lat < b.south - dLat || p.lat > b.north + dLat || p.lon < b.west - dLon || p.lon > b.east + dLon) continue;
        // The relief under the summit, on whichever grid holds it.
        std::optional<double> relief;
        for (const ElevationGrid* g : known)
            if (g && g->values.size() == size_t(g->size) * size_t(g->size) && inside(g->bounds, p.lon, p.lat)) {
                relief = g->sample(p.lon, p.lat);
                break;
            }
        const bool own = inside(b, p.lon, p.lat);
        if (relief && p.ele - *relief < kAgrees) { if (own) ++agreed; continue; }
        if (relief && p.ele - *relief > kPeakMaxRaise) {
            if (own) doubted.push_back({{"id", p.id}, {"name", p.name}, {"ele", p.ele}, {"relief", *relief}});
            continue;
        }
        if (!relief && own) ++unread;
        for (int row = 0; row < n; ++row) {
            const double lat = b.south + (b.north - b.south) * row / (n - 1);
            const double dy = (lat - p.lat) * kMetresPerDegree;
            if (std::abs(dy) >= kPeakReach) continue;
            for (int col = 0; col < n; ++col) {
                const double lon = b.west + (b.east - b.west) * col / (n - 1);
                const double dx = (lon - p.lon) * metresLon;
                const double d2 = dx * dx + dy * dy;
                if (d2 >= kPeakReach * kPeakReach) continue;
                const size_t i = size_t(row) * size_t(n) + size_t(col);
                const double dome = p.ele - d2 / (2.0 * kCrown);
                const double s2 = d2 / (kPeakReach * kPeakReach);
                const double fade = (1.0 - s2) * (1.0 - s2);
                const double lift = std::min(kPeakMaxRaise, std::max(0.0, dome - base[i]) * fade);
                if (base[i] + lift > out.values[i]) out.values[i] = base[i] + lift;
            }
        }
        for (auto* edge : {&out.southEdge, &out.northEdge}) {
            const double lat = edge == &out.southEdge ? b.south : b.north;
            const auto& source = edge == &out.southEdge ? grid.southEdge : grid.northEdge;
            for (size_t i = 0; i < edge->size(); ++i) {
                const double dx = ((*edge)[i].x - p.lon) * metresLon, dy = (lat - p.lat) * kMetresPerDegree;
                const double d2 = dx * dx + dy * dy;
                if (d2 >= kPeakReach * kPeakReach) continue;
                const double dome = p.ele - d2 / (2.0 * kCrown), s2 = d2 / (kPeakReach * kPeakReach);
                const double lift = std::min(kPeakMaxRaise, std::max(0.0, dome - source[i].y) * (1 - s2) * (1 - s2));
                (*edge)[i].y = std::max((*edge)[i].y, source[i].y + lift);
            }
        }
        if (own && relief) raised.push_back({{"id", p.id}, {"name", p.name}, {"ele", p.ele}, {"relief", *relief},
                                             {"raisedBy", p.ele - *relief}});
    }
    if (report)
        *report = {{"source", "OpenStreetMap natural=peak|volcano, ele"}, {"raised", raised}, {"agreed", agreed},
                   {"doubted", doubted}, {"reliefUnread", unread}};
    return out;
}

}  // namespace r1
