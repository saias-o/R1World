// The United States rulebook (MUTCD), each rule on the smallest scene that
// shows it, and the surveyed signs drawn where OSM mapped them.
#include "check.hpp"

#include "gen/predict.hpp"

#include <cmath>

using namespace r1;

namespace {
// Open Kansas: a grid of section roads, as flat as a map.
constexpr double kLon = -97.50, kLat = 38.50;
const double kPerLat = 111320.0, kPerLon = 111320.0 * std::cos(kLat * kPi / 180);

struct Scene {
    OsmData osm;
    Tile tile = tileAt(kLon, kLat);
    Anchor anchor = Anchor::at(tile.center().x, tile.center().y, 0);
    int64_t next = 1000;
    Scene() { osm.country = "US"; }
    P2 at(double east, double north) const { return {tile.center().x + east / kPerLon, tile.center().y + north / kPerLat}; }
    void road(std::vector<P2> points, Tags tags) { osm.roads.push_back({next++, std::move(points), std::move(tags)}); }
    void node(P2 p, Tags tags) { osm.features.push_back({next++, p.x, p.y, std::move(tags)}); }
    PredictOutput run() {
        const Anchor a = anchor;
        return predictDetails(osm, tile, anchor, [a](double lon, double lat) { return a.toEngine(lon, lat, 0.0); });
    }
};

struct Sign { std::string what, name; P2 xz; P2 faces; size_t children = 0; };
std::vector<Sign> signs(const PredictOutput& out, const std::string& what) {
    std::vector<Sign> found;
    for (const auto& n : out.nodes) {
        const std::string name = n["name"];
        const std::string code = name.substr(5, name.find(' ', 5) - 5);
        if (code != what) continue;
        const auto& t = n["transform"];
        const double yaw = 2 * std::atan2(t["rotation"][1].get<double>(), t["rotation"][3].get<double>());
        found.push_back({code, name, {t["position"][0], t["position"][2]}, {std::sin(yaw), std::cos(yaw)},
                         n.contains("children") ? n["children"].size() : 0});
    }
    return found;
}
}  // namespace

TEST(PredictUS, a_side_road_meeting_a_highway_stops_and_the_highway_does_not) {
    Scene s;
    s.road({s.at(-250, 0), s.at(0, 0), s.at(250, 0)}, {{"highway", "secondary"}});
    s.road({s.at(0, 0), s.at(0, -250)}, {{"highway", "residential"}});
    const auto out = s.run();
    CHECK(out.stats["jurisdiction"] == "US");
    const auto stops = signs(out, "US:R1-1");
    CHECK_MSG(stops.size() == 1, out.stats.dump());
    // South of the highway, on the right of northbound traffic (east), facing south.
    CHECK(stops[0].xz.y > 3 && stops[0].xz.x > 2);
    CHECK(stops[0].faces.y > 0.9);
}

TEST(PredictUS, a_crossing_with_lights_has_no_stop_sign) {
    Scene s;
    s.road({s.at(-250, 0), s.at(0, 0), s.at(250, 0)}, {{"highway", "secondary"}});
    s.road({s.at(0, 250), s.at(0, 0), s.at(0, -250)}, {{"highway", "residential"}});
    s.node(s.at(0, 0), {{"highway", "traffic_signals"}});
    const auto out = s.run();
    CHECK(signs(out, "US:R1-1").empty() && signs(out, "US:R1-1,US:R1-3P").empty());
}

TEST(PredictUS, two_equal_residential_streets_crossing_are_an_all_way_stop) {
    Scene s;
    s.road({s.at(-250, 0), s.at(0, 0), s.at(250, 0)}, {{"highway", "residential"}});
    s.road({s.at(0, 250), s.at(0, 0), s.at(0, -250)}, {{"highway", "residential"}});
    const auto out = s.run();
    CHECK_MSG(signs(out, "US:R1-1,US:R1-3P").size() == 4, out.stats.dump());
    CHECK(signs(out, "US:R1-1").empty());
}

TEST(PredictUS, a_speed_limit_is_posted_in_mph_where_it_changes) {
    Scene s;
    s.road({s.at(-250, 0), s.at(0, 0)}, {{"highway", "secondary"}, {"maxspeed", "35 mph"}});
    s.road({s.at(0, 0), s.at(250, 0)}, {{"highway", "secondary"}, {"maxspeed", "45 mph"}});
    const auto out = s.run();
    const auto up = signs(out, "US:R2-1[45]"), down = signs(out, "US:R2-1[35]");
    CHECK_MSG(up.size() == 1 && down.size() == 1, out.stats.dump());
    CHECK(up[0].xz.x > 5 && up[0].faces.x < -0.99);      // read driving east
    CHECK(down[0].xz.x < -5 && down[0].faces.x > 0.99);  // read driving west
}

TEST(PredictUS, a_one_way_street_ends_with_do_not_enter_for_the_wrong_way) {
    Scene s;
    s.road({s.at(-250, 0), s.at(0, 0), s.at(250, 0)}, {{"highway", "secondary"}});
    // Northbound one-way, ending at the secondary.
    s.road({s.at(0, -250), s.at(0, 0)}, {{"highway", "residential"}, {"oneway", "yes"}});
    const auto out = s.run();
    const auto dne = signs(out, "US:R5-1");
    CHECK_MSG(dne.size() == 1, out.stats.dump());
    // In the one-way, just off the crossing, facing whoever would turn in.
    CHECK(dne[0].xz.y > 0 && dne[0].faces.y < -0.9);
}

TEST(PredictUS, a_crossing_of_named_streets_has_its_blades_abbreviated) {
    Scene s;
    s.road({s.at(-250, 0), s.at(0, 0), s.at(250, 0)}, {{"highway", "tertiary"}, {"name", "Main Street"}});
    s.road({s.at(0, 250), s.at(0, 0), s.at(0, -250)}, {{"highway", "residential"}, {"name", "West 5th Avenue"}});
    const auto out = s.run();
    const auto blades = signs(out, "US:D3-1");
    CHECK_MSG(blades.size() == 1, out.stats.dump());
    CHECK(blades[0].name.find("MAIN ST / W 5TH AVE") != std::string::npos);
    CHECK(blades[0].children == 3);  // the post and two blades
    // On a corner, clear of both carriageways.
    CHECK(std::abs(blades[0].xz.x) > 3 && std::abs(blades[0].xz.y) > 3);
}

TEST(PredictUS, a_surveyed_stop_is_drawn_where_mapped_and_nothing_guessed_beside_it) {
    Scene s;
    s.road({s.at(-250, 0), s.at(0, 0), s.at(250, 0)}, {{"highway", "secondary"}});
    const P2 line = s.at(0, -6);
    s.road({s.at(0, 0), line, s.at(0, -250)}, {{"highway", "residential"}});
    s.node(line, {{"highway", "stop"}, {"direction", "backward"}});
    const auto out = s.run();
    const auto stops = signs(out, "US:R1-1");
    CHECK_MSG(stops.size() == 1, out.stats.dump());
    CHECK(stops[0].name.find("surveyed") != std::string::npos);
    CHECK(stops[0].faces.y > 0.9);  // read driving north, towards the highway
}

TEST(PredictUS, the_country_is_known_without_the_answer_saying_so) {
    OsmData osm;
    CHECK(jurisdictionAt(osm, -97.5, 38.5).code == "US");     // Kansas
    CHECK(jurisdictionAt(osm, -122.42, 37.77).code == "US");  // San Francisco
    CHECK(jurisdictionAt(osm, -149.9, 61.2).code == "US");    // Anchorage
    CHECK(jurisdictionAt(osm, -157.86, 21.3).code == "US");   // Honolulu
    CHECK(jurisdictionAt(osm, -79.38, 43.65).code.empty());   // Toronto
    CHECK(jurisdictionAt(osm, -99.13, 19.43).code.empty());   // Mexico City
    CHECK(jurisdictionAt(osm, -123.12, 49.28).code.empty());  // Vancouver
}
