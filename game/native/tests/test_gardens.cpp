#include "check.hpp"
#include "gen/gardens.hpp"
#include "gen/buildings.hpp"

using namespace r1;
namespace {
struct Scene {
    Tile tile=tileAt(-2.71559,47.56272);
    Anchor anchor=Anchor::at(tile.center().x,tile.center().y);
    OsmData osm;
    ElevationGrid grid{tile.bounds(),2,{0,0,0,0}};
    Scene(double lon=-2.71559,double lat=47.56272):tile(tileAt(lon,lat)),
        anchor(Anchor::at(tile.center().x,tile.center().y)),grid{tile.bounds(),2,{0,0,0,0}} {
        osm.country="FR";
        osm.roads.push_back({1,{geo(-180,0),geo(180,0)},{{"highway","unclassified"},{"width","6"},{"sidewalk","no"}}});
        house(10,0,23);
    }
    P2 geo(double x,double z) const {const auto g=anchor.toGeodetic(x,0,z);return {g.x,g.y};}
    void house(int64_t id,double x,double z,Tags tags={{"building","yes"}}) {
        osm.buildings.push_back({id,{geo(x-6,z-4),geo(x+6,z-4),geo(x+6,z+4),geo(x-6,z+4),geo(x-6,z-4)},std::move(tags)});
    }
    GardenOutput gardens(size_t budget=12000) const {
        return buildGardens(osm,planResidential(osm,anchor),tile,anchor,grid,{}, {},budget);
    }
};
size_t vertices(const GardenOutput& out) {size_t n=0;for(const auto& p:out.parts)n+=p.mesh.vertexCount();return n;}
}
TEST(Gardens, a_breton_house_gets_a_front_plot_and_an_open_access) {
    Scene s;auto plan=planResidential(s.osm,s.anchor);
    CHECK(plan.homes.size()==1);CHECK(plan.homes[0].style->key=="fr.brittany.detached");
    auto out=s.gardens();CHECK(out.stats["inferredGardens"]==1);CHECK(out.stats["lots"][0]["building"]==10);
    CHECK(clip::contains(out.drives,P2{0,15}));CHECK(vertices(out)>100);
    bool lawn=false,gate=false;
    for(const auto& p:out.parts) {
        lawn |= p.name=="Residential lawns";gate |= p.name=="Residential gates";
        if(p.name=="Garden wire fences")for(auto q:p.mesh.positions)
            CHECK_MSG(std::abs(q.x)>1.6 || q.z>17,"a fence closes the drive");
    }
    CHECK(lawn && gate);
    CHECK(vertices(out)<=12000);
}
TEST(Gardens, dense_urban_blocks_never_get_the_detached_rule_even_when_tagged_yes) {
    Scene s;
    for(int z=-3;z<=3;++z)for(int x=-3;x<=3;++x)
        if(x || z)s.house(100+(z+3)*7+x+3,x*14,23+z*14);
    auto p=planResidential(s.osm,s.anchor);CHECK(p.homes.empty());
    CHECK(p.stats["rejectedDense"].get<int>()>0);
    s.osm.barriers.push_back({80,{s.geo(-60,6),s.geo(60,6)},{{"barrier","fence"}}});
    auto out=s.gardens();CHECK(out.parts.empty());CHECK(out.stats["inferredGardens"]==0);
    CHECK(out.stats["excludedDenseSettlement"]==true);
}
TEST(Gardens, a_house_close_to_the_road_stays_low_without_inventing_a_front_garden) {
    Scene s;s.osm.buildings.clear();s.house(10,0,9);
    auto p=planResidential(s.osm,s.anchor);CHECK(p.homes.size()==1);
    CHECK(!p.homes[0].gardenEligible);CHECK(s.gardens().parts.empty());
}
TEST(Gardens, small_cadastral_annexes_are_not_a_dense_apartment_neighbourhood) {
    Scene s;
    for(int i=0;i<20;++i) {
        double x=35+(i%5)*3,z=10+(i/5)*3;
        s.osm.buildings.push_back({100+i,{s.geo(x,z),s.geo(x+1.5,z),s.geo(x+1.5,z+1.5),s.geo(x,z+1.5),s.geo(x,z)},{{"building","yes"}}});
    }
    auto p=planResidential(s.osm,s.anchor);CHECK(p.homes.size()==1);CHECK(p.stats["rejectedDense"]==0);
}
TEST(Gardens, attached_houses_and_public_or_large_buildings_do_not_get_plots) {
    Scene s;s.house(11,12,23);CHECK(planResidential(s.osm,s.anchor).homes.empty());
    s.osm.buildings.resize(1);
    for(Tags tags : {Tags{{"building","apartments"}},Tags{{"building","yes"},{"shop","bakery"}},
                    Tags{{"building","yes"},{"building:levels","5"}},Tags{{"building","yes"},{"amenity","school"}}}) {
        s.osm.buildings[0].tags=tags;CHECK(planResidential(s.osm,s.anchor).homes.empty());
    }
}
TEST(Gardens, countries_without_a_residential_rule_keep_their_existing_atlas) {
    Scene s;s.osm.country="DE";CHECK(planResidential(s.osm,s.anchor).homes.empty());
    const auto plan=planResidential(s.osm,s.anchor);
    auto ground=[&](double lon,double lat){return s.anchor.toEngine(lon,lat,0);};
    const auto existing=planNature(s.osm,s.tile,s.anchor,ground);
    const auto retained=planNature(s.osm,s.tile,s.anchor,ground,320,nullptr,nullptr,&plan);
    CHECK(existing.nodes==retained.nodes);
    CHECK(s.gardens().parts.empty());
}
TEST(Gardens, mapped_boundaries_silence_inferred_enclosures_and_keep_gate_openings) {
    Scene s;s.osm.barriers.push_back({50,{s.geo(-12,7),s.geo(12,7)},{{"barrier","fence"},{"height","1.7"}}});
    const auto g=s.geo(0,7);s.osm.features.push_back({51,g.x,g.y,{{"barrier","gate"},{"width","4"}}});
    auto out=s.gardens();CHECK(out.stats["inferredGardens"]==0);CHECK(out.stats["silencedBySurvey"]==1);
    CHECK(out.stats["measuredSegments"]==1);
    double high=0;
    for(const auto& p:out.parts)for(auto q:p.mesh.positions){high=std::max(high,q.y);CHECK(std::abs(q.x)>=1.96);}
    NEAR(high,1.7,.03);
}
TEST(Gardens, water_and_paving_silence_a_plot_without_trapping_a_house) {
    Scene s;
    auto water=clip::Paths64{clip::kMetres.path({{-40,3},{40,3},{40,50},{-40,50}})};
    auto out=buildGardens(s.osm,planResidential(s.osm,s.anchor),s.tile,s.anchor,s.grid,{},water);
    CHECK(out.parts.empty());CHECK(out.stats["inferredGardens"]==0);
}
TEST(Gardens, the_budget_drops_complete_lots_and_reports_why) {
    Scene s;auto out=s.gardens(200);CHECK(out.parts.empty());CHECK(out.stats["droppedForBudget"]==1);
    s.house(11,50,23);auto generous=s.gardens(12000),small=s.gardens(2100);
    CHECK(vertices(small)<=2100);CHECK(vertices(generous)<=12000);
    CHECK(generous.stats["inferredGardens"].get<int>()>=small.stats["inferredGardens"].get<int>());
    CHECK(writeGlb(generous.parts)==writeGlb(s.gardens(12000).parts));
}
TEST(Gardens, inferred_gabarits_keep_measured_dimensions_and_city_defaults) {
    const auto p=profileByKey("FRANCE");OrientedBox box;box.halfU=6;box.halfV=4;
    const Tags guessed{{"r1:residential","fr.brittany.detached"}};
    for(int id=0;id<40;++id) {
        auto g=planGabarit(guessed,id,p,box,1);CHECK(g.storeys<=2);CHECK(g.heightSource=="atlas:detached");
        auto measured=guessed;measured["height"]="8.5";measured["roof:shape"]="flat";
        g=planGabarit(measured,id,p,box,1);NEAR(g.wallHeight,8.5,1e-9);CHECK(g.heightSource=="tag:height");
        CHECK(g.roofShape=="flat" && g.roofSource=="tag:shape");
    }
    const auto paris=profileByKey("PARIS");
    auto city=planGabarit({},10,paris,box,1);CHECK(city.heightSource=="atlas");CHECK(city.storeys>=4);
}
TEST(Gardens, surveyed_barriers_and_gates_are_retained_from_osm) {
    const nlohmann::json document={{"elements",{
        {{"type","node"},{"id",1},{"lon",0},{"lat",0}},
        {{"type","node"},{"id",2},{"lon",.001},{"lat",0},{"tags",{{"barrier","gate"},{"width","3"}}}},
        {{"type","way"},{"id",9},{"nodes",{1,2}},{"tags",{{"barrier","fence"},{"fence_type","chain_link"}}}}
    }}};
    auto osm=normalizeOsm(document);CHECK(osm.barriers.size()==1);CHECK(osm.features.size()==1);
    CHECK(osm.barriers[0].tags.at("fence_type")=="chain_link");
}

TEST(BusShelters, a_small_unknown_footprint_by_a_stop_is_an_open_shelter_not_a_house) {
    Scene s;s.osm.buildings.clear();
    s.osm.buildings.push_back({20,{s.geo(-1.5,7),s.geo(1.5,7),s.geo(1.5,9),s.geo(-1.5,9),s.geo(-1.5,7)},{{"building","yes"}}});
    const auto at=s.geo(0,6);s.osm.features.push_back({21,at.x,at.y,{{"highway","bus_stop"}}});
    auto buildings=s.osm.buildings;auto report=classifyBusShelters(buildings,s.osm,s.anchor,true);
    CHECK(report.size()==1);CHECK(report[0]["busStop"]==21);CHECK(buildings[0].tags["r1:bus-shelter"]=="inferred");
    const std::vector<const OsmWay*> ways{&buildings[0]};
    auto out=buildBuildings(ways,[&](double x,double y){return s.anchor.toEngine(x,y,0);},profileByKey("FRANCE"),{},-1,0,{},{});
    CHECK(out.stats.busShelters==1);CHECK(out.interiors.empty());CHECK(out.footprints.size()==3);
    bool roof=false;
    for(const auto& p:out.parts) {
        CHECK(p.name.find("Walls")==std::string::npos);roof |= p.name=="Bus shelter roof";
        for(auto v:p.mesh.positions)CHECK(v.y<2.4);
    }
    CHECK(roof);for(const auto& r:out.footprints)CHECK(!pointInPolygon({0,8},r));
}
TEST(BusShelters, an_explicit_absence_or_a_dense_city_silences_the_guess) {
    Scene s;s.osm.buildings.clear();
    s.osm.buildings.push_back({20,{s.geo(0,7),s.geo(3,7),s.geo(3,9),s.geo(0,9),s.geo(0,7)},{{"building","yes"}}});
    const auto at=s.geo(0,6);s.osm.features.push_back({21,at.x,at.y,{{"highway","bus_stop"},{"shelter","no"}}});
    auto b=s.osm.buildings;CHECK(classifyBusShelters(b,s.osm,s.anchor,true).empty());
    s.osm.features[0].tags.erase("shelter");CHECK(classifyBusShelters(b,s.osm,s.anchor,false).empty());
    b[0].tags["power"]="substation";CHECK(classifyBusShelters(b,s.osm,s.anchor,true).empty());
}
TEST(BusShelters, tagged_transport_shelters_work_without_country_inference) {
    Scene s;s.osm.buildings.clear();
    s.osm.buildings.push_back({20,{s.geo(0,7),s.geo(3,7),s.geo(3,9),s.geo(0,9),s.geo(0,7)},
        {{"building","roof"},{"amenity","shelter"},{"shelter_type","public_transport"}}});
    auto b=s.osm.buildings;auto report=classifyBusShelters(b,s.osm,s.anchor,false);
    CHECK(report.size()==1);CHECK(b[0].tags["r1:bus-shelter"]=="measured");
}
TEST(Gardens, mapped_hedges_use_shared_shrubs_and_leave_the_gate_open) {
    Scene s;s.osm.buildings.clear();
    s.osm.barriers.push_back({80,{s.geo(-18,9),s.geo(18,9)},{{"barrier","hedge"},{"height","1.5"}}});
    const auto gate=s.geo(0,9);s.osm.features.push_back({81,gate.x,gate.y,{{"barrier","gate"},{"width","4"}}});
    auto ground=[&](double x,double y){return s.anchor.toEngine(x,y,0);};
    const auto plants=planNature(s.osm,s.tile,s.anchor,ground,320);
    int found=0;
    for(const auto& n:plants.nodes)if(n["name"].get<std::string>().find("mapped-hedge")==7) {
        ++found;const auto& p=n["transform"]["position"];CHECK(std::abs(p[0].get<double>())>=3.5);
        CHECK(n["children"][1]["importedFrom"]=="assets/models/external/nature_selected/shrub.glb");
        NEAR(n["transform"]["scale"][1].get<double>(),1.5,1e-9);
    }
    CHECK(found>=6);CHECK(plants.stats["placed"].get<int>()<=320);
}
TEST(Gardens, a_front_plot_crossing_a_tile_border_is_drawn_on_both_tiles) {
    Scene s;s.osm.buildings.clear();
    s.osm.roads[0].points={s.geo(-450,0),s.geo(450,0)};
    const auto outline=Drape(s.grid,s.anchor).outline();
    double edge=-1e9;for(const auto& p:outline)edge=std::max(edge,clip::kMetres.back(p).x);
    s.house(10,edge+5,23);
    auto plan=planResidential(s.osm,s.anchor);CHECK(plan.homes.size()==1);
    auto out=s.gardens();CHECK(out.stats["inferredGardens"]==1);
    bool lawn=false;
    for(const auto& p:out.parts)if(p.name=="Residential lawns") {
        lawn=true;for(auto q:p.mesh.positions)CHECK(q.x<=edge+.01);
    }
    CHECK(lawn);
}

TEST(RuralCountries, frontages_follow_local_courts_and_enclosures) {
    struct Place { const char* country; double lon,lat; const char* rule; const char* enclosure; };
    for(const auto& place:std::vector<Place>{{"US",-72.169,44.325,"us.new_england.detached","open"},
        {"RU",40.437,56.428,"ru.european.village","timber"},
        {"MA",-8.96488,29.69537,"ma.anti_atlas.village","wall"},
        {"JP",136.4806,35.46728,"jp.village","wall"}}) {
        Scene s(place.lon,place.lat);s.osm.country=place.country;
        auto plan=planResidential(s.osm,s.anchor);CHECK(plan.homes.size()==1);
        CHECK(plan.homes[0].style->key==place.rule);
        const auto out=s.gardens();CHECK(out.stats["inferredGardens"]==1);CHECK(vertices(out)<=12000);
        CHECK(out.stats["lots"][0]["enclosure"]==place.enclosure);
        bool lawn=false,court=false,wood=false,gate=false;
        for(const auto& part:out.parts) {
            lawn |= part.name=="Residential lawns";court |= part.name=="Residential mineral courts";
            wood |= part.name=="Garden timber fences";
            gate |= part.name=="Residential gates" || part.name=="Timber residential gates";
        }
        CHECK(lawn==(s.osm.country=="US" || s.osm.country=="RU"));
        CHECK(court==(s.osm.country=="MA" || s.osm.country=="JP"));
        CHECK(wood==(s.osm.country=="RU"));CHECK(gate==(s.osm.country!="US"));
        CHECK(clip::contains(out.drives,P2{0,15}));
    }
}
TEST(RuralCountries, all_four_dense_city_blocks_keep_the_original_profile) {
    for(const auto& place:std::vector<std::pair<std::string,P2>>{{"US",{-72.169,44.325}},
        {"RU",{40.437,56.428}},{"MA",{-8.96488,29.69537}},{"JP",{136.4806,35.46728}}}) {
        Scene s(place.second.x,place.second.y);s.osm.country=place.first;
        for(int z=-3;z<=3;++z)for(int x=-3;x<=3;++x)if(x || z)s.house(100+(z+3)*7+x+3,x*14,23+z*14);
        CHECK(planResidential(s.osm,s.anchor).homes.empty());CHECK(s.gardens().parts.empty());
    }
}
TEST(RuralCountries, regional_evidence_never_becomes_a_countrywide_stereotype) {
    Scene farRussia(130,55);farRussia.osm.country="RU";CHECK(planResidential(farRussia.osm,farRussia.anchor).homes.empty());
    Scene northMorocco(-5,35);northMorocco.osm.country="MA";CHECK(planResidential(northMorocco.osm,northMorocco.anchor).homes.empty());
    Scene wrongCountry(40.437,56.428);wrongCountry.osm.country="DE";CHECK(planResidential(wrongCountry.osm,wrongCountry.anchor).homes.empty());
}
TEST(RuralCountries, village_rows_have_local_roofs_without_invented_shared_gardens) {
    Scene s(136.4806,35.46728);s.osm.country="JP";s.house(11,12,23);
    auto plan=planResidential(s.osm,s.anchor);CHECK(plan.homes.size()==2);
    CHECK(!plan.homes[0].gardenEligible && !plan.homes[1].gardenEligible);
    CHECK(s.gardens().parts.empty());
}
TEST(RuralCountries, tagged_dimensions_override_every_new_country_prior) {
    OrientedBox box;box.halfU=6;box.halfV=4;
    for(const auto* key:{"us.detached","us.new_england.detached","ru.european.village","ma.anti_atlas.village","jp.village"}) {
        const auto* s=residentialStyle(key);CHECK(s);
        const Tags tags{{"r1:residential",key},{"height","8.5"},{"building:levels","2"},{"roof:shape","flat"}};
        auto g=planGabarit(tags,10,profileByKey("GENERIC"),box,1);
        NEAR(g.wallHeight,8.5,1e-9);CHECK(g.storeys==2);CHECK(g.heightSource=="tag:height");
        CHECK(g.roofSource=="tag:shape");
    }
}
TEST(RuralCountries, observed_compact_villages_get_roofs_but_never_dense_gardens) {
    Scene s(136.4806,35.46728);s.osm.country="JP";
    for(int z=-2;z<=2;++z)for(int x=-2;x<=2;++x)if(x || z)s.house(100+(z+2)*5+x+2,x*14,23+z*14);
    CHECK(planResidential(s.osm,s.anchor).homes.empty());
    const auto at=s.geo(0,23);s.osm.features.push_back({400,at.x,at.y,{{"place","village"}}});
    auto plan=planResidential(s.osm,s.anchor);CHECK(!plan.homes.empty());
    for(const auto& h:plan.homes)CHECK(!h.gardenEligible);
    CHECK(s.gardens().parts.empty());
    s.osm.features.push_back({401,at.x,at.y,{{"place","city"}}});
    CHECK(planResidential(s.osm,s.anchor).homes.empty());
}
TEST(RuralCountries, small_annexes_are_low_and_measured_heights_win) {
    Scene s(40.440817,56.428137);s.osm.country="RU";
    s.osm.buildings.push_back({12,{s.geo(12,24),s.geo(15,24),s.geo(15,27),s.geo(12,27),s.geo(12,24)},{{"building","yes"}}});
    auto plan=planResidential(s.osm,s.anchor);CHECK(plan.homes.size()==2);CHECK(!plan.homes[1].gardenEligible);
    OrientedBox box;box.halfU=1.5;box.halfV=1.5;
    Tags tags{{"r1:residential","ru.european.village"},{"r1:rural-annex","yes"}};
    auto g=planGabarit(tags,12,profileByKey("GENERIC"),box,1);CHECK(g.storeys==1);NEAR(g.wallHeight,2.3,1e-9);
    CHECK(g.heightSource=="atlas:rural-annex");
    tags["height"]="6";g=planGabarit(tags,12,profileByKey("GENERIC"),box,1);CHECK(g.heightSource=="tag:height");
    s.osm.buildings[1].tags["amenity"]="school";CHECK(planResidential(s.osm,s.anchor).homes.size()==1);
    s.osm.country="FR";CHECK(planResidential(s.osm,s.anchor).homes.size()==1);
}
TEST(RuralCountries, pedestrian_village_rows_and_compound_home_roofs_keep_their_footprints) {
    Scene s(137.59561,35.577542);s.osm.country="JP";s.osm.roads[0].tags["highway"]="pedestrian";
    auto plan=planResidential(s.osm,s.anchor);CHECK(plan.homes.size()==1);CHECK(!plan.homes[0].gardenEligible);
    CHECK(plan.homes[0].style->key=="jp.kiso.historic_rows");
    const auto c=s.geo(0,23);s.osm.features.push_back({200,c.x,c.y,{{"place","city"}}});
    CHECK(planResidential(s.osm,s.anchor).homes.empty());
    const Ring l{{0,0},{12,0},{12,4},{5,4},{5,10},{0,10},{0,0}};
    OsmWay home{500,l,{{"building","house"},{"r1:residential","us.new_england.detached"}}};
    auto build=[&]{return buildBuildings({&home},[](double x,double y){return P3{x,0,y};},profileByKey("GENERIC"),{},50,.08,{},{});};
    auto out=build();CHECK(out.stats.ruralGabarits[0]["roofComponents"].get<int>()==2);
    CHECK(out.stats.ruralGabarits[0]["roofShape"]!="flat");CHECK(out.footprints.size()==1);
    NEAR(std::abs(polygonArea(out.footprints[0])),78,1e-6);
    home.tags["roof:shape"]="flat";out=build();CHECK(out.stats.ruralGabarits[0]["roofShape"]=="flat");
    home.tags.erase("roof:shape");home.tags["r1:residential"]="fr.detached";
    out=build();CHECK(out.stats.ruralGabarits[0]["roofShape"]=="flat");
}
TEST(RuralCountries, small_town_shop_architecture_does_not_invent_private_plots_or_replace_public_uses) {
    Scene s(-8.972913,29.720753);s.osm.country="MA";s.osm.buildings[0].tags["shop"]="clothes";
    auto plan=planResidential(s.osm,s.anchor);CHECK(plan.homes.size()==1);CHECK(!plan.homes[0].gardenEligible);
    CHECK(s.gardens().parts.empty());
    OrientedBox box;box.halfU=6;box.halfV=4;
    auto g=planGabarit({{"r1:residential","ma.tafraout.small_town"}},10,profileByKey("GENERIC"),box,1,true);
    CHECK(g.storeys<=2);CHECK(g.roofShape=="flat");
    s.osm.buildings[0].tags["amenity"]="school";CHECK(planResidential(s.osm,s.anchor).homes.empty());
    s.osm.buildings[0].tags.erase("amenity");const auto c=s.geo(0,23);
    s.osm.features.push_back({200,c.x,c.y,{{"place","city"}}});CHECK(planResidential(s.osm,s.anchor).homes.empty());
    Scene wood(40.440817,56.428137);wood.osm.country="RU";
    for(const auto& part:wood.gardens().parts)CHECK(part.material.baseColorTexture.find("_and_window_")==std::string::npos);
}
