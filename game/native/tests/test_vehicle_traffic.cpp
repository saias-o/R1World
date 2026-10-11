#include "check.hpp"
#include "gen/scatter.hpp"
#include "gen/osm.hpp"

using namespace r1;
namespace {
const GroundAt ground=[](double lon,double lat){return P3{lon*111320,0,-lat*111320};};
OsmWay road(int64_t id,Tags tags) {
    OsmWay out;out.id=id;out.points={{0,0},{.001,0}};out.tags=std::move(tags);return out;
}
}
TEST(VehicleTraffic,directional_lanes_have_separate_centres) {
    const auto graph=buildLaneGraph({road(10,{{"highway","primary"},{"lanes","4"},{"lanes:forward","3"},{"lanes:backward","1"}})},ground,0,2,48);
    CHECK(graph["lanes"].size()==4);
    CHECK(graph["lanes"][0][0]==graph["lanes"][2][0]);
    CHECK(graph["lanes"][3][0]!=graph["lanes"][0][0]);
    CHECK(graph["lanes"][0][4]<graph["lanes"][1][4]);
    CHECK(graph["lanes"][1][4]<graph["lanes"][2][4]);
    CHECK(graph["lanes"][0][6]==10);
}
TEST(VehicleTraffic,one_way_lanes_are_centred_on_the_paved_profile) {
    const auto graph=buildLaneGraph({road(10,{{"highway","primary"},{"lanes","2"},{"oneway","yes"}})},ground,0,2,48);
    CHECK(graph["lanes"].size()==2);
    NEAR(graph["lanes"][0][4].get<double>()+graph["lanes"][1][4].get<double>(),0,1e-6);
}
TEST(VehicleTraffic,layer_separates_coincident_roads) {
    const auto graph=buildLaneGraph({road(10,{{"highway","primary"},{"layer","0"}}),road(11,{{"highway","primary"},{"layer","1"},{"bridge","yes"}})},ground,0,2,48);
    CHECK(graph["nodes"].size()==4);
    CHECK(graph["nodeKeys"][0]!=graph["nodeKeys"][2]);
}
TEST(VehicleTraffic,surveyed_signal_and_turn_restriction_reach_the_manifest) {
    const OsmNode signal{25,.001,0,{{"highway","traffic_signals"}}};
    const OsmTurnRestriction restriction{10,11,22,{.001,0},false};
    const auto graph=buildLaneGraph({road(10,{{"highway","primary"}})},ground,0,2,48,{signal},{restriction});
    CHECK(graph["signals"].size()==1);
    CHECK(graph["controls"].size()==1);
    CHECK(graph["controls"][0]["osm"]==25);
    CHECK(graph["lanes"][0][9]==0);
    CHECK(graph["restrictions"].size()==1);
    CHECK(graph["restrictions"][0][0]==10);
}
TEST(VehicleTraffic,turn_lane_tags_keep_their_movement_mask) {
    const auto graph=buildLaneGraph({road(10,{{"highway","primary"},{"lanes","2"},{"oneway","yes"},{"turn:lanes","left|through;right"}})},ground,0,2,48);
    CHECK(graph["lanes"][0][13]==2);
    CHECK(graph["lanes"][1][13]==5);
}
TEST(VehicleTraffic,left_driving_preserves_osm_strip_numbering) {
    const auto graph=buildLaneGraph({road(10,{{"highway","primary"},{"lanes","4"}})},ground,0,151,-33);
    CHECK(graph["leftHand"]==true);
    CHECK(graph["lanes"][0][4]>graph["lanes"][1][4]);
    CHECK(graph["lanes"][2][4]>graph["lanes"][3][4]);
    CHECK(graph["lanes"][0][7]==0);
    CHECK(graph["lanes"][1][7]==1);
}
TEST(VehicleTraffic,short_connectors_do_not_disconnect_the_road) {
    auto way=road(10,{{"highway","primary"}});
    way.points={{0,0},{.0004,0},{.00041,0},{.001,0}};
    const auto graph=buildLaneGraph({way},ground,0,2,48);
    CHECK(graph["lanes"].size()==6);
    CHECK(graph["lanes"][0][1]==graph["lanes"][2][0]);
    CHECK(graph["lanes"][2][1]==graph["lanes"][4][0]);
}
