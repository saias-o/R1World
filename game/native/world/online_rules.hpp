#pragma once
// Meeting the other players where you stand: the rules of online play, kept
// out of the frame so the tests can hold them. The session draws and sends
// (online.cpp); the engine finds and links the peers (plugins/p2p) and draws
// each avatar from its owner's snapshots (plugins/netcode Replica.hpp).
//
// There is no server. Each player is the authority on his own avatar and
// sends it twenty times a second; everyone else draws it a little in the
// past. A shot, a punch or a rocket is judged by the one it is aimed at,
// against where he really was at the instant the shooter saw (his history,
// his clock): the shooter's screen is believed within a tolerance, and
// nobody can be hurt by a claim his own history contradicts. Cars, animals
// and the weather are each machine's own and are never sent.
#include "../combat.hpp"
#include "geography.hpp"
#include "saida/netcode/Replica.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace r1::online {

// Hashed into every handshake and every meeting place, and never changed:
// every R1World finds and links every other, whatever its version, so a
// player of another version is said ("on another version"), never silently
// missing (CLAUDE.md §3).
constexpr const char* kApplication = "r1world/online";
// What this build speaks, shown to every peer once linked (p2p
// HostConfig::hello). Change it with the messages' format: two builds that
// differ stay linked, ignore each other's messages and send none.
constexpr const char* kProtocol = "r1world/online-3";
// The protocol a peer's hello names, printable, or "" when it names none.
std::string protocolOf(const std::vector<uint8_t>& hello);
// Meeting cells: rows of 0.02 degrees (2.2 km), each cut into as many columns
// as keep them about square. A player joins every cell within kMeetRadius of
// him, so two players that near always share one; he leaves a cell once it is
// farther than kLeaveRadius, so walking along a border does not churn.
constexpr double kCellDegrees = .02;
constexpr double kMeetRadius = 700, kLeaveRadius = 900;
constexpr double kSendSeconds = .05;     // twenty snapshots a second
constexpr double kAwaySeconds = 3;       // an avatar silent this long is hidden
constexpr double kHistorySeconds = 1.2;  // how old a claim can be and still be judged
constexpr size_t kMaxPlayers = 16;       // drawn in full at once (arms, aim, punches); the nearest win
constexpr size_t kMaxDrawn = 48;         // drawn at all: the rest as the crowd is, posed at a lower rate
// Linked at once: the nearest, wherever the cells' borders fall (plugins/p2p
// proximity: links kept to the nearest, newcomers found through them). A
// crowd of any size costs each player this many links, not one per player.
constexpr size_t kNeighbours = 32;
constexpr size_t kMaxTracked = 96;       // followed at once: every linked player
constexpr double kBodyRadius = .35;      // the hit volume around a body's axis, a little generous

struct Cell {
    int row = 0, column = 0;
    std::string name() const;
    bool operator<(const Cell& o) const { return row != o.row ? row < o.row : column < o.column; }
    bool operator==(const Cell& o) const { return row == o.row && column == o.column; }
};
Cell cellAt(double lon, double lat);
// Every cell touching the square of `radius` metres around the point.
std::vector<Cell> cellsWithin(double lon, double lat, double radius);

enum class Pose : uint8_t { Stand, Swim, Air, Down, Dead, Vehicle, Count };
enum class Motion : uint8_t { Idle, Run, Sprint, Jump, Count };

// What a player tells the others about his body.
struct Avatar {
    double lon = 0, lat = 0;
    float alt = 0;                     // the body's root
    float heading = 0;                 // where the body faces, degrees clockwise from north
    float aimYaw = 0, aimPitch = 0;    // where he looks, degrees
    float east = 0, north = 0, up = 0; // m/s
    Pose pose = Pose::Stand;
    Motion motion = Motion::Idle;
    combat::Weapon weapon = combat::Weapon::Fists;
    bool aiming = false;
    bool busy = false;                 // in a menu: out of the fight, and seen to be
    bool invincible = false;           // the PAINKILLER code: hit, never hurt, and seen to be
    uint8_t health = 100;
    uint8_t life = 0;  // counts respawns and teleports: a new life is a jump, never a glide
};
// A snapshot on the wire is 32 bytes: the clock in milliseconds, the place in
// 1e-7 degree (1.1 cm) and centimetres of altitude, angles in 16 bits,
// velocity in cm/s, the pose, motion, weapon and flags packed in two bytes.
constexpr size_t kStateBytes = 32;
// The blend the Replica draws with; t > 1 carries the motion on.
Avatar lerp(const Avatar& a, const Avatar& b, double t);
std::array<double, 3> feetEcef(const Avatar& a);

// How often he hears of us: what a player looks at deserves every snapshot,
// the rest of the crowd much less (Donnybrook's attention sets). A player
// near us, facing us, aiming at us or in a fight with us gets twenty a
// second; one within a few hundred metres five; anyone farther one. The
// receiver's Replica follows the rate it is sent at.
enum class Tier : uint8_t { Near, Middle, Far, Count };
constexpr double kTierSeconds[] = {kSendSeconds, .2, 1.};
constexpr size_t kMaxNear = 12;            // at most this many at twenty a second; the nearest win
constexpr double kNearMetres = 40, kViewMetres = 150, kMiddleMetres = 400, kContactSeconds = 5;
// `him` as we draw him, `me` as we are, `sinceContact` the seconds since
// either of us last named the other in a shot, punch or hit, `rank` his
// place among the players nearest us (0: the nearest).
Tier tierFor(const Avatar& me, const Avatar& him, double sinceContact, size_t rank);

enum class Kind : uint8_t { State = 1, Shot, Punch, Rocket, Hit, Count };
// A pistol round: from the muzzle along `direction` (Earth-centred, unit) for
// `length` metres, and the player it hit on the shooter's screen, if any, as
// he drew him at `seen` -- the victim's own clock (Replica::playback).
struct Shot {
    std::array<double, 3> muzzle{};
    std::array<float, 3> direction{};
    float length = 0;
    uint64_t victim = 0;
    double seen = 0;
};
struct Punch {
    uint64_t victim = 0;
    double seen = 0;
    bool hook = false;
};
// Every machine flies its own copy; each player judges his own blast.
struct Rocket {
    std::array<double, 3> muzzle{}, target{};
};
// The victim tells everyone near what a claim did: the shooter, and the
// witnesses who saw the shot (Witness).
struct Hit {
    uint64_t victim = 0;
    uint64_t shooter = 0;
    float damage = 0;
    bool killed = false;
};
struct Message {
    Kind kind = Kind::State;
    double sent = 0;  // the sender's clock
    Avatar avatar;
    Shot shot;
    Punch punch;
    Rocket rocket;
    Hit hit;
};
std::vector<uint8_t> encode(const Message& m);
// False for anything malformed, truncated, unknown or not finite.
bool decode(const uint8_t* data, size_t size, Message& out);

// The victim's judgment of a claim: damage, or why none.
struct Verdict {
    double damage = 0;
    bool head = false;
    const char* refusal = nullptr;
};
// `shooter` is where the shooter's own newest snapshot puts him, `then` where
// the victim was at the claim's instant, `since` the seconds since the last
// claim of that kind from that player that was accepted.
Verdict judgeShot(const Shot& shot, const Avatar& shooter, const Avatar& then, double height, double since);
Verdict judgePunch(const Punch& punch, const Avatar& attacker, const Avatar& then, double since);
// A rocket's launch: from where the shooter is, not faster than he can reload.
const char* judgeRocket(const Rocket& rocket, const Avatar& shooter, double since);

// ── Witnesses ─────────────────────────────────────────────────────────────
// No server can stop a player who rewrites his own game: he owns his avatar
// and judges the blows aimed at him. But every machine near him sees what he
// claims, in the same world (I3: the same ground, the same buildings), and
// can check it: a body faster than any runs, a new life every few seconds, a
// fight from a menu, a hit his own snapshots show and he refuses, a hit he
// accepts and loses no health to. Each strike adds to his suspicion, which
// fades with time; a suspected player is no longer drawn here and his blows
// are refused here. Nothing is shared or voted: each machine believes only
// what it saw, so no crowd of forged accounts can accuse an honest player.
constexpr double kSuspectAt = 3, kForgiveBelow = 1.5, kForgivePerSecond = .1;
constexpr double kFootTopSpeed = 7, kSwimTopSpeed = 2.2;  // the player's sprint and stroke (world/player.cpp)
constexpr double kAnswerSeconds = 2, kHealthSeconds = 2.5;
constexpr size_t kLivesPerMinute = 6;

class Witness {
public:
    // Every snapshot of every player we follow, by his clock; `now` is ours.
    void snapshot(uint64_t id, double sent, const Avatar& a, double now);
    // A shot one followed player claims on another (neither of them us).
    void shot(uint64_t shooter, const Shot& s, double sent, double height, double now);
    // A victim's word on a claim (his Hit, sent to everyone near).
    void verdict(uint64_t victim, const Hit& h, double now);
    // Answers overdue, health that did not fall: judged here.
    void tick(double now);
    void forget(uint64_t id);

    double suspicion(uint64_t id, double now) const;
    // Suspected from kSuspectAt until his suspicion fades below kForgiveBelow.
    bool suspected(uint64_t id) const;
    std::string reason(uint64_t id) const;
    struct Strike {
        uint64_t id;
        std::string why;
        double suspicion;
        bool suspectedNow;  // this strike made him a suspect
    };
    std::vector<Strike> takeStrikes();

private:
    struct Conduct {
        saida::netcode::History<Avatar> seen{kHistorySeconds + 1};
        bool known = false;
        Avatar last;
        double lastSent = 0, gap = 1;  // his clock: newest snapshot, and the one before it this far
        bool anchored = false;  // the start of the window speeds are measured over
        Avatar anchor;
        double anchorSent = 0;
        std::deque<double> lives;  // our clock, the last minute's new lives
        double lastShot = -1e9;   // his clock, his last shot claim
        double suspicion = 0, at = 0;
        bool suspected = false;
        std::string reason;
    };
    struct Claim {
        uint64_t shooter, victim;
        double at, damage;
        uint8_t health, life;
        bool answered = false, expectHealth = false;
        double answeredAt = 0;
        uint8_t below = 0;  // the health he should be at or under
    };
    void strike(uint64_t id, Conduct& c, double weight, const std::string& why, double now);
    std::map<uint64_t, Conduct> conduct_;
    std::vector<Claim> claims_;
    std::vector<Strike> strikes_;
};

// "Player 3F2A": the first four hex digits of the identity.
std::string playerName(uint64_t id);

// The online panel's line. `near` players drawn, `foreign` linked players on
// another version (never drawn), `population` the crowd's size as the
// engine's sketch estimates it, us included (0: not known), `problem` why
// nobody can be found, if anything stops it.
std::string panelLine(size_t near, size_t foreign, double population, bool ready, const std::string& problem);

} // namespace r1::online
