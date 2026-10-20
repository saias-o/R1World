#include "check.hpp"
#include "gen/understory.hpp"
#include "gen/streets.hpp"
#include "gen/grass.hpp"
#include <set>
#include <filesystem>

using namespace r1;
namespace {
struct Verge {
    Tile tile = tileAt(-2.72, 47.60);
    P2 centre = tile.center();
    Anchor anchor = Anchor::at(centre.x, centre.y);
    OsmData osm;
    clip::Paths64 drives;
    P2 geo(double x, double z) const { const auto g = anchor.toGeodetic(x, 0, z); return {g.x, g.y}; }
    Verge() { osm.country = "FR"; osm.roads.push_back({77, {geo(-180, 0), geo(180, 0)}, {{"highway", "tertiary"}, {"width", "6"}}}); }
    Scatter run(int budget = 512) const {
        return planUnderstory(osm, tile, anchor,
            [&](double lo, double la) { return anchor.toEngine(lo, la, 0); }, osm.roads, drives, budget);
    }
    std::vector<P2> box(double x0, double z0, double x1, double z1) const {
        return {geo(x0,z0),geo(x1,z0),geo(x1,z1),geo(x0,z1),geo(x0,z0)};
    }
};
double meanHeight(const Scatter& plants) {
    double sum = 0;
    for (const auto& node : plants.nodes) sum += node["transform"]["scale"][1].get<double>();
    return sum / std::max(size_t(1), plants.nodes.size());
}
}

TEST(Understory, rural_verges_are_dense_diverse_deterministic_and_clear_of_the_road) {
    Verge f;
    const auto out = f.run();
    CHECK(out.nodes.size() > 200);
    CHECK(out.nodes == f.run().nodes);
    CHECK(out.stats["byModel"].size() >= 6);
    for (const auto& node : out.nodes) {
        const auto& t = node["transform"];
        CHECK(std::abs(t["position"][2].get<double>()) > 4.1);
        CHECK(t["scale"][1].get<double>() <= 1.35);
        CHECK(node["groups"][1] == "understory");
        CHECK(node["children"].size() == 2);
        for (const auto& child : node["children"])
            CHECK(std::filesystem::exists(r1test::gameRoot() + "/" + child["importedFrom"].get<std::string>()));
    }
}
TEST(Understory, motorway_verges_are_taller_and_set_back_from_the_safety_shoulder) {
    Verge f;
    const auto rural = f.run();
    f.osm.roads[0].tags = {{"highway", "motorway"}, {"width", "14"}, {"embankment", "yes"}};
    const auto motorway = f.run();
    CHECK(motorway.nodes.size() > 200);
    CHECK(meanHeight(motorway) > meanHeight(rural) * 1.35);
    for (const auto& n : motorway.nodes) CHECK(std::abs(n["transform"]["position"][2].get<double>()) >= 10.0);
}
TEST(Understory, driveways_buildings_water_and_crossing_roads_keep_their_clearance) {
    Verge f;
    f.osm.buildings.push_back({80, f.box(-100, 3, -60, 25), {{"building", "house"}}});
    f.osm.landcover.push_back({81, f.box(80, -25, 120, -3), {{"natural", "water"}}});
    f.drives.push_back(clip::kMetres.path({{-40,-25},{-30,-25},{-30,25},{-40,25},{-40,-25}}));
    f.osm.roads.push_back({82, {f.geo(0,-70),f.geo(0,70)}, {{"highway","service"},{"width","4"}}});
    const auto out = f.run();
    CHECK(out.nodes.size() > 100);
    for (const auto& node : out.nodes) {
        const auto& p = node["transform"]["position"];
        const double x = p[0], z = p[2];
        CHECK(!(x > -101 && x < -59 && z > 2 && z < 26));
        CHECK(!(x > 79 && x < 121 && z > -26 && z < -2));
        CHECK(!(x > -40.45 && x < -29.55 && z > -25 && z < 25));
        CHECK(std::abs(x) > 2.45);
    }
}
TEST(Understory, budget_keeps_a_distributed_deterministic_subset_and_no_tile_geometry) {
    Verge f;
    const auto out = f.run(19);
    CHECK(out.nodes.size() == 19);
    CHECK(out.nodes == f.run(19).nodes);
    CHECK(out.stats["droppedForBudget"].get<int>() > 100);
    CHECK(f.run(0).nodes.empty());
    bool west = false, east = false;
    for (const auto& n : out.nodes) {
        west |= n["transform"]["position"][0].get<double>() < -50;
        east |= n["transform"]["position"][0].get<double>() > 50;
        CHECK(!n.contains("mesh"));
    }
    CHECK(west && east);
}
TEST(Understory, surveyed_hedge_height_wins_and_mapped_hedges_take_budget_priority) {
    Verge f;
    f.osm.barriers.push_back({90, {f.geo(-100, 25),f.geo(100, 25)}, {{"barrier","hedge"},{"height","2.7"}}});
    const auto out = f.run(25);
    CHECK(out.nodes.size() == 25);
    CHECK(out.stats["bySource"]["osm-hedge"] == 25);
    for (const auto& n : out.nodes) NEAR(n["transform"]["scale"][1].get<double>(), 2.7, 1e-9);
}
TEST(Understory, bridge_tunnel_and_paved_urban_streets_do_not_grow_wild_verges) {
    for (const auto& tags : {Tags{{"bridge","yes"}}, Tags{{"tunnel","yes"}}, Tags{{"sidewalk","both"}}}) {
        Verge f;
        for (const auto& [k,v] : tags) f.osm.roads[0].tags[k] = v;
        CHECK(f.run().nodes.empty());
    }
}
TEST(Understory, scrub_areas_grow_cover_without_a_road_but_crop_fields_do_not) {
    Verge f;
    f.osm.roads.clear();
    f.osm.vegetation.push_back({91, f.box(-75,-75,75,75), {{"natural","scrub"}}});
    const auto out = f.run();
    CHECK(out.nodes.size() > 100);
    CHECK(out.stats["bySource"].contains("osm-scrub"));
    f.osm.vegetation[0].tags = {{"landuse","farmland"}};
    CHECK(f.run().nodes.empty());
}
TEST(Understory, large_scrub_areas_leave_room_for_dense_verges_and_water_does_not_spend_the_budget) {
    Verge f;
    f.osm.vegetation.push_back({92,f.box(-180,-180,180,180),{{"natural","scrub"}}});
    const auto out=f.run(200);
    CHECK(out.nodes.size()==200);
    CHECK(out.stats["bySource"]["inferred-rural-verge"].get<int>()>=120);
    CHECK(out.stats["bySource"]["osm-scrub"].get<int>()>10);
    const auto dry=planUnderstory(f.osm,f.tile,f.anchor,
        [&](double lo,double la){return f.anchor.toEngine(lo,la,0);},f.osm.roads,f.drives,200,
        [](P2 p){return p.x>0;});
    CHECK(dry.nodes.size()==200);
    for(const auto& n:dry.nodes)CHECK(n["transform"]["position"][0].get<double>()>0);
}
TEST(Understory, plants_thin_procedural_grass_under_their_footprint_without_repainting_it) {
    GrassCover cover;cover.coverSize=16;cover.cover.assign(256,0xff806040u);
    cover.uvFromEngine={1./16,0,0,0,1./16,0};
    nlohmann::json plants=nlohmann::json::array({{{"name","Understory test"},
        {"transform",{{"position",{8.5,0,8.5}},{"scale",{2.,1.,2.}}}}}});
    maskUnderstoryGrass(cover,plants);
    CHECK((cover.cover[8*16+8]>>24)<64);
    CHECK(cover.cover.front()==0xff806040u);
    for(const auto pixel:cover.cover)CHECK((pixel&0xffffffu)==0x806040u);
}
TEST(Understory, detached_front_gardens_gain_massifs_but_surveyed_boundaries_silence_inference) {
    Verge f;
    f.osm.buildings.push_back({98,f.box(75,20,85,30),{{"building","house"}}});
    const auto open=f.run();
    CHECK(open.stats["bySource"].value("inferred-front-garden",0)>0);
    f.osm.barriers.push_back({99,f.box(71,12,89,34),{{"barrier","fence"}}});
    const auto surveyed=f.run();
    CHECK(surveyed.stats["bySource"].value("inferred-front-garden",0)==0);
}
