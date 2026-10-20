#include "scatter.hpp"

#include "clip.hpp"
#include "harbours.hpp"
#include "streets.hpp"
#include "spatial.hpp"
#include "gardens.hpp"
#include "buildings.hpp"
#include "waterways.hpp"

#include <fstream>
#include <cctype>
#include <mutex>
#include <set>
#include <unordered_map>

namespace r1 {

namespace {
bool selects(const std::vector<std::pair<std::string, std::vector<std::string>>>& selectors, const Tags& tags) {
    for (const auto& [key, values] : selectors) {
        auto it = tags.find(key);
        if (it == tags.end()) continue;
        for (const auto& v : values) if (v == it->second) return true;
    }
    return false;
}

const PropKind* classifyProp(const Tags& tags) {
    for (const PropKind& k : palette().props) if (selects(k.tags, tags)) return &k;
    return nullptr;
}

nlohmann::json yawRotation(double yaw) { return {0.0, std::sin(yaw * 0.5), 0.0, std::cos(yaw * 0.5)}; }
}  // namespace

// ── props ───────────────────────────────────────────────────────────────────

Scatter planProps(const std::vector<const OsmNode*>& features, const GroundAt& ground, const RegionProfile& profile,
                  const std::vector<Segment2>& roads, int budget) {
    constexpr int64_t kSaltModel = 0x4D4F444C, kSaltYaw = 0x59415721, kSaltScale = 0x53434C45;
    constexpr double kRoadSnap = 25.0;
    std::map<std::string, std::vector<const OsmNode*>> candidates;
    for (const OsmNode* node : features) {
        const PropKind* kind = classifyProp(node->tags);
        if (!kind || kind->share <= 0.0) continue;
        candidates[kind->name].push_back(node);
    }
    for (auto& [name, group] : candidates)
        std::stable_sort(group.begin(), group.end(), [](const OsmNode* a, const OsmNode* b) { return a->id < b->id; });
    // Each kind its share first, so a numerous kind cannot starve the others;
    // the leftovers then fall through in priority order.
    std::vector<const OsmNode*> kept, overflow;
    int spare = budget;
    for (const PropKind& kind : palette().props) {
        if (kind.share <= 0.0) continue;
        const auto& mine = candidates[kind.name];
        const int ceiling = std::max(1, int(budget * kind.share));
        const int take = std::min({int(mine.size()), ceiling, spare});
        kept.insert(kept.end(), mine.begin(), mine.begin() + take);
        spare -= take;
        overflow.insert(overflow.end(), mine.begin() + take, mine.end());
    }
    const int taken = std::min(int(overflow.size()), spare);
    kept.insert(kept.end(), overflow.begin(), overflow.begin() + taken);
    Scatter out;
    int placed = 0;
    std::map<std::string, int> byKind;
    for (const OsmNode* node : kept) {
        const PropKind& kind = *classifyProp(node->tags);
        std::vector<std::string> models = kind.models;
        if (models.empty())
            for (const auto& stem : profile.treeModels) models.push_back("assets/models/external/trees_lod/" + stem + ".glb");
        if (models.empty()) continue;
        PyRandom modelRng = seeded(node->id, kSaltModel);
        const std::string& model = models[modelRng.randrange(uint32_t(models.size()))];
        const P3 p = ground(node->lon, node->lat);
        double height = kind.height;
        if (kind.jitter != 0.0) {
            PyRandom scaleRng = seeded(node->id, kSaltScale);
            height *= 1.0 + (scaleRng.random() - 0.5) * 2.0 * kind.jitter;
        }
        const ModelBounds& m = modelBounds(model);
        const double scale = height / std::max(1e-6, m.high.y - m.low.y);
        std::optional<double> yaw;
        if (kind.facesRoad) {
            double best = kRoadSnap * kRoadSnap;
            for (const auto& [start, end] : roads) {
                const double dx = end.x - start.x, dz = end.y - start.y, length = dx * dx + dz * dz;
                if (length < 1e-9) continue;
                const double t = std::max(0.0, std::min(1.0, ((p.x - start.x) * dx + (p.z - start.y) * dz) / length));
                const double px = start.x + t * dx, pz = start.y + t * dz;
                const double d = (p.x - px) * (p.x - px) + (p.z - pz) * (p.z - pz);
                if (d < best) {
                    best = d;
                    yaw = std::atan2(dx, dz);
                    if (kind.name == "bench") {
                        // The bench's length is local X; its seat faces +Z.
                        yaw = std::atan2(-dz, dx);
                        if ((px - p.x) * std::sin(*yaw) + (pz - p.z) * std::cos(*yaw) < 0.)
                            *yaw += kPi;
                    }
                }
            }
        }
        if (!yaw) { PyRandom yawRng = seeded(node->id, kSaltYaw); yaw = yawRng.random() * kTau; }
        out.nodes.push_back({{"type", "Node"}, {"name", kind.name + " " + std::to_string(node->id)}, {"enabled", true},
                             {"transform", {{"position", {p.x, p.y, p.z}}, {"rotation", yawRotation(*yaw)},
                                            {"scale", {scale, scale, scale}}}},
                             {"importedFrom", model}});
        ++placed;
        ++byKind[kind.name];
    }
    out.stats = {{"placed", placed}, {"droppedForBudget", int(overflow.size()) - taken}, {"byKind", byKind}};
    return out;
}

// ── vegetation ──────────────────────────────────────────────────────────────

namespace {
constexpr int kNatureRevision = 12;
const std::string kCardDir = "assets/models/external/nature_cards";

const char* coniferObservation(const Tags& tags) {
    if (tagOr(tags, "leaf_type") == "needleleaved") return "leaf-type";
    const auto wood = tagOr(tags, "wood");
    if (wood == "coniferous" || wood == "conifer" || wood == "needleleaved") return "wood-tag";
    // Evergreen is not sufficient: many broadleaved trees are evergreen,
    // while Larix and Taxodium are conifers that shed their needles.
    static const std::set<std::string> genera = {
        "pinus", "abies", "picea", "cedrus", "larix", "tsuga", "pseudotsuga",
        "thuja", "taxus", "cupressus", "juniperus", "chamaecyparis", "cryptomeria",
        "sequoia", "sequoiadendron", "metasequoia", "taxodium", "sciadopitys",
        "araucaria", "podocarpus", "athrotaxis", "callitris", "cephalotaxus",
        "torreya", "calocedrus", "nothotsuga", "keteleeria", "cathaya",
        "dacrydium", "dacrycarpus", "phyllocladus", "prumnopitys",
        "conifer", "coniferous", "pine", "fir", "spruce", "larch"};
    std::string word;
    for (unsigned char ch : tagOr(tags, "genus") + " " + tagOr(tags, "species") + " " + tagOr(tags, "taxon") + " ") {
        if (std::isalpha(ch)) word += char(std::tolower(ch));
        else {
            if (genera.count(word)) return "botanical-tag";
            word.clear();
        }
    }
    return nullptr;
}

// Residential land use and tightly packed buildings also occur in villages.
// Require a nearby mapped city/town as well as urban land use/density; a nearer
// village/hamlet vetoes inference. Missing settlement evidence stays ineligible.
class UrbanPlanting {
public:
    UrbanPlanting(const OsmData& osm, const Anchor& anchor) {
        auto xy = [&](P2 p) { const auto e = anchor.toEngine(p.x, p.y, 0); return P2{e.x, e.z}; };
        for (const auto& n : osm.features) {
            const auto place = tagOr(n.tags, "place");
            if (place == "city" || place == "town" || place == "village" || place == "hamlet")
                settlements_.push_back({xy({n.lon, n.lat}), place == "city" || place == "town"});
        }
        auto areas = [&](const std::vector<OsmWay>& ways) {
            for (const auto& w : ways) {
                const auto use = tagOr(w.tags, "landuse"), natural = tagOr(w.tags, "natural");
                const bool urban = use == "residential" || use == "commercial" || use == "retail" || use == "industrial";
                const bool excluded = use == "forest" || use == "orchard" || use == "farmland" ||
                    use == "farmyard" || use == "vineyard" || use == "meadow" ||
                    natural == "wood" || natural == "scrub" || natural == "heath" || natural == "grassland";
                if ((!urban && !excluded) || w.points.size() < 4) continue;
                std::vector<P2> ring;
                for (P2 p : w.points) ring.push_back(xy(p));
                areasIndex_.add(areas_.size(), boxOf(ring));
                areas_.push_back({std::move(ring), excluded});
            }
        };
        areas(osm.landcover);
        areas(osm.vegetation);
        for (const auto& b : osm.buildings) {
            const auto kind = tagOr(b.tags, "building");
            if (b.points.size() < 4 || kind == "garage" || kind == "garages" || kind == "shed" ||
                kind == "barn" || kind == "farm_auxiliary" || kind == "greenhouse" ||
                kind == "roof" || kind == "carport" || kind == "ruins") continue;
            const P2 p = xy(b.points.front());
            buildingsIndex_.add(buildings_.size(), {p.x, p.y, p.x, p.y});
            buildings_.push_back(p);
        }
    }
    bool contains(P2 p) const {
        // This is deliberately a local proof, not a guessed municipal boundary:
        // do not extend a city label arbitrarily across surrounding settlements.
        double cityDistance = 1500, ruralDistance = 1e300;
        for (const auto& settlement : settlements_) {
            const double distance = dist(p, settlement.position);
            if (settlement.city) cityDistance = std::min(cityDistance, distance);
            else ruralDistance = std::min(ruralDistance, distance);
        }
        if (cityDistance >= 1500 || ruralDistance <= cityDistance) return false;
        const Box point{p.x, p.y, p.x, p.y};
        bool mapped = false;
        for (size_t i : areasIndex_.near(point)) {
            const auto& a = areas_[i];
            if (!pointInPolygon(p, a.ring)) continue;
            if (a.excluded) return false;
            mapped = true;
        }
        if (mapped) return true;
        // Density proves an urban position only after settlement classification.
        const Box neighbourhood = point.grown(175);
        int count = 0;
        for (size_t i : buildingsIndex_.near(neighbourhood))
            if (neighbourhood.contains(buildings_[i]) && ++count >= 25) return true;
        return false;
    }
private:
    struct Settlement { P2 position; bool city; };
    std::vector<Settlement> settlements_;
    struct Area { std::vector<P2> ring; bool excluded; };
    std::vector<Area> areas_;
    std::vector<P2> buildings_;
    BoxIndex areasIndex_{128}, buildingsIndex_{64};
};

// Buildings, roads and water grown by a margin, asked "is this point in you".
class Blocked {
public:
    void polygon(const std::vector<P2>& ring, double margin) { add({ring, margin, true}); }
    void line(const std::vector<P2>& line, double margin) { add({line, margin, false}); }
    bool contains(P2 p) const {
        auto it = cells_.find(key(long(std::floor(p.x / kCell)), long(std::floor(p.y / kCell))));
        if (it == cells_.end()) return false;
        for (size_t i : it->second) {
            const Item& item = items_[i];
            if (item.closed && item.points.size() >= 3 && pointInPolygon(p, item.points)) return true;
            if (clip::distance(item.points, p) < item.margin) return true;
        }
        return false;
    }

private:
    struct Item { std::vector<P2> points; double margin; bool closed; };
    static constexpr double kCell = 16.0;
    std::vector<Item> items_;
    std::unordered_map<int64_t, std::vector<size_t>> cells_;
    static int64_t key(long x, long z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); }
    void add(Item item) {
        if (item.points.empty()) return;
        if (item.closed && item.points.front() != item.points.back()) item.points.push_back(item.points.front());
        double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
        for (const P2& p : item.points) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.y); z1 = std::max(z1, p.y); }
        const size_t index = items_.size();
        items_.push_back(std::move(item));
        const double m = items_.back().margin;
        for (long x = long(std::floor((x0 - m) / kCell)); x <= long(std::floor((x1 + m) / kCell)); ++x)
            for (long z = long(std::floor((z0 - m) / kCell)); z <= long(std::floor((z1 + m) / kCell)); ++z)
                cells_[key(x, z)].push_back(index);
    }
};

std::pair<std::string, std::string> species(const Tags& tags, double lon, double lat, bool forest) {
    if (const char* reason = coniferObservation(tags)) return {"pine_tall", reason};
    std::string genus = tagOr(tags, "genus") + " " + tagOr(tags, "species");
    for (char& c : genus) c = char(std::tolower((unsigned char)c));
    auto has = [&](const char* s) { return genus.find(s) != std::string::npos; };
    bool blank = true;
    for (char c : genus) if (!std::isspace((unsigned char)c)) blank = false;
    // Observed cherries win worldwide, independently of urban planting rates.
    // Prunus alone also includes plums, peaches and almonds. Cerasus is an
    // explicit cherry genus; do not mistake e.g. pseudocerasus for that word.
    const auto cerasus = genus.find("cerasus");
    const bool cherryGenus = cerasus != std::string::npos &&
        (cerasus == 0 || std::isspace((unsigned char)genus[cerasus - 1])) &&
        (cerasus + 7 == genus.size() || std::isspace((unsigned char)genus[cerasus + 7]));
    if (has("prunus serrulata") || has("prunus yedoensis") || has("prunus x yedoensis") ||
        has("prunus × yedoensis") || has("prunus subhirtella") || cherryGenus)
        return {"sakura", "botanical-tag"};
    if (has("aloidendron") || has("aloe dichotoma")) return {"quiver_tree", "botanical-tag"};
    if (tagOr(tags, "leaf_type") == "broadleaved" || !blank) return {"broadleaf", "generic-morphology-for-tag"};
    if (forest && (std::abs(lat) > 57 || (5 < lon && lon < 16 && 44 < lat && lat < 48))) {
        return {"pine_tall", "regional-inference"};
    }
    // Quiver trees are southern African dryland plants, not a synonym for heat.
    if (13 < lon && lon < 23 && -32 < lat && lat < -20) return {"quiver_tree", "regional-inference"};
    return {"broadleaf", "regional-inference"};
}

double sourceHeight(const std::string& model) {
    static std::mutex lock;
    static std::map<std::string, double> cache;
    std::lock_guard<std::mutex> guard(lock);
    auto it = cache.find(model);
    if (it != cache.end()) return it->second;
    const std::string path = palette().gameRoot + "/" + kCardDir + "/" + model + ".source.json";
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    return cache[model] = nlohmann::json::parse(f).at("height").get<double>();
}

struct Candidate {
    int rank;
    __int128 ident;
    double lon, lat;
    Tags tags;
    const char* source;
    bool forest;
    double order;
};

std::string identString(__int128 v) {
    if (v == 0) return "0";
    const bool negative = v < 0;
    unsigned __int128 u = negative ? (unsigned __int128)(-v) : (unsigned __int128)v;
    std::string s;
    while (u) { s += char('0' + int(u % 10)); u /= 10; }
    if (negative) s += '-';
    return std::string(s.rbegin(), s.rend());
}
}  // namespace

Scatter planNature(const OsmData& osm, const Tile& tile, const Anchor& anchor, const GroundAt& ground, int budget,
                   const Canopy* canopy, const std::vector<OsmWay>* roadside,
                   const clip::Paths64* driveways) {
    const Bounds bounds = tile.bounds();
    const UrbanPlanting urbanPlanting(osm, anchor);
    auto xy = [&](double lo, double la) { const P3 p = anchor.toEngine(lo, la, 0); return P2{p.x, p.z}; };
    const P2 sw = xy(bounds.west, bounds.south), ne = xy(bounds.east, bounds.north);
    const double rx0 = std::min(sw.x, ne.x), rz0 = std::min(sw.y, ne.y), rx1 = std::max(sw.x, ne.x), rz1 = std::max(sw.y, ne.y);
    auto inRegion = [&](P2 p) { return rx0 < p.x && p.x < rx1 && rz0 < p.y && p.y < rz1; };
    Blocked blocked;
    const auto relationWater=inlandWaterRegion({},anchor,false,osm.waterAreas);
    if(driveways)for(const auto& p:*driveways)blocked.polygon(clip::kMetres.ring(p),.3);
    auto engineLine = [&](const std::vector<P2>& pts) { std::vector<P2> out; for (const P2& p : pts) out.push_back(xy(p.x, p.y)); return out; };
    struct ConiferForest { std::vector<P2> ring; const char* reason; };
    std::vector<ConiferForest> coniferForests;
    BoxIndex coniferIndex(128);
    std::set<int64_t> seenForests;
    for (const auto* ways : {&osm.vegetation, &osm.landcover}) for (const auto& way : *ways) {
        const bool forest = tagOr(way.tags, "natural") == "wood" || tagOr(way.tags, "landuse") == "forest";
        if (!forest || way.points.size() < 4 || !seenForests.insert(way.id).second) continue;
        const char* reason = coniferObservation(way.tags) ? "conifer-forest-tag" : nullptr;
        const auto observed = palette().treeAreaObservations.find(way.id);
        const bool identified = !tagOr(way.tags,"leaf_type").empty() || !tagOr(way.tags,"genus").empty() ||
            !tagOr(way.tags,"species").empty() || !tagOr(way.tags,"taxon").empty() || !tagOr(way.tags,"wood").empty();
        if (!reason && !identified && observed != palette().treeAreaObservations.end() &&
            coniferObservation(observed->second.identity)) reason = "atlas-conifer-observation";
        if (!reason) continue;
        auto ring = engineLine(way.points);
        coniferIndex.add(coniferForests.size(), boxOf(ring));
        coniferForests.push_back({std::move(ring), reason});
    }
    for (const auto& way : osm.buildings) if (way.points.size() >= 4) blocked.polygon(engineLine(way.points), 0.6);
    for (const auto& way : roadside ? *roadside : osm.roads) {
        if (taggedYes(way.tags, "bridge") || taggedYes(way.tags, "tunnel")) continue;
        const auto sidewalks = sidewalkSides(way.tags);
        double clearance = .7;
        for (const auto& side : sidewalks)
            clearance = std::max(clearance, lengthTag(tag(way.tags, ("sidewalk:"+side.first+":width").c_str()),
                                lengthTag(tag(way.tags,"sidewalk:width"),1.8)) + .5);
        blocked.line(engineLine(way.points), roadWidth(way.tags) / 2 + clearance);
    }
    for (const auto& way : osm.landcover)
        if (tagOr(way.tags, "natural") == "water" || has(way.tags, "water") || tagOr(way.tags, "leisure") == "pitch")
            blocked.polygon(engineLine(way.points), 0.5);

    std::vector<Candidate> candidates;
    for (const OsmNode& f : osm.features)
        if (tagOr(f.tags, "natural") == "tree" && tileAt(f.lon, f.lat) == tile)
            candidates.push_back({0, f.id, f.lon, f.lat, f.tags, "osm-point", false, 0});
    // Rows are mapped lines; spacing is inferred unless a count is tagged.
    for (const OsmWay& way : osm.treeRows) {
        const std::vector<P2> line = engineLine(way.points);
        const double length = clip::length(line);
        if (length < 1) continue;
        const int count = std::max(1, std::min(2000, int(lengthTag(tag(way.tags, "tree_count"), length / 8))));
        for (int i = 0; i < count; ++i) {
            const P2 p = clip::interpolate(line, (i + 0.5) * length / count);
            const P3 geo = anchor.toGeodetic(p.x, 0, p.y);
            if (tileAt(geo.x, geo.y) == tile)
                candidates.push_back({1, -(__int128(std::llabs(way.id)) * 4096 + i), geo.x, geo.y, way.tags,
                                      "osm-row-inferred-spacing", false, 0});
        }
    }
    // In temperate countryside, roadside trees are common even where OSM has
    // no individual trees or tree row. Keep trunks beyond the shoulder and
    // leave village streets, buildings and surveyed planting to the map.
    const std::string climate = climateAt(profileFor(tile.center().x, tile.center().y).climate, tile.center().y);
    if (climate == "temperate" || climate == "mediterranean") {
        struct BuildingEdge { std::vector<P2> ring; double x0, x1, z0, z1; };
        std::vector<BuildingEdge> buildingRings;
        for (const OsmWay& building : osm.buildings) {
            if (building.points.size() < 3) continue;
            auto ring = engineLine(building.points);
            BuildingEdge edge{std::move(ring), 1e300, -1e300, 1e300, -1e300};
            for (const P2& p : edge.ring) {
                edge.x0 = std::min(edge.x0, p.x); edge.x1 = std::max(edge.x1, p.x);
                edge.z0 = std::min(edge.z0, p.y); edge.z1 = std::max(edge.z1, p.y);
            }
            buildingRings.push_back(std::move(edge));
        }
        for (const OsmWay& road : osm.roads) {
            const std::string highway = tagOr(road.tags, "highway");
            if (highway != "secondary" && highway != "tertiary" && highway != "unclassified" && highway != "residential") continue;
            if (taggedYes(road.tags, "bridge") || taggedYes(road.tags, "tunnel") || has(road.tags, "r1:raised")) continue;
            const std::vector<P2> line = engineLine(road.points);
            const double shoulder = roadWidth(road.tags) * 0.5 + 4.5;
            for (size_t segment = 0; segment + 1 < line.size(); ++segment) {
                const P2 a = line[segment], b = line[segment + 1];
                const double length = dist(a, b);
                if (length < 8.0) continue;
                const P2 normal{-(b.y - a.y) / length, (b.x - a.x) / length};
                for (int step = 0; (step + 0.5) * 22.0 < length; ++step) {
                    const double t = (step + 0.5) * 22.0 / length;
                    for (int side : {-1, 1}) {
                        const __int128 ident = -(__int128(std::llabs(road.id)) * 1000003 +
                                                 __int128(segment) * 4099 + __int128(step) * 2 + (side + 1) / 2);
                        PyRandom rng = seeded(ident, 0x524F4144);
                        if (rng.random() < 0.18) continue;  // gaps, not an avenue grid
                        const double offset = shoulder + rng.random() * 2.0;
                        const P2 p{a.x + (b.x - a.x) * t + normal.x * side * offset,
                                   a.y + (b.y - a.y) * t + normal.y * side * offset};
                        if (!inRegion(p) || blocked.contains(p)) continue;
                        bool settled = false;
                        for (const auto& building : buildingRings) {
                            if (p.x < building.x0 - 35.0 || p.x > building.x1 + 35.0 ||
                                p.y < building.z0 - 35.0 || p.y > building.z1 + 35.0) continue;
                            if (pointInPolygon(p, building.ring) || clip::distance(building.ring, p) < 35.0) {
                                settled = true; break;
                            }
                        }
                        if (settled) continue;
                        const P3 geo = anchor.toGeodetic(p.x, 0, p.y);
                        if (tileAt(geo.x, geo.y) != tile) continue;
                        candidates.push_back({2, ident, geo.x, geo.y, {}, "inferred-rural-roadside", false, 0});
                    }
                }
            }
        }
    }
    for (const OsmWay& way : osm.vegetation) {  // already sorted by id
        const Tags& t = way.tags;
        const bool forest = tagOr(t, "natural") == "wood" || tagOr(t, "landuse") == "forest";
        const bool park = tagOr(t, "leisure") == "park" || tagOr(t, "leisure") == "garden";
        const bool orchard = tagOr(t, "landuse") == "orchard";
        const bool herb = tagOr(t, "landuse") == "grass" || tagOr(t, "landuse") == "meadow" ||
                          tagOr(t, "natural") == "grassland" || tagOr(t, "natural") == "wetland";
        if (!(forest || park || orchard || herb)) continue;
        const std::vector<P2> ring = engineLine(way.points);
        double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
        for (const P2& p : ring) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.y); z1 = std::max(z1, p.y); }
        x0 = std::max(x0, rx0); x1 = std::min(x1, rx1); z0 = std::max(z0, rz0); z1 = std::min(z1, rz1);
        if (x0 >= x1 || z0 >= z1) continue;
        const double spacing = herb ? 7 : forest ? 12 : park ? 24 : 9;
        // A deterministic stratified distribution, not a planted grid.
        for (long ix = long(std::floor(x0 / spacing)); ix < long(std::ceil(x1 / spacing)); ++ix)
            for (long iz = long(std::floor(z0 / spacing)); iz < long(std::ceil(z1 / spacing)); ++iz) {
                const __int128 ident = -(__int128(std::llabs(way.id)) * 1000003 + __int128(ix) * 73856093 + __int128(iz) * 19349663);
                PyRandom rng = seeded(ident, 0x4E4154);
                const double x = (ix + 0.2 + rng.random() * 0.6) * spacing;
                const double z = (iz + 0.2 + rng.random() * 0.6) * spacing;
                const P2 p{x, z};
                if (!inRegion(p) || !pointInPolygon(p, ring) || blocked.contains(p)) continue;
                const P3 geo = anchor.toGeodetic(x, 0, z);
                if (tileAt(geo.x, geo.y) != tile) continue;
                Tags inferred = t;
                if (herb) inferred["r1:grass"] = "yes";
                candidates.push_back({herb ? 4 : 3, ident, geo.x, geo.y, std::move(inferred), "osm-area-inferred-density", forest, 0});
            }
    }
    // Inferred planting follows measured tree cover; surveyed points and rows stay.
    int canopyRemoved = 0, canopyTrees = 0;
    if (canopy) {
        std::vector<Candidate> kept;
        std::vector<uint8_t> taken(size_t(Canopy::kCells * Canopy::kCells), 0);
        for (Candidate& c : candidates) {
            const bool grass = tagOr(c.tags, "r1:grass") == "yes";
            const Canopy::Class here = canopy->classAt(c.lon, c.lat);
            if (c.rank >= 2 && !grass && here != Canopy::Tree) { ++canopyRemoved; continue; }
            const int cell = canopy->cellOf(c.lon, c.lat);
            if (cell >= 0 && !grass) taken[size_t(cell)] = 1;
            kept.push_back(std::move(c));
        }
        candidates = std::move(kept);
        const P2 size = canopy->size();
        for (int row = 0; row < Canopy::kCells; ++row)
            for (int col = 0; col < Canopy::kCells; ++col) {
                const Canopy::Class kind = canopy->at(col, row);
                if (kind != Canopy::Tree || taken[size_t(row * Canopy::kCells + col)]) continue;
                const __int128 ident = -((__int128(0x43414E4F) << 64) + __int128(tile.row) * 100003 * 4096 +
                                         __int128(tile.col) * 4096 + row * Canopy::kCells + col);
                // A trunk within the cell, clear of roads and roofs; the crown
                // may overhang them, the trunk may not.
                PyRandom rng = seeded(ident, 0x43414E);
                const P2 middle = canopy->centre(col, row);
                std::optional<P2> spot;
                for (int attempt = 0; attempt < 5 && !spot; ++attempt) {
                    const P2 g{middle.x + (rng.random() - 0.5) * 0.8 * size.x, middle.y + (rng.random() - 0.5) * 0.8 * size.y};
                    const P2 p = xy(g.x, g.y);
                    if (inRegion(p) && !blocked.contains(p)) spot = g;
                }
                if (!spot) continue;
                Tags tags;
                int around = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int r = row + dy, cc = col + dx;
                        if ((dx || dy) && r >= 0 && cc >= 0 && r < Canopy::kCells && cc < Canopy::kCells) around += canopy->at(cc, r) == Canopy::Tree;
                    }
                const double h = canopy->heightAt(col, row) * (0.85 + rng.random() * 0.25);
                tags["height"] = std::to_string(std::max(3.0, h));
                candidates.push_back({1, ident, spot->x, spot->y, std::move(tags), "canopy-measured", around >= 6, 0});
                ++canopyTrees;
            }
    }
    // Surveyed trees always win over inferred fill; the hash spreads a capped
    // population over the tile instead of one corner of a forest.
    for (Candidate& c : candidates) { PyRandom r = seeded(c.ident, 99); c.order = r.random(); }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.order != b.order) return a.order < b.order;
        return a.ident < b.ident;
    });
    Scatter out;
    std::unordered_map<int64_t, std::vector<P2>> occupied;
    std::map<std::string, int> sources, models, reasons;
    int rejected = 0, budgetDropped = 0, trees = 0, grasses = 0, heights = 0;
    int urbanCherryEligible = 0, urbanCherrySelected = 0;
    auto cellKey = [](long x, long z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); };
    for (const Candidate& c : candidates) {
        const P2 p = xy(c.lon, c.lat);
        const double spacing=3.0;
        const long kx = long(std::floor(p.x / 3)), kz = long(std::floor(p.y / 3));
        bool crowded = false;
        for (long dx = -1; dx <= 1 && !crowded; ++dx)
            for (long dz = -1; dz <= 1 && !crowded; ++dz) {
                auto it = occupied.find(cellKey(kx + dx, kz + dz));
                if (it != occupied.end())
                    for (const P2& q : it->second) if (std::hypot(p.x - q.x, p.y - q.y) < spacing) { crowded = true; break; }
            }
        if (crowded) { ++rejected; continue; }
        // Roads exclude inferred planting; a surveyed tree may stand on a median.
        if (c.rank && (blocked.contains(p) || clip::contains(relationWater,p))) { ++rejected; continue; }
        const bool grass = tagOr(c.tags, "r1:grass") == "yes";
        if (grass ? grasses >= 160 : trees >= budget) { ++budgetDropped; continue; }
        occupied[cellKey(kx, kz)].push_back(p);
        const char* coniferForest = nullptr;
        if (!grass) for (size_t i : coniferIndex.near({p.x, p.y, p.x, p.y}))
            if (pointInPolygon(p, coniferForests[i].ring)) { coniferForest = coniferForests[i].reason; break; }
        auto [model, reason] = coniferForest
            ? std::make_pair(std::string("pine_tall"), std::string(coniferForest))
            : species(c.tags, c.lon, c.lat, c.forest);
        // Only infer a species when OSM does not supply botanical identity.
        // Japan's 15% replaces the northern 5%; the two rates never add up.
        const bool botanical = std::any_of(c.tags.begin(), c.tags.end(), [](const auto& t) {
            return (t.first == "genus" || t.first == "species") &&
                std::any_of(t.second.begin(), t.second.end(), [](unsigned char ch) { return !std::isspace(ch); });
        });
        const double cherryRate = c.lat > 0 ? (osm.country == "JP" ? 0.15 : 0.05) : 0.0;
        if (!grass && !coniferForest && !coniferObservation(c.tags) && !botanical &&
            cherryRate > 0 && std::string(c.source) != "inferred-rural-roadside" && urbanPlanting.contains(p)) {
            ++urbanCherryEligible;
            PyRandom cherryRng = seeded(c.ident, 0x53414B55);
            if (cherryRng.random() < cherryRate) {
                ++urbanCherrySelected;
                model = "sakura";
                reason = osm.country == "JP" ? "urban-cherry-japan-inference" : "urban-cherry-north-inference";
            }
        }
        if (model == "broadleaf" && std::abs(c.lat) >= 30) model = "urban_tree";
        if (grass) {
            const double lo = c.lon, la = c.lat;
            const bool arid = (-18 < lo && lo < 60 && 18 < la && la < 32) || (13 < lo && lo < 23 && -32 < la && la < -20) ||
                              (115 < lo && lo < 140 && -30 < la && la < -20);
            model = tagOr(c.tags, "natural") == "wetland" ? "grass_tall" : arid ? "grass_dry" : "grass_fresh";
        }
        PyRandom rng = seeded(c.ident, 81);
        const double nominal = grass ? (model == "grass_tall" ? 1.1 : 0.45)
                               : model == "pine_tall" ? (c.forest || coniferForest ? 22 : 18)
                               : model == "sakura" ? 6 : c.forest ? 13 : std::abs(c.lat) < 30 ? 10 : 9;
        const double observed = lengthTag(tag(c.tags, "height"), 0);
        const double height = observed > 0 ? observed : nominal * (0.8 + rng.random() * 0.4);
        const double width = height;
        heights += observed > 0;
        ++reasons[reason];
        if (grass) ++grasses; else ++trees;
        nlohmann::json children = nlohmann::json::array();
        if (grass) {
            children.push_back({{"type", "Node"}, {"name", "Near"}, {"importedFrom", "assets/models/external/nature_selected/" + model + ".glb"}});
        } else {
            const bool detailed = model == "urban_tree" || model == "sakura" || model == "pine_tall";
            if(detailed)
                children.push_back({{"type", "Node"}, {"name", "Far"},
                                    {"importedFrom", "assets/models/external/nature_selected/" + model + "_far.glb"}});
            else {
                const double k = 1.0 / sourceHeight(model);
                children.push_back({{"type", "Node"}, {"name", "Far"}, {"importedFrom", kCardDir + "/" + model + ".glb"},
                                    {"transform", {{"scale", {k, k, k}}}}});
            }
            // The middle level is the same tree, thinned (tools/r1/tree_lod.py).
            if (detailed) {
                children.push_back({{"type", "Node"}, {"name", "Near"}, {"importedFrom", "assets/models/external/nature_selected/" + model + ".glb"}});
                children.push_back({{"type", "Node"}, {"name", "Mid"}, {"importedFrom", "assets/models/external/nature_selected/" + model + "_mid.glb"}});
            }
        }
        PyRandom yawRng = seeded(c.ident, 71);
        const double yaw = yawRng.random() * kTau;
        const P3 at = ground(c.lon, c.lat);
        const double verticalPlacement = !grass && model != "sakura" ? -0.10 : 0.0;
        out.nodes.push_back({{"type", "Node"}, {"name", std::string("Nature ") + c.source + " " + identString(c.ident)},
                             {"enabled", true}, {"groups", {"vegetation", grass ? "grass" : "tree"}}, {"children", children},
                             {"transform", {{"position", {at.x, at.y + verticalPlacement, at.z}}, {"rotation", {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}},
                                            {"scale", {width, height, width}}}}});
        ++sources[c.source];
        ++models[model];
    }
    out.stats = {{"revision", kNatureRevision}, {"placed", out.nodes.size()}, {"trees", trees}, {"grassTufts", grasses},
                 {"heightsMeasured", heights}, {"modelSelection", reasons}, {"bySource", sources}, {"byModel", models},
                 {"rejectedOverlap", rejected}, {"droppedForBudget", budgetDropped}, {"budget", budget},
                 {"representation", "urban, sakura and adult pine trees: Near/Mid/multi-view Far; other species: layered source cards"},
                 {"urbanCherryPlanting", {{"eligible", urbanCherryEligible}, {"selected", urbanCherrySelected},
                                         {"japanRate", 0.15}, {"northRate", 0.05},
                                         {"settlementPolicy", "nearest mapped city/town within 1500m plus local urban landuse/density; village/hamlet and unknown excluded"}}},
                 {"canopy", canopy ? nlohmann::json{{"observed", true}, {"noSource", canopy->noSource},
                                                    {"treeCells", canopy->count(Canopy::Tree)},
                                                    {"treesFromCanopy", canopyTrees},
                                                    {"inferredRemovedByCanopy", canopyRemoved},
                                                    {"source", "Meta & WRI High Resolution Canopy Height Maps, CC BY 4.0 (imagery 2009-2020)"}}
                                   : nlohmann::json{{"observed", false}}},
                 {"speciesPolicy", "conifer forest areas force adult pine; botanical/leaf tags; untyped urban cherries 15% JP or 5% north; regional morphology"}};
    return out;
}

}  // namespace r1
