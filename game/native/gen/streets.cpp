#include "streets.hpp"

#include "clip.hpp"
#include "palette.hpp"
#include "terrain.hpp"

#include <set>
#include <unordered_map>

namespace r1 {

namespace {
const std::set<std::string> kMotor = {"motorway", "trunk", "primary", "secondary", "tertiary",
                                      "residential", "living_street", "service", "unclassified"};
const std::set<std::string> kUrban = {"primary", "secondary", "tertiary", "residential", "living_street"};
// OSM `surface` values laid as cobbles or setts: a measurement (rule 4).
const std::set<std::string> kCobbled = {"sett", "cobblestone", "unhewn_cobblestone", "cobblestone:flattened",
                                        "paving_stones"};

std::vector<std::pair<std::string, bool>> sidewalkSides(const Tags& tags) {
    const std::string general = tagOr(tags, "sidewalk");
    std::vector<std::pair<std::string, bool>> out;
    for (const char* side : {"left", "right"}) {
        const std::string tagged = tagOr(tags, (std::string("sidewalk:") + side).c_str(), general);
        if (tagged == "no" || tagged == "none" || tagged == "separate") continue;
        if (tagged == "yes" || tagged == "both" || tagged == side) out.push_back({side, false});
        else if (tagged.empty() && kUrban.count(tagOr(tags, "highway"))) out.push_back({side, true});
    }
    return out;
}

// The edges of a region, bucketed on a 2 m grid, for "which part of this
// segment lies along your boundary" questions.
struct EdgeIndex {
    std::vector<std::pair<P2, P2>> edges;
    std::unordered_map<int64_t, std::vector<size_t>> cells;
    static int64_t key(long x, long z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); }
    void add(P2 a, P2 b) {
        const size_t index = edges.size();
        edges.push_back({a, b});
        const long x0 = long(std::floor(std::min(a.x, b.x) / 2.0)), x1 = long(std::floor(std::max(a.x, b.x) / 2.0));
        const long z0 = long(std::floor(std::min(a.y, b.y) / 2.0)), z1 = long(std::floor(std::max(a.y, b.y) / 2.0));
        for (long x = x0; x <= x1; ++x)
            for (long z = z0; z <= z1; ++z) cells[key(x, z)].push_back(index);
    }
    // The stretches of a->b, as fractions of it, that run along an edge of
    // the region within `tolerance`: shapely's boundary intersection, with
    // the millimetre of slack two independently rounded outlines need.
    std::vector<std::pair<double, double>> along(P2 a, P2 b, double tolerance) const {
        std::vector<std::pair<double, double>> out;
        const double dx = b.x - a.x, dz = b.y - a.y, length = std::hypot(dx, dz);
        if (length < 1e-6) return out;
        const double ux = dx / length, uz = dz / length;
        std::vector<size_t> seen;
        const long x0 = long(std::floor(std::min(a.x, b.x) / 2.0)), x1 = long(std::floor(std::max(a.x, b.x) / 2.0));
        const long z0 = long(std::floor(std::min(a.y, b.y) / 2.0)), z1 = long(std::floor(std::max(a.y, b.y) / 2.0));
        for (long x = x0; x <= x1; ++x)
            for (long z = z0; z <= z1; ++z) {
                auto it = cells.find(key(x, z));
                if (it == cells.end()) continue;
                for (size_t i : it->second) {
                    if (std::find(seen.begin(), seen.end(), i) != seen.end()) continue;
                    seen.push_back(i);
                    const auto& [c, d] = edges[i];
                    const double oc = (c.x - a.x) * uz - (c.y - a.y) * ux, od = (d.x - a.x) * uz - (d.y - a.y) * ux;
                    if (std::abs(oc) > tolerance || std::abs(od) > tolerance) continue;
                    double t0 = ((c.x - a.x) * ux + (c.y - a.y) * uz) / length;
                    double t1 = ((d.x - a.x) * ux + (d.y - a.y) * uz) / length;
                    if (t0 > t1) std::swap(t0, t1);
                    t0 = std::max(t0, 0.0); t1 = std::min(t1, 1.0);
                    if ((t1 - t0) * length > 0.002) out.push_back({t0, t1});
                }
            }
        std::sort(out.begin(), out.end());
        std::vector<std::pair<double, double>> merged;
        for (const auto& iv : out) {
            if (!merged.empty() && iv.first <= merged.back().second + 1e-9) merged.back().second = std::max(merged.back().second, iv.second);
            else merged.push_back(iv);
        }
        return merged;
    }
};
}  // namespace

bool isMotorway(const std::string& highway) { return kMotor.count(highway) > 0; }

double lengthTag(const std::string* value, double fallback) {
    if (!value) return fallback;
    std::string s = *value;
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, " m") == 0) s.resize(s.size() - 2);
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    s = s.substr(a, b - a);
    if (s.empty()) return fallback;
    char* end = nullptr;
    const double n = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return fallback;
    return std::isfinite(n) && n > 0 && n <= 50 ? n : fallback;
}

double roadWidth(const Tags& tags) { return lengthTag(tag(tags, "width"), roadWidthOf(tagOr(tags, "highway"))); }

StreetOutput buildStreets(const std::vector<OsmWay>& roads, const std::vector<OsmNode>& features,
                          const ElevationGrid& elevations, const Anchor& anchor, const std::vector<Ring>& footprints) {
    using clip::Paths64;
    int sidewalkTagged = 0, sidewalkInferred = 0, zebras = 0, widthsTagged = 0, widthsInferred = 0, graded = 0;
    Paths64 roadPolys, cobblePolys, walkPolys;
    struct MotorLine { int64_t id; std::vector<P2> line; double half; };
    std::vector<MotorLine> motorLines;
    for (const OsmWay& road : roads) {
        const Tags& tags = road.tags;
        if (tagOr(tags, "area") == "yes" || tagOr(tags, "footway") == "crossing") continue;
        if (taggedYes(tags, "tunnel") || taggedYes(tags, "bridge")) { ++graded; continue; }
        std::vector<P2> line;
        for (const P2& p : road.points) { const P3 e = anchor.toEngine(p.x, p.y, 0.0); line.push_back({e.x, e.z}); }
        if (clip::length(line) < 0.1) continue;
        const double half = roadWidth(tags) * 0.5;
        const Paths64 strip = clip::bufferLine(line, half);
        ++(has(tags, "width") ? widthsTagged : widthsInferred);
        if (!kMotor.count(tagOr(tags, "highway"))) {
            walkPolys.insert(walkPolys.end(), strip.begin(), strip.end());
            continue;
        }
        Paths64& target = kCobbled.count(tagOr(tags, "surface")) ? cobblePolys : roadPolys;
        target.insert(target.end(), strip.begin(), strip.end());
        motorLines.push_back({road.id, line, half});
        for (const auto& [side, inferred] : sidewalkSides(tags)) {
            ++(inferred ? sidewalkInferred : sidewalkTagged);
            const double sw = lengthTag(tag(tags, ("sidewalk:" + side + ":width").c_str()),
                                        lengthTag(tag(tags, "sidewalk:width"), 1.8));
            // Engine z points south: geographic left is the numeric right.
            const double sign = side == "left" ? -1.0 : 1.0;
            const Paths64 band = clip::subtract(clip::singleSided(line, sign * (half + sw)), strip);
            walkPolys.insert(walkPolys.end(), band.begin(), band.end());
        }
    }
    Paths64 buildings;
    for (const Ring& r : footprints)
        if (r.size() >= 3) {
            const Paths64 b = clip::bufferRing(r, 0.0);
            buildings.insert(buildings.end(), b.begin(), b.end());
        }
    buildings = clip::unite(buildings);
    // Integer millimetres throughout: the precision snap shapely needed is
    // the grid itself here.
    const Paths64 asphalt = clip::subtract(clip::unite(roadPolys), buildings);
    const Paths64 cobbles = clip::subtract(clip::subtract(clip::unite(cobblePolys), buildings), asphalt);
    const Paths64 paving =
        clip::subtract(clip::subtract(clip::subtract(clip::unite(walkPolys), asphalt), cobbles), buildings);
    const Paths64 carriageway = clip::unite(asphalt, cobbles);

    // Zebra crossings, where OSM says they are painted.
    Paths64 stripes;
    std::set<int64_t> seen;
    for (const OsmNode& f : features) {
        if (seen.count(f.id) || tagOr(f.tags, "highway") != "crossing") continue;
        seen.insert(f.id);
        if (tagOr(f.tags, "crossing:markings") != "zebra" && tagOr(f.tags, "crossing_ref") != "zebra") continue;
        const P3 e = anchor.toEngine(f.lon, f.lat, 0.0);
        const P2 point{e.x, e.z};
        const MotorLine* best = nullptr;
        double bestDistance = 1e300;
        for (const MotorLine& m : motorLines) {
            const double d = clip::distance(m.line, point);
            if (d < bestDistance || (d == bestDistance && best && m.id < best->id)) { bestDistance = d; best = &m; }
        }
        if (!best || bestDistance > 1.0) continue;
        const double total = clip::length(best->line), along = clip::project(best->line, point);
        const P2 a = clip::interpolate(best->line, std::max(0.0, along - 0.2));
        const P2 b = clip::interpolate(best->line, std::min(total, along + 0.2));
        const double size = dist(a, b);
        if (size < 1e-6) continue;
        const double dx = (b.x - a.x) / size, dz = (b.y - a.y) / size;
        const P2 centre = clip::interpolate(best->line, along);
        const double span = lengthTag(tag(f.tags, "width"), 3.0) / 2;
        auto p = [&](double s, double t) { return P2{centre.x + dx * s - dz * t, centre.y + dz * s + dx * t}; };
        for (double offset = -best->half + 0.25; offset + 0.5 < best->half; offset += 1.0)
            stripes.push_back(clip::kMetres.path({p(-span, offset), p(span, offset), p(span, offset + 0.5), p(-span, offset + 0.5)}));
        ++zebras;
    }
    const Paths64 paint = clip::intersect(clip::unite(stripes), asphalt);

    Mesh roadMesh(UvMode::Planar), cobbleMesh(UvMode::Planar), walkMesh(UvMode::Planar), paintMesh, kerbMesh;
    struct Surface { const Paths64* region; Mesh* mesh; double lift; };
    const Surface surfaces[] = {{&asphalt, &roadMesh, 0.06}, {&cobbles, &cobbleMesh, 0.06},
                                {&paving, &walkMesh, 0.21}, {&paint, &paintMesh, 0.075}};
    const Bounds& bounds = elevations.bounds;
    const int size = kTerrainMeshSize - 1;
    std::vector<P3> grid;
    for (int row = 0; row <= size; ++row)
        for (int col = 0; col <= size; ++col) {
            const double lo = bounds.west + (bounds.east - bounds.west) * col / size;
            const double la = bounds.south + (bounds.north - bounds.south) * row / size;
            grid.push_back(anchor.toEngine(lo, la, elevations.sample(lo, la)));
        }
    auto g = [&](int row, int col) { return grid[size_t(row * (size + 1) + col)]; };
    auto box = [](std::initializer_list<P3> pts) {
        double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
        for (const P3& p : pts) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.z); z1 = std::max(z1, p.z); }
        return clip::Path64{clip::kMetres.at({x0 - 0.01, z0 - 0.01}), clip::kMetres.at({x1 + 0.01, z0 - 0.01}),
                            clip::kMetres.at({x1 + 0.01, z1 + 0.01}), clip::kMetres.at({x0 - 0.01, z1 + 0.01})};
    };

    // The height of the drawn terrain under an engine point. Asked of the
    // point itself, never of the triangle it was clipped against: a corner
    // cut on the edge between two terrain triangles is rounded to the
    // millimetre grid, and each triangle would extrapolate it differently --
    // two heights for one corner is a crack in the road and a vertex twice.
    auto surfaceY = [&](P2 q) {
        const P3 geo = anchor.toGeodetic(q.x, 0.0, q.y);
        double u = std::max(0.0, std::min(1.0, (geo.x - bounds.west) / (bounds.east - bounds.west))) * size;
        double v = std::max(0.0, std::min(1.0, (geo.y - bounds.south) / (bounds.north - bounds.south))) * size;
        const int col = std::min(size - 1, int(u)), row = std::min(size - 1, int(v));
        u -= col; v -= row;
        const P3 a = g(row, col), b = u >= v ? g(row, col + 1) : g(row + 1, col + 1), c = u >= v ? g(row + 1, col + 1) : g(row + 1, col);
        const double denom = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        const double wa = ((b.z - c.z) * (q.x - c.x) + (c.x - b.x) * (q.y - c.z)) / denom;
        const double wb = ((c.z - a.z) * (q.x - c.x) + (a.x - c.x) * (q.y - c.z)) / denom;
        return wa * a.y + wb * b.y + (1 - wa - wb) * c.y;
    };

    // A surface triangle never crosses a terrain triangle edge, so every road
    // lies in the plane of the ground drawn beneath it. The regions are cut to
    // a row first and a cell second, so each triangle clips only what is near.
    for (const Surface& s : surfaces) {
        if (s.region->empty()) continue;
        for (int row = 0; row < size; ++row) {
            const clip::Paths64 strip = clip::intersect(*s.region, {box({g(row, 0), g(row, size), g(row + 1, 0), g(row + 1, size)})});
            if (strip.empty()) continue;
            for (int col = 0; col < size; ++col) {
                const P3 sw = g(row, col), se = g(row, col + 1), ne = g(row + 1, col + 1), nw = g(row + 1, col);
                const clip::Paths64 cell = clip::intersect(strip, {box({sw, se, ne, nw})});
                if (cell.empty()) continue;
                for (const auto& tri : {std::array<P3, 3>{sw, se, ne}, std::array<P3, 3>{sw, ne, nw}}) {
                    const P3 a = tri[0], b = tri[1], c = tri[2];
                    const clip::Paths64 piece = clip::intersect(cell, {clip::kMetres.path({{a.x, a.z}, {b.x, b.z}, {c.x, c.z}})});
                    if (piece.empty()) continue;
                    auto vertex = [&](P2 q) { return P3{q.x, surfaceY(q) + s.lift, q.y}; };
                    for (const clip::Polygon& part : clip::polygons(piece)) {
                        if (std::abs(polygonArea(part.outer)) < 1e-6) continue;
                        for (const auto& t : clip::triangles(part))
                            s.mesh->addUpTriangle(vertex(t[0]), vertex(t[1]), vertex(t[2]));
                    }
                }
            }
        }
    }

    // Kerbs only where pavement meets carriageway: the paving edges that lie
    // on the carriageway's boundary, chained and simplified.
    EdgeIndex carriageEdges;
    for (const clip::Polygon& p : clip::polygons(carriageway)) {
        auto edges = [&](const Ring& r) { for (size_t i = 0; i < r.size(); ++i) carriageEdges.add(r[i], r[(i + 1) % r.size()]); };
        edges(p.outer);
        for (const Ring& h : p.holes) edges(h);
    }
    std::vector<std::pair<P2, P2>> contact;
    for (const clip::Polygon& p : clip::polygons(paving)) {
        auto edges = [&](const Ring& r) {
            for (size_t i = 0; i < r.size(); ++i) {
                const P2 a = r[i], b = r[(i + 1) % r.size()];
                for (const auto& [t0, t1] : carriageEdges.along(a, b, 0.003)) {
                    const P2 p0{a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0};
                    const P2 p1{a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1};
                    contact.push_back({t0 <= 0.0 ? a : p0, t1 >= 1.0 ? b : p1});
                }
            }
        };
        edges(p.outer);
        for (const Ring& h : p.holes) edges(h);
    }
    // line_merge: chain segments through the points exactly two of them share.
    auto keyOf = [](P2 p) { return std::make_pair(std::llround(p.x * 1000), std::llround(p.y * 1000)); };
    std::map<std::pair<long long, long long>, std::vector<size_t>> ends;
    for (size_t i = 0; i < contact.size(); ++i) {
        ends[keyOf(contact[i].first)].push_back(i);
        ends[keyOf(contact[i].second)].push_back(i);
    }
    std::vector<bool> used(contact.size(), false);
    std::vector<std::vector<P2>> lines;
    auto extend = [&](std::vector<P2>& line, size_t from) {
        for (;;) {
            const auto& at = ends[keyOf(line.back())];
            if (at.size() != 2) return;
            const size_t next = at[0] == from ? at[1] : at[0];
            if (used[next]) return;
            used[next] = true;
            const auto& seg = contact[next];
            line.push_back(keyOf(seg.first) == keyOf(line.back()) ? seg.second : seg.first);
            from = next;
        }
    };
    for (size_t i = 0; i < contact.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        std::vector<P2> forward{contact[i].first, contact[i].second};
        extend(forward, i);
        std::vector<P2> backward{contact[i].second, contact[i].first};
        extend(backward, i);
        std::vector<P2> line(backward.rbegin(), backward.rend() - 2);
        line.insert(line.end(), forward.begin(), forward.end());
        lines.push_back(clip::simplify(line, 0.02));
    }
    for (const auto& line : lines)
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            const P2 a = line[i], b = line[i + 1];
            const int n = std::max(1, int(std::ceil(dist(a, b) / 8.0)));
            auto p = [&](double t, double lift) {
                const double x = a.x + (b.x - a.x) * t, z = a.y + (b.y - a.y) * t;
                const P3 geo = anchor.toGeodetic(x, 0, z);
                return P3{x, groundPoint(geo.x, geo.y, elevations, anchor).y + lift, z};
            };
            for (int k = 0; k < n; ++k)
                kerbMesh.addQuad(p(double(k) / n, 0.06), p(double(k + 1) / n, 0.06), p(double(k + 1) / n, 0.21), p(double(k) / n, 0.21));
        }

    StreetOutput out;
    struct Spec { const char* name; Mesh* mesh; std::array<double, 3> color; const char* family; };
    const Spec specs[] = {{"Carriageway", &roadMesh, {0.10, 0.11, 0.12}, "asphalt"},
                          {"Cobbled carriageway", &cobbleMesh, {0.16, 0.155, 0.145}, "cobbles"},
                          {"Sidewalks", &walkMesh, {0.22, 0.215, 0.20}, "pavement"},
                          {"Kerbs", &kerbMesh, {0.26, 0.25, 0.23}, nullptr},
                          {"Surveyed zebra crossings", &paintMesh, {0.50, 0.49, 0.46}, nullptr}};
    for (const Spec& s : specs) {
        if (s.mesh->empty()) continue;
        const bool kerb = std::string(s.name) == "Kerbs";
        Mesh mesh = kerb ? std::move(*s.mesh) : smoothSurface(*s.mesh);
        out.parts.push_back({s.name, std::move(mesh),
                             surfaceMaterial(s.name, s.color, 0.88,
                                             s.family ? std::optional<std::string>(s.family) : std::nullopt, kerb)});
    }
    out.stats = {{"sidewalkSidesTagged", sidewalkTagged}, {"sidewalkSidesInferred", sidewalkInferred},
                 {"zebraCrossingsTagged", zebras}, {"widthsTagged", widthsTagged}, {"widthsInferred", widthsInferred},
                 {"unsupportedGradeSeparatedWays", graded},
                 {"carriagewayAreaM2", pyround(clip::area(asphalt), 2)},
                 {"cobbledAreaM2", pyround(clip::area(cobbles), 2)},
                 {"sidewalkAreaM2", pyround(clip::area(paving), 2)},
                 {"surfaceOverlapM2", pyround(clip::area(clip::intersect(asphalt, paving)), 6)}};
    return out;
}

}  // namespace r1
