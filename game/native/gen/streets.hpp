// Terrain-following streets, tagged sidewalks and surveyed zebra crossings.
// Widths without tags are estimates, never survey data, and the manifest
// counts which is which.
#pragma once

#include "clip.hpp"
#include "mesh.hpp"
#include "osm.hpp"
#include "polygons.hpp"

#include <functional>

namespace r1 {

// OSM `highway` values a car drives on.
bool isMotorway(const std::string& highway);
// A length tag in metres, or `fallback` when absent or absurd (0 < n <= 50).
double lengthTag(const std::string* value, double fallback);
double roadWidth(const Tags& tags);
// Shared by ground streets, bridges, markings and traffic. Width is paved
// width; the lane area excludes the inferred/tagged hard shoulders.
struct RoadProfile {
    int lanes = 1, direction = 0;
    double width = 0, leftShoulder = 0, rightShoulder = 0;
    bool express = false, link = false, widthTagged = false, lanesTagged = false;
    double travelWidth() const { return width - leftShoulder - rightShoulder; }
    double laneCentre() const { return (leftShoulder - rightShoulder) / 2; }
};
RoadProfile roadProfile(const Tags& tags);
int roadDirection(const Tags& tags);
// The sides ("left", "right") a road has a sidewalk on, and whether that is
// inferred (an urban street with no `sidewalk` tag) rather than tagged.
std::vector<std::pair<std::string, bool>> sidewalkSides(const Tags& tags);
// Infer settlement context from nearby building frontage, independently of
// highway class. Surveyed sidewalk tags still win. Copies keep OSM untouched.
std::vector<OsmWay> contextualRoads(const std::vector<OsmWay>& roads,
                                  const std::vector<OsmWay>& buildings, const Anchor& anchor);

// Lays flat regions (engine x, z) on the rendered terrain: each triangle is
// cut to the terrain triangle under it and takes that triangle's plane, so a
// road or a runway never floats over a dip or sinks into a hump.
// `adjust(row, col, h)` moves a grid vertex as the terrain moved it
// (buildTerrain's own argument), so what is laid stays on the ground drawn.
class Drape {
public:
    Drape(const ElevationGrid& elevations, const Anchor& anchor,
          const std::function<double(int, int, double)>& adjust = {});
    void lay(const clip::Paths64& region, double lift, Mesh& mesh) const;
    double heightAt(P2 engineXZ) const;
    // The tile's outline in engine (x, z): nothing is laid outside it.
    clip::Path64 outline() const;

private:
    Bounds bounds_;
    Anchor anchor_;
    int size_;
    std::vector<P3> grid_;
};

struct StreetOutput {
    std::vector<MeshPart> parts;
    nlohmann::json stats;
    // Every street surface laid on the ground (carriageway, cobbles and
    // pavement) inside the tile, engine (x, z): no water is drawn over it.
    clip::Paths64 ground;
};

// `roads` may be cut one segment per way (cook's clipRoads): the segments of
// a way are joined back into its line. They may reach past the tile, which
// they should, so a street crossing its edge meets its neighbour's without a
// notch: only what lies inside the tile is laid. `footprints` are the
// buildings' rings in engine (x, z), which no street surface may cover;
// `adjust` is the terrain's (see Drape).
StreetOutput buildStreets(const std::vector<OsmWay>& roads, const std::vector<OsmNode>& features,
                          const ElevationGrid& elevations, const Anchor& anchor,
                          const std::vector<Ring>& footprints,
                          const std::function<double(int, int, double)>& adjust = {},
                          const std::string& country = {},
                          const clip::Paths64& vergeExclusions = {});

}  // namespace r1
