// Airports and military bases (gen/airports.cpp): what OSM traced is paved
// and painted, every aircraft on it is inferred and says so, stands keep their
// aircraft clear of the terminal and nose-in, a military base gets exactly one
// helicopter and a military airfield nothing else, and nothing parks in water.
// The airports are drawn in metres around one inland tile.
#include "check.hpp"

#include "gen/airports.hpp"
#include "gen/buildings.hpp"
#include "gen/palette.hpp"

#include <filesystem>

using namespace r1;

namespace {
const Tile kTile = tileAt(5.0, 45.0);
const P2 kCentre = kTile.center();

// A point `east` and `north` metres from the tile's centre, in (lon, lat).
P2 at(double east, double north) {
    return {kCentre.x + east / (111320.0 * std::cos(radians(kCentre.y))), kCentre.y + north / 111320.0};
}
OsmWay line(int64_t id, std::vector<P2> metres, Tags tags) {
    OsmWay w{id, {}, std::move(tags)};
    for (const P2& m : metres) w.points.push_back(at(m.x, m.y));
    return w;
}
// A closed box from (x0, y0) to (x1, y1), metres.
OsmWay box(int64_t id, double x0, double y0, double x1, double y1, Tags tags) {
    return line(id, {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {x0, y0}}, std::move(tags));
}
ElevationGrid flat(double h = 100.0) { return ElevationGrid{kTile.bounds(), 2, {h, h, h, h}}; }
AirportOutput cook(const OsmData& osm, std::function<bool(double, double)> wet = [](double, double) { return false; }) {
    return buildAirports(osm, kTile, flat(), Anchor::at(kCentre.x, kCentre.y, 0), wet);
}
const MeshPart* part(const AirportOutput& out, const std::string& name) {
    for (const auto& p : out.parts) if (p.name == name) return &p;
    return nullptr;
}

// An intercontinental runway north of a terminal, an apron between them and
// four stand lines running south to the terminal's face.
OsmData airport() {
    OsmData osm;
    osm.aeroways.push_back(line(1, {{-1600, 180}, {1600, 180}}, {{"aeroway", "runway"}, {"ref", "09/27"}, {"surface", "asphalt"}}));
    osm.aeroways.push_back(box(2, -200, -205, 200, -40, {{"aeroway", "apron"}}));
    osm.aeroways.push_back(line(3, {{-200, 60}, {200, 60}}, {{"aeroway", "taxiway"}}));
    for (int i = 0; i < 4; ++i) {
        const double x = -120.0 + i * 70.0;
        osm.aeroways.push_back(line(10 + i, {{x, -60}, {x, -196}}, {{"aeroway", "parking_position"}}));
    }
    osm.buildings.push_back(box(100, -220, -280, 220, -210, {{"building", "yes"}, {"aeroway", "terminal"}}));
    return osm;
}
}  // namespace

TEST(Airports, a_runway_is_paved_to_its_width_and_painted) {
    const AirportOutput out = cook(airport());
    const MeshPart* runway = part(out, "Runways");
    CHECK(runway && part(out, "Runway markings") && part(out, "Aprons") && part(out, "Taxiways"));
    double y0 = 1e30, y1 = -1e30, z0 = 1e30, z1 = -1e30;
    for (const P3& p : runway->mesh.positions) { y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); z0 = std::min(z0, p.z); z1 = std::max(z1, p.z); }
    // 3.2 km long, so 45 m wide by default, on the flat ground and just above it.
    NEAR(z1 - z0, 45.0, 0.05);
    CHECK(y0 > 100.0 && y1 < 100.2);
    CHECK(out.stats["runways"] == 1 && out.stats["runwaysUnpaved"] == 0);
}
TEST(Airports, the_designator_is_the_tag_where_the_tag_agrees_with_the_bearing) {
    CHECK(runwayDesignator("09L/27R", 88.0) == "09L");
    CHECK(runwayDesignator("09L/27R", 268.0) == "27R");
    CHECK(runwayDesignator("03/21", 41.0) == "03");  // magnetic, a few degrees off true
    CHECK(runwayDesignator("", 181.0) == "18");
    CHECK(runwayDesignator("", 3.0) == "36");
    CHECK(runwayDesignator("H1", 90.0) == "09");     // not a runway name: the bearing
}
TEST(Airports, a_grass_strip_is_left_as_grass) {
    OsmData osm;
    osm.aeroways.push_back(line(1, {{-400, 0}, {400, 0}}, {{"aeroway", "runway"}, {"surface", "grass"}}));
    const AirportOutput out = cook(osm);
    CHECK(!part(out, "Runways") && out.stats["runwaysUnpaved"] == 1);
}
TEST(Airports, stands_park_airliners_nose_in_and_clear_of_the_terminal) {
    const AirportOutput out = cook(airport());
    CHECK_MSG(!out.aircraft.empty(), out.stats.dump());
    for (const auto& a : out.aircraft) {
        CHECK(a["inferred"].get<bool>());
        CHECK(a["source"].get<std::string>().find("stand line") != std::string::npos);
        // Nose south, into the terminal.
        NEAR(a["heading"].get<double>(), 180.0, 1.0);
        const AircraftType* type = aircraftType(a["type"]);
        CHECK(type && type->klass != "helicopter");
        // The terminal's face is 210 m south of the centre; the nose stops short of it.
        const double noseNorth = -a["z"].get<double>() - type->length / 2;
        CHECK_MSG(noseNorth > -210.0, a.dump());
        CHECK(a["y"].get<double>() > 100.0);  // on the apron, not under it
    }
    // An intercontinental runway: no business jet on an airliner's stand.
    for (const auto& a : out.aircraft) CHECK(a["type"] != "bizjet");
}
TEST(Airports, the_same_airport_parks_the_same_aircraft) {
    CHECK(cook(airport()).aircraft == cook(airport()).aircraft);
}
TEST(Airports, a_military_base_gets_one_helicopter_and_a_barracks_inside_it_is_the_same_base) {
    OsmData osm;
    osm.military.push_back(box(1, -250, -200, 250, 200, {{"landuse", "military"}}));
    osm.military.push_back(box(2, -80, -60, 80, 60, {{"military", "barracks"}}));
    osm.buildings.push_back(box(3, -30, -20, 30, 20, {{"building", "yes"}}));
    const AirportOutput out = cook(osm);
    CHECK_MSG(out.aircraft.size() == 1, out.aircraft.dump());
    CHECK(out.aircraft[0]["type"] == "helicopter" && out.aircraft[0]["source"].get<std::string>().find("military base") != std::string::npos);
    // On clear ground: not on the building in the middle of the base.
    const double x = out.aircraft[0]["x"], z = out.aircraft[0]["z"];
    CHECK(!(std::abs(x) < 30 + 6 && std::abs(z) < 20 + 6));
    CHECK(out.stats["militaryHelicopters"] == 1);
}
TEST(Airports, a_military_airfield_parks_a_helicopter_and_no_airliner) {
    OsmData osm = airport();
    osm.military.push_back(box(500, -2000, -600, 2000, 600, {{"landuse", "military"}, {"military", "airfield"}}));
    const AirportOutput out = cook(osm);
    CHECK_MSG(out.aircraft.size() == 1, out.aircraft.dump());
    CHECK(out.aircraft[0]["type"] == "helicopter");
    CHECK(part(out, "Runways"));  // it is still paved and painted
}
TEST(Airports, a_danger_area_is_not_a_base) {
    OsmData osm;
    osm.military.push_back(box(1, -250, -200, 250, 200, {{"military", "danger_area"}}));
    CHECK(cook(osm).aircraft.empty());
}
TEST(Airports, nothing_parks_in_the_water) {
    OsmData osm;
    osm.military.push_back(box(1, -250, -200, 250, 200, {{"landuse", "military"}}));
    CHECK(cook(osm, [](double, double) { return true; }).aircraft.empty());
}
TEST(Airports, a_tile_with_nothing_aeronautical_costs_nothing) {
    OsmData osm;
    osm.buildings.push_back(box(1, -30, -20, 30, 20, {{"building", "yes"}}));
    const AirportOutput out = cook(osm);
    CHECK(out.parts.empty() && out.aircraft.empty());
}
TEST(Airports, every_aircraft_is_on_disk_and_says_how_it_flies) {
    CHECK(palette().aircraft.size() >= 4);
    for (const auto& a : palette().aircraft) {
        CHECK_MSG(std::filesystem::exists(r1test::gameRoot() + "/" + a.nearModel), a.nearModel);
        CHECK_MSG(std::filesystem::exists(r1test::gameRoot() + "/" + a.farModel), a.farModel);
        CHECK(a.length > 5 && a.span > 5 && a.cg > 0 && a.top > 20);
        if (a.klass == "helicopter") CHECK(a.climb > 0);
        else CHECK(a.stall > 0 && a.stall < a.rotate && a.rotate < a.top && a.turnRadius > 0);
    }
    // The three handlings the request asked for, each its own.
    CHECK(aircraftType("widebody") && aircraftType("bizjet") && aircraftType("helicopter"));
    CHECK(aircraftType("bizjet")->rollRate > 2 * aircraftType("widebody")->rollRate);
}

// ── the terminal and the hangar ─────────────────────────────────────────────

TEST(Airports, a_hangar_is_a_steel_shed_unless_its_tags_say_otherwise) {
    static std::vector<OsmWay> keep;
    keep = {OsmWay{1, {{0, 0}, {60, 0}, {60, 40}, {0, 40}, {0, 0}}, {{"building", "hangar"}}},
            OsmWay{2, {{100, 0}, {160, 0}, {160, 40}, {100, 40}, {100, 0}}, {{"building", "hangar"}, {"height", "21"}}},
            OsmWay{3, {{200, 0}, {260, 0}, {260, 40}, {200, 40}, {200, 0}}, {{"building", "yes"}, {"aeroway", "terminal"}}}};
    std::vector<const OsmWay*> ways;
    for (const auto& w : keep) ways.push_back(&w);
    const BuildingOutput out = buildBuildings(ways, [](double x, double z) { return P3{x, 0.0, z}; }, profileByKey("PARIS"),
                                              {0, 0}, -1.0, 0.0, nullptr, nullptr);
    CHECK(out.tops.size() == 3);
    const double parapet = profileByKey("PARIS").parapetHeight;
    NEAR(out.tops[0], 12.0 + parapet, 1e-9);  // inferred: a hangar's height, a flat roof
    NEAR(out.tops[1], 21.0 + parapet, 1e-9);  // measured: the tag wins (rule 4), roof or none
    NEAR(out.tops[2], 15.0 + parapet, 1e-9);
    bool steel = false, glass = false;
    for (const auto& p : out.parts) {
        steel |= p.name.find("corrugated") != std::string::npos;
        glass |= p.name.find("Terminal glazing") != std::string::npos;
    }
    CHECK(steel && glass);
    CHECK(out.stats.heightInferred == 2 && out.stats.heightMeasured == 1);
}
