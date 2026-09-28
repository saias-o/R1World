// Rules that hold everywhere: how roads are built, not what a code says.
#include "predict.hpp"

#include "streets.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace r1 {

namespace {
constexpr double kCell = 64.0;

bool expressway(const RoadNet::Way& w) { return roadRank(w.highway) >= 5; }

// Two roads drawn across each other without a node in common do not meet:
// one passes over the other. OSM tags most such crossings (`bridge`,
// `tunnel`, `layer`); where it does not, and one of them is an expressway --
// which never meets a road at grade -- the crossing is a bridge. Which road
// is on top is the weaker half of the guess: a tagged layer decides, else the
// more important road, whose alignment the other was built around.
void gradeSeparations(const Context& c, const Rule&, std::vector<Prediction>& out) {
    struct Seg { int way, index; };
    const auto& ways = c.net.ways();
    const auto& verts = c.net.vertices();
    std::unordered_map<int64_t, std::vector<Seg>> grid;
    auto key = [](long x, long z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); };
    for (int w = 0; w < int(ways.size()); ++w)
        for (int i = 0; i + 1 < int(ways[size_t(w)].vertices.size()); ++i) {
            const P2 a = verts[size_t(ways[size_t(w)].vertices[size_t(i)])].xz;
            const P2 b = verts[size_t(ways[size_t(w)].vertices[size_t(i + 1)])].xz;
            for (long x = long(std::floor(std::min(a.x, b.x) / kCell)); x <= long(std::floor(std::max(a.x, b.x) / kCell)); ++x)
                for (long z = long(std::floor(std::min(a.y, b.y) / kCell)); z <= long(std::floor(std::max(a.y, b.y) / kCell)); ++z)
                    grid[key(x, z)].push_back({w, i});
        }
    std::set<std::pair<int, int>> seen;  // one bridge per pair of ways
    std::vector<std::pair<int64_t, std::vector<Seg>*>> cells;
    for (auto& [k, list] : grid) cells.push_back({k, &list});
    std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [k, list] : cells)
        for (size_t m = 0; m < list->size(); ++m)
            for (size_t n = m + 1; n < list->size(); ++n) {
                Seg s = (*list)[m], t = (*list)[n];
                if (s.way == t.way) continue;
                if (s.way > t.way) std::swap(s, t);
                const RoadNet::Way &ws = ways[size_t(s.way)], &wt = ways[size_t(t.way)];
                if (!expressway(ws) && !expressway(wt)) continue;
                const int sa = ws.vertices[size_t(s.index)], sb = ws.vertices[size_t(s.index + 1)];
                const int ta = wt.vertices[size_t(t.index)], tb = wt.vertices[size_t(t.index + 1)];
                if (sa == ta || sa == tb || sb == ta || sb == tb) continue;
                const P2 p = verts[size_t(sa)].xz, r{verts[size_t(sb)].xz.x - p.x, verts[size_t(sb)].xz.y - p.y};
                const P2 q = verts[size_t(ta)].xz, u{verts[size_t(tb)].xz.x - q.x, verts[size_t(tb)].xz.y - q.y};
                const double den = r.x * u.y - r.y * u.x;
                if (std::abs(den) < 1e-9) continue;
                const double tt = ((q.x - p.x) * u.y - (q.y - p.y) * u.x) / den;
                const double uu = ((q.x - p.x) * r.y - (q.y - p.y) * r.x) / den;
                if (tt <= 1e-6 || tt >= 1 - 1e-6 || uu <= 1e-6 || uu >= 1 - 1e-6) continue;
                // Surveyed: OSM already says how they cross.
                if (ws.bridge || ws.tunnel || wt.bridge || wt.tunnel || ws.layer != wt.layer) continue;
                if (!seen.insert({s.way, t.way}).second) continue;
                const int rs = roadRank(ws.highway), rt = roadRank(wt.highway);
                const bool tie = rs == rt;
                const bool sOver = tie ? ws.osm->id < wt.osm->id : rs > rt;
                const RoadNet::Way& over = sOver ? ws : wt;
                const RoadNet::Way& under = sOver ? wt : ws;
                const P2 dir = sOver ? r : u;
                const double length = std::hypot(dir.x, dir.y);
                Prediction pr;
                pr.what = "bridge";
                pr.xz = {p.x + r.x * tt, p.y + r.y * tt};
                pr.travel = {dir.x / length, dir.y / length};
                pr.evidence = over.osm->id;
                pr.detail = {{"over", over.osm->id}, {"under", under.osm->id},
                             {"overHighway", over.highway}, {"underHighway", under.highway},
                             {"overBasis", tie ? "tie, lower OSM id" : "more important road"},
                             {"overConfidence", tie ? 0.5 : 0.65},
                             // The deck spans the road beneath and its verges.
                             {"span", pyround(roadWidth(under.osm->tags) + 6.0, 1)}};
                out.push_back(std::move(pr));
            }
}
}  // namespace

std::vector<Rule> structureRules() {
    return {
        {"any.grade_separation",
         "roads crossing without a shared node, one an expressway, cross on a bridge; the more important on top",
         0.90, gradeSeparations},
    };
}

}  // namespace r1
