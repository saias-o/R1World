// The land animals: who may live where, how many, and how rarely indoors.
#include "check.hpp"

#include "gen/fauna.hpp"
#include "gen/palette.hpp"

#include <fstream>
#include <map>
#include <set>

using namespace r1;

namespace {

// A tile whose every patch of ground is drawn as `drawn` (a ground class),
// optionally with square buildings on a grid, `spacing` metres apart.
nlohmann::json cookAt(double lon, double lat, const std::string& country, const std::string& drawn,
                      double spacing = 0, int dRow = 0, int dCol = 0) {
    Tile t = tileAt(lon, lat);
    t.row += dRow;
    t.col += dCol;
    const P2 c = t.center();
    const Anchor anchor = Anchor::at(c.x, c.y);
    const std::string climate = climateAt(profileFor(c.x, c.y).climate, c.y);
    std::vector<Ring> footprints;
    if (spacing > 0)
        for (double x = -280; x < 280; x += spacing)
            for (double z = -280; z < 280; z += spacing)
                footprints.push_back({{x, z}, {x + spacing * 0.6, z}, {x + spacing * 0.6, z + spacing * 0.6}, {x, z + spacing * 0.6}});
    std::vector<OsmWay> landcover;
    FaunaInput in;
    in.tile = t;
    in.bounds = t.bounds();
    in.anchor = &anchor;
    in.classify = [drawn](double, double) { return drawn; };
    const auto profile = profileFor(c.x, c.y).ground.name;
    in.family = [profile, climate](const std::string& cls) { return groundFamily(cls, profile, climate); };
    in.footprints = &footprints;
    in.landcover = &landcover;
    in.country = country;
    in.climate = climate;
    return planFauna(in);
}

std::map<std::string, int> speciesOf(const nlohmann::json& fauna) {
    std::map<std::string, int> out;
    for (const auto& r : fauna.at("residents")) ++out[r.at("species").get<std::string>()];
    return out;
}

bool lives(const std::string& id, const std::string& country, double lon, double lat) {
    const Species* s = speciesById(id);
    CHECK_MSG(s, id);
    const std::string climate = climateAt(profileFor(lon, lat).climate, lat);
    return rangeAt(*s, country, lon, lat, climate).inside;
}

}  // namespace

TEST(Fauna, every_country_named_exists_and_every_model_has_both_levels) {
    std::ifstream in(r1test::gameRoot() + "/assets/world/countries.geojson");
    CHECK(in);
    const auto geo = nlohmann::json::parse(in);
    std::set<std::string> known;
    for (const auto& f : geo.at("features")) known.insert(f.at("properties").value("ISO_A2_EH", std::string()));
    std::ifstream measured(r1test::gameRoot() + "/assets/animals/animals.json");
    CHECK(measured);
    const auto animals = nlohmann::json::parse(measured).at("models");
    for (const Species& s : fauna().species) {
        for (const FaunaRange& r : s.range)
            for (const std::string& c : r.countries) CHECK_MSG(known.count(c), s.id << " names " << c);
        for (const auto* list : {&s.models, &s.indoorModels})
            for (const FaunaModel& m : *list) {
                CHECK_MSG(animals.contains(m.model), m.model << " is not measured (python -m r1.animals)");
                CHECK_MSG(animals.at(m.model).value("sameRig", false), m.model << ": LOD1 rig differs");
            }
    }
}

TEST(Fauna, no_elephant_in_paris_and_no_wolf_in_broceliande) {
    const double parisLon = 2.3522, parisLat = 48.8566, brocLon = -2.18, brocLat = 48.02;
    for (const char* id : {"african_bush_elephant", "asian_elephant", "lion", "giraffe", "tiger", "polar_bear",
                           "hippopotamus", "nile_crocodile", "plains_zebra", "dromedary", "western_gorilla"})
        CHECK_MSG(!lives(id, "FR", parisLon, parisLat), id << " in Paris");
    for (const char* id : {"gray_wolf", "brown_bear", "polar_bear", "arctic_fox"})
        CHECK_MSG(!lives(id, "FR", brocLon, brocLat), id << " in Brocéliande");
    for (const char* id : {"roe_deer", "wild_boar", "red_deer", "red_fox", "european_rabbit"})
        CHECK_MSG(lives(id, "FR", brocLon, brocLat), id << " missing from Brittany");
    CHECK(lives("gray_wolf", "FR", 6.6, 44.9));        // the southern Alps
    CHECK(lives("brown_bear", "FR", 0.8, 42.85));      // the Pyrenees
    CHECK(lives("african_bush_elephant", "KE", 37.0, -2.5));
    CHECK(lives("asian_elephant", "LK", 81.0, 7.8));
    CHECK(!lives("asian_elephant", "KE", 37.0, -2.5));
    CHECK(lives("polar_bear", "GL", -40, 72));
    CHECK(lives("arctic_fox", "IS", -19, 64.9));
    CHECK(!lives("red_fox", "IS", -19, 64.9));
    CHECK(lives("lion", "IN", 70.8, 21.1));            // Gir
    CHECK(!lives("lion", "IN", 77.2, 28.6));           // Delhi
}

TEST(Fauna, a_breton_forest_holds_deer_and_boar_and_never_savanna_game) {
    std::map<std::string, int> total;
    for (int d = 0; d < 60; ++d)
        for (const auto& [id, n] : speciesOf(cookAt(-2.18, 48.02, "FR", "forest", 0, d / 8, d % 8))) total[id] += n;
    const std::set<std::string> allowed{"roe_deer", "wild_boar", "red_deer", "red_fox", "european_rabbit", "fallow_deer", "common_wasp", "european_robin", "crow", "hawk", "eurasian_eagle_owl"};
    for (const auto& [id, n] : total) CHECK_MSG(allowed.count(id), id << " in a Breton forest");
    // About one roe deer group a forest tile: tens over sixty tiles, never hundreds.
    CHECK_MSG(total["roe_deer"] >= 30 && total["roe_deer"] <= 120, total["roe_deer"]);
    CHECK(total["wild_boar"] >= 1);
}

TEST(Fauna, paris_streets_hold_rats_and_cats_not_deer) {
    std::map<std::string, int> total;
    for (int d = 0; d < 30; ++d)
        for (const auto& [id, n] : speciesOf(cookAt(2.3522, 48.8566, "FR", "urban", 14, d / 6, d % 6))) total[id] += n;
    for (const auto& [id, n] : total) CHECK_MSG(id == "brown_rat" || id == "domestic_cat" || id=="rock_pigeon" || id=="crow", id << " in central Paris");
    CHECK(total["brown_rat"] + total["domestic_cat"] > 0);
}

TEST(Fauna, a_kenyan_savanna_holds_its_own_animals) {
    std::map<std::string, int> total;
    for (int d = 0; d < 40; ++d)
        for (const auto& [id, n] : speciesOf(cookAt(35.1, -1.5, "KE", "scrub", 0, d / 8, d % 8))) total[id] += n;
    for (const char* never : {"roe_deer", "red_deer", "wild_boar", "brown_bear", "asian_elephant", "polar_bear"})
        CHECK_MSG(!total.count(never), never << " in the Masai Mara");
    CHECK(total["plains_zebra"] + total["blue_wildebeest"] + total["thomsons_gazelle"] > 0);
}

TEST(Fauna, cattle_graze_pasture_never_the_city) {
    int meadow = 0, city = 0;
    for (int d = 0; d < 20; ++d) {
        meadow += speciesOf(cookAt(-1.0, 47.5, "FR", "grass", 0, d / 5, d % 5))["cattle"];
        city += speciesOf(cookAt(2.3522, 48.8566, "FR", "urban", 14, d / 5, d % 5))["cattle"];
    }
    CHECK(meadow > 0);
    CHECK(city == 0);
}

TEST(Fauna, residents_are_drawn_from_the_tile_key) {
    const auto a = cookAt(-2.18, 48.02, "FR", "forest");
    const auto b = cookAt(-2.18, 48.02, "FR", "forest");
    CHECK(a.dump() == b.dump());
    for (const auto& r : readResidents(a)) CHECK(!r.models.empty() && r.identity != 0);
    CHECK(a.at("source").get<std::string>().rfind("inferred", 0) == 0);
}

TEST(Fauna, young_never_lead_and_never_walk_alone) {
    for (int d = 0; d < 40; ++d)
        for (const auto& r : readResidents(cookAt(35.1, -1.5, "KE", "scrub", 0, d / 8, d % 8))) {
            const Species* s = speciesById(r.species);
            for (const auto& m : s->models)
                if (m.young) CHECK_MSG(r.models.front() != m.model, r.species << " led by " << m.model);
        }
}

TEST(Fauna, grid_measures_water_and_buildings) {
    HabitatGrid g;
    g.cols = 20; g.rows = 20; g.cell = 8;
    g.cells.assign(400, uint8_t(Habitat::Meadow));
    g.cells[0] = uint8_t(Habitat::Water);
    for (int i = 200; i < 220; ++i) g.cells[size_t(i)] = uint8_t(Habitat::Building);
    g.finish();
    CHECK(g.water[0] == 0 && g.water[1] == 1 && g.water[21] == 1 && g.water[399] == 19);
    CHECK(g.built[210] > g.built[0]);
    const auto round = HabitatGrid::from(g.json());
    CHECK(round.cells == g.cells && round.water == g.water && round.built == g.built);
}

TEST(Fauna, hours_and_seasons) {
    CHECK(activityAt("nocturnal", 12) < 0.1 && activityAt("nocturnal", 1) > 0.9);
    CHECK(activityAt("crepuscular", 19) > 0.9 && activityAt("crepuscular", 12) < 0.2);
    const Species& lizard = *speciesById("wall_lizard");
    const RangeAnswer france = rangeAt(lizard, "FR", 4.8, 43.9, "mediterranean");
    CHECK(seasonAt(lizard, france, 1, 43.9) == 0 && seasonAt(lizard, france, 7, 43.9) == 1);
    const Species& monarch = *speciesById("monarch");
    const RangeAnswer michoacan = rangeAt(monarch, "MX", -100.3, 19.6, "tropical");
    CHECK(michoacan.inside && michoacan.factor > 100);
    CHECK(seasonAt(monarch, michoacan, 1, 19.6) == 1 && seasonAt(monarch, michoacan, 7, 19.6) == 0);
    // The southern summer: Australian monarchs fly in January.
    const RangeAnswer sydney = rangeAt(monarch, "AU", 151.2, -33.9, "temperate");
    CHECK(seasonAt(monarch, sydney, 1, -33.9) == 1 && seasonAt(monarch, sydney, 7, -33.9) == 0);
}

TEST(Fauna, a_spider_in_a_few_houses_in_a_hundred) {
    InteriorPlan plan;
    plan.recipe = "home";
    plan.ring = {{0, 0}, {12, 0}, {12, 9}, {0, 9}};
    plan.door = {6, 0};
    plan.along = {1, 0};
    plan.inward = {0, 1};
    plan.ceiling = 2.6;
    InteriorLayout layout;
    layout.rooms = {{"living", {{0, 0}, {6, 0}, {6, 4.5}, {0, 4.5}}}, {"kitchen", {{6, 0}, {12, 0}, {12, 4.5}, {6, 4.5}}},
                    {"bedroom", {{0, 4.5}, {6, 4.5}, {6, 9}, {0, 9}}}, {"bathroom", {{6, 4.5}, {12, 4.5}, {12, 9}, {6, 9}}}};
    IndoorContext summer;
    summer.country = "FR";
    summer.climate = "temperate";
    summer.lon = 2.35;
    summer.lat = 48.86;
    summer.month = 7;
    summer.hour = 15;
    std::map<std::string, int> seen;
    int crowded = 0;
    const int homes = 3000;
    for (int i = 0; i < homes; ++i) {
        plan.id = 1000 + i;
        summer.day = 20300 + i % 7;
        const auto animals = planIndoor(plan, layout, summer);
        if (animals.size() > 2) ++crowded;
        for (const auto& a : animals) {
            ++seen[a.species];
            if (a.place == "trail") CHECK(a.count >= 6 && a.count <= 12 && a.length > 0.5);
            if (a.place == "wall") CHECK(a.height >= 1.2 && a.height <= 2.6);
        }
    }
    const double spiders = double(seen["house_spider"]) / homes;
    CHECK_MSG(spiders > 0.04 && spiders < 0.13, spiders);
    CHECK(crowded < homes / 100);
    IndoorContext winter = summer;
    winter.month = 1;
    int wasps = 0;
    for (int i = 0; i < homes; ++i) {
        plan.id = 1000 + i;
        for (const auto& a : planIndoor(plan, layout, winter)) wasps += a.species == "common_wasp" || a.species == "black_garden_ant";
    }
    CHECK(wasps == 0);
    // The same house on the same day keeps its spider.
    plan.id = 4242;
    CHECK(planIndoor(plan, layout, summer).size() == planIndoor(plan, layout, summer).size());
}

TEST(Birds, all_profiles_have_valid_flight_and_measured_models) {
    size_t species=0,models=0;
    for(const auto& s:fauna().species)if(s.klass=="bird") {
        ++species;CHECK(s.bird.enabled);CHECK(s.density>0);CHECK(!s.range.empty());
        CHECK(s.bird.speed>0&&s.bird.climb>0&&s.bird.turn>0);
        CHECK(s.bird.flyMin<=s.bird.flyMax&&s.bird.low<=s.bird.high);
        for(const auto& m:s.models){++models;CHECK(!m.model.empty());}
    }
    CHECK(species==12&&models==14);
}

TEST(Birds, regional_birds_do_not_fill_the_wrong_continent) {
    CHECK(lives("rock_pigeon","FR",2.35,48.86));
    CHECK(lives("european_robin","GB",-.1,51.5));
    CHECK(!lives("european_robin","US",-74,40.7));
    CHECK(lives("bald_eagle","US",-122,47.6));
    CHECK(!lives("bald_eagle","FR",2.35,48.86));
    CHECK(lives("scarlet_macaw","BR",-60,-3));
    CHECK(!lives("scarlet_macaw","FR",2.35,48.86));
    CHECK(lives("yellow_billed_shrike","SN",-16,14));
    CHECK(!lives("yellow_billed_shrike","FR",2.35,48.86));
    CHECK(lives("wild_turkey","US",-85,35));
    CHECK(!lives("wild_turkey","FR",2.35,48.86));
    CHECK(lives("indian_peafowl","IN",77,20));
    CHECK(!lives("indian_peafowl","CA",-100,50));
}

TEST(Birds, night_seasons_day_length_and_weather_change_flight) {
    const auto& robin=*speciesById("european_robin");
    const auto& owl=*speciesById("eurasian_eagle_owl");
    CHECK(birdActivityAt(owl,1,6,48,0,0)>birdActivityAt(owl,12,6,48,0,0)*5);
    CHECK(birdActivityAt(robin,12,6,48,0,0)>birdActivityAt(robin,1,6,48,0,0)*5);
    CHECK(birdActivityAt(robin,5,6,48,0,0)>birdActivityAt(robin,5,12,48,0,0)*2);
    CHECK(birdActivityAt(robin,12,6,48,8,20)==0);
    const auto& eagle=*speciesById("bald_eagle");
    auto north=rangeAt(eagle,"CA",-110,65,"boreal");CHECK(north.inside);
    CHECK(seasonAt(eagle,north,1,65)==0&&seasonAt(eagle,north,7,65)==1);
    CHECK(birdActivityAt(robin,12,6,-35,0,0)<birdActivityAt(robin,12,12,-35,0,0));
    CHECK(birdActivityAt(owl,12,6,80,0,0)<.1);
    CHECK(birdActivityAt(owl,12,12,80,0,0)>.5);
}

TEST(Birds, coastal_species_need_water_and_farm_birds_need_farm_habitat) {
    HabitatGrid g;g.cols=20;g.rows=20;g.cell=8;g.cells.assign(400,uint8_t(Habitat::Beach));g.finish();
    const auto& gull=*speciesById("gull");RangeAnswer range;range.inside=true;
    CHECK(suitability(gull,range,g,30)==0);
    g.cells[0]=uint8_t(Habitat::Water);g.finish();CHECK(suitability(gull,range,g,30)>0);
    const auto& hen=*speciesById("chicken");CHECK(suitability(hen,range,g,30)==0);
    g.cells[30]=uint8_t(Habitat::Farmyard);g.finish();CHECK(suitability(hen,range,g,30)>0);
}
