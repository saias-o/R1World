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
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace r1::online {

// Hashed into every handshake: another protocol version never connects.
constexpr const char* kApplication = "r1world/online-1";
// Meeting cells: rows of 0.02 degrees (2.2 km), each cut into as many columns
// as keep them about square. A player joins every cell within kMeetRadius of
// him, so two players that near always share one; he leaves a cell once it is
// farther than kLeaveRadius, so walking along a border does not churn.
constexpr double kCellDegrees = .02;
constexpr double kMeetRadius = 700, kLeaveRadius = 900;
constexpr double kSendSeconds = .05;     // twenty snapshots a second
constexpr double kAwaySeconds = 3;       // an avatar silent this long is hidden
constexpr double kHistorySeconds = 1.2;  // how old a claim can be and still be judged
constexpr size_t kMaxPlayers = 16;       // drawn at once; the nearest win
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
    uint8_t health = 100;
    uint8_t life = 0;  // counts respawns and teleports: a new life is a jump, never a glide
};
// The blend the Replica draws with; t > 1 carries the motion on.
Avatar lerp(const Avatar& a, const Avatar& b, double t);
std::array<double, 3> feetEcef(const Avatar& a);

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
// The victim tells the shooter what his claim did.
struct Hit {
    uint64_t victim = 0;
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

// "Player 3F2A": the first four hex digits of the identity.
std::string playerName(uint64_t id);

} // namespace r1::online
