// Road grading: a road on the ground is smoothed along, level across and
// banked into the survey, and nothing else moves.
#include "check.hpp"

#include "gen/grading.hpp"
#include "gen/terrain.hpp"

#include <cmath>
#include <functional>

using namespace r1;

namespace {
struct Ground {
    Tile tile = tileAt(2.35, 48.85);
    Bounds b = tile.bounds();
    double lon0 = tile.center().x, lat0 = tile.center().y;
    double kx = kMetresPerDegree * std::cos(radians(lat0)), ky = kMetresPerDegree;
    P2 at(double east, double north) const { return {lon0 + east / kx, lat0 + north / ky}; }
    ElevationGrid grid(const std::function<double(double, double)>& height, const Bounds& bounds) const {
        const int n = kTerrainMeshSize;
        ElevationGrid g{bounds, n, {}};
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) {
                const double lon = bounds.west + (bounds.east - bounds.west) * c / (n - 1);
                const double lat = bounds.south + (bounds.north - bounds.south) * r / (n - 1);
                g.values.push_back(height((lon - lon0) * kx, (lat - lat0) * ky));
            }
        return g;
    }
    // A straight road along the tile's middle row, from west to east.
    GradePlan road(Tags tags) const {
        GradePlan plan;
        OsmWay way{1, {}, std::move(tags)};
        for (double e = -400; e <= 400; e += 50) way.points.push_back(at(e, 0));
        plan.roads.push_back(way);
        return plan;
    }
};
const Tags kResidential{{"highway", "residential"}};
}  // namespace

TEST(Grading, a_bump_under_a_road_goes_and_its_real_climb_stays) {
    Ground g;
    // A 4 % climb eastward, and a 4 m knoll (a roof in the surface model) on the road.
    auto height = [](double e, double n) { return 100 + .04 * e + 4 * std::exp(-(e * e + n * n) / (2 * 10. * 10.)); };
    const auto grid = g.grid(height, g.b);
    const auto graded = gradedGround(grid, g.road(kResidential).roads, g.road(kResidential), GroundField(grid, {}));
    const P2 knoll = g.at(0, 0);
    CHECK(grid.sample(knoll.x, knoll.y) > 103.5);
    NEAR(graded.sample(knoll.x, knoll.y), 100., 1.0);
    const P2 west = g.at(-150, 0), east = g.at(150, 0);
    NEAR((graded.sample(east.x, east.y) - graded.sample(west.x, west.y)) / 300., .04, .004);
}

TEST(Grading, the_carriageway_is_level_across_a_side_slope) {
    Ground g;
    auto height = [](double, double n) { return 100 + .08 * n; };  // 8 % across the road
    const auto grid = g.grid(height, g.b);
    const auto graded = gradedGround(grid, g.road(kResidential).roads, g.road(kResidential), GroundField(grid, {}));
    const double half = 3.25;
    const P2 left = g.at(30, half), right = g.at(30, -half);
    CHECK(std::abs(graded.sample(left.x, left.y) - graded.sample(right.x, right.y)) / (2 * half) < .015);
    // The bank meets the survey again, untouched farther away.
    const P2 far = g.at(30, 80);
    NEAR(graded.sample(far.x, far.y), grid.sample(far.x, far.y), 1e-9);
}

TEST(Grading, bridges_tunnels_and_footpaths_keep_the_survey) {
    Ground g;
    auto height = [](double e, double n) { return 100 + 3 * std::sin(e / 23.) + .08 * n; };
    const auto grid = g.grid(height, g.b);
    for (const Tags& tags : {Tags{{"highway", "primary"}, {"bridge", "yes"}}, Tags{{"highway", "primary"}, {"tunnel", "yes"}},
                             Tags{{"highway", "footway"}}, Tags{{"highway", "primary"}, {"r1:raised", "yes"}}}) {
        const auto graded = gradedGround(grid, g.road(tags).roads, g.road(tags), GroundField(grid, {}));
        for (size_t i = 0; i < grid.values.size(); ++i) NEAR(graded.values[i], grid.values[i], 1e-12);
    }
}

TEST(Grading, ground_at_sea_level_is_left_to_the_shore) {
    Ground g;
    auto height = [](double e, double) { return e < 0 ? 0.2 : 0.2 + .02 * e + 2 * std::sin(e / 11.); };
    const auto grid = g.grid(height, g.b);
    const auto graded = gradedGround(grid, g.road(kResidential).roads, g.road(kResidential), GroundField(grid, {}));
    const P2 shore = g.at(-100, 0);
    NEAR(graded.sample(shore.x, shore.y), 0.2, 1e-12);
}

TEST(Grading, a_seam_keeps_its_joined_ground_when_only_one_side_knows_the_road) {
    Ground g;
    // A road running north across the boundary between this tile and the next.
    const Tile north = tileAt(g.lon0, g.b.north + 1e-7);
    const Bounds nb = north.bounds();
    auto height = [](double e, double n) { return 80 + .03 * n + 2.5 * std::sin(n / 17.) + .05 * e; };
    auto own = g.grid(height, g.b), above = g.grid(height, nb);
    for (int k = 0; k <= 20; ++k) {
        const double lon = g.b.west + (g.b.east - g.b.west) * k / 20.;
        const double h = height((lon - g.lon0) * g.kx, (g.b.north - g.lat0) * g.ky);
        own.northEdge.push_back({lon, h});
        above.southEdge.push_back({lon, h});
    }
    GradePlan plan;
    OsmWay way{7, {}, {{"highway", "secondary"}}};
    for (double n = -500; n <= 1100; n += 40) way.points.push_back(g.at(10, n));
    plan.roads.push_back(way);
    const std::vector<ElevationGrid> aroundOwn{above}, aroundAbove{own};
    // As in Paris: this tile's queries have the road, its neighbour's do not.
    const auto a = gradedGround(own, plan.roads, plan, GroundField(own, aroundOwn));
    const auto b = gradedGround(above, {}, GradePlan{}, GroundField(above, aroundAbove));
    CHECK(a.northEdge.size() == b.southEdge.size());
    for (size_t i = 0; i < a.northEdge.size(); ++i) NEAR(a.northEdge[i].y, b.southEdge[i].y, 1e-12);
    // Inside the tile, the road is graded all the same.
    double moved = 0;
    for (double back = 40; back <= 200; back += 5) {
        const P2 inside = g.at(10, (g.b.north - g.lat0) * g.ky - back);
        moved = std::max(moved, std::abs(a.sample(inside.x, inside.y) - own.sample(inside.x, inside.y)));
    }
    CHECK(moved > .05);
}
