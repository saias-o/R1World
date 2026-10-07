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
    std::vector<OsmWay> buildingWays = {way(9, {{0.0, 0.0001}}, {{"building", "apartments"}, {"building:levels", "2"}})};
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
TEST(Crowd, an_empty_rural_road_has_no_crowd_even_with_a_sidewalk) {
    Street s;
    s.buildingWays.clear();
    const auto doc = s.cook();
    CHECK(!doc["links"].empty());
    CHECK(doc["people"] == 0);
    for (const auto& link : doc["links"]) CHECK(link[3].get<double>() == 0);
}
TEST(Crowd, compact_housing_and_apartments_raise_local_demand) {
    Street sparse, compact, flats;
    sparse.buildingWays.clear(); compact.buildingWays.clear(); flats.buildingWays.clear();
    for (int i = 0; i < 4; ++i) {
        sparse.buildingWays.push_back(way(100 + i, {{-0.0008 + i * 0.0005, 0.00015}}, {{"building", "house"}}));
        compact.buildingWays.push_back(way(100 + i, {{0.00003 * i, 0.00015}}, {{"building", "house"}}));
    }
    flats.buildingWays.push_back(way(200, {{0.0, 0.00015}}, {{"building", "apartments"}, {"building:levels", "6"}}));
    const auto a = sparse.cook(), b = compact.cook(), c = flats.cook();
    CHECK(b["people"].get<int>() > a["people"].get<int>());
    CHECK(c["people"].get<int>() > a["people"].get<int>());
    const WalkGraph graph = WalkGraph::from(b);
    double near = 0, far = 0;
    for (const auto& link : graph.links) {
        const auto& n = graph.nodes[link.a];
        if (std::abs(n.x) < 30) near = std::max(near, link.demand);
        if (std::abs(n.x) > 85) far = std::max(far, link.demand);
    }
    CHECK(near > far);
    CHECK(far < near * 0.5);
    Crowd crowd;
    crowd.reset(&graph, 31, {{1.0, 2.4}});
    crowd.setPopulation(8);
    Crowd::Scene scene;
    scene.eyeZ = 100;
    for (int frame = 0; frame < 120; ++frame) crowd.update(1.0 / 30, scene);
    CHECK(crowd.live() > 0);
    for (const auto& walker : crowd.walkers())
        if (walker.alive && walker.link >= 0) CHECK(graph.links[walker.link].demand > 0);
}
TEST(Crowd, dispersed_houses_do_not_fill_a_country_road) {
    Street s;
    s.buildingWays.clear();
    for (int i = 0; i < 4; ++i)
        s.buildingWays.push_back(way(100+i, {{-.0008+i*.0005,.00015}}, {{"building","house"}}));
    CHECK(s.cook()["people"].get<int>() <= 1);
}
TEST(Crowd, only_the_local_share_of_walkers_is_simulated) {
    WalkGraph g;
    g.people = 20;
    g.nodes = {{-500,0,0,{}}, {500,0,0,{}}};
    g.links = {{0,1,false,1000,1}};
    NEAR(g.populationNear(0,0,100),4,1e-9);
    NEAR(g.populationNear(500,0,100),2,1e-9);
    NEAR(g.populationNear(0,101,100),0,1e-9);
    NEAR(g.populationNear(0,0,1000),20,1e-9);
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

// ── meeting the player ──────────────────────────────────────────────────────

namespace {
// A street's crowd a while after it filled up, the eye far off.
Crowd settled(const WalkGraph& g, uint32_t seed, int people) {
    Crowd crowd;
    crowd.reset(&g, seed, {{1.0, 2.4}});
    crowd.setPopulation(people);
    Crowd::Scene scene;
    scene.eyeZ = 100;
    for (int frame = 0; frame < 600; ++frame) crowd.update(1.0 / 30, scene);
    return crowd;
}
// Someone on a sidewalk, away from the street's ends and from everyone else.
int alone(const Crowd& crowd, bool walking, const std::set<int>& skip = {}, double room = 4.0) {
    const auto& ws = crowd.walkers();
    for (size_t i = 0; i < ws.size(); ++i) {
        const Walker& w = ws[i];
        if (!w.alive || w.link < 0 || std::abs(w.x) > 60 || skip.count(int(i))) continue;
        if (walking != (w.activity == Activity::Walk)) continue;
        if (!walking && w.activity != Activity::Idle && w.activity != Activity::Wait && w.activity != Activity::Phone) continue;
        bool crowded = false;
        for (size_t o = 0; o < ws.size(); ++o)
            crowded = crowded || (o != i && ws[o].alive && std::hypot(ws[o].x - w.x, ws[o].z - w.z) < room);
        if (!crowded) return int(i);
    }
    return -1;
}
}  // namespace

// The contact itself is the engine's (the player's capsule meeting theirs in
// Jolt, native/world.cpp); what follows it is the crowd's.
TEST(Crowd, walking_into_someone_staggers_them_and_they_answer_it) {
    Street s;
    const WalkGraph g = WalkGraph::from(s.cook());
    Crowd crowd = settled(g, 5, 30);
    const int i = alone(crowd, false);
    CHECK(i >= 0);
    const Walker& w = crowd.walkers()[i];
    const double x0 = w.x, z0 = w.z;
    // The player, jogging east into them from just west.
    Crowd::Scene scene;
    scene.eyeZ = 100;
    scene.playerX = x0 - 2 * Crowd::kBodyRadius; scene.playerZ = z0;
    CHECK(crowd.bump(size_t(i), 1.0, 0.0, 2.8));
    crowd.update(1.0 / 30, scene);
    CHECK(w.bumps == 1u);
    CHECK(w.activity == Activity::Stagger);
    CHECK(w.look == 1.0);
    // The contacts of the next frames are the same bump, not new ones.
    CHECK(!crowd.bump(size_t(i), 1.0, 0.0, 2.8));
    // Carried on east, off their line, and they answer.
    for (int frame = 0; frame < 20; ++frame) crowd.update(1.0 / 30, scene);
    CHECK(w.x - x0 > 0.12);
    CHECK(w.activity == Activity::Shrug || w.activity == Activity::Angry);
    CHECK(std::string(clipOf(w.activity)) == (w.activity == Activity::Shrug ? "shrug" : "angry"));
    // Facing him to say so.
    CHECK(std::sin(w.heading) < -0.9);
    // Then back to what they were doing.
    for (int frame = 0; frame < 30 * 6; ++frame) crowd.update(1.0 / 30, scene);
    CHECK(w.activity != Activity::Stagger && w.activity != Activity::Shrug && w.activity != Activity::Angry);
    CHECK(w.bumps == 1u);
}
TEST(Crowd, leaning_on_someone_or_a_sitter_is_no_bump) {
    Street s;
    const WalkGraph g = WalkGraph::from(s.cook());
    Crowd crowd = settled(g, 5, 30);
    const int i = alone(crowd, false);
    CHECK(i >= 0);
    CHECK(!crowd.bump(size_t(i), 1.0, 0.0, Crowd::kBumpSpeed * 0.9));
    CHECK(crowd.walkers()[i].bumps == 0u);
    CHECK(!crowd.bump(crowd.walkers().size() + 3, 1.0, 0.0, 5.0));
}
TEST(Crowd, a_sprint_knocks_someone_further_and_is_not_shrugged_off) {
    Street s;
    const WalkGraph g = WalkGraph::from(s.cook());
    double carried[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        Crowd crowd = settled(g, 5, 30);
        const int i = alone(crowd, false);
        const Walker& w = crowd.walkers()[i];
        const double x0 = w.x;
        Crowd::Scene scene;
        scene.eyeZ = 100;
        scene.playerX = x0 - 2 * Crowd::kBodyRadius; scene.playerZ = w.z;
        CHECK(crowd.bump(size_t(i), 1.0, 0.0, run == 0 ? 2.8 : 7.0));
        for (int frame = 0; frame < 31; ++frame) crowd.update(1.0 / 30, scene);
        carried[run] = w.x - x0;
        if (run == 1) CHECK(w.activity == Activity::Angry || w.activity == Activity::Dust);
    }
    CHECK(carried[1] > 2.0 * carried[0]);
    CHECK(carried[1] < 1.2);  // a stagger, not a flight
}
TEST(Crowd, a_walker_closes_on_whoever_is_ahead_of_them) {
    Street s;
    const WalkGraph g = WalkGraph::from(s.cook());
    Crowd crowd = settled(g, 13, 30);
    const int i = alone(crowd, true);
    CHECK(i >= 0);
    const Walker& w = crowd.walkers()[i];
    const double hx = std::sin(w.heading), hz = -std::cos(w.heading);
    NEAR(crowd.speedAlong(size_t(i), hx, hz), 1.0, 1e-9);
    NEAR(crowd.speedAlong(size_t(i), -hz, hx), 0.0, 1e-9);
}
TEST(Crowd, someone_walking_at_a_player_who_stands_there_steps_round_him) {
    WalkGraph g;
    g.nodes = {{-100, 0, 0, {0}}, {100, 0, 0, {0}}};
    g.links = {{0, 1, false, 200, 1}};
    Crowd crowd = settled(g, 13, 1);
    const int i = alone(crowd, true);
    CHECK(i >= 0);
    const Walker& w = crowd.walkers()[i];
    const double hx = std::sin(w.heading), hz = -std::cos(w.heading);
    Crowd::Scene scene;
    scene.eyeZ = 100;
    scene.playerX = w.x + 3.5 * hx; scene.playerZ = w.z + 3.5 * hz;
    double closest = 1e9;
    for (int frame = 0; frame < 30 * 7; ++frame) {
        crowd.update(1.0 / 30, scene);
        closest = std::min(closest, std::hypot(w.x - scene.playerX, w.z - scene.playerZ));
    }
    CHECK(w.bumps == 0u);
    CHECK(closest >= 2 * Crowd::kBodyRadius + 0.2);
    // Past him and back on their line.
    CHECK((w.x - scene.playerX) * hx + (w.z - scene.playerZ) * hz > 1.0 || w.activity != Activity::Walk);
}
TEST(Crowd, people_look_up_at_the_player_passing_in_front_not_behind) {
    Street s;
    const WalkGraph g = WalkGraph::from(s.cook());
    Crowd crowd = settled(g, 21, 40);
    int looked = 0, behind = 0, tried = 0;
    std::set<int> done;
    for (int attempt = 0; attempt < 12; ++attempt) {
        int i = alone(crowd, true, done, 1.5);
        if (i < 0) i = alone(crowd, false, done, 1.5);
        if (i < 0) break;
        done.insert(i);
        ++tried;
        const Walker& w = crowd.walkers()[i];
        Crowd::Scene scene;
        scene.eyeZ = 100;
        // The player `ahead` metres in front of them (behind if negative),
        // `t` to their right, crossing at 2.8 m/s: their frame is taken
        // afresh each frame, since a walker turns at a corner.
        auto put = [&](double ahead, double t, double speed) {
            const double hx = std::sin(w.heading), hz = -std::cos(w.heading), rx = -hz, rz = hx;
            scene.playerX = w.x + ahead * hx + t * rx; scene.playerZ = w.z + ahead * hz + t * rz;
            scene.playerVX = speed * rx; scene.playerVZ = speed * rz;
        };
        // Behind them first: nobody looks round at footsteps.
        bool sawBehind = false;
        for (int frame = 0; frame < 45; ++frame) {
            put(-3.0, -2.0 + 4.0 * frame / 45.0, 2.8);
            crowd.update(1.0 / 30, scene);
            sawBehind = sawBehind || w.look > 0;
        }
        behind += sawBehind;
        // Then across in front of them.
        bool saw = false;
        for (int frame = 0; frame < 45; ++frame) {
            put(3.0, -2.0 + 4.0 * frame / 45.0, 2.8);
            crowd.update(1.0 / 30, scene);
            saw = saw || w.look > 0;
        }
        looked += saw;
        // Gone behind them and out of reach: they let him go.
        for (int frame = 0; frame < 45; ++frame) {
            put(-15.0, 0.0, 0.0);
            crowd.update(1.0 / 30, scene);
        }
        CHECK(w.look == 0.0);
        // Out of the next one's way.
        scene.playerX = scene.playerZ = 1e9;
        crowd.update(1.0 / 30, scene);
    }
    CHECK(tried >= 3);
    CHECK(behind == 0);
    CHECK(looked * 2 >= tried);
}
