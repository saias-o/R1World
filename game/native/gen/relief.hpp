// The planet's relief, installed with the game: what the far relief stands on
// where nothing else has been read, offline included (PLAN §3: the world runs
// offline). Built once by tools/r1relief from the same Terrain Tiles the game
// reads online, so the two never disagree at a seam.
//
// The grid: each square degree holds kRowsPerDegree + 1 rows of samples from
// its south edge to its north edge, and as many columns as keep a sample about
// 278 m wide at the edge nearest the equator (columnsFor), corners on the
// degree lines. At a latitude edge the layer uses the northern cell's row
// to join grids whose longitude spacing differs. Heights are
// whole metres, clamped to [kLowestHeight, kHighestHeight]: the lowest dry
// land is the Dead Sea's shore, and the sea floor is not drawn.
//
// The code: each sample is predicted from its west, south and south-west
// neighbours (LOCO-I's median predictor) and the error is range-coded
// (gen/rangecoder) in the context of how rough the ground around it is. Cells
// with no land are not stored: they are sea. Cells are packed by
// kPackDegrees-square blocks, one file each, `<lat>_<lon>.r1relief` (south-west
// corner).
#pragma once

#include "common.hpp"
#include "osm.hpp"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace r1 {

constexpr int kRowsPerDegree = 400;  // 278 m
// The Terrain Tiles zoom used to build the installed layer.
constexpr int kReliefZoom = 9;
// Where the game installs it, under its root.
constexpr const char* kReliefDirectory = "assets/world/relief";
constexpr int kLowestHeight = -500;
constexpr int kHighestHeight = 9000;
constexpr int kPackDegrees = 10;
// The Terrain Tiles are Web Mercator: nothing is read past this latitude.
constexpr int kReliefLatitudeLimit = 85;

// Columns of the square degree whose south edge is `south`: samples about as
// wide as they are tall where the cell is widest.
int columnsFor(int south);

// One square degree's heights: (kRowsPerDegree + 1) x (columnsFor + 1)
// samples, row-major from the south-west corner, row along latitude.
struct ReliefCell {
    int south = 0, west = 0;
    std::vector<int16_t> heights;
    int rows() const { return kRowsPerDegree + 1; }
    int columns() const { return columnsFor(south) + 1; }
    int16_t at(int row, int column) const { return heights[size_t(row) * size_t(columns()) + size_t(column)]; }
    // Bilinear between the samples; (lon, lat) is clamped to the cell.
    double sample(double lon, double lat) const;
    bool hasLand() const;
};

// Compact bytes, and back. Deterministic: the same heights give the same bytes.
std::string encodeReliefCell(const ReliefCell& cell);
ReliefCell decodeReliefCell(const std::string& bytes, int south, int west);

// A pack's file: an index of its kPackDegrees^2 cells, then their codes; a
// cell coded in zero bytes is sea.
std::string packFileName(int south, int west);
std::string encodeReliefPack(const std::vector<std::optional<std::string>>& cells);
// Checks the header and every span against the file size, without decoding.
bool validReliefPackFile(const std::string& path);

// The installed layer: heights read on demand, the cells last used kept
// decoded. Safe to share between threads.
class ReliefLayer {
public:
    // Square degrees kept decoded: about 320 KB each.
    static constexpr size_t kCachedCells = 64;

    explicit ReliefLayer(std::string directory);
    // Whether any pack is installed at all.
    bool installed() const { return installed_; }
    // The height at (lon, lat); 0 over the sea; nullopt where the layer has
    // no pack (past kReliefLatitudeLimit, or not installed).
    std::optional<double> height(double lon, double lat) const;

private:
    struct Pack {
        std::string path;
        std::vector<std::pair<uint32_t, uint32_t>> index;  // offset, length of each cell
    };
    const Pack* pack(int south, int west) const;
    std::shared_ptr<const ReliefCell> cell(int south, int west) const;

    std::string directory_;
    bool installed_ = false;
    mutable std::mutex lock_;
    mutable std::map<std::pair<int, int>, std::optional<Pack>> packs_;
    mutable std::list<std::pair<std::pair<int, int>, std::shared_ptr<const ReliefCell>>> cells_;  // most recent first
};

// The layer installed under `gameRoot`, opened once and shared.
const ReliefLayer& installedRelief(const std::string& gameRoot);

// A tile-sized grid of the installed layer over `bounds`, kTerrainMeshSize
// square; nullopt where the layer has nothing there.
std::optional<ElevationGrid> installedGround(const Bounds& bounds, const std::string& gameRoot);
constexpr const char* kInstalledReliefSource = "the installed relief layer (278 m)";

}  // namespace r1
