#include "check.hpp"
#include "gen/interiors.hpp"
#include "gen/buildings.hpp"
#include "gen/clip.hpp"
#include "gen/cook.hpp"
#include <set>
using namespace r1;
namespace {
InteriorPlan room(const std::string& recipe) {
    InteriorPlan p;p.id=42;p.recipe=recipe;p.name=recipe;p.floor=10;p.ceiling=13;p.approach=10;
    p.doorStyle="swing";p.width=1.2;p.along={1,0};p.inward={0,1};
    p.ring={{-6,0},{6,0},{6,12},{-6,12}};return p;
}
bool hasFixture(const InteriorLayout& l,const std::string& kind) {
    return std::any_of(l.fixtures.begin(),l.fixtures.end(),[&](const auto& f){return f.kind==kind;});
}
}
TEST(Interior, building_use_selects_appropriate_recipes) {
    CHECK(interiorRecipe({{"building","house"},{"access","private"}})=="home");
    CHECK(interiorRecipe({{"building","yes"},{"amenity","police"}})=="police");
    CHECK(interiorRecipe({{"building","yes"},{"military","gendarmerie"}})=="police");
    CHECK(interiorRecipe({{"building","school"}})=="school");
    CHECK(interiorRecipe({{"building","yes"},{"office","company"}})=="office");
    CHECK(interiorRecipe({{"building","yes"},{"shop","car_repair"}})=="garage");
    CHECK(interiorRecipe({{"building","garages"}})=="garage");
    CHECK(interiorRecipe({{"building","church"}})=="worship");
    CHECK(interiorRecipe({{"building","mosque"}})=="mosque");
    CHECK(interiorRecipe({{"building","warehouse"}})=="warehouse");
    CHECK(interiorRecipe({{"building","roof"}}).empty());
    CHECK(interiorRecipe({{"building","retail"},{"shop","bakery"}})=="bakery");
}
TEST(Interior, home_has_separate_living_kitchen_bedroom_and_wet_room) {
    auto p=room("home");auto l=layoutInterior(p);CHECK(l.rooms.size()==4);
    for(const auto& kind:{"sofa","coffee_table","kitchen","dining","bed","wardrobe","shower","toilet","basin","partition"})CHECK_MSG(hasFixture(l,kind),kind);
    CHECK(!hasFixture(l,"shelf"));CHECK(!hasFixture(l,"checkout"));
    for(double v=.5;v<11.5;v+=.1)CHECK(!interiorBlocked(p,l,p.point(0,v),1));
    // Room doorways stay open off the corridor; walls elsewhere block.
    CHECK(!interiorBlocked(p,l,p.point(.98,1.7),1));
    CHECK(interiorBlocked(p,l,p.point(.98,3.5),1));
    CHECK(interiorBlocked(p,l,p.point(0,0),0));CHECK(!interiorBlocked(p,l,p.point(0,0),1));
}
TEST(Interior, a_small_house_and_an_offset_entrance_keep_all_domestic_functions) {
    for(auto ring:std::vector<Ring>{{{-4,0},{4,0},{4,8},{-4,8}},{{-.8,0},{8,0},{8,13},{-.8,13}}}) {
        auto p=room("home");p.ring=ring;auto l=layoutInterior(p);
        for(const auto& kind:{"sofa","kitchen","bed","toilet","shower"})CHECK_MSG(hasFixture(l,kind),kind);
        for(double v=.5;v<7.;v+=.1)CHECK(!interiorBlocked(p,l,p.point(0,v),1));
    }
}
TEST(Interior, civic_office_and_garage_have_specific_furnishings) {
    CHECK(hasFixture(layoutInterior(room("police")),"reception"));
    CHECK(hasFixture(layoutInterior(room("police")),"meeting"));
    CHECK(hasFixture(layoutInterior(room("police")),"filing"));
    CHECK(hasFixture(layoutInterior(room("school")),"school_desk"));
    CHECK(hasFixture(layoutInterior(room("school")),"blackboard"));
    CHECK(hasFixture(layoutInterior(room("office")),"desk"));
    CHECK(hasFixture(layoutInterior(room("garage")),"vehicle"));
    CHECK(hasFixture(layoutInterior(room("garage")),"workbench"));
    const auto mosque=layoutInterior(room("mosque"));CHECK(hasFixture(mosque,"prayer_mat"));CHECK(!hasFixture(mosque,"pew"));
    for(const auto& f:mosque.fixtures)CHECK(!interiorBlocked(room("mosque"),mosque,f.at,1));
}
TEST(Interior, seeded_layout_and_prototypes_roundtrip_fit_and_respect_budget) {
    for(const char* recipe:{"home","police","school","office","garage","clinic","worship","mosque","warehouse","restaurant","prison"}) {
        auto p=room(recipe);p.region="brittany";p.exteriorVehicles={{-4,10,-6}};
        auto q=InteriorPlan::read(p.json());CHECK(q.json()==p.json());
        const auto a=layoutInterior(p),b=layoutInterior(q);CHECK(a.fixtures.size()==b.fixtures.size());
        CHECK(a.fixtures.size()<=128);size_t vertices=0;std::set<std::string> keys;
        for(const auto& part:buildInteriorShell(p))vertices+=part.mesh.vertexCount();
        for(size_t i=0;i<a.fixtures.size();++i) {
            const auto& f=a.fixtures[i];CHECK(interiorFixtureKey(f)==interiorFixtureKey(b.fixtures[i]));
            for(double u:{-f.size.x/2,f.size.x/2})for(double v:{-f.size.y/2,f.size.y/2})CHECK(pointInPolygon(p.point(f.at.x+u,f.at.y+v),p.ring));
            auto mesh=buildInteriorFixture(f);CHECK(writeGlb(mesh)==writeGlb(buildInteriorFixture(b.fixtures[i])));
            if(keys.insert(interiorFixtureKey(f)).second)for(auto& part:mesh)vertices+=part.mesh.vertexCount();
        }
        for(auto& part:buildInteriorDoor(p))vertices+=2*part.mesh.vertexCount();
        CHECK_MSG(vertices<24000,std::string(recipe)+" vertices="+std::to_string(vertices));
        for(double v=.5;v<11.5;v+=.2)CHECK(!interiorBlocked(p,a,p.point(0,v),1));
    }
}
TEST(Interior, concave_and_small_footprints_never_furnish_outside) {
    for(auto ring:std::vector<Ring>{{{0,0},{4,0},{4,5},{0,5}},{{-6,0},{6,0},{6,4},{2,4},{2,12},{-6,12}}}) {
        auto p=room("home");p.ring=ring;CHECK(chooseRetailPortal(p,{{0,-3}}));
        auto l=layoutInterior(p);for(auto f:l.fixtures)for(double u:{-f.size.x/2,f.size.x/2})for(double v:{-f.size.y/2,f.size.y/2})
            CHECK(pointInPolygon(p.point(f.at.x+u,f.at.y+v),p.ring));
    }
}
TEST(Interior, civic_nodes_private_entrances_and_campuses_are_observations) {
    OsmData osm;OsmWay b;b.id=42;b.tags={{"building","yes"}};b.points={{0,0},{12,0},{12,12},{0,12},{0,0}};
    osm.features={{1,6,6,{{"amenity","police"},{"name","Gendarmerie"}}},{2,6,0,{{"entrance","main"},{"access","private"}}}};
    auto ground=[](double x,double y){return P3{x,10,y};};
    auto ways=interiorBuildings({b},osm,ground);CHECK(tagOr(ways[0].tags,"amenity")=="police");
    CHECK(tagOr(ways[0].tags,"r1:useSource")=="measured:tenant-node");CHECK(tagOr(ways[0].tags,"r1:entranceSource")=="measured");
    CHECK(!has(b.tags,"amenity"));b.tags["building"]="house";
    CHECK(interiorRecipe(interiorBuildings({b},osm,ground)[0].tags)=="home");
    b.tags["building"]="yes";osm.features.clear();OsmWay campus=b;campus.tags={{"amenity","school"}};osm.landcover.push_back(campus);
    CHECK(interiorRecipe(interiorBuildings({b},osm,ground)[0].tags)=="school");
}
TEST(Interior, dense_facade_keeps_entrances_rooms_and_observed_height) {
    OsmWay w;w.id=42;w.tags={{"building","house"},{"height","8"},{"roof:shape","flat"}};
    w.points={{-6,0},{6,0},{6,12},{-6,12},{-6,0}};
    auto ground=[](double x,double y){return P3{x,10,y};};
    const auto& profile=profileFor(2.35,48.85);
    auto a=buildBuildings({&w},ground,profile,{},500,0,{},{});
    auto b=buildBuildings({&w},ground,profile,{},500,0,{},{},BuildingLod::SimpleRoofline);
    CHECK(a.interiors.size()==1);CHECK(a.interiors[0].json()==b.interiors[0].json());
    CHECK(a.footprints==b.footprints);CHECK(a.tops==b.tops);NEAR(b.tops[0],18+profile.parapetHeight,1e-9);
    size_t full=0,compact=0;for(const auto& p:a.parts)full+=p.mesh.vertexCount();for(const auto& p:b.parts)compact+=p.mesh.vertexCount();
    CHECK(compact<full);
    const auto& plan=b.interiors[0];std::set<std::array<double,3>> corners;
    for(const auto& part:b.parts)if(part.name.rfind("Walls",0)==0) {
        const auto& mesh=part.mesh;
        for(size_t i=0;i<mesh.positions.size();++i) {
            const auto v=mesh.positions[i],n=mesh.normals[i];auto q=plan.local({v.x,v.z});
            if(std::abs(q.y)<1e-7&&n.x*(-plan.inward.x)+n.z*(-plan.inward.y)>.99)corners.insert({v.x,v.y,v.z});
        }
        for(size_t i=0;i+2<mesh.indices.size();i+=3) {
            Ring face;bool onPortal=true;
            for(size_t j=i;j<i+3;++j){auto v=mesh.positions[mesh.indices[j]];auto q=plan.local({v.x,v.z});onPortal&=std::abs(q.y)<1e-7;face.push_back({q.x,v.y});}
            if(onPortal)CHECK(!pointInPolygon({0,plan.floor+1.05},face));
        }
    }
    CHECK_MSG(corners.size()==8,corners.size());
}
TEST(Interior, garage_forecourt_cars_are_synthesized_only_on_clear_ground) {
    const auto tile=tileAt(2.35,48.85);const auto center=tile.center();const auto anchor=Anchor::at(center.x,center.y,0);
    auto way=[&](int64_t id,const Ring& r,const Tags& tags) {
        OsmWay w;w.id=id;w.tags=tags;for(auto p:r){auto q=anchor.toGeodetic(p.x,0,p.y);w.points.push_back({q.x,q.y});}w.points.push_back(w.points.front());return w;
    };
    OsmData osm;osm.queryVersion=10;osm.interiorUsesQueried=true;
    osm.buildings.push_back(way(42,{{-8,0},{8,0},{8,16},{-8,16}},{{"building","garage"},{"height","5"},{"roof:shape","flat"}}));
    Observations in;in.tile=tile;in.elevations={tile.bounds(),2,{10,10,10,10}};in.elevationSource="test:flat";
    auto plans=[&](){in.osm=std::make_shared<const OsmData>(osm);return cookTile(in).manifest.at("interiors");};
    auto before=plans();CHECK(before.size()==1);CHECK(before[0]["exteriorVehicles"].size()==2);
    CHECK(before[0]["vehicleSource"]=="synthesized:clear-forecourt");
    const auto plan=InteriorPlan::read(before[0]);const auto door=anchor.toGeodetic(plan.door.x,0,plan.door.y);
    osm.features.push_back({45,door.x,door.y,{{"entrance","main"}}});
    // A mapped park excludes parking even where ground classification is grass.
    osm.landcover.push_back(way(43,{plan.point(-12,-12),plan.point(12,-12),plan.point(12,0),plan.point(-12,0)},{{"leisure","park"}}));
    CHECK(plans()[0]["exteriorVehicles"].empty());osm.landcover.clear();
    // The mapped road in front changes the inferred target, but cannot receive cars.
    auto road=way(44,{plan.point(-30,-5),plan.point(30,-5),plan.point(30,-10),plan.point(-30,-10)},{{"highway","service"},{"width","6"}});
    osm.roads.push_back(road);CHECK(plans()[0]["exteriorVehicles"].empty());
}
