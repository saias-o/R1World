// Console-only: the rules of online play, without an engine, a window or a socket.
#include "world/online_rules.hpp"
#include "saida/netcode/Replica.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

namespace o = r1::online;
namespace w = r1::world;

static void cells() {
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> lon(-180, 180), lat(-89.9, 89.9), angle(0, 6.283185307179586),
        reach(0, o::kMeetRadius);
    size_t most = 0;
    for (int i = 0; i < 20000; ++i) {
        const double x = lon(rng), y = lat(rng);
        const auto mine = o::cellsWithin(x, y, o::kMeetRadius);
        most = std::max(most, mine.size());
        assert(std::count(mine.begin(), mine.end(), o::cellAt(x, y)) == 1);
        // Anyone within the meeting radius shares a cell with him.
        const double a = angle(rng), d = reach(rng);
        const auto there = w::advance(x, y, d * std::sin(a), d * std::cos(a));
        const auto theirs = o::cellsWithin(there.x, there.y, o::kMeetRadius);
        bool shared = false;
        for (const auto& c : mine) shared |= std::count(theirs.begin(), theirs.end(), c) > 0;
        assert(shared);
    }
    // The poles and the date line are cells like any other.
    for (double y : {-90., 90.}) assert(o::cellsWithin(0, y, o::kMeetRadius).size() >= 1);
    assert(o::cellAt(180, 10) == o::cellAt(-180, 10));
    assert(o::cellAt(2.3522, 48.8566).name() == o::cellAt(2.3523, 48.8567).name());
    std::printf("cells: at most %zu joined at once\n", most);
    assert(most <= 6);
}

static o::Avatar walker(double t) {
    o::Avatar a;
    const auto p = w::advance(2.3522, 48.8566, 3.5 * t, 0);
    a.lon = p.x, a.lat = p.y, a.alt = 35;
    a.heading = 90, a.east = 3.5, a.motion = o::Motion::Run;
    return a;
}

static void codec() {
    o::Message m;
    m.kind = o::Kind::State;
    m.sent = 12.5;
    m.avatar = walker(1);
    m.avatar.weapon = r1::combat::Weapon::Pistol, m.avatar.aiming = true, m.avatar.life = 3;
    auto bytes = o::encode(m);
    o::Message back;
    assert(o::decode(bytes.data(), bytes.size(), back));
    assert(back.avatar.lon == m.avatar.lon && back.avatar.aiming && back.avatar.life == 3 && back.sent == 12.5);
    std::printf("state: %zu bytes\n", bytes.size());
    // Truncated, padded, or out of range: refused.
    assert(!o::decode(bytes.data(), bytes.size() - 1, back));
    bytes.push_back(0);
    assert(!o::decode(bytes.data(), bytes.size(), back));
    m.avatar.lat = std::nan("");
    bytes = o::encode(m);
    assert(!o::decode(bytes.data(), bytes.size(), back));
    std::mt19937_64 rng(11);
    size_t accepted = 0;
    for (int i = 0; i < 100000; ++i) {
        std::vector<uint8_t> noise(rng() % 90);
        for (auto& b : noise) b = uint8_t(rng());
        if (!noise.empty()) noise[0] = uint8_t(1 + rng() % 5);
        accepted += o::decode(noise.data(), noise.size(), back);
    }
    assert(accepted < 10);  // only by chance: every refusal is a check, not a crash
    for (o::Kind k : {o::Kind::Shot, o::Kind::Punch, o::Kind::Rocket, o::Kind::Hit}) {
        o::Message e;
        e.kind = k;
        e.shot.direction = {1, 0, 0}, e.shot.length = 10, e.shot.muzzle = o::feetEcef(walker(0));
        e.rocket.muzzle = e.rocket.target = e.shot.muzzle;
        e.punch.victim = 9;
        const auto b = o::encode(e);
        assert(o::decode(b.data(), b.size(), back) && back.kind == k);
    }
}

// The whole claim: the victim runs and sends; the shooter draws him through a
// Replica on a jittery 80 ms line and fires at the body he draws; the victim
// judges on his own history. Hits where he was are believed; a claim a step
// off is refused.
static void claims() {
    saida::netcode::Replica<o::Avatar> drawn;
    saida::netcode::History<o::Avatar> history(o::kHistorySeconds);
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> jitter(0, .03);
    struct Flight { double arrive, sent; o::Avatar a; };
    std::vector<Flight> air;
    const double victimClock = 500.;  // his clock, unrelated to the shooter's
    int accepted = 0, refusedOff = 0, judged = 0;
    double nextSend = 0;
    const double height = 1.55;
    for (int frame = 0; frame < 600; ++frame) {
        const double now = frame / 60.;
        const o::Avatar me = walker(now);
        history.record(now + victimClock, me);
        if (now >= nextSend) nextSend += o::kSendSeconds, air.push_back({now + .08 + jitter(rng), now + victimClock, me});
        std::sort(air.begin(), air.end(), [](auto& a, auto& b) { return a.arrive < b.arrive; });
        while (!air.empty() && air.front().arrive <= now) drawn.push(air.front().sent, now, air.front().a), air.erase(air.begin());
        o::Avatar seen;
        if (drawn.sample(now, seen, o::lerp) == saida::netcode::ReplicaSample::Empty || now < 2 || frame % 15) continue;
        // The shooter stands 20 m south of the drawn body and fires at its chest.
        o::Avatar shooter;
        const auto p = w::advance(seen.lon, seen.lat, 0, -20);
        shooter.lon = p.x, shooter.lat = p.y, shooter.alt = seen.alt;
        const glm::dvec3 muzzle = w::ecef(shooter.lon, shooter.lat, shooter.alt + 1.4);
        for (double miss : {0., 2.}) {
            const auto aim = w::advance(seen.lon, seen.lat, miss, 0);
            const glm::dvec3 chest = w::ecef(aim.x, aim.y, seen.alt + 1.1);
            const glm::dvec3 d = chest - muzzle;
            o::Shot shot;
            shot.muzzle = {muzzle.x, muzzle.y, muzzle.z};
            const glm::dvec3 u = glm::normalize(d);
            shot.direction = {float(u.x), float(u.y), float(u.z)};
            shot.length = float(glm::length(d));
            shot.seen = drawn.playback();
            o::Avatar then;
            assert(history.at(shot.seen, then, o::lerp));
            const auto verdict = o::judgeShot(shot, shooter, then, height, 1.);
            ++judged;
            if (miss == 0) accepted += verdict.refusal == nullptr && verdict.damage == r1::combat::kPistol.damage;
            else refusedOff += verdict.refusal != nullptr;
        }
    }
    std::printf("claims: %d judged, %d true hits believed, %d claims a step off refused\n", judged, accepted, refusedOff);
    assert(accepted == judged / 2 && refusedOff == judged / 2);

    // A shooter faster than his pistol, or far from where he says, is refused.
    o::Shot shot;
    const o::Avatar victim = walker(0);
    const glm::dvec3 muzzle = w::ecef(victim.lon, victim.lat, victim.alt + 1.2) + glm::dvec3(0, 0, 0);
    shot.muzzle = {muzzle.x, muzzle.y, muzzle.z};
    shot.direction = {1, 0, 0}, shot.length = .01f;
    assert(o::judgeShot(shot, victim, victim, height, .01).refusal);
    o::Avatar far = victim;
    far.lat += .01;
    assert(o::judgeShot(shot, far, victim, height, 1).refusal);
    o::Avatar riding = victim;
    riding.pose = o::Pose::Vehicle;
    assert(o::judgeShot(shot, victim, riding, height, 1).refusal);
    // Fists reach a step, not across the street.
    o::Punch punch;
    punch.victim = 1;
    o::Avatar near = victim;
    const auto step = w::advance(victim.lon, victim.lat, 0, .9);
    near.lon = step.x, near.lat = step.y;
    assert(!o::judgePunch(punch, near, victim, 1).refusal);
    const auto street = w::advance(victim.lon, victim.lat, 0, 8);
    near.lon = street.x, near.lat = street.y;
    assert(o::judgePunch(punch, near, victim, 1).refusal);
    // A rocket from where he stands, not faster than he reloads.
    o::Rocket rocket;
    rocket.muzzle = o::feetEcef(victim), rocket.target = o::feetEcef(walker(30));
    assert(!o::judgeRocket(rocket, victim, 2));
    assert(o::judgeRocket(rocket, victim, .2));
    assert(o::judgeRocket(rocket, far, 2));
}

int main() {
    cells();
    codec();
    claims();
    assert(o::playerName(0x3f2a000000000000ull) == "Player 3F2A");
    std::printf("PASS online rules\n");
    return 0;
}
