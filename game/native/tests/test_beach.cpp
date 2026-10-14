#include "check.hpp"
#include "gen/beach.hpp"
#include "gen/terrain.hpp"
#include <limits>
#include <sstream>
using namespace r1;
namespace {
struct Coast {
    Tile tile=tileAt(2.35,48.85);
    Bounds b=tile.bounds();
    P2 centre=tile.center();
    Anchor anchor=Anchor::at(centre.x,centre.y,0);
    OsmData osm;
    Sea sea;
    ElevationGrid grid{b,41,std::vector<double>(41*41,3.)};
    Coast(bool mapped=false) {
        // Northbound coast, land west (engine X negative), sea east.
        osm.coastlines.push_back({1,{{centre.x,b.south-.002},{centre.x,b.north+.002}},{{"natural","coastline"}}});
        sea.region={clip::kDegrees.path({{centre.x,b.south},{b.east,b.south},{b.east,b.north},{centre.x,b.north}})};
        if(mapped)osm.landcover.push_back({2,{{centre.x-.001,b.south},{centre.x,b.south},
            {centre.x,b.north},{centre.x-.001,b.north},{centre.x-.001,b.south}},{{"natural","beach"}}});
    }
};
}
TEST(Beach, every_unmapped_coast_has_sand_and_mapped_beaches_are_wider) {
    Coast small,big(true);BeachPlan a(small.osm,small.anchor,small.b,&small.sea),b(big.osm,big.anchor,big.b,&big.sea);
    CHECK(a.sand({-10,0}));CHECK(!a.sand({-55,0}));CHECK(b.sand({-55,0}));
    CHECK(b.report()["sandAreaM2"].get<double>()>a.report()["sandAreaM2"].get<double>()*1.5);
    CHECK(a.sample({-10,0}).inland>0);CHECK(a.sample({10,0}).inland<0);
}
TEST(Beach, ports_override_even_mapped_sand_and_keep_their_original_ground) {
    for(const Tags& tags:std::vector<Tags>{{{"leisure","marina"}},{{"landuse","port"}},
        {{"water","harbour"}},{{"waterway","dock"}},{{"industrial","shipyard"}}}) {
        Coast c(true);
        c.osm.landcover.push_back({5,{{c.b.west,c.b.south},{c.b.east,c.b.south},
            {c.b.east,c.b.north},{c.b.west,c.b.north},{c.b.west,c.b.south}},tags});
        BeachPlan p(c.osm,c.anchor,c.b,&c.sea);
        CHECK(p.protectedHarbour({0,0}));CHECK(!p.sand({-10,0}));
        CHECK(p.sculpt(c.grid).values==c.grid.values);
        CHECK(p.surfaces(c.grid,{}).empty());CHECK(p.population(c.grid,{}).empty());
        CHECK(p.water(c.grid,{}).is_null());
    }
}
TEST(Beach, quays_piers_and_breakwaters_are_protected_without_erasing_nearby_beaches) {
    for(const auto* kind:{"quay","pier","breakwater"}) {
        Coast c(true);auto a=c.anchor.toGeodetic(0,0,-40),b=c.anchor.toGeodetic(0,0,40);
        c.osm.maritime.push_back({7,{{a.x,a.y},{b.x,b.y}},{{"man_made",kind}}});
        BeachPlan p(c.osm,c.anchor,c.b,&c.sea);
        CHECK(!p.sand({-10,0}));CHECK(p.sand({-10,180}));
        const auto sculpted=p.sculpt(c.grid);
        NEAR(sculpted.sample(c.centre.x,c.centre.y),3,1e-6);
    }
}
TEST(Beach, terrain_and_dunes_are_deterministic_and_sea_bed_is_below_water) {
    Coast c(true);BeachPlan p(c.osm,c.anchor,c.b,&c.sea);
    const auto a=p.sculpt(c.grid),b=p.sculpt(c.grid);CHECK(a.values==b.values);
    auto at=[&](double x){auto q=c.anchor.toGeodetic(x,0,0);return terrainElevation(q.x,q.y,a);};
    CHECK(at(20)<-1);CHECK(at(-40)>3.5);CHECK(at(-10)>0);CHECK(at(-10)<2);
    const auto parts=p.surfaces(a,{});CHECK(!parts.empty());
    size_t vertices=0;for(auto& part:parts)vertices+=part.mesh.vertexCount();CHECK(vertices<14000);
}
TEST(Beach, promenade_and_road_stop_sand_and_dunes_on_the_landward_side) {
    for(const auto* highway:{"footway","path","residential"}) {
        Coast c(true);
        auto a=c.anchor.toGeodetic(-25,0,-1000),b=c.anchor.toGeodetic(-25,0,1000);
        c.osm.roads.push_back({9,{{a.x,a.y},{b.x,b.y}},{{"highway",highway}}});
        BeachPlan p(c.osm,c.anchor,c.b,&c.sea);
        CHECK(p.sand({-10,0}));CHECK(!p.sand({-25,0}));CHECK(!p.sand({-55,0}));
        CHECK(p.behindCoastalRoad({-55,0}));
        auto q=c.anchor.toGeodetic(-55,0,0);
        NEAR(p.sculpt(c.grid).sample(q.x,q.y),3,1e-6);
        for(const auto& spot:p.population(p.sculpt(c.grid),{}))CHECK(spot["towel"][0].get<double>()>-25);
    }
}
TEST(Beach,_perpendicular_access_keeps_the_beach_on_both_sides) {
    Coast c(true);auto a=c.anchor.toGeodetic(-110,0,0),b=c.anchor.toGeodetic(0,0,0);
    c.osm.roads.push_back({10,{{a.x,a.y},{b.x,b.y}},{{"highway","footway"}}});
    BeachPlan p(c.osm,c.anchor,c.b,&c.sea);
    CHECK(!p.sand({-10,0}));CHECK(p.sand({-10,-40}));CHECK(p.sand({-10,40}));
}
TEST(Beach, no_fake_beaches_on_open_ocean_tile_edges) {
    Coast c;c.osm.coastlines.clear();c.sea.region={clip::kDegrees.path({{c.b.west,c.b.south},
        {c.b.east,c.b.south},{c.b.east,c.b.north},{c.b.west,c.b.north}})};
    BeachPlan p(c.osm,c.anchor,c.b,&c.sea);CHECK(p.empty());CHECK(p.population(c.grid,{}).empty());
}
TEST(Beach, surf_has_one_depth_per_vertex_and_both_wet_and_dry_swash) {
    Coast c;BeachPlan p(c.osm,c.anchor,c.b,&c.sea);auto water=p.water(p.sculpt(c.grid),{});
    CHECK(!water.is_null());CHECK(water["shoreMode"]==1);
    std::istringstream coords(water["surface"].get<std::string>()),depths(water["surfaceDepths"].get<std::string>());
    double x;size_t n=0,d=0;bool land=false,sea=false;
    while(coords>>x){CHECK(std::isfinite(x));++n;}
    while(depths>>x){++d;land|=x<0;sea|=x>0;}
    CHECK(n==d*3);CHECK(n%9==0);CHECK(land&&sea);
}
TEST(Beach, population_lives_on_dry_flat_sand_and_returns_deterministically) {
    Coast c(true);BeachPlan p(c.osm,c.anchor,c.b,&c.sea);auto grid=p.sculpt(c.grid);
    auto spots=p.population(grid,{});CHECK(!spots.empty());CHECK(spots==p.population(grid,{}));CHECK(spots.size()<=80);
    for(auto& spot:spots){const auto& a=spot["towel"];CHECK(p.sand({a[0],a[2]}));CHECK(a[1].get<double>()>.25);}
    P3 a{-12,1,0},b{12,-.85,0};bool rest=false,walk=false,swim=false,returned=false;
    for(int i=0;i<4000;++i){auto pose=beachPose(a,b,i*.1,0);rest|=pose.recline==1;walk|=pose.walking;
        swim|=pose.swimming;returned|=swim&&pose.recline==1;
        CHECK(pose.position.x>=-12&&pose.position.x<=12);CHECK(pose.recline>=0&&pose.recline<=1);}
    CHECK(rest&&walk&&swim&&returned);
}
TEST(Beach, cold_rain_night_unknown_weather_and_polar_darkness_remove_everything) {
    WeatherState w;w.known=w.temperatureKnown=true;w.temperature=30;
    CHECK(beachAttendance(13,45,w)>.99);
    for(double hour:{0.,7.,19.,23.})CHECK(beachAttendance(hour,45,w)==0);
    CHECK(beachAttendance(13,-10,w)==0);
    w.temperature=8;w.rain=1;CHECK(beachAttendance(13,45,w)==0);
    w.rain=0;CHECK(beachAttendance(13,45,w)==0);
    w.temperature=30;w.rain=.01;CHECK(beachAttendance(13,45,w)==0);
    w.rain=0;w.temperatureKnown=false;CHECK(beachAttendance(13,45,w)==0);
    w.temperatureKnown=true;w.code=95;CHECK(beachAttendance(13,45,w)==0);
    w.code=0;w.windSpeed=12;CHECK(beachAttendance(13,45,w)==0);
}
TEST(Beach, heat_and_sun_increase_attendance_but_sky_override_preserves_cold) {
    WeatherState w;w.known=w.temperatureKnown=true;w.temperature=20;
    const double cool=beachAttendance(13,45,w);w.temperature=27;CHECK(beachAttendance(13,45,w)>cool);
    const double sunny=beachAttendance(13,45,w);w.cover=.7;CHECK(beachAttendance(13,45,w)<sunny);
    w.temperature=8;const auto clear=resolveWeather(w,true,WeatherOverride::Clear);
    CHECK(clear.temperature==8&&clear.temperatureKnown);CHECK(beachAttendance(13,45,clear)==0);
    CHECK(!resolveWeather(w,false,WeatherOverride::Clear).temperatureKnown);
}
