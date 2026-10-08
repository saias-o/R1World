#include "terrain.hpp"

namespace r1 {

namespace {
struct SurfaceTriangle {
    P3 geo[3];  // longitude, latitude, altitude
    double weights[3];
};

SurfaceTriangle surfaceTriangle(double lon, double lat, const ElevationGrid& g) {
    const Bounds& b = g.bounds;
    const int last = kTerrainMeshSize - 1;
    const double x = std::clamp(lon, b.west, b.east), y = std::clamp(lat, b.south, b.north);
    const double uc = (x - b.west) / (b.east - b.west) * last;
    const double vr = (y - b.south) / (b.north - b.south) * last;
    const int c = std::min(last - 1, int(uc)), r = std::min(last - 1, int(vr));
    const double u = uc - c, v = vr - r;
    auto node = [&](int col, int row) {
        const double lo = b.west + (b.east - b.west) * col / last;
        const double la = b.south + (b.north - b.south) * row / last;
        return P3{lo, la, g.sample(lo, la)};
    };
    const bool south = r == 0 && u >= v && !g.southEdge.empty();
    const bool north = r == last - 1 && u < v && !g.northEdge.empty();
    if (south || north) {
        const auto& edge = south ? g.southEdge : g.northEdge;
        const P3 apex = south ? node(c + 1, r + 1) : node(c, r);
        const double w = south ? v : 1 - v;
        const double lo = node(c, r).x, hi = node(c + 1, r).x;
        // Project from the triangle's opposite vertex onto the boundary.
        const double at = w >= 1 ? hi : std::clamp((x - w * apex.x) / (1 - w), lo, hi);
        auto next = std::upper_bound(edge.begin(), edge.end(), at,
                                     [](double a, const P2& p) { return a < p.x; });
        const double e0 = std::max(lo, next == edge.begin() ? lo : (next - 1)->x);
        const double e1 = std::min(hi, next == edge.end() ? hi : next->x);
        const double t = e1 > e0 ? std::clamp((at - e0) / (e1 - e0), 0., 1.) : 0.;
        const double la = south ? b.south : b.north;
        return {{{e0, la, ElevationGrid::edgeSample(edge, e0)},
                 {e1, la, ElevationGrid::edgeSample(edge, e1)}, apex},
                {(1 - w) * (1 - t), (1 - w) * t, w}};
    }
    if (u >= v) return {{node(c, r), node(c + 1, r), node(c + 1, r + 1)}, {1 - u, u - v, v}};
    return {{node(c, r), node(c + 1, r + 1), node(c, r + 1)}, {1 - v, u, v - u}};
}
}  // namespace

double roadWidthOf(const std::string& highway) {
    static const std::map<std::string, double> widths = {
        {"motorway", 13.0}, {"trunk", 11.0}, {"primary", 9.0}, {"secondary", 8.0}, {"tertiary", 7.0},
        {"residential", 6.5}, {"unclassified", 6.5}, {"road", 6.5},
        {"motorway_link", 5.0}, {"trunk_link", 5.0}, {"primary_link", 6.5},
        {"secondary_link", 6.5}, {"tertiary_link", 6.5},
        {"living_street", 5.0}, {"service", 3.5}, {"pedestrian", 3.0},
        {"cycleway", 1.8}, {"footway", 1.5}, {"path", 1.2}, {"steps", 1.2}};
    auto it = widths.find(highway);
    return it == widths.end() ? 2.5 : it->second;
}

std::map<std::string, Mesh> buildTerrain(const Bounds& b, const ElevationGrid& elevations, const Anchor& anchor,
                                         const std::function<std::string(double, double)>& classify,
                                         const std::function<double(int, int, double)>& adjust,
                                         TerrainGrid* grid) {
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
    if (grid) {
        grid->points = points; grid->classes.clear();
        grid->southBoundary.clear(); grid->northBoundary.clear();
        grid->southClasses.clear(); grid->northClasses.clear();
        for (bool north : {false,true}) {
            const auto& edge = north ? elevations.northEdge : elevations.southEdge;
            auto& output = north ? grid->northBoundary : grid->southBoundary;
            const int row = north ? n-1 : 0;
            const double lat = north ? b.north : b.south;
            for (const auto& p : edge) {
                double u = (p.x-b.west)/(b.east-b.west), cell = u*(n-1);
                if (std::abs(cell-std::round(cell)) < 1e-6) {
                    const int col = int(std::round(cell));
                    output.push_back({double(col)/(n-1), points[size_t(row*n+col)].y});
                } else {
                    const int col = std::min(n-2,int(cell)); const double t=cell-col;
                    double h=p.y;
                    if(adjust) {
                        const double lo=b.west+(b.east-b.west)*col/(n-1),hi=b.west+(b.east-b.west)*(col+1)/(n-1);
                        const double hl=elevations.sample(lo,lat),hr=elevations.sample(hi,lat);
                        h+=(adjust(row,col,hl)-hl)*(1-t)+(adjust(row,col+1,hr)-hr)*t;
                    }
                    output.push_back({u,anchor.toEngine(p.x,lat,h).y});
                }
            }
        }
    }
    auto meshFor = [&](P2 a, P2 bb, P2 c) -> Mesh& {
        const std::string name = classify ? classify((a.x + bb.x + c.x) / 3.0, (a.y + bb.y + c.y) / 3.0) : "";
        return meshes[name];
    };
    auto uv = [](P3 p) { return UV{p.x, -p.z}; };
    for (int row = 0; row < n - 1; ++row)
        for (int col = 0; col < n - 1; ++col) {
            const size_t sw = size_t(row * n + col), se = sw + 1, ne = sw + size_t(n) + 1, nw = sw + size_t(n);
            auto emit = [&](P3 a, P3 bb, P3 c, P2 ga, P2 gb, P2 gc) {
                const UV tex[3] = {uv(a), uv(bb), uv(c)};
                meshFor(ga, gb, gc).addUpTriangle(a, bb, c, tex);
            };
            // The grass coverage grid keeps its two regular-cell classes.
            if (grid) for (auto tri : {std::array<size_t, 3>{sw, se, ne}, {sw, ne, nw}}) {
                const auto a = coordinates[tri[0]], bb = coordinates[tri[1]], c = coordinates[tri[2]];
                grid->classes.push_back(classify ? classify((a.x + bb.x + c.x) / 3., (a.y + bb.y + c.y) / 3.) : "");
            }
            auto boundary = [&](bool north) {
                const auto& edge = north ? elevations.northEdge : elevations.southEdge;
                const size_t left = north ? nw : sw, right = north ? ne : se, apex = north ? sw : ne;
                const double lo = coordinates[left].x, hi = coordinates[right].x, lat = coordinates[left].y;
                P3 prev = points[left]; P2 geoPrev = coordinates[left];
                auto segment = [&](P3 next, P2 geoNext) {
                    if(grid) {
                        const P2 apexGeo=coordinates[apex];
                        auto& classes=north ? grid->northClasses : grid->southClasses;
                        classes.push_back(classify ? classify((geoPrev.x+geoNext.x+apexGeo.x)/3.,
                                                              (geoPrev.y+geoNext.y+apexGeo.y)/3.) : "");
                    }
                    emit(prev,next,points[apex],geoPrev,geoNext,coordinates[apex]);
                    prev=next;geoPrev=geoNext;
                };
                auto it = std::upper_bound(edge.begin(), edge.end(), lo + 1e-10,
                                           [](double x, const P2& p) { return x < p.x; });
                for (; it != edge.end() && it->x < hi - 1e-10; ++it) {
                    const double t = (it->x - lo) / (hi - lo);
                    double h = it->y;
                    if (adjust) {
                        const int r = north ? n - 1 : 0;
                        const double hl = elevations.sample(lo, lat), hr = elevations.sample(hi, lat);
                        h += (adjust(r, col, hl) - hl) * (1 - t) + (adjust(r, col + 1, hr) - hr) * t;
                    }
                    const P3 next = anchor.toEngine(it->x, lat, h); const P2 geoNext{it->x, lat};
                    segment(next,geoNext);
                }
                segment(points[right],coordinates[right]);
            };
            if (row == 0 && !elevations.southEdge.empty()) boundary(false);
            else emit(points[sw], points[se], points[ne], coordinates[sw], coordinates[se], coordinates[ne]);
            if (row == n - 2 && !elevations.northEdge.empty()) boundary(true);
            else emit(points[sw], points[ne], points[nw], coordinates[sw], coordinates[ne], coordinates[nw]);
        }
    return meshes;
}

P3 groundPoint(double lon, double lat, const ElevationGrid& elevations, const Anchor& anchor, double lift) {
    const auto tri = surfaceTriangle(lon, lat, elevations);
    P3 vertices[3];
    for (int i = 0; i < 3; ++i) {
        const auto p = tri.geo[i];
        vertices[i] = anchor.toEngine(p.x, p.y, p.z);
    }
    // Clamp only the elevation sampling: a footprint crossing the tile edge
    // keeps its real x and z.
    const P3 p = anchor.toEngine(lon, lat, elevations.sample(lon, lat));
    const P3 a = vertices[0], bb = vertices[1], c = vertices[2];
    const double det = (bb.z-c.z)*(a.x-c.x)+(c.x-bb.x)*(a.z-c.z);
    // For an out-of-bounds footprint keep the boundary's clamped height.
    double y = 0;
    if (lon >= elevations.bounds.west && lon <= elevations.bounds.east &&
        lat >= elevations.bounds.south && lat <= elevations.bounds.north && std::abs(det) > 1e-12) {
        const double wa = ((bb.z-c.z)*(p.x-c.x)+(c.x-bb.x)*(p.z-c.z))/det;
        const double wb = ((c.z-a.z)*(p.x-c.x)+(a.x-c.x)*(p.z-c.z))/det;
        y = wa*a.y + wb*bb.y + (1-wa-wb)*c.y;
    } else for (int i=0;i<3;++i) y += vertices[i].y*tri.weights[i];
    return {p.x, y + lift, p.z};
}

double terrainElevation(double lon, double lat, const ElevationGrid& elevations) {
    const auto tri = surfaceTriangle(lon, lat, elevations);
    double h = 0;
    for (int i = 0; i < 3; ++i) h += tri.geo[i].z * tri.weights[i];
    return h;
}

}  // namespace r1
