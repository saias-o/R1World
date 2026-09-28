// Bridges: the profile a deck is solved at, the ground dug under it, and the
// works drawn from them, each on the smallest scene that shows it.
#include "check.hpp"

#include "gen/bridges.hpp"
#include "gen/terrain.hpp"

#include <cmath>

using namespace r1;

namespace {
constexpr double kLon = 1.30, kLat = 47.60;
const double kPerLat = 111320.0, kPerLon = 111320.0 * std::cos(kLat * kPi / 180);

struct Scene {
    OsmData osm;
    Tile tile = tileAt(kLon, kLat);
    Anchor anchor = Anchor::at(tile.center().x, tile.center().y, 0);
    ElevationGrid grid{tile.bounds(), 2, {50.0, 50.0, 50.0, 50.0}};
    std::vector<ElevationGrid> around;
    int64_t next = 1000;
    Scene() { osm.country = "FR"; }
    P2 at(double east, double north) const { return {tile.center().x + east / kPerLon, tile.center().y + north / kPerLat}; }
    int64_t road(std::vector<P2> points, Tags tags) {
        osm.roads.push_back({next, std::move(points), std::move(tags)});
        return next++;
    }
    GradePlan plan() {
        const Anchor a = anchor;
        const ElevationGrid& g = grid;
        const auto predicted = predictDetails(osm, tile, anchor, [&](double lon, double lat) { return groundPoint(lon, lat, g, a); });
        return planGrades(osm, predicted.structures, GroundField(grid, around), anchor, tile.bounds());
    }
};

double highest(const GradePlan& p, bool bridge) {
    double h = -1e300;
    for (const RaisedRun& r : p.runs)
        if (r.bridge == bridge)
            for (double l : r.levels) h = std::max(h, l);
    return h;
}
}  // namespace

TEST(Bridges, a_motorway_over_a_lane_clears_it_by_the_gabarit_and_climbs_to_it_at_five_percent) {
    Scene s;
    const int64_t motorway = s.road({s.at(-270, -5), s.at(270, 5)}, {{"highway", "motorway"}});
    s.road({s.at(0, -200), s.at(0, 200)}, {{"highway", "residential"}});
    const GradePlan p = s.plan();
    CHECK_MSG(p.stats["predictedSpans"] == 1, p.stats.dump());
    // The deck: 4.75 m over the lane and a metre of structure.
    NEAR(highest(p, true), 50.0 + 4.75 + 1.0, 0.05);
    // No embankment steeper than the grade a motorway is built to.
    for (const RaisedRun& r : p.runs)
        for (size_t i = 0; i + 1 < r.points.size(); ++i) {
            const double run = std::hypot((r.points[i + 1].x - r.points[i].x) * kPerLon, (r.points[i + 1].y - r.points[i].y) * kPerLat);
            CHECK(std::abs(r.levels[i + 1] - r.levels[i]) <= 0.05 * run + 1e-3);
        }
    // The motorway is cut where it leaves the ground; the lane never does.
    bool span = false, ramp = false;
    for (const OsmWay& w : p.roads) {
        if (w.id == motorway) {
            span |= tagOr(w.tags, "r1:bridge") == "predicted";
            ramp |= has(w.tags, "r1:raised");
        } else {
            CHECK(!has(w.tags, "r1:raised") && !has(w.tags, "bridge"));
        }
    }
    CHECK(span && ramp);
    CHECK(p.carves.empty());
}

TEST(Bridges, a_surveyed_bridge_rises_a_little_and_the_road_beneath_is_dug_for_the_rest) {
    Scene s;
    const P2 a = s.at(-20, 0), b = s.at(20, 0);
    s.road({s.at(-200, 0), a}, {{"highway", "residential"}});
    s.road({a, b}, {{"highway", "residential"}, {"bridge", "yes"}, {"layer", "1"}});
    s.road({b, s.at(200, 0)}, {{"highway", "residential"}});
    s.road({s.at(0, -150), s.at(0, 150)}, {{"highway", "residential"}});
    const GradePlan p = s.plan();
    NEAR(highest(p, true), 52.5, 0.05);
    CHECK_MSG(p.carves.size() == 1, p.stats.dump());
    NEAR(p.carves[0].depth, 5.75 - 2.5 + 0.3, 0.05);
    // The dig is in the relief the whole tile stands on, right under the deck.
    const ElevationGrid dug = carvedGround(s.grid, p);
    CHECK(dug.size == kTerrainMeshSize);
    CHECK(dug.sample(s.at(0, 0).x, s.at(0, 0).y) < 50.0 - 3.0);
    NEAR(dug.sample(s.at(0, 200).x, s.at(0, 200).y), 50.0, 1e-9);
}

TEST(Bridges, a_deck_spans_a_valley_straight_between_its_abutments) {
    Scene s;
    // Ground at 50 m, a valley 20 m deep down the middle of the tile.
    s.grid = ElevationGrid{s.tile.bounds(), 5, {}};
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 5; ++c) s.grid.values.push_back(c == 2 ? 30.0 : 50.0);
    const P2 a = s.at(-130, 0), b = s.at(130, 0);
    s.road({s.at(-260, 0), a}, {{"highway", "secondary"}});
    s.road({a, b}, {{"highway", "secondary"}, {"bridge", "viaduct"}});
    s.road({b, s.at(260, 0)}, {{"highway", "secondary"}});
    const GradePlan p = s.plan();
    int decks = 0;
    for (const RaisedRun& r : p.runs) {
        if (!r.bridge) continue;
        ++decks;
        // Straight from one abutment to the other, high over the valley floor.
        const size_t n = r.levels.size();
        for (size_t i = 0; i < n; ++i) NEAR(r.levels[i], r.levels[0] + (r.levels[n - 1] - r.levels[0]) * double(i) / double(n - 1), 0.05);
        CHECK(r.levels[n / 2] - r.grounds[n / 2] > 10.0);
    }
    CHECK(decks == 1);
    const auto works = buildBridges(p, s.tile.bounds(), s.grid, s.anchor);
    CHECK(works.stats["piers"].get<int>() >= 5);
}

TEST(Bridges, two_carriageways_mapped_side_by_side_are_one_deck) {
    Scene s;
    for (double north : {-6.0, 6.0}) {
        const P2 a = s.at(-40, north), b = s.at(40, north);
        s.road({s.at(-200, north), a}, {{"highway", "primary"}, {"oneway", "yes"}});
        s.road({a, b}, {{"highway", "primary"}, {"oneway", "yes"}, {"bridge", "yes"}, {"layer", "1"}});
        s.road({b, s.at(200, north)}, {{"highway", "primary"}, {"oneway", "yes"}});
    }
    s.road({s.at(0, -150), s.at(0, 150)}, {{"highway", "residential"}});
    const GradePlan p = s.plan();
    const auto works = buildBridges(p, s.tile.bounds(), s.grid, s.anchor);
    CHECK(works.stats["deckSidesJoined"].get<int>() > 0);
    // Each deck keeps its parapet on its outer side only.
    int outer = 0, inner = 0;
    for (const auto& piece : works.raised) {
        if (piece["solid"].get<bool>()) continue;
        const int rails = int(piece["rail"][0].get<bool>()) + int(piece["rail"][1].get<bool>());
        CHECK(rails == 1);
        (piece["a"][2].get<double>() < 0 ? outer : inner) += 1;  // north deck, south deck
    }
    CHECK(outer > 0 && inner > 0);
}

TEST(Bridges, a_tunnel_and_a_road_on_the_ground_are_left_where_they_are) {
    Scene s;
    s.road({s.at(-200, 0), s.at(200, 0)}, {{"highway", "motorway"}, {"tunnel", "yes"}});
    s.road({s.at(0, -200), s.at(0, 200)}, {{"highway", "residential"}});
    const GradePlan p = s.plan();
    CHECK(p.runs.empty() && p.carves.empty());
    CHECK(p.roads.size() == 2);
}
