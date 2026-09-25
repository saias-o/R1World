#include "crowd.hpp"

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
constexpr double kSampleStep = 5.0;       // metres between samples of a walked line
constexpr double kWeld = 0.75;      // two samples closer than this are one node
constexpr double kReach = 14.0;     // longest corner or crossing joined at a loose end
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
        if (taggedYes(tags, "tunnel") || taggedYes(tags, "bridge")) continue;
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

    // How many people this much street asks for.
    int busy = 0, allBuildings = int(buildings.size());
    for (const OsmWay* b : buildings)
        busy += kBusyBuilding.count(tagOr(b->tags, "building")) || has(b->tags, "shop") || has(b->tags, "amenity") ||
                has(b->tags, "office");
    int stops = 0, lights = 0;
    for (const OsmNode& f : features) {
        const std::string h = tagOr(f.tags, "highway");
        stops += h == "bus_stop";
        lights += h == "traffic_signals" || h == "crossing";
    }
    const double walked = sidewalkMetres + footMetres;
    const double asked = walked < 50 ? 0.0
                         : walked / 1000.0 * 4.0 + footMetres / 1000.0 * 4.0 + busy * 0.6 + stops * 2.0 + lights * 0.3 +
                               allBuildings / 40.0;
    const int people = int(std::min<double>(kCrowdTileCeiling, pyround(asked)));

    nlohmann::json nodeList = nlohmann::json::array();
    for (const P3& n : nodes) nodeList.push_back({pyround(n.x, 2), pyround(n.y, 2), pyround(n.z, 2)});
    return {{"revision", kCrowdRevision}, {"nodes", nodeList}, {"links", links}, {"seats", seats},
            {"people", people},
            {"inputs", {{"sidewalkMetres", pyround(sidewalkMetres, 1)}, {"footwayMetres", pyround(footMetres, 1)},
                        {"busyBuildings", busy}, {"buildings", allBuildings}, {"busStops", stops},
                        {"crossingsAndSignals", lights}, {"crossingsJoined", crossings}}},
            {"inferred", "density from the tile's content, before the hour; paths from OSM sidewalks and footways"}};
}

// ── run time ────────────────────────────────────────────────────────────────

WalkGraph WalkGraph::from(const nlohmann::json& crowd) {
    WalkGraph g;
    if (!crowd.is_object()) return g;
    for (const auto& n : crowd.value("nodes", nlohmann::json::array()))
        g.nodes.push_back({n[0].get<double>(), n[1].get<double>(), n[2].get<double>(), {}});
    for (const auto& l : crowd.value("links", nlohmann::json::array())) {
        Link link{l[0].get<int>(), l[1].get<int>(), l[2].get<int>() != 0, 0.0};
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
    }
    return "idle";
}

void Crowd::reset(const WalkGraph* graph, uint32_t seed, std::vector<CrowdPace> pace) {
    graph_ = graph;
    pace_ = pace.empty() ? std::vector<CrowdPace>{CrowdPace{}} : std::move(pace);
    walkers_.clear();
    seatTaken_.assign(graph ? graph->seats.size() : 0, -1);
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
    w.x = from.x + (to.x - from.x) * t - dz * w.side;
    w.z = from.z + (to.z - from.z) * t + dx * w.side;
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
    for (int l : options) if (l != came || options.size() == 1) total += g.links[l].crossing ? 0.35 : 1.0;
    double pick = random() * total;
    int next = options.empty() ? -1 : options.front();
    for (int l : options) {
        if (l == came && options.size() > 1) continue;
        pick -= g.links[l].crossing ? 0.35 : 1.0;
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
    // On a bench, now and then.
    if (!g.seats.empty() && random() < 0.12) {
        const int s = int(random() * g.seats.size()) % int(g.seats.size());
        if (seatTaken_[s] < 0 && acceptable(g.seats[s].x, g.seats[s].z)) {
            w = Walker{};
            w.alive = true; w.activity = Activity::Sit; w.seat = s; w.timer = 25.0 + 60.0 * random();
            seatTaken_[s] = int(slot);
            place(w);
            return true;
        }
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        const int l = int(random() * g.links.size()) % int(g.links.size());
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
                // Somebody standing in the way -- the player -- is waited for.
                const double px = scene.playerX - w.x, pz = scene.playerZ - w.z;
                const double hx = std::sin(w.heading), hz = -std::cos(w.heading);
                const double ahead = px * hx + pz * hz;
                if (w.activity == Activity::Walk && ahead > 0 && ahead < 1.6 && std::abs(px * hz - pz * hx) < 0.7) {
                    w.activity = Activity::Idle;
                    w.timer = 1.5;
                    break;
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
        if (w.alive && w.link >= 0) place(w);
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
