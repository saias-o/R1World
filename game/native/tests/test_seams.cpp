#include "check.hpp"
#include "gen/seams.hpp"
#include "gen/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <map>

using namespace r1;

namespace {
// Ground with a field's worth of hedges and ditches: a 50 m ripple a metre
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
        const double u = std::clamp((lon - b.west) / (b.east - b.west), 0.0, 1.0) * (kTerrainMeshSize - 1);
        const int i = std::min(kTerrainMeshSize - 2, int(u));
        const int row = north ? kTerrainMeshSize - 1 : 0;
        return g.at(row, i) * (1 - (u - i)) + g.at(row, i + 1) * (u - i);
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
