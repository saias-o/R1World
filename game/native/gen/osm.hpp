// OSM observations and the elevation grid a tile is built from.
#pragma once

#include "common.hpp"

#include <nlohmann/json.hpp>

namespace r1 {

struct OsmWay {
    int64_t id = 0;
    std::vector<P2> points;  // (lon, lat)
    Tags tags;
    bool closed() const { return points.size() >= 4 && points.front() == points.back(); }
};

struct OsmNode {
    int64_t id = 0;
    double lon = 0, lat = 0;
    Tags tags;
};

// The classified ways of one Overpass answer. Every list is sorted by id, so
// what a tile keeps never depends on the order Overpass happened to send.
struct OsmData {
    std::vector<OsmWay> buildings, roads, vegetation, waterways, landcover, treeRows, coastlines, maritime;
    // Everything `aeroway` (runways, taxiways, aprons, helipads, stands) and
    // the closed ways that say an area is military (gen/airports.cpp).
    std::vector<OsmWay> aeroways, military;
    std::vector<P2> trees;
    std::vector<OsmNode> features;
    // ISO 3166-1 alpha-2 of the country the neighbourhood's centre is in, as
    // OSM's own boundaries say; empty in an answer older than question 7.
    std::string country;
    int queryVersion = 1;
    bool retailQueried = false;
    bool fuelQueried = false;
    bool interiorUsesQueried = false;
};

// `layer`, when given, is a second answer read into the same data (the aero
// layer, sources.hpp); an element both answers carry is read once.
OsmData normalizeOsm(const nlohmann::json& document, const nlohmann::json* layer = nullptr,
                     const nlohmann::json* retail = nullptr);

struct ElevationGrid {
    Bounds bounds;
    int size = 2;
    std::vector<double> values;  // row-major, south to north, west to east
    double at(int row, int col) const { return values[size_t(row) * size_t(size) + size_t(col)]; }
    // Bilinear, clamped to the grid's bounds.
    double sample(double lon, double lat) const {
        double u = (lon - bounds.west) / (bounds.east - bounds.west);
        double v = (lat - bounds.south) / (bounds.north - bounds.south);
        u = std::max(0.0, std::min(1.0, u)) * (size - 1);
        v = std::max(0.0, std::min(1.0, v)) * (size - 1);
        const int x0 = (int)std::floor(u), y0 = (int)std::floor(v);
        const int x1 = std::min(x0 + 1, size - 1), y1 = std::min(y0 + 1, size - 1);
        const double tx = u - x0, ty = v - y0;
        const double low = at(y0, x0) * (1.0 - tx) + at(y0, x1) * tx;
        const double high = at(y1, x0) * (1.0 - tx) + at(y1, x1) * tx;
        return low * (1.0 - ty) + high * ty;
    }
};

}  // namespace r1
