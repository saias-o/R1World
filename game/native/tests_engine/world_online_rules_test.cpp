// Console-only: the rules of online play, without an engine, a window or a socket.
#include "world/online_rules.hpp"
#include "world/parking_rules.hpp"
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
    m.avatar.alt = 35.237f, m.avatar.heading = -179.99f, m.avatar.aimYaw = 179.99f, m.avatar.aimPitch = -42.3f;
    m.avatar.up = -1.234f, m.avatar.health = 57, m.avatar.busy = true, m.avatar.pose = o::Pose::Swim;
    auto bytes = o::encode(m);
    o::Message back;
    assert(bytes.size() == o::kStateBytes);
    assert(o::decode(bytes.data(), bytes.size(), back));
    // Every field within its step: a centimetre of place, a hundredth of a
    // degree of angle, a centimetre a second of speed.
    const o::Avatar& a = m.avatar;
    const o::Avatar& b = back.avatar;
    assert(w::metresBetween(a.lon, a.lat, b.lon, b.lat) < .012 && std::abs(a.alt - b.alt) <= .006);
    auto turn = [](double x, double y) { return std::abs(w::wrap(x - y)); };
    assert(turn(a.heading, b.heading) < .01 && turn(a.aimYaw, b.aimYaw) < .01 && std::abs(a.aimPitch - b.aimPitch) < .01);
    assert(std::abs(a.east - b.east) <= .006 && std::abs(a.up - b.up) <= .006 && b.north == 0);
    assert(b.aiming && b.busy && b.life == 3 && b.health == 57 && b.pose == o::Pose::Swim &&
           b.weapon == r1::combat::Weapon::Pistol && b.motion == o::Motion::Run && back.sent == 12.5);
    // The far side of the date line and the poles survive the rounding.
    for (double x : {-180., 179.9999999, 0.}) {
        o::Message e = m;
        e.avatar.lon = x, e.avatar.lat = x == 0 ? -90 : 89.9999999;
        const auto c = o::encode(e);
        assert(o::decode(c.data(), c.size(), back) && w::metresBetween(x, e.avatar.lat, back.avatar.lon, back.avatar.lat) < .02);
    }
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
    for (o::Kind k : {o::Kind::Shot, o::Kind::Punch, o::Kind::Rocket, o::Kind::Hit, o::Kind::Welcome, o::Kind::Vehicle}) {
        o::Message e;
        e.kind = k;
        e.shot.direction = {1, 0, 0}, e.shot.length = 10, e.shot.muzzle = o::feetEcef(walker(0));
        e.rocket.muzzle = e.rocket.target = e.shot.muzzle;
        e.punch.victim = 9;
        e.vehicle.car = 77, e.vehicle.state.lon = 2.35, e.vehicle.state.lat = 48.85;
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

// Attention: who hears of us twenty times a second, five, or once.
static void tiers() {
    o::Avatar me = walker(0);
    auto at = [&](double east, double north, double facing) {
        o::Avatar him = me;
        const auto p = w::advance(me.lon, me.lat, east, north);
        him.lon = p.x, him.lat = p.y, him.aimYaw = float(facing), him.heading = float(facing);
        return him;
    };
    using T = o::Tier;
    assert(o::tierFor(me, at(0, 30, 0), 99, 20) == T::Near);       // beside us
    assert(o::tierFor(me, at(0, 100, 0), 99, 20) == T::Middle);    // 100 m north, looking away
    assert(o::tierFor(me, at(0, 100, 180), 99, 20) == T::Near);    // ... looking at us
    assert(o::tierFor(me, at(0, 100, 0), 99, 3) == T::Near);       // among the nearest few
    assert(o::tierFor(me, at(0, 100, 0), 2, 20) == T::Near);       // in a fight with us
    o::Avatar sniper = at(300, 0, -90);                            // 300 m east, looking west
    assert(o::tierFor(me, sniper, 99, 20) == T::Middle);           // beyond his eyes' reach
    sniper.aiming = true, sniper.weapon = r1::combat::Weapon::Pistol;
    assert(o::tierFor(me, sniper, 99, 20) == T::Near);             // on his sights
    sniper.aimYaw = -60;
    assert(o::tierFor(me, sniper, 99, 20) == T::Middle);           // aiming elsewhere
    assert(o::tierFor(me, at(0, -1500, 0), 99, 20) == T::Far);
    // The bandwidth this buys, per player, in a crowd of a hundred spread
    // over the cell: at most kMaxNear at 20 Hz, the rest at 5 or 1.
    const double bytes = o::kStateBytes + 4 + 25 + 28;  // message frame, link, UDP/IPv4
    const double all = 99 * bytes / o::kSendSeconds;
    const double tiered = (o::kMaxNear * bytes / o::kTierSeconds[0]) + 40 * bytes / o::kTierSeconds[1] +
                          47 * bytes / o::kTierSeconds[2];
    std::printf("tiers: 99 others cost %.0f KB/s up at 20 Hz each, %.0f KB/s by attention\n", all / 1000, tiered / 1000);
}

// Witnesses: an honest player is never suspected; a speed hack, a stream of
// teleports, a refused or ignored hit and a hit that costs no health are.
static o::Avatar standing(double east, double north) {
    o::Avatar a;
    const auto p = w::advance(2.3522, 48.8566, east, north);
    a.lon = p.x, a.lat = p.y, a.alt = 35;
    return a;
}

static void witnesses() {
    // An honest sprinter who respawns now and then: never a suspect.
    {
        o::Witness wit;
        double t = 0;
        for (int i = 0; i < 1200; ++i) {
            t = i * .05;
            o::Avatar a = standing(7 * t, 0);
            a.east = 7, a.motion = o::Motion::Sprint, a.life = uint8_t(i / 400);
            wit.snapshot(1, 100 + t, a, t);
            wit.tick(t);
        }
        assert(!wit.suspected(1) && wit.suspicion(1, t) == 0);
    }
    // A speed hack at 20 m/s: suspected within three seconds.
    {
        o::Witness wit;
        double when = -1;
        for (int i = 0; i < 200 && when < 0; ++i) {
            const double t = i * .05;
            wit.snapshot(1, t, standing(20 * t, 0), t);
            if (wit.suspected(1)) when = t;
        }
        std::printf("speed hack: suspected after %.2f s: %s\n", when, wit.reason(1).c_str());
        assert(when > 0 && when < 3 && wit.reason(1).find("runs at 20") != std::string::npos);
        // Forgiven once he behaves, kSuspectAt / kForgivePerSecond later.
        wit.tick(when + 60);
        assert(!wit.suspected(1));
    }
    // A new life every four seconds: a teleport cheat.
    {
        o::Witness wit;
        for (int i = 0; i < 1200; ++i) {
            o::Avatar a = standing(0, 0);
            a.life = uint8_t(i / 80);
            wit.snapshot(1, i * .05, a, i * .05);
        }
        assert(wit.suspected(1) && wit.reason(1) == "a new life every few seconds");
    }
    // Shots: the shooter 20 m south of the victim, both standing still.
    auto scene = [](o::Witness& wit, bool invincible, double upTo) {
        for (int i = 0; i * .05 <= upTo; ++i) {
            const double t = i * .05;
            o::Avatar v = standing(0, 0);
            v.invincible = invincible;
            wit.snapshot(2, 500 + t, v, t);
            wit.snapshot(3, 900 + t, standing(0, -20), t);
        }
    };
    auto shotAt = [](double miss, double seen) {
        const o::Avatar v = standing(miss, 0);
        const glm::dvec3 muzzle = w::ecef(standing(0, -20).lon, standing(0, -20).lat, 35 + 1.4);
        const glm::dvec3 chest = w::ecef(v.lon, v.lat, 35 + 1.0), d = chest - muzzle;
        o::Shot s;
        s.muzzle = {muzzle.x, muzzle.y, muzzle.z};
        const glm::dvec3 u = glm::normalize(d);
        s.direction = {float(u.x), float(u.y), float(u.z)};
        s.length = float(glm::length(d));
        s.victim = 2, s.seen = seen;
        return s;
    };
    auto hit = [](float damage) {
        o::Hit h;
        h.victim = 2, h.shooter = 3, h.damage = damage;
        return h;
    };
    const double height = 1.55;
    {
        // Honest: the hit is accepted and his health falls.
        o::Witness wit;
        scene(wit, false, 2);
        wit.shot(3, shotAt(0, 501.9), 901.95, height, 2);
        wit.verdict(2, hit(34), 2.1);
        o::Avatar hurt = standing(0, 0);
        hurt.health = 66;
        wit.snapshot(2, 502.15, hurt, 2.15);
        wit.tick(5);
        assert(wit.suspicion(2, 5) == 0);
        // A claim a step off, refused: his word against a miss, no strike.
        wit.shot(3, shotAt(2, 502.1), 903, height, 5);
        wit.verdict(2, hit(0), 5.1);
        wit.tick(8);
        assert(wit.suspicion(2, 8) == 0);
    }
    {
        // A hit his own snapshots show, refused twice: a suspect.
        o::Witness wit;
        scene(wit, false, 2);
        wit.shot(3, shotAt(0, 501.9), 901.95, height, 2);
        wit.verdict(2, hit(0), 2.1);
        assert(!wit.suspected(2) && wit.suspicion(2, 2.1) > 1);  // one may be a mistake
        wit.shot(3, shotAt(0, 501.95), 902.7, height, 2.1);
        wit.verdict(2, hit(0), 2.2);
        assert(wit.suspected(2) && wit.reason(2) == "refuses a hit his own snapshots show");
        assert(wit.suspicion(3, 2.2) == 0);  // the shooter told the truth
    }
    {
        // Accepted, and no health lost; or never answered.
        o::Witness wit;
        scene(wit, false, 2);
        wit.shot(3, shotAt(0, 501.9), 901.95, height, 2);
        wit.verdict(2, hit(34), 2.1);
        wit.tick(4);
        assert(wit.suspicion(2, 4) == 0);  // a moment to show it
        wit.tick(5);
        assert(wit.reason(2) == "takes hits and loses no health");
        o::Witness quiet;
        scene(quiet, false, 2);
        quiet.shot(3, shotAt(0, 501.9), 901.95, height, 2);
        quiet.tick(4.5);
        assert(quiet.reason(2) == "ignores a hit his own snapshots show");
    }
    {
        // Invincible by the game's own code, and seen to be: no strike.
        o::Witness wit;
        scene(wit, true, 2);
        wit.shot(3, shotAt(0, 501.9), 901.95, height, 2);
        wit.verdict(2, hit(34), 2.1);
        wit.tick(6);
        assert(wit.suspicion(2, 6) == 0);
    }
    {
        // Shooting from a menu, or faster than a pistol fires.
        o::Witness wit;
        o::Avatar busy = standing(0, -20);
        busy.busy = true;
        wit.snapshot(3, 900, busy, 0);
        wit.shot(3, shotAt(0, 500), 900.1, height, .1);
        wit.shot(3, shotAt(0, 500), 900.15, height, .15);
        assert(wit.suspicion(3, .15) >= 1.5);
    }
    std::printf("witnesses: honest players never suspected; speed, teleports, refused, ignored and painless hits are\n");
}

// Another version is found and said; the panel says who is here and how many.
static void versions() {
    const std::string ours = o::kProtocol;
    assert(o::protocolOf(std::vector<uint8_t>(ours.begin(), ours.end())) == ours);
    assert(o::protocolOf({}).empty());                    // a build before hellos
    assert(o::protocolOf({'r', '1', 0x07, 'x'}).empty());  // not printable: names nothing
    assert(o::protocolOf(std::vector<uint8_t>(120, 'a')).size() == 48);
    assert(std::string(o::kApplication).find("online-") == std::string::npos);  // the meeting never moves with the version

    assert(o::panelLine(0, 0, 0, false, "") == "Online · looking for players…");
    assert(o::panelLine(0, 0, 1, true, "") == "Online · nobody else here yet");
    assert(o::panelLine(0, 0, 0, false, "UDP is blocked") == "Online: UDP is blocked");
    assert(o::panelLine(1, 0, 2, true, "UDP is blocked") == "Online · 1 player nearby");
    assert(o::panelLine(2, 0, 3, true, "") == "Online · 2 players nearby");
    // The sketch counts us: 150 is 149 others, 16 of them drawn.
    assert(o::panelLine(16, 0, 150.4, true, "") == "Online · 16 players nearby · about 149 players in this area");
    assert(o::panelLine(0, 0, 40, true, "") == "Online · nobody in sight · about 39 players in this area");
    assert(o::panelLine(0, 1, 2, true, "") == "Online · nobody on this version · 1 player on another version");
    assert(o::panelLine(3, 2, 6, true, "") == "Online · 3 players nearby · 2 players on another version");
    assert(o::panelLine(0, 1, 0, false, "UDP is blocked") == "Online · nobody on this version · 1 player on another version");
    std::printf("versions: %s\n", o::panelLine(16, 1, 150, true, "").c_str());
}

// Joining a friend: where he lands, when he may go, what he is told.
static void joining() {
    o::Avatar him;
    him.lon = 2.3522, him.lat = 48.8566, him.heading = 0;  // facing north: his right is east
    const auto at = o::besideFriend(him);
    const auto east = w::advance(him.lon, him.lat, o::kBesideMetres, 0);
    assert(std::abs(at[0] - east.x) < 1e-9 && std::abs(at[1] - east.y) < 1e-9);
    for (float heading : {0.f, 73.f, 181.f, -120.f}) {
        him.heading = heading;
        const auto b = o::besideFriend(him);
        const double d = std::hypot((b[0] - him.lon) * w::kMetresPerDegree * std::cos(him.lat * w::rad),
                                    (b[1] - him.lat) * w::kMetresPerDegree);
        assert(std::abs(d - o::kBesideMetres) < .05);
    }
    assert(!o::notYet(him));
    him.pose = o::Pose::Vehicle;
    assert(!o::notYet(him));  // at the wheel: beside the car
    him.pose = o::Pose::Air;
    assert(o::notYet(him) == std::string("in the air"));
    him.pose = o::Pose::Dead;
    assert(o::notYet(him) == std::string("between lives"));
    assert(o::joinLine(o::Join::Looking, "K7QM-2XPA", "") == "Looking for the player with code K7QM-2XPA…");
    assert(o::joinLine(o::Join::Going, "K7QM-2XPA", "Player 3F2A") == "Going to Player 3F2A");
    assert(o::joinLine(o::Join::Foreign, "K7QM-2XPA", "").find("another version") != std::string::npos);
    // The welcome is the kind alone, with the clock.
    o::Message m, back;
    m.kind = o::Kind::Welcome, m.sent = 12.5;
    const auto b = o::encode(m);
    assert(b.size() == 9 && o::decode(b.data(), b.size(), back) && back.kind == o::Kind::Welcome && back.sent == 12.5);
    assert(!o::decode(b.data(), b.size() - 1, back));
}

// Parked cars: the same places from the same graph, on the kerb of the
// side the country drives on, never on an avenue, a short lane or a bridge.
static void parking() {
    namespace tr = saida::traffic;
    auto street = [](float speed, float length, int16_t layer, bool oneWay) {
        tr::Graph g;
        g.nodes = {{0, 0}, {length, 0}};
        tr::Lane there{0, 1, speed, 1}, back{1, 0, speed, 1};
        there.offset = back.offset = 1.5f, there.width = back.width = 3.f, there.road = back.road = 4242;
        there.layer = back.layer = layer;
        g.lanes.push_back(there);
        if (!oneWay) g.lanes.push_back(back);
        g.build();
        return g;
    };
    for (bool leftHand : {false, true}) {
        const tr::Graph g = street(13.9f, 200, 0, false);
        const auto spots = r1::parking::kerbSpots(g, leftHand, -1.f, "v63_27771_23996");
        assert(spots.size() >= 8 && spots.size() <= 40);  // 28 places a side, a third of them taken
        tr::Flow flow;
        tr::Rules rules;
        rules.leftHand = leftHand, rules.lateralAxisSign = -1.f;
        flow.reset(&g, rules, 1);
        std::set<uint64_t> ids;
        for (const auto& s : spots) {
            ids.insert(s.id);
            // Beside its lane's cars, farther out: 1.5 m for the lane, 1.8 m for the parked car.
            const auto lane = flow.positionOf(s.lane, s.s);
            assert(std::abs(std::abs(s.y) - 1.8f) < 1e-3 && std::abs(lane.y) < std::abs(s.y) && lane.y * s.y > 0);
            assert(s.s >= r1::parking::kEndClearance && s.s <= 200 - r1::parking::kEndClearance);
            const auto dir = g.direction(s.lane);
            assert(std::abs(std::sin(s.yaw) - dir.x) < 1e-4 && std::abs(std::cos(s.yaw) - dir.y) < 1e-4);
            assert(std::string(r1::parking::modelFor(s.seed)) != "bus");
        }
        assert(ids.size() == spots.size());
        const auto again = r1::parking::kerbSpots(g, leftHand, -1.f, "v63_27771_23996");
        assert(again.size() == spots.size() && again.front().id == spots.front().id && again.back().x == spots.back().x);
        assert(r1::parking::kerbSpots(g, leftHand, -1.f, "v63_27771_23997").front().id != spots.front().id);
    }
    assert(r1::parking::kerbSpots(street(25.f, 200, 0, false), false, -1.f, "t").empty());  // an avenue at 90 km/h
    assert(r1::parking::kerbSpots(street(13.9f, 25, 0, false), false, -1.f, "t").empty());  // too short
    assert(r1::parking::kerbSpots(street(13.9f, 200, 1, false), false, -1.f, "t").empty()); // a bridge
    // A street drawn in short pieces (OpenStreetMap bends it at shape
    // points): parked along its whole length, not only on long pieces.
    {
        tr::Graph g;
        for (int i = 0; i <= 10; ++i) g.nodes.push_back({20.f * i, 0});
        for (uint32_t i = 0; i < 10; ++i) {
            tr::Lane there{i, i + 1, 13.9f, 1}, back{i + 1, i, 13.9f, 1};
            there.offset = back.offset = 1.5f, there.width = back.width = 3.f, there.road = back.road = 7;
            g.lanes.push_back(there), g.lanes.push_back(back);
        }
        g.build();
        const auto spots = r1::parking::kerbSpots(g, false, -1.f, "v63_27771_23996");
        size_t middle = 0;
        for (const auto& s : spots) {
            const float x = s.x;
            assert(x >= r1::parking::kEndClearance - 1e-3f && x <= 200 - r1::parking::kEndClearance + 1e-3f);
            middle += x > 40 && x < 160;
        }
        assert(spots.size() >= 8 && middle >= 5);
    }
    const auto oneWay = r1::parking::kerbSpots(street(13.9f, 200, 0, true), false, -1.f, "v63_1_1");
    assert(!oneWay.empty());
    for (const auto& s : oneWay) assert(s.lane == 0);
    // Who keeps a car two players took: the later take, then the lower identity.
    assert(o::outranks(3, 9, 2, 1) && !o::outranks(2, 1, 3, 9) && o::outranks(2, 1, 2, 9) && !o::outranks(2, 9, 2, 1));
    // A car's state on the wire: within a centimetre and a tenth of a degree.
    o::Message m, back;
    m.kind = o::Kind::Vehicle, m.sent = 3.25;
    m.vehicle.car = 0x1234567890abcdefull, m.vehicle.seed = 99, m.vehicle.claim = 4;
    m.vehicle.state = {2.3522, 48.8566, 35.25f, -170.5f, 4.5f, -2.25f, 13.4f, true};
    const auto b = o::encode(m);
    assert(o::decode(b.data(), b.size(), back) && back.vehicle.car == m.vehicle.car && back.vehicle.claim == 4 &&
           back.vehicle.seed == 99 && back.vehicle.state.driven);
    const auto& c = back.vehicle.state;
    assert(std::abs(c.lon - 2.3522) < 1e-6 && std::abs(c.lat - 48.8566) < 1e-6 && std::abs(c.alt - 35.25f) < .01f &&
           std::abs(c.yaw + 170.5f) < .01f && std::abs(c.pitch - 4.5f) < .01f && std::abs(c.roll + 2.25f) < .01f &&
           std::abs(c.speed - 13.4f) < .02f);
    m.vehicle.car = 0;  // a car with no place is nobody's
    const auto none = o::encode(m);
    assert(!o::decode(none.data(), none.size(), back));
    const o::Car a{0, 0, 0, 170, 0, 0, 0, true}, z{0, 0, 0, -170, 0, 0, 10, true};
    assert(std::abs(std::abs(o::lerpCar(a, z, .5).yaw) - 180) < .01f);  // the short way round
    std::printf("parking: %zu places on 200 m of street\n", r1::parking::kerbSpots(street(13.9f, 200, 0, false), false, -1.f, "v63_27771_23996").size());
}

int main() {
    cells();
    codec();
    claims();
    tiers();
    witnesses();
    versions();
    joining();
    parking();
    // Far away, on his feet: the impostor; nearer, down or swimming: the body.
    assert(!o::drawnAsImpostor(false, 59, o::Pose::Stand) && o::drawnAsImpostor(false, 61, o::Pose::Stand));
    assert(o::drawnAsImpostor(true, 55, o::Pose::Stand) && !o::drawnAsImpostor(true, 53, o::Pose::Stand));  // no flicker at the edge
    assert(!o::drawnAsImpostor(false, 500, o::Pose::Down) && !o::drawnAsImpostor(true, 500, o::Pose::Swim) &&
           !o::drawnAsImpostor(false, 500, o::Pose::Air));
    assert(o::kBodyMetres < o::kImpostorMetres);
    assert(o::playerName(0x3f2a000000000000ull) == "Player 3F2A");
    std::printf("PASS online rules\n");
    return 0;
}
