// Roads on the ground: the relief they are graded into.
//
// The relief is a survey sampled every ~14 m, and a surface model in places
// (buildings and trees lift SRTM). A road draped over it rolls with every knot
// and leans with every cell. A road engineer grades it instead:
//
//   * along the road, the ground under it is averaged over a Gaussian window
//     -- 12 m for a service road to 45 m for a motorway -- that continues
//     through junctions along the straightest way on. A real climb stays a
//     climb; a bump goes. A point above the road's mean counts less, so the
//     road cuts into a bump rather than riding it, and it never departs from
//     the survey by more than its class allows (more cut than fill);
//   * across it, the carriageway and most of a terrain cell beside it take the
//     road's level, so the road is level side to side;
//   * beyond, the ground meets the survey again in a smooth bank three times
//     as wide as it is high, never narrower than 10 m.
//
// Left alone: bridges, embankments and tunnels (gen/bridges solves their own
// profile; grading fades out 15-40 m from them), the ground dug beneath a
// bridge, footpaths, and ground at sea level. Two tiles do not read the same
// roads near their seam -- each reads its own neighbourhood queries -- so the
// seam keeps the joined relief both sides agree on (gen/seams), and grading
// fades in over 30 m from every tile edge. Grading is inferred, and the
// manifest says by how much it moved the survey and how the roads ride on it.
#pragma once

#include "bridges.hpp"

namespace r1 {

constexpr int kGradingRevision = 1;

// The tile's relief at the terrain mesh's resolution, graded under its roads.
// `roads` are the observed ways, as both tiles of a seam read them: not the
// plan's pieces, whose predicted bridges each tile solves for itself. The plan
// gives the works the grading keeps clear of. `field` is the ungraded, joined
// relief the road profiles are read from.
ElevationGrid gradedGround(const ElevationGrid& grid, const std::vector<OsmWay>& roads, const GradePlan& plan,
                           const GroundField& field, nlohmann::json* stats = nullptr);

// Where a road stands at its own level (an embankment, a low deck: gen/bridges
// `raised`) the terrain is a 14 m mesh, and its triangles reach over the
// narrow road wherever the ground beside it is higher. The road wins where it
// is: the drawn terrain is cut out under every such piece that the ground comes
// within 0.6 m of, each triangle keeping its own plane, and where the ground
// left beside the road is higher than it, a cutting face joins the road's edge
// to it. Decks high over the ground keep the terrain beneath them. `terrain` is
// the drawn ground by class, in engine coordinates; the faces are returned.
Mesh cutUnderRaisedRoads(std::map<std::string, Mesh>& terrain, const nlohmann::json& raised,
                         nlohmann::json* stats = nullptr);

}  // namespace r1
