#include "check.hpp"
#include "layout_clearance.hpp"
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
TEST(Interior, generated_boxes_have_outward_winding_and_normals) {
    for(double yaw:{0.,.75,2.4}) {
        const P3 center{3,7,-2};Mesh mesh;mesh.addBox(center,{2,3,4},yaw);
        CHECK(mesh.vertexCount()==24);CHECK(mesh.indices.size()==36);
        for(size_t i=0;i<mesh.indices.size();i+=3) {
            auto a=mesh.positions[mesh.indices[i]],b=mesh.positions[mesh.indices[i+1]],c=mesh.positions[mesh.indices[i+2]];
            auto normal=faceNormal(a,b,c);P3 away{(a.x+b.x+c.x)/3-center.x,(a.y+b.y+c.y)/3-center.y,(a.z+b.z+c.z)/3-center.z};
            CHECK(normal.x*away.x+normal.y*away.y+normal.z*away.z>0);
            auto stored=mesh.normals[mesh.indices[i]];NEAR(stored.x,normal.x,1e-9);NEAR(stored.y,normal.y,1e-9);NEAR(stored.z,normal.z,1e-9);
        }
    }
}
TEST(Interior, rounded_upholstery_is_outward_and_smooth) {
    const InteriorFixture sofa{{0,0},{2.3,.9},.9,"sofa",0};
    for(const auto& part:buildInteriorFixture(sofa))if(part.name=="Interior upholstery") {
        bool rounded=false;
        for(const auto& normal:part.mesh.normals) {
            NEAR(normal.x*normal.x+normal.y*normal.y+normal.z*normal.z,1.,1e-9);
            rounded|=std::abs(normal.x)>.1&&std::abs(normal.y)>.1;
        }
        CHECK(rounded);
        for(size_t i=0;i<part.mesh.indices.size();i+=3) {
            auto ia=part.mesh.indices[i],ib=part.mesh.indices[i+1],ic=part.mesh.indices[i+2];
            auto n=faceNormal(part.mesh.positions[ia],part.mesh.positions[ib],part.mesh.positions[ic]);
            const auto s=part.mesh.normals[ia];CHECK(n.x*s.x+n.y*s.y+n.z*s.z>0);
        }
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
    for(double v=.5;v<11.5;v+=.1)CHECK(!furnitureOccupies(l,{0,v}));
    // Room doorways stay open off the corridor; walls elsewhere block.
    CHECK(!furnitureOccupies(l,{.98,1.7}));
    CHECK(!furnitureOccupies(l,{.98,3.5})); // Open living/kitchen frontage.
    CHECK(furnitureOccupies(l,{.98,9.5})); // Private rooms retain walls.
    for(const auto& kind:{"fridge","bedside","lamp","plant","rug"})CHECK_MSG(hasFixture(l,kind),kind);
    CHECK(!furnitureOccupies(l,{0,0}));
}
TEST(Interior, a_small_house_and_an_offset_entrance_keep_all_domestic_functions) {
    for(auto ring:std::vector<Ring>{{{-4,0},{4,0},{4,8},{-4,8}},{{-.8,0},{8,0},{8,13},{-.8,13}}}) {
        auto p=room("home");p.ring=ring;auto l=layoutInterior(p);
        for(const auto& kind:{"sofa","kitchen","bed","toilet","shower"})CHECK_MSG(hasFixture(l,kind),kind);
        for(double v=.5;v<7.;v+=.1)CHECK(!furnitureOccupies(l,{0,v}));
    }
}
TEST(Interior, adjoining_private_rooms_have_dividing_walls) {
    auto p=room("home");p.ring={{-8,0},{8,0},{8,30},{-8,30}};
    const auto layout=layoutInterior(p);
    for(const auto& r:layout.rooms)if(r.use=="bedroom"||r.use=="bathroom") {
        double left=1e30,right=-1e30,back=-1e30;
        for(auto q:r.ring){q=p.local(q);left=std::min(left,q.x);right=std::max(right,q.x);back=std::max(back,q.y);}
        if(back>=29.4)continue; // The final room meets the building's back wall.
        bool divided=false;
        for(const auto& f:layout.fixtures)if(f.kind=="partition"&&std::abs(f.at.y-back)<.01&&f.size.x>=right-left-.01)divided=true;
        CHECK_MSG(divided,r.use);
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
    for(const auto& f:mosque.fixtures)CHECK(!furnitureOccupies(mosque,f.at));
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
        for(double v=.5;v<11.5;v+=.2)CHECK(!furnitureOccupies(a,{0,v}));
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
    CHECK(a.footprints==b.footprints);CHECK(a.tops==b.tops);NEAR(b.tops[0],18,1e-9);
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
TEST(Interior, a_door_above_the_ground_is_reached_by_steps_of_the_place) {
    auto p=room("home");p.approach=9.1;p.stairBase=8.9;p.steps=5;p.region="PARIS";
    const auto parts=buildInteriorShell(p);
    const MeshPart* stairs=nullptr;
    for(const auto& part:parts)if(part.name=="Entrance stairs")stairs=&part;
    CHECK(stairs&&!stairs->mesh.empty());
    CHECK(stairs->material.name=="Stone entrance steps");
    // Photographed stone at its real size, never a facade sheet with its window.
    CHECK(!stairs->material.baseColorTexture.empty());
    CHECK(stairs->material.baseColorTexture.find("_and_window_")==std::string::npos);
    // A tread at every riser's height, all facing up, none above the door.
    std::set<long> treads;double top=-1e9,outmost=0;
    for(size_t i=0;i<stairs->mesh.indices.size();i+=3) {
        const P3 a=stairs->mesh.positions[stairs->mesh.indices[i]],b=stairs->mesh.positions[stairs->mesh.indices[i+1]],
                 c=stairs->mesh.positions[stairs->mesh.indices[i+2]];
        const P3 n=faceNormal(a,b,c);
        if(n.y>.99)treads.insert(std::lround(a.y*100));
        top=std::max({top,a.y,b.y,c.y});outmost=std::min({outmost,a.z,b.z,c.z});
        // Sides face sideways, risers and nosings out of the door.
        if(std::abs(n.y)<.01&&std::abs(n.x)<.01)CHECK(n.z<0);
        if(std::abs(n.y)<.01&&std::abs(n.z)<.01)CHECK(n.x*((a.x+b.x+c.x)/3)>0);
    }
    CHECK((treads==std::set<long>{1000,982,964,946,928}));
    NEAR(top,10.,1e-9);
    NEAR(outmost,-(InteriorPlan::stairRun(5)+.03),1e-9);
    // The walked slope lies inside the flight, under every tread.
    const double run=p.approachRun();
    for(double v=0;v<=run;v+=.01) {
        const double slope=10-(10-9.1)*v/run;
        const int tread=v<=InteriorPlan::kLanding?0:int(std::ceil((v-InteriorPlan::kLanding)/InteriorPlan::kTread-1e-9));
        CHECK(slope<=10-tread*.18+1e-9);
    }
    // Commercial doors and other places get concrete; no steps, no flight.
    p.recipe="garage";CHECK(buildEntranceStairs(p).material.name=="Concrete entrance steps");
    p.recipe="home";p.region="TOKYO";CHECK(buildEntranceStairs(p).material.name=="Concrete entrance steps");
    p.steps=0;for(const auto& part:buildInteriorShell(p))CHECK(part.name!="Entrance stairs");
    // The plan carries its flight through the manifest.
    p.steps=5;const auto back=InteriorPlan::read(p.json());CHECK(back.steps==5&&back.stairBase==8.9);
}
