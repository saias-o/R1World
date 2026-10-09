#include "check.hpp"
#include "gen/ground.hpp"
#include "gen/buildings.hpp"
#include "gen/cook.hpp"
#include "gen/grass.hpp"

using namespace r1;

namespace {
struct District {
    Anchor anchor=Anchor::at(-2.76,47.655);
    Bounds bounds=tileAt(anchor.lon,anchor.lat).bounds();
    OsmData osm;
    std::map<int64_t,double> heights;
    void house(int64_t id,double x,double z,double side,double height) {
        OsmWay w;w.id=id;w.tags={{"building","house"},{"roof:shape","flat"}};
        for(auto p:Ring{{x,z},{x+side,z},{x+side,z+side},{x,z+side},{x,z}}) {
            const auto geo=anchor.toGeodetic(p.x,0,p.y);w.points.push_back({geo.x,geo.y});
        }
        osm.buildings.push_back(std::move(w));heights[id]=height;
    }
    void grid(double side,double height,int count=4) {
        for(int r=0;r<count;++r)for(int c=0;c<count;++c)
            house(1+r*count+c,-75+c*40.,-75+r*40.,side,height);
    }
    GroundInference inference(const char* profile="FRANCE",const char* country="FR") const {
        return GroundInference(osm,bounds,profileByKey(profile),country,heights);
    }
};
}

TEST(GroundInference, dense_vannes_blocks_use_made_ground_but_empty_land_keeps_its_region) {
    District d;d.grid(28,12);
    const auto policy=d.inference();
    CHECK(policy.at(d.anchor.lon,d.anchor.lat)=="inferred:urban");
    CHECK(policy.at(d.anchor.lon+.005,d.anchor.lat)=="inferred");
    CHECK(groundFamily(policy.at(d.anchor.lon,d.anchor.lat),"Ground, temperate","temperate")=="made_ground");
    CHECK(grassDensity("made_ground")==0);
}
TEST(GroundInference, completed_predicted_height_changes_the_choice_at_the_same_footprint_density) {
    District d;d.grid(18,6);
    const auto low=d.inference();
    for(auto& [id,h]:d.heights)h=80;
    const auto tall=d.inference();
    const auto a=low.density(d.anchor.lon,d.anchor.lat),b=tall.density(d.anchor.lon,d.anchor.lat);
    NEAR(a.coverage,b.coverage,1e-12);CHECK(b.heightWeightedCoverage>a.heightWeightedCoverage*3);
    CHECK(low.at(d.anchor.lon,d.anchor.lat)=="inferred");
    CHECK(tall.at(d.anchor.lon,d.anchor.lat)=="inferred:urban");
}
TEST(GroundInference, predicted_neighbour_heights_are_the_same_dimensions_as_rendering) {
    District d;d.grid(18,6);
    d.osm.buildings[0].tags["height"]="80";
    std::vector<const OsmWay*> ways;for(const auto& w:d.osm.buildings)ways.push_back(&w);
    const auto ground=[&](double lon,double lat){auto p=d.anchor.toEngine(lon,lat,0);p.y=0;return p;};
    const auto p=profileByKey("FRANCE");
    const auto finalHeights=predictBuildingHeights(ways,ground,p,ways);
    const auto rendered=buildBuildings(ways,ground,p,{},-1,0,{},{},BuildingLod::SimpleRoofline,ways);
    CHECK(rendered.visuals.size()==ways.size());
    for(const auto& v:rendered.visuals)NEAR(finalHeights.at(v.id),rendered.tops[v.footprint],1e-9);
    CHECK(!has(d.osm.buildings[1].tags,"height"));NEAR(finalHeights.at(2),80,1e-9);
    const GroundInference policy(d.osm,d.bounds,p,"FR",finalHeights);
    CHECK(policy.at(d.anchor.lon,d.anchor.lat)=="inferred:urban");
}
TEST(GroundInference, country_and_regional_priors_choose_earth_sand_or_grass) {
    District d;d.grid(18,6);
    const auto france=d.inference(),burkina=d.inference("SUB_SAHARAN","BF");
    CHECK(france.at(d.anchor.lon,d.anchor.lat)=="inferred");
    CHECK(burkina.at(d.anchor.lon,d.anchor.lat)=="inferred:bare");
    CHECK(groundFamily(burkina.at(d.anchor.lon,d.anchor.lat),"Ground, dry savanna","tropical")=="bare");
    CHECK(grassDensity("bare")==0);
    const auto desert=d.inference("MAGHREB_SAHARA","DZ");
    CHECK(groundFamily(desert.at(d.anchor.lon+.005,d.anchor.lat),"Ground, desert sand","arid")=="sand_desert");
    CHECK(groundFamily(france.at(d.anchor.lon,d.anchor.lat),"Ground, temperate","temperate")=="grass");
}
TEST(GroundInference, every_mapped_surface_wins_even_among_tall_dense_buildings) {
    District d;d.grid(28,80);const auto policy=d.inference();
    for(const std::string mapped:{"grass","forest","sand","water","urban","farmland"})
        CHECK(policy.at(d.anchor.lon,d.anchor.lat,&mapped)==mapped);
    CHECK(groundSwatch("inferred:urban",profileByKey("FRANCE")).name=="Made ground");
    CHECK(groundFamily("inferred:urban@frost","Ground, temperate","temperate")=="made_ground");
}
TEST(GroundInference, overlapping_footprints_do_not_multiply_coverage_and_a_single_farm_is_not_a_city) {
    District d;d.house(1,-60,-60,120,6);
    const auto single=d.inference();
    CHECK(single.at(d.anchor.lon,d.anchor.lat)=="inferred");
    auto copy=d.osm.buildings[0];copy.id=2;d.osm.buildings.push_back(copy);d.heights[2]=6;
    const auto overlap=d.inference();
    NEAR(single.density(d.anchor.lon,d.anchor.lat).coverage,overlap.density(d.anchor.lon,d.anchor.lat).coverage,1e-12);
}
TEST(GroundInference, a_large_isolated_tower_can_outweigh_the_usual_minimum_building_count) {
    District d;d.house(1,-60,-60,120,6);
    CHECK(d.inference().at(d.anchor.lon,d.anchor.lat)=="inferred");
    d.heights[1]=80;
    CHECK(d.inference().at(d.anchor.lon,d.anchor.lat)=="inferred:urban");
}
TEST(GroundInference, global_sampling_agrees_across_tiles_and_is_independent_of_building_order) {
    District d;d.grid(28,12);
    const auto original=d.inference();
    std::reverse(d.osm.buildings.begin(),d.osm.buildings.end());
    auto other=d.bounds;other.west+=kStep;other.east+=kStep;
    const GroundInference adjacent(d.osm,other,profileByKey("FRANCE"),"FR",d.heights);
    const double seam=d.bounds.east,lat=d.anchor.lat;
    const auto a=original.density(seam,lat),b=adjacent.density(seam,lat);
    NEAR(a.coverage,b.coverage,1e-12);NEAR(a.heightWeightedCoverage,b.heightWeightedCoverage,1e-12);
    CHECK(a.buildings==b.buildings && original.at(seam,lat)==adjacent.at(seam,lat));
}
TEST(GroundInference, a_cook_records_inference_as_estimated_and_masks_grass_with_the_new_ground) {
    District d;d.grid(28,12);d.osm.country="FR";
    for(auto& w:d.osm.buildings)w.tags["height"]="12";
    Observations in;in.tile=tileAt(d.anchor.lon,d.anchor.lat);in.osm=std::make_shared<OsmData>(d.osm);
    in.elevations={d.bounds,2,{0,0,0,0}};
    const auto cooked=cookTile(in);
    CHECK(cooked.manifest["ground"]["measuredFraction"]==0);
    CHECK(cooked.manifest["ground"]["trianglesByClass"].contains("inferred:urban"));
    CHECK(cooked.manifest["ground"]["inference"]["heightSource"]=="completed building prediction");
    CHECK(cooked.grass.grassy<.95);
}
