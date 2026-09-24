#include "airports.hpp"

#include "buildings.hpp"
#include "clip.hpp"
#include "palette.hpp"
#include "streets.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <set>

namespace r1 {

namespace {
using clip::Paths64;

// Above the streets' 6 cm: seen from a cockpit a kilometre away, a surface a
// few centimetres over the ground is what keeps it from shimmering into it.
constexpr double kPavedLift = 0.08, kPaintLift = 0.11;
// A draw-call budget, not a memory one: every aircraft is a shared model,
// but its ten or so materials are ten draws, and nine tiles are in view.
constexpr size_t kTileAircraft = 12;
constexpr size_t kApronAircraft = 12;
// Seeds: one purpose each (common.hpp `seeded`).
constexpr int64_t kSaltStand = 0x57A9D, kSaltApron = 0xA9A0, kSaltBase = 0xBA5E, kSaltPad = 0x4E11;

const std::set<std::string> kUnpaved = {"grass", "dirt", "ground", "earth", "sand", "gravel", "unpaved",
                                        "compacted", "fine_gravel", "mud", "snow", "ice", "salt"};
// What `military=*` names a base; ranges and danger areas are not one.
const std::set<std::string> kBases = {"airfield", "base", "barracks", "naval_base"};
const std::set<std::string> kNotBases = {"danger_area", "range", "training_area", "checkpoint", "trench",
                                         "bunker", "obstacle_course", "nuclear_explosion_site"};

std::vector<P2> engineLine(const std::vector<P2>& points, const Anchor& anchor) {
    std::vector<P2> out;
    for (const P2& p : points) {
        const P3 e = anchor.toEngine(p.x, p.y, 0.0);
        out.push_back({e.x, e.z});
    }
    return out;
}
Ring engineRing(const OsmWay& way, const Anchor& anchor) {
    Ring r = engineLine(way.points, anchor);
    if (r.size() > 1 && r.front() == r.back()) r.pop_back();
    return r;
}
double widthOf(const Tags& tags, double fallback) {
    const std::string* raw = tag(tags, "width");
    if (!raw) return fallback;
    const auto metres = taggedLength(*raw);
    return metres && *metres >= 3.0 && *metres <= 120.0 ? *metres : fallback;
}
bool paved(const Tags& tags) { return !kUnpaved.count(tagOr(tags, "surface")); }
std::string aeroway(const OsmWay& w) { return tagOr(w.tags, "aeroway"); }
bool area(const OsmWay& w) { return w.closed() && (tagOr(w.tags, "area") == "yes" || aeroway(w) == "apron" ||
                                                   aeroway(w) == "helipad" || aeroway(w) == "aerodrome"); }

// Engine (x, z) from a compass heading: east is +x and north is -z.
P2 forwardOf(double heading) { return {std::sin(radians(heading)), -std::cos(radians(heading))}; }
P2 rightOf(double heading) { return {std::cos(radians(heading)), std::sin(radians(heading))}; }
double headingOf(P2 d) { return pymod(degrees(std::atan2(d.x, -d.y)), 360.0); }
P2 plus(P2 a, P2 b, double k = 1.0) { return {a.x + b.x * k, a.y + b.y * k}; }

// Counter-clockwise, whatever order it was traced in. The unions are
// non-zero, so two overlapping paths wound opposite ways would cancel into a
// hole: every raw path goes through here before it meets another.
clip::Path64 positive(clip::Path64 path) {
    if (Clipper2Lib::Area(path) < 0) std::reverse(path.begin(), path.end());
    return path;
}

// A rectangle `along` long and `across` wide, centred on c, turned to `heading`.
clip::Path64 rect(P2 c, double heading, double along, double across) {
    const P2 f = forwardOf(heading), r = rightOf(heading);
    return positive(clip::kMetres.path({plus(plus(c, f, along / 2), r, across / 2), plus(plus(c, f, along / 2), r, -across / 2),
                                        plus(plus(c, f, -along / 2), r, -across / 2), plus(plus(c, f, -along / 2), r, across / 2)}));
}
Paths64 circle(P2 c, double radius, int sides = 24) {
    std::vector<P2> ring;
    for (int i = 0; i < sides; ++i)
        ring.push_back({c.x + radius * std::cos(kTau * i / sides), c.y + radius * std::sin(kTau * i / sides)});
    return {positive(clip::kMetres.path(ring))};
}

// ── paint ───────────────────────────────────────────────────────────────────

// Seven segments and three letters: enough for every runway designator.
// Glyph space is 3 m wide and 9 m tall, x to the pilot's right and y ahead.
std::vector<std::array<double, 4>> glyph(char c) {
    const double w = 3.0, h = 9.0, t = 0.9;
    const std::array<double, 4> a{0, h - t, w, h}, b{w - t, h / 2, w, h}, cc{w - t, 0, w, h / 2}, d{0, 0, w, t},
        e{0, 0, t, h / 2}, f{0, h / 2, t, h}, g{0, h / 2 - t / 2, w, h / 2 + t / 2};
    switch (c) {
        case '0': return {a, b, cc, d, e, f};
        case '1': return {b, cc};
        case '2': return {a, b, g, e, d};
        case '3': return {a, b, g, cc, d};
        case '4': return {f, g, b, cc};
        case '5': return {a, f, g, cc, d};
        case '6': return {a, f, g, e, d, cc};
        case '7': return {a, b, cc};
        case '8': return {a, b, cc, d, e, f, g};
        case '9': return {a, b, cc, d, f, g};
        case 'L': return {f, e, d};
        case 'C': return {a, f, e, d};
        case 'R': return {a, b, f, e, g};  // and a leg, below
        default: return {};
    }
}

struct Paint {
    Paths64 white, yellow;
    // A quad in a frame: origin o, `ahead` along f, `side` along r.
    static clip::Path64 quad(P2 o, P2 f, P2 r, double s0, double s1, double x0, double x1) {
        return positive(clip::kMetres.path({plus(plus(o, f, s0), r, x0), plus(plus(o, f, s0), r, x1),
                                            plus(plus(o, f, s1), r, x1), plus(plus(o, f, s1), r, x0)}));
    }
    void text(const std::string& s, P2 o, P2 f, P2 r, double at) {
        const double width = 3.0, gap = 1.5;
        const double total = s.size() * width + (s.size() - 1) * gap;
        for (size_t i = 0; i < s.size(); ++i) {
            const double x = -total / 2 + i * (width + gap);
            for (const auto& seg : glyph(s[i])) white.push_back(quad(o, f, r, at + seg[1], at + seg[3], x + seg[0], x + seg[2]));
            if (s[i] == 'R')
                white.push_back(positive(clip::kMetres.path({plus(plus(o, f, at + 4.5), r, x + 0.7), plus(plus(o, f, at + 4.5), r, x + 1.8),
                                                             plus(plus(o, f, at), r, x + width), plus(plus(o, f, at), r, x + width - 1.1)})));
        }
    }
};

struct Runway {
    const OsmWay* way;
    std::vector<P2> line;  // engine, the whole way
    double length, width;
    bool near;  // drawn here; the others only say what the airport receives
};

// The markings of one runway, from both of its thresholds (ICAO Annex 14 in
// its simplest form): threshold stripes, designator, aiming points, dashed
// centre line, edge lines on the wide ones.
void paintRunway(const Runway& rw, Paint& paint) {
    const double length = rw.length, half = rw.width / 2;
    if (length < 300.0 || rw.line.size() < 2) return;
    for (int end = 0; end < 2; ++end) {
        const P2 o = end == 0 ? rw.line.front() : rw.line.back();
        const P2 next = end == 0 ? clip::interpolate(rw.line, 60.0) : clip::interpolate(rw.line, length - 60.0);
        const double n = dist(o, next);
        if (n < 1.0) continue;
        const P2 f{(next.x - o.x) / n, (next.y - o.y) / n}, r{-f.y, f.x};
        // Threshold stripes: 1.8 m bars 30 m long, a 3.6 m pitch, a gap on the axis.
        const int pairs = std::max(2, int(rw.width / 3.75) / 2);
        for (int k = 0; k < pairs; ++k)
            for (double side : {-1.0, 1.0}) {
                const double x0 = side * (1.8 + k * 3.6), x1 = x0 + side * 1.8;
                if (std::abs(x1) > half - 1.0) continue;
                paint.white.push_back(Paint::quad(o, f, r, 6.0, 36.0, std::min(x0, x1), std::max(x0, x1)));
            }
        const double bearing = headingOf(f);
        const std::string name = runwayDesignator(tagOr(rw.way->tags, "ref"), bearing);
        std::string digits, letter;
        for (char c : name) (std::isdigit((unsigned char)c) ? digits : letter) += c;
        double at = 48.0;
        if (!letter.empty() && letter.size() == 1) { paint.text(letter, o, f, r, at); at += 15.0; }
        paint.text(digits, o, f, r, at);
        // Aiming points 300 m in, on runways long enough to land on properly.
        if (length > 1500.0)
            for (double side : {-1.0, 1.0})
                paint.white.push_back(Paint::quad(o, f, r, 300.0, 345.0, side * half * 0.2, side * half * (0.2 + 0.2)));
    }
    // The centre line, 30 m dashes on 50, between the two designators.
    for (double s = 90.0; s + 30.0 < length - 90.0; s += 50.0) {
        const P2 a = clip::interpolate(rw.line, s), b = clip::interpolate(rw.line, s + 30.0);
        const double n = dist(a, b);
        if (n < 1e-6) continue;
        const P2 f{(b.x - a.x) / n, (b.y - a.y) / n}, r{-f.y, f.x};
        paint.white.push_back(Paint::quad(a, f, r, 0.0, n, -0.45, 0.45));
    }
    if (rw.width >= 30.0) {
        const Paths64 edge = clip::subtract(clip::bufferLine(rw.line, half - 0.5), clip::bufferLine(rw.line, half - 1.4));
        paint.white.insert(paint.white.end(), edge.begin(), edge.end());
    }
}

// ── stands ──────────────────────────────────────────────────────────────────

struct Stand {
    int64_t id;
    P2 at;              // where the aircraft's centre goes, engine
    double heading;     // nose, compass degrees
    bool faced;         // the heading is measured (a stand line) or found (a building), not drawn
    std::string source;
    double room = 1e9;  // metres to the next stand: wingspans have to fit
};

struct Placed {
    const AircraftType* type;
    P2 at;
    double heading, y;
    std::string source;
    char how;  // 't' a surveyed stand, 'i' an inferred one, 'p' a helipad, 'b' a base
};

class Airfield {
public:
    Airfield(const OsmData& osm, const Tile& tile, const Anchor& anchor, const std::function<bool(double, double)>& wet)
        : osm_(osm), anchor_(anchor), wet_(wet) {
        const Bounds b = tile.bounds();
        // The neighbourhood this tile can see: what is placed near its edges
        // must be decided the same way by both tiles, so nothing here is cut
        // to the tile until the very end.
        const double margin = 0.01;
        near_ = {b.south - margin, b.west - margin / std::max(0.2, std::cos(radians(b.south))),
                 b.north + margin, b.east + margin / std::max(0.2, std::cos(radians(b.south)))};
        std::vector<Bounds> interest;
        auto note = [&](const std::vector<P2>& points) {
            Bounds box{1e300, 1e300, -1e300, -1e300};
            for (const P2& q : points) {
                box.west = std::min(box.west, q.x); box.east = std::max(box.east, q.x);
                box.south = std::min(box.south, q.y); box.north = std::max(box.north, q.y);
            }
            // Two hundred metres around, for the buildings a wing could reach.
            const double m = 0.002;
            interest.push_back({box.south - m, box.west - 2 * m, box.north + m, box.east + 2 * m});
        };
        for (const OsmWay& w : osm.military) {
            if (!touches(w)) continue;
            const std::string kind = tagOr(w.tags, "military");
            if (kNotBases.count(kind)) continue;
            if (tagOr(w.tags, "landuse") != "military" && !kBases.count(kind)) continue;
            military_.push_back({&w, engineRing(w, anchor)});
            note(w.points);
        }
        for (const OsmWay& w : osm.aeroways) {
            if (!touches(w)) continue;
            if (aeroway(w) == "aerodrome") {
                if (w.closed() && (tagOr(w.tags, "aerodrome") == "military" || tagOr(w.tags, "aerodrome:type") == "military" ||
                                   tagOr(w.tags, "military") == "airfield"))
                    military_.push_back({&w, engineRing(w, anchor)});
                continue;  // the fence line, not where anything parks
            }
            relevant_ = true;
            note(w.points);
        }
        for (const OsmNode& n : osm.features)
            if (has(n.tags, "aeroway") && near_.south <= n.lat && n.lat <= near_.north && near_.west <= n.lon && n.lon <= near_.east) {
                relevant_ = true;
                note({{n.lon, n.lat}});
            }
        relevant_ |= !military_.empty();
        if (!relevant_) return;
        // Only the buildings near something that parks: a city tile with a
        // barracks in one corner does not union ten thousand roofs for it.
        for (const OsmWay& w : osm.buildings) {
            const P2 q = w.points.front();
            bool wanted = false;
            for (const Bounds& i : interest) wanted |= i.west <= q.x && q.x <= i.east && i.south <= q.y && q.y <= i.north;
            if (!wanted) continue;
            const Ring r = engineRing(w, anchor);
            if (r.size() < 3) continue;
            const Paths64 p = clip::bufferRing(r, 0.0);
            buildings_.insert(buildings_.end(), p.begin(), p.end());
        }
        buildings_ = clip::unite(buildings_);
        buildingPolys_ = clip::polygons(buildings_);
    }
    bool relevant() const { return relevant_; }

    // Whether the way's box meets the neighbourhood's: a base or an
    // aerodrome drawn around the whole tile touches it with no vertex in it.
    bool touches(const OsmWay& w) const {
        double west = 1e300, east = -1e300, south = 1e300, north = -1e300;
        for (const P2& q : w.points) {
            west = std::min(west, q.x); east = std::max(east, q.x);
            south = std::min(south, q.y); north = std::max(north, q.y);
        }
        return west <= near_.east && east >= near_.west && south <= near_.north && north >= near_.south;
    }
    bool military(P2 p) const {
        for (const auto& [way, ring] : military_) if (ring.size() >= 3 && pointInPolygon(p, ring)) return true;
        return false;
    }
    double clearance(P2 p) const { return buildingPolys_.empty() ? 1e9 : clip::distance(buildingPolys_, p); }
    const Paths64& buildings() const { return buildings_; }
    const std::vector<std::pair<const OsmWay*, Ring>>& bases() const { return military_; }
    bool wet(P2 p) const {
        const P3 g = anchor_.toGeodetic(p.x, 0.0, p.y);
        return wet_(g.x, g.y);
    }
    // The nearest point of any building within `reach`, if there is one.
    std::optional<P2> nearestBuilding(P2 p, double reach) const {
        std::optional<P2> best;
        double bestD = reach;
        for (const clip::Polygon& poly : buildingPolys_) {
            const Ring& r = poly.outer;
            for (size_t i = 0; i < r.size(); ++i) {
                const P2 a = r[i], b = r[(i + 1) % r.size()];
                const double dx = b.x - a.x, dz = b.y - a.y, len = dx * dx + dz * dz;
                const double t = len > 0 ? std::max(0.0, std::min(1.0, ((p.x - a.x) * dx + (p.y - a.y) * dz) / len)) : 0.0;
                const P2 q{a.x + t * dx, a.y + t * dz};
                const double d = dist(p, q);
                if (d < bestD) { bestD = d; best = q; }
            }
        }
        return best;
    }

private:
    const OsmData& osm_;
    const Anchor& anchor_;
    const std::function<bool(double, double)>& wet_;
    Bounds near_;
    bool relevant_ = false;
    Paths64 buildings_;
    std::vector<clip::Polygon> buildingPolys_;
    std::vector<std::pair<const OsmWay*, Ring>> military_;
};

// What an airport whose longest runway is `runway` metres receives, largest
// first, drawn once per stand. A 900 m strip takes a business jet, a
// regional airport an airliner, an intercontinental one the wide-bodies.
std::vector<const AircraftType*> fleetFor(double runway, PyRandom& rng) {
    std::vector<std::string> names;
    const double roll = rng.random();
    if (runway >= 2800.0) names = roll < 0.4 ? std::vector<std::string>{"widebody", "airliner", "bizjet"}
                                 : roll < 0.92 ? std::vector<std::string>{"airliner", "bizjet"}
                                               : std::vector<std::string>{"bizjet"};
    else if (runway >= 1500.0) names = roll < 0.7 ? std::vector<std::string>{"airliner", "bizjet"}
                                                  : std::vector<std::string>{"bizjet"};
    else if (runway >= 900.0) names = {"bizjet"};
    std::vector<const AircraftType*> out;
    for (const auto& n : names) if (const AircraftType* t = aircraftType(n)) out.push_back(t);
    return out;
}
}  // namespace

std::string runwayDesignator(const std::string& ref, double bearing) {
    int computed = int(std::lround(pymod(bearing, 360.0) / 10.0));
    if (computed == 0) computed = 36;
    std::string best;
    int bestDiff = 3;
    size_t start = 0;
    while (start <= ref.size()) {
        const size_t slash = ref.find('/', start);
        std::string part = ref.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        part.erase(std::remove_if(part.begin(), part.end(), [](char c) { return std::isspace((unsigned char)c); }), part.end());
        size_t digits = 0;
        while (digits < part.size() && std::isdigit((unsigned char)part[digits])) ++digits;
        if (digits > 0 && digits <= 2) {
            const int n = std::atoi(part.substr(0, digits).c_str());
            std::string suffix = part.substr(digits);
            const bool letter = suffix.empty() || (suffix.size() == 1 && (suffix == "L" || suffix == "R" || suffix == "C"));
            const int diff = std::min(std::abs(n - computed), 36 - std::abs(n - computed));
            if (n >= 1 && n <= 36 && letter && diff < bestDiff) {
                bestDiff = diff;
                best = (n < 10 ? "0" : "") + std::to_string(n) + suffix;
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (!best.empty()) return best;
    return (computed < 10 ? "0" : "") + std::to_string(computed);
}

AirportOutput buildAirports(const OsmData& osm, const Tile& tile, const ElevationGrid& elevations,
                            const Anchor& anchor, const std::function<bool(double, double)>& wet) {
    AirportOutput out;
    int runwaysPaved = 0, runwaysUnpaved = 0, taxiways = 0, aprons = 0, helipads = 0;
    int standsTagged = 0, standsInferred = 0, bases = 0, widthsTagged = 0;
    std::map<std::string, int> byType;
    const Airfield field(osm, tile, anchor, wet);
    out.stats = {{"relevant", false}};
    if (!field.relevant()) return out;
    Paths64 runwaySurface, taxiSurface, apronSurface, padSurface;
    std::vector<Runway> runways;
    std::vector<std::pair<const OsmWay*, Ring>> apronRings;
    std::vector<std::vector<P2>> taxiLines;
    struct Pad { int64_t id; P2 at; std::string source; };
    std::vector<Pad> pads;
    for (const OsmWay& w : osm.aeroways) {
        const std::string kind = aeroway(w);
        const bool near = field.touches(w);
        // Every runway the query brought classifies the airport (sources.cpp
        // asks for them five kilometres around); only those near are drawn.
        if (kind == "runway" && !area(w)) {
            const std::vector<P2> line = engineLine(w.points, anchor);
            const double length = clip::length(line);
            runways.push_back({&w, line, length, widthOf(w.tags, length >= 2400 ? 45.0 : length >= 1200 ? 30.0 : 18.0), near});
        }
        if (!near) continue;
        widthsTagged += has(w.tags, "width");
        if (kind == "runway" || kind == "stopway") {
            const std::vector<P2> line = engineLine(w.points, anchor);
            const double length = clip::length(line);
            if (area(w)) {
                const Paths64 p{positive(clip::kMetres.path(engineRing(w, anchor)))};
                if (paved(w.tags)) runwaySurface.insert(runwaySurface.end(), p.begin(), p.end());
                continue;
            }
            const double width = widthOf(w.tags, kind == "stopway" ? 45.0 : length >= 2400 ? 45.0 : length >= 1200 ? 30.0 : 18.0);
            if (kind == "runway") ++(paved(w.tags) ? runwaysPaved : runwaysUnpaved);
            if (!paved(w.tags)) continue;
            const Paths64 p = clip::bufferLine(line, width / 2);
            runwaySurface.insert(runwaySurface.end(), p.begin(), p.end());
        } else if (kind == "taxiway" || kind == "taxilane") {
            ++taxiways;
            if (area(w)) {
                const Paths64 p{positive(clip::kMetres.path(engineRing(w, anchor)))};
                taxiSurface.insert(taxiSurface.end(), p.begin(), p.end());
                continue;
            }
            const std::vector<P2> line = engineLine(w.points, anchor);
            if (!paved(w.tags)) continue;
            const Paths64 p = clip::bufferRound(line, widthOf(w.tags, kind == "taxiway" ? 18.0 : 12.0) / 2);
            taxiSurface.insert(taxiSurface.end(), p.begin(), p.end());
            taxiLines.push_back(line);
        } else if (kind == "apron" && w.closed()) {
            ++aprons;
            const Ring r = engineRing(w, anchor);
            if (r.size() < 3) continue;
            apronRings.push_back({&w, r});
            if (paved(w.tags)) apronSurface.push_back(positive(clip::kMetres.path(r)));
        } else if (kind == "helipad") {
            ++helipads;
            const Ring r = engineRing(w, anchor);
            if (r.size() < 3) continue;
            if (w.closed()) padSurface.push_back(positive(clip::kMetres.path(r)));
            pads.push_back({w.id, centroid(r), "osm:way/" + std::to_string(w.id)});
        }
    }
    std::set<int64_t> seenNodes;
    for (const OsmNode& n : osm.features)
        if (tagOr(n.tags, "aeroway") == "helipad" && seenNodes.insert(n.id).second) {
            const P3 e = anchor.toEngine(n.lon, n.lat, 0.0);
            const P2 at{e.x, e.z};
            pads.push_back({n.id, at, "osm:node/" + std::to_string(n.id)});
            const Paths64 c = circle(at, 11.0);
            padSurface.insert(padSurface.end(), c.begin(), c.end());
            ++helipads;
        }

    // One surface per kind, never two on one square metre, and none under a
    // building (the terminals stand on their aprons).
    const Paths64& buildings = field.buildings();
    const Paths64 runway = clip::subtract(clip::unite(runwaySurface), buildings);
    const Paths64 taxi = clip::subtract(clip::subtract(clip::unite(taxiSurface), runway), buildings);
    const Paths64 pad = clip::subtract(clip::subtract(clip::subtract(clip::unite(padSurface), runway), taxi), buildings);
    const Paths64 apron = clip::subtract(clip::subtract(clip::subtract(clip::subtract(clip::unite(apronSurface), runway), taxi), pad), buildings);
    const Paths64 pavedAll = clip::unite(clip::unite(runway, taxi), clip::unite(apron, pad));

    Paint paint;
    for (const Runway& rw : runways) if (rw.near && paved(rw.way->tags)) paintRunway(rw, paint);
    for (const auto& line : taxiLines) {
        const Paths64 p = clip::bufferLine(line, 0.15);
        paint.yellow.insert(paint.yellow.end(), p.begin(), p.end());
    }
    for (const OsmWay& w : osm.aeroways)
        if (aeroway(w) == "parking_position" && !w.closed() && field.touches(w)) {
            const Paths64 p = clip::bufferLine(engineLine(w.points, anchor), 0.15);
            paint.yellow.insert(paint.yellow.end(), p.begin(), p.end());
        }
    for (const Pad& pad : pads) {
        const P2 at = pad.at;
        const Paths64 ring = clip::subtract(circle(at, 8.0, 32), circle(at, 7.4, 32));
        paint.white.insert(paint.white.end(), ring.begin(), ring.end());
        const P2 f{0.0, -1.0}, r{1.0, 0.0};
        for (const auto& q : {Paint::quad(at, f, r, -3.0, 3.0, -2.0, -1.2), Paint::quad(at, f, r, -3.0, 3.0, 1.2, 2.0),
                              Paint::quad(at, f, r, -0.4, 0.4, -1.2, 1.2)})
            paint.white.push_back(q);
    }
    const Paths64 white = clip::intersect(clip::unite(paint.white), pavedAll);
    const Paths64 yellow = clip::subtract(clip::intersect(clip::unite(paint.yellow), pavedAll), white);

    const Drape drape(elevations, anchor);
    Mesh runwayMesh(UvMode::Planar), taxiMesh(UvMode::Planar), apronMesh(UvMode::Planar), padMesh(UvMode::Planar);
    Mesh whiteMesh, yellowMesh;
    drape.lay(runway, kPavedLift, runwayMesh);
    drape.lay(taxi, kPavedLift, taxiMesh);
    drape.lay(apron, kPavedLift, apronMesh);
    drape.lay(pad, kPavedLift, padMesh);
    drape.lay(white, kPaintLift, whiteMesh);
    drape.lay(yellow, kPaintLift, yellowMesh);
    // Albedos: runway asphalt a shade darker than a street's (rubber and
    // jet exhaust), aprons concrete, and the paint the zebras use.
    struct Spec { const char* name; Mesh* mesh; std::array<double, 3> color; const char* family; };
    const Spec specs[] = {{"Runways", &runwayMesh, {0.095, 0.10, 0.105}, "asphalt"},
                          {"Taxiways", &taxiMesh, {0.11, 0.115, 0.12}, "asphalt"},
                          {"Aprons", &apronMesh, {0.20, 0.195, 0.185}, "quay"},
                          {"Helipads", &padMesh, {0.20, 0.195, 0.185}, "quay"},
                          {"Runway markings", &whiteMesh, {0.50, 0.49, 0.46}, nullptr},
                          {"Taxiway markings", &yellowMesh, {0.44, 0.35, 0.07}, nullptr}};
    for (const Spec& s : specs) {
        if (s.mesh->empty()) continue;
        out.parts.push_back({s.name, smoothSurface(*s.mesh),
                             surfaceMaterial(s.name, s.color, 0.85,
                                             s.family ? std::optional<std::string>(s.family) : std::nullopt)});
    }

    // ── the aircraft ────────────────────────────────────────────────────────
    auto longestRunwayNear = [&](P2 p) {
        double best = 0;
        for (const Runway& rw : runways)
            if (clip::distance(rw.line, p) < 4000.0) best = std::max(best, rw.length);
        return best;
    };
    std::vector<Placed> placed;
    Paths64 occupied;
    // Whether an aircraft of `type` fits at `at` facing `heading`: clear of
    // every building and every aircraft already parked by two metres, off the
    // runways, and mostly on the paving (`pavedShare`) -- wings may overhang grass.
    // `margin` is the clearance asked of buildings: two metres where the
    // stand is guessed, one where somebody surveyed it (a jet bridge is
    // a few metres from a nose).
    auto fits = [&](const AircraftType& type, P2 at, double heading, double pavedShare, double margin = 2.0) {
        const Paths64 body{rect(at, heading, type.length + 2 * margin, type.span + 2 * margin)};
        if (clip::area(clip::intersect(body, buildings)) > 0.5) return false;
        const Paths64 wings{rect(at, heading, type.length + 4.0, type.span + 4.0)};
        if (clip::area(clip::intersect(wings, occupied)) > 0.5) return false;
        if (clip::contains(runway, at)) return false;
        if (field.wet(at)) return false;
        if (pavedShare > 0) {
            const Paths64 core{rect(at, heading, type.length, type.span * 0.4)};
            if (clip::area(clip::intersect(core, pavedAll)) < pavedShare * clip::area(core)) return false;
        }
        return true;
    };
    auto park = [&](const AircraftType& type, P2 at, double heading, const std::string& source, char how) {
        occupied.push_back(rect(at, heading, type.length + 4.0, type.span + 4.0));
        placed.push_back({&type, at, heading, 0.0, source, how});
    };

    // Tagged stands first: somebody surveyed them.
    std::vector<Stand> stands;
    for (const OsmNode& n : osm.features) {
        if (tagOr(n.tags, "aeroway") != "parking_position" || !seenNodes.insert(n.id).second) continue;
        const P3 e = anchor.toEngine(n.lon, n.lat, 0.0);
        stands.push_back({n.id, {e.x, e.z}, 0.0, false, "osm:node/" + std::to_string(n.id)});
    }
    for (const OsmWay& w : osm.aeroways) {
        if (aeroway(w) != "parking_position" || w.closed() || !field.touches(w)) continue;
        const std::vector<P2> line = engineLine(w.points, anchor);
        if (line.size() < 2 || clip::length(line) < 3.0) continue;
        // A stand line runs from the taxilane to where the nose stops: the
        // end nearer a building is the stop, and the aircraft faces it.
        const P2 a = line.front(), b = line.back();
        const bool forward = field.clearance(b) <= field.clearance(a);
        const P2 stop = forward ? b : a, before = forward ? line[line.size() - 2] : line[1];
        stands.push_back({w.id, stop, headingOf({stop.x - before.x, stop.y - before.y}), true,
                          "osm:way/" + std::to_string(w.id)});
    }
    std::sort(stands.begin(), stands.end(), [](const Stand& a, const Stand& b) { return a.id < b.id; });
    for (Stand& s : stands)
        for (const Stand& o : stands)
            if (&o != &s) s.room = std::min(s.room, dist(s.at, o.at));
    for (const Stand& s : stands) {
        if (field.military(s.at)) continue;
        PyRandom rng = seeded(s.id, kSaltStand);
        if (rng.random() > 0.7) continue;  // an empty stand is a stand too
        const double runwayLength = longestRunwayNear(s.at);
        const auto fleet = fleetFor(runwayLength, rng);
        for (const AircraftType* type : fleet) {
            if (type->span > s.room + 6.0) continue;
            // A business jet on an airliner's stand is the wrong aircraft,
            // not a smaller right one: it takes the stands too small for one.
            if (type->klass == "jet" && fleet.size() > 1 && s.room >= 36.0) continue;
            double heading = s.heading;
            P2 centre = s.at;
            if (s.faced) {
                // The stand line ends where the nose stops.
                centre = plus(s.at, forwardOf(heading), -(type->length / 2 - 1.0));
            } else {
                const auto wall = field.nearestBuilding(s.at, 150.0);
                heading = wall ? headingOf({wall->x - s.at.x, wall->y - s.at.y})
                               : rng.random() * 360.0;
                // Back off until the nose clears the wall by four metres.
                for (int k = 0; k < 20 && wall && field.clearance(plus(centre, forwardOf(heading), type->length / 2)) < 4.0; ++k)
                    centre = plus(centre, forwardOf(heading), -2.0);
            }
            if (!fits(*type, centre, heading, 0.0, 1.0)) continue;
            park(*type, centre, heading, s.source + (s.faced ? " (stand line)" : " (faces the nearest building)"), 't');
            break;
        }
    }
    // Aprons nobody drew stands on: rows across their long side, the way
    // an apron is laid out, facing the nearest building.
    std::sort(apronRings.begin(), apronRings.end(), [](const auto& a, const auto& b) { return a.first->id < b.first->id; });
    for (const auto& [way, ring] : apronRings) {
        if (!paved(way->tags) || std::abs(polygonArea(ring)) < 2500.0) continue;
        bool tagged = false;
        for (const Stand& s : stands) tagged |= pointInPolygon(s.at, ring);
        if (tagged || field.military(centroid(ring))) continue;
        const OrientedBox box = orientedBox(ring);
        const double runwayLength = longestRunwayNear({box.cx, box.cz});
        PyRandom rng = seeded(way->id, kSaltApron);
        // Rows as deep as the largest aircraft the airport receives, twenty
        // metres of taxilane between them; along a row, each aircraft takes
        // its own wingspan and six metres.
        const double depth = (runwayLength >= 2800.0 ? 64.0 : runwayLength >= 1500.0 ? 38.0 : 21.0) + 20.0;
        const P2 axis{box.vx(), box.vz()};
        size_t here = 0;
        for (double v = -box.halfV + depth / 2; v <= box.halfV - depth / 2 + 1e-9 && here < kApronAircraft; v += depth) {
            double u = -box.halfU + 4.0;
            while (u < box.halfU - 4.0 && here < kApronAircraft) {
                const auto fleet = fleetFor(runwayLength, rng);
                if (fleet.empty()) break;
                const bool empty = rng.random() > 0.6;
                bool parked = false;
                for (const AircraftType* type : fleet) {
                    if (empty || type->length > depth - 12.0 || u + type->span > box.halfU - 2.0) continue;
                    const P2 at = box.point(u + type->span / 2, v);
                    if (!pointInPolygon(at, ring)) continue;
                    const auto wall = field.nearestBuilding(at, 200.0);
                    double heading = headingOf(axis);
                    if (wall && (wall->x - at.x) * axis.x + (wall->y - at.y) * axis.y < 0) heading = pymod(heading + 180.0, 360.0);
                    if (!fits(*type, at, heading, 0.75)) continue;
                    park(*type, at, heading, "osm:way/" + std::to_string(way->id) + " apron (inferred stand)", 'i');
                    ++here;
                    u += type->span + 6.0;
                    parked = true;
                    break;
                }
                if (!parked) u += empty ? fleet.back()->span + 6.0 : 8.0;
            }
        }
    }
    // Helipads at an aerodrome carry a helicopter more often than not.
    const AircraftType* helicopter = nullptr;
    for (const auto& a : palette().aircraft) if (a.klass == "helicopter") { helicopter = &a; break; }
    std::sort(pads.begin(), pads.end(), [](const Pad& a, const Pad& b) { return a.id < b.id; });
    for (const Pad& pad : pads) {
        if (!helicopter || field.military(pad.at) || longestRunwayNear(pad.at) < 400.0) continue;
        PyRandom rng = seeded(pad.id, kSaltPad);
        if (rng.random() > 0.6) continue;
        const double heading = rng.random() * 360.0;
        if (fits(*helicopter, pad.at, heading, 0.0)) park(*helicopter, pad.at, heading, pad.source + " helipad", 'p');
    }
    // One helicopter per military base: on its helipad if it has one, on an
    // apron if not, and otherwise on the first clear ground from its middle.
    if (helicopter) {
        std::vector<std::pair<const OsmWay*, Ring>> bases_ = field.bases();
        std::stable_sort(bases_.begin(), bases_.end(), [](const auto& a, const auto& b) {
            const double x = std::abs(polygonArea(a.second)), y = std::abs(polygonArea(b.second));
            return x != y ? x > y : a.first->id < b.first->id;
        });
        std::vector<const Ring*> taken;
        for (const auto& [way, ring] : bases_) {
            if (ring.size() < 3 || std::abs(polygonArea(ring)) < 800.0) continue;
            const P2 middle = centroid(ring);
            bool inside = false;
            for (const Ring* t : taken) inside |= pointInPolygon(middle, *t);
            if (inside) continue;  // a barracks inside a base is the same base
            taken.push_back(&ring);
            std::optional<P2> spot;
            std::string how;
            for (const Pad& pad : pads)
                if (!spot && pointInPolygon(pad.at, ring)) { spot = pad.at; how = "helipad"; }
            for (const auto& [w, apronRing] : apronRings) {
                if (spot) break;
                const P2 c = centroid(apronRing);
                if (pointInPolygon(c, ring) && field.clearance(c) > 12.0) { spot = c; how = "apron"; }
            }
            for (int i = 0; !spot && i < 600; ++i) {
                const double a = i * 2.39996323, d = 6.0 * std::sqrt(double(i));
                const P2 at{middle.x + d * std::cos(a), middle.y + d * std::sin(a)};
                if (pointInPolygon(at, ring) && field.clearance(at) > 12.0 && !field.wet(at)) { spot = at; how = "clear ground"; }
            }
            if (!spot) continue;
            PyRandom rng = seeded(way->id, kSaltBase);
            const double heading = rng.random() * 360.0;
            park(*helicopter, *spot, heading, "osm:way/" + std::to_string(way->id) + " military base, " + how, 'b');
        }
    }

    // Only what stands in this tile is the tile's.
    const Bounds b = tile.bounds();
    size_t index = 0;
    for (const Placed& p : placed) {
        if (index >= kTileAircraft) break;
        const P3 geo = anchor.toGeodetic(p.at.x, 0.0, p.at.y);
        if (!(b.west <= geo.x && geo.x < b.east && b.south <= geo.y && geo.y < b.north)) continue;
        standsTagged += p.how == 't';
        standsInferred += p.how == 'i';
        bases += p.how == 'b';
        const bool onPaving = clip::contains(pavedAll, p.at);
        const double y = drape.heightAt(p.at) + (onPaving ? kPavedLift : 0.0);
        ++byType[p.type->name];
        out.aircraft.push_back({{"name", "Aircraft " + std::to_string(index++)}, {"type", p.type->name},
                                {"class", p.type->klass}, {"x", pyround(p.at.x, 3)}, {"y", pyround(y, 3)},
                                {"z", pyround(p.at.y, 3)}, {"heading", pyround(p.heading, 3)},
                                {"source", p.source}, {"inferred", true}});
    }
    out.stats = {{"relevant", true}, {"runways", runwaysPaved + runwaysUnpaved}, {"runwaysUnpaved", runwaysUnpaved},
                 {"taxiways", taxiways}, {"aprons", aprons}, {"helipads", helipads},
                 {"widthsTagged", widthsTagged},
                 {"standsTagged", standsTagged}, {"standsInferred", standsInferred},
                 {"militaryHelicopters", bases}, {"aircraft", byType}, {"aircraftNearby", placed.size()},
                 {"pavedAreaM2", pyround(clip::area(pavedAll), 1)}};
    return out;
}

}  // namespace r1
