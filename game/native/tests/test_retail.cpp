#include "check.hpp"
#include "layout_clearance.hpp"
#include "gen/retail.hpp"
#include "gen/buildings.hpp"
#include "gen/predict.hpp"
#include "gen/cook.hpp"
#include <set>
using namespace r1;
namespace {
InteriorPlan room() {
    InteriorPlan p;p.id=42;p.ring={{-15,0},{15,0},{15,25},{-15,25}};
    p.door={0,0};p.along={1,0};p.inward={0,1};p.floor=10;p.ceiling=14;p.approach=9.5;
    p.name="Marché du Centre";p.recipe="market";return p;
}
OsmWay geoWay(int64_t id,const Ring& r,const Tags& t,const Anchor& a) {
    OsmWay w;w.id=id;w.tags=t;for(auto p:r){auto g=a.toGeodetic(p.x,0,p.y);w.points.push_back({g.x,g.y});}
    w.points.push_back(w.points.front());return w;
}
}
TEST(Retail, sliding_portal_geometry_and_clear_aisle) {
    auto p=room();auto layout=layoutInterior(p);
    CHECK(!buildDoorLeaf(p.width).empty());
    CHECK(!furnitureOccupies(layout,{0,0}));
    CHECK(!furnitureOccupies(layout,{3,0}));
    for(double v=-4;v<24;v+=.1)CHECK(!furnitureOccupies(layout,{0,v}));
    for(auto f:layout.fixtures)CHECK(furnitureOccupies(layout,{f.at.x,f.at.y}));
    NEAR(slideDoor(0,true,1),1,1e-9);NEAR(slideDoor(1,false,3),0,1e-9);
    NEAR(slideDoor(.5,true,-1),.5,1e-9);
}
TEST(Retail, deterministic_layout_roundtrips_and_stays_inside_concave_shell) {
    auto p=room();p.ring={{-15,0},{15,0},{15,10},{6,10},{6,25},{-15,25}};
    const auto q=InteriorPlan::read(p.json());const auto a=layoutInterior(p),b=layoutInterior(q);
    CHECK(writeGlb(buildInteriorShell(p))==writeGlb(buildInteriorShell(q)));
    CHECK(a.fixtures.size()==b.fixtures.size());
    for(size_t i=0;i<a.fixtures.size();++i)CHECK(writeGlb(buildInteriorFixture(a.fixtures[i]))==writeGlb(buildInteriorFixture(b.fixtures[i])));
    CHECK(!a.fixtures.empty());
    for(auto f:a.fixtures)for(double u:{-f.size.x/2,f.size.x/2})for(double v:{-f.size.y/2,f.size.y/2})
        CHECK(pointInPolygon(p.point(f.at.x+u,f.at.y+v),p.ring));
    size_t count=0;for(auto& part:buildInteriorShell(p))count+=part.mesh.vertexCount();
    std::set<std::string> seen;
    for(auto& f:a.fixtures)if(seen.insert(interiorFixtureKey(f)).second)
        for(auto& part:buildInteriorFixture(f))count+=part.mesh.vertexCount();
    CHECK(count<24000);
}
TEST(Retail, tenant_nodes_and_measured_entrances_are_used_without_mutating_osm) {
    auto a=Anchor::at(2.35,48.85,0);OsmData osm;
    auto w=geoWay(1,{{-15,0},{15,0},{15,25},{-15,25}},{{"building","yes"},{"height","9"}},a);
    osm.buildings.push_back(w);auto center=a.toGeodetic(0,0,10),entrance=a.toGeodetic(6,0,0);
    osm.features.push_back({2,center.x,center.y,{{"shop","bakery"},{"name","Pain doré"}}});
    osm.features.push_back({3,entrance.x,entrance.y,{{"entrance","main"}}});
    auto ground=[&](double x,double y){auto p=a.toEngine(x,y,0);p.y=10;return p;};
    auto enriched=retailBuildings({&w},osm,ground);CHECK(!has(w.tags,"shop"));CHECK(tagOr(enriched[0].tags,"shop")=="bakery");
    auto built=buildBuildings({&enriched[0]},ground,profileFor(2.35,48.85),{},-1,0,{},{});
    CHECK(built.interiors.size()==1);auto p=built.interiors.front();
    CHECK(p.name=="Pain doré");CHECK(p.entranceSource=="measured");CHECK(p.recipe=="bakery");
    NEAR(p.door.x,6,.02);NEAR(p.door.y,0,.02);CHECK(p.footprint==0);
    // A supermarket mapped inside a mall anchors its sales floor there.
    auto mall=w;mall.tags={{"building","retail"},{"shop","mall"},{"name","Galerie"}};
    osm.features.push_back({4,center.x,center.y,{{"shop","supermarket"},{"name","Carrefour"}}});
    const auto anchored=retailBuildings({&mall},osm,ground)[0];
    CHECK(tagOr(anchored.tags,"r1:anchorName")=="Carrefour");
    NEAR(retailPoints(tagOr(anchored.tags,"r1:anchor"))[0].x,0,.02);
    NEAR(retailPoints(tagOr(anchored.tags,"r1:anchor"))[0].y,10,.02);
    osm.features.pop_back();
    // A staff door is no shop entrance.
    osm.features.back().tags["access"]="private";
    CHECK(tagOr(retailBuildings({&w},osm,ground)[0].tags,"r1:entranceSource")=="inferred");
    CHECK(facadeLettering(p.name,12,.45).has_value());
    CHECK(!retailUse({{"shop","funeral_directors"}}));CHECK(!retailUse({{"shop","vacant"}}));
}
TEST(Retail, parking_has_store_identity_bays_and_road_building_exclusions) {
    auto p=room();auto a=Anchor::at(2.35,48.85,0);OsmData osm;
    auto b=tileAt(2.35,48.85).bounds();ElevationGrid elevation{b,2,{10,10,10,10}};
    // Use a tile-centred frame as the terrain draper does.
    auto c=tileAt(2.35,48.85).center();a=Anchor::at(c.x,c.y,0);
    osm.landcover.push_back(geoWay(10,{{-25,-36},{25,-36},{25,3},{-25,3}},{{"amenity","parking"}},a));
    auto output=buildRetailParking(osm,{p},{p.ring},elevation,a);
    CHECK(output.manifest.size()==1);CHECK(output.manifest[0]["storeId"]==42);
    CHECK(output.manifest[0]["spaces"].get<int>()>8);CHECK(output.parts.size()==2);
    for(auto& part:output.parts)for(auto q:part.mesh.positions)CHECK(!pointInPolygon({q.x,q.z},p.ring));
    osm.landcover[0].tags={{"surface","concrete"}};
    CHECK(buildRetailParking(osm,{p},{p.ring},elevation,a).manifest.empty());
    osm.landcover[0].tags={{"amenity","parking"},{"parking","underground"}};
    CHECK(buildRetailParking(osm,{p},{p.ring},elevation,a).manifest.empty());
}
TEST(Retail, the_portal_prefers_a_mapped_entrance_that_opens_on_a_clear_aisle) {
    // A strip of shallow units along the front, the store proper behind.
    auto p=room();p.ring={{-20,0},{20,0},{20,5},{-10,5},{-10,30},{-20,30}};
    double offset=-1;
    CHECK(chooseRetailPortal(p,{{10,0},{-15,0}},{},&offset));
    NEAR(p.door.x,-15,1e-9);NEAR(p.door.y,0,1e-9);NEAR(offset,0,1e-9);
    // One mapped entrance is kept even facing a wall: it is measured.
    CHECK(chooseRetailPortal(p,{{10,0}},{},&offset));NEAR(p.door.x,10,1e-9);
    // An inferred target off the outline moves to the nearest clear door.
    CHECK(chooseRetailPortal(p,{{-5,-8}},{},&offset));NEAR(p.door.x,-12,1e-9);NEAR(p.door.y,0,1e-9);CHECK(offset>8);
    CHECK(retailFronts({{"r1:front","1.5,-2;3,4.25"}}).size()==2);
    CHECK(retailFronts({{"r1:front","1.5,-2;3,4.25"}})[1].y==4.25);
}
TEST(Retail, shopfront_contains_a_real_hole_not_a_door_drawn_on_a_wall) {
    auto p=room();auto parts=buildShopfront(p);
    // No triangle of the shell crosses the middle of the doorway at 1 m.
    for(auto& part:parts)if(part.name=="Shop shell")for(size_t i=0;i<part.mesh.indices.size();i+=3) {
        auto a=part.mesh.positions[part.mesh.indices[i]],b=part.mesh.positions[part.mesh.indices[i+1]],c=part.mesh.positions[part.mesh.indices[i+2]];
        if(std::abs(a.z)+std::abs(b.z)+std::abs(c.z)<1e-6)
            CHECK(!pointInTriangle({0,p.floor+1},{a.x,a.y},{b.x,b.y},{c.x,c.y}));
    }
}
TEST(Retail, large_rooms_distribute_shared_furniture_across_the_floor) {
    auto p=room();p.ring={{-35,0},{35,0},{35,65},{-35,65}};
    const auto layout=layoutInterior(p);
    CHECK(layout.fixtures.size()<=130);
    std::set<std::string> prototypes;double farthest=0;bool left=false,right=false;
    for(const auto& f:layout.fixtures) {
        prototypes.insert(interiorFixtureKey(f));farthest=std::max(farthest,f.at.y);
        left=left||f.at.x<-20;right=right||f.at.x>20;
    }
    CHECK(prototypes.size()<=5);CHECK(farthest>55);CHECK(left&&right);
    for(double v=-4;v<64;v+=.1)CHECK(!furnitureOccupies(layout,{0,v}));
}
TEST(Retail, a_parking_tile_can_identify_a_store_owned_by_another_tile) {
    const auto tile=tileAt(2.35,48.85);const auto c=tile.center();
    const auto a=Anchor::at(c.x,c.y,0);
    ElevationGrid elevation{tile.bounds(),2,{10,10,10,10}};OsmData osm;
    // No local InteriorPlan: only the neighbouring building observation.
    osm.buildings.push_back(geoWay(42,{{-15,0},{15,0},{15,25},{-15,25}},
        {{"building","retail"},{"name","Neighbour market"}},a));
    osm.landcover.push_back(geoWay(10,{{-25,-36},{25,-36},{25,-2},{-25,-2}},{{"amenity","parking"}},a));
    auto result=buildRetailParking(osm,{}, {},elevation,a);
    CHECK(result.manifest.size()==1);CHECK(result.manifest[0]["storeId"]==42);
    CHECK(result.manifest[0]["storeName"]=="Neighbour market");
    CHECK(result.manifest[0]["spaces"].get<int>()>0);CHECK(result.parts.size()==2);
}
TEST(Retail, a_supermarket_mapped_in_a_mall_gets_aisles_and_checkouts_behind_the_gallery) {
    auto p=room();p.recipe="mall";p.name="Le Fourchene";p.ring={{-40,0},{40,0},{40,90},{-40,90}};
    p.anchor=P2{0,65};p.anchorName="Carrefour";
    const auto q=InteriorPlan::read(p.json());CHECK(q.anchor.has_value());CHECK(q.anchorName=="Carrefour");
    const auto layout=layoutInterior(q);
    size_t shelves=0,kiosks=0,tills=0;std::set<std::string> prototypes;
    for(const auto& f:layout.fixtures) {
        prototypes.insert(interiorFixtureKey(f));
        const bool store=p.inAnchor(p.point(f.at.x,f.at.y));
        if(f.kind=="shelf"){++shelves;CHECK(store);}
        if(f.kind=="kiosk"){++kiosks;CHECK(!store);}
        if(f.kind=="checkout"){++tills;CHECK(store);CHECK(f.at.y<45);}
    }
    // Gondolas are three modules of one prototype: the floor fills, the
    // arena does not grow.
    CHECK(shelves>=150);CHECK(shelves%3==0);CHECK(kiosks>0);CHECK(tills>=4);CHECK(tills<=12);
    CHECK(prototypes.size()<=9);
    size_t vertices=0;for(auto& part:buildInteriorShell(p))vertices+=part.mesh.vertexCount();
    std::set<std::string> built;
    for(const auto& f:layout.fixtures)if(built.insert(interiorFixtureKey(f)).second)
        for(auto& part:buildInteriorFixture(f))vertices+=part.mesh.vertexCount();
    CHECK(vertices<24000);
    for(double v=-4;v<88;v+=.1)CHECK(!furnitureOccupies(layout,{0,v}));
    CHECK(p.place(p.point(0,80))=="Carrefour \xC2\xB7 Le Fourchene");CHECK(p.place(p.point(0,5))=="Le Fourchene");
    // Without a mapped supermarket a mall stays a gallery.
    p.anchor.reset();for(const auto& f:layoutInterior(p).fixtures)CHECK(f.kind!="shelf");
}
