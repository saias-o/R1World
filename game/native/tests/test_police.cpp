#include "check.hpp"
#include "police.hpp"
#include "combat.hpp"

using namespace r1::police;

TEST(Police, observed_crimes_escalate_to_air_support_and_hiding_ends_the_search) {
    Wanted w;
    w.report(Crime::Assault);CHECK(w.percent==14&&!w.lethal());
    w.update(60,true);CHECK(w.percent==14&&!w.searching());
    w.report(Crime::Gunshot);CHECK(w.lethal());
    for(int i=0;i<8;++i)w.report(Crime::Gunshot);
    CHECK(w.percent==100&&w.helicopter&&w.cars()==4);
    w.update(12,false);CHECK(w.percent==100);
    w.update(10,false);NEAR(w.percent,92,1e-9);CHECK(w.searching());
    w.update(1,true);CHECK(w.unseen==0&&w.percent==92);
    w.update(200,false);CHECK(w.percent==0&&!w.violent&&!w.helicopter&&w.cars()==1);
    r1::combat::Arsenal arsenal;arsenal.select(r1::combat::Weapon::Pistol);arsenal.disarm();
    CHECK(arsenal.held()==r1::combat::Weapon::Fists&&arsenal.loaded()==0&&arsenal.reserve()==0);
    arsenal.give(17);arsenal.select(r1::combat::Weapon::Pistol);CHECK(arsenal.reload());
}

TEST(Police, routing_respects_disconnected_floors_and_directed_lanes) {
    Routes routes;
    int a=routes.add({0,0,0}),b=routes.add({10,0,0}),c=routes.add({10,0,10}),bridge=routes.add({10,8,10});
    routes.link(a,b);routes.link(b,c);
    auto path=routes.route({0,0,0},{10,0,10});CHECK(path.size()==3);
    CHECK(routes.route({10,0,10},{0,0,0}).empty());
    CHECK(routes.route({0,0,0},routes.points[size_t(bridge)]).empty());
    CHECK(routes.route({1000,0,0},{0,0,0}).empty());
    path=routes.route({0,0,0},{70,0,30},true);
    CHECK(path.size()==3); // A roadside destination need not sit on a lane.
    CHECK(Routes::distance(path.back(),{10,0,10})<.01f);
    // A suspect far from every road is still approachable: each point knows
    // the closest roadside its directed routes reach.
    auto gaps=routes.approaches({70,0,30});
    NEAR(gaps[size_t(a)],Routes::distance({10,0,10},{70,0,30}),1e-4);
    NEAR(gaps[size_t(c)],gaps[size_t(a)],1e-4);
    gaps=routes.approaches({0,0,0});
    CHECK(gaps[size_t(a)]==0&&gaps[size_t(b)]==10); // b cannot drive back up a.
    NEAR(gaps[size_t(bridge)],Routes::distance({10,8,10},{0,0,0}),1e-4);
    const auto costs=routes.costsTo({c});
    CHECK(costs[size_t(a)]==20&&costs[size_t(b)]==10&&costs[size_t(c)]==0);
    CHECK(std::isinf(costs[size_t(bridge)]));
}
