#include "check.hpp"
#include "gen/roadside.hpp"

using namespace r1;
namespace {
Ring rectangle(double left,double bottom,double right,double top) {
    return {{left,bottom},{right,bottom},{right,top},{left,top}};
}
}
TEST(Roadside,wide_carriageway_uses_paved_edge_in_both_driving_sides) {
    RoadSurface surface{{rectangle(-12,-50,12,50)}};
    const auto right=surface.roadside({2,0},{1,0},.45,40);
    const auto left=surface.roadside({-2,0},{-1,0},.45,40);
    CHECK(right&&left);
    NEAR(right->x,12.45,1e-6);NEAR(left->x,-12.45,1e-6);
    CHECK(!surface.contains(*right)&&!surface.contains(*left));
}
TEST(Roadside,junction_mouth_and_adjacent_road_are_not_a_mounting_surface) {
    RoadSurface surface{{rectangle(-4,-50,4,50),rectangle(8,-50,16,50)}};
    const auto post=surface.roadside({2,0},{1,0},.45,40);
    CHECK(post);NEAR(post->x,4.45,1e-6);
    // One connected cross-shaped outline, as the street generator emits it.
    surface.contours={{{-4,-30},{4,-30},{4,-5},{20,-5},{20,5},{4,5},{4,30},{-4,30},{-4,5},{-20,5},{-20,-5},{-4,-5}}};
    const auto mouth=surface.roadside({2,0},{1,0},.45,40);
    CHECK(mouth);NEAR(mouth->x,20.45,1e-6);CHECK(!surface.contains(*mouth));
}
TEST(Roadside,diagonal_kerb_keeps_perpendicular_clearance) {
    RoadSurface surface{{{{-10,-10},{10,-10},{20,10},{-10,10}}}};
    const auto post=surface.roadside({0,0},{1,0},.45,40);
    CHECK(post);NEAR(post->x,15+.45*std::sqrt(1.25),1e-6);
}
TEST(Roadside,median_island_and_missing_edge_are_handled) {
    RoadSurface surface{{rectangle(-12,-50,12,50),rectangle(-1,-40,1,40)}};
    const auto island=surface.roadside({-4,0},{1,0},.45,40);
    CHECK(island);NEAR(island->x,-.55,1e-6);CHECK(!surface.contains(*island));
    CHECK(!surface.roadside({-4,0},{-1,0},.45,2));
    CHECK(!RoadSurface{}.roadside({0,0},{1,0},.45,40));
}
