#include "crowd.hpp"

#include "spatial.hpp"
#include "streets.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

namespace r1 {

namespace {
// Ways a pedestrian walks along the middle of.
const std::set<std::string> kFoot = {"pedestrian", "footway", "path", "steps"};
// Buildings where people come and go all day.
const std::set<std::string> kBusyBuilding = {"retail", "commercial", "kiosk", "supermarket", "office",
                                             "train_station", "transportation", "public", "civic"};
const std::set<std::string> kNonResidentialBuilding = {"garage", "garages", "shed", "barn", "farm_auxiliary",
    "industrial", "warehouse", "retail", "commercial", "kiosk", "supermarket", "office", "train_station",
    "transportation", "public", "civic", "church", "school", "hospital", "roof", "construction"};
constexpr double kSampleStep = 5.0;       // metres between samples of a walked line
constexpr double kWeld = 0.75;      // two samples closer than this are one node
constexpr double kReach = 14.0;     // longest corner or crossing joined at a loose end
// Passing someone who stands on the path: centre to centre, and the widest
// step aside a sidewalk leaves room for before one has to stop instead.
constexpr double kPass = 0.85, kMostDodge = 1.1;
constexpr double kPavingLift = 0.21;  // the paving's drape lift in streets.cpp

// Segments and rings bucketed on a grid, for "is this point in one of you".
template <class T>
struct Buckets {
    double cell;
    std::unordered_map<int64_t, std::vector<size_t>> cells;
    std::vector<T> items;
    explicit Buckets(double c) : cell(c) {}
    static int64_t key(long x, long z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); }
    void add(T item, double x0, double z0, double x1, double z1) {
        const size_t index = items.size();
        items.push_back(std::move(item));
        for (long x = long(std::floor(x0 / cell)); x <= long(std::floor(x1 / cell)); ++x)
            for (long z = long(std::floor(z0 / cell)); z <= long(std::floor(z1 / cell)); ++z) cells[key(x, z)].push_back(index);
    }
    const std::vector<size_t>* at(P2 p) const {
        auto it = cells.find(key(long(std::floor(p.x / cell)), long(std::floor(p.y / cell))));
        return it == cells.end() ? nullptr : &it->second;
    }
};

struct Carriage { P2 a, b; double half; };

double segmentDistance(P2 p, P2 a, P2 b) {
    const double dx = b.x - a.x, dz = b.y - a.y, l2 = dx * dx + dz * dz;
    const double t = l2 > 1e-12 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dz) / l2, 0.0, 1.0) : 0.0;
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dz));
}

std::vector<P2> offsetLine(const std::vector<P2>& line, double distance) {
    std::vector<P2> out;
    const size_t n = line.size();
    for (size_t i = 0; i < n; ++i) {
        auto normal = [&](size_t a, size_t b) {
            const double dx = line[b].x - line[a].x, dz = line[b].y - line[a].y, l = std::max(1e-9, std::hypot(dx, dz));
            return P2{-dz / l, dx / l};
        };
        P2 m = i == 0 ? normal(0, 1) : i == n - 1 ? normal(n - 2, n - 1) : [&] {
            const P2 a = normal(i - 1, i), b = normal(i, i + 1);
            P2 s{a.x + b.x, a.y + b.y};
            const double l = std::hypot(s.x, s.y);
            if (l < 1e-9) return a;
            s = {s.x / l, s.y / l};
            const double k = 1.0 / std::max(s.x * a.x + s.y * a.y, 0.5);
            return P2{s.x * k, s.y * k};
        }();
        out.push_back({line[i].x + m.x * distance, line[i].y + m.y * distance});
    }
    return out;
}
}  // namespace

double crowdHourFactor(double hour) {
    // Pedestrian counts through a working day, smoothed: nearly nobody
    // before dawn, the morning rush, lunch, the evening peak, then it thins.
    static const double table[25] = {0.10, 0.06, 0.04, 0.03, 0.03, 0.05, 0.15, 0.45, 0.80, 0.75, 0.75, 0.85,
                                     1.00, 1.00, 0.85, 0.80, 0.85, 1.00, 1.00, 0.85, 0.60, 0.45, 0.30, 0.18, 0.10};
    const double h = pymod(hour, 24.0);
    const int i = int(h);
    return table[i] + (table[i + 1] - table[i]) * (h - i);
}

nlohmann::json buildWalkGraph(const std::vector<OsmWay>& roads, const std::vector<OsmNode>& features,
                              const std::vector<const OsmWay*>& buildings, const std::vector<Ring>& footprints,
                              const ElevationGrid& elevations, const Anchor& anchor, const nlohmann::json& props) {
    const Drape drape(elevations, anchor);
    auto engine = [&](P2 lonLat) { const P3 e = anchor.toEngine(lonLat.x, lonLat.y, 0.0); return P2{e.x, e.z}; };

    // Where nobody walks: inside a building, and on a carriageway.
    Buckets<size_t> rings(20.0);
    for (size_t i = 0; i < footprints.size(); ++i) {
        const Ring& r = footprints[i];
        if (r.size() < 3) continue;
        double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
        for (const P2& p : r) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.y); z1 = std::max(z1, p.y); }
        rings.add(i, x0, z0, x1, z1);
    }
    auto indoors = [&](P2 p) {
        if (const auto* list = rings.at(p))
            for (size_t i : *list) if (pointInPolygon(p, footprints[rings.items[i]])) return true;
        return false;
    };
    Buckets<Carriage> carriages(16.0);
    struct Walked { std::vector<P2> line; bool crossing, sidewalk; };
    std::vector<Walked> lines;
    double sidewalkMetres = 0, footMetres = 0;
    for (const OsmWay& road : roads) {
        const Tags& tags = road.tags;
        if (taggedYes(tags, "tunnel") || taggedYes(tags, "bridge") || has(tags, "r1:raised")) continue;
        const std::string highway = tagOr(tags, "highway");
        std::vector<P2> line;
        for (const P2& p : road.points) {
            const P2 e = engine(p);
            if (line.empty() || dist(line.back(), e) > 0.05) line.push_back(e);
        }
        if (line.size() < 2) continue;
        if (kFoot.count(highway)) {
            lines.push_back({line, tagOr(tags, "footway") == "crossing", false});
            continue;
        }
        if (!isMotorway(highway) || tagOr(tags, "area") == "yes") continue;
        const double half = roadWidth(tags) * 0.5;
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            const P2 a = line[i], b = line[i + 1];
            carriages.add({a, b, half}, std::min(a.x, b.x) - half, std::min(a.y, b.y) - half,
                          std::max(a.x, b.x) + half, std::max(a.y, b.y) + half);
        }
        for (const auto& [side, inferred] : sidewalkSides(tags)) {
            const double width = lengthTag(tag(tags, ("sidewalk:" + side + ":width").c_str()),
                                           lengthTag(tag(tags, "sidewalk:width"), 1.8));
            // Engine z points south: geographic left is the numeric right (streets.cpp).
            const double sign = side == "left" ? -1.0 : 1.0;
            lines.push_back({offsetLine(line, sign * (half + width * 0.5)), false, true});
        }
    }
    auto onCarriageway = [&](P2 p) {
        if (const auto* list = carriages.at(p))
            for (size_t i : *list) {
                const Carriage& c = carriages.items[i];
                if (segmentDistance(p, c.a, c.b) < c.half + 0.2) return true;
            }
        return false;
    };

    std::vector<P3> nodes;
    std::vector<std::set<int>> lineOf;  // the walked lines each node belongs to
    std::map<std::pair<long long, long long>, int> index;
    std::set<std::pair<int, int>> linked;
    nlohmann::json links = nlohmann::json::array();
    auto nodeAt = [&](P2 p, int line) {
        const auto key = std::make_pair(pyround(p.x / kWeld), pyround(p.y / kWeld));
        auto it = index.find(key);
        int id;
        if (it != index.end()) id = it->second;
        else {
            id = int(nodes.size());
            index[key] = id;
            nodes.push_back({p.x, drape.heightAt(p) + kPavingLift, p.y});
            lineOf.emplace_back();
        }
        lineOf[id].insert(line);
        return id;
    };
    auto link = [&](int a, int b, bool crossing) {
        if (a == b || !linked.insert({std::min(a, b), std::max(a, b)}).second) return;
        links.push_back({a, b, crossing ? 1 : 0});
    };
    std::vector<int> ends;
    for (size_t li = 0; li < lines.size(); ++li) {
        const Walked& w = lines[li];
        std::vector<std::pair<P2, bool>> samples;  // point, kept
        for (size_t i = 0; i + 1 < w.line.size(); ++i) {
            const P2 a = w.line[i], b = w.line[i + 1];
            const int steps = std::max(1, int(std::ceil(dist(a, b) / kSampleStep)));
            for (int s = (i == 0 ? 0 : 1); s <= steps; ++s) {
                const double t = double(s) / steps;
                const P2 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                samples.push_back({p, !indoors(p) && (w.crossing || !onCarriageway(p))});
            }
        }
        int previous = -1;
        double run = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            if (!samples[i].second) {
                if (previous >= 0) ends.push_back(previous);
                previous = -1;
                continue;
            }
            const int id = nodeAt(samples[i].first, int(li));
            if (previous < 0) ends.push_back(id);
            else if (previous != id) {
                link(previous, id, w.crossing);
                run += std::hypot(nodes[id].x - nodes[previous].x, nodes[id].z - nodes[previous].z);
            }
            previous = id;
        }
        if (previous >= 0) ends.push_back(previous);
        (w.sidewalk ? sidewalkMetres : footMetres) += run;
    }
    // Loose ends meet the nearest walk of another line: round a corner on
    // the same pavement, or across the street to the one opposite.
    Buckets<int> near(kReach);
    for (size_t i = 0; i < nodes.size(); ++i)
        near.add(int(i), nodes[i].x, nodes[i].z, nodes[i].x, nodes[i].z);
    int crossings = 0;
    std::sort(ends.begin(), ends.end());
    ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
    for (int e : ends) {
        const P2 p{nodes[e].x, nodes[e].z};
        std::vector<std::pair<double, int>> candidates;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz) {
                const auto* list = near.at({p.x + dx * kReach, p.y + dz * kReach});
                if (!list) continue;
                for (size_t k : *list) {
                    const int n = near.items[k];
                    if (n == e) continue;
                    bool shared = false;
                    for (int l : lineOf[n]) shared |= lineOf[e].count(l) > 0;
                    if (shared) continue;
                    const double d = std::hypot(nodes[n].x - p.x, nodes[n].z - p.y);
                    if (d <= kReach) candidates.push_back({d, n});
                }
            }
        std::sort(candidates.begin(), candidates.end());
        int joinedCorner = 0, joinedCrossing = 0;
        for (const auto& [d, n] : candidates) {
            const P2 q{nodes[n].x, nodes[n].z};
            bool blocked = false, road = false;
            const int steps = std::max(1, int(std::ceil(d)));
            for (int s = 1; s < steps; ++s) {
                const P2 m{p.x + (q.x - p.x) * s / steps, p.y + (q.y - p.y) * s / steps};
                blocked |= indoors(m);
                road |= onCarriageway(m);
            }
            if (blocked) continue;
            int& joined = road ? joinedCrossing : joinedCorner;
            if (joined) continue;
            // A corner is short; a crossing may span a boulevard.
            if (!road && d > 6.0) continue;
            joined = 1;
            link(e, n, road);
            crossings += road;
            if (joinedCorner && joinedCrossing) break;
        }
    }

    // Benches: the ones the props actually placed, two places each.
    nlohmann::json seats = nlohmann::json::array();
    for (const auto& prop : props) {
        const std::string name = prop.value("name", std::string());
        if (name.rfind("bench ", 0) != 0) continue;
        const auto& t = prop.at("transform");
        const auto& p = t.at("position");
        const auto& q = t.at("rotation");
        const double s = t.at("scale")[0].get<double>();
        const double yaw = 2.0 * std::atan2(q[1].get<double>(), q[3].get<double>());
        // bench.glb: 0.81 wide, the seat's middle 0.05 in front of its origin.
        const double seatTop = p[1].get<double>() + kBenchSeat * 0.526 * s;
        for (double across : {-0.2, 0.2}) {
            const double x = p[0].get<double>() + (std::cos(yaw) * across + std::sin(yaw) * 0.05) * s;
            const double z = p[2].get<double>() + (-std::sin(yaw) * across + std::cos(yaw) * 0.05) * s;
            seats.push_back({pyround(x, 2), pyround(seatTop, 3), pyround(z, 2), pyround(yaw, 4)});
        }
    }

    // Demand is local to each walked segment. A long pavement through open
    // country contributes nothing; a block with many homes attracts walkers.
    struct ActivitySource { P2 p; double weight; bool busy, home, served = false; };
    std::vector<ActivitySource> activity;
    for (const OsmWay* b : buildings) {
        if (b->points.empty()) continue;
        const std::string kind = tagOr(b->tags, "building");
        const bool busy = kBusyBuilding.count(kind) || has(b->tags, "shop") || has(b->tags, "amenity") || has(b->tags, "office");
        const bool home = !busy && !kind.empty() && !kNonResidentialBuilding.count(kind) && kind != "no";
        if (!busy && !home) continue;
        P2 centre{0, 0};
        const size_t count = b->closed() ? b->points.size() - 1 : b->points.size();
        for (size_t i = 0; i < count; ++i) { const P2 p = engine(b->points[i]); centre.x += p.x; centre.y += p.y; }
        centre.x /= count; centre.y /= count;
        double weight = 1.0;
        if (home) {
            const double levels = std::clamp(lengthTag(tag(b->tags, "building:levels"), 1.0), 1.0, 12.0);
            const double flats = std::clamp(lengthTag(tag(b->tags, "building:flats"), 0.0), 0.0, 30.0);
            // A tagged apartment block represents several households; a
            // house or an untyped OSM building starts at one household.
            weight = kind == "apartments" ? 2.0 + 0.65 * (levels - 1.0) : 1.0 + 0.3 * (levels - 1.0);
            if (flats > 0) weight = std::max(weight, flats * 0.55);
            weight = std::min(weight, 8.0);
        } else {
            weight = 1.8;
        }
        activity.push_back({centre, weight, busy, home});
    }
    // The sources near a point, in their order (the demand below is a sum).
    // A metre of slack: the tests below decide, the grid only skips.
    BoxIndex sources(64.0);
    for (size_t i = 0; i < activity.size(); ++i) sources.add(i, {activity[i].p.x, activity[i].p.y, activity[i].p.x, activity[i].p.y});
    auto sourcesNear = [&](P2 p, double radius) { return sources.near(Box{p.x, p.y, p.x, p.y}.grown(radius + 1.0)); };
    // Close homes reinforce one another: the same number of dwellings in a
    // compact block supports more foot traffic than isolated farmhouses.
    for (size_t i = 0; i < activity.size(); ++i) {
        auto& source = activity[i];
        if (!source.home) continue;
        int neighbours = 0;
        for (size_t j : sourcesNear(source.p, 50.0)) {
            const auto& other = activity[j];
            if (j != i && other.home &&
                std::abs(other.p.x - source.p.x) < 50.0 && std::abs(other.p.y - source.p.y) < 50.0 &&
                dist(other.p, source.p) < 50.0) ++neighbours;
        }
        source.weight *= 1.0 + std::min(0.75, neighbours * 0.12);
    }
    constexpr double kActivityRadius = 80.0;
    int activeLinks = 0;
    for (auto& entry : links) {
        const P3& a = nodes[entry[0].get<int>()], &b = nodes[entry[1].get<int>()];
        const P2 midpoint{(a.x + b.x) * 0.5, (a.z + b.z) * 0.5};
        double demand = 0;
        for (size_t i : sourcesNear(midpoint, kActivityRadius)) {
            auto& source = activity[i];
            if (std::abs(source.p.x - midpoint.x) > kActivityRadius ||
                std::abs(source.p.y - midpoint.y) > kActivityRadius) continue;
            const double distance = segmentDistance(source.p, {a.x, a.z}, {b.x, b.z});
            if (distance >= kActivityRadius) continue;
            demand += source.weight * (1.0 - distance / kActivityRadius);
            source.served = true;
        }
        entry.push_back(pyround(demand, 3));
        activeLinks += demand > 0 && entry[2].get<int>() == 0;
    }
    int busy = 0, homes = 0;
    double homeMass = 0, busyMass = 0;
    for (const auto& source : activity) if (source.served) {
        busy += source.busy; homes += source.home;
        if (source.home) homeMass += source.weight; else busyMass += source.weight;
    }
    int stops = 0, lights = 0;
    for (const OsmNode& f : features) {
        const std::string h = tagOr(f.tags, "highway");
        stops += h == "bus_stop";
        lights += h == "traffic_signals" || h == "crossing";
    }
    const double asked = activeLinks == 0 ? 0.0 : homeMass * 0.7 + busyMass * 0.7;
    const int people = int(std::min<double>(kCrowdTileCeiling, pyround(asked)));

    nlohmann::json nodeList = nlohmann::json::array();
    for (const P3& n : nodes) nodeList.push_back({pyround(n.x, 2), pyround(n.y, 2), pyround(n.z, 2)});
    return {{"revision", kCrowdRevision}, {"nodes", nodeList}, {"links", links}, {"seats", seats},
            {"people", people},
            {"inputs", {{"sidewalkMetres", pyround(sidewalkMetres, 1)}, {"footwayMetres", pyround(footMetres, 1)},
                        {"busyBuildings", busy}, {"homesNearPaths", homes}, {"residentialDemand", pyround(homeMass, 2)},
                        {"buildings", buildings.size()},
                        {"activeLinks", activeLinks}, {"busStops", stops},
                        {"crossingsAndSignals", lights}, {"crossingsJoined", crossings}}},
            {"inferred", "density from nearby homes, their levels and local building concentration, before the hour; paths from OSM sidewalks and footways"}};
}

// ── run time ────────────────────────────────────────────────────────────────

WalkGraph WalkGraph::from(const nlohmann::json& crowd) {
    WalkGraph g;
    if (!crowd.is_object()) return g;
    for (const auto& n : crowd.value("nodes", nlohmann::json::array()))
        g.nodes.push_back({n[0].get<double>(), n[1].get<double>(), n[2].get<double>(), {}});
    for (const auto& l : crowd.value("links", nlohmann::json::array())) {
        Link link{l[0].get<int>(), l[1].get<int>(), l[2].get<int>() != 0, 0.0,
                  l.size() > 3 ? l[3].get<double>() : 1.0};
        if (link.a < 0 || link.b < 0 || link.a >= int(g.nodes.size()) || link.b >= int(g.nodes.size())) continue;
        const Node &a = g.nodes[link.a], &b = g.nodes[link.b];
        link.length = std::hypot(b.x - a.x, b.z - a.z);
        if (link.length < 1e-3) continue;
        g.nodes[link.a].links.push_back(int(g.links.size()));
        g.nodes[link.b].links.push_back(int(g.links.size()));
        g.links.push_back(link);
    }
    for (const auto& s : crowd.value("seats", nlohmann::json::array()))
        g.seats.push_back({s[0].get<double>(), s[1].get<double>(), s[2].get<double>(), s[3].get<double>()});
    g.people = crowd.value("people", 0);
    return g;
}

const char* clipOf(Activity a) {
    switch (a) {
        case Activity::Walk: return "walk";
        case Activity::Idle: return "idle";
        case Activity::Wait: return "wait";
        case Activity::Phone: return "phone";
        case Activity::Talk: return "talk";
        case Activity::Sit: return "sit";
        case Activity::Flee: return "run";
        // Carried off their line: the body's stagger is the game's spring,
        // over a stance.
        case Activity::Stagger: return "idle";
        case Activity::Shrug: return "shrug";
        case Activity::Angry: return "angry";
        case Activity::Dust: return "dust";
    }
    return "idle";
}

void Crowd::reset(const WalkGraph* graph, uint32_t seed, std::vector<CrowdPace> pace) {
    graph_ = graph;
    pace_ = pace.empty() ? std::vector<CrowdPace>{CrowdPace{}} : std::move(pace);
    walkers_.clear();
    seatTaken_.assign(graph ? graph->seats.size() : 0, -1);
    spawnChoices_.clear();
    spawnDemand_ = 0;
    if (graph) for (size_t i = 0; i < graph->links.size(); ++i) {
        const auto& link = graph->links[i];
        if (link.crossing || link.demand <= 0) continue;
        spawnDemand_ += link.demand;
        spawnChoices_.push_back({spawnDemand_, int(i)});
    }
    state_ = (uint64_t(seed) << 1) | 1;
    wanted_ = 0;
}

double Crowd::random() {
    // xorshift64*: the simulation's own draws, never the cook's.
    state_ ^= state_ >> 12; state_ ^= state_ << 25; state_ ^= state_ >> 27;
    return double((state_ * 2685821657736338717ull) >> 11) / double(1ull << 53);
}

int Crowd::live() const {
    int n = 0;
    for (const Walker& w : walkers_) n += w.alive;
    return n;
}

void Crowd::stand(Walker& w, double lo, double hi) {
    const double r = random();
    w.activity = r < 0.45 ? Activity::Idle : r < 0.75 ? Activity::Phone : Activity::Wait;
    w.timer = lo + (hi - lo) * random();
}

void Crowd::place(Walker& w) const {
    const auto& g = *graph_;
    if (w.activity == Activity::Sit && w.seat >= 0) {
        const auto& s = g.seats[w.seat];
        w.x = s.x; w.y = s.y; w.z = s.z;
        w.heading = s.yaw;
        return;
    }
    if (w.link < 0) return;
    const auto& l = g.links[w.link];
    const auto& from = g.nodes[w.forward ? l.a : l.b];
    const auto& to = g.nodes[w.forward ? l.b : l.a];
    const double t = std::clamp(w.along / l.length, 0.0, 1.0);
    const double dx = (to.x - from.x) / l.length, dz = (to.z - from.z) / l.length;
    // Keep right of the direction of travel: two people meeting pass.
    // Right of (dx, dz) with z south is (-dz, dx).
    const double side = w.side + w.dodge;
    w.x = from.x + (to.x - from.x) * t - dz * side + w.offX;
    w.z = from.z + (to.z - from.z) * t + dx * side + w.offZ;
    w.y = from.y + (to.y - from.y) * t;
    // Across a street the pavement's 15 cm kerb is stepped down.
    if (l.crossing && t > 0.12 && t < 0.88) w.y -= 0.15;
    if (w.activity == Activity::Walk || w.activity == Activity::Flee) w.heading = std::atan2(dx, -dz);
}

void Crowd::arrive(size_t slot, int node) {
    Walker& w = walkers_[slot];
    const auto& g = *graph_;
    const auto& options = g.nodes[node].links;
    const int came = w.link;
    double total = 0;
    for (int l : options) if ((l != came || options.size() == 1) && g.links[l].demand > 0)
        total += (g.links[l].crossing ? 0.35 : 1.0) * g.links[l].demand;
    // At the end of the inhabited walk, turn back instead of drifting out
    // along a path with no homes or activity nearby.
    if (total <= 0 && came >= 0 && g.links[came].demand > 0) {
        w.link = came; w.forward = g.links[came].a == node; w.along = 0;
        return;
    }
    double pick = random() * total;
    int next = -1;
    for (int l : options) {
        if ((l == came && options.size() > 1) || g.links[l].demand <= 0) continue;
        pick -= (g.links[l].crossing ? 0.35 : 1.0) * g.links[l].demand;
        next = l;
        if (pick <= 0) break;
    }
    if (next < 0) { w.alive = false; return; }
    w.link = next;
    w.forward = g.links[next].a == node;
    w.along = 0;
    if (w.activity == Activity::Walk && !g.links[next].crossing && random() < 0.06) stand(w, 4.0, 14.0);
}

bool Crowd::spawn(size_t slot, const Scene& scene) {
    const auto& g = *graph_;
    if (g.links.empty()) return false;
    Walker& w = walkers_[slot];
    // Out of the way of the eye: never appear within sight close by.
    auto acceptable = [&](double x, double z) {
        const double dx = x - scene.eyeX, dz = z - scene.eyeZ, d = std::hypot(dx, dz);
        if (d < kSpawnNear || d > kSpawnFar) return false;
        const double ahead = (dx * scene.faceX + dz * scene.faceZ) / std::max(1e-6, d);
        return d > 70.0 || ahead < 0.2;
    };
    auto activeSeat = [&](const WalkGraph::Seat& seat) {
        for (const auto& link : g.links) {
            if (link.crossing || link.demand <= 0) continue;
            const auto& a = g.nodes[link.a], &b = g.nodes[link.b];
            if (segmentDistance({seat.x, seat.z}, {a.x, a.z}, {b.x, b.z}) < 20.0) return true;
        }
        return false;
    };
    // On a bench near the inhabited walks, now and then.
    if (!g.seats.empty() && random() < 0.12) {
        const int s = int(random() * g.seats.size()) % int(g.seats.size());
        if (seatTaken_[s] < 0 && activeSeat(g.seats[s]) && acceptable(g.seats[s].x, g.seats[s].z)) {
            w = Walker{};
            w.alive = true; w.activity = Activity::Sit; w.seat = s; w.timer = 25.0 + 60.0 * random();
            seatTaken_[s] = int(slot);
            place(w);
            return true;
        }
    }
    if (spawnDemand_ <= 0) return false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        const double pick = random() * spawnDemand_;
        const auto chosen = std::lower_bound(spawnChoices_.begin(), spawnChoices_.end(), pick,
            [](const std::pair<double, int>& entry, double value) { return entry.first < value; });
        const int l = (chosen == spawnChoices_.end() ? spawnChoices_.back() : *chosen).second;
        const auto& link = g.links[l];
        if (link.crossing) continue;
        const double t = random();
        const auto &a = g.nodes[link.a], &b = g.nodes[link.b];
        if (!acceptable(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t)) continue;
        w = Walker{};
        w.alive = true; w.link = l; w.forward = random() < 0.5; w.along = t * link.length;
        w.side = 0.35 + 0.4 * random();
        const double r = random();
        if (r < 0.66) w.activity = Activity::Walk;
        else stand(w, 6.0, 30.0);
        // Two friends talking, facing each other, now and then.
        if (w.activity != Activity::Walk && random() < 0.3) {
            for (size_t o = 0; o < walkers_.size() && int(o) < wanted_; ++o) {
                if (o == slot || walkers_[o].alive) continue;
                Walker& f = walkers_[o];
                f = w;
                f.side = -w.side;  // across the path from the other
                w.activity = f.activity = Activity::Talk;
                w.partner = int(o); f.partner = int(slot);
                f.timer = w.timer = 12.0 + 25.0 * random();
                place(f);
                break;
            }
        }
        place(w);
        if (w.partner >= 0) {
            Walker& f = walkers_[w.partner];
            w.heading = std::atan2(f.x - w.x, -(f.z - w.z));
            f.heading = std::atan2(w.x - f.x, -(w.z - f.z));
        }
        return true;
    }
    return false;
}

void Crowd::react(Walker& w) {
    // A brush is shrugged off. A shove is not, and neither is a second bump.
    const double r = random();
    if (w.grudge <= 1.0 && w.bumpSpeed < kShove) {
        w.activity = r < 0.7 ? Activity::Shrug : Activity::Angry;
    } else {
        w.activity = r < 0.6 ? Activity::Angry : Activity::Dust;
    }
    w.timer = w.activity == Activity::Shrug ? 1.8 + 0.6 * random() : 2.5 + 2.0 * random();
}

double Crowd::speedAlong(size_t slot, double dirX, double dirZ) const {
    if (slot >= walkers_.size() || !walkers_[slot].alive) return 0;
    const Walker& w = walkers_[slot];
    if (w.activity != Activity::Walk && w.activity != Activity::Flee) return 0;
    const CrowdPace& pace = pace_[slot % pace_.size()];
    const double speed = w.activity == Activity::Flee ? pace.run : pace.walk;
    return speed * (std::sin(w.heading) * dirX - std::cos(w.heading) * dirZ);
}

bool Crowd::bump(size_t slot, double dirX, double dirZ, double speed) {
    if (slot >= walkers_.size()) return false;
    Walker& w = walkers_[slot];
    // A bench holds its sitter, and a bump lasts a moment.
    if (!w.alive || w.activity == Activity::Sit || speed <= kBumpSpeed || w.bumpCool > 0) return false;
    const double n = std::hypot(dirX, dirZ);
    if (n < 1e-9) return false;
    dirX /= n; dirZ /= n;
    const bool shove = speed >= kShove;
    w.bumpCool = 0.8;
    ++w.bumps;
    w.bumpX = dirX; w.bumpZ = dirZ; w.bumpSpeed = speed;
    // Knocked back along the blow, further by a sprint than by a walk: the
    // stagger's own steps, which the path is walked back from after.
    const double push = speed * (shove ? 0.6 : 0.45);
    w.pushX += dirX * push; w.pushZ += dirZ * push;
    // What they were doing waits for them.
    const bool reacting = w.activity == Activity::Stagger || w.activity == Activity::Shrug ||
                          w.activity == Activity::Angry || w.activity == Activity::Dust;
    if (!reacting) {
        w.resume = w.activity == Activity::Flee ? Activity::Walk : w.activity;
        w.resumeTimer = w.timer;
    }
    w.grudge += shove ? 2.0 : 1.0;
    w.activity = Activity::Stagger;
    w.timer = shove ? 0.7 : 0.35;
    // Nobody is walked into without looking round at who did it.
    w.lookTimer = std::max(w.lookTimer, w.timer + 4.0);
    if (w.partner >= 0) walkers_[w.partner].lookTimer = std::max(walkers_[w.partner].lookTimer, 3.0);
    return true;
}

void Crowd::lookAtPlayer(Walker& w, double dt, const Scene& scene) {
    w.lookCool = std::max(0.0, w.lookCool - dt);
    if (scene.playerX > 1e8) { w.lookTimer = 0; w.look = 0; return; }
    const double px = scene.playerX - w.x, pz = scene.playerZ - w.z, d = std::hypot(px, pz);
    // 1 straight ahead of them, -1 straight behind.
    const double facing = d > 1e-6 ? (px * std::sin(w.heading) - pz * std::cos(w.heading)) / d : 1.0;
    if (w.lookTimer > 0) {
        w.lookTimer -= dt;
        // Gone out of reach, or round behind them once he is past: the head
        // follows him to its limit, holds there a moment, and lets him go.
        if (d > 2 * kLookRange || (facing < -0.3 && d > 2.0)) w.lookTimer = std::min(w.lookTimer, 0.8);
        if (w.lookTimer <= 0) w.lookCool = 5.0 + 10.0 * random();
    } else if (w.lookCool <= 0 && d < kLookRange && facing > 0.25) {
        // He passes in front of them, moving or right there. Not everyone
        // looks up: someone on the telephone or deep in a conversation less.
        const double speed = std::hypot(scene.playerVX, scene.playerVZ);
        if (speed > 0.5 || d < 2.5) {
            const double chance = speed >= kShove ? 0.95
                                  : w.activity == Activity::Phone ? 0.35
                                  : w.activity == Activity::Talk ? 0.5
                                  : 0.8;
            if (random() < chance) w.lookTimer = 2.5 + 3.5 * random();
            else w.lookCool = 3.0 + 4.0 * random();
        }
    }
    // Told off or shrugged at, he is faced; dusting down is done where they stand.
    if ((w.activity == Activity::Shrug || w.activity == Activity::Angry) && d > 1e-6) w.heading = std::atan2(px, -pz);
    w.look = w.lookTimer > 0 ? 1.0 : 0.0;
}

void Crowd::update(double dt, const Scene& scene) {
    if (!graph_) return;
    const auto& g = *graph_;
    if (int(walkers_.size()) < wanted_) walkers_.resize(size_t(wanted_));
    for (size_t i = 0; i < walkers_.size(); ++i) {
        Walker& w = walkers_[i];
        if (!w.alive) continue;
        const double far = std::hypot(w.x - scene.eyeX, w.z - scene.eyeZ);
        // Too far, or more people than the hour wants: gone, out of sight.
        const bool surplus = int(i) >= wanted_;
        if (far > kDespawn || (surplus && far > kSpawnNear &&
                               ((w.x - scene.eyeX) * scene.faceX + (w.z - scene.eyeZ) * scene.faceZ) < 0)) {
            if (w.seat >= 0) seatTaken_[w.seat] = -1;
            if (w.partner >= 0 && w.partner < int(walkers_.size())) walkers_[w.partner].partner = -1;
            w.alive = false;
            continue;
        }
        const CrowdPace& pace = pace_[i % pace_.size()];
        // A car coming through: everyone standing near it runs for it.
        const double carGap = std::hypot(w.x - scene.carX, w.z - scene.carZ);
        if (scene.carSpeed > 3.0 && carGap < 7.0 && w.link >= 0 && w.activity != Activity::Sit) {
            if (w.activity != Activity::Flee) {
                const auto& l = g.links[w.link];
                const auto& to = g.nodes[w.forward ? l.b : l.a];
                // Away from the car along the path.
                if ((to.x - w.x) * (scene.carX - w.x) + (to.z - w.z) * (scene.carZ - w.z) > 0) {
                    w.forward = !w.forward;
                    w.along = l.length - w.along;
                }
                if (w.partner >= 0) { walkers_[w.partner].partner = -1; w.partner = -1; }
            }
            w.activity = Activity::Flee;
            w.timer = 2.5;
        }
        switch (w.activity) {
            case Activity::Sit:
                if ((w.timer -= dt) <= 0) {
                    // Up, and to the nearest walk.
                    int best = -1;
                    double bestD = 20.0;
                    for (size_t n = 0; n < g.nodes.size(); ++n) {
                        const double d = std::hypot(g.nodes[n].x - w.x, g.nodes[n].z - w.z);
                        if (d < bestD && !g.nodes[n].links.empty()) { bestD = d; best = int(n); }
                    }
                    seatTaken_[w.seat] = -1;
                    w.seat = -1;
                    if (best < 0) { w.alive = false; break; }
                    w.activity = Activity::Walk;
                    w.target = best;
                    w.link = -1;
                }
                break;
            case Activity::Stagger: case Activity::Shrug: case Activity::Angry: case Activity::Dust:
                if ((w.timer -= dt) <= 0) {
                    if (w.activity == Activity::Stagger) react(w);
                    else { w.activity = w.resume; w.timer = w.resumeTimer; }
                }
                break;
            case Activity::Idle: case Activity::Wait: case Activity::Phone: case Activity::Talk:
                if ((w.timer -= dt) <= 0) {
                    if (w.partner >= 0) {
                        Walker& f = walkers_[w.partner];
                        f.partner = -1;
                        f.activity = Activity::Walk;
                        f.forward = !w.forward;
                        if (f.link >= 0) f.along = g.links[f.link].length - w.along;
                        w.partner = -1;
                    }
                    w.activity = Activity::Walk;
                }
                break;
            case Activity::Walk: case Activity::Flee: {
                const double speed = w.activity == Activity::Flee ? pace.run : pace.walk;
                if (w.activity == Activity::Flee && (w.timer -= dt) <= 0) w.activity = Activity::Walk;
                if (w.link < 0) {
                    // Off the graph, to a node (leaving a bench).
                    if (w.target < 0) { w.alive = false; break; }
                    const auto& n = g.nodes[w.target];
                    const double dx = n.x - w.x, dz = n.z - w.z, d = std::hypot(dx, dz);
                    if (d <= speed * dt) { w.x = n.x; w.z = n.z; w.y = n.y; arrive(i, w.target); w.target = -1; }
                    else {
                        w.x += dx / d * speed * dt; w.z += dz / d * speed * dt;
                        w.heading = std::atan2(dx, -dz);
                    }
                    break;
                }
                // The player in the way is passed: a step aside, taken from a
                // few metres out, and held until he is behind. Measured from
                // the walker's own line, not where the step has put them, so
                // the step does not undo itself. Only with no room to pass
                // does anyone stop.
                const double px = scene.playerX - w.x, pz = scene.playerZ - w.z;
                const double hx = std::sin(w.heading), hz = -std::cos(w.heading);
                const double ahead = px * hx + pz * hz;
                const double across = px * -hz + pz * hx + w.dodge;  // right of the line
                w.dodgeWanted = 0;
                if (w.activity == Activity::Walk && ahead > -0.6 && ahead < 4.0 && std::abs(across) < kPass) {
                    w.dodgeWanted = across >= 0 ? across - kPass : across + kPass;
                    if (std::abs(w.dodgeWanted) > kMostDodge) {
                        w.dodgeWanted = std::clamp(w.dodgeWanted, -kMostDodge, kMostDodge);
                        if (ahead > 0 && ahead < 0.9) {
                            w.activity = Activity::Idle;
                            w.timer = 1.0;
                            break;
                        }
                    }
                }
                w.along += speed * dt;
                while (w.link >= 0 && w.along >= g.links[w.link].length) {
                    const double over = w.along - g.links[w.link].length;
                    const auto& l = g.links[w.link];
                    arrive(i, w.forward ? l.b : l.a);
                    if (!w.alive) break;
                    w.along += over;
                    if (w.activity != Activity::Walk && w.activity != Activity::Flee) break;
                }
                break;
            }
        }
        if (!w.alive) continue;
        // A push dies away in about half a second; walking again, the path
        // is regained in a couple of seconds, and so is the step aside.
        const double fade = std::exp(-6.0 * dt);
        w.pushX *= fade; w.pushZ *= fade;
        if (w.link >= 0) { w.offX += w.pushX * dt; w.offZ += w.pushZ * dt; }
        else { w.x += w.pushX * dt; w.z += w.pushZ * dt; }
        if (w.activity == Activity::Walk || w.activity == Activity::Flee) {
            const double back = std::exp(-1.2 * dt);
            w.offX *= back; w.offZ *= back;
            w.dodge += (w.dodgeWanted - w.dodge) * (1 - std::exp(-3.0 * dt));
        }
        if (w.link >= 0) place(w);
        w.bumpCool = std::max(0.0, w.bumpCool - dt);
        w.grudge *= std::exp(-dt / 30.0);
        lookAtPlayer(w, dt, scene);
    }
    // Fill up to what the hour wants, a couple a frame.
    int missing = wanted_ - live(), tries = 2;
    for (size_t i = 0; i < walkers_.size() && missing > 0 && tries > 0; ++i) {
        if (walkers_[i].alive || int(i) >= wanted_) continue;
        --tries;
        if (spawn(i, scene)) --missing;
    }
}

}  // namespace r1
