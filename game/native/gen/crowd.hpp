// The people on the pavement: where they may walk and how many of them
// there are, cooked with each tile, and the small simulation that walks them.
//
// Where they walk is surveyed: the sidewalks the streets are drawn with
// (tagged or inferred exactly as `streets.cpp` infers them), OSM's footways,
// pedestrian streets and paths, and the benches the props put down. How many
// is inferred and says so (PLAN §3 I5): it follows the homes and active
// buildings near each path, the number of floors and how closely homes stand
// together. At run time, the local hour and rain scale that population.
//
// The simulation needs no engine: the game draws what it decides, and the
// tests run it headless. Among what it decides is how people meet the player:
// a stagger and a reaction when he walks into someone, a step aside when he
// stands in the way, and a look that follows him when he passes in front.
// Whether he walked into someone is not decided here: people are bodies in the
// engine's physics, and the game reports each contact the engine finds
// (Crowd::bump).
#pragma once

#include "osm.hpp"
#include "polygons.hpp"

#include <cstdint>

namespace r1 {

constexpr int kCrowdRevision = 2;
// Most people a tile's content can ask for, before the hour is applied.
constexpr int kCrowdTileCeiling = 40;
// bench.glb's seat, as a fraction of its height (0.237 of 0.526, measured
// on the model): a sitter's pelvis goes on it, whatever the bench's scale.
constexpr double kBenchSeat = 0.237 / 0.526;

// Cook side: the tile's walkable network and its people, as the manifest's
// "crowd" entry. `props` are the tile's placed props, for the benches.
nlohmann::json buildWalkGraph(const std::vector<OsmWay>& roads, const std::vector<OsmNode>& features,
                              const std::vector<const OsmWay*>& buildings, const std::vector<Ring>& footprints,
                              const ElevationGrid& elevations, const Anchor& anchor,
                              const nlohmann::json& props);

// How busy a street is at a local solar hour (0..24), 0..1.
double crowdHourFactor(double hour);

// ── run time ────────────────────────────────────────────────────────────────

struct WalkGraph {
    struct Node { double x = 0, y = 0, z = 0; std::vector<int> links; };
    struct Link { int a = 0, b = 0; bool crossing = false; double length = 0, demand = 1; };
    struct Seat { double x = 0, y = 0, z = 0, yaw = 0; };  // y: the seat's top
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<Seat> seats;
    int people = 0;
    static WalkGraph from(const nlohmann::json& crowd);
};

// Stagger is the moment a bump carries someone off their feet's line; the
// three after it are what they do about it.
enum class Activity : uint8_t { Walk, Idle, Wait, Phone, Talk, Sit, Flee, Stagger, Shrug, Angry, Dust };
const char* clipOf(Activity a);

struct Walker {
    bool alive = false;
    Activity activity = Activity::Walk;
    int link = -1;            // the link walked or stood on; -1 while going to `target`
    bool forward = true;      // a -> b
    double along = 0;         // metres from the link's start in the walking direction
    int target = -1;          // a node walked to off the graph (leaving a bench)
    double timer = 0;         // seconds left of a standing activity
    int seat = -1, partner = -1;
    double side = 0.5;        // keeps right of the path by this much
    double x = 0, y = 0, z = 0, heading = 0;  // heading: radians, atan2(east, north)
    // Meeting the player.
    double dodge = 0, dodgeWanted = 0;  // metres stepped right of `side` to pass him
    double offX = 0, offZ = 0;          // pushed off the path by a bump, walked back
    double pushX = 0, pushZ = 0;        // m/s of that push, dying away
    Activity resume = Activity::Walk;   // what a reaction goes back to, and for how long
    double resumeTimer = 0;
    double bumpCool = 0;                // a shove is one bump, not one a frame
    double grudge = 0;                  // bumps lately, fading; past one nothing is shrugged off
    uint32_t bumps = 0;                 // grows with each bump: the game staggers the body
    double bumpX = 0, bumpZ = 0, bumpSpeed = 0;  // the last one: away from the player, m/s
    // Looking at the player: `look` is 0 or 1 and the game eases the head.
    double look = 0, lookTimer = 0, lookCool = 0;
};

struct CrowdPace { double walk = 1.0, run = 2.4; };  // m/s, as drawn

class Crowd {
public:
    // `pace` is one entry per slot's avatar: slot i always draws avatar
    // i % pace.size(), so a node never changes clothes in view.
    void reset(const WalkGraph* graph, uint32_t seed, std::vector<CrowdPace> pace);
    void setPopulation(int n) { wanted_ = n; }
    int wanted() const { return wanted_; }
    // `eye` and `facing` in the tile's frame (x east, z south); the player on
    // foot, moving at `playerV` -- the speed he means, which someone in his
    // way stops; `car` at `carSpeed` scatters people.
    struct Scene {
        double eyeX = 0, eyeZ = 0, faceX = 0, faceZ = -1;
        double playerX = 1e9, playerZ = 1e9, playerVX = 0, playerVZ = 0;
        double carX = 1e9, carZ = 1e9, carSpeed = 0;
    };
    void update(double dt, const Scene& scene);
    const std::vector<Walker>& walkers() const { return walkers_; }
    int live() const;

    // The player ran into walker `slot` -- a contact the engine's physics
    // found -- along (dirX, dirZ), from him toward them, closing at `speed`
    // m/s. Slower than kBumpSpeed is leaning, not bumping, and one bump
    // lasts a moment: the contacts of the frames after it are the same one.
    // Returns whether it counted.
    bool bump(size_t slot, double dirX, double dirZ, double speed);
    // How fast walker `slot` moves along (dirX, dirZ), m/s.
    double speedAlong(size_t slot, double dirX, double dirZ) const;

    static constexpr double kSpawnNear = 28.0, kSpawnFar = 110.0, kDespawn = 150.0;
    // A person drawn at 0.8 is about 0.36 m across the shoulders; with the
    // arms swinging, this is the radius of the capsule each is in the
    // engine's physics, the player included.
    static constexpr double kBodyRadius = 0.22;
    // Slower than this into someone is leaning on them, not bumping them;
    // faster than kShove is a sprint, and nobody shrugs that off.
    static constexpr double kBumpSpeed = 0.6, kShove = 4.5;
    // How close the player passes in front of someone to be looked at.
    static constexpr double kLookRange = 7.0;

private:
    double random();
    bool spawn(size_t slot, const Scene& scene);
    void place(Walker& w) const;
    void arrive(size_t slot, int node);
    void stand(Walker& w, double lo, double hi);
    void react(Walker& w);
    void lookAtPlayer(Walker& w, double dt, const Scene& scene);
    const WalkGraph* graph_ = nullptr;
    std::vector<CrowdPace> pace_;
    std::vector<Walker> walkers_;
    std::vector<int> seatTaken_;
    std::vector<std::pair<double, int>> spawnChoices_;
    double spawnDemand_ = 0;
    uint64_t state_ = 1;
    int wanted_ = 0;
};

}  // namespace r1
