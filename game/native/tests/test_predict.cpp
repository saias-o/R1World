// The predictive model of details: each rule on the smallest scene that
// shows it, and the arbitration that keeps what is measured above it.
#include "check.hpp"

#include "gen/predict.hpp"

#include <cmath>

using namespace r1;

namespace {
// A flat piece of the Loir-et-Cher: open country, French roads.
constexpr double kLon = 1.30, kLat = 47.60;
const double kPerLat = 111320.0, kPerLon = 111320.0 * std::cos(kLat * kPi / 180);

struct Scene {
    OsmData osm;
    Tile tile = tileAt(kLon, kLat);
    Anchor anchor = Anchor::at(tile.center().x, tile.center().y, 0);
    int64_t next = 1000;
    Scene() { osm.country = "FR"; }
    // Metres east and north of the tile's centre.
    P2 at(double east, double north) const {
        return {tile.center().x + east / kPerLon, tile.center().y + north / kPerLat};
    }
    OsmWay& road(std::vector<P2> points, Tags tags) {
        osm.roads.push_back({next++, std::move(points), std::move(tags)});
        return osm.roads.back();
    }
    // A roundabout of radius 20 m at (east, north); its vertex at `angle`
    // degrees (0 east, counter-clockwise) is where roads meet it.
    std::vector<P2> ring(double east, double north) {
        std::vector<P2> pts;
        for (int k = 0; k <= 12; ++k) {
            const double a = kTau * (k % 12) / 12;  // anticlockwise, as France drives them
            pts.push_back(at(east + 20 * std::cos(a), north + 20 * std::sin(a)));
        }
        road(pts, {{"highway", "secondary"}, {"junction", "roundabout"}});
        return pts;
    }
    void town() {
        for (int i = 0; i < 40; ++i) {
            const P2 c = at(-120 + (i % 8) * 30, -60 + (i / 8) * 30);
            osm.buildings.push_back({next++, {c, {c.x + 0.0001, c.y}, {c.x + 0.0001, c.y + 0.0001}, c}, {{"building", "yes"}}});
        }
    }
    // A village east of the tile's centre, its houses addressed to `city`.
    void village(const std::string& city) {
        for (int i = 0; i < 30; ++i) {
            const P2 c = at(100 + (i % 6) * 30, -60 + (i / 6) * 30);
            Tags tags{{"building", "house"}};
            if (!city.empty()) tags["addr:city"] = city;
            osm.buildings.push_back({next++, {c, {c.x + 0.0001, c.y}, {c.x + 0.0001, c.y + 0.0001}, c}, tags});
        }
    }
    PredictOutput run(const std::optional<Bounds>& extent = std::nullopt) {
        const Anchor a = anchor;
        return predictDetails(osm, tile, anchor, [a](double lon, double lat) { return a.toEngine(lon, lat, 0.0); }, extent);
    }
};

struct Sign { std::string what; P2 xz; P2 faces; };
std::vector<Sign> signs(const PredictOutput& out, const std::string& what = "") {
    std::vector<Sign> found;
    for (const auto& n : out.nodes) {
        const std::string name = n["name"];
        const std::string code = name.substr(5, name.find(' ', 5) - 5);
        if (!what.empty() && code != what) continue;
        const auto& t = n["transform"];
        const double yaw = 2 * std::atan2(t["rotation"][1].get<double>(), t["rotation"][3].get<double>());
        found.push_back({code, {t["position"][0], t["position"][2]}, {std::sin(yaw), std::cos(yaw)}});
    }
    return found;
}
int placed(const PredictOutput& out, const char* rule) { return out.stats["rules"][rule]["placed"]; }
}  // namespace

TEST(Predict, a_departementale_leaving_a_roundabout_in_the_country_is_a_priority_road_without_its_80) {
    Scene s;
    const auto ring = s.ring(-100, 0);
    // East exit onto the D 12, 80 km/h: the default, so no sign says it.
    s.road({ring[0], s.at(0, 0), s.at(150, 0)}, {{"highway", "secondary"}, {"ref", "D 12"}, {"maxspeed", "80"}});
    s.road({ring[6], s.at(-250, 0)}, {{"highway", "unclassified"}});
    const auto out = s.run();
    CHECK(out.stats["jurisdiction"] == "FR");
    const auto ab6 = signs(out, "FR:AB6");
    CHECK_MSG(ab6.size() == 1, out.stats.dump());
    // 60 m down the road from the exit, on the right of eastbound traffic
    // (south: engine +z), its face turned back towards the roundabout.
    const P3 exit = s.anchor.toEngine(ring[0].x, ring[0].y, 0);
    NEAR(ab6[0].xz.x, exit.x + 60, 1.0);
    CHECK(ab6[0].xz.y > 3 && ab6[0].xz.y < 8);
    CHECK(ab6[0].faces.x < -0.99);
    CHECK(signs(out, "FR:B14[80]").empty());
    // Both entries give way to the ring, each on its own traffic's right.
    const auto give = signs(out, "FR:AB3a");
    CHECK(give.size() == 2);
    for (const Sign& g : give) CHECK(std::abs(g.faces.x) > 0.99);
}

TEST(Predict, in_town_the_roundabout_exit_carries_no_priority_diamond) {
    Scene s;
    s.town();
    const auto ring = s.ring(-100, 0);
    s.road({ring[0], s.at(150, 0)}, {{"highway", "secondary"}, {"ref", "D 12"}});
    const auto out = s.run();
    CHECK(signs(out, "FR:AB6").empty());
    CHECK(placed(out, "fr.roundabout.give_way") == 1);
}

TEST(Predict, a_signed_limit_is_posted_after_the_junction_and_the_default_never_is) {
    Scene s;
    s.road({s.at(-200, 0), s.at(0, 0)}, {{"highway", "secondary"}, {"maxspeed", "80"}});
    s.road({s.at(0, 0), s.at(200, 0)}, {{"highway", "secondary"}, {"maxspeed", "70"}});
    s.road({s.at(0, 0), s.at(0, -200)}, {{"highway", "unclassified"}});
    const auto out = s.run();
    const auto seventy = signs(out, "FR:B14[70]");
    CHECK_MSG(seventy.size() == 1, out.stats.dump());
    CHECK(seventy[0].xz.x > 10 && seventy[0].faces.x < -0.99);  // eastbound traffic reads it
    CHECK(signs(out, "FR:B14[80]").empty());
}

TEST(Predict, a_hump_has_its_warning_and_its_30_before_it_both_ways) {
    Scene s;
    s.road({s.at(-200, 0), s.at(0, 0), s.at(200, 0)}, {{"highway", "tertiary"}, {"maxspeed", "50"}});
    s.osm.features.push_back({s.next++, s.at(0, 0).x, s.at(0, 0).y, {{"traffic_calming", "hump"}}});
    const auto out = s.run();
    const auto warn = signs(out, "FR:A2b,FR:B14[30]");
    CHECK_MSG(warn.size() == 2, out.stats.dump());
    for (const Sign& w : warn) {
        // Before the hump, facing away from it: towards the traffic coming.
        CHECK(std::abs(w.xz.x) > 40);
        CHECK(w.faces.x * w.xz.x > 0.99 * std::abs(w.xz.x));
    }
}

TEST(Predict, a_surveyed_give_way_silences_the_guess) {
    Scene s;
    const auto ring = s.ring(-100, 0);
    s.road({ring[6], s.at(-250, 0)}, {{"highway", "unclassified"}});
    const P2 line = s.at(-124, 0);
    s.osm.features.push_back({s.next++, line.x, line.y, {{"highway", "give_way"}}});
    const auto out = s.run();
    // The surveyed one is drawn where it was mapped; no guess beside it.
    const auto give = signs(out, "FR:AB3a");
    CHECK(give.size() == 1);
    CHECK(out.stats["rules"]["survey"]["placed"] == 1);
    CHECK(out.stats["rules"]["fr.roundabout.give_way"]["silencedBySurvey"] == 1);
    // It faces the traffic driving towards the ring (east): it looks west.
    CHECK(give[0].faces.x < -0.9);
}

TEST(Predict, an_expressway_crossing_a_lane_without_a_node_passes_over_it) {
    Scene s;
    const auto& motorway = s.road({s.at(-200, -10), s.at(200, 10)}, {{"highway", "motorway"}});
    const int64_t over = motorway.id;
    const auto& lane = s.road({s.at(0, -150), s.at(0, 150)}, {{"highway", "residential"}});
    const int64_t under = lane.id;
    auto out = s.run();
    const auto& structures = out.stats["structures"];
    CHECK_MSG(structures.size() == 1, out.stats.dump());
    CHECK(structures[0]["kind"] == "bridge");
    CHECK(structures[0]["over"] == over && structures[0]["under"] == under);
    // Where OSM already says how they cross, nothing is guessed.
    s.osm.roads[0].tags["bridge"] = "yes";
    CHECK(s.run().stats["structures"].empty());
}

TEST(Predict, a_country_without_a_rulebook_gets_only_the_rules_that_hold_everywhere) {
    Scene s;
    s.osm.country = "DE";
    const auto ring = s.ring(-100, 0);
    s.road({ring[0], s.at(150, 0)}, {{"highway", "secondary"}});
    const auto out = s.run();
    CHECK(out.nodes.empty());
    CHECK(out.stats["jurisdiction"] == "DE" && out.stats["rulebook"] == false);
    CHECK(out.stats["rules"].contains("any.grade_separation"));
    CHECK(!out.stats["rules"].contains("fr.roundabout.give_way"));
}

TEST(Predict, the_country_comes_from_osm_first_and_traffic_side_follows_it) {
    OsmData osm;
    osm.country = "GB";
    const Jurisdiction gb = jurisdictionAt(osm, 1.3, 47.6);  // a point in France: OSM's word wins
    CHECK(gb.code == "GB" && !gb.rightHand && !gb.rulebook);
    osm.country.clear();
    const Jurisdiction fr = jurisdictionAt(osm, 1.3, 47.6);
    CHECK(fr.code == "FR" && fr.rightHand && fr.rulebook);
    CHECK(jurisdictionAt(osm, 13.4, 52.5).code.empty());
}

TEST(Predict, every_sign_a_french_rule_can_propose_has_a_model_on_both_mounts) {
    for (const char* code : {"FR:AB3a", "FR:AB6", "FR:B30", "FR:A2b,FR:B14[30]", "FR:B14[50]", "FR:B14[70]", "FR:B14[90]"}) {
        const auto it = palette().signs.find(code);
        CHECK_MSG(it != palette().signs.end(), code);
        CHECK(it->second.mounts.count("rural") && it->second.mounts.count("urban"));
    }
}

TEST(Predict, a_town_is_named_at_its_gates_in_both_directions) {
    Scene s;
    s.village("Montrichard Val de Cher");
    s.road({s.at(-270, 0), s.at(270, 0)}, {{"highway", "tertiary"}});
    const auto out = s.run();
    const auto in = signs(out, "FR:EB10"), away = signs(out, "FR:EB20");
    CHECK_MSG(in.size() == 1 && away.size() == 1, out.stats.dump());
    // West of the village, where its houses begin: entering eastwards, the
    // sign looks west at the traffic; leaving, the struck-through one east.
    CHECK(in[0].xz.x < 100 && in[0].xz.x > -60);
    CHECK(in[0].faces.x < -0.99 && away[0].faces.x > 0.99);
    CHECK(out.stats["townsNamed"]["Montrichard Val de Cher"] == 2);
    for (const auto& n : out.nodes) {
        const std::string name = n["name"];
        if (name.find("FR:EB") == std::string::npos) continue;
        // Posts, the plate's three parts, a letter per letter, a bar on the way out.
        const size_t letters = std::string("MONTRICHARDVALDECHER").size();
        const bool exit = name.find("FR:EB20") != std::string::npos;
        CHECK(n["children"].size() == 2 + 3 + letters + (exit ? 1 : 0));
    }
}

TEST(Predict, a_town_its_houses_do_not_name_gets_no_sign) {
    Scene s;
    s.village("");
    s.road({s.at(-270, 0), s.at(270, 0)}, {{"highway", "tertiary"}});
    CHECK(signs(s.run(), "FR:EB10").empty());
}

TEST(Predict, where_the_observations_stop_a_town_does_not_end) {
    Scene s;
    s.village("Chissay");
    s.road({s.at(-270, 0), s.at(270, 0)}, {{"highway", "tertiary"}});
    // The answer was asked about a box that starts where the village does:
    // what lies west of it was never seen.
    Bounds box = s.tile.bounds();
    box.west = s.at(60, 0).x;
    CHECK(signs(s.run(box), "FR:EB10").empty());
}
