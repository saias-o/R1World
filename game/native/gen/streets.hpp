// Terrain-following streets, tagged sidewalks and surveyed zebra crossings.
// Widths without tags are estimates, never survey data, and the manifest
// counts which is which.
#pragma once

#include "clip.hpp"
#include "mesh.hpp"
#include "osm.hpp"
#include "polygons.hpp"

namespace r1 {

// OSM `highway` values a car drives on.
bool isMotorway(const std::string& highway);
// A length tag in metres, or `fallback` when absent or absurd (0 < n <= 50).
double lengthTag(const std::string* value, double fallback);
double roadWidth(const Tags& tags);

// Lays flat regions (engine x, z) on the rendered terrain: each triangle is
// cut to the terrain triangle under it and takes that triangle's plane, so a
// road or a runway never floats over a dip or sinks into a hump.
class Drape {
public:
    Drape(const ElevationGrid& elevations, const Anchor& anchor);
    void lay(const clip::Paths64& region, double lift, Mesh& mesh) const;
    double heightAt(P2 engineXZ) const;

private:
    Bounds bounds_;
    Anchor anchor_;
    int size_;
    std::vector<P3> grid_;
};

struct StreetOutput {
    std::vector<MeshPart> parts;
    nlohmann::json stats;
};

// `roads` are already clipped to the tile; `footprints` are the buildings'
// rings in engine (x, z), which no street surface may cover.
StreetOutput buildStreets(const std::vector<OsmWay>& roads, const std::vector<OsmNode>& features,
                          const ElevationGrid& elevations, const Anchor& anchor,
                          const std::vector<Ring>& footprints);

}  // namespace r1
