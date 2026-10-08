#include "bridges.hpp"

#include "clip.hpp"
#include "palette.hpp"
#include "roadnet.hpp"
#include "road_details.hpp"
#include "streets.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <queue>
#include <set>
#include <unordered_map>

namespace r1 {

double GroundField::at(double lon, double lat) const {
    auto holds = [&](const ElevationGrid& g) {
        return g.bounds.west <= lon && lon <= g.bounds.east && g.bounds.south <= lat && lat <= g.bounds.north;
    };
    if (holds(own_)) return own_.sample(lon, lat);
    for (const ElevationGrid& g : around_) if (holds(g)) return g.sample(lon, lat);
    return own_.sample(lon, lat);
}

namespace {
constexpr double kGabarit = 4.75;        // over a road: the French minimum, and Europe's commonest
constexpr double kFootClearance = 2.6;   // over a path
constexpr double kDeck = 1.0, kFootDeck = 0.6;  // structure under the road surface
constexpr double kDeckStep = 10.0;       // a deck is solved and drawn every 10 m at most
constexpr double kRampStep = 8.0;        // and an embankment every 8 m
constexpr double kSlope = 1.5;           // embankment slopes: 2 in 3
constexpr double kMargin = 0.012;        // degrees of latitude solved around the tile
constexpr double kRaisedFrom = 0.05;     // a road this far above the ground is on an embankment
// How much a deck may rise above the level of its own ends to clear a road:
// a predicted crossing and anything over an expressway climb on embankments;
// a surveyed bridge meets its streets where OSM drew it, and the ground under
// it is dug for the rest.
constexpr double kCapPredicted = 7.5, kCapSurveyed = 2.5;

bool footway(const std::string& highway) {
    return !isMotorway(highway) && highway.find("_link") == std::string::npos && highway != "road";
}
double gradeOf(const std::string& highway) {
    if (highway == "steps") return 0.6;
    if (footway(highway) || highway == "service") return 0.08;
    if (highway.find("_link") != std::string::npos) return 0.06;
    return 0.05;
}

struct Work {
    const OsmWay* osm = nullptr;
    Tags tags;
    std::string highway;
    bool relevant = false, tunnel = false, tagged = false;
    int layer = 0;
    std::vector<P2> points;
    std::vector<char> bridge, predicted;  // per segment
    std::vector<int> vertex;              // per point
};

struct Edge {
    int u, v, way, seg;
    double length, grade, thickness;
    bool bridge, foot;
    int layer;
};

struct Obstacle {
    int over, under;  // edges
    double t, u;      // where, along each
    P2 xz, lonlat;
    double cap, d0 = 0;
};

P2 lerp(P2 a, P2 b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
}  // namespace

std::optional<double> GradePlan::levelAt(P2 p) const {
    if (auto it = levels.find(p); it != levels.end()) return it->second;
    for (const RaisedRun& r : runs)
        for (size_t i = 0; i + 1 < r.points.size(); ++i) {
            const P2 a = r.points[i], b = r.points[i + 1];
            if (p.x < std::min(a.x, b.x) - 1e-9 || p.x > std::max(a.x, b.x) + 1e-9 ||
                p.y < std::min(a.y, b.y) - 1e-9 || p.y > std::max(a.y, b.y) + 1e-9)
                continue;
            const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
            if (l2 <= 0) continue;
            const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0);
            if (std::hypot(a.x + dx * t - p.x, a.y + dy * t - p.y) > 1e-8) continue;
            return r.levels[i] * (1 - t) + r.levels[i + 1] * t;
        }
    return std::nullopt;
}

std::optional<double> GradePlan::levelNear(P2 p, double reach) const {
    const double kx = kMetresPerDegree * std::cos(radians(p.y)), ky = kMetresPerDegree;
    std::optional<double> best;
    double nearest = reach;
    for (const RaisedRun& r : runs)
        for (size_t i = 0; i + 1 < r.points.size(); ++i) {
            const P2 a{(r.points[i].x - p.x) * kx, (r.points[i].y - p.y) * ky};
            const P2 b{(r.points[i + 1].x - p.x) * kx, (r.points[i + 1].y - p.y) * ky};
            if (std::min(a.x, b.x) > reach || std::max(a.x, b.x) < -reach || std::min(a.y, b.y) > reach || std::max(a.y, b.y) < -reach)
                continue;
            const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
            const double t = l2 > 0 ? std::clamp(-(a.x * dx + a.y * dy) / l2, 0.0, 1.0) : 0.0;
            const double d = std::hypot(a.x + dx * t, a.y + dy * t);
            if (d < nearest) { nearest = d; best = r.levels[i] * (1 - t) + r.levels[i + 1] * t; }
        }
    return best;
}

// ── the profile ─────────────────────────────────────────────────────────────

GradePlan planGrades(const OsmData& osm, const std::vector<Prediction>& structures, const GroundField& field,
                     const Anchor& anchor, const Bounds& b) {
    GradePlan plan;
    auto xz = [&](P2 p) { const P3 e = anchor.toEngine(p.x, p.y, 0.0); return P2{e.x, e.z}; };
    const double mLon = kMargin / std::max(0.05, std::cos(radians((b.south + b.north) / 2)));

    // 1. The roads near the tile, their segments marked bridge or not.
    std::vector<Work> works(osm.roads.size());
    std::map<int64_t, size_t> byId;
    for (size_t i = 0; i < osm.roads.size(); ++i) {
        const OsmWay& road = osm.roads[i];
        Work& w = works[i];
        w.osm = &road;
        w.tags = road.tags;
        w.highway = tagOr(road.tags, "highway");
        w.points = road.points;
        w.tunnel = taggedYes(road.tags, "tunnel") || tagOr(road.tags, "covered") == "yes";
        w.tagged = taggedYes(road.tags, "bridge");
        w.layer = std::atoi(tagOr(road.tags, "layer", w.tagged ? "1" : "0").c_str());
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        for (const P2& p : road.points) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
        w.relevant = tagOr(road.tags, "area") != "yes" && !w.tunnel && road.points.size() >= 2 &&
                     x1 >= b.west - mLon && x0 <= b.east + mLon && y1 >= b.south - kMargin && y0 <= b.north + kMargin;
        w.bridge.assign(road.points.size() - 1, w.tagged);
        w.predicted.assign(road.points.size() - 1, 0);
        byId[road.id] = i;
    }

    // 2. The predicted crossings become spans of their upper road.
    int predictedSpans = 0;
    for (const Prediction& p : structures) {
        if (p.what != "bridge") continue;
        auto it = byId.find(p.detail.value("over", int64_t(0)));
        if (it == byId.end()) continue;
        Work& w = works[it->second];
        if (!w.relevant) continue;
        std::vector<P2> line;
        for (const P2& q : w.points) line.push_back(xz(q));
        const double at = clip::project(line, p.xz), half = p.detail.value("span", 12.0) / 2 + 3.0;
        const double s0 = std::max(0.0, at - half), s1 = std::min(clip::length(line), at + half);
        // Cut the way at s0 and s1, then mark what lies between.
        auto cut = [&](double s) {
            double walked = 0;
            for (size_t i = 0; i + 1 < w.points.size(); ++i) {
                const double l = dist(line[i], line[i + 1]);
                if (s <= walked + 1e-6) return;
                if (s < walked + l - 1e-6) {
                    const double t = (s - walked) / l;
                    w.points.insert(w.points.begin() + long(i) + 1, lerp(w.points[i], w.points[i + 1], t));
                    line.insert(line.begin() + long(i) + 1, lerp(line[i], line[i + 1], t));
                    w.bridge.insert(w.bridge.begin() + long(i), w.bridge[i]);
                    w.predicted.insert(w.predicted.begin() + long(i), w.predicted[i]);
                    return;
                }
                walked += l;
            }
        };
        cut(s0);
        cut(s1);
        double walked = 0;
        for (size_t i = 0; i + 1 < w.points.size(); ++i) {
            const double l = dist(line[i], line[i + 1]), mid = walked + l / 2;
            if (mid > s0 && mid < s1 && !w.bridge[i]) { w.bridge[i] = 1; w.predicted[i] = 1; }
            walked += l;
        }
        w.layer = std::max(w.layer, 1);
        ++predictedSpans;
    }

    // 3. Decks every 10 m, so a deck bends where it is pushed.
    for (Work& w : works) {
        if (!w.relevant) continue;
        std::vector<P2> pts{w.points.front()};
        std::vector<char> br, pr;
        for (size_t i = 0; i + 1 < w.points.size(); ++i) {
            const int n = w.bridge[i] ? std::max(1, int(std::ceil(dist(xz(w.points[i]), xz(w.points[i + 1])) / kDeckStep))) : 1;
            for (int k = 1; k <= n; ++k) {
                pts.push_back(k == n ? w.points[i + 1] : lerp(w.points[i], w.points[i + 1], double(k) / n));
                br.push_back(w.bridge[i]);
                pr.push_back(w.predicted[i]);
            }
        }
        w.points = std::move(pts);
        w.bridge = std::move(br);
        w.predicted = std::move(pr);
    }

    // 4. The graph, welded on OSM's shared nodes.
    std::vector<P2> vll, vxz;
    std::vector<double> ground;
    std::map<P2, int> index;
    std::vector<Edge> edges;
    std::vector<std::vector<int>> at;
    for (size_t wi = 0; wi < works.size(); ++wi) {
        Work& w = works[wi];
        if (!w.relevant) continue;
        for (const P2& p : w.points) {
            auto it = index.find(p);
            int v;
            if (it == index.end()) {
                v = int(vll.size());
                index[p] = v;
                vll.push_back(p);
                vxz.push_back(xz(p));
                ground.push_back(field.at(p.x, p.y));
                at.emplace_back();
            } else v = it->second;
            w.vertex.push_back(v);
        }
        const bool foot = footway(w.highway);
        for (size_t i = 0; i + 1 < w.points.size(); ++i) {
            const int u = w.vertex[i], v = w.vertex[i + 1];
            if (u == v) continue;
            const int e = int(edges.size());
            edges.push_back({u, v, int(wi), int(i), dist(vxz[size_t(u)], vxz[size_t(v)]),
                             gradeOf(w.highway), foot ? kFootDeck : kDeck, bool(w.bridge[i]), foot, w.layer});
            at[size_t(u)].push_back(e);
            at[size_t(v)].push_back(e);
        }
    }
    const size_t nv = vll.size();
    std::vector<char> onGround(nv, 0), interior(nv, 0), pinned(nv, 0);
    for (size_t v = 0; v < nv; ++v) {
        int bridges = 0;
        for (int e : at[v]) {
            if (edges[size_t(e)].bridge) ++bridges;
            else onGround[v] = 1;
        }
        if (at[v].size() == 1) onGround[v] = 1;
        interior[v] = !onGround[v] && bridges == 2;
    }

    // 5. Chains: a deck's vertices between the two points it stands on.
    struct Chain { std::vector<int> vertices; std::vector<double> along; };
    std::vector<Chain> chains;
    std::vector<char> seen(edges.size(), 0);
    for (size_t v = 0; v < nv; ++v) {
        if (interior[v]) continue;
        for (int first : at[v]) {
            if (!edges[size_t(first)].bridge || seen[size_t(first)]) continue;
            Chain c{{int(v)}, {0.0}};
            int e = first, cur = int(v);
            for (;;) {
                seen[size_t(e)] = 1;
                const Edge& ed = edges[size_t(e)];
                const int next = ed.u == cur ? ed.v : ed.u;
                c.vertices.push_back(next);
                c.along.push_back(c.along.back() + ed.length);
                if (!interior[size_t(next)]) break;
                int onward = -1;
                for (int o : at[size_t(next)]) if (o != e && edges[size_t(o)].bridge) onward = o;
                if (onward < 0 || seen[size_t(onward)]) break;
                e = onward;
                cur = next;
            }
            chains.push_back(std::move(c));
        }
    }

    // Decks mapped side by side -- two carriageways and a footway on one
    // bridge -- are one structure at one level: their vertices facing each
    // other across less than 15 m are held level with each other.
    std::vector<std::pair<int, int>> abreast;
    {
        std::vector<int> chainOf(nv, -1);
        std::vector<P2> along(nv, P2{0, 0});
        for (size_t ci = 0; ci < chains.size(); ++ci) {
            const auto& vs = chains[ci].vertices;
            for (size_t k = 0; k < vs.size(); ++k) {
                if (chainOf[size_t(vs[k])] < 0) chainOf[size_t(vs[k])] = int(ci);
                const P2 a = vxz[size_t(vs[k ? k - 1 : 0])], b = vxz[size_t(vs[std::min(k + 1, vs.size() - 1)])];
                const double l = std::max(1e-9, dist(a, b));
                along[size_t(vs[k])] = {(b.x - a.x) / l, (b.y - a.y) / l};
            }
        }
        constexpr double kAbreast = 15.0;
        std::map<std::pair<long, long>, std::vector<int>> cells;
        for (size_t v = 0; v < nv; ++v)
            if (chainOf[v] >= 0)
                cells[{long(std::floor(vxz[v].x / kAbreast)), long(std::floor(vxz[v].y / kAbreast))}].push_back(int(v));
        for (const auto& [cell, list] : cells)
            for (int a : list)
                for (long dx = -1; dx <= 1; ++dx)
                    for (long dz = -1; dz <= 1; ++dz) {
                        auto it = cells.find({cell.first + dx, cell.second + dz});
                        if (it == cells.end()) continue;
                        for (int c : it->second) {
                            if (c <= a || chainOf[size_t(c)] == chainOf[size_t(a)]) continue;
                            const P2 d{vxz[size_t(c)].x - vxz[size_t(a)].x, vxz[size_t(c)].y - vxz[size_t(a)].y};
                            const double l = std::hypot(d.x, d.y), t = along[size_t(a)].x * d.x + along[size_t(a)].y * d.y;
                            if (l < kAbreast && std::abs(t) < 0.5 * l) abreast.push_back({a, c});
                        }
                    }
    }

    // 6. What each deck passes over.
    std::vector<Obstacle> obstacles;
    {
        constexpr double kCell = 32.0;
        std::unordered_map<int64_t, std::vector<int>> grid;
        auto key = [](long x, long z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); };
        for (int e = 0; e < int(edges.size()); ++e) {
            const P2 a = vxz[size_t(edges[size_t(e)].u)], c = vxz[size_t(edges[size_t(e)].v)];
            for (long x = long(std::floor(std::min(a.x, c.x) / kCell)); x <= long(std::floor(std::max(a.x, c.x) / kCell)); ++x)
                for (long z = long(std::floor(std::min(a.y, c.y) / kCell)); z <= long(std::floor(std::max(a.y, c.y) / kCell)); ++z)
                    grid[key(x, z)].push_back(e);
        }
        std::vector<std::pair<int64_t, std::vector<int>*>> cells;
        for (auto& [k, list] : grid) cells.push_back({k, &list});
        std::sort(cells.begin(), cells.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
        std::set<std::pair<int, int>> pairs;
        for (const auto& [k, list] : cells)
            for (int e : *list) {
                const Edge& E = edges[size_t(e)];
                if (!E.bridge) continue;
                for (int f : *list) {
                    const Edge& F = edges[size_t(f)];
                    if (f == e || F.u == E.u || F.u == E.v || F.v == E.u || F.v == E.v) continue;
                    if (F.bridge && F.layer >= E.layer) continue;
                    if (!pairs.insert({e, f}).second) continue;
                    const P2 p = vxz[size_t(E.u)], r{vxz[size_t(E.v)].x - p.x, vxz[size_t(E.v)].y - p.y};
                    const P2 q = vxz[size_t(F.u)], s{vxz[size_t(F.v)].x - q.x, vxz[size_t(F.v)].y - q.y};
                    const double den = r.x * s.y - r.y * s.x;
                    if (std::abs(den) < 1e-9) continue;
                    const double t = ((q.x - p.x) * s.y - (q.y - p.y) * s.x) / den;
                    const double u = ((q.x - p.x) * r.y - (q.y - p.y) * r.x) / den;
                    // Half-open, so a crossing on a shared vertex is counted once.
                    if (t < 0 || t >= 1 || u < 0 || u >= 1) continue;
                    const Work& we = works[size_t(E.way)];
                    const Work& wf = works[size_t(F.way)];
                    const bool predicted = we.predicted[size_t(E.seg)];
                    const double cap = predicted || roadRank(we.highway) >= 5 || roadRank(wf.highway) >= 5 ? kCapPredicted : kCapSurveyed;
                    obstacles.push_back({e, f, t, u, {p.x + r.x * t, p.y + r.y * t},
                                         lerp(vll[size_t(E.u)], vll[size_t(E.v)], t), cap});
                    // The road beneath keeps to the ground near the crossing.
                    if (!F.bridge)
                        for (int w : {F.u, F.v})
                            if (dist(vxz[size_t(w)], obstacles.back().xz) < 40.0) pinned[size_t(w)] = 1;
                }
            }
    }

    // 7. Solve: ground, straight decks, clearances, and ramps between them.
    std::vector<double> h = ground;
    auto level = [&](int e, double t) { const Edge& E = edges[size_t(e)]; return h[size_t(E.u)] * (1 - t) + h[size_t(E.v)] * t; };
    auto baseline = [&](bool first) {
        for (const Chain& c : chains) {
            const double h0 = h[size_t(c.vertices.front())], h1 = h[size_t(c.vertices.back())], length = c.along.back();
            for (size_t k = 1; k + 1 < c.vertices.size(); ++k) {
                const size_t v = size_t(c.vertices[k]);
                if (!interior[v]) continue;
                const double line = h0 + (h1 - h0) * (length > 0 ? c.along[k] / length : 0);
                h[v] = first ? line : std::max(h[v], line);
            }
        }
    };
    std::priority_queue<std::pair<double, int>> queue;
    auto propagate = [&] {
        while (!queue.empty()) {
            const auto [value, v] = queue.top();
            queue.pop();
            if (value < h[size_t(v)] - 1e-9) continue;
            for (int e : at[size_t(v)]) {
                const Edge& E = edges[size_t(e)];
                const int w = E.u == v ? E.v : E.u;
                if (pinned[size_t(w)] && !E.bridge) continue;
                const double candidate = h[size_t(v)] - E.grade * E.length;
                if (candidate > h[size_t(w)] + 1e-4) {
                    h[size_t(w)] = candidate;
                    queue.push({candidate, w});
                }
            }
        }
    };
    baseline(true);
    for (Obstacle& o : obstacles) o.d0 = level(o.over, o.t);
    for (int pass = 0; pass < 6; ++pass) {
        if (pass) baseline(false);
        for (const Obstacle& o : obstacles) {
            const Edge& E = edges[size_t(o.over)];
            const Edge& F = edges[size_t(o.under)];
            const double need = level(o.under, o.u) + (F.foot ? kFootClearance : kGabarit) + E.thickness;
            const double target = std::min(need, o.d0 + o.cap);
            if (level(o.over, o.t) >= target - 1e-3) continue;
            for (int v : {E.u, E.v})
                if (h[size_t(v)] < target) { h[size_t(v)] = target; queue.push({target, v}); }
        }
        for (const auto& [a, c] : abreast) {
            const double level = std::max(h[size_t(a)], h[size_t(c)]);
            for (int v : {a, c})
                if (h[size_t(v)] < level - 1e-4) { h[size_t(v)] = level; queue.push({level, v}); }
        }
        propagate();
    }

    // 8. What the deck could not give, the ground gives.
    double deepest = 0;
    for (const Obstacle& o : obstacles) {
        const Edge& E = edges[size_t(o.over)];
        const Edge& F = edges[size_t(o.under)];
        const double need = level(o.under, o.u) + (F.foot ? kFootClearance : kGabarit) + E.thickness;
        const double deficit = need - level(o.over, o.t);
        const double half = roadWidth(works[size_t(F.way)].tags) / 2;
        plan.underneath.push_back({lerp(vll[size_t(F.u)], vll[size_t(F.v)], o.u), half});
        if (deficit > 0.05) {
            plan.carves.push_back({lerp(vll[size_t(F.u)], vll[size_t(F.v)], o.u), deficit + 0.3, 26.0});
            deepest = std::max(deepest, deficit + 0.3);
        }
    }
    for (size_t v = 0; v < nv; ++v) {
        if (!interior[v]) continue;
        double thickness = kDeck;
        for (int e : at[v]) thickness = edges[size_t(e)].thickness;
        const double room = h[v] - thickness - ground[v];
        if (room < 0.8) {
            plan.carves.push_back({vll[v], 1.2 - room, 10.0});
            deepest = std::max(deepest, 1.2 - room);
        }
    }

    // 9. The roads, cut where they leave the ground; the runs to build.
    double raisedMetres = 0, bridgeMetres = 0, highest = 0;
    int surveyedRuns = 0, predictedRuns = 0;
    for (size_t wi = 0; wi < works.size(); ++wi) {
        const Work& w = works[wi];
        if (!w.relevant) { plan.roads.push_back(*w.osm); continue; }
        // 0 ground, 1 embankment, 2 bridge.
        std::vector<int> kind(w.points.size() - 1, 0);
        for (size_t i = 0; i + 1 < w.points.size(); ++i) {
            const size_t u = size_t(w.vertex[i]), v = size_t(w.vertex[i + 1]);
            if (w.bridge[i]) kind[i] = 2;
            else if (h[u] > ground[u] + kRaisedFrom || h[v] > ground[v] + kRaisedFrom) kind[i] = 1;
        }
        const double grade = gradeOf(w.highway);
        size_t i = 0;
        while (i < kind.size()) {
            size_t j = i;
            while (j + 1 < kind.size() && kind[j + 1] == kind[i] &&
                   (kind[i] != 2 || w.predicted[j + 1] == w.predicted[i]))
                ++j;
            OsmWay piece{w.osm->id, {}, w.tags};
            RaisedRun run;
            run.way = w.osm->id;
            run.bridge = kind[i] == 2;
            run.predicted = run.bridge && w.predicted[i];
            auto add = [&](P2 p, double lv, double g) {
                piece.points.push_back(p);
                if (kind[i]) { run.points.push_back(p); run.levels.push_back(lv); run.grounds.push_back(g); }
            };
            for (size_t k = i; k <= j; ++k) {
                const size_t u = size_t(w.vertex[k]), v = size_t(w.vertex[k + 1]);
                if (k == i) add(w.points[k], h[u], ground[u]);
                if (kind[k] == 1) {
                    // Between two points an embankment follows the ground or
                    // the ramp down from either end, whichever is higher.
                    const double length = dist(vxz[u], vxz[v]);
                    const int n = std::max(1, int(std::ceil(length / kRampStep)));
                    for (int s = 1; s < n; ++s) {
                        const double t = double(s) / n;
                        const P2 p = lerp(w.points[k], w.points[k + 1], t);
                        const double g = field.at(p.x, p.y);
                        add(p, std::max({g, h[u] - grade * length * t, h[v] - grade * length * (1 - t)}), g);
                    }
                }
                add(w.points[k + 1], h[v], ground[v]);
                if (kind[k] == 1) raisedMetres += dist(vxz[u], vxz[v]);
                if (kind[k] == 2) bridgeMetres += dist(vxz[u], vxz[v]);
            }
            if (kind[i] == 2) {
                piece.tags["bridge"] = tagOr(w.tags, "bridge", "yes");
                if (run.predicted) piece.tags["r1:bridge"] = "predicted";
                run.abutStart = onGround[size_t(w.vertex[i])] && run.levels.front() - run.grounds.front() > 0.8;
                run.abutEnd = onGround[size_t(w.vertex[j + 1])] && run.levels.back() - run.grounds.back() > 0.8;
                ++(run.predicted ? predictedRuns : surveyedRuns);
            }
            if (kind[i] == 1) piece.tags["r1:raised"] = "yes";
            if (kind[i]) {
                run.tags = piece.tags;
                for (size_t k = 0; k < run.points.size(); ++k) {
                    plan.levels[run.points[k]] = run.levels[k];
                    highest = std::max(highest, run.levels[k] - run.grounds[k]);
                }
                plan.runs.push_back(std::move(run));
            }
            plan.roads.push_back(std::move(piece));
            i = j + 1;
        }
    }
    plan.stats = {{"revision", kBridgeRevision}, {"surveyedBridgeRuns", surveyedRuns}, {"predictedBridgeRuns", predictedRuns},
                  {"predictedSpans", predictedSpans}, {"obstaclesCleared", obstacles.size()},
                  {"bridgeMetres", pyround(bridgeMetres, 1)}, {"embankmentMetres", pyround(raisedMetres, 1)},
                  {"highestAboveGround", pyround(highest, 2)}, {"groundDug", plan.carves.size()},
                  {"deepestDig", pyround(deepest, 2)}, {"clearance", kGabarit}, {"maxGrade", 0.05}};
    return plan;
}

// ── the relief ──────────────────────────────────────────────────────────────

ElevationGrid carvedGround(const ElevationGrid& grid, const GradePlan& plan) {
    // At the terrain mesh's own resolution: a dig is a few cells wide, and a
    // coarser source grid would dig a valley.
    const int n = kTerrainMeshSize;
    ElevationGrid out{grid.bounds, n, {}};
    out.values.reserve(size_t(n * n));
    const double kx = kMetresPerDegree * std::cos(radians((grid.bounds.south + grid.bounds.north) / 2)), ky = kMetresPerDegree;
    auto depth = [&](double lon, double lat) {
        double dig = 0;
        for (const GradePlan::Carve& c : plan.carves) {
            const double d = std::hypot((lon - c.at.x) * kx, (lat - c.at.y) * ky);
            if (d >= c.radius) continue;
            const double core = c.radius * 0.35;
            dig = std::max(dig, c.depth * (d <= core ? 1.0 : (c.radius - d) / (c.radius - core)));
        }
        return dig;
    };
    for (int row = 0; row < n; ++row)
        for (int col = 0; col < n; ++col) {
            const double lon = grid.bounds.west + (grid.bounds.east - grid.bounds.west) * col / (n - 1);
            const double lat = grid.bounds.south + (grid.bounds.north - grid.bounds.south) * row / (n - 1);
            out.values.push_back(grid.sample(lon, lat) - depth(lon, lat));
        }
    out.southEdge = grid.southEdge; out.northEdge = grid.northEdge;
    for (auto* edge : {&out.southEdge, &out.northEdge})
        for (auto& p : *edge) p.y -= depth(p.x, edge == &out.southEdge ? grid.bounds.south : grid.bounds.north);
    return out;
}

// ── the works ───────────────────────────────────────────────────────────────

namespace {
// A quad turned to face `facing`, whichever order its corners came in.
void facing(Mesh& mesh, P3 a, P3 b, P3 c, P3 d, P3 want) {
    const P3 n = faceNormal(a, b, c);
    if (n.x * want.x + n.y * want.y + n.z * want.z < 0) std::swap(b, d);
    mesh.addQuad(a, b, c, d);
}
}  // namespace

BridgeOutput buildBridges(const GradePlan& plan, const Bounds& bounds, const ElevationGrid& carved, const Anchor& anchor,
                          const std::string& country) {
    Mesh road(UvMode::Planar), walk(UvMode::Planar), kerb, structure(UvMode::Slope), earth(UvMode::Slope);
    BridgeOutput out;
    auto owned = [&](P2 p) { return bounds.west <= p.x && p.x < bounds.east && bounds.south <= p.y && p.y < bounds.north; };
    // Within reach of the tile: a piece's width and its slopes (40 m).
    const double reachLat = 40.0 / kMetresPerDegree, reachLon = reachLat / std::max(0.05, std::cos(radians((bounds.south + bounds.north) / 2)));
    auto overTile = [&](P2 p) {
        return bounds.west - reachLon <= p.x && p.x <= bounds.east + reachLon && bounds.south - reachLat <= p.y &&
               p.y <= bounds.north + reachLat;
    };
    auto groundAt = [&](P2 exz) {
        const P3 geo = anchor.toGeodetic(exz.x, 0.0, exz.y);
        return groundPoint(geo.x, geo.y, carved, anchor).y;
    };
    std::vector<RoadAxis> neighbours;
    struct Platform { int64_t id; clip::Paths64 footprint; double x0=1e300,x1=-1e300,z0=1e300,z1=-1e300,minY=1e300; };
    std::vector<Platform> platforms;
    for(const auto& way:plan.roads) {
        RoadAxis axis;axis.id=way.id;axis.tags=way.tags;axis.bridge=taggedYes(way.tags,"bridge");
        std::vector<P2> line;
        Platform platform{way.id,{}};
        const double half=roadWidth(way.tags)/2;
        for(const auto& p:way.points) {
            const auto point=anchor.toEngine(p.x,p.y,plan.levelAt(p).value_or(carved.sample(p.x,p.y)));
            axis.points.push_back(point);line.push_back({point.x,point.z});
            platform.x0=std::min(platform.x0,point.x-half);platform.x1=std::max(platform.x1,point.x+half);
            platform.z0=std::min(platform.z0,point.z-half);platform.z1=std::max(platform.z1,point.z+half);
            platform.minY=std::min(platform.minY,point.y);
        }
        if(line.size()>1 && !axis.bridge && isMotorway(tagOr(way.tags,"highway")) && !taggedYes(way.tags,"tunnel")) {
            platform.footprint=clip::bufferLineRoundJoins(line,half+.15);
            platforms.push_back(std::move(platform));
        }
        neighbours.push_back(std::move(axis));
    }
    auto earthFace=[&](int64_t own,P3 a,P3 b,P3 c,P3 d,P3 normal) {
        const double x0=std::min({a.x,b.x,c.x,d.x}),x1=std::max({a.x,b.x,c.x,d.x});
        const double z0=std::min({a.z,b.z,c.z,d.z}),z1=std::max({a.z,b.z,c.z,d.z});
        const double y1=std::max({a.y,b.y,c.y,d.y});
        clip::Paths64 exclusions;
        for(const auto& p:platforms) {
            if(p.id==own || p.x1<x0 || p.x0>x1 || p.z1<z0 || p.z0>z1 || p.minY>y1+.2)continue;
            exclusions.insert(exclusions.end(),p.footprint.begin(),p.footprint.end());
        }
        if(exclusions.empty()){facing(earth,a,b,c,d,normal);return;}
        exclusions=clip::unite(exclusions);
        for(const auto& tri:{std::array<P3,3>{a,b,c},std::array<P3,3>{a,c,d}}) {
            const P3 n=faceNormal(tri[0],tri[1],tri[2]);
            if(std::abs(n.y)<1e-6){earth.addTriangle(tri[0],tri[1],tri[2]);continue;}
            const clip::Paths64 shape{clip::kMetres.path({{tri[0].x,tri[0].z},{tri[1].x,tri[1].z},{tri[2].x,tri[2].z}})};
            for(const auto& polygon:clip::polygons(clip::subtract(shape,exclusions)))
                for(const auto& cut:clip::triangles(polygon)) {
                    auto point=[&](P2 q){return P3{q.x,tri[0].y-(n.x*(q.x-tri[0].x)+n.z*(q.y-tri[0].z))/n.y,q.y};};
                    earth.addUpTriangle(point(cut[0]),point(cut[1]),point(cut[2]));
                }
        }
    };
    std::vector<P2> under;
    std::vector<double> underHalf;
    for (const auto& [p, half] : plan.underneath) {
        const P3 e = anchor.toEngine(p.x, p.y, 0.0);
        under.push_back({e.x, e.z});
        underHalf.push_back(half);
    }
    // A road's cross-section: the carriageway, then on each side (right,
    // left) a band -- its pavement, or a hard strip.
    struct Section { double half = 0, band[2] = {0.5, 0.5}; bool pavement[2] = {false, false}, foot = false; };
    auto sectionOf = [](const RaisedRun& run) {
        Section c;
        c.foot = footway(tagOr(run.tags, "highway"));
        c.half = roadWidth(run.tags) / 2;
        if (c.foot) c.band[0] = c.band[1] = 0.15;
        else if (roadProfile(run.tags).express) c.band[0] = c.band[1] = 0;
        else
            for (const auto& [side, inferred] : sidewalkSides(run.tags)) {
                const int s = side == "right" ? 0 : 1;
                c.band[s] = lengthTag(tag(run.tags, ("sidewalk:" + side + ":width").c_str()), lengthTag(tag(run.tags, "sidewalk:width"), 1.8));
                c.pavement[s] = true;
            }
        return c;
    };
    // The axes of every deck, so decks mapped side by side -- a bridge drawn
    // as two carriageways and a footway -- are built as one.
    struct DeckAxis { std::vector<P2> xz; std::vector<double> y; double reach = 0; bool bridge=false, median=false; };
    std::vector<DeckAxis> decks(plan.runs.size());
    for (size_t r = 0; r < plan.runs.size(); ++r) {
        const auto& run=plan.runs[r];
        const auto profile=roadProfile(run.tags);
        decks[r].bridge=run.bridge;decks[r].median=profile.express && profile.direction && !profile.link;
        for (size_t i=0;i<run.points.size();++i) { const P2& p=run.points[i];const P3 e = anchor.toEngine(p.x, p.y, run.levels[i]); decks[r].xz.push_back({e.x, e.z});decks[r].y.push_back(e.y); }
        const Section c = sectionOf(plan.runs[r]);
        decks[r].reach = c.half + std::max(c.band[0], c.band[1]);
    }
    int piers = 0, abutments = 0, pieces = 0, joinedSides = 0, medianSides=0;
    for (size_t r = 0; r < plan.runs.size(); ++r) {
        const RaisedRun& run = plan.runs[r];
        const size_t n = run.points.size();
        if (n < 2) continue;
        const Section section = sectionOf(run);
        const bool foot = section.foot;
        const double half = section.half;
        const double* band = section.band;
        const bool* pavement = section.pavement;
        const double thickness = foot ? kFootDeck : kDeck;
        const double parapet = foot ? 1.1 : 1.0, parapetWidth = foot ? 0.12 : 0.3;
        std::vector<P3> axis(n);
        for (size_t i = 0; i < n; ++i) axis[i] = anchor.toEngine(run.points[i].x, run.points[i].y, run.levels[i]);
        std::vector<P2> right(n), dir(n - 1);
        std::vector<double> miter(n, 1.0);
        for (size_t i = 0; i + 1 < n; ++i) {
            const double dx = axis[i + 1].x - axis[i].x, dz = axis[i + 1].z - axis[i].z, l = std::max(1e-9, std::hypot(dx, dz));
            dir[i] = {dx / l, dz / l};
        }
        for (size_t i = 0; i < n; ++i) {
            const P2 a = dir[i ? i - 1 : 0], c = dir[std::min(i, n - 2)];
            const P2 ra{-a.y, a.x}, rc{-c.y, c.x};
            P2 m{ra.x + rc.x, ra.y + rc.y};
            const double l = std::hypot(m.x, m.y);
            m = l > 1e-9 ? P2{m.x / l, m.y / l} : rc;
            right[i] = m;
            miter[i] = 1.0 / std::max(0.4, m.x * rc.x + m.y * rc.y);
        }
        // A point of the cross-section: `offset` metres right of the axis.
        auto at = [&](size_t i, double offset, double dy) {
            const double k = offset * miter[i];
            return P3{axis[i].x + right[i].x * k, axis[i].y + dy, axis[i].z + right[i].y * k};
        };
        const double outer[2] = {half + band[0], half + band[1]};
        // Where another deck runs alongside, the two are one deck: the gap is
        // decked over, and neither has a parapet or a cornice on that side.
        std::vector<double> reach[2] = {std::vector<double>(n, outer[0]), std::vector<double>(n, outer[1])};
        std::vector<double> middleHeight[2]={std::vector<double>(n),std::vector<double>(n)};
        std::vector<char> joined[2] = {std::vector<char>(n, 0), std::vector<char>(n, 0)};
        if (run.bridge || decks[r].median)
            for (size_t i = 0; i < n; ++i)
                for (int s = 0; s < 2; ++s) {
                    const double sign = s == 0 ? 1.0 : -1.0;
                    const P2 o{axis[i].x, axis[i].z}, d{right[i].x * sign, right[i].y * sign};
                    double nearest = 1e300, other = 0,otherHeight=0;
                    for (size_t q = 0; q < decks.size(); ++q) {
                        if (q == r || decks[q].xz.size() < 2) continue;
                        if(decks[q].bridge!=run.bridge || (!run.bridge && !decks[q].median))continue;
                        const auto& line = decks[q].xz;
                        for (size_t k = 0; k + 1 < line.size(); ++k) {
                            const P2 a = line[k], e{line[k + 1].x - a.x, line[k + 1].y - a.y};
                            const double den = d.x * e.y - d.y * e.x;
                            if (std::abs(den) < 1e-9) continue;
                            const double t = ((a.x - o.x) * e.y - (a.y - o.y) * e.x) / den;
                            const double u = ((a.x - o.x) * d.y - (a.y - o.y) * d.x) / den;
                            if (u < 0 || u > 1 || t <= 0.1 || t >= nearest) continue;
                            const double length=std::hypot(e.x,e.y);
                            if(length<.01 || std::abs((e.x*dir[std::min(i,n-2)].x+e.y*dir[std::min(i,n-2)].y)/length)<.94)continue;
                            const double height=decks[q].y[k]+(decks[q].y[k+1]-decks[q].y[k])*u;
                            if(std::abs(height-axis[i].y)>(run.bridge?1.5:.75))continue;
                            nearest = t;
                            other = decks[q].reach;
                            otherHeight=height;
                        }
                    }
                    const double gap = nearest - outer[s] - other;
                    if (gap >= (run.bridge?-.5:.25) && gap < (run.bridge?8.0:12.0)) {
                        joined[s][i] = 1;
                        // Past the middle of the gap by a hand's width: the
                        // two fills overlap, and nobody falls between decks.
                        reach[s][i] = outer[s] + std::max(0.0, gap / 2) + 0.25;
                        middleHeight[s][i]=(axis[i].y+otherHeight)/2;
                    }
                }
        // An embankment's slope meets the ground where 2 in 3 down from the
        // shoulder crosses it.
        auto toe = [&](size_t i, int side, double from) {
            const double sign = side == 0 ? 1.0 : -1.0;
            double reach = from;
            for (int it = 0; it < 4; ++it) {
                const P3 p = at(i, sign * reach, 0);
                const double drop = std::max(0.0, axis[i].y - groundAt({p.x, p.z}));
                reach = from + kSlope * drop;
            }
            P3 p = at(i, sign * reach, 0);
            p.y = groundAt({p.x, p.z}) - 0.15;
            return p;
        };
        const P3 down{0, -1, 0};
        // Each side's edge: past the parapet, the fill to a neighbour deck, or
        // an embankment's shoulder.
        auto edge = [&](int s, size_t k) {
            return run.bridge ? reach[s][k] + (joined[s][k] ? 0.0 : parapetWidth) : joined[s][k]?reach[s][k]:outer[s]+0.6;
        };
        for (size_t i = 0; i + 1 < n; ++i) {
            const size_t j = i + 1;
            // What stands over this tile is walkable from it, whichever tile
            // draws it: a tile mounted alone still carries its bridges.
            if (overTile(run.points[i]) || overTile(run.points[j])) {
                const bool rail[2] = {run.bridge && !joined[0][i] && !joined[0][j], run.bridge && !joined[1][i] && !joined[1][j]};
                out.raised.push_back(
                    {{"a", {pyround(axis[i].x, 3), pyround(axis[i].y + .06, 3), pyround(axis[i].z, 3)}},
                     {"b", {pyround(axis[j].x, 3), pyround(axis[j].y + .06, 3), pyround(axis[j].z, 3)}},
                     // Right of the axis (a to b), then left.
                     {"half", {pyround(std::max(edge(0, i), edge(0, j)), 2), pyround(std::max(edge(1, i), edge(1, j)), 2)}},
                     {"rail", {rail[0], rail[1]}},
                     {"solid", !run.bridge}});
            }
            if (!owned(lerp(run.points[i], run.points[j], 0.5))) continue;
            ++pieces;
            road.addUpQuad(at(i, -half, .06), at(i, half, .06), at(j, half, .06), at(j, -half, .06));
            for (int s = 0; s < 2; ++s) {
                const double sign = s == 0 ? 1.0 : -1.0, top = pavement[s] ? .21 : .06;
                const P3 inward{-right[i].x * sign, 0, -right[i].y * sign}, outward{-inward.x, 0, -inward.z};
                Mesh& surface = pavement[s] ? walk : road;
                surface.addUpQuad(at(i, sign * half, top), at(i, sign * reach[s][i], top), at(j, sign * reach[s][j], top), at(j, sign * half, top));
                if (pavement[s])
                    facing(kerb, at(i, sign * half, .06), at(j, sign * half, .06), at(j, sign * half, .21), at(i, sign * half, .21), inward);
                if (run.bridge && (joined[s][i] || joined[s][j])) {
                    ++joinedSides;
                } else if (run.bridge) {
                    const double o = outer[s], p = outer[s] + parapetWidth;
                    facing(structure, at(i, sign * o, top), at(j, sign * o, top), at(j, sign * o, .06 + parapet), at(i, sign * o, .06 + parapet), inward);
                    structure.addUpQuad(at(i, sign * o, .06 + parapet), at(i, sign * p, .06 + parapet), at(j, sign * p, .06 + parapet), at(j, sign * o, .06 + parapet));
                    facing(structure, at(i, sign * p, -thickness), at(j, sign * p, -thickness), at(j, sign * p, .06 + parapet), at(i, sign * p, .06 + parapet), outward);
                } else if(joined[s][i] && joined[s][j]) {
                    // Opposing carriageways at the same level share one median:
                    // no two overlapping triangular embankments in the gap.
                    P3 mi=at(i,sign*reach[s][i],0),mj=at(j,sign*reach[s][j],0);
                    mi.y=middleHeight[s][i];mj.y=middleHeight[s][j];
                    earth.addUpQuad(at(i,sign*outer[s],0),mi,mj,at(j,sign*outer[s],0));
                    ++medianSides;
                } else {
                    // The shoulder, then the slope down to the ground.
                    const double shoulder = outer[s] + 0.6;
                    earth.addUpQuad(at(i, sign * outer[s], 0), at(i, sign * shoulder, 0), at(j, sign * shoulder, 0), at(j, sign * outer[s], 0));
                    facing(kerb, at(i, sign * outer[s], 0), at(j, sign * outer[s], 0), at(j, sign * outer[s], top), at(i, sign * outer[s], top), outward);
                    const P3 ti = at(i, sign * shoulder, 0), tj = at(j, sign * shoulder, 0);
                    const P3 bi = toe(i, s, shoulder), bj = toe(j, s, shoulder);
                    if (ti.y - bi.y > 0.02 || tj.y - bj.y > 0.02)
                        earthFace(run.way,ti,tj,bj,bi,{outward.x,0.7,outward.z});
                }
            }
            if (run.bridge)
                facing(structure, at(i, -edge(1, i), -thickness), at(i, edge(0, i), -thickness),
                       at(j, edge(0, j), -thickness), at(j, -edge(1, j), -thickness), down);
        }
        if (!run.bridge) continue;
        // Abutments: where a deck meets its embankment, a wall closes the
        // embankment's end and carries the deck.
        for (int end = 0; end < 2; ++end) {
            if (!(end == 0 ? run.abutStart : run.abutEnd)) continue;
            const size_t i = end == 0 ? 0 : n - 1;
            if (!owned(run.points[i])) continue;
            const P2 d = end == 0 ? dir[0] : P2{-dir[n - 2].x, -dir[n - 2].y};
            const double wl = outer[1] + parapetWidth, wr = outer[0] + parapetWidth;
            const P3 tl = at(i, -wl, .06), tr = at(i, wr, .06);
            const P3 bl = toe(i, 1, outer[1] + 0.6), br = toe(i, 0, outer[0] + 0.6);
            facing(structure, tl, tr, br, bl, {d.x, 0, d.y});
            ++abutments;
        }
        // Piers under a deck high enough to need them, never on the road below.
        std::vector<double> along{0};
        for (size_t i = 0; i + 1 < n; ++i) along.push_back(along.back() + std::hypot(axis[i + 1].x - axis[i].x, axis[i + 1].z - axis[i].z));
        const double length = along.back();
        const int count = length > 45 ? int(length / 32) : 0;
        for (int k = 1; k <= count; ++k) {
            for (double shift : {0.0, 7.0, -7.0, 14.0, -14.0}) {
                const double s = std::clamp(length * k / (count + 1) + shift, 6.0, length - 6.0);
                size_t i = 0;
                while (i + 2 < n && along[i + 1] < s) ++i;
                const double t = (s - along[i]) / std::max(1e-9, along[i + 1] - along[i]);
                const P3 c{axis[i].x + (axis[i + 1].x - axis[i].x) * t, axis[i].y + (axis[i + 1].y - axis[i].y) * t,
                           axis[i].z + (axis[i + 1].z - axis[i].z) * t};
                bool clear = true;
                for (size_t u = 0; u < under.size(); ++u) clear &= dist({c.x, c.z}, under[u]) > underHalf[u] + 3.0;
                if (!clear) continue;
                const P3 geo = anchor.toGeodetic(c.x, 0, c.z);
                if (owned({geo.x, geo.y})) {
                    const double bottom = groundAt({c.x, c.z}) - 0.5, top = c.y - thickness;
                    if (top - bottom > 2.0) {
                        const double across = std::min(8.0, 0.55 * (outer[0] + outer[1]));
                        structure.addBox({c.x, (top + bottom) / 2, c.z}, {across, top - bottom, 1.4},
                                         std::atan2(dir[i].x, dir[i].y));
                        ++piers;
                    }
                }
                break;
            }
        }
    }
    const Swatch* grass = nullptr;
    for (const auto& c : palette().groundClasses) if (c.name == "grass") grass = &c.swatch;
    struct Spec { const char* name; Mesh* mesh; Material material; bool smooth; };
    const Spec specs[] = {
        {"Bridge carriageway", &road, surfaceMaterial("Carriageway", {0.10, 0.11, 0.12}, 0.88, std::string("asphalt")), true},
        {"Bridge sidewalks", &walk, surfaceMaterial("Sidewalks", {0.22, 0.215, 0.20}, 0.88, std::string("pavement")), true},
        {"Bridge kerbs", &kerb, surfaceMaterial("Kerbs", {0.26, 0.25, 0.23}, 0.88, std::nullopt, true), false},
        // The measured colour alone: the project's "concrete" photographs are
        // facades, windows and all (CLAUDE.md rule 1: never a worse asset).
        {"Bridge structure", &structure, surfaceMaterial("Bridge concrete", {0.24, 0.235, 0.22}, 0.85, std::nullopt, true), false},
        {"Embankments", &earth, grass ? surfaceMaterial(grass->name, grass->color, grass->roughness, std::string("grass"), true)
                                      : surfaceMaterial("Grass", {0.14, 0.20, 0.08}, 0.95, std::string("grass"), true), false}};
    for (const Spec& s : specs)
        if (!s.mesh->empty()) out.parts.push_back({s.name, s.smooth ? smoothSurface(*s.mesh) : std::move(*s.mesh), s.material});
    out.stats = plan.stats;
    out.stats["pieces"] = pieces;
    out.stats["piers"] = piers;
    out.stats["abutments"] = abutments;
    out.stats["deckSidesJoined"] = joinedSides;
    out.stats["sharedMedianSides"]=medianSides;
    std::vector<RoadAxis> detailAxes;
    for(const auto& run:plan.runs) {
        RoadAxis axis;axis.id=run.way;axis.tags=run.tags;axis.bridge=run.bridge;
        for(size_t i=0;i<run.points.size();++i)axis.points.push_back(anchor.toEngine(run.points[i].x,run.points[i].y,run.levels[i]));
        detailAxes.push_back(std::move(axis));
    }
    const Drape drape(carved,anchor);
    auto details=buildRoadDetails(detailAxes,{drape.outline()},{},nullptr,country,neighbours);
    for(auto& p:details.parts)out.parts.push_back(std::move(p));
    out.stats["details"]=details.stats;
    return out;
}

}  // namespace r1
