// The measured canopy: where trees stand and how tall they
// are, on a 48 x 48 grid over each tile (about 12 m a cell). It is a layer of
// the world any system may ask (vegetation, animals, shade...), measured, not
// inferred (PLAN §3 I5).
//
// The source is Meta and WRI's High Resolution Canopy Height Maps (1 m,
// global, CC BY 4.0; imagery 2009-2020). The raw map is tens of terabytes and
// is never kept: one band of it is read over HTTP, converted at once into this
// grid, and only the grid is stored. A cell is Tree when enough of it stands
// 3 m or taller; a
// block of 8 x 8 cells keeps the median height of its trees. Coded with an
// adaptive context model (as JBIG codes a page), a tile weighs a few dozen to a
// few hundred bytes: about 7 GB for all the land of the planet. Tiles are
// packed by the square degree, never a file each.
#pragma once

#include "common.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace r1 {

struct Canopy {
    static constexpr int kCells = 48, kBlock = 8, kBlocks = kCells / kBlock;
    enum Class : uint8_t { None = 0, Tree = 1 };
    Bounds bounds;
    // Row 0 is the south edge, column 0 the west.
    std::array<uint8_t, kCells * kCells> cells{};
    // Median canopy height of each block's tree cells, metres (0: no tree).
    std::array<uint8_t, kBlocks * kBlocks> heights{};
    // The source had no data here (open sea): every cell is None, measured.
    bool noSource = false;

    Class at(int col, int row) const { return Class(cells[size_t(row * kCells + col)]); }
    // The cell under a point, clamped to the tile; -1 outside it.
    int cellOf(double lon, double lat) const;
    Class classAt(double lon, double lat) const;
    double heightAt(int col, int row) const { return heights[size_t((row / kBlock) * kBlocks + col / kBlock)]; }
    // The geodetic middle of a cell.
    P2 centre(int col, int row) const;
    P2 size() const { return {(bounds.east - bounds.west) / kCells, (bounds.north - bounds.south) / kCells}; }
    int count(Class c) const;
};

// Compact bytes, and back. Deterministic: the same grid gives the same bytes.
std::string encodeCanopy(const Canopy& canopy);
Canopy decodeCanopy(const std::string& bytes, const Bounds& bounds);

// 1 m canopy heights accumulated into a tile's cells, then classified.
class CanopyGrid {
public:
    explicit CanopyGrid(const Bounds& bounds);
    void add(int col, int row, uint8_t metres);
    Canopy finish() const;
    const Bounds& bounds() const { return bounds_; }

private:
    Bounds bounds_;
    struct Cell { uint32_t pixels = 0, tall = 0; uint8_t top = 0; };
    std::vector<Cell> cells_;
};

// Downloads the band of the source covering `tile`'s row and converts every
// tile of that band the downloaded rows cover entirely (a band spans the
// source file's 78 km, so one download serves about a hundred tiles).
// Throws SourceUnavailable when the source cannot be reached.
std::vector<std::pair<Tile, Canopy>> fetchCanopyBand(const Tile& tile);

// The store: `<root>/cache/world/canopy/<lat>_<lon>.r1c`, one file per square
// degree, records appended as tiles are converted. Thread-safe.
std::optional<Canopy> storedCanopy(const std::string& root, const Tile& tile);
void storeCanopy(const std::string& root, const Tile& tile, const Canopy& canopy);
std::string canopyRegionPath(const std::string& root, const Tile& tile);

}  // namespace r1
