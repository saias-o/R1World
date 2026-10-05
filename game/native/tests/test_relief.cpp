#include "check.hpp"
#include "gen/relief.hpp"
#include "gen/far_relief.hpp"
#include "gen/sources.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace r1;

namespace {
// A square degree of mountains: ridges, a cliff and a sea in its south-west.
ReliefCell mountains(int south, int west) {
    ReliefCell cell{south, west, {}};
    for (int r = 0; r < cell.rows(); ++r)
        for (int c = 0; c < cell.columns(); ++c) {
            const double x = double(c) / cell.columns(), y = double(r) / cell.rows();
            double h = 1800.0 * std::sin(9.0 * x) * std::cos(7.0 * y) + 900.0 * x + 300.0 * y;
            if (x > 0.6 && x < 0.62) h += 400.0;  // a cliff band
            cell.heights.push_back(int16_t(std::lround(std::clamp(h, double(kLowestHeight), double(kHighestHeight)))));
        }
    return cell;
}
}  // namespace

TEST(Relief, a_degree_is_about_278_m_square) {
    CHECK(columnsFor(0) == kRowsPerDegree);
    CHECK(columnsFor(-1) == kRowsPerDegree);  // its north edge is the equator
    CHECK(columnsFor(45) == int(std::ceil(kRowsPerDegree * std::cos(radians(45.0)))));
    CHECK(columnsFor(-46) == columnsFor(45));  // mirrored: the edge nearest the equator
    CHECK(columnsFor(84) >= 1);
}

TEST(Relief, a_cell_comes_back_exactly) {
    const ReliefCell cell = mountains(45, 5);
    const std::string bytes = encodeReliefCell(cell);
    const ReliefCell back = decodeReliefCell(bytes, 45, 5);
    CHECK(back.heights == cell.heights);
    // Smooth relief costs a few bits a sample, not the sixteen it is stored in.
    CHECK(bytes.size() < cell.heights.size() * 2 / 3);
    CHECK(encodeReliefCell(cell) == bytes);  // deterministic
}

TEST(Relief, heights_out_of_range_are_clamped) {
    ReliefCell cell = mountains(27, 86);
    cell.heights[0] = -9000;   // the sea floor is not drawn
    cell.heights[1] = 12000;
    const ReliefCell back = decodeReliefCell(encodeReliefCell(cell), 27, 86);
    CHECK(back.heights[0] == kLowestHeight && back.heights[1] == kHighestHeight);
}

TEST(Relief, the_layer_reads_its_packs) {
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / "r1-relief").string();
    fs::remove_all(dir);
    fs::create_directories(dir);
    CHECK(!ReliefLayer(dir).installed());

    std::vector<std::optional<std::string>> cells(size_t(kPackDegrees * kPackDegrees));
    const ReliefCell alps = mountains(45, 5), east = mountains(45, 6);
    cells[size_t(5 * kPackDegrees + 5)] = encodeReliefCell(alps);
    cells[size_t(5 * kPackDegrees + 6)] = encodeReliefCell(east);
    std::ofstream(dir + "/" + packFileName(40, 0), std::ios::binary) << encodeReliefPack(cells);

    const ReliefLayer layer(dir);
    CHECK(layer.installed());
    const double lat = 45.0 + 123.0 / kRowsPerDegree, lon = 5.0 + 77.0 / columnsFor(45);
    NEAR(*layer.height(lon, lat), alps.at(123, 77), 1e-9);
    NEAR(*layer.height(43.5 - 40.0, 41.5), 0.0, 1e-12);       // a cell with no land is sea
    CHECK(!layer.height(25.0, 41.5));                          // no pack there
    CHECK(!layer.height(5.5, 86.0));                           // past the Mercator images
    fs::remove_all(dir);
}

TEST(Relief, truncated_cells_and_invalid_pack_spans_are_refused) {
    auto bytes = encodeReliefCell(mountains(45, 5));
    bytes.resize(bytes.size() / 2);
    bool refused = false;
    try { decodeReliefCell(bytes, 45, 5); }
    catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);

    namespace fs = std::filesystem;
    const auto path = (fs::temp_directory_path() / "r1-invalid-relief.r1relief").string();
    std::vector<std::optional<std::string>> cells(size_t(kPackDegrees * kPackDegrees));
    cells[0] = encodeReliefCell(mountains(40, 0));
    const auto valid = encodeReliefPack(cells);
    std::ofstream(path, std::ios::binary) << valid;
    CHECK(validReliefPackFile(path));
    for (int fault = 0; fault < 3; ++fault) {
        auto broken = valid;
        if (fault == 0) broken.resize(broken.size() - 1);
        if (fault == 1) broken[5] = 0;  // a cell overlapping the header
        if (fault == 2) broken[12] = char(0xFF);  // hostile length
        std::ofstream(path, std::ios::binary | std::ios::trunc) << broken;
        CHECK(!validReliefPackFile(path));
    }
    fs::remove(path);
}

TEST(Relief, offline_rings_and_their_border_use_the_installed_layer) {
    namespace fs = std::filesystem;
    const auto root = (fs::temp_directory_path() / "r1-installed-rings").string();
    fs::remove_all(root);
    fs::create_directories(root + "/" + kReliefDirectory);
    std::vector<std::optional<std::string>> cells(size_t(kPackDegrees * kPackDegrees));
    cells[size_t(5 * kPackDegrees + 5)] = encodeReliefCell(mountains(45, 5));
    std::ofstream(root + "/" + kReliefDirectory + "/" + packFileName(40, 0), std::ios::binary) << encodeReliefPack(cells);
    const ObservationStore store(root);
    const Anchor anchor = Anchor::at(5.5, 45.5, 0.0);
    const auto measured = [&](double lon, double lat) { return *installedRelief(root).height(lon, lat); };
    FarLayers layers;
    constexpr int resolution = 16;
    for (const double spacing : {128.0, kInstalledReliefSpacing, 512.0}) {
        const double origin = -resolution * spacing / 2;
        const auto actual = sampleWorldFarLevel(store, anchor, origin, origin, spacing, resolution, layers, false);
        const auto expected = sampleFarLevel(anchor, origin, origin, spacing, resolution, measured, layers, measured);
        CHECK(actual.heights == expected.heights);
        CHECK(actual.layers == expected.layers);
        CHECK(actual.installedFallback == (spacing < kInstalledReliefSpacing));
    }
    const auto fine = sampleWorldFarLevel(store, anchor, -1024.0, -1024.0, 128.0, resolution, layers, false);
    const auto coarse = sampleWorldFarLevel(store, anchor, -2048.0, -2048.0, 256.0, resolution, layers);
    const size_t width = size_t(resolution + 1);
    // The fine eastern border meets samples in the coarse ring's interior.
    for (int row = 0; row <= resolution; row += 2)
        NEAR(fine.heights[size_t(row) * width + resolution],
             coarse.heights[size_t(4 + row / 2) * width + 12], 1e-5);
    CHECK(!fs::exists(root + "/cache"));
    CHECK(!installedRelief(root).height(std::nan(""), 45.5));
    CHECK(!installedRelief(root).height(5.5, std::nan("")));
    fs::remove_all(root);
}

TEST(Relief, negative_pack_coordinates_and_date_line_wrap_are_read) {
    namespace fs = std::filesystem;
    const auto dir = (fs::temp_directory_path() / "r1-relief-wrap").string();
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::vector<std::optional<std::string>> cells(size_t(kPackDegrees * kPackDegrees));
    const ReliefCell west = mountains(-46, -180);
    cells[size_t(4 * kPackDegrees)] = encodeReliefCell(west);
    std::ofstream(dir + "/" + packFileName(-50, -180), std::ios::binary) << encodeReliefPack(cells);
    const ReliefLayer layer(dir);
    NEAR(*layer.height(-179.5, -45.5), west.sample(-179.5, -45.5), 1e-9);
    NEAR(*layer.height(180.5, -45.5), *layer.height(-179.5, -45.5), 1e-9);
    fs::remove_all(dir);
}

TEST(Relief, latitude_edges_join_grids_with_different_column_counts) {
    namespace fs = std::filesystem;
    const auto dir = (fs::temp_directory_path() / "r1-relief-latitude-edge").string();
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::vector<std::optional<std::string>> cells(size_t(kPackDegrees * kPackDegrees));
    for (int south : {45, 46}) {
        ReliefCell cell{south, 5, {}};
        const int width = cell.columns();
        for (int row = 0; row < cell.rows(); ++row)
            for (int col = 0; col < width; ++col)
                cell.heights.push_back(int16_t(std::lround(1000.0 + 500.0 * std::sin(30.0 * col / (width - 1)))));
        cells[size_t((south - 40) * kPackDegrees + 5)] = encodeReliefCell(cell);
    }
    std::ofstream(dir + "/" + packFileName(40, 0), std::ios::binary) << encodeReliefPack(cells);
    const ReliefLayer layer(dir);
    for (double lon : {5.12345, 5.50123, 5.87654})
        NEAR(*layer.height(lon, 46.0 - 1e-9), *layer.height(lon, 46.0), 1e-5);
    fs::remove_all(dir);
}
