#include "check.hpp"
#include "gen/peaks.hpp"
#include "gen/sources.hpp"
#include "gen/terrain.hpp"

#include <array>
#include <map>

using namespace r1;

namespace {
ElevationGrid flat(const Tile& tile, double h) {
    ElevationGrid g{tile.bounds(), kTerrainMeshSize, {}};
    g.values.assign(size_t(kTerrainMeshSize) * size_t(kTerrainMeshSize), h);
    return g;
}
// The Sugarloaf's tile, and its summit: 396 m surveyed, about 300 m modelled.
const Tile kRio = tileAt(-43.156605, -22.949413);
const Peak kSugarloaf{1, -43.156605, -22.949413, 396.0, "Pão de Açúcar"};
}  // namespace

TEST(Peaks, ele_is_read_only_as_metres) {
    CHECK(parseElevation("1901") == 1901.0);
    CHECK(parseElevation("1901 m") == 1901.0);
    CHECK(parseElevation("1901m") == 1901.0);
    CHECK(parseElevation("396.5") == 396.5);
    CHECK(!parseElevation("6234 ft"));
    CHECK(!parseElevation("1200-1300"));
    CHECK(!parseElevation("ca. 900"));
    CHECK(!parseElevation(""));
    CHECK(!parseElevation("12000"));
}

TEST(Peaks, the_summit_stands_at_its_surveyed_height) {
    nlohmann::json report;
    const ElevationGrid base = flat(kRio, 300.0);
    const ElevationGrid g = raiseToPeaks(base, {kSugarloaf}, {&base}, &report);
    CHECK(g.size == kTerrainMeshSize);
    // On the drawn ground, not only at a vertex: the terrain mesh is what is walked.
    const double top = g.sample(kSugarloaf.lon, kSugarloaf.lat);
    CHECK(top > 393.0 && top <= 396.0);
    // Past the summit's reach nothing moves; within it the ground falls away.
    const double near = g.sample(kSugarloaf.lon + 40.0 / (kMetresPerDegree * std::cos(radians(kSugarloaf.lat))), kSugarloaf.lat);
    CHECK(near < top && near > 300.0);
    const double far = g.sample(kSugarloaf.lon, kSugarloaf.lat + 200.0 / kMetresPerDegree);
    NEAR(far, 300.0, 1e-9);
    CHECK(report["raised"].size() == 1);
    NEAR(report["raised"][0]["raisedBy"].get<double>(), 96.0, 1e-9);
}

TEST(Peaks, a_doubtful_tag_is_refused_and_said) {
    nlohmann::json report;
    const ElevationGrid base = flat(kRio, 300.0);
    Peak feet = kSugarloaf;
    feet.ele = 1299.0;  // 396 m written in feet without the unit
    const ElevationGrid g = raiseToPeaks(base, {feet}, {&base}, &report);
    CHECK(g.values == base.values);
    CHECK(report["doubted"].size() == 1);
    CHECK(report["raised"].empty());
}

TEST(Peaks, a_summit_the_relief_already_has_changes_nothing) {
    nlohmann::json report;
    const ElevationGrid base = flat(kRio, 396.2);
    const ElevationGrid g = raiseToPeaks(base, {kSugarloaf}, {&base}, &report);
    CHECK(g.values == base.values);
    CHECK(report["agreed"].get<int>() == 1);
}

// Two tiles side by side raise their shared edge the same way, whichever of
// them holds the summit: the raise reads only the vertex, the peak and the
// distance between them.
TEST(Peaks, neighbours_share_the_raised_edge) {
    const Tile west = kRio, east{kRio.row, kRio.col + 1};
    const Bounds b = west.bounds();
    const Peak edge{2, b.east - 30.0 / (kMetresPerDegree * std::cos(radians(b.south))), (b.south + b.north) / 2, 380.0, ""};
    const ElevationGrid w = flat(west, 300.0), e = flat(east, 300.0);
    const ElevationGrid rw = raiseToPeaks(w, {edge}, {&w, &e}), re = raiseToPeaks(e, {edge}, {&e, &w});
    const int n = kTerrainMeshSize;
    bool moved = false;
    for (int row = 0; row < n; ++row) {
        NEAR(rw.at(row, n - 1), re.at(row, 0), 1e-9);
        moved |= re.at(row, 0) > 300.5;
    }
    CHECK(moved);
}

TEST(Peaks, a_tile_reads_the_degrees_its_summits_reach_from) {
    // Well inside a degree: one list. On a degree's edge: the neighbour's too.
    CHECK(peakCells(tileAt(5.5, 45.5).bounds()).size() == 1);
    const auto cells = peakCells(tileAt(5.9999, 45.5).bounds());
    CHECK(cells.size() == 2);
    CHECK((cells[0] == PeakCell{45, 5}));
    CHECK((cells[1] == PeakCell{45, 6}));
}

// The Terrain Tiles are read between pixel centres, across an image's edge as
// within it: a ramp of one metre a pixel reads back as itself.
TEST(Peaks, terrain_tiles_are_read_between_pixel_centres) {
    const int zoom = 13, pixels = (1 << zoom) * 256;
    std::map<std::pair<int, int>, std::vector<unsigned char>> images;
    auto rgb = [&](int x, int y) -> const unsigned char* {
        auto& image = images[{x, y}];
        if (image.empty()) {
            image.resize(256 * 256 * 3);
            for (int py = 0; py < 256; ++py)
                for (int px = 0; px < 256; ++px) {
                    const double h = 100.0 + (x * 256 + px) % 1000;  // metres
                    const double v = h + 32768.0;
                    unsigned char* p = &image[size_t((py * 256 + px) * 3)];
                    p[0] = (unsigned char)(int(v) / 256);
                    p[1] = (unsigned char)(int(v) % 256);
                    p[2] = (unsigned char)((v - std::floor(v)) * 256);
                }
        }
        return image.data();
    };
    // Halfway between the last pixel centre of image 4000 and the first of 4001.
    const double gx = 4001 * 256.0;
    const double lon = gx / pixels * 360.0 - 180.0;
    const double h = terrariumHeight(lon, 45.0, zoom, rgb);
    NEAR(h, 100.0 + ((4001 * 256 - 1) % 1000) + 0.5, 1e-6);
    // A pixel without data is not a height of -32768 m.
    auto empty = [&](int, int) -> const unsigned char* {
        static std::vector<unsigned char> zeros(256 * 256 * 3, 0);
        return zeros.data();
    };
    CHECK(std::isnan(terrariumHeight(lon, 45.0, zoom, empty)));
}
