// The measured ground of a tile. Everything that stands on the ground
// samples it through `groundPoint`, so it sits on the rendered triangles
// rather than on the smoother source surface.
#pragma once

#include "mesh.hpp"
#include "osm.hpp"

#include <functional>

namespace r1 {

constexpr int kTerrainMeshSize = 41;

double roadWidthOf(const std::string& highway);

// The relief of one box, one mesh per ground class. `classify(lon, lat)`
// names the class of a triangle from its centre; `adjust(row, col, h)` may
// move a vertex (the sea floor is sunk under the animated surface).
std::map<std::string, Mesh> buildTerrain(const Bounds& bounds, const ElevationGrid& elevations,
                                         const Anchor& anchor,
                                         const std::function<std::string(double, double)>& classify,
                                         const std::function<double(int, int, double)>& adjust);

// The engine point on the rendered terrain triangle under (lon, lat).
P3 groundPoint(double lon, double lat, const ElevationGrid& elevations, const Anchor& anchor, double lift = 0.0);

}  // namespace r1
