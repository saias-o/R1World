#include "check.hpp"
#include "gen/canopy.hpp"
#include "gen/scatter.hpp"
#include <filesystem>
using namespace r1;
namespace {
Canopy sample(const Tile& tile) {
    Canopy c;
    c.bounds = tile.bounds();
    // A diagonal row of trees, a small grove, a hedge line.
    for (int i = 5; i < 40; ++i) c.cells[size_t(i * Canopy::kCells + i)] = Canopy::Tree;
    for (int r = 30; r < 36; ++r) for (int col = 2; col < 9; ++col) c.cells[size_t(r * Canopy::kCells + col)] = Canopy::Tree;
    for (int col = 10; col < 40; ++col) c.cells[size_t(2 * Canopy::kCells + col)] = Canopy::Low;
    for (auto& h : c.heights) h = 0;
    c.heights[0] = 12; c.heights[7] = 9; c.heights[14] = 18; c.heights[21] = 15; c.heights[24] = 11; c.heights[28] = 22;
    c.heights[35] = 8;
    return c;
}
}  // namespace

TEST(Canopy, the_code_round_trips_and_stays_a_few_dozen_bytes) {
    const Tile tile = tileAt(-2.6556, 47.63);
    Canopy c = sample(tile);
    // Heights are kept only where a block has trees.
    for (int b = 0; b < Canopy::kBlocks * Canopy::kBlocks; ++b) {
        bool trees = false;
        const int by = b / Canopy::kBlocks, bx = b % Canopy::kBlocks;
        for (int y = by * 8; y < by * 8 + 8; ++y) for (int x = bx * 8; x < bx * 8 + 8; ++x) trees |= c.at(x, y) == Canopy::Tree;
        if (!trees) c.heights[size_t(b)] = 0;
    }
    const std::string bytes = encodeCanopy(c);
    CHECK(bytes.size() < 120);
    const Canopy back = decodeCanopy(bytes, tile.bounds());
    CHECK(back.cells == c.cells); CHECK(back.heights == c.heights);
    CHECK(encodeCanopy(back) == bytes);
    // Bare ground costs two bytes, and says whether the source had any data.
    Canopy bare; bare.bounds = tile.bounds(); bare.noSource = true;
    CHECK(encodeCanopy(bare).size() == 2); CHECK(decodeCanopy(encodeCanopy(bare), tile.bounds()).noSource);
}

TEST(Canopy, a_cell_is_a_tree_only_when_enough_of_it_stands_tall) {
    const Tile tile = tileAt(-2.6556, 47.63);
    CanopyGrid grid(tile.bounds());
    for (int i = 0; i < 100; ++i) {
        grid.add(0, 0, i < 10 ? 14 : 0);          // a crown over a tenth: tree
        grid.add(1, 0, i < 2 ? 12 : 0);           // two tall pixels: a post, not a tree
        grid.add(2, 0, i < 40 ? 2 : 0);           // 40 % at 2 m: hedge
        grid.add(3, 0, i < 10 ? 2 : 0);           // 10 % at 2 m: parked cars
    }
    const Canopy c = grid.finish();
    CHECK(c.at(0, 0) == Canopy::Tree); CHECK(c.at(1, 0) == Canopy::None);
    CHECK(c.at(2, 0) == Canopy::Low); CHECK(c.at(3, 0) == Canopy::None);
    CHECK(c.heights[0] == 14);
    const P2 middle = c.centre(0, 0);
    CHECK(c.classAt(middle.x, middle.y) == Canopy::Tree);
    CHECK(c.cellOf(tile.bounds().west - 1, middle.y) == -1);
}

TEST(Canopy, tiles_pack_by_the_square_degree_and_append) {
    const auto root = (std::filesystem::temp_directory_path() / "r1-canopy-test").string();
    std::filesystem::remove_all(root);
    const Tile a = tileAt(-2.6556, 47.63), b{a.row, a.col + 1}, far = tileAt(2.35, 48.85);
    CHECK(!storedCanopy(root, a));
    storeCanopy(root, a, sample(a));
    Canopy empty; empty.bounds = b.bounds();
    storeCanopy(root, b, empty);
    storeCanopy(root, far, sample(far));
    CHECK(canopyRegionPath(root, a) == canopyRegionPath(root, b));
    CHECK(canopyRegionPath(root, a) != canopyRegionPath(root, far));
    const auto back = storedCanopy(root, a);
    CHECK(back.has_value()); CHECK(back->count(Canopy::Tree) == sample(a).count(Canopy::Tree));
    CHECK(storedCanopy(root, b)->count(Canopy::Tree) == 0);
    // Storing a tile twice leaves one record: the file only grows once.
    const auto size = std::filesystem::file_size(canopyRegionPath(root, a));
    storeCanopy(root, a, sample(a));
    CHECK(std::filesystem::file_size(canopyRegionPath(root, a)) == size);
    std::filesystem::remove_all(root);
}

TEST(Canopy, measured_canopy_places_trees_and_removes_inferred_ones_where_it_sees_none) {
    const Tile tile = tileAt(-2.6556, 47.63);
    const P2 c = tile.center();
    const Anchor anchor = Anchor::at(c.x, c.y, 0);
    auto ground = [&](double lon, double lat) { auto p = anchor.toEngine(lon, lat, 0); p.y = 10; return p; };
    OsmData osm;
    // A park covering the whole tile: without the canopy it is sown evenly.
    const Bounds b = tile.bounds();
    OsmWay park; park.id = 7; park.tags = {{"leisure", "park"}};
    park.points = {{b.west, b.south}, {b.east, b.south}, {b.east, b.north}, {b.west, b.north}, {b.west, b.south}};
    osm.vegetation.push_back(park);
    const Scatter sown = planNature(osm, tile, anchor, ground);
    Canopy canopy = sample(tile);
    const Scatter measured = planNature(osm, tile, anchor, ground, 640, &canopy);
    CHECK(measured.stats["canopy"]["observed"] == true);
    CHECK(measured.stats["canopy"]["inferredRemovedByCanopy"].get<int>() > 0);
    CHECK(measured.stats["canopy"]["treesFromCanopy"].get<int>() > 0);
    CHECK(sown.stats["canopy"]["observed"] == false);
    // Every tree stands in a tree cell (or a shrub in a low one) and carries a height.
    int trees = 0;
    for (const auto& node : measured.nodes) {
        const auto& p = node["transform"]["position"];
        const P3 g = anchor.toGeodetic(p[0], 0, p[2]);
        const bool tree = std::find(node["groups"].begin(), node["groups"].end(), "tree") != node["groups"].end();
        if (!tree) continue;
        const auto kind = canopy.classAt(g.x, g.y);
        CHECK(kind != Canopy::None);
        trees += kind == Canopy::Tree;
    }
    CHECK(trees > 40);
    // The same canopy gives the same trees.
    CHECK(planNature(osm, tile, anchor, ground, 640, &canopy).nodes == measured.nodes);
}
