#include "streets.hpp"

#include "clip.hpp"
#include "palette.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <cstdint>
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

Drape::Drape(const ElevationGrid& elevations, const Anchor& anchor,
             const std::function<double(int, int, double)>& adjust)
    : bounds_(elevations.bounds), anchor_(anchor), size_(kTerrainMeshSize - 1) {
    for (int row = 0; row <= size_; ++row)
        for (int col = 0; col <= size_; ++col) {
            const double lo = bounds_.west + (bounds_.east - bounds_.west) * col / size_;
            const double la = bounds_.south + (bounds_.north - bounds_.south) * row / size_;
            double height = elevations.sample(lo, la);
            if (adjust) height = adjust(row, col, height);
            grid_.push_back(anchor.toEngine(lo, la, height));
        }
}

clip::Path64 Drape::outline() const {
    auto g = [&](int row, int col) { return grid_[size_t(row * (size_ + 1) + col)]; };
    std::vector<P2> ring;
    for (int col = 0; col < size_; ++col) ring.push_back({g(0, col).x, g(0, col).z});
    for (int row = 0; row < size_; ++row) ring.push_back({g(row, size_).x, g(row, size_).z});
    for (int col = size_; col > 0; --col) ring.push_back({g(size_, col).x, g(size_, col).z});
    for (int row = size_; row > 0; --row) ring.push_back({g(row, 0).x, g(row, 0).z});
    return clip::kMetres.path(ring);
}

// The height of the drawn terrain under an engine point. Asked of the
// point itself, never of the triangle it was clipped against: a corner
// cut on the edge between two terrain triangles is rounded to the
// millimetre grid, and each triangle would extrapolate it differently --
// two heights for one corner is a crack in the road and a vertex twice.
double Drape::heightAt(P2 q) const {
    auto g = [&](int row, int col) { return grid_[size_t(row * (size_ + 1) + col)]; };
    const P3 geo = anchor_.toGeodetic(q.x, 0.0, q.y);
    double u = std::max(0.0, std::min(1.0, (geo.x - bounds_.west) / (bounds_.east - bounds_.west))) * size_;
    double v = std::max(0.0, std::min(1.0, (geo.y - bounds_.south) / (bounds_.north - bounds_.south))) * size_;
    const int col = std::min(size_ - 1, int(u)), row = std::min(size_ - 1, int(v));
    u -= col; v -= row;
    const P3 a = g(row, col), b = u >= v ? g(row, col + 1) : g(row + 1, col + 1), c = u >= v ? g(row + 1, col + 1) : g(row + 1, col);
    const double denom = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    const double wa = ((b.z - c.z) * (q.x - c.x) + (c.x - b.x) * (q.y - c.z)) / denom;
    const double wb = ((c.z - a.z) * (q.x - c.x) + (a.x - c.x) * (q.y - c.z)) / denom;
    return wa * a.y + wb * b.y + (1 - wa - wb) * c.y;
}

// A surface triangle never crosses a terrain triangle edge, so every road
// lies in the plane of the ground drawn beneath it. The regions are cut to
// a row first and a cell second, so each triangle clips only what is near.
void Drape::lay(const clip::Paths64& region, double lift, Mesh& mesh) const {
    if (region.empty()) return;
    auto g = [&](int row, int col) { return grid_[size_t(row * (size_ + 1) + col)]; };
    auto footprint = [](std::initializer_list<P3> pts) {
        clip::Path64 path;
        for (const P3& p : pts) path.push_back(clip::kMetres.at({p.x, p.z}));
        return path;
    };
    // Integer boxes of paths: a clip of shapes whose boxes are apart is empty.
    struct Extent { int64_t x0, y0, x1, y1; };
    auto extent = [](const clip::Path64& path) {
        Extent e{INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
        for (const auto& p : path) { e.x0 = std::min(e.x0, p.x); e.y0 = std::min(e.y0, p.y); e.x1 = std::max(e.x1, p.x); e.y1 = std::max(e.y1, p.y); }
        return e;
    };
    auto extents = [&](const clip::Paths64& paths) {
        std::vector<Extent> out;
        for (const auto& path : paths) out.push_back(extent(path));
        return out;
    };
    auto meets = [](const std::vector<Extent>& paths, const Extent& e) {
        return std::any_of(paths.begin(), paths.end(), [&](const Extent& p) { return p.x0 <= e.x1 && e.x0 <= p.x1 && p.y0 <= e.y1 && e.y0 <= p.y1; });
    };
    const std::vector<Extent> regionExtents = extents(region);
    for (int row = 0; row < size_; ++row) {
        // Broad phase only: include every projected grid point, since a row
        // on the curved Earth need not lie between its two end points.
        double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
        for (int col = 0; col <= size_; ++col)
            for (int r : {row, row + 1}) {
                const P3 p = g(r, col);
                x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
                z0 = std::min(z0, p.z); z1 = std::max(z1, p.z);
            }
        const clip::Path64 band = clip::kMetres.path({{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}});
        // Clipper sweeps along y: a path wholly above or below the band adds
        // no scanline, edge or winding inside it, so the strip is the same
        // without it. Only those are left out, in their order.
        const Extent bandExtent = extent(band);
        clip::Paths64 crossing;
        for (size_t i = 0; i < region.size(); ++i)
            if (regionExtents[i].y0 <= bandExtent.y1 && bandExtent.y0 <= regionExtents[i].y1) crossing.push_back(region[i]);
        const clip::Paths64 strip = clip::intersect(crossing, {band});
        if (strip.empty()) continue;
        const std::vector<Extent> stripExtents = extents(strip);
        for (int col = 0; col < size_; ++col) {
            const P3 sw = g(row, col), se = g(row, col + 1), ne = g(row + 1, col + 1), nw = g(row + 1, col);
            const clip::Path64 quad = footprint({sw, se, ne, nw});
            if (!meets(stripExtents, extent(quad))) continue;
            const clip::Paths64 cell = clip::intersect(strip, {quad});
            if (cell.empty()) continue;
            const std::vector<Extent> cellExtents = extents(cell);
            for (const auto& tri : {std::array<P3, 3>{sw, se, ne}, std::array<P3, 3>{sw, ne, nw}}) {
                const P3 a = tri[0], b = tri[1], c = tri[2];
                const clip::Path64 triangle = clip::kMetres.path({{a.x, a.z}, {b.x, b.z}, {c.x, c.z}});
                if (!meets(cellExtents, extent(triangle))) continue;
                const clip::Paths64 piece = clip::intersect(cell, {triangle});
                if (piece.empty()) continue;
                auto vertex = [&](P2 q) { return P3{q.x, heightAt(q) + lift, q.y}; };
                for (const clip::Polygon& part : clip::polygons(piece)) {
                    if (std::abs(polygonArea(part.outer)) < 1e-6) continue;
                    for (const auto& t : clip::triangles(part))
                        mesh.addUpTriangle(vertex(t[0]), vertex(t[1]), vertex(t[2]));
                }
            }
        }
    }
}

StreetOutput buildStreets(const std::vector<OsmWay>& roads, const std::vector<OsmNode>& features,
                          const ElevationGrid& elevations, const Anchor& anchor, const std::vector<Ring>& footprints,
                          const std::function<double(int, int, double)>& adjust) {
    using clip::Paths64;
    int sidewalkTagged = 0, sidewalkInferred = 0, zebras = 0, widthsTagged = 0, widthsInferred = 0, graded = 0;
    // A way comes cut into its segments, and a strip per segment left a
    // notch on the outside of every bend: the segments of one way are joined
    // back into its line, which is buffered with round joins.
    struct Line { const OsmWay* way; std::vector<P2> points; };
    std::vector<Line> axes;
    for (const OsmWay& road : roads) {
        const Tags& tags = road.tags;
        if (tagOr(tags, "area") == "yes" || tagOr(tags, "footway") == "crossing") continue;
        // Bridges and embankments are built off the ground (gen/bridges).
        if (taggedYes(tags, "tunnel") || taggedYes(tags, "bridge") || has(tags, "r1:raised")) { ++graded; continue; }
        std::vector<P2> line;
        for (const P2& p : road.points) {
            const P3 e = anchor.toEngine(p.x, p.y, 0.0);
            if (line.empty() || dist(line.back(), {e.x, e.z}) > 1e-9) line.push_back({e.x, e.z});
        }
        if (line.size() < 2) continue;
        Line* previous = axes.empty() ? nullptr : &axes.back();
        if (previous && previous->way->id == road.id && previous->way->tags == tags &&
            dist(previous->points.back(), line.front()) < 1e-6)
            previous->points.insert(previous->points.end(), line.begin() + 1, line.end());
        else
            axes.push_back({&road, std::move(line)});
    }
    // Where two lines meet end to end, or one ends on another, the end is
    // rounded: the two strips of a bend split between two ways then close
    // the corner as one line's join would.
    auto keyOf = [](P2 p) { return std::make_pair(std::llround(p.x * 1000), std::llround(p.y * 1000)); };
    std::map<std::pair<long long, long long>, int> meetings;
    for (const Line& l : axes)
        for (const P2& p : l.points) ++meetings[keyOf(p)];
    auto roundEnd = [&](const std::vector<P2>& line, bool atEnd, double half, Paths64& into) {
        const P2 end = atEnd ? line.back() : line.front();
        if (meetings[keyOf(end)] < 2) return;
        const P2 before = atEnd ? line[line.size() - 2] : line[1];
        const double length = dist(before, end);
        if (length < 1e-9) return;
        const P2 d{(end.x - before.x) / length, (end.y - before.y) / length}, n{-d.y, d.x};
        std::vector<P2> cap;
        for (int k = 0; k <= 16; ++k) {
            const double t = -kPi / 2 + kPi * k / 16;
            cap.push_back({end.x + half * (std::sin(t) * n.x + std::cos(t) * d.x),
                           end.y + half * (std::sin(t) * n.y + std::cos(t) * d.y)});
        }
        into.push_back(clip::kMetres.path(cap));
    };
    Paths64 roadPolys, cobblePolys, walkPolys;
    struct MotorLine { int64_t id; std::vector<P2> line; double half; };
    std::vector<MotorLine> motorLines;
    for (const Line& l : axes) {
        const Tags& tags = l.way->tags;
        const std::vector<P2>& line = l.points;
        if (clip::length(line) < 0.1) continue;
        const double half = roadWidth(tags) * 0.5;
        const Paths64 strip = clip::bufferLineRoundJoins(line, half);
        ++(has(tags, "width") ? widthsTagged : widthsInferred);
        const bool motor = kMotor.count(tagOr(tags, "highway")) > 0;
        Paths64& target = !motor ? walkPolys : kCobbled.count(tagOr(tags, "surface")) ? cobblePolys : roadPolys;
        target.insert(target.end(), strip.begin(), strip.end());
        roundEnd(line, false, half, target);
        roundEnd(line, true, half, target);
        if (!motor) continue;
        motorLines.push_back({l.way->id, line, half});
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
    // the grid itself here. The tile keeps what lies inside it; its
    // neighbour lays the rest of a street that crosses the edge.
    const Drape drape(elevations, anchor, adjust);
    const Paths64 tile{drape.outline()};
    const Paths64 asphalt = clip::intersect(clip::subtract(clip::unite(roadPolys), buildings), tile);
    const Paths64 cobbles =
        clip::intersect(clip::subtract(clip::subtract(clip::unite(cobblePolys), buildings), asphalt), tile);
    const Paths64 paving = clip::intersect(
        clip::subtract(clip::subtract(clip::subtract(clip::unite(walkPolys), asphalt), cobbles), buildings), tile);
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
        // One past the edge is drawn where it reaches in, and counted by its own tile.
        const Bounds& box = elevations.bounds;
        const bool inside = box.west <= f.lon && f.lon <= box.east && box.south <= f.lat && f.lat <= box.north;
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
        if (inside) ++zebras;
    }
    const Paths64 paint = clip::intersect(clip::unite(stripes), asphalt);

    Mesh roadMesh(UvMode::Planar), cobbleMesh(UvMode::Planar), walkMesh(UvMode::Planar), paintMesh, kerbMesh;
    struct Surface { const Paths64* region; Mesh* mesh; double lift; };
    const Surface surfaces[] = {{&asphalt, &roadMesh, 0.06}, {&cobbles, &cobbleMesh, 0.06},
                                {&paving, &walkMesh, 0.21}, {&paint, &paintMesh, 0.075}};
    for (const Surface& s : surfaces) drape.lay(*s.region, s.lift, *s.mesh);

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
            // On the ground the street was laid on, so the kerb meets both.
            auto p = [&](double t, double lift) {
                const P2 q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                return P3{q.x, drape.heightAt(q) + lift, q.y};
            };
            for (int k = 0; k < n; ++k)
                kerbMesh.addQuad(p(double(k) / n, 0.06), p(double(k + 1) / n, 0.06), p(double(k + 1) / n, 0.21), p(double(k) / n, 0.21));
        }

    StreetOutput out;
    out.ground = clip::unite(carriageway, paving);
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
