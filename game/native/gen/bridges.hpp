// Bridges: the roads that leave the ground, and the ground that makes room.
//
// OSM says which way is a bridge (`bridge=yes`), and the predictive model says
// which crossing is one where OSM is silent (gen/predict.hpp). Neither says how
// high it stands. That is solved here, as a road engineer would:
//
//   * a deck clears what passes beneath it -- 4.75 m over a road (the French
//     gabarit, the most common one in Europe), 2.6 m over a path -- plus its
//     own structure, and never sags into a valley: between its abutments it
//     spans straight;
//   * a road reaches a raised deck on an embankment, climbing at most 5 %
//     (8 % for a footpath), and every road joined to that embankment climbs
//     with it;
//   * what the deck cannot rise to give -- a surveyed bridge is drawn at the
//     level of the streets it joins -- is dug out of the relief beneath it: the
//     road below dips under the bridge.
//
// The profile is solved on the neighbourhood's roads, so the tiles a bridge
// crosses agree on it, then drawn tile by tile: deck, cornices, parapets,
// piers, abutments, embankments and their slopes. What stands above the
// ground is also written for the game to walk and drive on (`raised`).
#pragma once

#include "mesh.hpp"
#include "predict.hpp"

#include <optional>

namespace r1 {

constexpr int kBridgeRevision = 1;

// The ground bridges are solved on: the tile's own relief and, past its
// edges, its neighbours', so both sides of a tile boundary solve alike.
class GroundField {
public:
    GroundField(const ElevationGrid& own, const std::vector<ElevationGrid>& around) : own_(own), around_(around) {}
    double at(double lon, double lat) const;

private:
    const ElevationGrid& own_;
    const std::vector<ElevationGrid>& around_;
};

// One stretch of road off the ground: a bridge, or a road on its embankment.
struct RaisedRun {
    int64_t way = 0;
    Tags tags;
    bool bridge = false, predicted = false;
    std::vector<P2> points;      // lon, lat
    std::vector<double> levels;  // the road surface's elevation at each point
    std::vector<double> grounds; // the uncarved ground under each point
    bool abutStart = false, abutEnd = false;  // a bridge end standing clear of the ground
};

struct GradePlan {
    // The observed roads, cut where they leave the ground: bridges keep or get
    // `bridge=yes` (predicted ones say `r1:bridge=predicted`), embankments get
    // `r1:raised=yes`. Every downstream generator reads these.
    std::vector<OsmWay> roads;
    std::vector<RaisedRun> runs;
    // Where the relief is dug so a road passes under a bridge.
    struct Carve { P2 at; double depth, radius; };
    std::vector<Carve> carves;
    // Roads passing beneath a bridge (lon, lat, half width): no pier there.
    std::vector<std::pair<P2, double>> underneath;
    nlohmann::json stats;

    // The elevation of a road point that is off the ground, on the vertex or
    // along a run; nullopt for a point on the ground.
    std::optional<double> levelAt(P2 lonlat) const;
    // The level of the raised road whose axis passes within `reach` metres of
    // a point: what a sign beside an embankment stands on.
    std::optional<double> levelNear(P2 lonlat, double reach) const;
    std::map<P2, double> levels;
};

GradePlan planGrades(const OsmData& osm, const std::vector<Prediction>& structures, const GroundField& ground,
                     const Anchor& anchor, const Bounds& bounds);

// The tile's relief at the terrain mesh's resolution, dug where the plan says.
ElevationGrid carvedGround(const ElevationGrid& grid, const GradePlan& plan);

struct BridgeOutput {
    std::vector<MeshPart> parts;
    // What the game walks and drives on above the ground: pieces of road axis
    // with their half width, `solid` for an embankment (it walls whoever is
    // below it) or a deck (passed under).
    nlohmann::json raised = nlohmann::json::array();
    nlohmann::json stats;
};

BridgeOutput buildBridges(const GradePlan& plan, const Bounds& bounds, const ElevationGrid& carved,
                          const Anchor& anchor);

}  // namespace r1
