#include "check.hpp"
#include "gen/far_relief.hpp"

#include <cmath>

using namespace r1;

namespace {
// A ramp rising east at `grade` (rise over run) from `base` metres: one ring of
// 8 x 8 cells, 64 m apart, sampled around (lon, lat).
FarLevel ramp(double lon, double lat, double base, double grade, FarLayers& layers) {
    const Anchor anchor = Anchor::at(lon, lat, 0.0);
    auto height = [&](double x, double) {
        const double east = (x - lon) * kMetresPerDegree * std::cos(radians(lat));
        return base + grade * east;
    };
    return sampleFarLevel(anchor, -256.0, -256.0, 64.0, 8, height, layers);
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
