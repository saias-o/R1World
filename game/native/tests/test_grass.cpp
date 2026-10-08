#include "check.hpp"
#include "gen/grass.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

using namespace r1;

namespace {

// A tile at Theix on a gentle slope: lawn to the west of its middle, bare
// ground to the east.
struct Lawn {
    Tile tile = tileAt(-2.655, 47.629);
    Bounds b = tile.bounds();
    Anchor anchor = Anchor::at((b.west + b.east) / 2, (b.south + b.north) / 2, 0.0);
    double middle = (b.west + b.east) / 2;
    TerrainGrid grid;

    Lawn() {
        ElevationGrid e{b, kTerrainMeshSize, {}};
        for (int r = 0; r < kTerrainMeshSize; ++r)
            for (int c = 0; c < kTerrainMeshSize; ++c) e.values.push_back(30.0 + 0.2 * c + 0.1 * r);
        buildTerrain(b, e, anchor, [&](double lon, double) { return std::string(lon < middle ? "lawn" : "bare"); },
                     {}, &grid);
    }
    GrassCover cover(const std::vector<const MeshPart*>& laid, const std::vector<Ring>& footprints) const {
        return grassCover(
            grid,
            [](const std::string& cls) -> std::optional<std::string> {
                if (cls == "lawn") return std::string("grass");
                return std::string("bare");
            },
            [](const std::string&) { return std::array<double, 3>{0.08, 0.16, 0.05}; }, laid, footprints);
    }
    // The cover texel under an engine point.
    static uint32_t at(const GrassCover& g, double x, double z) {
        const auto& m = g.uvFromEngine;
        const double u = m[0] * x + m[1] * z + m[2], v = m[3] * x + m[4] * z + m[5];
        const int i = std::clamp(int(u * g.coverSize), 0, g.coverSize - 1);
        const int j = std::clamp(int(v * g.coverSize), 0, g.coverSize - 1);
        return g.cover[size_t(j) * g.coverSize + i];
    }
    P3 point(double fu, double fv) const {
        return anchor.toEngine(b.west + (b.east - b.west) * fu, b.south + (b.north - b.south) * fv, 0.0);
    }
};

MeshPart flatSquare(const char* name, P3 centre, double half, double lift) {
    MeshPart part;
    part.name = name;
    auto at = [&](double dx, double dz) { return P3{centre.x + dx, centre.y + lift, centre.z + dz}; };
    part.mesh.addUpQuad(at(-half, -half), at(half, -half), at(half, half), at(-half, half));
    return part;
}

}  // namespace

TEST(Grass, grows_on_grassy_ground_in_its_measured_colour) {
    const Lawn lawn;
    const GrassCover g = lawn.cover({}, {});
    CHECK(!g.empty());
    CHECK(g.coverSize == kGrassCoverSize);
    const P3 west = lawn.point(0.25, 0.5), east = lawn.point(0.75, 0.5);
    const uint32_t grass = Lawn::at(g, west.x, west.z);
    CHECK(grass >> 24 == 255);  // grass grows its full density
    // The family's measured albedo, sRGB-encoded: 0.08 linear is 79.
    CHECK(std::abs(int(grass & 0xff) - 79) <= 1);
    CHECK(Lawn::at(g, east.x, east.z) == 0u);  // bare ground grows none
}

TEST(Grass, nothing_grows_under_what_is_laid_or_built) {
    const Lawn lawn;
    const P3 street = lawn.point(0.2, 0.3), deck = lawn.point(0.2, 0.6), house = lawn.point(0.3, 0.8);
    // At the height of the tile's lowest corner: no higher above the ground
    // there than a pavement is, wherever on the slope it lies.
    const MeshPart laid = flatSquare("Streets", {street.x, lawn.grid.points[0].y, street.z}, 6.0, 0.06);
    const MeshPart bridge = flatSquare("Bridge deck", {deck.x, lawn.grid.points[0].y, deck.z}, 6.0, 8.0);
    const Ring footprint = {{house.x - 5, house.z - 4}, {house.x + 5, house.z - 4}, {house.x + 5, house.z + 4},
                            {house.x - 5, house.z + 4}};
    const GrassCover g = lawn.cover({&laid, &bridge}, {footprint});
    CHECK(Lawn::at(g, street.x, street.z) == 0u);
    CHECK(Lawn::at(g, house.x, house.z) == 0u);
    CHECK(Lawn::at(g, deck.x, deck.z) >> 24 == 255);  // grass grows under a bridge
    CHECK(Lawn::at(g, street.x + 12.0, street.z) >> 24 == 255);  // and beside the street
}

TEST(Grass, stands_on_the_ground_as_drawn) {
    const Lawn lawn;
    const GrassCover g = lawn.cover({}, {});
    CHECK(g.groundSamples == kTerrainMeshSize);
    CHECK(g.heights.size() == lawn.grid.points.size());
    for (size_t k = 0; k < g.heights.size(); ++k) NEAR(g.heights[k], lawn.grid.points[k].y, 1e-3);
    // The grid's corners are the unit square of (u, v): exactly at the three
    // the map is made from, within the meridians' convergence at the fourth
    // (1e-4 of the tile, 5 cm).
    const auto& m = g.uvFromEngine;
    const int n = kTerrainMeshSize;
    for (const auto& [index, u, v] : {std::tuple{0, 0.0, 0.0}, std::tuple{n - 1, 1.0, 0.0},
                                      std::tuple{(n - 1) * n, 0.0, 1.0}, std::tuple{n * n - 1, 1.0, 1.0}}) {
        const P3 p = lawn.grid.points[size_t(index)];
        const double tolerance = index == n * n - 1 ? 2e-4 : 1e-9;
        NEAR(m[0] * p.x + m[1] * p.z + m[2], u, tolerance);
        NEAR(m[3] * p.x + m[4] * p.z + m[5], v, tolerance);
    }
}

TEST(Grass, a_tile_with_no_grassy_ground_has_no_cover) {
    const Lawn lawn;
    const GrassCover g = grassCover(
        lawn.grid, [](const std::string&) { return std::optional<std::string>("asphalt"); },
        [](const std::string&) { return std::array<double, 3>{0.1, 0.1, 0.1}; }, {}, {});
    CHECK(g.empty());
}

TEST(Grass, refined_ground_boundaries_reach_the_blades) {
    const Lawn lawn;
    ElevationGrid e{lawn.b,kTerrainMeshSize,std::vector<double>(kTerrainMeshSize*kTerrainMeshSize,30.)};
    const double step=(lawn.b.east-lawn.b.west)/(kTerrainMeshSize-1);
    for(int col=0;col<kTerrainMeshSize;++col)e.southEdge.push_back({lawn.b.west+col*step,30.});
    e.southEdge.insert(e.southEdge.begin()+1,{lawn.b.west+.4*step,40.});
    TerrainGrid grid;
    buildTerrain(lawn.b,e,lawn.anchor,[](double,double){return std::string("lawn");},{},&grid);
    CHECK(grid.southClasses.size()+1==grid.southBoundary.size());
    const auto cover=grassCover(grid,[](const std::string&){return std::optional<std::string>("grass");},
                                [](const std::string&){return std::array<double,3>{.1,.2,.1};},{},{});
    CHECK(!cover.empty());
    CHECK(cover.southBoundary.size()==size_t(kTerrainMeshSize+1));
    CHECK(cover.northBoundary.empty());
    NEAR(cover.southBoundary[1][0],.4/40,1e-7);
    const P3 tip=lawn.anchor.toEngine(lawn.b.west+.4*step,lawn.b.south,40.);
    NEAR(cover.southBoundary[1][1],tip.y,1e-5);
    for(int col=0;col<kTerrainMeshSize;++col) {
        const size_t index=col==0?0:size_t(col+1);
        NEAR(cover.southBoundary[index][0],double(col)/40,1e-7);
        NEAR(cover.southBoundary[index][1],cover.heights[size_t(col)],1e-5);
    }
}
