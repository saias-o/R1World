#include "terrain.hpp"

namespace r1 {

double roadWidthOf(const std::string& highway) {
    static const std::map<std::string, double> widths = {
        {"motorway", 13.0}, {"trunk", 11.0}, {"primary", 9.0}, {"secondary", 8.0}, {"tertiary", 7.0},
        {"residential", 5.5}, {"living_street", 5.0}, {"service", 3.5}, {"pedestrian", 3.0},
        {"cycleway", 1.8}, {"footway", 1.5}, {"path", 1.2}, {"steps", 1.2}};
    auto it = widths.find(highway);
    return it == widths.end() ? 2.5 : it->second;
}

std::map<std::string, Mesh> buildTerrain(const Bounds& b, const ElevationGrid& elevations, const Anchor& anchor,
                                         const std::function<std::string(double, double)>& classify,
                                         const std::function<double(int, int, double)>& adjust) {
    const int n = kTerrainMeshSize;
    std::vector<P3> points(size_t(n * n));
    std::vector<P2> coordinates(size_t(n * n));
    for (int row = 0; row < n; ++row) {
        const double lat = b.south + (b.north - b.south) * row / (n - 1);
        for (int col = 0; col < n; ++col) {
            const double lon = b.west + (b.east - b.west) * col / (n - 1);
            double height = elevations.sample(lon, lat);
            if (adjust) height = adjust(row, col, height);
            points[size_t(row * n + col)] = anchor.toEngine(lon, lat, height);
            coordinates[size_t(row * n + col)] = {lon, lat};
        }
    }
    std::map<std::string, Mesh> meshes;
    auto meshFor = [&](P2 a, P2 bb, P2 c) -> Mesh& {
        if (!classify) return meshes[""];
        return meshes[classify((a.x + bb.x + c.x) / 3.0, (a.y + bb.y + c.y) / 3.0)];
    };
    auto uv = [](P3 p) { return UV{p.x, -p.z}; };
    for (int row = 0; row < n - 1; ++row)
        for (int col = 0; col < n - 1; ++col) {
            const size_t sw = size_t(row * n + col), se = sw + 1, ne = sw + size_t(n) + 1, nw = sw + size_t(n);
            const UV first[3] = {uv(points[sw]), uv(points[se]), uv(points[ne])};
            meshFor(coordinates[sw], coordinates[se], coordinates[ne])
                .addUpTriangle(points[sw], points[se], points[ne], first);
            const UV second[3] = {uv(points[sw]), uv(points[ne]), uv(points[nw])};
            meshFor(coordinates[sw], coordinates[ne], coordinates[nw])
                .addUpTriangle(points[sw], points[ne], points[nw], second);
        }
    return meshes;
}

P3 groundPoint(double lon, double lat, const ElevationGrid& elevations, const Anchor& anchor, double lift) {
    const Bounds& b = elevations.bounds;
    const int size = kTerrainMeshSize - 1;
    double u = std::max(0.0, std::min(1.0, (lon - b.west) / (b.east - b.west))) * size;
    double v = std::max(0.0, std::min(1.0, (lat - b.south) / (b.north - b.south))) * size;
    const int col = std::min(size - 1, int(u)), row = std::min(size - 1, int(v));
    u -= col; v -= row;
    const int corners[2][3][2] = {{{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {1, 1}, {0, 1}}};
    const bool lowerRight = u >= v;
    const double weights[3] = {lowerRight ? 1 - u : 1 - v, lowerRight ? u - v : u, lowerRight ? v : v - u};
    double y = 0;
    for (int i = 0; i < 3; ++i) {
        const int cx = corners[lowerRight ? 0 : 1][i][0], cy = corners[lowerRight ? 0 : 1][i][1];
        const double lo = b.west + double(col + cx) / size * (b.east - b.west);
        const double la = b.south + double(row + cy) / size * (b.north - b.south);
        y += anchor.toEngine(lo, la, elevations.sample(lo, la)).y * weights[i];
    }
    // Clamp only the elevation sampling: a footprint crossing the tile edge
    // keeps its real x and z.
    const P3 p = anchor.toEngine(lon, lat, elevations.sample(lon, lat));
    return {p.x, y + lift, p.z};
}

}  // namespace r1
