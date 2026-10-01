#include "harbours.hpp"

#include "streets.hpp"

#include <fstream>
#include <mutex>

namespace r1 {

namespace {
constexpr double kSeaLevelWaterMax = 2.5;
constexpr double kSeaFloor = -4.0;
constexpr double kLandAboveSea = 0.25;
constexpr int64_t kSaltBoat = 0x0B0A7, kSaltYard = 0x7A2D;
constexpr int kTileBoatBudget = 70, kTileShipBudget = 4, kTileContainerBudget = 60, kShipContainerBudget = 80;
const double kContainer[3] = {2.44, 2.59, 12.19};  // a forty-foot box: width, height, length
const char* kContainerModels[3] = {"cargo_container_a", "cargo_container_b", "cargo_container_c"};
const std::string kBoatDir = "assets/models/external/kenney_boats/";

const std::map<std::string, std::array<double, 3>> kPaint = {
    {"white", {0.72, 0.72, 0.70}}, {"red", {0.40, 0.05, 0.04}}, {"black", {0.04, 0.04, 0.045}},
    {"green", {0.05, 0.24, 0.08}}, {"yellow", {0.60, 0.48, 0.08}}, {"grey", {0.30, 0.30, 0.30}},
    {"blue", {0.05, 0.10, 0.35}}};

clip::Path64 boxPath(const Bounds& b) {
    return clip::kDegrees.path({{b.west, b.south}, {b.east, b.south}, {b.east, b.north}, {b.west, b.north}});
}

bool insideBounds(const Bounds& b, double lon, double lat) {
    return b.west <= lon && lon <= b.east && b.south <= lat && lat <= b.north;
}

// A point inside a region (shapely's representative_point, near enough for a
// height test): the centroid when it is inside, else a triangle's.
P2 representativePoint(const clip::Polygon& poly) {
    const P2 c = centroid(poly.outer);
    bool inside = pointInPolygon(c, poly.outer);
    for (const Ring& h : poly.holes) if (pointInPolygon(c, h)) inside = false;
    if (inside) return c;
    const auto tris = clip::triangles(poly);
    if (tris.empty()) return poly.outer.front();
    return {(tris[0][0].x + tris[0][1].x + tris[0][2].x) / 3, (tris[0][0].y + tris[0][1].y + tris[0][2].y) / 3};
}

// The parts of a way inside the tile, as (points, closed) pairs, in (lon, lat).
std::vector<std::pair<std::vector<P2>, bool>> clipped(const OsmWay& way, const Bounds& bounds) {
    std::vector<std::pair<std::vector<P2>, bool>> out;
    const clip::Path64 box = boxPath(bounds);
    if (way.closed()) {
        const clip::Paths64 region = clip::intersect(clip::bufferRing(way.points, 0.0, clip::kDegrees), {box});
        for (const clip::Polygon& p : clip::polygons(region, clip::kDegrees)) {
            std::vector<P2> ring = p.outer;
            // Counter-clockwise in (lon, lat), so clockwise in the engine's (x, z):
            // the skirt under a closed pier deck faces out. GEOS handed Python
            // its shells the other way round, and those skirts faced in.
            if (polygonArea(ring) < 0) std::reverse(ring.begin(), ring.end());
            ring.push_back(ring.front());
            out.push_back({ring, true});
        }
        return out;
    }
    const auto lo = clip::kDegrees.at({bounds.west, bounds.south}), hi = clip::kDegrees.at({bounds.east, bounds.north});
    const Clipper2Lib::Rect64 rect(lo.x, lo.y, hi.x, hi.y);
    for (const auto& piece : Clipper2Lib::RectClipLines(rect, clip::kDegrees.path(way.points)))
        if (piece.size() >= 2) out.push_back({clip::kDegrees.ring(piece), false});
    return out;
}

std::vector<P2> enginePoints(const Anchor& anchor, const std::vector<P2>& points) {
    std::vector<P2> out;
    for (const P2& p : points) { const P3 e = anchor.toEngine(p.x, p.y, 0.0); out.push_back({e.x, e.z}); }
    return out;
}

double seaY(const Anchor& anchor, double lon, double lat) { return anchor.toEngine(lon, lat, 0.0).y; }

std::vector<P2> subdivide(const std::vector<P2>& points, double step) {
    std::vector<P2> out{points.front()};
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const P2 a = points[i], b = points[i + 1];
        const int n = std::max(1, int(std::ceil(dist(a, b) / step)));
        for (int k = 1; k <= n; ++k) out.push_back({a.x + (b.x - a.x) * k / n, a.y + (b.y - a.y) * k / n});
    }
    return out;
}

void offsets(const std::vector<P2>& line, double half, std::vector<P2>& left, std::vector<P2>& right) {
    for (size_t i = 0; i < line.size(); ++i) {
        const P2 a = line[i == 0 ? 0 : i - 1], b = line[std::min(line.size() - 1, i + 1)];
        const double dx = b.x - a.x, dz = b.y - a.y;
        double length = std::hypot(dx, dz);
        if (length == 0) length = 1.0;
        const double nx = -dz / length, nz = dx / length;
        left.push_back({line[i].x + nx * half, line[i].y + nz * half});
        right.push_back({line[i].x - nx * half, line[i].y - nz * half});
    }
}

template <class F>
void strip(Mesh& mesh, const std::vector<P2>& left, const std::vector<P2>& right, F y, bool uvs = true) {
    double run = 0;
    for (size_t i = 0; i + 1 < left.size(); ++i) {
        const P2 a0 = left[i], a1 = left[i + 1], b0 = right[i], b1 = right[i + 1];
        const double seg = dist({(a0.x + b0.x) / 2, (a0.y + b0.y) / 2}, {(a1.x + b1.x) / 2, (a1.y + b1.y) / 2});
        const double w = dist(a0, b0);
        const UV quad[4] = {{run, 0.0}, {run + seg, 0.0}, {run + seg, w}, {run, w}};
        mesh.addUpQuad({a0.x, y(i), a0.y}, {a1.x, y(i + 1), a1.y}, {b1.x, y(i + 1), b1.y}, {b0.x, y(i), b0.y},
                       uvs ? quad : nullptr);
        run += seg;
    }
}

void pile(Mesh& mesh, double cx, double cz, double y0, double y1, double half) {
    const double d[2][2] = {{half, 0.0}, {0.0, half}};
    for (const auto& dd : d)
        mesh.addQuad({cx - dd[0], y0, cz - dd[1]}, {cx + dd[0], y0, cz + dd[1]}, {cx + dd[0], y1, cz + dd[1]},
                     {cx - dd[0], y1, cz - dd[1]});
}

std::vector<std::string> colours(const Tags& tags) {
    std::string raw = tagOr(tags, "seamark:landmark:colour");
    if (raw.empty()) raw = tagOr(tags, "seamark:light:colour");
    if (raw.empty()) raw = tagOr(tags, "colour");
    if (raw.empty()) raw = tagOr(tags, "building:colour");
    for (char& c : raw) if (c == ',') c = ';';
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t end = raw.find(';', start);
        if (end == std::string::npos) end = raw.size();
        std::string name = raw.substr(start, end - start);
        size_t a = 0, b = name.size();
        while (a < b && std::isspace((unsigned char)name[a])) ++a;
        while (b > a && std::isspace((unsigned char)name[b - 1])) --b;
        name = name.substr(a, b - a);
        for (char& c : name) c = char(std::tolower((unsigned char)c));
        if (!name.empty() && kPaint.count(name)) out.push_back(name);
        start = end + 1;
    }
    if (out.empty()) out.push_back("white");
    return out;
}

void lighthouse(std::map<std::string, Mesh>& paint, const OsmNode& node, const GroundFn& ground) {
    const P3 base = ground(node.lon, node.lat);
    const double x = base.x, y = base.y, z = base.z;
    double height = std::max(8.0, std::min(lengthTag(tag(node.tags, "height"), 20.0), 60.0));
    const auto cols = colours(node.tags);
    const bool banded = cols.size() > 1;
    const int sides = 16;
    double r0 = std::max(2.2, height * 0.13), r1 = std::max(1.5, height * 0.08);
    const double surveyed = lengthTag(tag(node.tags, "r1:radius"), 0.0);
    if (1.0 <= surveyed && surveyed <= 15.0) { r0 = surveyed; r1 = std::max(1.2, surveyed * 0.62); }
    const int bands = banded ? 5 : 1;
    auto ring = [&](double radius, double h) {
        std::vector<P3> out;
        for (int i = 0; i < sides; ++i)
            out.push_back({x + radius * std::cos(2 * kPi * i / sides), y + h, z + radius * std::sin(2 * kPi * i / sides)});
        return out;
    };
    for (int band = 0; band < bands; ++band) {
        const double h0 = height * band / bands, h1 = height * (band + 1) / bands;
        const double ra = r0 + (r1 - r0) * band / bands, rb = r0 + (r1 - r0) * (band + 1) / bands;
        Mesh& mesh = paint[cols[size_t(band) % cols.size()]];
        const auto lo = ring(ra, h0), hi = ring(rb, h1);
        for (int i = 0; i < sides; ++i) {
            const int j = (i + 1) % sides;
            mesh.addQuad(lo[size_t(j)], lo[size_t(i)], hi[size_t(i)], hi[size_t(j)]);
        }
    }
    bool black = false;
    for (const auto& c : cols) black |= c == "black";
    Mesh& top = paint[black ? "black" : "red"];
    const auto gallery = ring(r1 + 0.8, height);
    for (int i = 0; i < sides; ++i)
        top.addUpTriangle({x, y + height + 0.3, z}, gallery[size_t(i)], gallery[size_t((i + 1) % sides)]);
    Mesh& glass = paint["glass"];
    const auto lo = ring(r1 * 0.75, height + 0.3), hi = ring(r1 * 0.75, height + 2.8);
    for (int i = 0; i < sides; ++i) {
        const int j = (i + 1) % sides;
        glass.addQuad(lo[size_t(j)], lo[size_t(i)], hi[size_t(i)], hi[size_t(j)]);
    }
    const auto cap = ring(r1 * 0.9, height + 2.8);
    for (int i = 0; i < sides; ++i)
        top.addUpTriangle({x, y + height + 4.2, z}, cap[size_t(i)], cap[size_t((i + 1) % sides)]);
}

// ── contexts ────────────────────────────────────────────────────────────────
const char* kPortWords[] = {"cargo", "container", "industrial", "bulk", "roro", "tanker"};
const char* kFerryWords[] = {"ferry", "passenger"};
bool anyWord(const std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words) if (s.find(w) != std::string::npos) return true;
    return false;
}
bool anyPort(const std::string& s) { for (const char* w : kPortWords) if (s.find(w) != std::string::npos) return true; return false; }

struct Area { std::string context; std::vector<clip::Polygon> poly; };

std::vector<Area> areas(const OsmData& osm, const Anchor& anchor) {
    std::vector<Area> out;
    std::vector<const OsmWay*> ways;
    for (const auto& w : osm.maritime) ways.push_back(&w);
    for (const auto& w : osm.landcover) ways.push_back(&w);
    for (const OsmWay* way : ways) {
        const Tags& t = way->tags;
        if (!way->closed()) continue;
        std::string context;
        if (tagOr(t, "leisure") == "marina") context = "marina";
        else if (tagOr(t, "landuse") == "port" || tagOr(t, "industrial") == "port" ||
                 (has(t, "harbour") && anyPort(tagOr(t, "harbour:category") + tagOr(t, "harbour"))))
            context = "port";
        else if (has(t, "harbour") || tagOr(t, "landuse") == "harbour") context = "fishing";
        if (context.empty()) continue;
        auto poly = clip::polygons(clip::bufferRing(enginePoints(anchor, way->points), 0.0));
        if (!poly.empty()) out.push_back({context, std::move(poly)});
    }
    return out;
}

std::vector<std::pair<std::string, P2>> harbourPoints(const std::vector<OsmNode>& features, const Anchor& anchor) {
    std::vector<std::pair<std::string, P2>> out;
    for (const OsmNode& node : features) {
        const Tags& t = node.tags;
        std::string category = tagOr(t, "seamark:harbour:category") + " " + tagOr(t, "harbour:category");
        for (char& c : category) c = char(std::tolower((unsigned char)c));
        std::string context;
        if (tagOr(t, "leisure") == "marina" || category.find("marina") != std::string::npos) context = "marina";
        else if (anyPort(category)) context = "port";
        else if (anyWord(category, {kFerryWords[0], kFerryWords[1]})) context = "ferry";
        else if (has(t, "harbour") || tagOr(t, "seamark:type") == "harbour") context = "fishing";
        else continue;
        const P3 e = anchor.toEngine(node.lon, node.lat, 0.0);
        out.push_back({context, {e.x, e.z}});
    }
    return out;
}

std::string contextOf(double x, double z, const std::vector<Area>& areas,
                      const std::vector<std::pair<std::string, P2>>& points, const std::string& fallback) {
    for (const Area& a : areas) if (clip::distance(a.poly, {x, z}) < 25.0) return a.context;
    if (fallback == "marina" || fallback == "canal") return fallback;
    for (const auto& [context, p] : points) if (dist(p, {x, z}) < 300.0) return context;
    return fallback;
}

std::map<std::string, double> mixFor(const std::string& context, const std::string& climate, const RegionProfile& profile) {
    static const std::map<std::string, std::map<std::string, double>> mixes = {
        {"marina", {{"sail", 0.50}, {"motor", 0.45}, {"fishing", 0.05}}},
        {"fishing", {{"fishing", 0.55}, {"motor", 0.20}, {"rowing", 0.15}, {"sail", 0.10}}},
        {"port", {{"tug", 0.25}, {"motor", 0.15}, {"fishing", 0.10}, {"cargo", 0.50}}},
        {"ferry", {{"motor", 0.45}, {"sail", 0.25}, {"fishing", 0.20}, {"liner", 0.10}}},
        {"canal", {{"motor", 0.60}, {"rowing", 0.40}}},
        {"shore", {{"motor", 0.40}, {"rowing", 0.35}, {"sail", 0.25}}}};
    std::map<std::string, double> mix = mixes.at(context);
    if ((climate == "tropical" || climate == "arid") && (context == "fishing" || context == "shore")) {
        mix["rowing"] += 0.25;
        mix["sail"] = mix["sail"] * 0.4;
    }
    if (context == "canal" && (profile.key == "AMSTERDAM" || profile.key == "LOW_COUNTRIES")) mix["houseboat"] = 1.2;
    double total = 0;
    for (const auto& [k, v] : mix) total += v;
    std::map<std::string, double> out;
    for (const auto& [k, v] : mix) if (v > 0) out[k] = v / total;
    return out;
}

std::string pickMix(const std::map<std::string, double>& mix, PyRandom& rng) {
    double cursor = rng.random();
    for (const auto& [name, share] : mix) {
        cursor -= share;
        if (cursor <= 0) return name;
    }
    return mix.rbegin()->first;
}

double segmentDistance(P2 a0, P2 a1, P2 b0, P2 b1) {
    auto ps = [](P2 p, P2 s0, P2 s1) {
        const double dx = s1.x - s0.x, dz = s1.y - s0.y, l = dx * dx + dz * dz;
        const double t = l < 1e-12 ? 0.0 : std::max(0.0, std::min(1.0, ((p.x - s0.x) * dx + (p.y - s0.y) * dz) / l));
        return dist(p, {s0.x + dx * t, s0.y + dz * t});
    };
    return std::min({ps(a0, b0, b1), ps(a1, b0, b1), ps(b0, a0, a1), ps(b1, a0, a1)});
}

std::vector<const BoatKind*> kindsOf(const std::string& kind) {
    std::vector<const BoatKind*> out;
    for (const BoatKind& b : palette().boats) if (b.kind == kind) out.push_back(&b);
    return out;
}

double r3(double v) { return pyround(v, 3); }
double r5(double v) { return pyround(v, 5); }
}  // namespace

double beamOf(const BoatKind& kind) {
    if (kind.lengthRatio > 0) return kind.length / kind.lengthRatio;
    const ModelBounds m = modelBounds(kBoatDir + kind.model + ".glb");
    return (m.high.x - m.low.x) * kind.length / std::max(1e-3, m.high.z - m.low.z);
}

// ── the sea ─────────────────────────────────────────────────────────────────

std::optional<Sea> seaGeometry(const std::vector<OsmWay>& coastlines, const Bounds& bounds,
                               const std::function<double(double, double)>& elevationAt, std::string& how) {
    const clip::Path64 box = boxPath(bounds);
    clip::Paths64 lines;
    const auto lo = clip::kDegrees.at({bounds.west, bounds.south}), hi = clip::kDegrees.at({bounds.east, bounds.north});
    const Clipper2Lib::Rect64 rect(lo.x, lo.y, hi.x, hi.y);
    for (const OsmWay& way : coastlines) {
        if (way.points.size() < 2) continue;
        const clip::Path64 path = clip::kDegrees.path(way.points);
        if (!Clipper2Lib::RectClipLines(rect, path).empty()) lines.push_back(path);
    }
    if (lines.empty()) { how = "no coastline"; return std::nullopt; }
    // The box split along the coastlines: cut by a centimetre-wide gap, so
    // each side is its own piece.
    const clip::Paths64 gaps = Clipper2Lib::InflatePaths(lines, 5e-8 * clip::kDegrees.scale,
                                                        Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Butt, 5.0);
    const auto pieces = clip::polygons(clip::subtract({box}, gaps), clip::kDegrees);
    std::vector<std::array<int, 2>> votes(pieces.size(), {0, 0});
    const double lat = (bounds.south + bounds.north) / 2.0, k = std::cos(radians(lat)), eps = 2e-6;
    auto inPiece = [&](const clip::Polygon& p, P2 q) {
        if (!pointInPolygon(q, p.outer)) return false;
        for (const Ring& h : p.holes) if (pointInPolygon(q, h)) return false;
        return true;
    };
    for (const OsmWay& way : coastlines)
        for (size_t i = 0; i + 1 < way.points.size(); ++i) {
            const P2 p0 = way.points[i], p1 = way.points[i + 1];
            const P2 mid{(p0.x + p1.x) / 2.0, (p0.y + p1.y) / 2.0};
            if (!insideBounds(bounds, mid.x, mid.y)) continue;
            const double east = (p1.x - p0.x) * k, north = p1.y - p0.y, length = std::hypot(east, north);
            if (length < 1e-12) continue;
            // Right of the direction of travel is the sea.
            const double rx = north / length, ry = -east / length;
            for (int side = 0; side < 2; ++side) {
                const double sign = side == 0 ? 1.0 : -1.0;
                const P2 probe{mid.x + sign * rx * eps / k, mid.y + sign * ry * eps};
                for (size_t j = 0; j < pieces.size(); ++j)
                    if (inPiece(pieces[j], probe)) { ++votes[j][size_t(side)]; break; }
            }
        }
    clip::Paths64 sea;
    for (size_t j = 0; j < pieces.size(); ++j) {
        const auto [wet, dry] = votes[j];
        bool isSea;
        if (wet || dry) isSea = wet > dry;
        else { const P2 p = representativePoint(pieces[j]); isSea = elevationAt(p.x, p.y) <= 0.5; }
        if (!isSea) continue;
        sea.push_back(clip::kDegrees.path(pieces[j].outer));
        for (const Ring& h : pieces[j].holes) sea.push_back(clip::kDegrees.path(h));
    }
    if (sea.empty()) { how = "coastline, no sea in tile"; return std::nullopt; }
    how = "coastline";
    return Sea{sea};
}

std::optional<Sea> offlineSea(const Bounds& bounds) {
    static std::once_flag once;
    static std::vector<std::pair<std::array<double, 4>, clip::Paths64>> land;
    std::call_once(once, [] {
        std::ifstream f(palette().gameRoot + "/assets/world/land.geojson", std::ios::binary);
        if (!f) throw std::runtime_error("cannot read assets/world/land.geojson");
        const auto doc = nlohmann::json::parse(f);
        auto addPolygon = [](const nlohmann::json& rings) {
            clip::Paths64 paths;
            std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
            for (const auto& r : rings) {
                std::vector<P2> pts;
                for (const auto& c : r) {
                    pts.push_back({c[0].get<double>(), c[1].get<double>()});
                    box = {std::min(box[0], pts.back().x), std::min(box[1], pts.back().y),
                           std::max(box[2], pts.back().x), std::max(box[3], pts.back().y)};
                }
                paths.push_back(clip::kDegrees.path(pts));
            }
            land.push_back({box, std::move(paths)});
        };
        for (const auto& feature : doc.at("features")) {
            const auto& g = feature.at("geometry");
            if (g.at("type") == "Polygon") addPolygon(g.at("coordinates"));
            else for (const auto& poly : g.at("coordinates")) addPolygon(poly);
        }
    });
    clip::Paths64 near;
    for (const auto& [box, paths] : land)
        if (box[0] <= bounds.east && box[2] >= bounds.west && box[1] <= bounds.north && box[3] >= bounds.south)
            near.insert(near.end(), paths.begin(), paths.end());
    const clip::Paths64 sea = clip::subtract({boxPath(bounds)}, Clipper2Lib::Union(near, Clipper2Lib::FillRule::EvenOdd));
    if (sea.empty()) return std::nullopt;
    return Sea{sea};
}

std::optional<clip::Paths64> tidalWater(const std::vector<OsmWay>& landcover, const std::vector<OsmWay>& maritime) {
    static const char* tidal[] = {"harbour", "dock", "lagoon", "bay", "strait", "fjord", "sound"};
    clip::Paths64 harbourAreas;
    std::vector<std::pair<const Tags*, clip::Paths64>> water;
    std::vector<const OsmWay*> ways;
    for (const auto& w : landcover) ways.push_back(&w);
    for (const auto& w : maritime) ways.push_back(&w);
    for (const OsmWay* way : ways) {
        const Tags& t = way->tags;
        if (!way->closed()) continue;
        const clip::Paths64 poly = clip::bufferRing(way->points, 0.0, clip::kDegrees);
        if (poly.empty()) continue;
        if (tagOr(t, "leisure") == "marina" || tagOr(t, "landuse") == "port" || tagOr(t, "landuse") == "harbour" ||
            tagOr(t, "industrial") == "port" || has(t, "harbour"))
            harbourAreas.insert(harbourAreas.end(), poly.begin(), poly.end());
        if (tagOr(t, "natural") == "water" || !tagOr(t, "water").empty() || tagOr(t, "waterway") == "riverbank")
            water.push_back({&t, poly});
    }
    const clip::Paths64 ports = clip::unite(harbourAreas);
    clip::Paths64 keep;
    for (const auto& [t, poly] : water) {
        bool isTidal = tagOr(*t, "tidal") == "yes";
        for (const char* w : tidal) isTidal |= tagOr(*t, "water") == w;
        if (!isTidal && !ports.empty()) {
            const auto parts = clip::polygons(poly, clip::kDegrees);
            if (!parts.empty()) isTidal = clip::contains(ports, representativePoint(parts.front()), clip::kDegrees);
        }
        if (isTidal) keep.insert(keep.end(), poly.begin(), poly.end());
    }
    if (keep.empty()) return std::nullopt;
    return clip::unite(keep);
}

Cells::Cells(const Bounds& bounds, int size, const Sea* sea, const Landcover& landcover,
             const std::function<double(double, double)>& elevationAt, const clip::Paths64* tidal,
             const std::function<bool(double, double)>& inlandAt)
    : bounds_(bounds), size_(size), sea_(sea) {
    for (int row = 0; row < size; ++row) {
        const double lat = bounds.south + (bounds.north - bounds.south) * (row + 0.5) / size;
        std::vector<int> line;
        for (int col = 0; col < size; ++col) {
            const double lon = bounds.west + (bounds.east - bounds.west) * (col + 0.5) / size;
            if (sea && sea->contains(lon, lat)) line.push_back(2);
            else {
                const std::string* cls = landcover.at(lon, lat);
                if ((cls && *cls == "water") || (inlandAt && inlandAt(lon, lat))) {
                    const double level = elevationAt(lon, lat);
                    const bool tide = tidal && clip::contains(*tidal, P2{lon, lat}, clip::kDegrees);
                    line.push_back(tide || (sea && level <= kSeaLevelWaterMax) ? 2 : 1);
                } else line.push_back(0);
            }
            hasSea_ |= line.back() == 2;
        }
        codes_.push_back(std::move(line));
    }
}

int Cells::at(double lon, double lat) const {
    const int col = int((lon - bounds_.west) / (bounds_.east - bounds_.west) * size_);
    const int row = int((lat - bounds_.south) / (bounds_.north - bounds_.south) * size_);
    // Python's int() truncates toward zero, so a point just outside reads as 0.
    const double fc = (lon - bounds_.west) / (bounds_.east - bounds_.west) * size_;
    const double fr = (lat - bounds_.south) / (bounds_.north - bounds_.south) * size_;
    if (fc <= -1.0 || fr <= -1.0 || col < 0 || row < 0 || col >= size_ || row >= size_) return -1;
    return codes_[size_t(row)][size_t(col)];
}

double Cells::adjust(int row, int col, double height) const {
    if (!hasSea_) return height;
    std::vector<int> around;
    for (int r : {row - 1, row})
        for (int c : {col - 1, col})
            if (0 <= r && r < size_ && 0 <= c && c < size_) around.push_back(codes_[size_t(r)][size_t(c)]);
    bool allSea = !around.empty(), anySea = false, anyLand = false;
    for (int code : around) { allSea &= code == 2; anySea |= code == 2; anyLand |= code == 0; }
    if (allSea) return std::min(height, kSeaFloor);
    if (!anySea && anyLand) return std::max(height, kLandAboveSea);
    return height;
}

std::vector<std::string> Cells::rows() const {
    std::vector<std::string> out;
    for (const auto& line : codes_) {
        std::string s;
        for (int c : line) s += char('0' + c);
        out.push_back(s);
    }
    return out;
}

int Cells::seaCells() const {
    int n = 0;
    for (const auto& line : codes_) for (int c : line) n += c == 2;
    return n;
}

nlohmann::json HarbourStats::json() const {
    return {{"coastline", coastline}, {"seaCells", seaCells}, {"piers", piers}, {"breakwaters", breakwaters},
            {"quays", quays}, {"lighthouses", lighthouses}, {"boats", boats}, {"containers", containers},
            {"berthsRefused", berthsRefused}, {"pilesDropped", pilesDropped}, {"boatsAreInferred", true}};
}

bool isLighthouse(const Tags& tags) { return tagOr(tags, "man_made") == "lighthouse" || tagOr(tags, "building") == "lighthouse"; }

OsmNode tracedLighthouse(const OsmWay& way) {
    std::vector<P2> ring = way.points;
    if (ring.front() == ring.back()) ring.pop_back();
    double lon = 0, lat = 0;
    for (const P2& p : ring) { lon += p.x; lat += p.y; }
    lon /= ring.size(); lat /= ring.size();
    const double k = std::cos(radians(lat));
    double radius = 0;
    for (const P2& p : ring) radius = std::max(radius, std::hypot((p.x - lon) * k, p.y - lat));
    radius *= kMetresPerDegree;
    OsmNode node{way.id, lon, lat, way.tags};
    node.tags["man_made"] = "lighthouse";
    char buf[32]; std::snprintf(buf, sizeof buf, "%.2f", radius);
    node.tags["r1:radius"] = buf;
    return node;
}

Works buildWorks(const std::vector<OsmWay>& maritime, const std::vector<OsmNode>& features, const GroundFn& ground,
                 const Anchor& anchor, const Cells& cells, HarbourStats& stats, bool piles) {
    Works out;
    Mesh deck, riprap(UvMode::Slope), quay(UvMode::Slope);
    std::map<std::string, Mesh> paint;
    auto surfacesAt = [&](double x, double z) {
        const P3 geo = anchor.toGeodetic(x, 0.0, z);
        const double g = ground(geo.x, geo.y).y;
        return std::make_pair(g, cells.hasSea() ? seaY(anchor, geo.x, geo.y) : g);
    };
    for (const OsmWay& way : maritime)
        for (const auto& [points, closed] : clipped(way, cells.bounds())) {
            const std::string kind = tagOr(way.tags, "man_made");
            const std::vector<P2> line = enginePoints(anchor, points);
            if (kind == "pier") {
                ++stats.piers;
                const bool floating = tagOr(way.tags, "floating") == "yes";
                const double freeboard = floating ? 0.45 : 1.3;
                if (closed) {
                    const std::vector<P2> ring(line.begin(), line.end() - 1);
                    double y = -1e300;
                    for (const P2& p : ring) { const auto [g, w] = surfacesAt(p.x, p.y); y = std::max(y, std::max(g, w + freeboard)); }
                    for (const auto& t : triangulate(ring)) {
                        const P2 pa = ring[size_t(t[0])], pb = ring[size_t(t[1])], pc = ring[size_t(t[2])];
                        const UV uv[3] = {{pa.x, -pa.y}, {pb.x, -pb.y}, {pc.x, -pc.y}};
                        deck.addUpTriangle({pa.x, y, pa.y}, {pb.x, y, pb.y}, {pc.x, y, pc.y}, uv);
                    }
                    for (size_t i = 0; i < ring.size(); ++i) {
                        const P2 p0 = ring[i], p1 = ring[(i + 1) % ring.size()];
                        deck.addQuad({p0.x, y - 2.5, p0.y}, {p1.x, y - 2.5, p1.y}, {p1.x, y, p1.y}, {p0.x, y, p0.y});
                    }
                    nlohmann::json pts = nlohmann::json::array();
                    for (const P2& p : ring) pts.push_back({r3(p.x), r3(p.y)});
                    out.decks.push_back({{"y", r3(y)}, {"points", pts}});
                    continue;
                }
                const double half = lengthTag(tag(way.tags, "width"), floating ? 2.5 : 3.0) / 2.0;
                const auto path = subdivide(line, 6.0);
                std::vector<std::pair<double, double>> levels;
                std::vector<double> heights;
                for (const P2& p : path) {
                    levels.push_back(surfacesAt(p.x, p.y));
                    heights.push_back(std::max(levels.back().first, levels.back().second + freeboard));
                }
                std::vector<P2> left, right;
                offsets(path, half, left, right);
                strip(deck, left, right, [&](size_t i) { return heights[i]; });
                for (const auto* side : {&left, &right})
                    for (size_t i = 0; i + 1 < side->size(); ++i) {
                        const P2 a = (*side)[i], b = (*side)[i + 1];
                        deck.addQuad({a.x, heights[i] - 0.35, a.y}, {b.x, heights[i + 1] - 0.35, b.y},
                                     {b.x, heights[i + 1], b.y}, {a.x, heights[i], a.y});
                    }
                if (!floating && piles)
                    for (size_t i = 0; i < levels.size(); i += 2) {
                        const auto [g, w] = levels[i];
                        for (const auto* side : {&left, &right})
                            pile(deck, (*side)[i].x, (*side)[i].y, std::min(g, w) + kSeaFloor, heights[i] - 0.35, 0.16);
                    }
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    nlohmann::json pts = nlohmann::json::array();
                    for (const P2& p : {left[i], left[i + 1], right[i + 1], right[i]}) pts.push_back({r3(p.x), r3(p.y)});
                    out.decks.push_back({{"y", r3((heights[i] + heights[i + 1]) / 2)}, {"points", pts}});
                }
            } else if (kind == "breakwater" || kind == "groyne") {
                ++stats.breakwaters;
                const double crest = kind == "breakwater" ? 2.5 : 1.2;
                const double topHalf = lengthTag(tag(way.tags, "width"), kind == "breakwater" ? 4.0 : 2.0) / 2.0;
                const auto path = subdivide(closed ? std::vector<P2>(line.begin(), line.end() - 1) : line, 8.0);
                std::vector<double> tops, feet;
                for (const P2& p : path) {
                    const auto [g, w] = surfacesAt(p.x, p.y);
                    tops.push_back(std::max(g, w) + crest);
                    feet.push_back(w + kSeaFloor);
                }
                const double baseHalf = topHalf + (crest - kSeaFloor) * 1.5;
                std::vector<P2> tl, tr, bl, br;
                offsets(path, topHalf, tl, tr);
                offsets(path, baseHalf, bl, br);
                strip(riprap, tl, tr, [&](size_t i) { return tops[i]; }, false);
                for (int pass = 0; pass < 2; ++pass) {
                    const auto& outer = pass == 0 ? bl : tr;
                    const auto& inner = pass == 0 ? tl : br;
                    const bool outerIsBase = pass == 0;
                    for (size_t i = 0; i + 1 < path.size(); ++i) {
                        const P2 o0 = outer[i], o1 = outer[i + 1], n0 = inner[i], n1 = inner[i + 1];
                        const double yo0 = outerIsBase ? feet[i] : tops[i], yo1 = outerIsBase ? feet[i + 1] : tops[i + 1];
                        const double yi0 = outerIsBase ? tops[i] : feet[i], yi1 = outerIsBase ? tops[i + 1] : feet[i + 1];
                        riprap.addUpQuad({o0.x, yo0, o0.y}, {o1.x, yo1, o1.y}, {n1.x, yi1, n1.y}, {n0.x, yi0, n0.y});
                    }
                }
            } else if (kind == "quay") {
                ++stats.quays;
                const auto path = subdivide(line, 8.0);
                std::vector<std::pair<double, double>> levels;
                for (const P2& p : path) levels.push_back(surfacesAt(p.x, p.y));
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    const P2 a = path[i], b = path[i + 1];
                    const auto [ga, wa] = levels[i];
                    const auto [gb, wb] = levels[i + 1];
                    quay.addQuad({a.x, wa + kSeaFloor, a.y}, {b.x, wb + kSeaFloor, b.y}, {b.x, std::max(gb, wb + 0.8), b.y},
                                 {a.x, std::max(ga, wa + 0.8), a.y});
                }
            }
        }
    for (const OsmNode& node : features) {
        if (tagOr(node.tags, "man_made") != "lighthouse") continue;
        ++stats.lighthouses;
        lighthouse(paint, node, ground);
    }
    if (!deck.empty())
        out.parts.push_back({"Harbour \xE2\x80\x94 piers", std::move(deck), surfaceMaterial("Pier deck", {0.16, 0.13, 0.10}, 0.85, std::string("deck"))});
    if (!riprap.empty())
        out.parts.push_back({"Harbour \xE2\x80\x94 breakwaters", std::move(riprap),
                             surfaceMaterial("Rock armour", {0.24, 0.23, 0.21}, 0.95, std::string("riprap"), true)});
    if (!quay.empty())
        out.parts.push_back({"Harbour \xE2\x80\x94 quays", std::move(quay),
                             surfaceMaterial("Quay wall", {0.20, 0.20, 0.19}, 0.9, std::string("quay"), true)});
    for (auto& [name, mesh] : paint) {
        std::array<double, 3> colour = name == "glass" ? std::array<double, 3>{0.06, 0.08, 0.09}
                                       : kPaint.count(name) ? kPaint.at(name) : kPaint.at("white");
        Material m;
        m.name = "Lighthouse " + name;
        m.color = {colour[0], colour[1], colour[2], 1.0};
        m.roughness = name == "glass" ? 0.15 : 0.6;
        out.parts.push_back({"Lighthouse \xE2\x80\x94 " + name, std::move(mesh), m});
    }
    return out;
}

std::vector<Berth> planBoats(const OsmData& osm, const Anchor& anchor, const Cells& cells, const RegionProfile& profile,
                             const std::string& climate, HarbourStats& stats) {
    const auto areaList = areas(osm, anchor);
    const auto points = harbourPoints(osm.features, anchor);
    struct Line { int64_t id; std::vector<P2> line; double clearance; std::string fallback; };
    std::vector<Line> lines;
    int piers = 0;
    for (const auto& w : osm.maritime) piers += tagOr(w.tags, "man_made") == "pier";
    const std::string pierDefault = piers >= 6 ? "marina" : "shore";
    auto add = [&](const OsmWay& way, double clearance, const std::string& fallback) {
        for (const auto& [pts, closedPiece] : clipped(way, cells.bounds())) {
            std::vector<P2> line = enginePoints(anchor, pts);
            // A quay drawn with a node every few metres is still one wall.
            if (line.size() > 2) line = clip::simplify(line, 1.5);
            lines.push_back({way.id, line, clearance, fallback});
        }
    };
    for (const auto& way : osm.maritime) {
        const std::string kind = tagOr(way.tags, "man_made");
        if (kind == "pier") add(way, lengthTag(tag(way.tags, "width"), 3.0) / 2.0, pierDefault);
        else if (kind == "quay") add(way, 0.0, "");
    }
    if (!areaList.empty() || !points.empty())
        for (const auto& way : osm.coastlines) add(way, 0.0, "");
    for (const auto& way : osm.landcover) {
        const Tags& t = way.tags;
        if (tagOr(t, "water") == "canal" || tagOr(t, "waterway") == "canal") add(way, 0.0, "canal");
        else if (!areaList.empty() && (tagOr(t, "natural") == "water" || !tagOr(t, "water").empty())) add(way, 0.0, "");
    }
    auto water = [&](double x, double z, bool needSea) {
        const P3 geo = anchor.toGeodetic(x, 0.0, z);
        const int code = cells.at(geo.x, geo.y);
        return needSea ? code == 2 : code > 0;
    };
    std::vector<Berth> placed;
    int ships = 0;
    const Bounds& b = cells.bounds();
    const double halfCell = (b.north - b.south) * kMetresPerDegree / cells.size() / 2.0;
    for (const Line& l : lines) {
        PyRandom rng = seeded(l.id, kSaltBoat);
        for (size_t i = 0; i + 1 < l.line.size(); ++i) {
            const P2 a = l.line[i], c = l.line[i + 1];
            const double seg = dist(a, c);
            if (seg < 6.0) continue;
            const double ux = (c.x - a.x) / seg, uz = (c.y - a.y) / seg;
            const std::string context = contextOf((a.x + c.x) / 2, (a.y + c.y) / 2, areaList, points, l.fallback);
            if (context.empty()) continue;
            const auto mix = mixFor(context, climate, profile);
            static const std::map<std::string, double> fills = {{"marina", 0.85}, {"fishing", 0.60}, {"port", 0.70},
                                                                {"ferry", 0.55}, {"canal", 0.45}, {"shore", 0.30}};
            const double fill = fills.at(context);
            for (double sign : {1.0, -1.0}) {
                const double nx = -uz * sign, nz = ux * sign;
                double t = 2.0;
                while (t < seg - 2.0 && int(placed.size()) < kTileBoatBudget) {
                    std::string name = pickMix(mix, rng);
                    if ((name == "cargo" || name == "liner") && (seg < 130.0 || ships >= kTileShipBudget))
                        name = mix.count("tug") ? "tug" : "motor";
                    auto choices = kindsOf(name);
                    if (choices.empty()) choices = kindsOf("motor");
                    const BoatKind& kind = *choices[rng.randrange(uint32_t(choices.size()))];
                    const double beam = beamOf(kind);
                    if (t + kind.length > seg - 1.0) { t += 3.0; continue; }
                    if (rng.random() > fill) { t += kind.length * 0.7; continue; }
                    const double along = t + kind.length / 2.0, offset = l.clearance + beam / 2.0 + 0.7;
                    const double cx = a.x + ux * along + nx * offset, cz = a.y + uz * along + nz * offset;
                    const double inner = std::min(beam / 2, std::max(l.clearance + 0.7, halfCell) - offset);
                    const bool big = kind.kind == "cargo" || kind.kind == "liner";
                    bool ok = true;
                    for (double f : {-1.0, 0.0, 1.0})
                        for (double g : {inner, beam / 2})
                            ok = ok && water(cx + ux * kind.length / 2 * f + nx * g, cz + uz * kind.length / 2 * f + nz * g, big);
                    if (!ok) { ++stats.berthsRefused; t += 3.0; continue; }
                    const P2 a0{cx - ux * kind.length / 2, cz - uz * kind.length / 2}, a1{cx + ux * kind.length / 2, cz + uz * kind.length / 2};
                    bool clash = false;
                    for (const Berth& o : placed) {
                        const P2 b0{o.x - o.axis.x * o.kind->length / 2, o.z - o.axis.y * o.kind->length / 2};
                        const P2 b1{o.x + o.axis.x * o.kind->length / 2, o.z + o.axis.y * o.kind->length / 2};
                        if (segmentDistance(a0, a1, b0, b1) < (beam + o.beam) / 2 + 0.5) { clash = true; break; }
                    }
                    if (clash) { t += 3.0; continue; }
                    const bool flip = rng.random() < 0.5;
                    placed.push_back({&kind, cx, cz, flip ? P2{-ux, -uz} : P2{ux, uz}, beam});
                    ships += big;
                    ++stats.boats[kind.kind];
                    t += kind.length + (context == "marina" ? 1.5 : 3.0);
                }
            }
        }
    }
    return placed;
}

namespace {
nlohmann::json deckLoad(double hullY, double sx, double s, PyRandom rng) {
    // ship_cargo_c's hatches, in its model's units (bow +Z): span, half-width, height.
    const double z0 = -4.9, z1 = -0.5, halfWidth = 1.5, deckY = 1.58;
    const double w = kContainer[0], h = kContainer[1], l = kContainer[2];
    const int bays = int(std::floor((z1 - z0) * s / (l + 0.3)));
    const int rows = int(std::floor(2 * halfWidth * sx / (w + 0.05)));
    nlohmann::json out = nlohmann::json::array();
    for (int bay = 0; bay < bays; ++bay) {
        const double mz = z0 + (z1 - z0) * (bay + 0.5) / bays;
        for (int row = 0; row < rows; ++row) {
            const double mx = -halfWidth + 2 * halfWidth * (row + 0.5) / rows;
            const int tiers = 1 + int(rng.randrange(3));
            for (int tier = 0; tier < tiers; ++tier) {
                if (int(out.size()) >= kShipContainerBudget) return out;
                const std::string model = kBoatDir + kContainerModels[rng.randrange(3)] + ".glb";
                const ModelBounds& m = modelBounds(model);
                const double cy = hullY + deckY * s + tier * h;
                out.push_back({{"type", "Node"}, {"name", "Box " + std::to_string(out.size())}, {"enabled", true},
                               {"transform", {{"position", {r3(-mx * sx), r3(cy - m.low.y * h / (m.high.y - m.low.y)), r3(-mz * s)}},
                                              {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                              {"scale", {r5(w / (m.high.x - m.low.x)), r5(h / (m.high.y - m.low.y)), r5(l / (m.high.z - m.low.z))}}}},
                               {"importedFrom", model}});
            }
        }
    }
    return out;
}
}  // namespace

std::pair<nlohmann::json, nlohmann::json> boatNodes(const std::vector<Berth>& berths, const Anchor& anchor) {
    nlohmann::json nodes = nlohmann::json::array(), manifest = nlohmann::json::array();
    for (size_t index = 0; index < berths.size(); ++index) {
        const Berth& berth = berths[index];
        const BoatKind& kind = *berth.kind;
        const std::string model = kBoatDir + kind.model + ".glb";
        const ModelBounds& m = modelBounds(model);
        const double s = kind.length / std::max(1e-3, m.high.z - m.low.z);
        const double sx = kind.lengthRatio > 0 ? berth.beam / std::max(1e-3, m.high.x - m.low.x) : s;
        const double height = (m.high.y - m.low.y) * s;
        const P3 geo = anchor.toGeodetic(berth.x, 0.0, berth.z);
        const double y = seaY(anchor, geo.x, geo.y);
        const double heading = pymod(degrees(std::atan2(berth.axis.x, -berth.axis.y)), 360.0);
        const double half = radians(-heading) / 2.0;
        const std::string name = "Boat " + std::to_string(index);
        nlohmann::json node = {
            {"type", "Node"}, {"name", name}, {"enabled", true}, {"groups", {"boat"}},
            {"transform", {{"position", {r3(berth.x), r3(y), r3(berth.z)}},
                           {"rotation", {0.0, std::sin(half), 0.0, std::cos(half)}}, {"scale", {1.0, 1.0, 1.0}}}},
            {"children", {{{"type", "Node"}, {"name", "Hull"}, {"enabled", true},
                           {"transform", {{"position", {0.0, r3(-m.low.y * s - height * kind.draft), 0.0}},
                                          {"rotation", {0.0, 1.0, 0.0, 0.0}}, {"scale", {r5(sx), r5(s), r5(s)}}}},
                           {"importedFrom", model}},
                          {{"type", "Node"}, {"name", "Helm"}, {"enabled", true}}}}};
        if (kind.model == "ship_cargo_c") {
            const double hullY = -m.low.y * s - height * kind.draft;
            for (auto& box : deckLoad(hullY, sx, s, seeded(int64_t(index) + 1, kSaltYard ^ kSaltBoat)))
                node["children"].push_back(std::move(box));
        }
        nodes.push_back(std::move(node));
        manifest.push_back({{"name", name}, {"kind", kind.kind}, {"model", kind.name}, {"x", r3(berth.x)}, {"y", r3(y)},
                            {"z", r3(berth.z)}, {"heading", r3(heading)}, {"length", kind.length},
                            {"beam", r3(berth.beam)}, {"top", kind.top}, {"accel", kind.accel}, {"turn", kind.turn}});
    }
    return {nodes, manifest};
}

nlohmann::json planContainers(const OsmData& osm, const Anchor& anchor, const Cells& cells,
                              const std::vector<Ring>& footprints, const GroundFn& ground, HarbourStats& stats) {
    nlohmann::json nodes = nlohmann::json::array();
    std::vector<const clip::Polygon*> yards;
    const auto areaList = areas(osm, anchor);
    for (const Area& a : areaList)
        if (a.context == "port" && !a.poly.empty()) yards.push_back(&a.poly.front());
    if (yards.empty()) return nodes;
    // The footprints grown by two metres: inside one, or within 2 m of its edge.
    auto blocked = [&](P2 p) {
        for (const Ring& r : footprints) {
            if (r.size() < 3) continue;
            std::vector<P2> closedRing(r.begin(), r.end());
            closedRing.push_back(r.front());
            if (pointInPolygon(p, r) || clip::distance(closedRing, p) < 2.0) return true;
        }
        return false;
    };
    const double w = kContainer[0], h = kContainer[1], l = kContainer[2];
    for (size_t yi = 0; yi < yards.size(); ++yi) {
        const clip::Polygon& yard = *yards[yi];
        PyRandom rng = seeded(int64_t(yi) + 1, kSaltYard);
        // Clockwise, as GEOS drew the yards: the rows run the way they always did.
        Ring ring = yard.outer;
        if (polygonArea(ring) > 0) std::reverse(ring.begin(), ring.end());
        ring.push_back(ring.front());
        size_t longest = 0;
        for (size_t i = 0; i + 1 < ring.size(); ++i)
            if (dist(ring[i], ring[i + 1]) > dist(ring[longest], ring[longest + 1])) longest = i;
        double ux = ring[longest + 1].x - ring[longest].x, uz = ring[longest + 1].y - ring[longest].y;
        double n = std::hypot(ux, uz);
        if (n == 0) n = 1.0;
        ux /= n; uz /= n;
        const double yaw = std::atan2(ux, uz);
        double minx = 1e300, minz = 1e300, maxx = -1e300, maxz = -1e300;
        for (const P2& p : yard.outer) { minx = std::min(minx, p.x); maxx = std::max(maxx, p.x); minz = std::min(minz, p.y); maxz = std::max(maxz, p.y); }
        const double stepA = l + 1.5, stepB = w + 0.4;
        for (int i = 0; i < int((maxx - minx) / stepB) + 1; ++i)
            for (int j = 0; j < int((maxz - minz) / stepA) + 1; ++j) {
                if (int(nodes.size()) >= kTileContainerBudget) break;
                const double ca = (j - (maxz - minz) / stepA / 2) * stepA, cb = (i - (maxx - minx) / stepB / 2) * stepB;
                const double cx = (minx + maxx) / 2 + ux * ca - uz * cb, cz = (minz + maxz) / 2 + uz * ca + ux * cb;
                bool inYard = pointInPolygon({cx, cz}, yard.outer);
                for (const Ring& hole : yard.holes) if (pointInPolygon({cx, cz}, hole)) inYard = false;
                if (!inYard || blocked({cx, cz})) continue;
                const P3 geo = anchor.toGeodetic(cx, 0.0, cz);
                if (cells.at(geo.x, geo.y) != 0 || rng.random() > 0.55) continue;
                const double gy = ground(geo.x, geo.y).y;
                const int levels = 1 + int(rng.randrange(4));
                for (int level = 0; level < levels; ++level) {
                    const std::string model = kBoatDir + kContainerModels[rng.randrange(3)] + ".glb";
                    const ModelBounds& m = modelBounds(model);
                    const double s = l / std::max(1e-3, m.high.z - m.low.z), sx = w / std::max(1e-3, m.high.x - m.low.x);
                    const double sy = h / std::max(1e-3, m.high.y - m.low.y), half = yaw / 2.0;
                    nodes.push_back({{"type", "Node"}, {"name", "Container " + std::to_string(nodes.size())}, {"enabled", true},
                                     {"transform", {{"position", {r3(cx), r3(gy - m.low.y * sy + level * h), r3(cz)}},
                                                    {"rotation", {0.0, std::sin(half), 0.0, std::cos(half)}},
                                                    {"scale", {r5(sx), r5(sy), r5(s)}}}},
                                     {"importedFrom", model}});
                    ++stats.containers;
                }
            }
    }
    return nodes;
}

nlohmann::json seaNode(const Bounds& bounds, const Anchor& anchor, const std::string& name) {
    double half = 0;
    for (const P2& c : {P2{bounds.west, bounds.south}, P2{bounds.east, bounds.north}, P2{bounds.west, bounds.north},
                        P2{bounds.east, bounds.south}}) {
        const P3 e = anchor.toEngine(c.x, c.y, 0.0);
        half = std::max(half, std::max(std::abs(e.x), std::abs(e.z)));
    }
    return {{"type", "Water"}, {"name", name}, {"size", pyround(half + 1.0, 2)}, {"amplitude", 0.12},
            {"wavelength", 9.0}, {"shoreMode", 0},
            {"transform", {{"position", {0.0, 0.0, 0.0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}};
}

const ModelBounds& modelBounds(const std::string& projectPath) {
    static std::mutex lock;
    static std::map<std::string, ModelBounds> cache;
    std::lock_guard<std::mutex> guard(lock);
    auto it = cache.find(projectPath);
    if (it != cache.end()) return it->second;
    const std::string path = palette().gameRoot + "/" + projectPath;
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read model " + projectPath);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    nlohmann::json doc;
    if (bytes.size() >= 20 && std::string(bytes.data(), 4) == "glTF") {
        uint32_t length = 0;
        std::memcpy(&length, bytes.data() + 12, 4);
        doc = nlohmann::json::parse(bytes.begin() + 20, bytes.begin() + 20 + length);
    } else {
        doc = nlohmann::json::parse(bytes.begin(), bytes.end());
    }
    ModelBounds m{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
    for (const auto& mesh : doc.value("meshes", nlohmann::json::array()))
        for (const auto& primitive : mesh.value("primitives", nlohmann::json::array())) {
            auto attributes = primitive.find("attributes");
            if (attributes == primitive.end() || !attributes->contains("POSITION")) continue;
            const auto& accessor = doc.at("accessors").at(attributes->at("POSITION").get<size_t>());
            if (!accessor.contains("min") || !accessor.contains("max")) continue;
            m.low = {std::min(m.low.x, accessor["min"][0].get<double>()), std::min(m.low.y, accessor["min"][1].get<double>()),
                     std::min(m.low.z, accessor["min"][2].get<double>())};
            m.high = {std::max(m.high.x, accessor["max"][0].get<double>()), std::max(m.high.y, accessor["max"][1].get<double>()),
                      std::max(m.high.z, accessor["max"][2].get<double>())};
        }
    if (m.high.y <= m.low.y) throw std::runtime_error(projectPath + " declares no usable extent");
    return cache[projectPath] = m;
}

}  // namespace r1
