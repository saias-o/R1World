#include "check.hpp"
#include "rocket.hpp"
#include "combat.hpp"
#include <limits>
using namespace r1;

TEST(Rocket, independent_ammunition_reload_cancel_and_confiscation) {
    combat::Arsenal a;a.select(combat::Weapon::RocketLauncher);a.update(.3);
    CHECK(a.loaded()==1&&a.reserve()==12);
    CHECK(a.pull()==combat::Arsenal::Trigger::Fired);CHECK(a.loaded()==0&&a.reloading());
    a.update(1);a.select(combat::Weapon::Pistol);
    CHECK(a.loaded()==17&&a.reserve()==68&&!a.reloading());
    a.update(3);CHECK(a.loaded()==17);
    a.select(combat::Weapon::RocketLauncher);CHECK(a.loaded()==0&&a.reserve()==12);
    CHECK(a.reload());a.update(2.39);CHECK(a.loaded()==0);a.update(.02);
    CHECK(a.loaded()==1&&a.reserve()==11);
    a.disarm();a.select(combat::Weapon::RocketLauncher);a.update(3);
    CHECK(a.loaded()==0&&a.reserve()==0&&!a.reload());
    CHECK(a.pull()==combat::Arsenal::Trigger::Empty);
    a.select(combat::Weapon::Pistol);CHECK(a.loaded()==0&&a.reserve()==0);
}
TEST(Rocket, blast_falloff_has_a_bounded_radius) {
    NEAR(rocket::damage(0),180,1e-6);NEAR(rocket::damage(7),90,1e-6);
    CHECK(rocket::damage(14)==0&&rocket::damage(100)==0);
    CHECK(rocket::damage(std::numeric_limits<double>::quiet_NaN())==0);
}
TEST(Rocket, police_needs_vehicle_distance_and_live_sight) {
    CHECK(!rocket::authorized(59,true,true,70,false));
    CHECK(!rocket::authorized(100,false,true,70,true));
    CHECK(!rocket::authorized(100,true,false,70,true));
    CHECK(!rocket::authorized(100,true,true,20,true));
    CHECK(!rocket::authorized(100,true,true,150,false));
    CHECK(rocket::authorized(100,true,true,150,true));
    CHECK(rocket::authorized(60,true,true,60,false));
}
TEST(Rocket, losing_cover_resets_lock_and_cooldown_survives_reacquisition) {
    rocket::FireControl fire;fire.update(3,true);CHECK(fire.ready());
    fire.update(.1,false);CHECK(!fire.ready());fire.update(1,true);CHECK(!fire.ready());
    fire.update(.5,true);CHECK(fire.ready());fire.fired(true);CHECK(!fire.ready());
    fire.update(5,true);CHECK(!fire.ready());fire.update(.5,true);CHECK(fire.ready());
    fire.fired(false);fire.update(5.5,true);CHECK(!fire.ready());fire.update(1.5,true);CHECK(fire.ready());
}
TEST(Rocket, intercept_leads_moving_targets_and_rejects_impossible_chases) {
    NEAR(rocket::intercept(85*85,0,0),1,1e-6);
    const double t=rocket::intercept(50*50,50*20,20*20);
    NEAR(50+20*t,rocket::kSpeed*t,1e-6);
    NEAR(rocket::intercept(50*50,50*100,100*100),0,1e-6);
    const double side=rocket::intercept(60*60,0,30*30);
    NEAR(std::sqrt(60*60+30*30*side*side),rocket::kSpeed*side,1e-6);
}
