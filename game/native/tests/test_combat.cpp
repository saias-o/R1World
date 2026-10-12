// The rules of a fight (native/combat.hpp): health, the pistol's magazine and
// reload, the rhythm of a punch combo, and how hard a car or a fall hits.
#include "check.hpp"

#include "combat.hpp"

using namespace r1::combat;

TEST(Combat, health_counts_down_to_the_one_blow_that_kills) {
    Health h;
    CHECK(!h.take(60));
    CHECK(!h.dead());
    CHECK(h.take(60));
    CHECK(h.dead());
    CHECK(h.hp == 0);
    CHECK(!h.take(10));  // the dead are not killed twice
    h.heal(50);
    CHECK(h.hp == 0);    // nor healed
    h.restore();
    CHECK(h.hp == h.max && !h.dead());
}

TEST(Combat, a_pistol_empties_its_magazine_clicks_and_reloads_from_the_reserve) {
    Arsenal a;
    CHECK(a.held() == Weapon::Fists);
    a.select(Weapon::Pistol);
    a.update(1);
    const int magazine = kPistol.magazine, reserve = a.reserve();
    int fired = 0;
    for (int i = 0; i < 400 && fired < magazine; ++i) {
        if (a.pull() == Arsenal::Trigger::Fired) ++fired;
        a.update(kPistol.interval / 4);
        if (a.reloading()) break;
    }
    CHECK(fired == magazine);
    CHECK(a.loaded() == 0);
    // The last round started the reload on its own; the trigger waits for it.
    CHECK(a.reloading());
    CHECK(a.pull() == Arsenal::Trigger::Reloading);
    a.update(kPistol.reloadSeconds + 0.01);
    CHECK(!a.reloading());
    CHECK(a.loaded() == magazine);
    CHECK(a.reserve() == reserve - magazine);
}

TEST(Combat, the_trigger_respects_the_weapon_rhythm) {
    Arsenal a;
    a.select(Weapon::Pistol);
    a.update(1);
    CHECK(a.pull() == Arsenal::Trigger::Fired);
    CHECK(a.pull() == Arsenal::Trigger::None);  // within the interval
    a.update(kPistol.interval);
    CHECK(a.pull() == Arsenal::Trigger::Fired);
}

TEST(Combat, an_empty_pistol_with_no_reserve_only_clicks) {
    Arsenal a;
    a.select(Weapon::Pistol);
    a.update(1);
    // Spend every round.
    for (int i = 0; i < 10000; ++i) {
        a.pull();
        a.update(kPistol.reloadSeconds);
        if (a.loaded() == 0 && a.reserve() == 0 && !a.reloading()) break;
    }
    CHECK(a.loaded() == 0 && a.reserve() == 0);
    CHECK(a.pull() == Arsenal::Trigger::Empty);
    CHECK(!a.reloading());
    a.give(30);
    CHECK(a.reload());
}

TEST(Combat, punches_alternate_hands_and_the_third_is_a_hook) {
    Punch p;
    double now = 0;
    CHECK(p.throwAt(now));
    CHECK(!p.throwAt(now + 0.05));  // one punch at a time
    bool landed = false;
    for (int i = 0; i < 40; ++i) { landed = p.update(0.01) || landed; now += 0.01; }
    CHECK(landed);
    CHECK(!p.active());
    const bool first = p.rightHand();
    CHECK(p.throwAt(now));
    CHECK(p.rightHand() != first);
    CHECK(!p.hook());
    for (int i = 0; i < 40; ++i) { p.update(0.01); now += 0.01; }
    CHECK(p.throwAt(now));
    CHECK(p.hook());
    CHECK(p.damage() > kFists.damage);
    // A punch thrown long after starts the combo again.
    for (int i = 0; i < 40; ++i) p.update(0.01);
    now += 5;
    CHECK(p.throwAt(now));
    CHECK(p.combo() == 1);
}

TEST(Combat, a_punch_lands_once_at_full_reach) {
    Punch p;
    CHECK(p.throwAt(0));
    int landings = 0;
    double reachAtLanding = 0;
    for (int i = 0; i < 50; ++i) {
        if (p.update(0.01)) { ++landings; reachAtLanding = p.extension(); }
    }
    CHECK(landings == 1);
    NEAR(reachAtLanding, 1.0, 1e-9);
    CHECK(p.extension() == 0);
}

TEST(Combat, a_car_nudges_at_walking_pace_floors_at_town_speed_and_kills_fast) {
    CHECK(!carImpact(1.5).knockDown);
    const CarHit town = carImpact(30 / 3.6);
    CHECK(town.knockDown);
    CHECK(town.damage > 20 && town.damage < 100);
    CHECK(carImpact(60 / 3.6).damage >= 100);
    CHECK(carImpact(-20).damage >= 100);  // closing speed either way
    // More speed never hurts less.
    for (double v = 2.5; v < 20; v += 0.5) CHECK(carImpact(v + 0.5).damage >= carImpact(v).damage);
}

TEST(Combat, a_short_drop_is_free_a_long_one_kills) {
    CHECK(fallDamage(6.3) == 0);   // two metres
    CHECK(fallDamage(10) > 0 && fallDamage(10) < 100);
    CHECK(fallDamage(13) == 100);  // about nine metres
}
