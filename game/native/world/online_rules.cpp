#include "online_rules.hpp"
#include "saida/netcode/Wire.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <utility>

namespace r1::online {
using saida::netcode::Reader;
using saida::netcode::Writer;
using world::rad;

namespace {
constexpr int kCellRows = int(180. / kCellDegrees + .5);
constexpr double kMetresPerDegree = world::kMetresPerDegree;

int cellColumns(int row) {
    const double middle = -90. + (row + .5) * kCellDegrees;
    return std::max(1, int(std::floor(360. * std::cos(middle * rad) / kCellDegrees)));
}
bool finite(double v) { return std::isfinite(v); }
double distance(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
double speedOf(const Avatar& a) { return std::sqrt(double(a.east) * a.east + double(a.north) * a.north + double(a.up) * a.up); }
// Within 50 km of the Earth's surface: anything else is no point of this game.
bool onEarth(const std::array<double, 3>& p) {
    const double r = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    return finite(r) && r > 6.30e6 && r < 6.43e6;
}
double angleLerp(double a, double b, double t) { return a + world::wrap(b - a) * t; }

void writePoint(Writer& w, const std::array<double, 3>& p) { for (double v : p) w.f64(v); }
// Quantization of a snapshot (kStateBytes): every field rounds to its step
// and saturates at its range instead of wrapping.
template <class T>
T quantize(double v, double step) {
    const double q = std::round(v / step);
    return T(std::clamp(q, double(std::numeric_limits<T>::min()), double(std::numeric_limits<T>::max())));
}
void put24(Writer& w, uint32_t v) { w.u8(uint8_t(v)), w.u8(uint8_t(v >> 8)), w.u8(uint8_t(v >> 16)); }
uint32_t get24(Reader& r) {
    const uint32_t a = r.u8(), b = r.u8(), c = r.u8();
    return a | b << 8 | c << 16;
}
constexpr double kDegreeStep = 1e-7, kAltitudeFloor = -1000, kAltitudeStep = .01, kSpeedStep = .01;
constexpr double kTurn = 65536. / 360.;
std::array<double, 3> readPoint(Reader& r) { return {r.f64(), r.f64(), r.f64()}; }
} // namespace

std::string Cell::name() const { return "cell/" + std::to_string(row) + "/" + std::to_string(column); }

Cell cellAt(double lon, double lat) {
    const int row = std::clamp(int(std::floor((std::clamp(lat, -90., 90.) + 90.) / kCellDegrees)), 0, kCellRows - 1);
    const int n = cellColumns(row);
    return {row, std::clamp(int(std::floor((world::wrap(lon) + 180.) / 360. * n)), 0, n - 1)};
}

std::vector<Cell> cellsWithin(double lon, double lat, double radius) {
    // Samples closer than a cell is wide: a 5 x 5 grid over the square
    // touches every cell the square does (cells are 2.2 km, the square at
    // most 1.8 km across).
    std::set<Cell> found;
    const double cosine = std::max(1e-6, std::cos(std::clamp(lat, -90., 90.) * rad));
    for (int i = -2; i <= 2; ++i)
        for (int j = -2; j <= 2; ++j) {
            const double north = radius * i / 2., east = radius * j / 2.;
            const double la = std::clamp(lat + north / kMetresPerDegree, -90., 90.);
            found.insert(cellAt(lon + east / (kMetresPerDegree * cosine), la));
        }
    return {found.begin(), found.end()};
}

Avatar lerp(const Avatar& a, const Avatar& b, double t) {
    if (a.life != b.life || !(t >= 0)) return b;
    Avatar out = t < .5 ? a : b;
    out.lon = world::wrap(angleLerp(a.lon, b.lon, t));
    out.lat = std::clamp(a.lat + (b.lat - a.lat) * t, -90., 90.);
    out.alt = float(a.alt + (b.alt - a.alt) * t);
    out.heading = float(world::wrap(angleLerp(a.heading, b.heading, std::min(t, 1.))));
    out.aimYaw = float(world::wrap(angleLerp(a.aimYaw, b.aimYaw, std::min(t, 1.))));
    out.aimPitch = float(a.aimPitch + (b.aimPitch - a.aimPitch) * std::min(t, 1.));
    const double v = std::min(t, 1.);
    out.east = float(a.east + (b.east - a.east) * v);
    out.north = float(a.north + (b.north - a.north) * v);
    out.up = float(a.up + (b.up - a.up) * v);
    return out;
}

Car lerpCar(const Car& a, const Car& b, double t) {
    if (!(t >= 0)) return b;
    Car out = b;
    out.lon = world::wrap(angleLerp(a.lon, b.lon, t));
    out.lat = std::clamp(a.lat + (b.lat - a.lat) * t, -90., 90.);
    out.alt = float(a.alt + (b.alt - a.alt) * t);
    const double v = std::min(t, 1.);
    out.yaw = float(world::wrap(angleLerp(a.yaw, b.yaw, v)));
    out.pitch = float(a.pitch + (b.pitch - a.pitch) * v);
    out.roll = float(a.roll + (b.roll - a.roll) * v);
    out.speed = float(a.speed + (b.speed - a.speed) * v);
    return out;
}

bool outranks(uint16_t claim, uint64_t owner, uint16_t otherClaim, uint64_t otherOwner) {
    return claim != otherClaim ? claim > otherClaim : owner <= otherOwner;
}

std::array<double, 3> feetEcef(const Avatar& a) {
    const auto p = world::ecef(a.lon, a.lat, a.alt);
    return {p.x, p.y, p.z};
}

std::vector<uint8_t> encode(const Message& m) {
    std::vector<uint8_t> out;
    Writer w(out);
    w.u8(uint8_t(m.kind));
    if (m.kind == Kind::State) {
        // Twenty a second to everyone near: every byte counts (kStateBytes).
        const Avatar& a = m.avatar;
        w.u32(uint32_t(std::clamp(std::round(m.sent * 1000.), 0., 4294967295.)));
        w.u32(uint32_t(quantize<int32_t>(world::wrap(a.lon), kDegreeStep)));
        w.u32(uint32_t(quantize<int32_t>(std::clamp(a.lat, -90., 90.), kDegreeStep)));
        put24(w, uint32_t(std::clamp(std::round((a.alt - kAltitudeFloor) / kAltitudeStep), 0., 16777215.)));
        w.u16(uint16_t(int(std::round((world::wrap(a.heading) + 180.) * kTurn)) & 0xffff));
        w.u16(uint16_t(int(std::round((world::wrap(a.aimYaw) + 180.) * kTurn)) & 0xffff));
        w.u16(uint16_t(quantize<int16_t>(std::clamp(double(a.aimPitch), -90., 90.), 90. / 32767.)));
        w.u16(uint16_t(quantize<int16_t>(a.east, kSpeedStep)));
        w.u16(uint16_t(quantize<int16_t>(a.north, kSpeedStep)));
        w.u16(uint16_t(quantize<int16_t>(a.up, kSpeedStep)));
        w.u8(uint8_t(uint8_t(a.pose) | uint8_t(a.motion) << 3 | uint8_t(a.weapon) << 5 | (a.aiming ? 0x80 : 0)));
        w.u8(uint8_t((a.busy ? 1 : 0) | (a.invincible ? 2 : 0)));
        w.u8(a.health), w.u8(a.life);
        return out;
    }
    w.f64(m.sent);
    switch (m.kind) {
    case Kind::Shot:
        writePoint(w, m.shot.muzzle);
        for (float v : m.shot.direction) w.f32(v);
        w.f32(m.shot.length), w.u64(m.shot.victim), w.f64(m.shot.seen);
        break;
    case Kind::Punch: w.u64(m.punch.victim), w.f64(m.punch.seen), w.u8(m.punch.hook ? 1 : 0); break;
    case Kind::Rocket: writePoint(w, m.rocket.muzzle), writePoint(w, m.rocket.target); break;
    case Kind::Hit: w.u64(m.hit.victim), w.u64(m.hit.shooter), w.f32(m.hit.damage), w.u8(m.hit.killed ? 1 : 0); break;
    case Kind::Welcome: break;  // the kind says it all
    case Kind::Vehicle: {
        const Vehicle& v = m.vehicle;
        const Car& c = v.state;
        w.u64(v.car), w.u32(v.seed), w.u8(v.model), w.u16(v.claim);
        w.u32(uint32_t(quantize<int32_t>(world::wrap(c.lon), kDegreeStep)));
        w.u32(uint32_t(quantize<int32_t>(std::clamp(c.lat, -90., 90.), kDegreeStep)));
        put24(w, uint32_t(std::clamp(std::round((c.alt - kAltitudeFloor) / kAltitudeStep), 0., 16777215.)));
        w.u16(uint16_t(int(std::round((world::wrap(c.yaw) + 180.) * kTurn)) & 0xffff));
        w.u16(uint16_t(quantize<int16_t>(std::clamp(double(c.pitch), -90., 90.), 90. / 32767.)));
        w.u16(uint16_t(quantize<int16_t>(std::clamp(double(c.roll), -180., 180.), 180. / 32767.)));
        w.u16(uint16_t(quantize<int16_t>(c.speed, .02)));
        w.u8(c.driven ? 1 : 0);
        break;
    }
    default: break;
    }
    return out;
}

bool decode(const uint8_t* data, size_t size, Message& out) {
    if (!data) return false;
    Reader r(data, size);
    Message m;
    const uint8_t kind = r.u8();
    if (kind < uint8_t(Kind::State) || kind >= uint8_t(Kind::Count)) return false;
    m.kind = Kind(kind);
    if (m.kind == Kind::State) {
        if (size != kStateBytes) return false;
        Avatar& a = m.avatar;
        m.sent = r.u32() / 1000.;
        const int32_t lon = int32_t(r.u32()), lat = int32_t(r.u32());
        a.lon = lon * kDegreeStep, a.lat = lat * kDegreeStep;
        a.alt = float(kAltitudeFloor + get24(r) * kAltitudeStep);
        a.heading = float(world::wrap(r.u16() / kTurn - 180.));
        a.aimYaw = float(world::wrap(r.u16() / kTurn - 180.));
        a.aimPitch = float(int16_t(r.u16()) * (90. / 32767.));
        a.east = float(int16_t(r.u16()) * kSpeedStep);
        a.north = float(int16_t(r.u16()) * kSpeedStep);
        a.up = float(int16_t(r.u16()) * kSpeedStep);
        const uint8_t packed = r.u8(), flags = r.u8();
        a.health = r.u8(), a.life = r.u8();
        const uint8_t pose = packed & 7, motion = packed >> 3 & 3, weapon = packed >> 5 & 3;
        if (!r.ok() || !r.done() || std::abs(a.lon) > 180 || std::abs(a.lat) > 90 || a.alt >= 20000 ||
            std::abs(a.aimPitch) > 90 || pose >= uint8_t(Pose::Count) || weapon > uint8_t(combat::Weapon::RocketLauncher) ||
            flags > 3 || a.health > 100)
            return false;
        a.pose = Pose(pose), a.motion = Motion(motion), a.weapon = combat::Weapon(weapon);
        a.aiming = packed & 0x80, a.busy = flags & 1, a.invincible = flags & 2;
        out = m;
        return true;
    }
    m.sent = r.f64();
    bool sane = finite(m.sent);
    switch (m.kind) {
    case Kind::Shot: {
        Shot& s = m.shot;
        s.muzzle = readPoint(r);
        for (float& v : s.direction) v = r.f32();
        s.length = r.f32(), s.victim = r.u64(), s.seen = r.f64();
        const double unit = std::sqrt(double(s.direction[0]) * s.direction[0] + double(s.direction[1]) * s.direction[1] +
                                      double(s.direction[2]) * s.direction[2]);
        sane = sane && onEarth(s.muzzle) && finite(unit) && std::abs(unit - 1) < .02 && finite(s.length) &&
               s.length > 0 && s.length <= 1000 && finite(s.seen);
        break;
    }
    case Kind::Punch: {
        m.punch.victim = r.u64(), m.punch.seen = r.f64();
        const uint8_t hook = r.u8();
        m.punch.hook = hook == 1;
        sane = sane && finite(m.punch.seen) && hook <= 1;  // victim 0: a swing at nobody, shown
        break;
    }
    case Kind::Rocket:
        m.rocket.muzzle = readPoint(r), m.rocket.target = readPoint(r);
        sane = sane && onEarth(m.rocket.muzzle) && onEarth(m.rocket.target);
        break;
    case Kind::Hit: {
        m.hit.victim = r.u64(), m.hit.shooter = r.u64(), m.hit.damage = r.f32();
        const uint8_t killed = r.u8();
        m.hit.killed = killed == 1;
        sane = sane && finite(m.hit.damage) && m.hit.damage >= 0 && m.hit.damage <= 1000 && killed <= 1;
        break;
    }
    case Kind::Welcome: break;
    case Kind::Vehicle: {
        Vehicle& v = m.vehicle;
        Car& c = v.state;
        v.car = r.u64(), v.seed = r.u32(), v.model = r.u8(), v.claim = r.u16();
        const int32_t lon = int32_t(r.u32()), lat = int32_t(r.u32());
        c.lon = lon * kDegreeStep, c.lat = lat * kDegreeStep;
        c.alt = float(kAltitudeFloor + get24(r) * kAltitudeStep);
        c.yaw = float(world::wrap(r.u16() / kTurn - 180.));
        c.pitch = float(int16_t(r.u16()) * (90. / 32767.));
        c.roll = float(int16_t(r.u16()) * (180. / 32767.));
        c.speed = float(int16_t(r.u16()) * .02);
        const uint8_t driven = r.u8();
        c.driven = driven == 1;
        sane = sane && v.car != 0 && std::abs(c.lon) <= 180 && std::abs(c.lat) <= 90 && c.alt < 20000 && driven <= 1;
        break;
    }
    default: return false;
    }
    if (!r.ok() || !r.done() || !sane) return false;
    out = m;
    return true;
}

Verdict judgeShot(const Shot& shot, const Avatar& shooter, const Avatar& then, double height, double since) {
    Verdict v;
    const auto& pistol = combat::kPistol;
    if (then.pose == Pose::Vehicle) return v.refusal = "the target was in a vehicle", v;
    if (then.pose == Pose::Dead) return v.refusal = "the target was already dead", v;
    if (since < pistol.interval * .75) return v.refusal = "faster than a pistol fires", v;
    if (shot.length > pistol.range + 1) return v.refusal = "beyond a pistol's range", v;
    const double slack = (shooter.pose == Pose::Vehicle ? 10. : 4.) + speedOf(shooter) * .5;
    if (distance(shot.muzzle, feetEcef(shooter)) > slack) return v.refusal = "fired from where the shooter is not", v;
    const glm::dvec3 hit(shot.muzzle[0] + double(shot.direction[0]) * shot.length,
                         shot.muzzle[1] + double(shot.direction[1]) * shot.length,
                         shot.muzzle[2] + double(shot.direction[2]) * shot.length);
    // The victim's frame at that instant: x east, y up, z south.
    const auto local = world::Frame(then.lon, then.lat, then.alt).local(hit);
    if (std::hypot(local.x, local.z) > kBodyRadius + .6 || local.y < -.4 || local.y > height + .4)
        return v.refusal = "the target was not there", v;
    v.head = local.y > height * .84;
    v.damage = pistol.damage * (v.head ? pistol.headMultiplier : 1.);
    return v;
}

Verdict judgePunch(const Punch& punch, const Avatar& attacker, const Avatar& then, double since) {
    Verdict v;
    if (then.pose != Pose::Stand && then.pose != Pose::Air) return v.refusal = "the target was not on his feet", v;
    if (attacker.pose != Pose::Stand && attacker.pose != Pose::Air) return v.refusal = "the attacker was not on his feet", v;
    if (since < .3) return v.refusal = "faster than fists go", v;
    const glm::dvec3 at = world::ecef(attacker.lon, attacker.lat, attacker.alt);
    const auto local = world::Frame(then.lon, then.lat, then.alt).local(at);
    if (std::hypot(local.x, local.z) > 2.2 || std::abs(local.y) > 1.6) return v.refusal = "out of reach", v;
    v.damage = combat::kFists.damage * (punch.hook ? combat::Punch::kHookMultiplier : 1.);
    return v;
}

const char* judgeRocket(const Rocket& rocket, const Avatar& shooter, double since) {
    if (since < combat::kRocket.interval * .75) return "faster than a launcher reloads";
    const double slack = (shooter.pose == Pose::Vehicle ? 15. : 5.) + speedOf(shooter) * .5;
    if (distance(rocket.muzzle, feetEcef(shooter)) > slack) return "launched from where the shooter is not";
    if (distance(rocket.muzzle, rocket.target) > combat::kRocket.range + 5) return "aimed beyond a launcher's range";
    return nullptr;
}

Tier tierFor(const Avatar& me, const Avatar& him, double sinceContact, size_t rank) {
    const double d = world::metresBetween(him.lon, him.lat, me.lon, me.lat) + std::abs(double(me.alt) - him.alt) * .5;
    if (d < kNearMetres || rank < 6 || sinceContact < kContactSeconds) return Tier::Near;
    // Where we are, seen from him: in front of his eyes, or on his sights.
    const auto there = world::Frame(him.lon, him.lat, him.alt).local(world::ecef(me.lon, me.lat, me.alt));
    const double bearing = std::atan2(there.x, -there.z) / rad;
    const double off = std::abs(world::wrap(bearing - him.aimYaw));
    if (d < kViewMetres && off < 50) return Tier::Near;
    if (him.aiming && him.weapon != combat::Weapon::Fists && d < combat::kRocket.range &&
        off < 8 + std::atan2(2., d) / rad)
        return Tier::Near;
    return d < kMiddleMetres ? Tier::Middle : Tier::Far;
}

// ── Witnesses ─────────────────────────────────────────────────────────────

void Witness::strike(uint64_t id, Conduct& c, double weight, const std::string& why, double now) {
    c.suspicion = suspicion(id, now) + weight, c.at = now, c.reason = why;
    const bool fresh = !c.suspected && c.suspicion >= kSuspectAt;
    c.suspected = c.suspected || fresh;
    strikes_.push_back({id, why, c.suspicion, fresh});
}

double Witness::suspicion(uint64_t id, double now) const {
    const auto it = conduct_.find(id);
    if (it == conduct_.end()) return 0;
    return std::max(0., it->second.suspicion - (now - it->second.at) * kForgivePerSecond);
}

bool Witness::suspected(uint64_t id) const {
    const auto it = conduct_.find(id);
    return it != conduct_.end() && it->second.suspected;
}

std::string Witness::reason(uint64_t id) const {
    const auto it = conduct_.find(id);
    return it == conduct_.end() ? std::string() : it->second.reason;
}

std::vector<Witness::Strike> Witness::takeStrikes() { return std::exchange(strikes_, {}); }

void Witness::forget(uint64_t id) {
    conduct_.erase(id);
    claims_.erase(std::remove_if(claims_.begin(), claims_.end(), [&](const Claim& c) { return c.shooter == id || c.victim == id; }),
                  claims_.end());
}

void Witness::snapshot(uint64_t id, double sent, const Avatar& a, double now) {
    Conduct& c = conduct_[id];
    if (c.known && sent <= c.lastSent) return;  // late or repeated: the history has it
    c.seen.record(sent, a);
    // A health that fell as a hit said it would.
    for (Claim& k : claims_)
        if (k.victim == id && k.expectHealth && (a.health <= k.below || a.life != k.life)) k.expectHealth = false;
    if (c.known && a.life != c.last.life) {
        c.lives.push_back(now);
        while (!c.lives.empty() && c.lives.front() < now - 60) c.lives.pop_front();
        if (c.lives.size() > kLivesPerMinute) strike(id, c, 1, "a new life every few seconds", now);
        c.anchored = false;
    }
    // Speed over half a second at least, on foot or swimming all along: no
    // body goes faster than the game moves it (a margin for the snapshots).
    const bool measured = a.pose == Pose::Stand || a.pose == Pose::Swim;
    if (!measured || !c.anchored || c.anchor.pose != a.pose || c.anchor.life != a.life) {
        c.anchored = measured, c.anchor = a, c.anchorSent = sent;
    } else if (sent - c.anchorSent >= .5) {
        const double dt = sent - c.anchorSent;
        const double speed = world::metresBetween(c.anchor.lon, c.anchor.lat, a.lon, a.lat) / dt;
        const double top = a.pose == Pose::Swim ? kSwimTopSpeed : kFootTopSpeed;
        if (speed > top * 1.5 + 1.5)
            strike(id, c, 1, (a.pose == Pose::Swim ? "swims at " : "runs at ") + std::to_string(std::lround(speed)) + " m/s", now);
        else if (a.busy && c.anchor.busy && speed > 1.5)
            strike(id, c, .5, "moves while in a menu", now);
        c.anchor = a, c.anchorSent = sent;
    }
    if (c.known) c.gap = sent - c.lastSent;
    c.known = true, c.last = a, c.lastSent = sent;
}

void Witness::shot(uint64_t shooter, const Shot& s, double sent, double height, double now) {
    Conduct& by = conduct_[shooter];
    const bool fromMenu = by.known && by.last.busy;
    if (fromMenu) strike(shooter, by, 1, "shoots from a menu", now);
    const bool tooFast = sent - by.lastShot < combat::kPistol.interval * .75;
    if (tooFast) strike(shooter, by, .5, "fires faster than a pistol", now);
    by.lastShot = sent;
    const auto v = conduct_.find(s.victim);
    if (fromMenu || tooFast || v == conduct_.end() || !v->second.known || !by.known) return;
    Conduct& victim = v->second;
    // Judged as the victim judges it, but tighter: a witness draws him from
    // fewer snapshots, so only a clear hit, at an instant the victim can
    // still judge, seen at five snapshots a second or more, is held against him.
    Avatar then;
    if (victim.gap > .21 || victim.lastSent - s.seen > .5 || !victim.seen.at(s.seen, then, lerp)) return;
    if (then.busy) return;
    const Verdict verdict = judgeShot(s, by.last, then, height, 1e9);
    if (verdict.refusal) return;
    const glm::dvec3 end(s.muzzle[0] + double(s.direction[0]) * s.length, s.muzzle[1] + double(s.direction[1]) * s.length,
                         s.muzzle[2] + double(s.direction[2]) * s.length);
    const auto local = world::Frame(then.lon, then.lat, then.alt).local(end);
    if (std::hypot(local.x, local.z) > kBodyRadius) return;
    if (claims_.size() >= 64) claims_.erase(claims_.begin());
    claims_.push_back({shooter, s.victim, now, verdict.damage, victim.last.health, victim.last.life});
}

void Witness::verdict(uint64_t victim, const Hit& h, double now) {
    for (Claim& k : claims_) {
        if (k.answered || k.victim != victim || k.shooter != h.shooter) continue;
        k.answered = true, k.answeredAt = now;
        Conduct& c = conduct_[victim];
        if (h.damage <= 0) {
            strike(victim, c, 2, "refuses a hit his own snapshots show", now);
        } else if (!(c.known && c.last.invincible) && k.health > 0) {
            // The damage he named should show in his health.
            k.expectHealth = true;
            k.below = uint8_t(std::max(0., std::floor(k.health - h.damage * .9)));
        }
        return;
    }
}

void Witness::tick(double now) {
    for (size_t i = 0; i < claims_.size();) {
        Claim& k = claims_[i];
        Conduct& c = conduct_[k.victim];
        if (!k.answered && now - k.at > kAnswerSeconds) {
            strike(k.victim, c, 2, "ignores a hit his own snapshots show", now);
        } else if (k.answered && k.expectHealth && now - k.answeredAt > kHealthSeconds) {
            if (!(c.known && c.last.invincible)) strike(k.victim, c, 2, "takes hits and loses no health", now);
        } else if (!k.answered || k.expectHealth) {
            ++i;
            continue;
        }
        claims_.erase(claims_.begin() + long(i));
    }
    for (auto& [id, c] : conduct_)
        if (c.suspected && suspicion(id, now) < kForgiveBelow) c.suspected = false;
}

std::string playerName(uint64_t id) {
    char text[16];
    std::snprintf(text, sizeof text, "Player %04X", unsigned(id >> 48));
    return text;
}

std::string protocolOf(const std::vector<uint8_t>& hello) {
    std::string out;
    for (size_t i = 0; i < hello.size() && i < 48; ++i) {
        if (hello[i] < 0x20 || hello[i] > 0x7e) return "";
        out += char(hello[i]);
    }
    return out;
}

std::string panelLine(size_t near, size_t foreign, double population, bool ready, const std::string& problem) {
    if (!problem.empty() && !near && !foreign) return "Online: " + problem;
    auto players = [](size_t n) { return std::to_string(n) + (n == 1 ? " player" : " players"); };
    // The sketch counts us, and everyone of the crowd's cell we do not draw.
    const size_t crowd = population >= 1.5 ? size_t(std::lround(population)) - 1 : 0;
    std::string line = "Online · ";
    if (near) line += players(near) + " nearby";
    else if (foreign) line += "nobody on this version";
    else if (crowd) line += "nobody in sight";
    else line += ready || foreign ? "nobody else here yet" : "looking for players…";
    if (crowd > near + foreign) line += " · about " + players(crowd) + " in this area";
    if (foreign) line += " · " + players(foreign) + " on another version";
    return line;
}

std::string joinLine(Join state, const std::string& code, const std::string& name) {
    switch (state) {
    case Join::Looking: return "Looking for the player with code " + code + "…";
    case Join::Quiet:
        return "Nobody found with code " + code + " yet: check the code, and that your friend is online (still looking)";
    case Join::Foreign: return "The player with code " + code + " is on another version of R1World: you cannot meet";
    case Join::Waiting: return "Found " + name + ": waiting for " + name + " to be on the ground";
    case Join::Going: return "Going to " + name;
    case Join::Joined: return "Beside " + name;
    }
    return "";
}

std::array<double, 2> besideFriend(const Avatar& him) {
    const double right = (him.heading + 90) * rad;
    const auto at = world::advance(him.lon, him.lat, kBesideMetres * std::sin(right), kBesideMetres * std::cos(right));
    return {at.x, at.y};
}

bool drawnAsImpostor(bool now, double metres, Pose pose) {
    if (pose != Pose::Stand) return false;
    return metres > (now ? kBodyMetres : kImpostorMetres);
}

const char* notYet(const Avatar& him) {
    if (him.pose == Pose::Air) return "in the air";
    if (him.pose == Pose::Dead) return "between lives";
    return nullptr;
}

} // namespace r1::online
