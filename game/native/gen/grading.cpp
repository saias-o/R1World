#include "grading.hpp"

#include "streets.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <unordered_map>

namespace r1 {
namespace {
double smoothstep(double a, double b, double x) {
    x = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return x * x * (3 - 2 * x);
}

// How a class of road is graded: the window along it, and how far below
// (cut) or above (fill) the survey its level may go.
struct Grade {
    double sigma = 0, cut = 0, fill = 0;
};

std::optional<Grade> gradeOf(const Tags& tags) {
    auto absent = [&](const char* key) {
        const std::string v = tagOr(tags, key, "no");
        return v == "no" || v.empty();
    };
    if (!absent("bridge") || !absent("tunnel") || !absent("covered") || tagOr(tags, "r1:raised") == "yes" ||
        tagOr(tags, "area") == "yes")
        return std::nullopt;
    if (const auto* layer = tag(tags, "layer"); layer && std::atof(layer->c_str()) < 0) return std::nullopt;
    const std::string h = tagOr(tags, "highway");
    const std::string base = h.size() > 5 && h.compare(h.size() - 5, 5, "_link") == 0 ? h.substr(0, h.size() - 5) : h;
    // A surface model's roof or canopy on a road is a few metres; a deeper
    // difference is the road's real relief, and stays.
    if (base == "motorway" || base == "trunk") return Grade{45, 8.0, 4.0};
    if (base == "primary" || base == "secondary") return Grade{30, 5.0, 3.0};
    if (base == "tertiary" || base == "unclassified" || base == "residential" || base == "road") return Grade{20, 4.0, 2.0};
    if (base == "living_street" || base == "service") return Grade{12, 2.5, 1.5};
    return std::nullopt;
}

constexpr double kStation = 4.0;     // a road's level is solved every 4 m
constexpr double kBucket = 32.0;     // segment index cell
constexpr double kBankMin = 10.0, kBankMax = 30.0, kBankRatio = 3.0;
constexpr double kStraightOn = 0.766;  // cos 40 degrees: a junction continues a road within it
constexpr double kSeaLevel = 0.5;    // ground at or below it is the shore's, not the road's
constexpr double kSeamFade = 30.0;   // metres from a tile's edge the grading fades in over

// Metres east and north of a point, in that point's own frame: the same
// figures on both sides of a seam, whichever tile asks.
struct Frame {
    double lon0 = 0, lat0 = 0, kx = 1;
    explicit Frame(P2 at = {}) : lon0(at.x), lat0(at.y), kx(kMetresPerDegree * std::cos(radians(at.y))) {}
    P2 operator()(P2 g) const { return {(g.x - lon0) * kx, (g.y - lat0) * kMetresPerDegree}; }
};

struct Road {
    Frame frame;
    int64_t id = 0;
    std::vector<P2> geo, xy;
    std::vector<double> s;
    double half = 0;
    Grade grade;
    struct Link {
        int road = -1;
        bool atStart = true;
    } next[2];
    std::vector<double> levels;
    bool solved = false;
    double length() const { return s.back(); }
    P2 geoAt(double at) const {
        const auto hi = std::upper_bound(s.begin(), s.end(), at);
        if (hi == s.begin()) return geo.front();
        if (hi == s.end()) return geo.back();
        const size_t i = size_t(hi - s.begin());
        const double t = (at - s[i - 1]) / std::max(1e-9, s[i] - s[i - 1]);
        return {geo[i - 1].x + (geo[i].x - geo[i - 1].x) * t, geo[i - 1].y + (geo[i].y - geo[i - 1].y) * t};
    }
};

class Grader {
public:
    Grader(const ElevationGrid& grid, const std::vector<OsmWay>& ways, const GradePlan& plan, const GroundField& field)
        : field_(field) {
        const Bounds& b = grid.bounds;
        bounds_ = b;
        lon0_ = (b.west + b.east) / 2;
        lat0_ = (b.south + b.north) / 2;
        kx_ = kMetresPerDegree * std::cos(radians(lat0_));
        ky_ = kMetresPerDegree;
        cell_ = (b.north - b.south) * ky_ / (kTerrainMeshSize - 1);
        const P2 lo = local({b.west, b.south}), hi = local({b.east, b.north});
        // A window reaches 2.5 sigma down the road and past a few junctions.
        const double margin = 250.0;
        for (const OsmWay& way : ways) {
            const auto grade = gradeOf(way.tags);
            if (!grade || way.points.size() < 2) continue;
            Road r;
            r.frame = Frame(way.points.front());
            r.id = way.id;
            r.grade = *grade;
            r.half = roadWidth(way.tags) / 2;
            bool near = false;
            for (const P2& g : way.points) {
                const P2 p = r.frame(g), q = local(g);
                if (!r.xy.empty() && std::hypot(p.x - r.xy.back().x, p.y - r.xy.back().y) < 1e-3) continue;
                r.s.push_back(r.xy.empty() ? 0.0 : r.s.back() + std::hypot(p.x - r.xy.back().x, p.y - r.xy.back().y));
                r.xy.push_back(p);
                r.geo.push_back(g);
                near |= q.x > lo.x - margin && q.x < hi.x + margin && q.y > lo.y - margin && q.y < hi.y + margin;
            }
            if (near && r.xy.size() >= 2) roads_.push_back(std::move(r));
        }
        link();
        for (size_t i = 0; i < roads_.size(); ++i) {
            const Road& r = roads_[i];
            // Indexed in the tile's frame, with slack for the frames' difference.
            const double reach = r.half + coreExtra() + kBankMax + 5.0;
            for (size_t k = 0; k + 1 < r.xy.size(); ++k) {
                const P2 a = local(r.geo[k]), c = local(r.geo[k + 1]);
                const int x0 = int(std::floor((std::min(a.x, c.x) - reach) / kBucket));
                const int x1 = int(std::floor((std::max(a.x, c.x) + reach) / kBucket));
                const int y0 = int(std::floor((std::min(a.y, c.y) - reach) / kBucket));
                const int y1 = int(std::floor((std::max(a.y, c.y) + reach) / kBucket));
                if (x1 < std::floor((lo.x - reach) / kBucket) || x0 > std::floor((hi.x + reach) / kBucket) ||
                    y1 < std::floor((lo.y - reach) / kBucket) || y0 > std::floor((hi.y + reach) / kBucket))
                    continue;
                for (int x = x0; x <= x1; ++x)
                    for (int y = y0; y <= y1; ++y) buckets_[key(x, y)].push_back({int(i), int(k)});
            }
        }
        for (const RaisedRun& run : plan.runs)
            for (size_t k = 0; k + 1 < run.points.size(); ++k) runs_.push_back({run.points[k], run.points[k + 1]});
        for (const GradePlan::Carve& c : plan.carves) carves_.push_back({c.at, c.radius});
    }

    size_t roads() const { return roads_.size(); }

    // 1 clear of the works; 0 on and beside an embankment, a deck or a dig.
    double fadeAt(P2 geo) const {
        // Two tiles do not read the same roads near their seam (each has its
        // own neighbourhood queries), so the seam keeps the joined survey both
        // agree on, and the grading fades in from it.
        const double edge = std::min({(geo.x - bounds_.west) * kx_, (bounds_.east - geo.x) * kx_,
                                      (geo.y - bounds_.south) * ky_, (bounds_.north - geo.y) * ky_});
        double fade = smoothstep(0, kSeamFade, edge);
        for (const auto& [a, b] : runs_) {
            const Frame f(a);
            fade = std::min(fade, smoothstep(15, 40, distance(f(geo), {0, 0}, f(b)).first));
        }
        for (const auto& [c, radius] : carves_) {
            const P2 d = Frame(c)(geo);
            fade = std::min(fade, smoothstep(radius, radius + 15, std::hypot(d.x, d.y)));
        }
        return fade;
    }

    // How a ground rolls under the tile's roads: the change of grade every
    // 10 m along them (m per 10 m) and the slope across the carriageway, each
    // averaged; the worst roll is also given.
    struct Ride {
        double roll = 0, worstRoll = 0, across = 0;
        int samples = 0;
    };
    Ride ride(const ElevationGrid& g) const {
        Ride out;
        const Bounds& b = g.bounds;
        auto inside = [&](P2 p) { return b.west <= p.x && p.x <= b.east && b.south <= p.y && p.y <= b.north; };
        for (const Road& r : roads_) {
            constexpr double step = 10.0;
            for (double s = step; s + step <= r.length(); s += step) {
                const P2 a = r.geoAt(s - step), m = r.geoAt(s), c = r.geoAt(s + step);
                if (!inside(a) || !inside(m) || !inside(c)) continue;
                const double roll = std::abs(g.sample(a.x, a.y) - 2 * g.sample(m.x, m.y) + g.sample(c.x, c.y));
                // Across: the carriageway's edges, square to the road here.
                const P2 pa = r.frame(a), pc = r.frame(c);
                const double dx = pc.x - pa.x, dy = pc.y - pa.y, l = std::hypot(dx, dy);
                if (l <= 0) continue;
                const double nx = -dy / l * r.half / r.frame.kx, ny = dx / l * r.half / kMetresPerDegree;
                const double across = std::abs(g.sample(m.x + nx, m.y + ny) - g.sample(m.x - nx, m.y - ny)) / (2 * r.half);
                out.roll += roll;
                out.worstRoll = std::max(out.worstRoll, roll);
                out.across += across;
                ++out.samples;
            }
        }
        if (out.samples) {
            out.roll /= out.samples;
            out.across /= out.samples;
        }
        return out;
    }

    // The graded height of a ground point whose survey says `raw`.
    double at(P2 geo, double raw) {
        if (raw <= kSeaLevel) return raw;
        const P2 p = local(geo);
        const double fade = fadeAt(geo);
        if (fade <= 0) return raw;
        const auto found = buckets_.find(key(int(std::floor(p.x / kBucket)), int(std::floor(p.y / kBucket))));
        if (found == buckets_.end()) return raw;
        // The nearest point of each road.
        std::vector<std::pair<int, std::pair<double, double>>> nearest;
        for (const auto& [road, segment] : found->second) {
            const Road& r = roads_[size_t(road)];
            auto [d, t] = distance(r.frame(geo), r.xy[size_t(segment)], r.xy[size_t(segment) + 1]);
            const double s = r.s[size_t(segment)] + t * (r.s[size_t(segment) + 1] - r.s[size_t(segment)]);
            auto it = std::find_if(nearest.begin(), nearest.end(), [&](const auto& n) { return n.first == road; });
            if (it == nearest.end()) nearest.push_back({road, {d, s}});
            else if (d < it->second.first) it->second = {d, s};
        }
        double weight = 0, weighted = 0;
        for (const auto& [road, place] : nearest) {
            const Road& r = roads_[size_t(road)];
            const double core = r.half + coreExtra();
            if (place.first >= core + kBankMax) continue;
            const double target = level(road, place.second);
            const double bank = std::clamp(kBankRatio * std::abs(target - raw), kBankMin, kBankMax);
            const double w = (1 - smoothstep(core, core + bank, place.first)) * fade;
            weight += w;
            weighted += w * target;
        }
        if (weight <= 0) return raw;
        if (weight <= 1) return raw + (weighted - weight * raw);
        return weighted / weight;
    }

private:
    const GroundField& field_;
    Bounds bounds_;
    double lon0_ = 0, lat0_ = 0, kx_ = 1, ky_ = 1, cell_ = 14;
    std::vector<Road> roads_;
    std::unordered_map<int64_t, std::vector<std::pair<int, int>>> buckets_;
    std::vector<std::pair<P2, P2>> runs_;
    std::vector<std::pair<P2, double>> carves_;

    // The carriageway is drawn on the cells it crosses: the nodes of those
    // cells carry its level too, or it leans between a graded node and not.
    double coreExtra() const { return 0.85 * cell_; }
    static int64_t key(int x, int y) { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); }
    P2 local(P2 g) const { return {(g.x - lon0_) * kx_, (g.y - lat0_) * ky_}; }
    static std::pair<double, double> distance(P2 p, P2 a, P2 b) {
        const double dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
        const double t = l > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l, 0.0, 1.0) : 0.0;
        return {std::hypot(p.x - a.x - dx * t, p.y - a.y - dy * t), t};
    }

    // Each end continues into the straightest road that leaves the same node.
    void link() {
        std::map<P2, std::vector<std::pair<int, bool>>> ends;
        for (size_t i = 0; i < roads_.size(); ++i) {
            ends[roads_[i].geo.front()].push_back({int(i), true});
            ends[roads_[i].geo.back()].push_back({int(i), false});
        }
        auto unit = [](P2 a, P2 b) {
            const double l = std::hypot(b.x - a.x, b.y - a.y);
            return l > 0 ? P2{(b.x - a.x) / l, (b.y - a.y) / l} : P2{0, 0};
        };
        for (size_t i = 0; i < roads_.size(); ++i)
            for (int end = 0; end < 2; ++end) {
                const Road& r = roads_[i];
                const size_t n = r.xy.size();
                const P2 out = end ? unit(r.xy[n - 2], r.xy[n - 1]) : unit(r.xy[1], r.xy[0]);
                double best = kStraightOn;
                for (const auto& [j, atStart] : ends[end ? r.geo.back() : r.geo.front()]) {
                    if (j == int(i) && atStart == (end == 0)) continue;  // the end we leave by
                    const Road& o = roads_[size_t(j)];
                    const size_t m = o.xy.size();
                    const P2 on = atStart ? unit(o.xy[0], o.xy[1]) : unit(o.xy[m - 1], o.xy[m - 2]);
                    const double straight = out.x * on.x + out.y * on.y;
                    if (straight > best) {
                        best = straight;
                        roads_[i].next[end] = {j, atStart};
                    }
                }
            }
    }

    // A point `t` metres from (road, s), walking `dir` and on through junctions.
    std::optional<P2> along(int road, double s, int dir, double t) const {
        for (int hop = 0; hop < 12; ++hop) {
            const Road& r = roads_[size_t(road)];
            const double to = s + dir * t;
            if (to >= 0 && to <= r.length()) return r.geoAt(to);
            t -= dir > 0 ? r.length() - s : s;
            const auto next = r.next[dir > 0 ? 1 : 0];
            if (next.road < 0) return std::nullopt;
            road = next.road;
            s = next.atStart ? 0.0 : roads_[size_t(road)].length();
            dir = next.atStart ? 1 : -1;
        }
        return std::nullopt;
    }

    double profile(int road, double s) const {
        const Road& r = roads_[size_t(road)];
        const double sigma = r.grade.sigma, step = std::max(2.0, sigma / 5);
        const int reach = int(std::ceil(2.5 * sigma / step));
        std::vector<std::pair<double, double>> samples;
        for (int k = -reach; k <= reach; ++k) {
            const auto p = k == 0 ? std::optional<P2>(r.geoAt(s)) : along(road, s, k > 0 ? 1 : -1, std::abs(k) * step);
            if (!p) continue;
            const double x = k * step / sigma;
            samples.push_back({std::exp(-0.5 * x * x), field_.at(p->x, p->y)});
        }
        auto mean = [&](double centre, bool robust) {
            double sw = 0, sh = 0;
            for (const auto& [w, h] : samples) {
                const double over = h - centre;
                // A knot above the road counts less: the road cuts into a bump.
                const double weight = robust && over > 0 ? w / (1 + over * over) : w;
                sw += weight;
                sh += weight * h;
            }
            return sh / std::max(sw, 1e-12);
        };
        double level = mean(0, false);
        for (int pass = 0; pass < 3; ++pass) level = mean(level, true);
        const P2 here = r.geoAt(s);
        const double survey = field_.at(here.x, here.y);
        return std::clamp(level, survey - r.grade.cut, survey + r.grade.fill);
    }

    double level(int road, double s) {
        Road& r = roads_[size_t(road)];
        if (!r.solved) {
            const int n = std::max(2, int(std::ceil(r.length() / kStation)) + 1);
            r.levels.resize(size_t(n));
            for (int i = 0; i < n; ++i) r.levels[size_t(i)] = profile(road, r.length() * i / (n - 1));
            r.solved = true;
        }
        const double u = std::clamp(s / std::max(r.length(), 1e-9), 0.0, 1.0) * double(r.levels.size() - 1);
        const size_t i = std::min(r.levels.size() - 2, size_t(u));
        return r.levels[i] + (r.levels[i + 1] - r.levels[i]) * (u - double(i));
    }
};
}  // namespace

Mesh cutUnderRaisedRoads(std::map<std::string, Mesh>& terrain, const nlohmann::json& raised, nlohmann::json* stats) {
    constexpr double kReach = 0.6;   // ground this close under the road is cut out
    constexpr double kCell = 8.0;    // triangle and footprint index
    Mesh faces(UvMode::Slope);
    // The drawn ground's triangles, indexed, to read its height anywhere.
    struct Triangle { P3 a, b, c; };
    std::vector<Triangle> triangles;
    std::unordered_map<int64_t, std::vector<size_t>> index;
    auto key = [](double x, double z) {
        return (int64_t(std::floor(x / kCell)) << 32) ^ int64_t(uint32_t(int32_t(std::floor(z / kCell))));
    };
    for (const auto& [name, mesh] : terrain)
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const Triangle t{mesh.positions[mesh.indices[i]], mesh.positions[mesh.indices[i + 1]], mesh.positions[mesh.indices[i + 2]]};
            const double x0 = std::min({t.a.x, t.b.x, t.c.x}), x1 = std::max({t.a.x, t.b.x, t.c.x});
            const double z0 = std::min({t.a.z, t.b.z, t.c.z}), z1 = std::max({t.a.z, t.b.z, t.c.z});
            for (double x = std::floor(x0 / kCell) * kCell; x <= x1; x += kCell)
                for (double z = std::floor(z0 / kCell) * kCell; z <= z1; z += kCell) index[key(x, z)].push_back(triangles.size());
            triangles.push_back(t);
        }
    auto heightAt = [&](P2 p) -> std::optional<double> {
        const auto found = index.find(key(p.x, p.y));
        if (found == index.end()) return std::nullopt;
        for (size_t i : found->second) {
            const Triangle& t = triangles[i];
            const double d = (t.b.z - t.c.z) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.z - t.c.z);
            if (std::abs(d) < 1e-12) continue;
            const double wa = ((t.b.z - t.c.z) * (p.x - t.c.x) + (t.c.x - t.b.x) * (p.y - t.c.z)) / d;
            const double wb = ((t.c.z - t.a.z) * (p.x - t.c.x) + (t.a.x - t.c.x) * (p.y - t.c.z)) / d;
            if (wa < -1e-9 || wb < -1e-9 || wa + wb > 1 + 1e-9) continue;
            return wa * t.a.y + wb * t.b.y + (1 - wa - wb) * t.c.y;
        }
        return std::nullopt;
    };
    // The pieces the ground reaches: their footprints, shoulders included.
    struct Piece { P3 a, b; P2 right; double half[2]; };
    std::vector<Piece> pieces;
    clip::Paths64 footprints;
    for (const auto& r : raised) {
        Piece p;
        const auto& a = r.at("a");
        const auto& b = r.at("b");
        p.a = {a[0].get<double>(), a[1].get<double>(), a[2].get<double>()};
        p.b = {b[0].get<double>(), b[1].get<double>(), b[2].get<double>()};
        p.half[0] = r.at("half")[0].get<double>();
        p.half[1] = r.at("half")[1].get<double>();
        const double dx = p.b.x - p.a.x, dz = p.b.z - p.a.z, l = std::hypot(dx, dz);
        if (l < 1e-6) continue;
        const P2 dir{dx / l, dz / l};
        p.right = {-dir.y, dir.x};
        // `a` and `b` are the carriageway's surface; the shoulder is 6 cm lower.
        auto corner = [&](double t, double offset) {
            const double along = t < .5 ? -0.2 : 0.2;
            return P2{p.a.x + dx * t + dir.x * along + p.right.x * offset, p.a.z + dz * t + dir.y * along + p.right.y * offset};
        };
        bool reached = false;
        for (double t : {0.0, 0.5, 1.0})
            for (double offset : {-p.half[1], 0.0, p.half[0]}) {
                const P2 q{p.a.x + dx * t + p.right.x * offset, p.a.z + dz * t + p.right.y * offset};
                const auto h = heightAt(q);
                reached |= h && *h >= p.a.y + (p.b.y - p.a.y) * t - .06 - kReach;
            }
        if (!reached) continue;
        footprints.push_back(clip::kMetres.path({corner(0, p.half[0]), corner(1, p.half[0]), corner(1, -p.half[1]), corner(0, -p.half[1])}));
        pieces.push_back(p);
    }
    double faceArea = 0, tallest = 0, removed = 0;
    if (!pieces.empty()) {
        const clip::Paths64 cut = clip::unite(footprints);
        // Cuttings first, against the ground as drawn before the cut.
        for (const Piece& p : pieces)
            for (int s = 0; s < 2; ++s) {
                const double sign = s == 0 ? 1.0 : -1.0, offset = sign * p.half[s];
                const double length = std::hypot(p.b.x - p.a.x, p.b.z - p.a.z);
                const int n = std::max(1, int(std::ceil(length / 2.0)));
                struct Sample { P3 low, high; bool open; };
                std::vector<Sample> samples;
                for (int k = 0; k <= n; ++k) {
                    const double t = double(k) / n;
                    const P2 q{p.a.x + (p.b.x - p.a.x) * t + p.right.x * offset, p.a.z + (p.b.z - p.a.z) * t + p.right.y * offset};
                    const double road = p.a.y + (p.b.y - p.a.y) * t - .06;
                    const auto ground = heightAt(q);
                    // Only where the cut's edge is: beside another piece the road goes on.
                    const P2 out{q.x + p.right.x * sign * 0.15, q.y + p.right.y * sign * 0.15};
                    const bool open = ground && *ground > road + .02 && !clip::contains(cut, out);
                    samples.push_back({{q.x, road - .02, q.y}, {q.x, open ? *ground : road, q.y}, open});
                }
                for (int k = 0; k < n; ++k) {
                    const Sample &i = samples[size_t(k)], &j = samples[size_t(k + 1)];
                    if (!i.open && !j.open) continue;
                    P3 a = i.low, b = j.low, c = j.high, d = i.high;
                    // Facing the road: its normal points back across the carriageway.
                    const P3 n0 = faceNormal(a, b, c);
                    if (n0.x * -p.right.x * sign + n0.z * -p.right.y * sign < 0) std::swap(b, d);
                    faces.addQuad(a, b, c, d);
                    tallest = std::max({tallest, i.high.y - i.low.y, j.high.y - j.low.y});
                    faceArea += std::hypot(j.low.x - i.low.x, j.low.z - i.low.z) *
                                ((i.high.y - i.low.y) + (j.high.y - j.low.y)) / 2;
                }
            }
        // Then the ground under the road goes, each piece left on its own plane.
        std::vector<std::array<double, 4>> boxes;
        for (const auto& path : cut) {
            std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
            for (const auto& pt : path) {
                const double x = double(pt.x) / clip::kMetres.scale, z = double(pt.y) / clip::kMetres.scale;
                box = {std::min(box[0], x), std::min(box[1], z), std::max(box[2], x), std::max(box[3], z)};
            }
            boxes.push_back(box);
        }
        for (auto& [name, mesh] : terrain) {
            Mesh kept;
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const P3 a = mesh.positions[mesh.indices[i]], b = mesh.positions[mesh.indices[i + 1]], c = mesh.positions[mesh.indices[i + 2]];
                auto uv = [](P3 q) { return UV{q.x, -q.z}; };
                const double x0 = std::min({a.x, b.x, c.x}), x1 = std::max({a.x, b.x, c.x});
                const double z0 = std::min({a.z, b.z, c.z}), z1 = std::max({a.z, b.z, c.z});
                const bool touches = std::any_of(boxes.begin(), boxes.end(), [&](const auto& box) {
                    return box[0] <= x1 && x0 <= box[2] && box[1] <= z1 && z0 <= box[3];
                });
                const P3 normal = faceNormal(a, b, c);
                if (!touches || std::abs(normal.y) < 1e-6) {
                    const UV tex[3] = {uv(a), uv(b), uv(c)};
                    kept.addUpTriangle(a, b, c, tex);
                    continue;
                }
                const clip::Paths64 shape{clip::kMetres.path({{a.x, a.z}, {b.x, b.z}, {c.x, c.z}})};
                const clip::Paths64 left = clip::subtract(shape, cut);
                removed += clip::area(shape) - clip::area(left);
                auto on = [&](P2 q) { return P3{q.x, a.y - (normal.x * (q.x - a.x) + normal.z * (q.y - a.z)) / normal.y, q.y}; };
                for (const auto& polygon : clip::polygons(left))
                    for (const auto& t : clip::triangles(polygon)) {
                        const P3 pa = on(t[0]), pb = on(t[1]), pc = on(t[2]);
                        const UV tex[3] = {uv(pa), uv(pb), uv(pc)};
                        kept.addUpTriangle(pa, pb, pc, tex);
                    }
            }
            mesh = std::move(kept);
        }
    }
    if (stats)
        *stats = {{"source", "inferred: terrain cut under roads at their own level"}, {"pieces", pieces.size()},
                  {"groundRemovedM2", pyround(removed, 1)}, {"cuttingFaceM2", pyround(faceArea, 1)},
                  {"tallestCutting", pyround(tallest, 2)}};
    return faces;
}

ElevationGrid gradedGround(const ElevationGrid& grid, const std::vector<OsmWay>& roads, const GradePlan& plan,
                           const GroundField& field, nlohmann::json* stats) {
    const int n = kTerrainMeshSize;
    const Bounds& b = grid.bounds;
    Grader grader(grid, roads, plan, field);
    ElevationGrid out{b, n, {}};
    out.values.reserve(size_t(n * n));
    int moved = 0;
    double cut = 0, fill = 0, total = 0;
    auto record = [&](double raw, double graded) {
        const double d = graded - raw;
        if (std::abs(d) < 0.05) return;
        ++moved;
        total += std::abs(d);
        cut = std::max(cut, -d);
        fill = std::max(fill, d);
    };
    for (int row = 0; row < n; ++row)
        for (int col = 0; col < n; ++col) {
            const double lon = b.west + (b.east - b.west) * col / (n - 1);
            const double lat = b.south + (b.north - b.south) * row / (n - 1);
            const double raw = grid.size == n ? grid.at(row, col) : grid.sample(lon, lat), graded = grader.at({lon, lat}, raw);
            record(raw, graded);
            out.values.push_back(graded);
        }
    out.southEdge = grid.southEdge;
    out.northEdge = grid.northEdge;
    for (auto* edge : {&out.southEdge, &out.northEdge})
        for (auto& p : *edge) p.y = grader.at({p.x, edge == &out.southEdge ? b.south : b.north}, p.y);
    if (stats) {
        const auto before = grader.ride(grid), after = grader.ride(out);
        auto ride = [](const Grader::Ride& r) {
            return nlohmann::json{{"rollPer10m", pyround(r.roll, 3)}, {"worstRollPer10m", pyround(r.worstRoll, 2)},
                                  {"acrossSlope", pyround(r.across, 4)}};
        };
        *stats = {{"revision", kGradingRevision}, {"source", "inferred: road profiles smoothed along the survey"},
                  {"roads", grader.roads()}, {"nodesMoved", moved},
                  {"deepestCut", pyround(cut, 2)}, {"highestFill", pyround(fill, 2)},
                  {"meanShift", pyround(moved ? total / moved : 0.0, 2)},
                  {"survey", ride(before)}, {"graded", ride(after)}, {"rideSamples", after.samples}};
    }
    return out;
}

}  // namespace r1
