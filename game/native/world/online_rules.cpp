#include "online_rules.hpp"
#include "saida/netcode/Wire.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

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

std::array<double, 3> feetEcef(const Avatar& a) {
    const auto p = world::ecef(a.lon, a.lat, a.alt);
    return {p.x, p.y, p.z};
}

std::vector<uint8_t> encode(const Message& m) {
    std::vector<uint8_t> out;
    Writer w(out);
    w.u8(uint8_t(m.kind));
    w.f64(m.sent);
    switch (m.kind) {
    case Kind::State: {
        const Avatar& a = m.avatar;
        w.f64(a.lon), w.f64(a.lat), w.f32(a.alt), w.f32(a.heading), w.f32(a.aimYaw), w.f32(a.aimPitch);
        w.f32(a.east), w.f32(a.north), w.f32(a.up);
        w.u8(uint8_t(a.pose)), w.u8(uint8_t(a.motion)), w.u8(uint8_t(a.weapon)), w.u8(a.aiming ? 1 : 0);
        w.u8(a.health), w.u8(a.life);
        break;
    }
    case Kind::Shot:
        writePoint(w, m.shot.muzzle);
        for (float v : m.shot.direction) w.f32(v);
        w.f32(m.shot.length), w.u64(m.shot.victim), w.f64(m.shot.seen);
        break;
    case Kind::Punch: w.u64(m.punch.victim), w.f64(m.punch.seen), w.u8(m.punch.hook ? 1 : 0); break;
    case Kind::Rocket: writePoint(w, m.rocket.muzzle), writePoint(w, m.rocket.target); break;
    case Kind::Hit: w.u64(m.hit.victim), w.f32(m.hit.damage), w.u8(m.hit.killed ? 1 : 0); break;
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
    m.sent = r.f64();
    bool sane = finite(m.sent);
    switch (m.kind) {
    case Kind::State: {
        Avatar& a = m.avatar;
        a.lon = r.f64(), a.lat = r.f64(), a.alt = r.f32(), a.heading = r.f32(), a.aimYaw = r.f32(), a.aimPitch = r.f32();
        a.east = r.f32(), a.north = r.f32(), a.up = r.f32();
        const uint8_t pose = r.u8(), motion = r.u8(), weapon = r.u8(), flags = r.u8();
        a.health = r.u8(), a.life = r.u8();
        sane = sane && finite(a.lon) && finite(a.lat) && std::abs(a.lon) <= 180 && std::abs(a.lat) <= 90 &&
               finite(a.alt) && a.alt > -1000 && a.alt < 20000 && finite(a.heading) && finite(a.aimYaw) &&
               finite(a.aimPitch) && std::abs(a.aimPitch) <= 90 && pose < uint8_t(Pose::Count) &&
               motion < uint8_t(Motion::Count) && weapon <= uint8_t(combat::Weapon::RocketLauncher) && flags <= 1 &&
               a.health <= 100;
        sane = sane && finite(a.east) && finite(a.north) && finite(a.up) && speedOf(a) < 400;
        a.pose = Pose(pose), a.motion = Motion(motion), a.weapon = combat::Weapon(weapon), a.aiming = flags == 1;
        break;
    }
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
        m.hit.victim = r.u64(), m.hit.damage = r.f32();
        const uint8_t killed = r.u8();
        m.hit.killed = killed == 1;
        sane = sane && finite(m.hit.damage) && m.hit.damage >= 0 && m.hit.damage <= 1000 && killed <= 1;
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

std::string playerName(uint64_t id) {
    char text[16];
    std::snprintf(text, sizeof text, "Player %04X", unsigned(id >> 48));
    return text;
}

} // namespace r1::online
