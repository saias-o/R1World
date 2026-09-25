// The people: where they may walk, how many, and how they walk.
#include "check.hpp"

#include "gen/crowd.hpp"
#include "gen/streets.hpp"

#include <set>

using namespace r1;

namespace {
OsmWay way(int64_t id, std::vector<P2> points, Tags tags) { return OsmWay{id, std::move(points), std::move(tags)}; }

// A flat 220 m box on the equator, one street across it east to west.
struct Street {
    Anchor anchor = Anchor::at(0, 0);
    ElevationGrid grid{{-0.001, -0.001, 0.001, 0.001}, 2, {0.0, 0.0, 0.0, 0.0}};
    std::vector<OsmWay> roads = {way(1, {{-0.0009, 0.0}, {0.0009, 0.0}}, {{"highway", "residential"}, {"width", "6"}})};
    std::vector<OsmNode> features;
    std::vector<OsmWay> buildingWays;
    std::vector<Ring> footprints;
    nlohmann::json props = nlohmann::json::array();
    nlohmann::json cook() {
        std::vector<const OsmWay*> buildings;
        for (const OsmWay& b : buildingWays) buildings.push_back(&b);
        return buildWalkGraph(roads, features, buildings, footprints, grid, anchor, props);
    }
};
double nearestLink(const WalkGraph& g, double x, double z) {
    double best = 1e300;
    for (const auto& l : g.links) {
        const auto &a = g.nodes[l.a], &b = g.nodes[l.b];
        const double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
        const double t = std::clamp(((x - a.x) * dx + (z - a.z) * dz) / l2, 0.0, 1.0);
        best = std::min(best, std::hypot(x - a.x - t * dx, z - a.z - t * dz));
    }
    return best;
}
}  // namespace

TEST(Crowd, people_walk_the_sidewalks_never_the_carriageway) {
    Street s;
    const auto doc = s.cook();
    const WalkGraph g = WalkGraph::from(doc);
    CHECK(!g.nodes.empty());
    std::set<int> sides;
    for (const auto& n : g.nodes) {
        // Half the carriageway (3 m) plus half an inferred 1.8 m sidewalk.
        NEAR(std::abs(n.z), 3.9, 0.05);
        sides.insert(n.z > 0 ? 1 : -1);
        // On the paving, which stands 21 cm above the terrain.
        NEAR(n.y, 0.21, 1e-6);
    }
    CHECK(sides.size() == 2);
    // The two ends of the street are crossed.
    int crossing = 0;
    for (const auto& l : g.links) crossing += l.crossing;
    CHECK(crossing >= 2);
    CHECK(doc["people"].get<int>() > 0);
    CHECK(doc["inferred"].get<std::string>().find("density") != std::string::npos);
}
TEST(Crowd, nobody_walks_through_a_building) {
    Street s;
    // A house across the southern sidewalk, 20 m wide.
    s.footprints.push_back({{-10, 2}, {10, 2}, {10, 12}, {-10, 12}});
    const WalkGraph g = WalkGraph::from(s.cook());
    for (const auto& n : g.nodes) CHECK(!(n.z > 2 && n.z < 12 && n.x > -10 && n.x < 10));
    for (const auto& l : g.links) {
        const auto &a = g.nodes[l.a], &b = g.nodes[l.b];
        for (int k = 1; k < 10; ++k) {
            const double x = a.x + (b.x - a.x) * k / 10, z = a.z + (b.z - a.z) * k / 10;
            CHECK(!(z > 2 && z < 12 && x > -10 && x < 10));
        }
    }
}
TEST(Crowd, the_open_country_holds_nobody) {
    Street s;
    s.roads = {way(1, {{-0.0009, 0.0}, {0.0009, 0.0}}, {{"highway", "track"}})};
    const auto doc = s.cook();
    CHECK(doc["people"].get<int>() == 0);
    CHECK(doc["nodes"].empty());
}
TEST(Crowd, shops_and_stops_bring_people_up_to_a_ceiling) {
    Street quiet, busy;
    for (int i = 0; i < 30; ++i)
        busy.buildingWays.push_back(way(100 + i, {{0, 0}, {0, 0}}, {{"building", "retail"}, {"shop", "bakery"}}));
    busy.features.push_back({7, 0.0, 0.0001, {{"highway", "bus_stop"}}});
    const int few = quiet.cook()["people"].get<int>(), many = busy.cook()["people"].get<int>();
    CHECK(many > few);
    CHECK(many <= kCrowdTileCeiling);
}
TEST(Crowd, the_street_empties_at_night) {
    CHECK(crowdHourFactor(3.5) < 0.1);
    NEAR(crowdHourFactor(12.5), 1.0, 1e-9);
    CHECK(crowdHourFactor(8.0) > crowdHourFactor(6.0));
    NEAR(crowdHourFactor(24.0), crowdHourFactor(0.0), 1e-12);
}
TEST(Crowd, a_bench_seats_two_facing_the_way_it_faces) {
    Street s;
    const double yaw = 0.6, scale = 1.6;
    s.props.push_back({{"name", "bench 42"},
                       {"transform", {{"position", {5.0, 0.0, 8.0}}, {"rotation", {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}},
                                      {"scale", {scale, scale, scale}}}}});
    s.props.push_back({{"name", "street lamp 43"},
                       {"transform", {{"position", {0.0, 0.0, 0.0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}});
    const WalkGraph g = WalkGraph::from(s.cook());
    CHECK(g.seats.size() == 2);
    for (const auto& seat : g.seats) {
        NEAR(seat.yaw, yaw, 1e-4);
        NEAR(seat.y, 0.237 * scale, 1e-3);
        CHECK(std::hypot(seat.x - 5.0, seat.z - 8.0) < 0.5);
    }
}
TEST(Crowd, walkers_stay_on_their_paths_and_appear_out_of_sight) {
    Street s;
    s.roads.push_back(way(2, {{0.0, -0.0009}, {0.0, 0.0009}}, {{"highway", "residential"}, {"width", "6"}}));
    const WalkGraph g = WalkGraph::from(s.cook());
    Crowd crowd;
    crowd.reset(&g, 7, {{1.0, 2.4}, {1.2, 2.3}});
    crowd.setPopulation(12);
    Crowd::Scene scene;
    scene.eyeX = 0; scene.eyeZ = 60; scene.faceX = 0; scene.faceZ = -1;
    std::vector<bool> seen(12, false);
    for (int frame = 0; frame < 3000; ++frame) {
        crowd.update(1.0 / 30, scene);
        const auto& ws = crowd.walkers();
        for (size_t i = 0; i < ws.size(); ++i) {
            if (!ws[i].alive) continue;
            if (!seen[i]) {
                // A newcomer is never close in front of the eye.
                const double dx = ws[i].x - scene.eyeX, dz = ws[i].z - scene.eyeZ;
                CHECK(std::hypot(dx, dz) >= Crowd::kSpawnNear - 1e-6);
                seen[i] = true;
            }
            if (ws[i].activity != Activity::Sit && ws[i].link >= 0) CHECK(nearestLink(g, ws[i].x, ws[i].z) < 0.8);
        }
        for (size_t i = 0; i < ws.size(); ++i) if (!ws[i].alive) seen[i] = false;
    }
    CHECK(crowd.live() > 6);
}
TEST(Crowd, a_car_coming_through_scatters_the_people_near_it) {
    Street s;
    const WalkGraph g = WalkGraph::from(s.cook());
    Crowd crowd;
    crowd.reset(&g, 3, {{1.0, 2.4}});
    crowd.setPopulation(20);
    Crowd::Scene scene;
    scene.eyeX = 0; scene.eyeZ = 100;  // far enough that they spawn in view
    for (int frame = 0; frame < 600; ++frame) crowd.update(1.0 / 30, scene);
    CHECK(crowd.live() > 0);
    const Walker* someone = nullptr;
    for (const auto& w : crowd.walkers()) if (w.alive && w.activity != Activity::Sit) { someone = &w; break; }
    CHECK(someone);
    scene.carX = someone->x + 2; scene.carZ = someone->z; scene.carSpeed = 12;
    crowd.update(1.0 / 30, scene);
    int fleeing = 0;
    for (const auto& w : crowd.walkers()) fleeing += w.alive && w.activity == Activity::Flee;
    CHECK(fleeing >= 1);
    CHECK(std::string(clipOf(Activity::Flee)) == "run");
}
TEST(Crowd, the_same_seed_walks_the_same_crowd) {
    Street s;
    const WalkGraph g = WalkGraph::from(s.cook());
    Crowd a, b;
    a.reset(&g, 11, {{1.0, 2.4}});
    b.reset(&g, 11, {{1.0, 2.4}});
    a.setPopulation(8); b.setPopulation(8);
    Crowd::Scene scene;
    scene.eyeZ = 100;
    for (int frame = 0; frame < 900; ++frame) { a.update(1.0 / 30, scene); b.update(1.0 / 30, scene); }
    for (size_t i = 0; i < a.walkers().size(); ++i) {
        CHECK(a.walkers()[i].alive == b.walkers()[i].alive);
        CHECK(a.walkers()[i].x == b.walkers()[i].x && a.walkers()[i].z == b.walkers()[i].z);
    }
}
