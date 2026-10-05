#include "check.hpp"
#include "gen/far_relief.hpp"

#include <cmath>

using namespace r1;

namespace {
// A ramp rising east at `grade` (rise over run) from `base` metres: one ring of
// 8 x 8 cells, `spacing` metres apart, sampled around (lon, lat).
FarLevel ramp(double lon, double lat, double base, double grade, FarLayers& layers, double spacing = kSlopeBaseline) {
    const Anchor anchor = Anchor::at(lon, lat, 0.0);
    auto height = [&](double x, double) {
        const double east = (x - lon) * kMetresPerDegree * std::cos(radians(lat));
        return base + grade * east;
    };
    const int cells = 8;
    const double half = 0.5 * cells * spacing;
    return sampleFarLevel(anchor, -half, -half, spacing, cells, height, layers);
}
}  // namespace

TEST(FarRelief, the_treeline_follows_the_snowline) {
    NEAR(treeline(45.0), 0.75 * snowline(45.0), 1e-9);
    CHECK(treeline(45.0) > 1900.0 && treeline(45.0) < 2200.0);
    CHECK(treeline(60.0) > 600.0 && treeline(60.0) < 1000.0);
    CHECK(treeline(0.0) > 3500.0 && treeline(0.0) < 4500.0);
}

// A slope nobody ploughs is wooded where the climate is wet, scrub where it
// is mediterranean; the plain keeps the region's ground.
TEST(FarRelief, steep_slopes_below_the_treeline_are_wooded) {
    FarLayers alps, cape;
    const FarLevel vercors = ramp(5.72, 45.20, 800.0, 0.5, alps);
    CHECK(vercors.forestCells == 64 && vercors.rockCells == 0);
    CHECK(vercors.layers[0] == FarLayers::kForest);
    const FarLevel plain = ramp(5.72, 45.20, 220.0, 0.02, alps);
    CHECK(plain.forestCells == 0 && plain.scrubCells == 0);
    CHECK(plain.layers[0] > FarLayers::kScrub);
    const FarLevel table = ramp(18.42, -33.96, 400.0, 0.5, cape);
    CHECK(table.scrubCells == 64 && table.forestCells == 0);
    // Above the trees the slope is the region's ground again, below the snow.
    const FarLevel high = ramp(5.72, 45.20, 2400.0, 0.5, alps);
    CHECK(high.forestCells == 0 && high.snowCells < 64);
}

// Terrain is self-affine: a cliff band a coarse cell averages away still
// reads as rock, judged over the finest ring's baseline.
TEST(FarRelief, a_slope_is_judged_over_the_finest_baseline) {
    NEAR(baselineSlope(0.4, kSlopeBaseline), 0.4, 1e-12);
    NEAR(baselineSlope(0.4, 16.0 * kSlopeBaseline), 0.4 * 2.0, 1e-12);  // 16^(1/4)
    FarLayers alps;
    const FarLevel fine = ramp(5.72, 45.20, 800.0, 0.6, alps);
    CHECK(fine.rockCells == 0 && fine.forestCells == 64);
    const FarLevel coarse = ramp(5.72, 45.20, 800.0, 0.6, alps, 8.0 * kSlopeBaseline);
    CHECK(coarse.rockCells == 64);
}

TEST(FarRelief, cliffs_use_a_wall_scan_and_forests_follow_the_climate) {
    const auto temperate = FarLayers("temperate").materials();
    const auto tropical = FarLayers("tropical").materials();
    CHECK(temperate[FarLayers::kWater].baseColorTexture.empty());
    CHECK(temperate[FarLayers::kRock].baseColorTexture.find("cliff_albedo") != std::string::npos);
    NEAR(temperate[FarLayers::kRock].uvScale, 1.0 / 27.0, 1e-9);
    CHECK(temperate[FarLayers::kRock].baseColorTexture != temperate[FarLayers::kForest].baseColorTexture);
    CHECK(temperate[FarLayers::kForest].baseColorTexture != tropical[FarLayers::kForest].baseColorTexture);
    for (const int at : {FarLayers::kSnow, FarLayers::kRock, FarLayers::kForest, FarLayers::kScrub}) {
        CHECK(!temperate[size_t(at)].normalTexture.empty());
        CHECK(!temperate[size_t(at)].metallicRoughnessTexture.empty());
    }
}
