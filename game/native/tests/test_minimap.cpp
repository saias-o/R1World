#include "check.hpp"
#include "minimap.hpp"
#include "gen/cook.hpp"

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
          "Unknown city · France");
}

TEST(Minimap, neighbourhood_polylines_survive_partial_3d_residency) {
    // Only this small tile is resident, but its OSM answer covers neighbours.
    const Bounds resident{-0.0002, -0.0002, 0.0002, 0.0002};
    const OsmWay road{1, {{-0.0025,0}, {-0.001,0}, {0.001,0}, {0.0025,0}},
                      {{"highway","residential"}}};
    const auto tile=makeMiniMapTile({road}, {}, resident);
    CHECK(tile.roads.size()==3);
    const auto single=miniMapRoadRml({&tile},0,0);
    CHECK(!single.empty());
    // Publishing a second tile must not duplicate every shared OSM segment.
    CHECK(miniMapRoadRml({&tile,&tile},0,0)==single);
    auto reversed=road;
    std::reverse(reversed.points.begin(),reversed.points.end());
    const auto neighbour=makeMiniMapTile({reversed},{},resident);
    CHECK(miniMapRoadRml({&tile,&neighbour},0,0)==single);
    // A road entirely outside the resident tile is still visible to the east.
    const OsmWay east{2, {{0.002,-0.001},{0.002,0.001}},{{"highway","primary"}}};
    const auto observed=makeMiniMapTile({east},{},resident);
    CHECK(miniMapRoadRml({&observed},0,0).find("rank-3")!=std::string::npos);
}

TEST(Minimap, dense_centre_does_not_erase_viewport_edges) {
    MiniMapTile tile;
    // More segments than the display budget, concentrated in the centre.
    for(int row=0;row<40;++row)for(int col=0;col<40;++col) {
        const double x=col*0.000005,y=row*0.000005;
        tile.roads.push_back({{x,y},{x+0.00005,y},"",3});
    }
    tile.roads.push_back({{0.002,-0.001},{0.002,0.001},"",0});
    const auto roads=miniMapRoadRml({&tile},0,0);
    CHECK(roads.find("rank-0")!=std::string::npos);
    size_t count=0,at=0;
    while((at=roads.find("class='mini-road",at))!=std::string::npos){++count;++at;}
    CHECK(count==1200);
}

TEST(Minimap, cooker_keeps_observed_roads_outside_its_3d_tile) {
    Observations in;
    in.tile=tileAt(2.3522,48.8566);
    const auto bounds=in.tile.bounds();
    OsmData osm;
    osm.queryVersion=10;
    const double east=bounds.east+0.0001,lat=in.tile.center().y;
    osm.roads.push_back({1,{{east,lat-0.001},{east,lat},{east,lat+0.001}},
                         {{"highway","residential"}}});
    in.osm=std::make_shared<const OsmData>(osm);
    in.elevations={bounds,2,{0,0,0,0}};
    in.elevationSource="test:flat";
    const auto cooked=cookTile(in);
    CHECK(cooked.minimap.roads.size()==2);
    CHECK(!miniMapRoadRml({&cooked.minimap},east,lat).empty());
}
