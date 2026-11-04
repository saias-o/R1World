#pragma once
// The parked cars of a shared world (multiplayer on): the same cars at the
// same places for everyone, with no server to say where they are. Each
// machine reads them off the tile's own street graph -- the one its traffic
// drives on -- by rules that only depend on that graph and the tile's name,
// so two machines with the same tile park the same cars. A car's identity
// is its place, not its rank in a list: a street edited on OpenStreetMap
// moves the cars of that street, never every car after it.
#include "saida/traffic/Traffic.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace r1::parking {

constexpr float kSpacing = 6.5f;        // one car length and its gap, along the kerb
constexpr float kEndClearance = 12.f;   // none this near a junction or a dead end
constexpr float kShortest = 30.f;       // a street shorter than this between two junctions parks nobody
constexpr float kFastest = 14.5f;       // m/s: town streets (50 km/h), never avenues
constexpr float kKerbGap = .25f;        // between the kerb and the car's side
constexpr float kHalfWidth = .95f;      // a car's half width, for where its centre stands
constexpr int kTaken = 35;              // of a hundred places, this many have a car

struct Spot {
    uint64_t id = 0;     // the place, everywhere the same (tile, road, position)
    uint32_t seed = 0;   // the car's model and paint
    float x = 0, y = 0;  // in the graph's plane (a tile's x and z)
    float yaw = 0;       // as saida::traffic poses say it: direction {sin, cos}
    uint32_t lane = 0;
    float s = 0;         // along that lane
};

// Kerbside places along the tile's town streets: on the kerb of the side the
// country drives on, in the outermost lane, kSpacing apart, kEndClearance
// from a junction or a dead end -- not from a shape point, where a street
// drawn in short pieces merely bends; a hash of the place decides whether a
// car stands there.
// `lateralAxisSign` is the Rules' (-1 when the plane's second axis points
// south, as a tile's does).
std::vector<Spot> kerbSpots(const saida::traffic::Graph& graph, bool leftHand, float lateralAxisSign,
                            const std::string& tile);

// Which of the fleet's cars stands there: never a bus, a truck or a bike.
const char* modelFor(uint32_t seed);

} // namespace r1::parking
