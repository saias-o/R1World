// The people on the pavement: where they may walk and how many of them
// there are, cooked with each tile, and the small simulation that walks them.
//
// Where they walk is surveyed: the sidewalks the streets are drawn with
// (tagged or inferred exactly as `streets.cpp` infers them), OSM's footways,
// pedestrian streets and paths, and the benches the props put down. How many
// is inferred and says so (PLAN §3 I5): it follows what the tile holds --
// shops and offices, bus stops, crossings, buildings, pavement -- and, at
// run time, the local hour and the rain. A commercial avenue at noon is
// busy, a village is quiet, the open country at night is empty.
//
// The simulation needs no engine: the game draws what it decides, and the
// tests run it headless.
#pragma once

#include "osm.hpp"
#include "polygons.hpp"

#include <cstdint>

namespace r1 {

constexpr int kCrowdRevision = 1;
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
    struct Link { int a = 0, b = 0; bool crossing = false; double length = 0; };
    struct Seat { double x = 0, y = 0, z = 0, yaw = 0; };  // y: the seat's top
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<Seat> seats;
    int people = 0;
    static WalkGraph from(const nlohmann::json& crowd);
};

enum class Activity : uint8_t { Walk, Idle, Wait, Phone, Talk, Sit, Flee };
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
};

struct CrowdPace { double walk = 1.0, run = 2.4; };  // m/s, as drawn

class Crowd {
public:
    // `pace` is one entry per slot's avatar: slot i always draws avatar
    // i % pace.size(), so a node never changes clothes in view.
    void reset(const WalkGraph* graph, uint32_t seed, std::vector<CrowdPace> pace);
    void setPopulation(int n) { wanted_ = n; }
    int wanted() const { return wanted_; }
    // `eye` and `facing` in the tile's frame (x east, z south); `player`
    // stands where nobody walks through; `car` at `carSpeed` scatters people.
    struct Scene {
        double eyeX = 0, eyeZ = 0, faceX = 0, faceZ = -1;
        double playerX = 1e9, playerZ = 1e9;
        double carX = 1e9, carZ = 1e9, carSpeed = 0;
    };
    void update(double dt, const Scene& scene);
    const std::vector<Walker>& walkers() const { return walkers_; }
    int live() const;

    static constexpr double kSpawnNear = 28.0, kSpawnFar = 110.0, kDespawn = 150.0;

private:
    double random();
    bool spawn(size_t slot, const Scene& scene);
    void place(Walker& w) const;
    void arrive(size_t slot, int node);
    void stand(Walker& w, double lo, double hi);
    const WalkGraph* graph_ = nullptr;
    std::vector<CrowdPace> pace_;
    std::vector<Walker> walkers_;
    std::vector<int> seatTaken_;
    uint64_t state_ = 1;
    int wanted_ = 0;
};

}  // namespace r1
