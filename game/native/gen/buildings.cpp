#include "buildings.hpp"

#include <algorithm>
#include <regex>

namespace r1 {

namespace {
// Salts keeping one building's draws independent (atlas.seeded).
constexpr int64_t kSaltWall = 0x57414C4C, kSaltRoof = 0x524F4F46, kSaltShape = 0x53484150, kSaltHeight = 0x48474854;

// Below this a "building" is a mapping artefact; a 1.5 m² shed is real.
constexpr double kMinFootprintArea = 1.5;
constexpr double kWeldDistance = 0.05;
// A vertex nearer than this to the chord of its neighbours is not a corner.
constexpr double kCollinearSagitta = 0.08;
constexpr double kRectifyToleranceDeg = 7.0;
// A pitched roof is built over the oriented box, so only when the footprint
// fills it; wider than one ridge can span, the roof is flat.
constexpr double kRectangularityForPitch = 0.82;
constexpr double kMaxRidgeSpan = 19.0;
constexpr double kMaxRoofOverWall = 1.0;
constexpr double kPartyWallDistance = 0.7, kPartyWallOverlap = 1.2, kPartyWallCell = 8.0;

// ── footprint hygiene ───────────────────────────────────────────────────────
Ring weld(const Ring& ring) {
    Ring out;
    for (const P2& p : ring) {
        if (!out.empty() && dist(out.back(), p) < kWeldDistance) continue;
        out.push_back(p);
    }
    while (out.size() > 2 && dist(out.front(), out.back()) < kWeldDistance) out.pop_back();
    return out;
}

Ring dropCollinear(const Ring& ring) {
    if (ring.size() < 4) return ring;
    Ring out;
    const size_t count = ring.size();
    for (size_t i = 0; i < count; ++i) {
        const P2 previous = ring[(i + count - 1) % count], current = ring[i], following = ring[(i + 1) % count];
        const double chord = dist(previous, following);
        if (chord < 1e-9) continue;
        const double area2 = std::abs((following.x - previous.x) * (previous.y - current.y) -
                                      (previous.x - current.x) * (following.y - previous.y));
        if (area2 / chord > kCollinearSagitta) out.push_back(current);
    }
    return out.size() >= 3 ? out : ring;
}

double dominantAxis(const Ring& ring) {
    double sinSum = 0, cosSum = 0;
    const size_t count = ring.size();
    for (size_t i = 0; i < count; ++i) {
        const P2 p0 = ring[i], p1 = ring[(i + 1) % count];
        const double dx = p1.x - p0.x, dy = p1.y - p0.y, length = std::hypot(dx, dy);
        if (length < 1e-9) continue;
        const double angle = std::atan2(dy, dx) * 4.0;
        sinSum += std::sin(angle) * length;
        cosSum += std::cos(angle) * length;
    }
    if (std::abs(sinSum) < 1e-12 && std::abs(cosSum) < 1e-12) return 0.0;
    return pymod(std::atan2(sinSum, cosSum) / 4.0, kPi / 2.0);
}

Ring rectify(const Ring& ring) {
    const size_t count = ring.size();
    if (count < 4) return ring;
    const double grid = dominantAxis(ring);
    struct Line { P2 point, dir; };
    std::vector<Line> lines;
    for (size_t i = 0; i < count; ++i) {
        const P2 p0 = ring[i], p1 = ring[(i + 1) % count];
        const double dx = p1.x - p0.x, dy = p1.y - p0.y, length = std::hypot(dx, dy);
        if (length < 1e-9) { lines.push_back({p0, {1.0, 0.0}}); continue; }
        const double angle = std::atan2(dy, dx);
        const double folded = pymod(angle - grid, kPi / 2.0);
        const double offset = folded <= kPi / 4.0 ? folded : folded - kPi / 2.0;
        const P2 mid{(p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5};
        if (std::abs(offset) <= radians(kRectifyToleranceDeg)) {
            const double snapped = angle - offset;
            lines.push_back({mid, {std::cos(snapped), std::sin(snapped)}});
        } else {
            lines.push_back({mid, {dx / length, dy / length}});
        }
    }
    Ring out;
    for (size_t i = 0; i < count; ++i) {
        const Line& a = lines[(i + count - 1) % count];
        const Line& b = lines[i];
        const double denominator = a.dir.x * b.dir.y - a.dir.y * b.dir.x;
        if (std::abs(denominator) < 1e-6) { out.push_back(ring[i]); continue; }
        const double wx = b.point.x - a.point.x, wy = b.point.y - a.point.y;
        const double t = (wx * b.dir.y - wy * b.dir.x) / denominator;
        const P2 candidate{a.point.x + a.dir.x * t, a.point.y + a.dir.y * t};
        out.push_back(dist(candidate, ring[i]) < 1.5 ? candidate : ring[i]);
    }
    return out;
}

}  // namespace

std::optional<Ring> cleanFootprint(const Ring& ring) {
    const Ring welded = weld(ring);
    if (welded.size() < 3) return std::nullopt;
    const Ring simplified = dropCollinear(welded);
    if (simplified.size() < 3 || std::abs(polygonArea(simplified)) < kMinFootprintArea) return std::nullopt;
    Ring rectified = rectify(simplified);
    if (std::abs(polygonArea(rectified)) < kMinFootprintArea) rectified = simplified;
    if (polygonArea(rectified) < 0.0) std::reverse(rectified.begin(), rectified.end());
    return rectified;
}

// ── oriented bounding box ───────────────────────────────────────────────────

OrientedBox orientedBox(const Ring& ring) {
    const Ring hull = convexHull(ring);
    if (hull.size() < 3) {
        double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
        for (const P2& p : ring) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.y); z1 = std::max(z1, p.y); }
        return {(x0 + x1) * 0.5, (z0 + z1) * 0.5, 1.0, 0.0, std::max(0.5, (x1 - x0) * 0.5), std::max(0.5, (z1 - z0) * 0.5)};
    }
    OrientedBox best;
    double bestArea = 1e300;
    for (size_t i = 0; i < hull.size(); ++i) {
        const P2 p0 = hull[i], p1 = hull[(i + 1) % hull.size()];
        const double dx = p1.x - p0.x, dz = p1.y - p0.y, length = std::hypot(dx, dz);
        if (length < 1e-9) continue;
        const double ax = dx / length, az = dz / length, bx = -az, bz = ax;
        double aMin = 1e300, aMax = -1e300, bMin = 1e300, bMax = -1e300;
        for (const P2& p : hull) {
            const double along = p.x * ax + p.y * az, across = p.x * bx + p.y * bz;
            aMin = std::min(aMin, along); aMax = std::max(aMax, along);
            bMin = std::min(bMin, across); bMax = std::max(bMax, across);
        }
        const double extentA = aMax - aMin, extentB = bMax - bMin, area = extentA * extentB;
        if (area >= bestArea) continue;
        const double midA = (aMax + aMin) * 0.5, midB = (bMax + bMin) * 0.5;
        const double cx = ax * midA + bx * midB, cz = az * midA + bz * midB;
        best = extentA >= extentB ? OrientedBox{cx, cz, ax, az, extentA * 0.5, extentB * 0.5}
                                  : OrientedBox{cx, cz, bx, bz, extentB * 0.5, extentA * 0.5};
        bestArea = area;
    }
    return best;
}

namespace {

// ── party walls ─────────────────────────────────────────────────────────────
bool segmentsFaceEachOther(P2 p0, P2 p1, P2 q0, P2 q1) {
    const double dx = p1.x - p0.x, dz = p1.y - p0.y, length = std::hypot(dx, dz);
    if (length < 1e-6) return false;
    const double ax = dx / length, az = dz / length, nx = az, nz = -ax;
    const double d0 = (q0.x - p0.x) * nx + (q0.y - p0.y) * nz, d1 = (q1.x - p0.x) * nx + (q1.y - p0.y) * nz;
    if (std::abs(d0) > kPartyWallDistance || std::abs(d1) > kPartyWallDistance) return false;
    const double t0 = (q0.x - p0.x) * ax + (q0.y - p0.y) * az, t1 = (q1.x - p0.x) * ax + (q1.y - p0.y) * az;
    const double low = std::min(t0, t1), high = std::max(t0, t1);
    return std::min(high, length) - std::max(low, 0.0) >= kPartyWallOverlap;
}

struct PartyWallIndex {
    struct Entry { int owner; P2 p0, p1; };
    std::map<std::pair<long, long>, std::vector<Entry>> cells;
    static std::pair<long, long> key(P2 p) {
        return {long(std::floor(p.x / kPartyWallCell)), long(std::floor(p.y / kPartyWallCell))};
    }
    void add(int owner, P2 p0, P2 p1) { cells[key({(p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5})].push_back({owner, p0, p1}); }
    bool isParty(int owner, P2 p0, P2 p1) const {
        const auto [cx, cz] = key({(p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5});
        for (long dx = -1; dx <= 1; ++dx)
            for (long dz = -1; dz <= 1; ++dz) {
                auto it = cells.find({cx + dx, cz + dz});
                if (it == cells.end()) continue;
                for (const Entry& e : it->second)
                    if (e.owner != owner && segmentsFaceEachOther(p0, p1, e.p0, e.p1)) return true;
            }
        return false;
    }
};

// ── height and roof ─────────────────────────────────────────────────────────

double slopeSign(const std::string& raw) {
    std::string text;
    for (char c : raw) if (c != ' ' && c != '\t' && c != '\n') text += char(std::tolower((unsigned char)c));
    if (text.empty()) return 1.0;
    static const std::map<std::string, double> compass = {
        {"n", 0.0}, {"nne", 22.5}, {"ne", 45.0}, {"ene", 67.5}, {"e", 90.0}, {"ese", 112.5}, {"se", 135.0},
        {"sse", 157.5}, {"s", 180.0}, {"ssw", 202.5}, {"sw", 225.0}, {"wsw", 247.5}, {"w", 270.0},
        {"wnw", 292.5}, {"nw", 315.0}, {"nnw", 337.5}};
    auto it = compass.find(text);
    if (it != compass.end()) return it->second < 180.0 ? 1.0 : -1.0;
    auto bearing = taggedLength(text);
    if (!bearing) return 1.0;
    return pymod(*bearing, 360.0) < 180.0 ? 1.0 : -1.0;
}

double stack(int storeys, const RegionProfile& profile, bool commercial) {
    if (commercial && storeys >= 2) return profile.groundStoreyHeight + (storeys - 1) * profile.storeyHeight;
    return storeys * profile.storeyHeight;
}

bool truthy(const std::optional<double>& v) { return v && *v != 0.0; }

// A terminal or a hangar is not a house of its region: it is a glass hall or
// a steel shed wherever it stands. Only its tags say so -- `aeroway` on the
// building, or `building=terminal|hangar` -- and only what they did not
// measure is changed (CLAUDE.md rule 4).
std::string aviationKind(const Tags& tags) {
    const std::string aero = tagOr(tags, "aeroway"), building = tagOr(tags, "building");
    if (aero == "hangar" || building == "hangar") return "hangar";
    if (aero == "terminal" || building == "terminal" || aero == "control_tower" || aero == "tower") return "terminal";
    return "";
}
// Albedos: tinted glass over dark mullions, galvanised cladding, a steel roof.
const Swatch kTerminalWall{"Terminal glazing", {0.085, 0.10, 0.11}, 0.25, 1.0};
const Swatch kHangarWall{"Hangar cladding, corrugated steel", {0.21, 0.215, 0.22}, 0.55, 1.0};
const Swatch kAviationRoof{"Aviation roof, metal seam", {0.18, 0.185, 0.19}, 0.5, 1.0};

}  // namespace

Gabarit planGabarit(const Tags& tags, int64_t osmId, const RegionProfile& profile, const OrientedBox& box,
                    double rectangularity, bool commercial) {
    PyRandom shapeRng = seeded(osmId, kSaltShape), heightRng = seeded(osmId, kSaltHeight);
    Gabarit g;
    std::string tagged;
    for (char c : tagOr(tags, "roof:shape")) tagged += char(std::tolower((unsigned char)c));
    while (!tagged.empty() && std::isspace((unsigned char)tagged.back())) tagged.pop_back();
    while (!tagged.empty() && std::isspace((unsigned char)tagged.front())) tagged.erase(tagged.begin());
    static const char* known[] = {"flat", "gabled", "hipped", "pyramidal", "skillion",
                                  "half-hipped", "gambrel", "mansard", "round", "dome"};
    bool isKnown = false;
    for (const char* k : known) isKnown |= tagged == k;
    if (isKnown) {
        static const std::map<std::string, std::string> nearest = {
            {"half-hipped", "hipped"}, {"gambrel", "gabled"}, {"mansard", "hipped"}, {"round", "gabled"}, {"dome", "pyramidal"}};
        auto it = nearest.find(tagged);
        g.roofShape = it == nearest.end() ? tagged : it->second;
        g.roofSource = "tag:shape";
    } else {
        g.roofShape = profile.roofShape(shapeRng);
        g.roofSource = "atlas";
    }
    if (g.roofShape != "flat" && rectangularity < kRectangularityForPitch) { g.roofShape = "flat"; g.roofSource = "atlas:not-rectangular"; }
    if (g.roofShape != "flat" && g.roofShape != "skillion" && box.halfV * 2.0 > kMaxRidgeSpan) {
        g.roofShape = "flat"; g.roofSource = "atlas:span";
    }
    const auto pitch = taggedLength(tagOr(tags, "roof:angle"));
    const double pitchDegrees = (truthy(pitch) && 5.0 <= *pitch && *pitch <= 60.0) ? *pitch : profile.roofPitch(shapeRng);
    if (g.roofShape == "flat") g.roofHeight = 0.0;
    else {
        const auto taggedRoof = taggedLength(tagOr(tags, "roof:height"));
        if (taggedRoof && 0.5 <= *taggedRoof && *taggedRoof <= 25.0) g.roofHeight = *taggedRoof;
        else {
            const double run = g.roofShape != "skillion" ? box.halfV : box.halfV * 2.0;
            g.roofHeight = std::min(12.0, std::max(0.6, std::tan(radians(pitchDegrees)) * run));
        }
    }
    const auto levels = taggedLength(tagOr(tags, "building:levels"));
    const auto total = taggedLength(tagOr(tags, "height"));
    if (total && 2.0 <= *total && *total <= 300.0) {
        const double reserve = std::min(2.4, *total * 0.6);
        g.roofHeight = std::min(g.roofHeight, std::max(0.0, *total - reserve));
        g.wallHeight = *total - g.roofHeight;
        g.storeys = truthy(levels) ? std::max(1, int(pyround(*levels)))
                                   : std::max(1, int(pyround(g.wallHeight / profile.storeyHeight)));
        g.heightSource = "tag:height";
    } else if (levels && 1.0 <= *levels && *levels <= 80.0) {
        g.storeys = std::max(1, int(pyround(*levels)));
        g.wallHeight = stack(g.storeys, profile, commercial);
        g.heightSource = "tag:levels";
    } else {
        g.storeys = profile.storeys(heightRng);
        g.wallHeight = stack(g.storeys, profile, commercial) + (heightRng.random() - 0.5) * 0.9;
        g.heightSource = "atlas";
    }
    if (g.heightSource == "atlas") g.wallHeight = std::max(2.4, g.wallHeight);
    g.wallHeight = std::min(90.0, g.wallHeight);
    if (g.roofSource != "tag:shape" || !truthy(taggedLength(tagOr(tags, "roof:height"))))
        g.roofHeight = std::min(g.roofHeight, g.wallHeight * kMaxRoofOverWall);
    return g;
}

namespace {

// ── mesh buckets: one mesh per swatch, not one per building ─────────────────
struct MeshBook {
    UvMode mode = UvMode::None;
    std::map<std::string, std::pair<Swatch, Mesh>> meshes;
    Mesh& mesh(const Swatch& s) {
        auto it = meshes.find(s.name);
        if (it == meshes.end()) it = meshes.emplace(s.name, std::make_pair(s, Mesh(mode))).first;
        return it->second.second;
    }
    void parts(std::vector<MeshPart>& out, const std::string& prefix, bool doubleSided, const MaterialFor& materials) {
        for (auto& [name, entry] : meshes) {
            if (entry.second.empty()) continue;
            Material m;
            if (materials) m = materials(entry.first, doubleSided);
            else {
                m.name = name; m.color = {entry.first.color[0], entry.first.color[1], entry.first.color[2], 1.0};
                m.roughness = entry.first.roughness; m.doubleSided = doubleSided;
            }
            out.push_back({prefix + " \xE2\x80\x94 " + name, std::move(entry.second), m});
        }
    }
};

// ── wall frames and faces ───────────────────────────────────────────────────
struct WallFrame {
    double ox, oz, ground, ax, az, nx, nz, length;
    P3 point(double s, double t, double depth = 0.0) const {
        return {ox + ax * s - nx * depth, ground + t, oz + az * s - nz * depth};
    }
};

std::optional<WallFrame> wallFrame(P2 p0, P2 p1, double ground) {
    const double dx = p1.x - p0.x, dz = p1.y - p0.y, length = std::hypot(dx, dz);
    if (length < 1e-6) return std::nullopt;
    const double ax = dx / length, az = dz / length;
    return WallFrame{p0.x, p0.y, ground, ax, az, az, -ax, length};
}

void face(Mesh& mesh, const WallFrame& f, double s0, double t0, double s1, double t1, double depth = 0.0,
          const std::pair<double, double>* uvScale = nullptr) {
    if (s1 - s0 < 1e-4 || t1 - t0 < 1e-4) return;
    if (uvScale) {
        const double u = uvScale->first, v = uvScale->second;
        const UV uvs[4] = {{s0 * u, -t0 * v}, {s0 * u, -t1 * v}, {s1 * u, -t1 * v}, {s1 * u, -t0 * v}};
        mesh.addQuad(f.point(s0, t0, depth), f.point(s0, t1, depth), f.point(s1, t1, depth), f.point(s1, t0, depth), uvs);
        return;
    }
    mesh.addQuad(f.point(s0, t0, depth), f.point(s0, t1, depth), f.point(s1, t1, depth), f.point(s1, t0, depth));
}

void reveal(Mesh& mesh, const WallFrame& f, double s0, double t0, double s1, double t1, double depth) {
    if (depth <= 1e-4) return;
    mesh.addQuad(f.point(s0, t0), f.point(s0, t1), f.point(s0, t1, depth), f.point(s0, t0, depth));
    mesh.addQuad(f.point(s1, t0), f.point(s1, t0, depth), f.point(s1, t1, depth), f.point(s1, t1));
    mesh.addUpQuad(f.point(s0, t0), f.point(s1, t0), f.point(s1, t0, depth), f.point(s0, t0, depth));
    mesh.addQuad(f.point(s0, t1), f.point(s1, t1), f.point(s1, t1, depth), f.point(s0, t1, depth));
}

// ── roofs ───────────────────────────────────────────────────────────────────
void addSlab(Mesh& mesh, std::vector<P3> loop, double thickness) {
    if (loop.size() < 3) return;
    Ring flat;
    for (const P3& p : loop) flat.push_back({p.x, p.z});
    if (polygonArea(flat) < 0.0) std::reverse(loop.begin(), loop.end());
    if (thickness <= 0.0) {
        for (size_t i = 1; i + 1 < loop.size(); ++i) mesh.addUpTriangle(loop[0], loop[i], loop[i + 1]);
        return;
    }
    std::vector<P3> under;
    for (const P3& p : loop) under.push_back({p.x, p.y - thickness, p.z});
    for (size_t i = 1; i + 1 < loop.size(); ++i) {
        mesh.addUpTriangle(loop[0], loop[i], loop[i + 1]);
        mesh.addTriangle(under[0], under[i], under[i + 1]);
    }
    for (size_t i = 0; i < loop.size(); ++i) {
        const size_t next = (i + 1) % loop.size();
        mesh.addQuad(loop[i], loop[next], under[next], under[i]);
    }
}

std::vector<std::vector<P3>> roofLoops(const OrientedBox& box, const std::string& shape, double eave,
                                       double yEave, double yRidge, double direction) {
    const double hu = box.halfU + eave, hv = box.halfV + eave;
    auto at = [&](double a, double b, double y) { P2 p = box.point(a, b); return P3{p.x, y, p.y}; };
    if (shape == "gabled")
        return {{at(-hu, -hv, yEave), at(hu, -hv, yEave), at(hu, 0.0, yRidge), at(-hu, 0.0, yRidge)},
                {at(hu, hv, yEave), at(-hu, hv, yEave), at(-hu, 0.0, yRidge), at(hu, 0.0, yRidge)}};
    if (shape == "hipped") {
        const double ridge = std::max(0.0, hu - hv);
        if (ridge < 0.05) return roofLoops(box, "pyramidal", eave, yEave, yRidge, direction);
        return {{at(-hu, -hv, yEave), at(hu, -hv, yEave), at(ridge, 0.0, yRidge), at(-ridge, 0.0, yRidge)},
                {at(hu, hv, yEave), at(-hu, hv, yEave), at(-ridge, 0.0, yRidge), at(ridge, 0.0, yRidge)},
                {at(-hu, hv, yEave), at(-hu, -hv, yEave), at(-ridge, 0.0, yRidge)},
                {at(hu, -hv, yEave), at(hu, hv, yEave), at(ridge, 0.0, yRidge)}};
    }
    if (shape == "pyramidal") {
        const P3 apex = at(0.0, 0.0, yRidge);
        const P3 c[4] = {at(-hu, -hv, yEave), at(hu, -hv, yEave), at(hu, hv, yEave), at(-hu, hv, yEave)};
        std::vector<std::vector<P3>> out;
        for (int i = 0; i < 4; ++i) out.push_back({c[i], c[(i + 1) % 4], apex});
        return out;
    }
    if (shape == "skillion") {
        const double low = direction >= 0.0 ? -hv : hv, high = direction >= 0.0 ? hv : -hv;
        return {{at(-hu, low, yEave), at(hu, low, yEave), at(hu, high, yRidge), at(-hu, high, yRidge)}};
    }
    return {};
}

std::vector<std::array<P3, 3>> gableWalls(const OrientedBox& box, const std::string& shape, double yEave,
                                          double yRidge, double direction) {
    const double hu = box.halfU, hv = box.halfV;
    auto at = [&](double a, double b, double y) { P2 p = box.point(a, b); return P3{p.x, y, p.y}; };
    if (shape == "gabled")
        return {{at(-hu, -hv, yEave), at(-hu, hv, yEave), at(-hu, 0.0, yRidge)},
                {at(hu, hv, yEave), at(hu, -hv, yEave), at(hu, 0.0, yRidge)}};
    if (shape == "skillion") {
        const double low = direction >= 0.0 ? -hv : hv, high = direction >= 0.0 ? hv : -hv;
        return {{at(-hu, low, yEave), at(-hu, high, yEave), at(-hu, high, yRidge)},
                {at(hu, high, yEave), at(hu, low, yEave), at(hu, high, yRidge)}};
    }
    return {};
}

void emitParapet(Mesh& mesh, const Ring& ring, double yDeck, double height) {
    // Python compared the inset ring by identity, which a copy never passes:
    // every flat roof gets its parapet, inset or not.
    const Ring inner = insetPolygon(ring, 0.28);
    const double top = yDeck + height;
    const size_t count = ring.size();
    for (size_t i = 0; i < count; ++i) {
        const size_t next = (i + 1) % count;
        auto frame = wallFrame(ring[i], ring[next], yDeck);
        if (!frame) continue;
        face(mesh, *frame, 0.0, 0.0, frame->length, height);
        const P2 a = ring[i], b = ring[next], ia = inner[i], ib = inner[next];
        mesh.addUpQuad({a.x, top, a.y}, {b.x, top, b.y}, {ib.x, top, ib.y}, {ia.x, top, ia.y});
        auto innerFrame = wallFrame(ib, ia, yDeck);
        if (innerFrame) face(mesh, *innerFrame, 0.0, 0.0, innerFrame->length, height);
    }
}

// ── towers ──────────────────────────────────────────────────────────────────
bool isWorship(const Tags& tags) {
    static const char* worship[] = {"church", "chapel", "cathedral", "basilica", "mosque", "synagogue",
                                    "temple", "monastery", "shrine"};
    if (tagOr(tags, "amenity") == "place_of_worship") return true;
    const std::string b = tagOr(tags, "building");
    for (const char* w : worship) if (b == w) return true;
    return false;
}

std::string steepleStyle(const Tags& tags) {
    const std::string religion = tagOr(tags, "religion");
    if (religion == "muslim" || tagOr(tags, "building") == "mosque") return "minaret";
    if (religion == "jewish" || religion == "buddhist" || religion == "hindu" || religion == "sikh" || religion.empty())
        return religion.empty() ? "spire" : "tower";
    return "spire";
}

void emitSteeple(Mesh& walls, Mesh& roofs, const OrientedBox& box, double ground, double wallHeight,
                 const std::string& style) {
    double side = std::max(2.2, std::min(box.halfV * 1.5, 7.0));
    const double centreA = box.halfU > side ? -(box.halfU - side * 0.5) : 0.0;
    const P2 c = box.point(centreA, 0.0);
    const double yaw = std::atan2(box.ux, box.uz);
    double shaft, cap;
    if (style == "minaret") {
        side = std::min(side * 0.45, 3.2);
        shaft = std::max(12.0, wallHeight * 2.8);
        cap = side * 1.6;
    } else if (style == "spire") {
        shaft = std::max(8.0, wallHeight * 1.7);
        cap = side * 2.4;
    } else {
        shaft = std::max(6.0, wallHeight * 1.35);
        cap = side * 0.45;
    }
    walls.addBox({c.x, ground + shaft * 0.5, c.y}, {side, shaft, side}, yaw);
    const P3 apex{c.x, ground + shaft + cap, c.y};
    const double half = side * 0.5;
    P3 corners[4];
    const double d[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    for (int i = 0; i < 4; ++i)
        corners[i] = {c.x + d[i][0] * std::cos(yaw) + d[i][1] * std::sin(yaw), ground + shaft,
                      c.y - d[i][0] * std::sin(yaw) + d[i][1] * std::cos(yaw)};
    for (int i = 0; i < 4; ++i) roofs.addTriangle(corners[i], corners[(i + 1) % 4], apex);
}

// ── facades ─────────────────────────────────────────────────────────────────
std::vector<std::pair<double, double>> floorLevels(double wallHeight, int storeys, const RegionProfile& profile,
                                                   bool commercial) {
    std::vector<std::pair<double, double>> out;
    if (storeys <= 0) return out;
    if (commercial && storeys >= 2) {
        const double ground = std::min(profile.groundStoreyHeight, wallHeight * 0.55);
        const double rest = (wallHeight - ground) / (storeys - 1);
        out.push_back({0.0, ground});
        for (int i = 0; i < storeys - 1; ++i) out.push_back({ground + rest * i, rest});
        return out;
    }
    const double even = wallHeight / storeys;
    for (int i = 0; i < storeys; ++i) out.push_back({even * i, even});
    return out;
}

void emitFacade(Mesh& walls, Mesh& trim, Mesh& glass, const WallFrame& f,
                const std::vector<std::pair<double, double>>& levels, double wallHeight,
                const RegionProfile& profile, bool commercial) {
    const int bays = std::max(1, int(pyround(f.length / profile.bayWidth)));
    const double bay = f.length / bays;
    for (size_t floor = 0; floor < levels.size(); ++floor) {
        const auto [base, height] = levels[floor];
        const bool shopfront = commercial && floor == 0;
        double pier, width, openingHeight, sill;
        if (shopfront) {
            pier = profile.shopfrontPierMin;
            width = std::min(bay * profile.shopfrontWidthRatio, bay - 2.0 * pier);
            openingHeight = std::min(profile.shopfrontHeight, height - profile.lintelMin);
            sill = profile.shopfrontSill;
        } else {
            pier = profile.pierMin; width = profile.windowWidth;
            openingHeight = profile.windowHeight; sill = profile.windowSill;
        }
        const bool fits = width + 2.0 * pier <= bay && sill + openingHeight + profile.lintelMin <= height &&
                          openingHeight > 0.3 && width > 0.5;
        if (!fits) { face(walls, f, 0.0, base, f.length, base + height); continue; }
        const double top = base + height;
        for (int i = 0; i < bays; ++i) {
            const double centre = (i + 0.5) * bay, s0 = centre - width * 0.5, s1 = centre + width * 0.5;
            const double t0 = base + sill, t1 = base + sill + openingHeight;
            face(walls, f, i * bay, base, (i + 1) * bay, t0);
            face(walls, f, i * bay, t1, (i + 1) * bay, top);
            face(walls, f, i * bay, t0, s0, t1);
            face(walls, f, s1, t0, (i + 1) * bay, t1);
            reveal(trim, f, s0, t0, s1, t1, profile.windowInset);
            face(glass, f, s0, t0, s1, t1, profile.windowInset);
        }
    }
    if (!levels.empty()) {
        const double last = levels.back().first + levels.back().second;
        if (wallHeight - last > 0.05) face(walls, f, 0.0, last, f.length, wallHeight);
    }
}

struct Planned {
    int64_t id;
    const Tags* tags;
    Ring ring;
    double ground, foundation;
    OrientedBox box;
    Gabarit gabarit;
    bool commercial, detailed;
};
}  // namespace

std::optional<double> taggedLength(const std::string& raw) {
    static const std::regex number(R"([-+]?\d+(?:[.,]\d+)?)");
    std::smatch m;
    if (!std::regex_search(raw, m, number)) return std::nullopt;
    std::string s = m.str(0);
    std::replace(s.begin(), s.end(), ',', '.');
    double value = std::strtod(s.c_str(), nullptr);
    if (raw.find("ft") != std::string::npos || raw.find('\'') != std::string::npos) value *= 0.3048;
    return value;
}

nlohmann::json BuildingStats::json() const {
    return {{"count", total}, {"rejected", rejected}, {"heightMeasured", heightMeasured},
            {"heightInferred", heightInferred}, {"roofTagged", roofTagged}, {"roofInferred", roofInferred},
            {"roofFlattenedForShape", roofFlattened}, {"facadesDetailed", detailed}, {"partyWalls", partyWalls},
            {"steeples", steeples}, {"roofShapes", shapes}};
}

BuildingOutput buildBuildings(const std::vector<const OsmWay*>& ways,
                              const std::function<P3(double, double)>& groundOf, const RegionProfile& profile,
                              P2 detailCenter, double detailRadius, double roofThickness,
                              const MaterialFor& wallMaterials, const MaterialFor& roofMaterials, BuildingLod lod) {
    BuildingOutput out;
    BuildingStats& stats = out.stats;
    std::vector<Planned> planned;
    PartyWallIndex party;
    for (const OsmWay* way : ways) {
        std::vector<P2> raw(way->points.begin(), way->points.end() - 1);
        std::vector<P3> placed;
        Ring flat;
        for (const P2& p : raw) { placed.push_back(groundOf(p.x, p.y)); flat.push_back({placed.back().x, placed.back().z}); }
        auto ring = cleanFootprint(flat);
        if (!ring) { ++stats.rejected; continue; }
        double groundSum = 0;
        for (const P3& p : placed) groundSum += p.y;
        const double ground = groundSum / placed.size();
        // Floors stay level; the downhill gap is closed with masonry, sampled
        // along long edges too so a hollow cannot open a hole under a facade.
        std::vector<double> samples;
        for (const P3& p : placed) samples.push_back(p.y);
        for (size_t i = 0; i < raw.size(); ++i) {
            const P2 a = raw[i], b = raw[(i + 1) % raw.size()];
            const P3 pa = placed[i], pb = placed[(i + 1) % raw.size()];
            const double span = std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y) + (pa.z - pb.z) * (pa.z - pb.z));
            const int n = std::max(1, int(std::ceil(span / 4.0)));
            for (int j = 1; j < n; ++j)
                samples.push_back(groundOf(a.x + (b.x - a.x) * j / n, a.y + (b.y - a.y) * j / n).y);
        }
        const double foundation = *std::min_element(samples.begin(), samples.end()) - 0.30;
        const OrientedBox box = orientedBox(*ring);
        const double rectangularity = box.area() > 1e-6 ? std::abs(polygonArea(*ring)) / box.area() : 0.0;
        const bool commercial = profile.isCommercial(way->tags);
        Gabarit gabarit = planGabarit(way->tags, way->id, profile, box, rectangularity, commercial);
        if (const std::string kind = aviationKind(way->tags); !kind.empty()) {
            if (gabarit.heightSource == "atlas") {
                gabarit.wallHeight = kind == "terminal" ? 15.0 : 12.0;
                gabarit.storeys = kind == "terminal" ? 3 : 1;
                gabarit.heightSource = "atlas:aviation";
            }
            if (gabarit.roofSource != "tag:shape") {
                // A tagged total height stays the total: the roof it no longer
                // has goes back into the walls.
                if (gabarit.heightSource == "tag:height") gabarit.wallHeight += gabarit.roofHeight;
                gabarit.roofShape = "flat";
                gabarit.roofHeight = 0.0;
                gabarit.roofSource = "atlas:aviation";
            }
        }
        double cx = 0, cz = 0;
        for (const P2& p : *ring) { cx += p.x; cz += p.y; }
        cx /= ring->size(); cz /= ring->size();
        const double distance = std::hypot(cx - detailCenter.x, cz - detailCenter.y);
        planned.push_back({way->id, &way->tags, std::move(*ring), ground, foundation, box, gabarit, commercial,
                           distance <= detailRadius});
    }
    for (size_t owner = 0; owner < planned.size(); ++owner) {
        const Ring& r = planned[owner].ring;
        for (size_t e = 0; e < r.size(); ++e) party.add(int(owner), r[e], r[(e + 1) % r.size()]);
    }

    MeshBook walls, foundations, roofs;
    roofs.mode = roofMaterials ? UvMode::Slope : UvMode::None;
    Mesh trim, glass;
    for (size_t owner = 0; owner < planned.size(); ++owner) {
        const Planned& b = planned[owner];
        const Gabarit& g = b.gabarit;
        ++stats.total;
        out.footprints.push_back(b.ring);
        ++stats.shapes[g.roofShape];
        if (g.heightSource.rfind("atlas", 0) == 0) ++stats.heightInferred; else ++stats.heightMeasured;
        if (g.roofSource == "tag:shape") ++stats.roofTagged; else ++stats.roofInferred;
        if (g.roofSource == "atlas:not-rectangular") ++stats.roofFlattened;
        if (b.detailed) ++stats.detailed;

        PyRandom wallRng = seeded(b.id, kSaltWall), roofRng = seeded(b.id, kSaltRoof);
        // Drawn whatever the building is, so no other building's draw moves.
        const Swatch& regionalWall = profile.wallSwatch(wallRng);
        const Swatch& regionalRoof = profile.roofSwatch(roofRng);
        const std::string aviation = aviationKind(*b.tags);
        const Swatch& wallSwatch = aviation == "terminal" ? kTerminalWall : aviation == "hangar" ? kHangarWall : regionalWall;
        const Swatch& roofSwatch = aviation.empty() ? regionalRoof : kAviationRoof;
        Mesh& wallMesh = walls.mesh(wallSwatch);
        Mesh& roofMesh = roofs.mesh(roofSwatch);
        const double yEave = b.ground + g.wallHeight;
        // A steeple rises to about 2.8 walls and its spire above that
        // (emitSteeple); an estimate that errs tall, since it is a clearance.
        out.tops.push_back(isWorship(*b.tags) ? b.ground + std::max(12.0, g.wallHeight * 2.8) * 1.6
                           : yEave + (g.roofShape == "flat" ? profile.parapetHeight : g.roofHeight));
        const auto levels = floorLevels(g.wallHeight, g.storeys, profile, b.commercial);
        const bool raised = truthy(taggedLength(tagOr(*b.tags, "min_height"))) ||
                            truthy(taggedLength(tagOr(*b.tags, "building:min_level")));
        const size_t count = b.ring.size();
        for (size_t e = 0; e < count; ++e) {
            const P2 p0 = b.ring[e], p1 = b.ring[(e + 1) % count];
            auto frame = wallFrame(p0, p1, b.ground);
            if (!frame) continue;
            const bool unifiedBase = lod != BuildingLod::Full && !b.detailed;
            const double base = !raised && unifiedBase ? b.foundation - b.ground : 0.0;
            if (!raised && !unifiedBase)
                face(foundations.mesh(wallSwatch), *frame, 0.0, b.foundation - b.ground, frame->length, 0.0);
            const bool shared = party.isParty(int(owner), p0, p1);
            if (shared) ++stats.partyWalls;
            if (b.detailed && !shared && frame->length >= 2.0) {
                emitFacade(wallMesh, trim, glass, *frame, levels, g.wallHeight, profile, b.commercial);
            } else if (wallMaterials) {
                const double bays = std::max(1.0, double(pyround(frame->length / profile.bayWidth)));
                const std::pair<double, double> scale{bays / frame->length, g.storeys / std::max(1e-3, g.wallHeight)};
                face(wallMesh, *frame, 0.0, base, frame->length, g.wallHeight, 0.0, &scale);
            } else {
                face(wallMesh, *frame, 0.0, base, frame->length, g.wallHeight);
            }
        }
        for (const auto& t : triangulate(b.ring)) {
            const P2 pa = b.ring[size_t(t[0])], pb = b.ring[size_t(t[1])], pc = b.ring[size_t(t[2])];
            roofMesh.addUpTriangle({pa.x, yEave, pa.y}, {pb.x, yEave, pb.y}, {pc.x, yEave, pc.y});
        }
        const bool worship = isWorship(*b.tags);
        if (g.roofShape == "flat") {
            if (lod != BuildingLod::SimpleRoofline)
                emitParapet(wallMesh, b.ring, yEave, profile.parapetHeight);
            if (worship) {
                emitSteeple(wallMesh, roofMesh, b.box, b.ground, g.wallHeight, steepleStyle(*b.tags));
                ++stats.steeples;
            }
            continue;
        }
        if (worship) {
            emitSteeple(wallMesh, roofMesh, b.box, b.ground, g.wallHeight, steepleStyle(*b.tags));
            ++stats.steeples;
        }
        const double sign = slopeSign(tagOr(*b.tags, "roof:direction"));
        const double yRidge = yEave + g.roofHeight;
        for (auto& loop : roofLoops(b.box, g.roofShape, profile.eaveOverhang, yEave, yRidge, sign))
            addSlab(roofMesh, loop, roofThickness);
        for (const auto& gable : gableWalls(b.box, g.roofShape, yEave, yRidge, sign))
            wallMesh.addTriangle(gable[0], gable[1], gable[2]);
    }
    walls.parts(out.parts, "Walls", false, wallMaterials);
    foundations.parts(out.parts, "Foundations", false, nullptr);
    roofs.parts(out.parts, "Roofs", roofThickness <= 0.0, roofMaterials);
    if (!trim.empty()) {
        Material m; m.name = profile.trim.name; m.roughness = profile.trim.roughness;
        m.color = {profile.trim.color[0], profile.trim.color[1], profile.trim.color[2], 1.0};
        out.parts.push_back({"Openings \xE2\x80\x94 reveals", std::move(trim), m});
    }
    if (!glass.empty()) {
        Material m; m.name = profile.glass.name; m.roughness = profile.glass.roughness;
        m.color = {profile.glass.color[0], profile.glass.color[1], profile.glass.color[2], 1.0};
        out.parts.push_back({"Openings \xE2\x80\x94 glazing", std::move(glass), m});
    }
    return out;
}

}  // namespace r1
