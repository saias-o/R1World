#include "check.hpp"
#include "gen/seams.hpp"
#include "gen/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <map>

using namespace r1;

namespace {
// Ground with a field's worth of mapped boundaries and ditches: a 50 m ripple a metre
// high, which 41 nodes a tile sample differently in two staggered rows.
double field(double lon, double lat) {
    const double x = lon * kMetresPerDegree * std::cos(radians(lat)), y = lat * kMetresPerDegree;
    return 20.0 + 0.004 * x + 1.0 * std::sin(2 * kPi * x / 50.0) + 0.6 * std::cos(2 * kPi * y / 37.0);
}

ElevationGrid sampled(const Tile& t, double offset) {
    const Bounds b = t.bounds();
    ElevationGrid g{b, kTerrainMeshSize, {}};
    for (int r = 0; r < kTerrainMeshSize; ++r)
        for (int c = 0; c < kTerrainMeshSize; ++c) {
            const double lon = b.west + (b.east - b.west) * c / (kTerrainMeshSize - 1);
            const double lat = b.south + (b.north - b.south) * r / (kTerrainMeshSize - 1);
            g.values.push_back(field(lon, lat) + offset);
        }
    return g;
}

// A neighbourhood around Saint-Armel, where IGN and Terrain Tiles meet.
struct World {
    std::map<Tile, RankedGround> raw;
    std::map<Tile, ElevationGrid> stitched;
    RankedGround of(const Tile& t, int rank, double offset) { return raw[t] = RankedGround{sampled(t, offset), rank}; }
    GroundOf around() {
        return [this](const Tile& t) -> std::optional<RankedGround> {
            auto it = raw.find(t);
            if (it == raw.end()) return std::nullopt;
            return it->second;
        };
    }
    void stitchAll() {
        for (const auto& [t, g] : raw) stitched[t] = stitchedGround(t, g, around());
    }
    // What a tile draws along its north or south edge: straight between nodes.
    double edge(const Tile& t, bool north, double lon) const {
        const ElevationGrid& g = stitched.at(t);
        const Bounds b = t.bounds();
        return terrainElevation(lon, north ? b.north : b.south, g);
    }
    // The largest step along the seam between `low` and `high` (rows r, r+1).
    double rowStep(const Tile& low, const Tile& high) const {
        const Bounds a = low.bounds(), b = high.bounds();
        const double from = std::max(a.west, b.west), to = std::min(a.east, b.east);
        double worst = 0;
        for (int k = 0; k <= 400; ++k) {
            const double lon = from + (to - from) * k / 400;
            worst = std::max(worst, std::abs(edge(low, true, lon) - edge(high, false, lon)));
        }
        return worst;
    }
};

const Tile kHere = tileAt(-2.7134, 47.5625);
}  // namespace

TEST(Seams, survey_grades_rank_as_trusted) {
    CHECK(groundRank("IGN RGE ALTI bare-earth terrain via Geoplateforme") >
          groundRank("Mapzen Terrain Tiles z13 (SRTM and open DEM via AWS)"));
    CHECK(groundRank("Mapzen Terrain Tiles z13 (SRTM and open DEM via AWS)") >
          groundRank("Mapzen Terrain Tiles z12 (SRTM and open DEM via AWS)"));
    CHECK(groundRank("Mapzen Terrain Tiles z12 (SRTM and open DEM via AWS)") >
          groundRank("Copernicus DEM GLO-90 via Open-Meteo (fallback)"));
    CHECK(groundRank("Copernicus DEM GLO-90 via Open-Meteo (fallback)") > groundRank("the installed relief layer (278 m)"));
    CHECK(groundRank("the installed relief layer (278 m)") > groundRank("temporary flat ground (relief pending)"));
}

TEST(Seams, staggered_rows_of_one_survey_meet) {
    // Two rows of the same survey: the nodes along their shared edge are
    // staggered, and the straight edges between them disagreed.
    World w;
    for (int dr = -1; dr <= 2; ++dr)
        for (int dc = -2; dc <= 2; ++dc) {
            const int row = kHere.row + dr;
            const Tile t = tileAt(kHere.center().x + dc * (kHere.bounds().east - kHere.bounds().west), -90.0 + (row + 0.5) * kStep);
            w.of(t, 5, 0.0);
        }
    w.stitched.clear();
    for (const auto& [t, g] : w.raw) w.stitched[t] = g.grid;
    const Tile above = tileAt(kHere.center().x, kHere.bounds().north + kStep / 2);
    CHECK(above.row == kHere.row + 1);
    const double before = w.rowStep(kHere, above);
    w.stitchAll();
    const double after = w.rowStep(kHere, above);
    CHECK(before > 0.3);  // the step the car stopped on (0.35 here)
    CHECK(after < 0.06);
    // A row's own neighbours share their nodes: they meet to the millimetre.
    const Tile east{kHere.row, kHere.col + 1};
    for (int j = 0; j < kTerrainMeshSize; ++j)
        NEAR(w.stitched.at(kHere).at(j, kTerrainMeshSize - 1), w.stitched.at(east).at(j, 0), 1e-6);
}

TEST(Seams, the_better_survey_keeps_its_edge) {
    // IGN here, Terrain Tiles to the east, four metres apart: the coarser
    // ground joins the survey, never the other way round.
    World w;
    const double width = kHere.bounds().east - kHere.bounds().west;
    for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -2; dc <= 2; ++dc) {
            const int row = kHere.row + dr;
            const Tile t = tileAt(kHere.center().x + dc * width, -90.0 + (row + 0.5) * kStep);
            const bool survey = t.bounds().west < kHere.bounds().east - 1e-9;
            w.of(t, survey ? 5 : 4, survey ? 0.0 : 4.0);
        }
    w.stitchAll();
    const Tile east{kHere.row, kHere.col + 1};
    double surveyMoved = 0, coarseMoved = 0;
    for (int j = 5; j < kTerrainMeshSize - 5; ++j) {
        NEAR(w.stitched.at(kHere).at(j, kTerrainMeshSize - 1), w.stitched.at(east).at(j, 0), 1e-6);
        surveyMoved = std::max(surveyMoved, std::abs(w.stitched.at(kHere).at(j, kTerrainMeshSize - 1) - w.raw.at(kHere).grid.at(j, kTerrainMeshSize - 1)));
        coarseMoved = std::max(coarseMoved, std::abs(w.stitched.at(east).at(j, 0) - w.raw.at(east).grid.at(j, 0)));
    }
    CHECK(coarseMoved > 3.5);
    CHECK(surveyMoved < 0.6);  // only what the row seams' smoothing asks of the corners
    // Far from every edge, the survey is untouched.
    NEAR(w.stitched.at(kHere).at(20, 20), w.raw.at(kHere).grid.at(20, 20), 1e-9);
}

TEST(Seams, a_tile_alone_keeps_its_ground_inside) {
    World w;
    const RankedGround own = w.of(kHere, 5, 0.0);
    const ElevationGrid alone = stitchedGround(kHere, own, {});
    NEAR(alone.at(20, 20), own.grid.at(20, 20), 1e-9);
    CHECK(alone.size == kTerrainMeshSize);
}

TEST(Seams, steep_staggered_survey_edges_close) {
    // Grenoble's steep relief exposed the residual left by smoothing and
    // interpolating the common curve on two different regular lattices.
    World w;
    const Tile low{27038,26176}, high{27039,26174};
    const double width = low.bounds().east - low.bounds().west;
    for (int dr = -1; dr <= 2; ++dr) for (int dc = -2; dc <= 2; ++dc) {
        const int row = low.row + dr;
        const Tile t = tileAt(low.center().x + dc * width, -90.0 + (row + .5) * kStep);
        auto g = sampled(t, 0);
        for (auto& h : g.values) h *= 80;
        w.raw[t] = {g, 5}; w.stitched[t] = g;
    }
    CHECK(w.rowStep(low, high) > 1);
    w.stitchAll();
    CHECK(w.rowStep(low, high) < 1e-6);
    NEAR(w.stitched.at(low).at(20,20), w.raw.at(low).grid.at(20,20), 1e-9);
    CHECK(w.stitched.at(low).northEdge.size() > kTerrainMeshSize);
}

TEST(Seams, refined_boundary_queries_follow_the_drawn_triangles) {
    const Bounds b{45., 5., 45.005, 5.007};
    ElevationGrid g{b, kTerrainMeshSize, std::vector<double>(kTerrainMeshSize*kTerrainMeshSize, 200.)};
    const double step = (b.east-b.west)/(kTerrainMeshSize-1);
    // A ridge between two regular vertices must be drawn and queried alike.
    g.southEdge = {{b.west,200.},{b.west+.4*step,240.},{b.west+step,200.},{b.east,200.}};
    g.northEdge = {{b.west,200.},{b.west+.6*step,260.},{b.west+step,200.},{b.east,200.}};
    const Anchor anchor = Anchor::at(5.,45.);
    TerrainGrid regular;
    const auto meshes = buildTerrain(b,g,anchor,nullptr,nullptr,&regular);
    const Mesh& mesh = meshes.at("");
    CHECK(mesh.indices.size() == size_t(6*40*40+6));
    CHECK(regular.classes.size() == size_t(2*40*40));
    for (bool north : {false,true}) for (double u : {.1,.3,.5,.7,.9}) for (double inward : {0.,.1,.3,.6}) {
        const double lon=b.west+u*step;
        const double lat=north ? b.north-inward*(b.north-b.south)/40 : b.south+inward*(b.north-b.south)/40;
        const P3 q=groundPoint(lon,lat,g,anchor);
        bool found=false;
        for (size_t i=0;i<mesh.indices.size();i+=3) {
            const P3 a=mesh.positions[mesh.indices[i]],bb=mesh.positions[mesh.indices[i+1]],c=mesh.positions[mesh.indices[i+2]];
            const double det=(bb.z-c.z)*(a.x-c.x)+(c.x-bb.x)*(a.z-c.z);
            const double wa=((bb.z-c.z)*(q.x-c.x)+(c.x-bb.x)*(q.z-c.z))/det;
            const double wb=((c.z-a.z)*(q.x-c.x)+(a.x-c.x)*(q.z-c.z))/det,wc=1-wa-wb;
            if(wa < -1e-4 || wb < -1e-4 || wc < -1e-4)continue;
            NEAR(q.y,wa*a.y+wb*bb.y+wc*c.y,.003);
            found=true;break;
        }
        CHECK(found);
        NEAR(terrainElevation(lon,lat,g),anchor.toGeodetic(q.x,q.y,q.z).z,.03);
    }
    NEAR(terrainElevation(b.west+.4*step,b.south,g),240.,1e-7);
    NEAR(terrainElevation(b.west+.6*step,b.north,g),260.,1e-7);
}

TEST(Seams, shared_knots_work_with_a_sparse_neighbourhood_and_mixed_surveys) {
    World w;
    const Tile low{27038,26176}, high{27039,26174};
    w.of(low,5,0.);w.of(high,4,4.);
    w.stitchAll();
    CHECK(w.rowStep(low,high) < 1e-6);
}
