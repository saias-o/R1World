#include "check.hpp"
#include "gen/fuel.hpp"
#include "gen/interiors.hpp"
#include "gen/predict.hpp"
#include "gen/streets.hpp"
using namespace r1;
namespace {
const Anchor kAnchor = Anchor::at(2.35, 48.85, 0);
OsmWay geoWay(int64_t id, const Ring& r, const Tags& t) {
    OsmWay w; w.id = id; w.tags = t;
    for (auto p : r) { auto g = kAnchor.toGeodetic(p.x, 0, p.y); w.points.push_back({g.x, g.y}); }
    w.points.push_back(w.points.front());
    return w;
}
OsmNode geoNode(int64_t id, P2 p, const Tags& t) {
    auto g = kAnchor.toGeodetic(p.x, 0, p.y);
    return {id, g.x, g.y, t};
}
// The Theix canopy's measured outline: 22 m by 6.8 m, a cadastre wall=no.
const Ring kCanopy = {{0, 0}, {4.1, 21.6}, {10.8, 20.3}, {6.7, -1.3}};
}  // namespace

TEST(Fuel, a_station_and_its_canopy_are_never_a_store_and_a_shed_is_no_canopy) {
    const Tags canopy{{"building", "roof"}, {"amenity", "fuel"}, {"shop", "gas"}, {"name", "Carrefour"}};
    CHECK(openRoof(canopy)); CHECK(!retailUse(canopy));
    CHECK(!retailUse({{"amenity", "fuel"}, {"shop", "gas"}, {"name", "Carrefour Market"}}));
    // A station building with a convenience store is its kiosk: walls.
    CHECK(!openRoof({{"building", "yes"}, {"amenity", "fuel"}, {"shop", "convenience"}}));
    CHECK(openRoof({{"building", "yes"}, {"amenity", "fuel"}, {"shop", "gas"}, {"layer", "1"}}));
    // The cadastre's light constructions keep their walls unless a station owns one.
    CHECK(!openRoof({{"building", "yes"}, {"wall", "no"}}));
    CHECK(retailUse({{"building", "yes"}, {"wall", "no"}, {"shop", "bakery"}}));
}

TEST(Fuel, the_fascia_reads_station_in_the_country_language_or_the_brand) {
    CHECK(fuelTitle("FR") == "STATION-SERVICE"); CHECK(fuelTitle("DE") == "TANKSTELLE");
    CHECK(fuelTitle("ES") == "ESTACIÓN DE SERVICIO"); CHECK(fuelTitle("GB") == "PETROL STATION");
    CHECK(fuelTitle("US") == "GAS STATION"); CHECK(fuelTitle("RU").empty());
    // Accents the font lacks fall back to the plain letter, as on town signs.
    for (auto c : {"FR", "DE", "ES", "IT", "PT", "CZ", "PL", "TR", "HU", "RO", "IS"})
        if (!facadeLettering(fuelTitle(c), 14, .4)) { std::printf("cannot spell %s\n", c); CHECK(false); }
    CHECK(countryAt(2.35, 48.85) == "FR"); CHECK(countryAt(13.4, 52.5) == "DE");

    OsmData osm;
    osm.buildings.push_back(geoWay(1, kCanopy, {{"building", "yes"}, {"wall", "no"}}));
    osm.features.push_back(geoNode(2, {5, 10}, {{"amenity", "fuel"}, {"brand", "Carrefour"}, {"shop", "gas"}}));
    const auto tile = tileAt(osm.features[0].lon, osm.features[0].lat);
    for (auto [country, title] : {std::pair<const char*, const char*>{"FR", "STATION-SERVICE"}, {"DE", "TANKSTELLE"},
                                  {"RU", "Carrefour"}, {"GR", "Carrefour"}}) {
        nlohmann::json manifest = nlohmann::json::array();
        const auto marked = fuelCanopies({&osm.buildings[0]}, osm, tile, country, manifest);
        CHECK(marked.size() == 1); CHECK(openRoof(marked[0].tags));
        CHECK(tagOr(marked[0].tags, "r1:fuelTitle") == title);
        CHECK(tagOr(marked[0].tags, "r1:fuelBrand") == "Carrefour");
        CHECK(manifest.size() == 1); CHECK(manifest[0]["canopySource"] == "measured");
        CHECK(!has(osm.buildings[0].tags, "r1:fuel"));  // the observation is never mutated
    }
}

TEST(Fuel, a_canopy_stands_on_islands_people_and_cars_pass_under) {
    OsmData osm;
    osm.buildings.push_back(geoWay(1, kCanopy, {{"building", "yes"}, {"wall", "no"}}));
    osm.features.push_back(geoNode(2, {5, 10}, {{"amenity", "fuel"}, {"brand", "Carrefour"}}));
    nlohmann::json manifest = nlohmann::json::array();
    const auto tile = tileAt(osm.features[0].lon, osm.features[0].lat);
    auto marked = fuelCanopies({&osm.buildings[0]}, osm, tile, "FR", manifest);
    auto ground = [&](double x, double y) { auto p = kAnchor.toEngine(x, y, 0); p.y = 10; return p; };
    auto built = buildBuildings({&marked[0]}, ground, profileFor(2.35, 48.85), {}, -1, 0, {}, {});
    CHECK(built.openRoofs == 1); CHECK(built.interiors.empty());
    // The canopy itself is no footprint; its two islands are.
    CHECK(built.footprints.size() == 2); CHECK(built.fuelStations.size() == 1);
    CHECK(built.fuelStations[0]["islands"] == 2); CHECK(built.fuelStations[0]["pumps"] == 4);
    for (const auto& island : built.footprints) {
        for (auto p : island) CHECK(pointInPolygon(p, kCanopy));
        CHECK(!pointInPolygon(centroid(kCanopy), island));
    }
    // A lorry's clearance under the fascia, a 0.9 m fascia.
    double soffit = 1e9, roof = -1e9;
    for (const auto& part : built.parts) {
        if (part.name == "Canopy soffit") for (auto p : part.mesh.positions) soffit = std::min(soffit, p.y);
        if (part.name == "Canopy roof") for (auto p : part.mesh.positions) roof = std::max(roof, p.y);
    }
    NEAR(soffit, 10 + 4.7, 1e-6); NEAR(roof, 10 + 5.6, 1e-6);
    CHECK(built.lettering.size() == 2);
    for (const auto& l : built.lettering) {
        CHECK(l["text"] == "STATION-SERVICE");
        // The whole glyph cell stays inside the fascia.
        const double cap = l["height"], y = l["at"][1];
        CHECK(y - 0.36 * cap >= soffit); CHECK(y + 1.36 * cap <= roof);
    }
    // A plain roof on posts is drawn the same way, with nothing under it.
    auto shelter = geoWay(3, {{30, 0}, {36, 0}, {36, 4}, {30, 4}}, {{"building", "roof"}});
    auto plain = buildBuildings({&shelter}, ground, profileFor(2.35, 48.85), {}, -1, 0, {}, {});
    CHECK(plain.openRoofs == 1); CHECK(plain.footprints.empty()); CHECK(plain.fuelStations.empty());
}

TEST(Fuel, a_station_without_a_mapped_canopy_gets_one_clear_of_roads_or_says_why) {
    OsmData osm;
    OsmWay road; road.id = 9; road.tags = {{"highway", "residential"}};
    for (P2 p : {P2{-40, -12}, P2{40, -12}}) { auto g = kAnchor.toGeodetic(p.x, 0, p.y); road.points.push_back({g.x, g.y}); }
    osm.roads.push_back(road);
    osm.features.push_back(geoNode(5, {0, 0}, {{"amenity", "fuel"}, {"brand", "TotalEnergies"}}));
    const auto tile = tileAt(osm.features[0].lon, osm.features[0].lat);
    nlohmann::json manifest = nlohmann::json::array();
    auto out = fuelCanopies({}, osm, tile, "FR", manifest);
    CHECK(out.size() == 1); CHECK(manifest[0]["canopySource"] == "inferred");
    CHECK(tagOr(out[0].tags, "r1:fuelSource") == "inferred"); CHECK(out[0].id < 0);
    // Along the road, and not on it.
    Ring engine;
    for (size_t i = 0; i + 1 < out[0].points.size(); ++i) {
        auto e = kAnchor.toEngine(out[0].points[i].x, out[0].points[i].y, 0); engine.push_back({e.x, e.z});
    }
    for (auto p : engine) CHECK(std::abs(p.y + 12) > roadWidth(road.tags) / 2);
    const auto box = orientedBox(engine);
    NEAR(std::abs(box.ux), 1, 1e-3);
    // Boxed in by buildings on every side: no canopy, and the reason.
    for (int k = 0; k < 4; ++k) {
        const double a = k * kPi / 2;
        const P2 c{std::cos(a) * 6, std::sin(a) * 6};
        osm.buildings.push_back(geoWay(100 + k, {{c.x - 5, c.y - 5}, {c.x + 5, c.y - 5}, {c.x + 5, c.y + 5}, {c.x - 5, c.y + 5}},
                                       {{"building", "yes"}}));
    }
    manifest = nlohmann::json::array();
    out = fuelCanopies({}, osm, tile, "FR", manifest);
    CHECK(out.empty()); CHECK(manifest[0]["canopySource"] == "none"); CHECK(manifest[0].contains("reason"));
}

TEST(Fuel, liveries_match_whole_words_and_the_totem_stands_beside_the_road) {
    const auto red = fuelLivery("TotalEnergies"), blue = fuelLivery("E.Leclerc"), plain = fuelLivery("Genin");
    CHECK(red[0] > red[2]); CHECK(blue[2] > blue[0]);
    CHECK(plain == fuelLivery("")); CHECK(fuelLivery("U") == fuelLivery("Super U"));
    CanopyBook book;
    const Tags tags{{"building", "roof"}, {"r1:fuel", "1"}, {"r1:fuelBrand", "Carrefour"}, {"r1:fuelTitle", "STATION-SERVICE"}};
    buildOpenRoof(kCanopy, orientedBox(kCanopy), 10, 5, tags, 1, book);
    OsmWay road; road.tags = {{"highway", "tertiary"}};
    for (P2 p : {P2{30, -40}, P2{30, 60}}) { auto g = kAnchor.toGeodetic(p.x, 0, p.y); road.points.push_back({g.x, g.y}); }
    auto ground = [&](double x, double y) { auto p = kAnchor.toEngine(x, y, 0); p.y = 10; return p; };
    const size_t before = book.obstacles.size();
    placeFuelTotems({road}, kAnchor, ground, {}, book);
    CHECK(book.stations[0]["totem"] == "placed"); CHECK(book.obstacles.size() == before + 1);
    const P2 totem = centroid(book.obstacles.back());
    CHECK(!pointInPolygon(totem, kCanopy));
    NEAR(totem.x, 30 - roadWidth(road.tags) / 2 - 2, 0.2);
    // Its lettering is the brand, on both faces, in the livery.
    CHECK(book.lettering.back()["text"] == "Carrefour");
    placeFuelTotems({}, kAnchor, ground, {}, book);
    CHECK(book.stations[0]["totem"] == "no public road within 45 m");
}
