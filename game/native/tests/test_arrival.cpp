#include "check.hpp"
#include "arrival.hpp"
#include "combat.hpp"

#include <algorithm>

using namespace r1::arrival;

namespace {
constexpr double kEarth = 6371008.8;
constexpr double kStep = 1. / 30.;  // the reference machine's frame
// The globe at the whole-Earth zoom (GlobeView::altitude at zoom 1).
constexpr double kWholeEarth = 2.2;
// The camera's altitude above the ground when the world takes over.
const double kHandover = (fallHeight(kZoomSeconds) + kCameraAbove) / kEarth;

double metresBetween(GlobePose a, GlobePose b) {
    const double p1 = a.lat * kRadians, p2 = b.lat * kRadians, dl = (b.lon - a.lon) * kRadians;
    const double c = std::sin(p1) * std::sin(p2) + std::cos(p1) * std::cos(p2) * std::cos(dl);
    return kEarth * std::acos(std::clamp(c, -1., 1.));
}
}  // namespace

TEST(Arrival, the_canopy_opens_fifty_metres_up_twenty_seconds_after_go) {
    NEAR(fallHeight(kDeploySeconds), kDeployHeight, 1e-9);
    // Before the world shows he waits where the fall begins.
    NEAR(fallHeight(0), fallHeight(kZoomSeconds), 1e-9);
    NEAR(fallHeight(kZoomSeconds), 574., 1e-9);
    // A fall, continuous, at the speed it says, never climbing.
    double previous = fallHeight(kZoomSeconds);
    for (double t = kZoomSeconds + .01; t <= kDeploySeconds + 1e-9; t += .01) {
        const double h = fallHeight(t);
        CHECK(h < previous);
        NEAR((previous - h) / .01, fallSpeed(t - .005), .2);
        previous = h;
    }
    // A belly-to-earth skydiver's speed, slowed by tracking before the pull.
    CHECK(fallSpeed(kZoomSeconds) >= 50 && fallSpeed(kZoomSeconds) <= 60);
    CHECK(fallSpeed(kDeploySeconds) < fallSpeed(kTrackFrom));
}

TEST(Arrival, the_globe_flies_from_where_it_is_to_the_drop_zone) {
    const GlobePose from{2.35, 32., kWholeEarth}, to{139.77, 35.68, kHandover};
    const GlobeFlight flight(from, to, kTerminal, kEarth);
    const auto start = flight.at(0), end = flight.at(kZoomSeconds);
    NEAR(start.lon, from.lon, 1e-9); NEAR(start.lat, from.lat, 1e-9); NEAR(start.altitude, from.altitude, 1e-12);
    NEAR(end.lon, to.lon, 1e-9); NEAR(end.lat, to.lat, 1e-9); NEAR(end.altitude, to.altitude, 1e-15);
    // Smooth: no frame moves the centre further than the view is wide, and
    // the descent never overshoots below the handover altitude.
    GlobePose last = start;
    for (double t = kStep; t <= kZoomSeconds + 1e-9; t += kStep) {
        const auto pose = flight.at(t);
        const double width = 2 * GlobeFlight::kTangent * std::max(pose.altitude, last.altitude) * kEarth;
        CHECK_MSG(metresBetween(last, pose) < width * .35, "t=" << t << " moved " << metresBetween(last, pose) << " m");
        CHECK(pose.altitude >= to.altitude * (1 - 1e-9));
        CHECK(pose.altitude <= GlobeFlight::kHighest);
        last = pose;
    }
}

TEST(Arrival, the_globe_hands_over_to_a_fall_already_under_way) {
    // From the whole Earth, from a city view and from street level close by.
    const GlobePose to{2.3522, 48.8566, kHandover};
    for (const GlobePose from : {GlobePose{-40, 20, kWholeEarth}, GlobePose{2.2, 48.9, 13700. / kEarth},
                                 GlobePose{2.3530, 48.8570, 855. / kEarth}}) {
        const GlobeFlight flight(from, to, kTerminal, kEarth);
        const double h = 1e-3;
        const double rate = (flight.at(kZoomSeconds - h).altitude - flight.at(kZoomSeconds).altitude) * kEarth / h;
        CHECK_MSG(rate > kTerminal * .8 && rate < kTerminal * 1.25,
                  "from altitude " << from.altitude * kEarth << " m the camera arrives at " << rate << " m/s");
    }
}

TEST(Arrival, a_far_place_is_flown_to_over_the_globe_not_through_it) {
    // Paris to Sydney, nearly antipodal: the path climbs out first.
    const GlobePose from{2.35, 48.86, 4000. / kEarth}, to{151.21, -33.87, kHandover};
    const GlobeFlight flight(from, to, kTerminal, kEarth);
    double highest = 0;
    for (double t = 0; t <= kZoomSeconds; t += kStep) highest = std::max(highest, flight.at(t).altitude);
    CHECK(highest > 1.);
    CHECK(flight.arc() > 2.5);
}

TEST(Arrival, the_zoom_waits_for_the_world_and_never_jerks) {
    Clock clock;
    // Ready: the arrival keeps real time.
    for (int i = 0; i < 150; ++i) clock.advance(kStep, true);
    NEAR(clock.seconds, 5., 1e-6);
    // Not ready: it slows into a stop at the end of the zoom, never past it.
    double pace = 1;
    for (int i = 0; i < 600; ++i) {
        const double moved = clock.advance(kStep, false) / kStep;
        CHECK(moved <= pace + 1e-9 || moved - pace < .12);  // slows, never lurches
        CHECK(clock.seconds <= kZoomSeconds);
        pace = moved;
    }
    CHECK(clock.holding());
    CHECK(clock.seconds > kZoomSeconds - .05);
    // Ready again: it picks up smoothly.
    double before = clock.rate;
    for (int i = 0; i < 90; ++i) {
        clock.advance(kStep, true);
        CHECK(clock.rate - before < .15);
        before = clock.rate;
    }
    CHECK(!clock.holding());
    CHECK(clock.seconds > kZoomSeconds);
    CHECK(clock.rate > .99);
    // Past the zoom nothing waits.
    const double t = clock.seconds;
    clock.advance(kStep, false);
    NEAR(clock.seconds - t, kStep, 1e-3);
}

TEST(Arrival, left_alone_he_reaches_the_opening_point_as_the_canopy_opens) {
    // He starts kApproach short of it, as the game drops him.
    Freefall fall;
    double east = 0, north = -kApproach;
    for (double t = kZoomSeconds; t < kDeploySeconds - 1e-9; t += kStep) {
        fall.step(kStep, {}, 0, -east, -north, kDeploySeconds - t);
        east += fall.east * kStep;
        north += fall.north * kStep;
        CHECK(fall.speed() <= Freefall::kMost + 1e-9);
    }
    CHECK(!fall.steered);
    CHECK_MSG(std::hypot(east, north) < 6., "missed by " << std::hypot(east, north) << " m");
    // He faces where he flies.
    Freefall flying;
    flying.east = 0; flying.north = 20;
    NEAR(flying.facing(90), 0, 1e-9);
}

TEST(Arrival, the_keys_steer_the_fall_and_hand_it_over_for_good) {
    Freefall fall;
    // Forward, facing east: he tracks east, at a tracking skydiver's speed.
    for (int i = 0; i < 150; ++i) fall.step(kStep, {1, 0}, 90, 0, 500, 8);
    CHECK(fall.steered);
    CHECK(fall.east > Freefall::kForward * .95);
    NEAR(fall.north, 0, 1e-6);
    // Let go, he falls straight: the autopilot does not take him back.
    for (int i = 0; i < 300; ++i) fall.step(kStep, {}, 90, 0, 500, 8);
    CHECK(fall.speed() < .1);
    NEAR(fall.facing(90), 90, 1e-9);
    // Sideways and backwards are slower than forwards.
    Freefall side, back;
    for (int i = 0; i < 300; ++i) { side.step(kStep, {0, 1}, 0, 0, 0, 5); back.step(kStep, {-1, 0}, 0, 0, 0, 5); }
    NEAR(side.east, Freefall::kSide, .1);
    NEAR(back.north, -Freefall::kBack, .1);
}

namespace {
struct Landing { double seconds = 0, sink = 0, worstDeceleration = 0, east = 0, north = 0; };
Landing flyCanopy(Canopy& canopy, const Steer& keys, double startEast, double startNorth, double from = kDeployHeight) {
    Landing out;
    double height = from, east = startEast, north = startNorth, wanted = canopy.heading;
    double sink = canopy.sink;
    while (height > 0 && out.seconds < 120) {
        wanted = canopy.step(kStep, keys, wanted, -east, -north, height);
        out.worstDeceleration = std::max(out.worstDeceleration, (sink - canopy.sink) / kStep);
        sink = canopy.sink;
        height -= canopy.sink * kStep;
        east += canopy.east() * kStep;
        north += canopy.north() * kStep;
        out.seconds += kStep;
    }
    out.sink = canopy.sink; out.east = east; out.north = north;
    return out;
}
}  // namespace

TEST(Arrival, the_canopy_opens_fifty_metres_up_and_lands_him_on_his_feet) {
    Canopy canopy;
    canopy.open(0, 20, kOpeningSpeed);
    const auto landing = flyCanopy(canopy, {}, 0, -kGlideLead);
    // Long enough to fly, a landing that does not hurt, an opening that
    // snatches without crushing (under 6 g).
    CHECK_MSG(landing.seconds > 5., "under canopy for " << landing.seconds << " s");
    CHECK_MSG(landing.sink < 2.5, "lands at " << landing.sink << " m/s");
    CHECK(r1::combat::fallDamage(landing.sink) == 0);
    CHECK_MSG(landing.worstDeceleration < 6 * 9.81, "opening at " << landing.worstDeceleration / 9.81 << " g");
    // Left alone, the canopy lands him on the ground he chose.
    CHECK_MSG(std::hypot(landing.east, landing.north) < 15., "landed " << std::hypot(landing.east, landing.north) << " m away");
    CHECK(!canopy.steered);
    NEAR(canopy.inflation(), 1, 1e-9);
}

TEST(Arrival, the_player_flies_the_canopy) {
    // Toggles turn it, risers dive it, brakes slow it.
    Canopy right, risers, brakes;
    for (auto* c : {&right, &risers, &brakes}) c->open(0, 9, Canopy::kSink);
    double wanted = 0;
    for (int i = 0; i < 90; ++i) {
        wanted = right.step(kStep, {0, 1}, wanted, 0, 0, 40);
        risers.step(kStep, {1, 0}, 0, 0, 0, 40);
        brakes.step(kStep, {-1, 0}, 0, 0, 0, 40);
    }
    CHECK(right.steered);
    CHECK(right.heading > 45 && right.heading < 180);
    CHECK(risers.forward > Canopy::kForward + 2 && risers.sink > Canopy::kSink + 1);
    CHECK(brakes.forward < Canopy::kForward - 3 && brakes.sink < Canopy::kSink - 1);
    // Where he looks it turns, at a wing's rate.
    Canopy look;
    look.open(0, 9, Canopy::kSink);
    for (int i = 0; i < 60; ++i) look.step(kStep, {1e-2, 0}, 90, 0, 0, 40);
    CHECK(look.heading > 30 && look.heading <= 90 + 1e-9);
    // Steered, he still flares near the ground: a step, never a fall.
    Canopy steered;
    steered.open(0, 20, kOpeningSpeed);
    const auto landing = flyCanopy(steered, {1, 0}, 0, 0);
    CHECK(r1::combat::fallDamage(landing.sink) == 0);
}

TEST(Arrival, opened_early_over_high_ground_the_canopy_still_bites) {
    // Over a hill the fall reaches the opening height before twenty seconds,
    // still at terminal speed: it opens higher, and lands him as gently.
    NEAR(openingHeight(kOpeningSpeed), kDeployHeight, 1e-12);
    CHECK(openingHeight(kTerminal) > kDeployHeight + 20);
    Canopy canopy;
    canopy.open(0, 20, kTerminal);
    const auto landing = flyCanopy(canopy, {}, 0, -kGlideLead, openingHeight(kTerminal));
    CHECK_MSG(landing.sink < 2.5, "lands at " << landing.sink << " m/s");
    CHECK(r1::combat::fallDamage(landing.sink) == 0);
    CHECK_MSG(landing.worstDeceleration < 6 * 9.81, "opening at " << landing.worstDeceleration / 9.81 << " g");
}
