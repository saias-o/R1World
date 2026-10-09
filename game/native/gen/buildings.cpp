#include "buildings.hpp"
#include "fuel.hpp"
#include "clip.hpp"

#include <algorithm>
#include <limits>
#include <regex>
#include <set>

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

std::optional<int> taggedStoreys(const Tags& tags) {
    const auto n = taggedLength(tagOr(tags, "building:levels"));
    if (!n || *n < 1 || *n > std::numeric_limits<int>::max()) return std::nullopt;
    return std::max(1, int(pyround(*n)));
}

int estimatedStoreys(double walls, const RegionProfile& profile) {
    return int(std::clamp(std::nearbyint(walls / profile.storeyHeight),
                          1., double(std::numeric_limits<int>::max())));
}

// A mapped total includes its roof and parapet: decoration must fit inside it.
void fitTotalHeight(Gabarit& g, double total) {
    const double reserve = std::min(2.4, total * 0.6);
    if (g.roofShape == "flat") {
        g.parapetHeight = std::min(g.parapetHeight, total - reserve);
        g.wallHeight = total - g.parapetHeight;
    } else {
        g.roofHeight = std::min(g.roofHeight, total - reserve);
        g.wallHeight = total - g.roofHeight;
    }
}

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
const Swatch kRetailWall{"Retail corrugated cladding", {0.19, 0.20, 0.205}, 0.8, 1.0};
const Swatch kRetailRoof{"Retail metal roof", {0.12, 0.13, 0.14}, 0.8, 1.0};
const Swatch kHangarWall{"Hangar cladding, corrugated steel", {0.21, 0.215, 0.22}, 0.55, 1.0};
const Swatch kAviationRoof{"Aviation roof, metal seam", {0.18, 0.185, 0.19}, 0.5, 1.0};

}  // namespace

Gabarit planGabarit(const Tags& tags, int64_t osmId, const RegionProfile& profile, const OrientedBox& box,
                    double rectangularity, bool commercial) {
    if(const auto* residential=residentialStyle(tagOr(tags,"r1:residential"));residential && (!commercial || residential->architectureOnly)) {
        RegionProfile home=residentialProfile(profile,*residential);
        Tags base=tags;base.erase("r1:residential");
        auto g=planGabarit(base,osmId,home,box,rectangularity,commercial);
        if(tagOr(tags,"r1:rural-annex")=="yes" && g.heightSource=="atlas") {
            g.storeys=1;g.wallHeight=2.3;g.heightSource="atlas:rural-annex";
            g.roofHeight=std::min(g.roofHeight,1.5);
        }
        if(g.heightSource=="atlas")g.heightSource="atlas:detached";
        if(g.roofSource=="atlas")g.roofSource="atlas:detached";
        return g;
    }
    PyRandom shapeRng = seeded(osmId, kSaltShape), heightRng = seeded(osmId, kSaltHeight);
    if(tagOr(tags,"r1:utility-hut")=="yes" && !has(tags,"height") && !has(tags,"building:levels") && !has(tags,"roof:shape"))
        return Gabarit{2.6,0,1,"atlas:utility-hut","flat","atlas:utility-hut"};
    Gabarit g;
    g.parapetHeight = profile.parapetHeight;
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
        if (taggedRoof && *taggedRoof > 0.0) g.roofHeight = *taggedRoof;
        else {
            const double run = g.roofShape != "skillion" ? box.halfV : box.halfV * 2.0;
            g.roofHeight = std::min(12.0, std::max(0.6, std::tan(radians(pitchDegrees)) * run));
        }
    }
    const auto levels = taggedStoreys(tags);
    const auto total = taggedLength(tagOr(tags, "height"));
    if (total && *total > 0.0) {
        fitTotalHeight(g, *total);
        g.storeys = levels ? *levels : estimatedStoreys(g.wallHeight, profile);
        g.heightSource = "tag:height";
    } else if (levels) {
        g.storeys = *levels;
        g.wallHeight = stack(g.storeys, profile, commercial);
        g.heightSource = "tag:levels";
    } else {
        g.storeys = profile.storeys(heightRng);
        g.wallHeight = stack(g.storeys, profile, commercial) + (heightRng.random() - 0.5) * 0.9;
        g.heightSource = "atlas";
    }
    if (g.heightSource == "atlas") g.wallHeight = std::max(2.4, g.wallHeight);
    if(g.heightSource=="atlas" && tagOr(tags,"wall")=="no" && tagOr(tags,"building")=="yes" && !commercial && !has(tags,"r1:fuel")) {
        g.storeys=1;g.wallHeight=2.6;g.roofHeight=std::min(g.roofHeight,1.5);
        g.heightSource="atlas:light-building";
    }
    else if(g.heightSource=="atlas" && openRoof(tags) && !has(tags,"r1:fuel") && !has(tags,"roof:shape")) {
        g.storeys=1;g.wallHeight=2.6;g.roofHeight=std::min(g.roofHeight,1.5);
        g.roofShape="flat";g.roofHeight=0;g.parapetHeight=0;g.roofSource="atlas:open-roof";
        g.heightSource="atlas:open-roof";
    }
    if (!truthy(taggedLength(tagOr(tags, "roof:height"))) && g.heightSource != "tag:height")
        g.roofHeight = std::min(g.roofHeight, g.wallHeight * kMaxRoofOverWall);
    return g;
}

namespace {

// ── mesh buckets: one mesh per swatch, not one per building ─────────────────
struct MeshBook {
    UvMode mode = UvMode::None;
    UV repeat{1.0, 1.0};
    std::map<std::string, std::pair<Swatch, Mesh>> meshes;
    using Mark = std::map<std::string, size_t>;
    Mark mark() const {
        Mark out;
        for (const auto& [name, entry] : meshes) out[name] = entry.second.indices.size();
        return out;
    }
    Mesh& mesh(const Swatch& s) {
        auto it = meshes.find(s.name);
        if (it == meshes.end()) {
            Mesh m(mode);
            m.facadeRepeat = repeat;
            it = meshes.emplace(s.name, std::make_pair(s, std::move(m))).first;
        }
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
    // Copy only this building's triangles from the shared tile buckets. The
    // plan and party-wall index are shared; welding remains local to a level.
    void capture(std::vector<MeshPart>& out, const Mark& before, const std::string& prefix,
                 bool doubleSided, const MaterialFor& materials) const {
        for (const auto& [name, entry] : meshes) {
            auto first = before.find(name);
            const size_t offset = first == before.end() ? 0 : first->second;
            if (offset == entry.second.indices.size()) continue;
            MeshBook one;
            one.mode = mode; one.repeat = repeat;
            Mesh& piece = one.mesh(entry.first);
            for (size_t i = offset; i < entry.second.indices.size(); ++i) {
                const uint32_t index = entry.second.indices[i];
                piece.indices.push_back(piece.vertex(entry.second.positions[index], entry.second.normals[index],
                                                       entry.second.texcoords[index]));
            }
            one.parts(out, prefix, doubleSided, materials);
        }
    }
};

Mesh slice(const Mesh& source, size_t offset) {
    Mesh out(source.uvMode); out.facadeRepeat = source.facadeRepeat;
    for (size_t i = offset; i < source.indices.size(); ++i) {
        const uint32_t index = source.indices[i];
        out.indices.push_back(out.vertex(source.positions[index], source.normals[index], source.texcoords[index]));
    }
    return out;
}

Material flatMaterial(const Swatch& swatch, bool doubleSided = false) {
    Material out;
    out.name = swatch.name; out.color = {swatch.color[0], swatch.color[1], swatch.color[2], 1.0};
    out.roughness = swatch.roughness; out.doubleSided = doubleSided;
    return out;
}

void addOpenings(std::vector<MeshPart>& parts, Mesh trim, Mesh glass, const RegionProfile& profile) {
    if (!trim.empty()) parts.push_back({"Openings \xE2\x80\x94 reveals", std::move(trim), flatMaterial(profile.trim)});
    if (!glass.empty()) parts.push_back({"Openings \xE2\x80\x94 glazing", std::move(glass), flatMaterial(profile.glass)});
}

// ── shopfronts: one part per material, not four per shop ────────────────────
// A part is a mesh to upload, a draw and a static body. Four per shop made the
// rue de Rivoli tile (v26_27772_23992) 849 parts, 820 of them shopfronts, and
// it took a minute to come up. The fascia is the one material that varies.
struct ShopfrontBook {
    std::vector<MeshPart> parts;
    void add(std::vector<MeshPart>&& shop) {
        for (MeshPart& part : shop) {
            auto same = std::find_if(parts.begin(), parts.end(), [&](const MeshPart& p) {
                return p.name == part.name && p.material == part.material;
            });
            if (same == parts.end()) { parts.push_back(std::move(part)); continue; }
            const Mesh& m = part.mesh;
            for (uint32_t i : m.indices) same->mesh.indices.push_back(same->mesh.vertex(m.positions[i], m.normals[i], m.texcoords[i]));
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

// Cadastral homes have bays, extensions and L-shaped corners. Clip each roof
// plane to their surveyed footprint instead of replacing the roof with a deck
// or covering the empty corner of the enclosing rectangle.
void footprintRoof(Mesh& roof,Mesh& walls,const Ring& ring,const OrientedBox& box,
                   const std::string& shape,double eave,double low,double high,double sign,double thickness) {
    const auto loops=roofLoops(box,shape,eave,low,high,sign);
    struct Plane {P3 at,n;double y(P2 p)const{return at.y-(n.x*(p.x-at.x)+n.z*(p.y-at.z))/n.y;}};
    std::vector<Plane> planes;
    const auto outline=eave>0?clip::bufferRing(ring,eave):clip::Paths64{clip::kMetres.path(ring)};
    for(const auto& loop:loops) {
        Plane plane{loop[0],faceNormal(loop[0],loop[1],loop[2])};
        if(std::abs(plane.n.y)<1e-8)continue;
        planes.push_back(plane);
        Ring flat;for(const auto& p:loop)flat.push_back({p.x,p.z});
        for(const auto& polygon:clip::polygons(clip::intersect(outline,{clip::kMetres.path(flat)})))
            for(const auto& t:clip::triangles(polygon)) {
                std::vector<P3> triangle;for(auto p:t)triangle.push_back({p.x,plane.y(p),p.y});
                addSlab(roof,std::move(triangle),thickness);
            }
    }
    auto top=[&](P2 p){double y=high;for(const auto& plane:planes)y=std::min(y,plane.y(p));return std::max(low,y);};
    for(size_t i=0;i<ring.size();++i) {
        const auto a=ring[i],b=ring[(i+1)%ring.size()];std::vector<double> cuts{0,1};
        for(size_t p=0;p<planes.size();++p)for(size_t q=p+1;q<planes.size();++q) {
            const double da=planes[p].y(a)-planes[q].y(a),db=planes[p].y(b)-planes[q].y(b);
            if(da*db<0)cuts.push_back(da/(da-db));
        }
        std::sort(cuts.begin(),cuts.end());
        for(size_t c=1;c<cuts.size();++c) {
            if(cuts[c]-cuts[c-1]<1e-8)continue;
            P2 from{a.x+(b.x-a.x)*cuts[c-1],a.y+(b.y-a.y)*cuts[c-1]};
            P2 to{a.x+(b.x-a.x)*cuts[c],a.y+(b.y-a.y)*cuts[c]};
            const double yf=top(from),yt=top(to);
            if(yf>low+1e-6)walls.addTriangle({from.x,low,from.y},{from.x,yf,from.y},{to.x,yt,to.y});
            if(yt>low+1e-6)walls.addTriangle({from.x,low,from.y},{to.x,yt,to.y},{to.x,low,to.y});
        }
    }
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
    double ground, foundation, interiorFloor;
    OrientedBox box;
    Gabarit gabarit;
    bool commercial, detailed;
    std::vector<OrientedBox> roofBoxes;
};

// Ancillary structures and monuments do not describe the height of a street's
// ordinary buildings. Their own mapped dimensions still take precedence.
bool ordinaryHeightNeighbour(const Tags& tags) {
    static const std::set<std::string> ancillary = {"shed", "garage", "garages", "roof", "carport",
        "hut", "service", "kiosk", "greenhouse", "ruins", "tower", "lighthouse"};
    return !ancillary.count(tagOr(tags,"building")) && !openRoof(tags) && !isWorship(tags) &&
           aviationKind(tags).empty() && !has(tags,"r1:bus-shelter") &&
           tagOr(tags,"r1:rural-annex")!="yes" && tagOr(tags,"r1:utility-hut")!="yes" &&
           tagOr(tags,"wall")!="no" &&
           tagOr(tags,"man_made")!="tower" && tagOr(tags,"man_made")!="lighthouse";
}

struct HeightReference {
    int64_t id;
    P2 center;
    double height;
    bool measured;
    double area;
    const Tags* tags;
};
std::vector<OrientedBox> homeRoofWings(const Ring& ring,const OrientedBox& box);
OrientedBox roofPlanBox(const OrientedBox& box,const std::vector<OrientedBox>& wings) {
    auto out=box;
    if(!wings.empty()) {
        out.halfV=0;for(const auto& wing:wings)out.halfV=std::max(out.halfV,wing.halfV);
    }
    return out;
}

// A fixed local radius prevents a distant city's heights leaking into a village.
// The grid bounds lookup cost; ID resolves distance ties independently of OSM order.
struct HeightNeighbourhood {
    static constexpr double radius = 250., cellSize = 50.;
    std::vector<HeightReference> references;
    std::map<std::pair<int,int>,std::vector<size_t>> cells;
    static std::pair<int,int> cell(P2 p) {
        return {int(std::floor(p.x/cellSize)),int(std::floor(p.y/cellSize))};
    }
    HeightNeighbourhood(const std::vector<const OsmWay*>& ways,
                        const std::vector<const OsmWay*>& surroundings,
                        const std::function<P3(double,double)>& groundOf,const RegionProfile& profile) {
        std::map<int64_t,const OsmWay*> unique;
        for(const auto* w:surroundings)unique[w->id]=w;
        // Enriched local tags win over the same raw way in the neighbourhood.
        for(const auto* w:ways)unique[w->id]=w;
        for(const auto& [id,w]:unique) {
            if(w->points.size()<4 || !w->closed() || !ordinaryHeightNeighbour(w->tags))continue;
            const auto height=taggedLength(tagOr(w->tags,"height"));
            const bool measured=height && *height>0;
            if(!measured && !taggedStoreys(w->tags))continue;
            Ring ring;
            for(size_t i=0;i+1<w->points.size();++i) {
                const auto p=groundOf(w->points[i].x,w->points[i].y);ring.push_back({p.x,p.z});
            }
            const auto clean=cleanFootprint(ring);
            if(!clean)continue;
            const auto box=orientedBox(*clean);
            const double rectangularity=std::abs(polygonArea(*clean))/box.area();
            const bool home=residentialStyle(tagOr(w->tags,"r1:residential"));
            const auto wings=home && rectangularity<kRectangularityForPitch?homeRoofWings(*clean,box):std::vector<OrientedBox>{};
            const auto roofBox=roofPlanBox(box,wings);
            const bool homePitch=home && roofBox.halfV*2<=kMaxRidgeSpan;
            const double total=measured?*height:
                planGabarit(w->tags,id,profile,roofBox,homePitch?1.:rectangularity,
                            profile.isCommercial(w->tags)).totalHeight();
            references.push_back({id,centroid(*clean),total,measured,std::abs(polygonArea(*clean)),&w->tags});
            cells[cell(references.back().center)].push_back(references.size()-1);
        }
    }
    std::vector<std::pair<double,size_t>> nearest(int64_t id,P2 at,const Tags& tags,double area) const {
        std::vector<std::pair<double,size_t>> measured,levels;
        const auto lo=cell({at.x-radius,at.y-radius}),hi=cell({at.x+radius,at.y+radius});
        auto before=[&](const auto& a,const auto& b) {
            return a.first<b.first || (a.first==b.first && references[a.second].id<references[b.second].id);
        };
        for(int x=lo.first;x<=hi.first;++x)for(int z=lo.second;z<=hi.second;++z) {
            auto found=cells.find({x,z});if(found==cells.end())continue;
            for(size_t n:found->second) {
                const auto& ref=references[n];if(ref.id==id)continue;
                // A house borrows dimensions from comparable homes, not a
                // school hall, apartment tower or tiny cadastral outbuilding.
                const auto kind=tagOr(tags,"building");
                if(has(tags,"r1:residential") || kind=="house" || kind=="detached" || kind=="bungalow") {
                    const auto donor=tagOr(*ref.tags,"building");
                    if(!donor.empty() && donor!="yes" && donor!="house" && donor!="detached" &&
                       donor!="residential" && donor!="bungalow" && donor!="terrace" && donor!="cabin")continue;
                    bool publicUse=false;
                    for(const char* key:{"amenity","shop","office","industrial","tourism"})publicUse |= has(*ref.tags,key);
                    if(publicUse || ref.area<45 || ref.area<area/3 || ref.area>area*3)continue;
                    const auto floors=taggedStoreys(*ref.tags);
                    if(has(tags,"r1:residential") && floors && *floors>2)continue;
                }
                const double distance=dist(at,ref.center);if(distance>radius)continue;
                auto& best=ref.measured?measured:levels;
                best.push_back({distance,n});std::sort(best.begin(),best.end(),before);
                if(best.size()>3)best.resize(3);
            }
        }
        return measured.empty()?levels:measured;
    }
};
// A small orthogonal home can be several joined wings. Partition its actual
// footprint into rectangles, rather than flattening its roof or roofing the
// empty corner of its enclosing box. Non-orthogonal/courtyard cases stay on
// the existing fallback; this bounded pass never changes urban buildings.
std::vector<OrientedBox> homeRoofWings(const Ring& ring,const OrientedBox& box) {
    Ring local;std::vector<double> us,vs;
    for(auto p:ring) {
        const double dx=p.x-box.cx,dz=p.y-box.cz;
        P2 q{dx*box.ux+dz*box.uz,dx*box.vx()+dz*box.vz()};
        local.push_back(q);us.push_back(q.x);vs.push_back(q.y);
    }
    for(size_t i=0;i<local.size();++i) {
        const auto a=local[i],b=local[(i+1)%local.size()];
        if(std::abs(a.x-b.x)>.18 && std::abs(a.y-b.y)>.18)return {};
    }
    auto weld=[](std::vector<double>& values) {
        std::sort(values.begin(),values.end());std::vector<double> unique;
        for(double v:values)if(unique.empty() || v-unique.back()>.18)unique.push_back(v);
        values=std::move(unique);
    };
    weld(us);weld(vs);
    if(us.size()<2 || vs.size()<2 || us.size()>12 || vs.size()>12)return {};
    struct Rect {size_t first,last;double low,high;};
    std::vector<Rect> rectangles;
    for(size_t v=1;v<vs.size();++v)for(size_t u=1;u<us.size();) {
        if(!pointInPolygon({(us[u-1]+us[u])/2,(vs[v-1]+vs[v])/2},local)){++u;continue;}
        const size_t first=u-1;
        while(u<us.size() && pointInPolygon({(us[u-1]+us[u])/2,(vs[v-1]+vs[v])/2},local))++u;
        const size_t last=u-1;
        auto previous=std::find_if(rectangles.begin(),rectangles.end(),[&](const Rect& r) {
            return r.first==first && r.last==last && std::abs(r.high-vs[v-1])<.01;
        });
        if(previous!=rectangles.end())previous->high=vs[v];
        else rectangles.push_back({first,last,vs[v-1],vs[v]});
    }
    if(rectangles.size()<2 || rectangles.size()>6)return {};
    double covered=0;std::vector<OrientedBox> result;
    for(const auto& r:rectangles) {
        const double du=us[r.last]-us[r.first],dv=r.high-r.low;
        if(std::min(du,dv)<1.2 || std::min(du,dv)>kMaxRidgeSpan)return {};
        const auto centre=box.point((us[r.first]+us[r.last])/2,(r.low+r.high)/2);
        OrientedBox wing;wing.cx=centre.x;wing.cz=centre.y;
        wing.ux=du>=dv?box.ux:box.vx();wing.uz=du>=dv?box.uz:box.vz();
        wing.halfU=std::max(du,dv)/2;wing.halfV=std::min(du,dv)/2;
        result.push_back(wing);covered+=du*dv;
    }
    if(std::abs(covered-std::abs(polygonArea(ring)))>std::max(1.,covered*.015))return {};
    return result;
}
}  // namespace

std::optional<double> taggedLength(const std::string& raw) {
    static const std::regex number(R"([-+]?\d+(?:[.,]\d+)?)");
    std::smatch m;
    if (!std::regex_search(raw, m, number)) return std::nullopt;
    std::string s = m.str(0);
    std::replace(s.begin(), s.end(), ',', '.');
    double value = std::strtod(s.c_str(), nullptr);
    if (raw.find("ft") != std::string::npos || raw.find('\'') != std::string::npos) value *= 0.3048;
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

nlohmann::json BuildingStats::json() const {
    return {{"count", total}, {"rejected", rejected}, {"heightMeasured", heightMeasured},
            {"heightInferred", heightInferred}, {"roofTagged", roofTagged}, {"roofInferred", roofInferred},
            {"roofFlattenedForShape", roofFlattened}, {"facadesDetailed", detailed}, {"partyWalls", partyWalls},
            {"steeples", steeples}, {"roofShapes", shapes}, {"busShelters",busShelters},
            {"heightFromLevels",heightFromLevels}, {"heightFromNeighbours",heightFromNeighbours},
            {"heightFromAtlas",heightFromAtlas}, {"neighbourHeights",neighbourHeights},
            {"ruralGabarits",ruralGabarits}};
}

namespace {
std::vector<Planned> planBuildings(const std::vector<const OsmWay*>& ways,
    const std::function<P3(double,double)>& groundOf, const RegionProfile& profile,
    P2 detailCenter, double detailRadius, BuildingStats& stats,
    const std::vector<const OsmWay*>& surroundings) {
    std::vector<Planned> planned;
    const HeightNeighbourhood heights(ways,surroundings,groundOf,profile);
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
        const auto* home=residentialStyle(tagOr(way->tags,"r1:residential"));
        std::vector<OrientedBox> roofBoxes;
        if(home && rectangularity<kRectangularityForPitch &&
           tagOr(way->tags,"roof:shape")!="flat" && !commercial)roofBoxes=homeRoofWings(*ring,box);
        const bool footprintPitch=home && !commercial && rectangularity<kRectangularityForPitch && box.halfV*2<=kMaxRidgeSpan;
        Gabarit gabarit = planGabarit(way->tags, way->id, profile, roofPlanBox(box,roofBoxes),
            roofBoxes.empty() && !footprintPitch?rectangularity:1., commercial);
        if(!roofBoxes.empty() && gabarit.roofShape!="flat" && gabarit.roofSource.rfind("atlas",0)==0)
            gabarit.roofSource="atlas:compound-home";
        else if(footprintPitch && gabarit.roofShape!="flat" && gabarit.roofSource.rfind("atlas",0)==0)
            gabarit.roofSource="atlas:footprint-home";
        const bool retail = retailUse(way->tags);
        if (retail && (tagOr(way->tags,"building")=="retail" || tagOr(way->tags,"building")=="supermarket" || tagOr(way->tags,"building")=="mall")) {
            if (gabarit.heightSource=="atlas") { gabarit.wallHeight=retailRecipe(way->tags)=="mall"?7.5:5.2;gabarit.storeys=1; }
            if (gabarit.roofSource!="tag:shape") {
                const double total = gabarit.totalHeight();
                gabarit.roofShape="flat";gabarit.roofHeight=0;gabarit.roofSource="atlas:retail";
                if(gabarit.heightSource=="tag:height")fitTotalHeight(gabarit,total);
            }
        }
        if (const std::string kind = aviationKind(way->tags); !kind.empty()) {
            if (gabarit.heightSource == "atlas") {
                gabarit.wallHeight = kind == "terminal" ? 15.0 : 12.0;
                gabarit.storeys = kind == "terminal" ? 3 : 1;
                gabarit.heightSource = "atlas:aviation";
            }
            if (gabarit.roofSource != "tag:shape") {
                // A tagged total height stays the total: the roof it no longer
                // has goes back into the walls.
                const double total = gabarit.totalHeight();
                gabarit.roofShape = "flat";
                gabarit.roofHeight = 0.0;
                gabarit.roofSource = "atlas:aviation";
                if(gabarit.heightSource=="tag:height")fitTotalHeight(gabarit,total);
            }
        }
        double cx = 0, cz = 0;
        for (const P2& p : *ring) { cx += p.x; cz += p.y; }
        cx /= ring->size(); cz /= ring->size();
        const double distance = std::hypot(cx - detailCenter.x, cz - detailCenter.y);
        const double interiorFloor=*std::max_element(samples.begin(),samples.end())+.08;
        const double floor=retail?interiorFloor:ground;
        planned.push_back({way->id, &way->tags, std::move(*ring), floor, foundation, interiorFloor, box, gabarit, commercial,
                           distance <= detailRadius,std::move(roofBoxes)});
    }
    // Only original height/level observations are references. Inferred buildings
    // never become donors, so neither ordering nor a chain of guesses changes the result.
    for(auto& b:planned) {
        if(b.gabarit.heightSource.rfind("atlas",0)!=0 || !ordinaryHeightNeighbour(*b.tags))continue;
        const auto neighbours=heights.nearest(b.id,centroid(b.ring),*b.tags,std::abs(polygonArea(b.ring)));
        if(neighbours.empty())continue;
        double total=0;
        nlohmann::json refs=nlohmann::json::array();
        for(const auto& [distance,n]:neighbours) {
            const auto& ref=heights.references[n];total+=ref.height;
            refs.push_back({{"building",ref.id},{"heightM",ref.height},{"distanceM",pyround(distance,2)},
                            {"source",ref.measured?"tag:height":"tag:levels"}});
        }
        total/=neighbours.size();
        fitTotalHeight(b.gabarit,total);
        const auto* home=residentialStyle(tagOr(*b.tags,"r1:residential"));
        const RegionProfile local=home?residentialProfile(profile,*home):profile;
        b.gabarit.storeys=estimatedStoreys(b.gabarit.wallHeight,local);
        b.gabarit.heightSource="neighbours";
        stats.neighbourHeights.push_back({{"building",b.id},{"heightM",total},
            {"source","inferred:neighbours"},{"radiusM",HeightNeighbourhood::radius},{"references",std::move(refs)}});
    }
    return planned;
}
double plannedHeight(const Planned& b) {
    return isWorship(*b.tags) && b.gabarit.heightSource!="tag:height"
        ? std::max(12.,b.gabarit.wallHeight*2.8)*1.6 : b.gabarit.totalHeight();
}
}  // namespace

std::map<int64_t,double> predictBuildingHeights(const std::vector<const OsmWay*>& ways,
    const std::function<P3(double,double)>& groundOf, const RegionProfile& profile,
    const std::vector<const OsmWay*>& surroundings) {
    BuildingStats stats;
    std::map<int64_t,double> heights;
    for (const auto& b:planBuildings(ways,groundOf,profile,{},-1,stats,surroundings))
        heights[b.id]=plannedHeight(b);
    return heights;
}

BuildingOutput buildBuildings(const std::vector<const OsmWay*>& ways,
                              const std::function<P3(double, double)>& groundOf, const RegionProfile& profile,
                              P2 detailCenter, double detailRadius, double roofThickness,
                              const MaterialFor& wallMaterials, const MaterialFor& roofMaterials, BuildingLod lod,
                              const std::vector<const OsmWay*>& surroundings) {
    BuildingOutput out;
    BuildingStats& stats=out.stats;
    auto planned=planBuildings(ways,groundOf,profile,detailCenter,detailRadius,stats,surroundings);
    PartyWallIndex party;
    CanopyBook canopies;
    for (size_t owner = 0; owner < planned.size(); ++owner) {
        // A roof on posts shares no wall with anyone.
        if (openRoof(*planned[owner].tags) || has(*planned[owner].tags,"r1:bus-shelter")) continue;
        const Ring& r = planned[owner].ring;
        for (size_t e = 0; e < r.size(); ++e) party.add(int(owner), r[e], r[(e + 1) % r.size()]);
    }

    MeshBook walls, foundations, roofs, ruralTrim;
    ShopfrontBook shopfronts;
    roofs.mode = roofMaterials ? UvMode::Slope : UvMode::None;
    // What a wall is given without UVs -- a gable, a parapet, a steeple --
    // still shows its sheet at a bay and a storey, never one texel of it.
    if (wallMaterials) {
        walls.mode = UvMode::Facade;
        walls.repeat = {profile.bayWidth, profile.storeyHeight};
    }
    Mesh trim, glass, shelterWood(UvMode::Slope),shelterRoof(UvMode::Slope),shelterFrame;
    for (size_t owner = 0; owner < planned.size(); ++owner) {
        const Planned& b = planned[owner];
        const Gabarit& g = b.gabarit;
        if(has(*b.tags,"r1:bus-shelter")) {
            const auto& box=b.box;
            const P2 front{taggedLength(tagOr(*b.tags,"r1:shelter-front-x")).value_or(0),
                           taggedLength(tagOr(*b.tags,"r1:shelter-front-z")).value_or(0)};
            const double sign=front.x*box.vx()+front.y*box.vz()>=0?1:-1;
            const auto taggedHeight=taggedLength(tagOr(*b.tags,"height"));
            const double height=taggedHeight && *taggedHeight>=1 && *taggedHeight<=12 ? *taggedHeight : 2.25;
            const double yaw=std::atan2(-box.uz,box.ux);
            auto point=[&](double u,double v,double y){const auto p=box.point(u,v);return P3{p.x,b.ground+y,p.y};};
            shelterRoof.addBox(point(0,0,height),{box.halfU*2+.24,.12,box.halfV*2+.24},yaw);
            shelterWood.addBox(point(0,-sign*box.halfV,height*.48),{box.halfU*2,.96*height,.065},yaw);
            for(double u:{-box.halfU,box.halfU}) {
                shelterWood.addBox(point(u,0,height*.48),{.065,.96*height,box.halfV*2},yaw);
                shelterFrame.addBox(point(u,sign*box.halfV,height*.5),{.09,height,.09},yaw);
            }
            shelterWood.addBox(point(0,-sign*(box.halfV-.25),.48),{std::max(.5,box.halfU*1.7),.08,.4},yaw);
            // Only the three walls are obstacles. The open front is playable,
            // and no full house/interior/door is synthesised for a bus stop.
            for(int side=0;side<3;++side) {
                const double u0=side==0?-box.halfU:side==1?-box.halfU:box.halfU-.08;
                const double u1=side==0?box.halfU:side==1?-box.halfU+.08:box.halfU;
                const double v0=side==0?-sign*box.halfV: -box.halfV;
                const double v1=side==0?v0+sign*.08:box.halfV;
                out.footprints.push_back({box.point(u0,v0),box.point(u1,v0),box.point(u1,v1),box.point(u0,v1)});
                out.tops.push_back(b.ground+height);
            }
            ++stats.busShelters;
            continue;
        }
        // An open roof is no footprint: people and cars pass under it. What
        // stands under a fuel canopy is added as obstacles below. It is
        // counted apart from the buildings (the manifest's fuel.openRoofs).
        if (openRoof(*b.tags)) {
            buildOpenRoof(b.ring, b.box, b.ground, g.wallHeight, *b.tags, b.id, canopies);
            ++out.openRoofs;
            continue;
        }
        ++stats.total;
        out.footprints.push_back(b.ring);
        ++stats.shapes[g.roofShape];
        if (g.heightSource == "tag:height") ++stats.heightMeasured;
        else {
            ++stats.heightInferred;
            if(g.heightSource=="tag:levels")++stats.heightFromLevels;
            else if(g.heightSource=="neighbours")++stats.heightFromNeighbours;
            else ++stats.heightFromAtlas;
        }
        if (g.roofSource == "tag:shape") ++stats.roofTagged; else ++stats.roofInferred;
        if (g.roofSource == "atlas:not-rectangular") ++stats.roofFlattened;
        if (b.detailed) ++stats.detailed;

        PyRandom wallRng = seeded(b.id, kSaltWall), roofRng = seeded(b.id, kSaltRoof);
        // Drawn whatever the building is, so no other building's draw moves.
        const Swatch& regionalWall = profile.wallSwatch(wallRng);
        const Swatch& regionalRoof = profile.roofSwatch(roofRng);
        PyRandom homeRoofRng=seeded(b.id,kSaltRoof);
        const auto* residential=residentialStyle(tagOr(*b.tags,"r1:residential"));
        if(residential)stats.ruralGabarits.push_back({{"building",b.id},{"rule",residential->key},
            {"storeys",g.storeys},{"wallHeight",g.wallHeight},{"roofHeight",g.roofHeight},
            {"heightSource",g.heightSource},{"roofShape",g.roofShape},{"roofSource",g.roofSource},
            {"roofComponents",b.roofBoxes.empty()?1:b.roofBoxes.size()}});
        std::optional<RegionProfile> homeProfile;
        if(residential)homeProfile=residentialProfile(profile,*residential);
        const RegionProfile& facadeProfile=homeProfile?*homeProfile:profile;
        PyRandom homeWallRng=seeded(b.id,kSaltWall);
        const bool mappedWall=has(*b.tags,"building:material") || has(*b.tags,"building:colour") || has(*b.tags,"building:color");
        const Swatch& houseWall=residential && !residential->walls.empty() && !mappedWall ? RegionProfile::pick(residential->walls,homeWallRng) : regionalWall;
        const bool mappedRoof=has(*b.tags,"roof:material") || has(*b.tags,"roof:colour") || has(*b.tags,"roof:color");
        const Swatch& houseRoof=residential && !mappedRoof ? RegionProfile::pick(residential->roofs,homeRoofRng) : regionalRoof;
        const std::string aviation = aviationKind(*b.tags);
        const bool retailCladding=retailUse(*b.tags)&&tagOr(*b.tags,"building")=="retail"&&g.storeys<=2;
        const Swatch& wallSwatch = aviation == "terminal" ? kTerminalWall : aviation == "hangar" ? kHangarWall : retailCladding?kRetailWall:houseWall;
        const Swatch& roofSwatch = aviation.empty() ? (retailCladding&&g.roofShape=="flat"?kRetailRoof:houseRoof) : kAviationRoof;
        const double yEave = b.ground + g.wallHeight;
        // A steeple rises to about 2.8 walls and its spire above that
        // (emitSteeple); an estimate that errs tall, since it is a clearance.
        const bool inferSteeple=isWorship(*b.tags) && g.heightSource!="tag:height";
        out.tops.push_back(b.ground+plannedHeight(b));
        // Very tall buildings keep their full height and floor count. Above this
        // detail budget, their existing facade textures replace modelled windows.
        double perimeter=0;
        for(size_t e=0;e<b.ring.size();++e)perimeter+=dist(b.ring[e],b.ring[(e+1)%b.ring.size()]);
        const bool modelOpenings=g.storeys<=128 && perimeter/facadeProfile.bayWidth*g.storeys<=1024;
        const auto levels = modelOpenings ? floorLevels(g.wallHeight,g.storeys,facadeProfile,b.commercial)
                                         : std::vector<std::pair<double,double>>{};
        const bool raised = truthy(taggedLength(tagOr(*b.tags, "min_height"))) ||
                            truthy(taggedLength(tagOr(*b.tags, "building:min_level")));
        std::optional<InteriorPlan> interior;
        std::vector<MeshPart> shop;
        if (!interiorRecipe(*b.tags).empty() && !raised && yEave-b.interiorFloor>=2.2) {
            InteriorPlan p;p.id=b.id;p.ring=b.ring;p.floor=b.interiorFloor;p.footprint=out.footprints.size()-1;
            p.recipe=interiorRecipe(*b.tags);p.name=interiorName(*b.tags);p.region=profile.key;
            p.useSource=tagOr(*b.tags,"r1:useSource","inferred:building");
            p.doorStyle=retailInterior(p.recipe)?"sliding":"swing";
            p.width=retailInterior(p.recipe)?2.4:p.recipe=="garage"?2.8:1.2;
            p.ceiling=p.floor+std::min(retailInterior(p.recipe)?4.2:p.recipe=="garage"?4.:3.,yEave-p.floor-.1);
            p.nameSource=has(*b.tags,"name")?"measured:name":has(*b.tags,"brand")?"measured:brand":"synthesized";
            p.entranceSource=tagOr(*b.tags,"r1:entranceSource","inferred");
            if(const auto at=retailPoints(tagOr(*b.tags,"r1:anchor"));!at.empty()) {
                p.anchor=at.front();p.anchorName=tagOr(*b.tags,"r1:anchorName");
            }
            auto targets=retailFronts(*b.tags);
            if(targets.empty())targets.push_back({b.box.cx,b.box.cz});
            double offset=0;
            if(chooseRetailPortal(p,targets,[&](size_t e){return !party.isParty(int(owner),b.ring[e],b.ring[(e+1)%b.ring.size()]);},&offset)) {
                // A mapped entrance that faced a wall gave way to a clearer
                // door elsewhere: the manifest must not call that one measured.
                if(offset>1.5)p.entranceSource="inferred";
                // The approach meets the sampled terrain four metres outside.
                p.approach=p.floor-.08;
                interior=p;out.interiors.push_back(p);
                if(retailInterior(p.recipe)) {
                    shop = buildShopfront(p);
                    shopfronts.add(std::vector<MeshPart>(shop));
                }
            }
        }
        if(!interior&&!interiorRecipe(*b.tags).empty())out.interiorUnavailable.push_back({
            {"id",b.id},{"reason",raised?"raised-building":yEave-b.interiorFloor<2.2?"insufficient-headroom":"no-exposed-door-edge"}});
        // Render from this single plan: no second portal choice, gabarit draw,
        // or isolated party-wall index may change a building between levels.
        auto render = [&](MeshBook& walls, MeshBook& foundations, MeshBook& roofs, MeshBook& ruralTrim,
                          Mesh& trim, Mesh& glass, bool detailed, BuildingLod geometryLod,
                          double slabThickness, bool countStats) {
        Mesh& wallMesh = walls.mesh(wallSwatch);
        Mesh& roofMesh = roofs.mesh(roofSwatch);
        const size_t count = b.ring.size();
        const bool simpleFlat=g.roofShape=="flat" && geometryLod==BuildingLod::SimpleRoofline;
        const double shellHeight=g.wallHeight+(simpleFlat?g.parapetHeight:0);
        for (size_t e = 0; e < count; ++e) {
            const P2 p0 = b.ring[e], p1 = b.ring[(e + 1) % count];
            auto frame = wallFrame(p0, p1, b.ground);
            if (!frame) continue;
            const bool unifiedBase = geometryLod != BuildingLod::Full && !detailed;
            const double base = !raised && unifiedBase ? b.foundation - b.ground : 0.0;
            auto foundation=[&]() {
                // The approach slab crosses the facade at floor level. Keep
                // its portal free below the threshold too, so the character
                // capsule cannot catch the plinth while climbing the ramp.
                if(interior&&e==interior->edge) {
                    const double middle=dist(p0,interior->door),half=interior->width/2+.4;
                    face(foundations.mesh(wallSwatch),*frame,0.,b.foundation-b.ground,std::max(0.,middle-half),0.);
                    face(foundations.mesh(wallSwatch),*frame,std::min(frame->length,middle+half),b.foundation-b.ground,frame->length,0.);
                } else face(foundations.mesh(wallSwatch),*frame,0.,b.foundation-b.ground,frame->length,0.);
            };
            if(interior&&retailInterior(interior->recipe)) {
                foundation();
                const double low=interior->ceiling-b.ground;
                // Above the shop, the storeys the building has, windows and all.
                const double bays=std::max(1.0,double(pyround(frame->length/profile.bayWidth)));
                const std::pair<double,double> scale{bays/frame->length,g.storeys/std::max(1e-3,g.wallHeight)};
                if(g.wallHeight>low)face(wallMesh,*frame,0.,low,frame->length,g.wallHeight,0.,&scale);
                continue;
            }
            if(interior&&e==interior->edge) {
                // Keep the regional facade; cut only its actual doorway.
                if(!unifiedBase)foundation();
                const double middle=dist(p0,interior->door),half=interior->width/2;
                const double threshold=interior->floor-b.ground;
                const std::pair<double,double> scale{1./profile.bayWidth,1./profile.storeyHeight};
                // Eight welded corners cover a facade with a rectangular
                // doorway. Four overlapping strips need extra T-junctions
                // and exhausted a dense Vannes tile's arena budget.
                clip::Polygon facade{{{0,base},{frame->length,base},{frame->length,shellHeight},{0,shellHeight}},
                    {{{middle-half,threshold},{middle+half,threshold},{middle+half,threshold+2.15},{middle-half,threshold+2.15}}}};
                for(auto triangle:clip::triangles(facade)) {
                    auto a=frame->point(triangle[0].x,triangle[0].y),c=frame->point(triangle[1].x,triangle[1].y),d=frame->point(triangle[2].x,triangle[2].y);
                    auto n=faceNormal(a,c,d);if(n.x*frame->nx+n.z*frame->nz<0)std::swap(triangle[1],triangle[2]);
                    UV uv[3];P3 point[3];for(int i=0;i<3;++i){point[i]=frame->point(triangle[i].x,triangle[i].y);uv[i]={triangle[i].x*scale.first,-triangle[i].y*scale.second};}
                    wallMesh.addTriangle(point[0],point[1],point[2],uv);
                }
                continue;
            }
            if (!raised && !unifiedBase)
                face(foundations.mesh(wallSwatch), *frame, 0.0, b.foundation - b.ground, frame->length, 0.0);
            const bool shared = party.isParty(int(owner), p0, p1);
            if (shared && countStats) ++stats.partyWalls;
            if (detailed && !shared && frame->length >= 2.0 && !levels.empty()) {
                Mesh& reveals=residential && residential->trim ? ruralTrim.mesh(facadeProfile.trim) : trim;
                emitFacade(wallMesh, reveals, glass, *frame, levels, shellHeight, facadeProfile, b.commercial);
            } else if (wallMaterials) {
                const double bays = std::max(1.0, double(pyround(frame->length / profile.bayWidth)));
                const std::pair<double, double> scale{bays / frame->length, g.storeys / std::max(1e-3, g.wallHeight)};
                face(wallMesh, *frame, 0.0, base, frame->length, shellHeight, 0.0, &scale);
            } else {
                face(wallMesh, *frame, 0.0, base, frame->length, shellHeight);
            }
        }
        // Pitched roofs already close the volume with their sloping panels
        // and gables. An extra eave-height membrane is invisible inside that
        // volume; streamed rooms provide their own ceiling below the attic.
        if(g.roofShape=="flat")for (const auto& t : triangulate(b.ring)) {
            const P2 pa = b.ring[size_t(t[0])], pb = b.ring[size_t(t[1])], pc = b.ring[size_t(t[2])];
            const double deck=yEave+(simpleFlat?g.parapetHeight:0);
            roofMesh.addUpTriangle({pa.x, deck, pa.y}, {pb.x, deck, pb.y}, {pc.x, deck, pc.y});
        }
        const bool worship = inferSteeple;
        if (g.roofShape == "flat") {
            if (geometryLod != BuildingLod::SimpleRoofline)
                emitParapet(wallMesh, b.ring, yEave, g.parapetHeight);
            if (worship) {
                emitSteeple(wallMesh, roofMesh, b.box, b.ground, g.wallHeight, steepleStyle(*b.tags));
                if (countStats) ++stats.steeples;
            }
            return;
        }
        if (worship) {
            emitSteeple(wallMesh, roofMesh, b.box, b.ground, g.wallHeight, steepleStyle(*b.tags));
            if (countStats) ++stats.steeples;
        }
        const double sign = slopeSign(tagOr(*b.tags, "roof:direction"));
        const double yRidge = yEave + g.roofHeight;
        auto roof=[&](const OrientedBox& box,double ridge) {
            for(auto& loop:roofLoops(box,g.roofShape,facadeProfile.eaveOverhang,yEave,ridge,sign))addSlab(roofMesh,loop,slabThickness);
            for(const auto& gable:gableWalls(box,g.roofShape,yEave,ridge,sign))wallMesh.addTriangle(gable[0],gable[1],gable[2]);
        };
        if(b.roofBoxes.empty() && homeProfile && std::abs(polygonArea(b.ring))/b.box.area()<.98)
            footprintRoof(roofMesh,wallMesh,b.ring,b.box,g.roofShape,facadeProfile.eaveOverhang,yEave,yRidge,sign,slabThickness);
        else if(b.roofBoxes.empty())roof(b.box,yRidge);
        else for(const auto& wing:b.roofBoxes)roof(wing,yEave+g.roofHeight*wing.halfV/roofPlanBox(b.box,b.roofBoxes).halfV);
        };
        const auto wallMark = walls.mark(), foundationMark = foundations.mark(), roofMark = roofs.mark(), ruralMark = ruralTrim.mark();
        const size_t trimMark = trim.indices.size(), glassMark = glass.indices.size();
        render(walls, foundations, roofs, ruralTrim, trim, glass, b.detailed, lod, roofThickness, true);

        BuildingVisual visual;
        visual.id = b.id; visual.footprint = out.footprints.size() - 1;
        visual.reducedNear = shop;
        walls.capture(visual.reducedNear, wallMark, "Walls", true, wallMaterials);
        foundations.capture(visual.reducedNear, foundationMark, "Foundations", true, nullptr);
        roofs.capture(visual.reducedNear, roofMark, "Roofs", roofThickness <= 0.0, roofMaterials);
        ruralTrim.capture(visual.reducedNear, ruralMark, "Rural opening reveals", false, nullptr);
        addOpenings(visual.reducedNear, slice(trim, trimMark), slice(glass, glassMark), profile);

        MeshBook nearWalls, nearFoundations, nearRoofs, nearRural;
        nearWalls.mode = walls.mode; nearWalls.repeat = walls.repeat; nearRoofs.mode = roofs.mode;
        Mesh nearTrim, nearGlass;
        const double nearThickness = std::max(0.15, roofThickness);
        render(nearWalls, nearFoundations, nearRoofs, nearRural, nearTrim, nearGlass, true,
               BuildingLod::Full, nearThickness, false);
        visual.levels[0] = std::move(shop);
        nearWalls.parts(visual.levels[0], "Walls", true, wallMaterials);
        nearFoundations.parts(visual.levels[0], "Foundations", true, nullptr);
        nearRoofs.parts(visual.levels[0], "Roofs", false, roofMaterials);
        nearRural.parts(visual.levels[0], "Rural opening reveals", false, nullptr);
        addOpenings(visual.levels[0], std::move(nearTrim), std::move(nearGlass), profile);

        visual.low = {1e300, 1e300, 1e300}; visual.high = {-1e300, -1e300, -1e300};
        for (const auto& part : visual.levels[0]) for (const auto& p : part.mesh.positions) {
            visual.low = {std::min(visual.low.x,p.x),std::min(visual.low.y,p.y),std::min(visual.low.z,p.z)};
            visual.high = {std::max(visual.high.x,p.x),std::max(visual.high.y,p.y),std::max(visual.high.z,p.z)};
        }

        // Mid keeps the actual polygon and roof ridge, but has no opening
        // reveals, glazing, separate foundations, slabs, parapets or steeple.
        Mesh midWalls(wallMaterials ? UvMode::Facade : UvMode::None), midRoof(roofs.mode);
        midWalls.facadeRepeat = walls.repeat;
        const double midBase = raised ? b.ground : b.foundation;
        for (size_t e = 0; e < b.ring.size(); ++e) {
            auto frame = wallFrame(b.ring[e],b.ring[(e+1)%b.ring.size()],b.ground);
            if (!frame) continue;
            const double bays = std::max(1.0,double(pyround(frame->length/profile.bayWidth)));
            const std::pair<double,double> scale{bays/frame->length,g.storeys/std::max(1e-3,g.wallHeight)};
            const double top = yEave + (g.roofShape == "flat" ? g.parapetHeight : 0.0);
            face(midWalls,*frame,0.0,midBase-b.ground,frame->length,top-b.ground,0.0,
                 wallMaterials ? &scale : nullptr);
        }
        if (g.roofShape == "flat") {
            const double top = yEave + g.parapetHeight;
            for (const auto& t : triangulate(b.ring)) {
                const auto a=b.ring[size_t(t[0])],c=b.ring[size_t(t[1])],d=b.ring[size_t(t[2])];
                midRoof.addUpTriangle({a.x,top,a.y},{c.x,top,c.y},{d.x,top,d.y});
            }
        } else {
            const double sign = slopeSign(tagOr(*b.tags,"roof:direction"));
            auto midPitch = [&](const OrientedBox& box,double ridge) {
                for (auto loop : roofLoops(box,g.roofShape,0.0,yEave,ridge,sign)) addSlab(midRoof,std::move(loop),0.0);
                for (const auto& gable : gableWalls(box,g.roofShape,yEave,ridge,sign))
                    midWalls.addTriangle(gable[0],gable[1],gable[2]);
            };
            if(b.roofBoxes.empty() && homeProfile && std::abs(polygonArea(b.ring))/b.box.area()<.98)
                footprintRoof(midRoof,midWalls,b.ring,b.box,g.roofShape,0,yEave,yEave+g.roofHeight,sign,0);
            else if (b.roofBoxes.empty()) midPitch(b.box,yEave+g.roofHeight);
            else for (const auto& wing : b.roofBoxes) midPitch(wing,yEave+g.roofHeight*wing.halfV/roofPlanBox(b.box,b.roofBoxes).halfV);
        }
        visual.levels[1].push_back({"Walls",std::move(midWalls),wallMaterials ? wallMaterials(wallSwatch,true) : flatMaterial(wallSwatch,true)});
        visual.levels[1].push_back({"Roofs",std::move(midRoof),roofMaterials ? roofMaterials(roofSwatch,true) : flatMaterial(roofSwatch,true)});

        // Far is one closed OBB: ten wall/base triangles and two on the roof.
        // Its exact vertical extrema come from Near, including roof furniture.
        Mesh farWalls, farRoof;
        P3 bottom[4], top[4];
        const double corners[4][2] = {{-1,-1},{1,-1},{1,1},{-1,1}};
        for (int i=0;i<4;++i) {
            const auto p=b.box.point(corners[i][0]*b.box.halfU,corners[i][1]*b.box.halfV);
            bottom[i]={p.x,visual.low.y,p.y}; top[i]={p.x,visual.high.y,p.y};
        }
        for (int i=0;i<4;++i) {
            const int j=(i+1)%4;
            farWalls.addQuad(bottom[i],top[i],top[j],bottom[j]);
        }
        farWalls.addQuad(bottom[0],bottom[1],bottom[2],bottom[3]);
        farRoof.addUpQuad(top[0],top[1],top[2],top[3]);
        visual.levels[2].push_back({"Far walls",std::move(farWalls),flatMaterial(wallSwatch)});
        visual.levels[2].push_back({"Far roof",std::move(farRoof),flatMaterial(roofSwatch)});
        for (auto& level : visual.levels) for (auto& part : level) part.mesh.releaseWeld();
        for (auto& part : visual.reducedNear) part.mesh.releaseWeld();
        out.visuals.push_back(std::move(visual));
    }
    for (auto& part : shopfronts.parts) out.parts.push_back(std::move(part));
    walls.parts(out.parts, "Walls", true, wallMaterials);
    foundations.parts(out.parts, "Foundations", true, nullptr);
    roofs.parts(out.parts, "Roofs", roofThickness <= 0.0, roofMaterials);
    ruralTrim.parts(out.parts,"Rural opening reveals",false,nullptr);
    if(!shelterWood.empty()) {
        out.parts.push_back({"Bus shelter timber",std::move(shelterWood),
            surfaceMaterial("Bus shelter timber",{.18,.115,.06},.88,"deck")});
        out.staticParts.push_back(out.parts.back());
    }
    if(!shelterRoof.empty()) {
        out.parts.push_back({"Bus shelter roof",std::move(shelterRoof),
            surfaceMaterial("Bus shelter roof",{.14,.15,.16},.8,"metal_seam")});
        out.staticParts.push_back(out.parts.back());
    }
    if(!shelterFrame.empty()) {
        Material frame;frame.name="Bus shelter frame";frame.color={.07,.08,.07,1};frame.roughness=.78;
        out.parts.push_back({"Bus shelter frame",std::move(shelterFrame),frame});
        out.staticParts.push_back(out.parts.back());
    }
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
    for (auto& part : canopies.parts()) {
        out.parts.push_back(std::move(part));
        out.staticParts.push_back(out.parts.back());
    }
    out.footprints.insert(out.footprints.end(), canopies.obstacles.begin(), canopies.obstacles.end());
    out.tops.insert(out.tops.end(), canopies.obstacleTops.begin(), canopies.obstacleTops.end());
    out.fuelStations = std::move(canopies.stations);
    out.lettering = std::move(canopies.lettering);
    return out;
}

}  // namespace r1
