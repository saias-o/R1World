#include "landmarks.hpp"

#include "palette.hpp"

#include <fstream>
#include <mutex>

namespace r1 {

namespace {
struct Catalogue { std::vector<Landmark> list; double range = 5000; };

const Catalogue& catalogue() {
    static std::once_flag once;
    static Catalogue c;
    std::call_once(once, [] {
        const std::string path = palette().gameRoot + "/assets/world/landmarks/landmarks.json";
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot read " + path);
        const auto doc = nlohmann::json::parse(f);
        if (doc.value("revision", 0) != kLandmarkRevision)
            throw std::runtime_error("landmarks.json is revision " + std::to_string(doc.value("revision", 0)) +
                                     ", the game expects " + std::to_string(kLandmarkRevision));
        c.range = doc.at("range").get<double>();
        auto rings = [](const nlohmann::json& j) {
            std::vector<Ring> out;
            for (const auto& r : j) {
                Ring ring;
                for (const auto& p : r) ring.push_back({p[0].get<double>(), p[1].get<double>()});
                out.push_back(std::move(ring));
            }
            return out;
        };
        for (const auto& e : doc.at("landmarks")) {
            Landmark l;
            l.slug = e.at("slug"); l.name = e.at("name"); l.wikidata = e.at("wikidata"); l.osm = e.at("osm");
            l.bearingSource = e.at("bearingSource");
            l.lon = e.at("lon"); l.lat = e.at("lat"); l.bearing = e.at("bearing"); l.height = e.at("height");
            l.groundAlt = e.at("alt"); l.groundSource = e.at("altSource");
            l.clearance = rings(e.at("clearance"));
            l.solids = rings(e.at("solids"));
            for (const auto& level : e.at("levels"))
                l.levels.push_back({level.at("path"), level.at("until"), level.at("vertices")});
            c.list.push_back(std::move(l));
        }
    });
    return c;
}

// A recipe-frame (x, z) point in the tile's engine frame (Sculpt.parts).
P2 turn(double bearing, P2 point, P3 origin) {
    const double b = radians(bearing);
    return {origin.x + point.x * std::sin(b) + point.y * std::cos(b), origin.z - point.x * std::cos(b) + point.y * std::sin(b)};
}

// The widest clearance of any landmark (Khufu's half-diagonal, 165 m), plus a
// margin: how far from a tile a landmark can reach into it.
constexpr double kReach = 250.0;

std::vector<const Landmark*> around(const Bounds& b) {
    const double lon = (b.west + b.east) * 0.5, lat = (b.south + b.north) * 0.5;
    const double half = std::hypot((b.east - b.west) * 111320.0 * std::cos(radians(lat)), (b.north - b.south) * 110540.0) * 0.5;
    std::vector<const Landmark*> out;
    for (const Landmark& l : landmarks()) {
        const double dx = (l.lon - lon) * 111320.0 * std::cos(radians(lat)), dy = (l.lat - lat) * 110540.0;
        if (std::hypot(dx, dy) <= half + kReach) out.push_back(&l);
    }
    return out;
}
}  // namespace

const std::vector<Landmark>& landmarks() { return catalogue().list; }
double landmarkFarRange() { return catalogue().range; }

LandmarkPlacement placeLandmarks(const Bounds& bounds, const std::function<P3(double, double)>& ground,
                                 const std::vector<const OsmWay*>& buildings) {
    LandmarkPlacement out;
    const auto candidates = around(bounds);
    if (candidates.empty()) { out.kept = buildings; return out; }
    for (const OsmWay* way : buildings) {
        bool replaced = false;
        const std::string qid = tagOr(way->tags, "wikidata");
        if (!qid.empty()) for (const Landmark* l : candidates) replaced |= l->wikidata == qid;
        if (!replaced) {
            // A building centred inside a landmark's clearance is its trace.
            std::vector<P2> points = way->points;
            if (points.front() == points.back()) points.pop_back();
            double lon = 0, lat = 0;
            for (const P2& p : points) { lon += p.x; lat += p.y; }
            const P3 centre = ground(lon / points.size(), lat / points.size());
            for (const Landmark* l : candidates) {
                const P3 origin = ground(l->lon, l->lat);
                for (const Ring& ring : l->clearance) {
                    Ring turned;
                    for (const P2& p : ring) turned.push_back(turn(l->bearing, p, origin));
                    replaced |= pointInPolygon({centre.x, centre.z}, turned);
                }
            }
        }
        if (replaced) out.replaced.push_back(way->id);
        else out.kept.push_back(way);
    }
    std::vector<nlohmann::json> manifest;
    for (const Landmark* l : candidates) {
        if (!(bounds.west <= l->lon && l->lon < bounds.east && bounds.south <= l->lat && l->lat < bounds.north)) continue;
        const P3 at = ground(l->lon, l->lat);
        out.nodes.push_back({{"type", "Node"}, {"name", "landmark " + l->slug}, {"enabled", true}, {"groups", {"landmark"}},
                             {"transform", {{"position", {at.x, at.y, at.z}}, {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                            {"scale", {1.0, 1.0, 1.0}}}},
                             {"importedFrom", l->levels.at(0).path}});
        for (const Ring& ring : l->solids) {
            Ring turned;
            for (const P2& p : ring) turned.push_back(turn(l->bearing, p, at));
            out.solids.push_back(std::move(turned));
            out.solidTops.push_back(at.y + l->height);
        }
        out.vertices += l->levels.at(0).vertices;
        manifest.push_back({{"slug", l->slug}, {"name", l->name}, {"wikidata", l->wikidata},
                            {"anchor", {{"source", "osm:" + l->osm}, {"lon", l->lon}, {"lat", l->lat}}},
                            {"bearing", {{"source", l->bearingSource == "osm" ? "osm:" + l->osm : l->bearingSource},
                                         {"degrees", l->bearing}}},
                            {"height", {{"source", "official"}, {"metres", l->height}}},
                            {"shape", {{"source", "recipe"}, {"revision", kLandmarkRevision}}},
                            {"vertices", l->levels.at(0).vertices}});
    }
    std::sort(manifest.begin(), manifest.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a["slug"].get<std::string>() < b["slug"].get<std::string>();
    });
    for (auto& m : manifest) out.manifest.push_back(std::move(m));
    return out;
}

}  // namespace r1
