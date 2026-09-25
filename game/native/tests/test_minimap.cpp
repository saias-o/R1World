#include "check.hpp"
#include "minimap.hpp"

using namespace r1;

TEST(Minimap, cached_osm_roads_and_addresses_drive_the_hud) {
    const Bounds area{-0.002, -0.002, 0.002, 0.002};
    const OsmWay street{1, {{-0.001, 0.0}, {0.001, 0.0}},
                        {{"highway", "residential"}, {"name", "Rue A & B"}}};
    OsmData osm;
    osm.buildings.push_back({2, {{0.0, 0.0}, {0.0001, 0.0}, {0.0, 0.0001}, {0.0, 0.0}},
                             {{"addr:city", "Paris"}, {"addr:country", "FR"}}});
    const MiniMapTile tile = makeMiniMapTile({street}, osm, area);
    const std::vector<const MiniMapTile*> visible{&tile};
    const std::string roads = miniMapRoadRml(visible, 0.0, 0.0);
    CHECK(roads.find("rank-1") != std::string::npos);
    CHECK(roads.find("Rue A &amp; B") != std::string::npos);
    CHECK(miniMapRoadRml(visible, 1.0, 0.0).empty());
    CHECK(miniMapPlace(visible, {}, {{"FR", "France"}}, "", 0.0, 0.0) == "Paris · France");
    MiniMapTile neighbour;
    neighbour.bounds = {-0.002, 0.002, 0.002, 0.004};
    neighbour.cities["Lyon"] = 100;
    CHECK(miniMapPlace({&tile, &neighbour}, {}, {{"FR", "France"}}, "", 0.0, 0.0) == "Paris · France");
    CHECK(miniMapPlace({}, {{"Europe/Paris", "FR"}}, {{"FR", "France"}}, "Europe/Paris", 0.0, 0.0) ==
          "Ville non renseignée · France");
}
