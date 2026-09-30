#include "scatter.hpp"

#include "clip.hpp"
#include "harbours.hpp"
#include "streets.hpp"

#include <fstream>
#include <mutex>
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
                if (d < best) { best = d; yaw = std::atan2(dx, dz); }
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
constexpr int kNatureRevision = 3;
const std::string kCardDir = "assets/models/external/nature_cards";

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

std::pair<std::string, std::string> species(const Tags& tags, double lon, double lat, __int128 identifier, bool forest) {
    std::string genus = tagOr(tags, "genus") + " " + tagOr(tags, "species");
    for (char& c : genus) c = char(std::tolower((unsigned char)c));
    auto has = [&](const char* s) { return genus.find(s) != std::string::npos; };
    bool blank = true;
    for (char c : genus) if (!std::isspace((unsigned char)c)) blank = false;
    if (has("pinus")) return {"pine_sapling", "botanical-tag"};
    if (has("abies") || has("picea") || has("cedrus")) return {"fir_sapling", "botanical-tag"};
    if (has("aloidendron") || has("aloe dichotoma")) return {"quiver_tree", "botanical-tag"};
    if (tagOr(tags, "leaf_type") == "needleleaved") return {"pine_sapling", "leaf-type"};
    if (tagOr(tags, "leaf_type") == "broadleaved" || !blank) return {"broadleaf", "generic-morphology-for-tag"};
    if (forest && (std::abs(lat) > 57 || (5 < lon && lon < 16 && 44 < lat && lat < 48))) {
        PyRandom rng = seeded(identifier, 17);
        return {rng.randrange(2) == 0 ? "fir_sapling" : "pine_sapling", "regional-inference"};
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
                   const Canopy* canopy) {
    const Bounds bounds = tile.bounds();
    auto xy = [&](double lo, double la) { const P3 p = anchor.toEngine(lo, la, 0); return P2{p.x, p.z}; };
    const P2 sw = xy(bounds.west, bounds.south), ne = xy(bounds.east, bounds.north);
    const double rx0 = std::min(sw.x, ne.x), rz0 = std::min(sw.y, ne.y), rx1 = std::max(sw.x, ne.x), rz1 = std::max(sw.y, ne.y);
    auto inRegion = [&](P2 p) { return rx0 < p.x && p.x < rx1 && rz0 < p.y && p.y < rz1; };
    Blocked blocked;
    auto engineLine = [&](const std::vector<P2>& pts) { std::vector<P2> out; for (const P2& p : pts) out.push_back(xy(p.x, p.y)); return out; };
    for (const auto& way : osm.buildings) if (way.points.size() >= 4) blocked.polygon(engineLine(way.points), 0.6);
    for (const auto& way : osm.roads) {
        if (taggedYes(way.tags, "bridge") || taggedYes(way.tags, "tunnel")) continue;
        blocked.line(engineLine(way.points), roadWidth(way.tags) / 2 + 0.7);
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
        const bool scrub = tagOr(t, "natural") == "scrub" || tagOr(t, "natural") == "shrubbery";
        const bool park = tagOr(t, "leisure") == "park" || tagOr(t, "leisure") == "garden";
        const bool orchard = tagOr(t, "landuse") == "orchard";
        const bool herb = tagOr(t, "landuse") == "grass" || tagOr(t, "landuse") == "meadow" ||
                          tagOr(t, "natural") == "grassland" || tagOr(t, "natural") == "wetland";
        if (!(forest || scrub || park || orchard || herb)) continue;
        const std::vector<P2> ring = engineLine(way.points);
        double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
        for (const P2& p : ring) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.y); z1 = std::max(z1, p.y); }
        x0 = std::max(x0, rx0); x1 = std::min(x1, rx1); z0 = std::max(z0, rz0); z1 = std::min(z1, rz1);
        if (x0 >= x1 || z0 >= z1) continue;
        const double spacing = herb ? 7 : forest ? 12 : scrub ? 8 : park ? 24 : 9;
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
                if (scrub) inferred["r1:shrub"] = "yes";
                if (herb) inferred["r1:grass"] = "yes";
                candidates.push_back({herb ? 4 : 3, ident, geo.x, geo.y, std::move(inferred), "osm-area-inferred-density", forest, 0});
            }
    }
    // The measured canopy: what was inferred must stand where it says trees
    // (or, for scrub, vegetation) stand; each tree cell with nothing on it
    // gets a tree, each low cell a shrub. Surveyed points and rows stay.
    int canopyRemoved = 0, canopyTrees = 0, canopyShrubs = 0;
    if (canopy) {
        std::vector<Candidate> kept;
        std::vector<uint8_t> taken(size_t(Canopy::kCells * Canopy::kCells), 0);
        for (Candidate& c : candidates) {
            const bool grass = tagOr(c.tags, "r1:grass") == "yes", shrub = has(c.tags, "r1:shrub");
            const Canopy::Class here = canopy->classAt(c.lon, c.lat);
            if (c.rank >= 2 && !grass && !(here == Canopy::Tree || (shrub && here == Canopy::Low))) { ++canopyRemoved; continue; }
            const int cell = canopy->cellOf(c.lon, c.lat);
            if (cell >= 0 && !grass) taken[size_t(cell)] = 1;
            kept.push_back(std::move(c));
        }
        candidates = std::move(kept);
        const P2 size = canopy->size();
        for (int row = 0; row < Canopy::kCells; ++row)
            for (int col = 0; col < Canopy::kCells; ++col) {
                const Canopy::Class kind = canopy->at(col, row);
                if (kind == Canopy::None || taken[size_t(row * Canopy::kCells + col)]) continue;
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
                if (kind == Canopy::Tree) {
                    const double h = canopy->heightAt(col, row) * (0.85 + rng.random() * 0.25);
                    tags["height"] = std::to_string(std::max(3.0, h));
                    candidates.push_back({1, ident, spot->x, spot->y, std::move(tags), "canopy-measured", around >= 6, 0});
                    ++canopyTrees;
                } else {
                    tags["r1:shrub"] = "yes";
                    tags["height"] = std::to_string(1.2 + rng.random() * 1.2);
                    candidates.push_back({3, ident, spot->x, spot->y, std::move(tags), "canopy-measured-low", false, 0});
                    ++canopyShrubs;
                }
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
    auto cellKey = [](long x, long z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); };
    for (const Candidate& c : candidates) {
        const P2 p = xy(c.lon, c.lat);
        const long kx = long(std::floor(p.x / 3)), kz = long(std::floor(p.y / 3));
        bool crowded = false;
        for (long dx = -1; dx <= 1 && !crowded; ++dx)
            for (long dz = -1; dz <= 1 && !crowded; ++dz) {
                auto it = occupied.find(cellKey(kx + dx, kz + dz));
                if (it != occupied.end())
                    for (const P2& q : it->second) if (std::hypot(p.x - q.x, p.y - q.y) < 3) { crowded = true; break; }
            }
        if (crowded) { ++rejected; continue; }
        // Roads exclude inferred planting; a surveyed tree may stand on a median.
        if (c.rank && blocked.contains(p)) { ++rejected; continue; }
        const bool grass = tagOr(c.tags, "r1:grass") == "yes";
        if (grass ? grasses >= 160 : trees >= budget) { ++budgetDropped; continue; }
        occupied[cellKey(kx, kz)].push_back(p);
        auto [model, reason] = species(c.tags, c.lon, c.lat, c.ident, c.forest);
        if (model == "broadleaf" && std::abs(c.lat) >= 30) model = "urban_tree";
        if (grass) {
            const double lo = c.lon, la = c.lat;
            const bool arid = (-18 < lo && lo < 60 && 18 < la && la < 32) || (13 < lo && lo < 23 && -32 < la && la < -20) ||
                              (115 < lo && lo < 140 && -30 < la && la < -20);
            model = tagOr(c.tags, "natural") == "wetland" ? "grass_tall" : arid ? "grass_dry" : "grass_fresh";
        }
        PyRandom rng = seeded(c.ident, 81);
        const double nominal = grass ? (model == "grass_tall" ? 1.1 : 0.45)
                               : has(c.tags, "r1:shrub") ? 2 : c.forest ? 13 : std::abs(c.lat) < 30 ? 10 : 9;
        const double observed = lengthTag(tag(c.tags, "height"), 0);
        const double height = observed > 0 ? observed : nominal * (0.8 + rng.random() * 0.4);
        heights += observed > 0;
        ++reasons[reason];
        if (grass) ++grasses; else ++trees;
        nlohmann::json children = nlohmann::json::array();
        if (grass) {
            children.push_back({{"type", "Node"}, {"name", "Near"}, {"importedFrom", "assets/models/external/nature_selected/" + model + ".glb"}});
        } else {
            const double k = 1.0 / sourceHeight(model);
            children.push_back({{"type", "Node"}, {"name", "Far"}, {"importedFrom", kCardDir + "/" + model + ".glb"},
                                {"transform", {{"scale", {k, k, k}}}}});
            if (model == "urban_tree")
                children.push_back({{"type", "Node"}, {"name", "Near"}, {"importedFrom", "assets/models/external/nature_selected/urban_tree.glb"}});
        }
        PyRandom yawRng = seeded(c.ident, 71);
        const double yaw = yawRng.random() * kTau;
        const P3 at = ground(c.lon, c.lat);
        out.nodes.push_back({{"type", "Node"}, {"name", std::string("Nature ") + c.source + " " + identString(c.ident)},
                             {"enabled", true}, {"groups", {"vegetation", grass ? "grass" : "tree"}}, {"children", children},
                             {"transform", {{"position", {at.x, at.y, at.z}}, {"rotation", {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}},
                                            {"scale", {height, height, height}}}}});
        ++sources[c.source];
        ++models[model];
    }
    out.stats = {{"revision", kNatureRevision}, {"placed", out.nodes.size()}, {"trees", trees}, {"grassTufts", grasses},
                 {"heightsMeasured", heights}, {"modelSelection", reasons}, {"bySource", sources}, {"byModel", models},
                 {"rejectedOverlap", rejected}, {"droppedForBudget", budgetDropped}, {"budget", budget},
                 {"representation", "layered cards baked from original CC0 scans"},
                 {"canopy", canopy ? nlohmann::json{{"observed", true}, {"noSource", canopy->noSource},
                                                    {"treeCells", canopy->count(Canopy::Tree)}, {"lowCells", canopy->count(Canopy::Low)},
                                                    {"treesFromCanopy", canopyTrees}, {"shrubsFromCanopy", canopyShrubs},
                                                    {"inferredRemovedByCanopy", canopyRemoved},
                                                    {"source", "Meta & WRI High Resolution Canopy Height Maps, CC BY 4.0 (imagery 2009-2020)"}}
                                   : nlohmann::json{{"observed", false}}},
                 {"speciesPolicy", "botanical/leaf tags then approximate regional morphology"}};
    return out;
}

// ── traffic ─────────────────────────────────────────────────────────────────

nlohmann::json buildLaneGraph(const std::vector<OsmWay>& roads, const GroundAt& ground, int buildings, double lon, double lat) {
    static const std::map<std::string, double> classSpeed = {
        {"motorway", 33.0}, {"trunk", 25.0}, {"primary", 16.7}, {"secondary", 13.9}, {"tertiary", 13.9},
        {"residential", 8.3}, {"living_street", 5.6}, {"unclassified", 11.1}};
    static const std::map<std::string, double> classWeight = {
        {"motorway", 6.0}, {"trunk", 5.0}, {"primary", 4.0}, {"secondary", 3.0}, {"tertiary", 2.0},
        {"residential", 0.7}, {"living_street", 0.25}, {"unclassified", 1.0}};
    constexpr double kWeld = 0.75, kMinLane = 2.0;
    // Where the left-hand-driving world is, as coarse boxes (west, south, east, north).
    static const double leftHand[][4] = {{-11.0, 49.8, 2.1, 61.0}, {60.0, 5.0, 92.5, 37.0}, {127.0, 24.0, 146.5, 46.0},
                                         {95.0, -11.0, 141.5, 21.0}, {112.0, -48.0, 179.5, -8.0},
                                         {11.0, -35.0, 42.0, 5.0}, {-78.5, 17.5, -76.0, 18.6}};
    std::vector<P3> nodes;
    nlohmann::json lanes = nlohmann::json::array();
    int measured = 0, inferred = 0;
    double metres = 0;
    // Welded in plan and in height: a bridge's node is not the road's beneath.
    std::map<std::tuple<long long, long long, long long>, int> index3;
    auto nodeAt = [&](P3 p) {
        const auto key = std::make_tuple(pyround(p.x / kWeld), pyround(p.z / kWeld), pyround(p.y / 3.0));
        auto it = index3.find(key);
        if (it != index3.end()) return it->second;
        index3[key] = int(nodes.size());
        nodes.push_back(p);
        return int(nodes.size()) - 1;
    };
    for (const OsmWay& road : roads) {
        const Tags& tags = road.tags;
        const std::string highway = tagOr(tags, "highway");
        if (!isMotorway(highway) || highway == "service" || tagOr(tags, "area") == "yes") continue;
        // Bridges are driven at the level they were solved at (gen/bridges).
        if (taggedYes(tags, "tunnel")) continue;
        double speed = classSpeed.count(highway) ? classSpeed.at(highway) : 11.1;
        bool isMeasured = false;
        if (const std::string* raw = tag(tags, "maxspeed"); raw && !raw->empty()) {
            std::string text;
            for (char c : *raw) text += char(std::tolower((unsigned char)c));
            size_t a = 0, b = text.size();
            while (a < b && std::isspace((unsigned char)text[a])) ++a;
            while (b > a && std::isspace((unsigned char)text[b - 1])) --b;
            text = text.substr(a, b - a);
            const bool mph = text.size() >= 3 && text.compare(text.size() - 3, 3, "mph") == 0;
            if (mph) text.resize(text.size() - 3);
            if (text.size() >= 4 && text.compare(text.size() - 4, 4, "km/h") == 0) text.resize(text.size() - 4);
            while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
            while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
            const double number = lengthTag(&text, 0.0);
            if (number > 0) { speed = number * (mph ? 0.44704 : 1 / 3.6); isMeasured = true; }
        }
        const double weight = classWeight.count(highway) ? classWeight.at(highway) : 1.0;
        std::string one;
        for (char c : tagOr(tags, "oneway")) one += char(std::tolower((unsigned char)c));
        int direction = 0;
        if (one == "yes" || one == "true" || one == "1") direction = 1;
        else if (one == "-1" || one == "reverse") direction = -1;
        else if (tagOr(tags, "junction") == "roundabout" || tagOr(tags, "junction") == "circular") direction = 1;
        bool used = false;
        std::vector<P3> points;
        for (const P2& p : road.points) points.push_back(ground(p.x, p.y));
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const P3 a = points[i], b = points[i + 1];
            const double span = std::hypot(a.x - b.x, a.z - b.z);
            if (span < kMinLane) continue;
            const int first = nodeAt(a), second = nodeAt(b);
            if (first == second) continue;
            if (direction >= 0) lanes.push_back({first, second, pyround(speed, 1), weight});
            if (direction <= 0) lanes.push_back({second, first, pyround(speed, 1), weight});
            metres += span;
            used = true;
        }
        if (used) ++(isMeasured ? measured : inferred);
    }
    int cars = 0;
    if (!lanes.empty()) cars = int(std::min<long long>(22, pyround(buildings / 28.0 + metres / 1000.0 * 1.6)));
    bool left = false;
    for (const auto& box : leftHand) left |= box[0] <= lon && lon <= box[2] && box[1] <= lat && lat <= box[3];
    nlohmann::json nodeList = nlohmann::json::array();
    for (const P3& n : nodes) nodeList.push_back({pyround(n.x, 2), pyround(n.y, 2), pyround(n.z, 2)});
    return {{"nodes", nodeList}, {"lanes", lanes}, {"cars", cars}, {"leftHand", left}, {"roadMetres", pyround(metres, 1)},
            {"speedsTagged", measured}, {"speedsInferred", inferred}};
}

}  // namespace r1
