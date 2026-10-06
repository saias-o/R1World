#include "check.hpp"
#include "gen/road_details.hpp"
#include "gen/scatter.hpp"

using namespace r1;

TEST(Roads, theix_single_lane_expressway_ramps_are_carriageways) {
    const Tags tags{{"highway","trunk_link"},{"lanes","1"},{"oneway","yes"}};
    const auto p=roadProfile(tags);
    CHECK(isMotorway("trunk_link") && isMotorway("motorway_link"));
    NEAR(p.travelWidth(),3.5,1e-12);
    NEAR(p.width,5.0,1e-12);
    CHECK(p.lanesTagged && !p.widthTagged && p.direction==1);
    const auto graph=buildLaneGraph({{35018659,{{0,0},{.001,0}},tags}},
        [](double x,double y){return P3{x*111320,0,-y*111320};},0,-2.65,47.63);
    CHECK(graph["lanes"].size()==1);
}
TEST(Roads, countryside_defaults_allow_two_vehicles_to_pass) {
    CHECK(roadWidth({{"highway","unclassified"}})>=6.5);
    CHECK(roadWidth({{"highway","residential"}})>=6.5);
    NEAR(roadWidth({{"highway","unclassified"},{"width","4.2"}}),4.2,1e-12);
}
TEST(Roads, lane_count_and_reverse_direction_define_the_paved_profile) {
    const auto two=roadProfile({{"highway","motorway"},{"lanes","2"}});
    const auto three=roadProfile({{"highway","motorway"},{"lanes","3"}});
    const auto reverse=roadProfile({{"highway","motorway"},{"lanes","2"},{"oneway","-1"}});
    CHECK(two.direction==1 && reverse.direction==-1);
    NEAR(three.width-two.width,3.5,1e-12);
    NEAR(two.leftShoulder,reverse.rightShoulder,1e-12);
    NEAR(two.rightShoulder,reverse.leftShoulder,1e-12);
    const auto tagged=roadProfile({{"highway","motorway"},{"lanes","3"},{"width","9"}});
    NEAR(tagged.width,9,1e-12);
    CHECK(tagged.travelWidth()>0);
}
TEST(Roads, expressway_markings_and_steel_follow_the_raised_profile) {
    const clip::Paths64 tile{clip::kMetres.path({{-100,-100},{100,-100},{100,100},{-100,100}})};
    const RoadAxis road{1,{{"highway","trunk"},{"oneway","yes"},{"lanes","2"},{"r1:raised","yes"}},
                        {{-80,5,0},{80,5,0}},false};
    const auto out=buildRoadDetails({road},tile,{},nullptr,"FR");
    bool paint=false,steel=false;
    for(const auto& p:out.parts) {
        if(p.name=="Road lane and edge markings") {
            paint=true;for(const auto& v:p.mesh.positions)NEAR(v.y,5.063,1e-6);
        }
        if(p.name=="Road guardrails") {
            steel=true;CHECK(p.material.metallic>.9 && p.material.roughness<.4);
            for(const auto& v:p.mesh.positions)CHECK(v.y>=5 && v.y<=5.91);
        }
    }
    CHECK(paint && steel);
    CHECK(out.stats["guardedWaysInferred"]==1);
}
TEST(Roads, mapped_absence_prevents_paint_and_barriers) {
    const clip::Paths64 tile{clip::kMetres.path({{-100,-100},{100,-100},{100,100},{-100,100}})};
    const RoadAxis road{1,{{"highway","trunk"},{"oneway","yes"},{"lanes","2"},
                        {"lane_markings","no"},{"guardrail","no"}},{{-80,0,0},{80,0,0}},false};
    CHECK(buildRoadDetails({road},tile,{},nullptr,"FR").parts.empty());
}
TEST(Roads, an_entry_road_opens_the_guardrail_on_its_own_side) {
    const clip::Paths64 tile{clip::kMetres.path({{-100,-100},{100,-100},{100,100},{-100,100}})};
    const RoadAxis main{1,{{"highway","trunk"},{"oneway","yes"},{"lanes","2"}},
                       {{-80,0,0},{0,0,0},{80,0,0}},false};
    const RoadAxis entry{2,{{"highway","trunk_link"},{"oneway","yes"},{"lanes","1"}},
                        {{0,0,0},{0,0,80}},false};
    const auto out=buildRoadDetails({main},tile,{},nullptr,"FR",{main,entry});
    CHECK(out.stats["guardrailMetres"].get<double>()<320);
    for(const auto& part:out.parts)if(part.name=="Road guardrails") {
        for(size_t i=0;i<part.mesh.indices.size();i+=3) {
            P3 mid{};
            for(int k=0;k<3;++k){const auto p=part.mesh.positions[part.mesh.indices[i+k]];mid.x+=p.x/3;mid.z+=p.z/3;}
            CHECK(!(std::abs(mid.x)<2.5 && mid.z>4 && mid.z<7));
        }
    }
}
