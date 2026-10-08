// Where two tiles meet, their ground meets. Each tile's relief is its own
// survey, sampled at its own 41 x 41 nodes; two neighbours disagreed at the
// edge they share -- by up to a metre between two IGN tiles, whose rows do not
// start at the same longitude, and by four between an IGN tile and a Terrain
// Tiles one -- and a car met the step as an obstacle nobody could see.
//
// The rule both sides apply, from the same inputs, so that they agree:
//   - on a seam, the better survey keeps its edge and the other joins it;
//     two of the same grade meet half-way (groundRank);
//   - along a row's edge, whose nodes are staggered from the next row's, the
//     seam is smoothed over kSeamSmoothing metres and both meshes include
//     the union of the rows' boundary nodes, so their straight edges coincide;
//   - the correction fades inward over kSeamBand of the tile.
// What is measured stays measured where it is better; what is moved is moved
// toward a better or an equal measurement, and the manifest says by how much.
#pragma once

#include "common.hpp"
#include "osm.hpp"

#include <functional>
#include <optional>
#include <string>

namespace r1 {

// Metres either side of a point a row's seam is averaged over (triangle filter).
constexpr double kSeamSmoothing = 40.0;
// The fraction of the tile, from each edge, a seam's correction fades over.
constexpr double kSeamBand = 0.3;

// How far a relief source is trusted where two tiles meet: the higher keeps
// its edge. A source this does not know ranks with the flat ground.
int groundRank(const std::string& source);

struct RankedGround {
    ElevationGrid grid;
    int rank = 0;
};
// A neighbour's relief as the neighbour itself is cooked with it; none when
// nothing is known there yet.
using GroundOf = std::function<std::optional<RankedGround>(const Tile&)>;

struct SeamReport {
    double largestShift = 0;  // metres the ground moved, at worst
    int neighbours = 0;       // tiles whose relief was known
};

// The tile's regular ground plus shared boundary knots, joined to its neighbours'.
ElevationGrid stitchedGround(const Tile& tile, const RankedGround& own, const GroundOf& around,
                             SeamReport* report = nullptr);

// A tile's ground and its eight neighbours', each joined as that tile is
// cooked: a dig under a bridge across a seam is planned on one ground from
// both sides (gen/bridges), or it opens the step again. `neighbour` is asked
// once a tile.
struct JoinedGround {
    ElevationGrid own;
    std::vector<ElevationGrid> around;
    SeamReport seams;
};
JoinedGround joinedGround(const Tile& tile, const RankedGround& own, const GroundOf& neighbour);

}  // namespace r1
