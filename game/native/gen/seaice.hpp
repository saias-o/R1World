// Sea ice: where the ocean is frozen, what kind of ice it is, and the pack
// drawn from it.
//
// Where it is frozen and how old the ice is are measured: NOAA's daily ASCAT
// ice classification (open water, first-year, mixed, multi-year) on a 4.28 km
// polar grid, read once per neighbourhood and kept like every observation
// (CLAUDE.md §7). The satellite sees nothing within some 35 km of the pole;
// that hole takes the class of the nearest cell read, and the manifest counts
// the cells so filled. Without a reading, a static climatology says where the
// ice never leaves, and says it is inferred (PLAN §3 I5).
//
// What the pack looks like is synthesised, deterministically (§3 I3): floes,
// leads open or frozen over, pressure ridges and their blocks, snow dunes,
// melt ponds, all one function of a position on a polar plane that has no
// seam at the pole and no seam between tiles. The season comes from the
// observation's date, never from a clock.
#pragma once

#include "mesh.hpp"
#include "osm.hpp"

#include <memory>

namespace r1 {

// The classes of the ASCAT product, as its files carry them.
enum : uint8_t { kIceUnknown = 0, kIceLand = 1, kIceWater = 2, kIceSeasonal = 3, kIceMixed = 4, kIcePerennial = 5 };

struct SeaIce {
    std::string source, date;
    int dayOfYear = 60;
    bool measured = false, north = true;
    int filled = 0;  // cells without a reading (pole hole, land mask) given the nearest reading
    // The measured window, on the dataset's grid: x east, rows running south.
    double x0 = 0, y0 = 0, step = 0;
    int cols = 0, rows = 0;
    std::vector<uint8_t> classes;

    struct Sample { double concentration = 0, age = 0; };
    // Concentration 0..1 and age 0 (first-year) .. 1 (multi-year) at a point.
    Sample at(double lon, double lat) const;
    // The class read (or filled) at a point, 0 where the window has none.
    int classAt(double lon, double lat) const;
    // Is there ice anywhere in `b`?
    bool any(const Bounds& b) const;
};

// Is (lon, lat) on the ASCAT grid (the Arctic, down to about 49°N)?
bool seaIceGridCovers(double lon, double lat);
// The window a tile's observation is read in: 48 cells square, centred on the
// 16-cell block holding the tile, so every tile of a block shares one file.
struct SeaIceWindow {
    int blockRow = 0, blockCol = 0;
    std::string file() const;  // under cache/world/seaice/
    std::string url() const;
};
SeaIceWindow seaIceWindow(double lon, double lat);
// The ERDDAP answer, reduced to what is kept on disk.
nlohmann::json seaIceDocument(const nlohmann::json& erddap);
SeaIce seaIceFrom(const nlohmann::json& document);
// Where the ice never leaves: north of 80°N, south of 70°S.
SeaIce inferredSeaIce(double lat);

// One point of the pack.
enum class IceKind : uint8_t { Snow, Bare, Young, FrozenPond, Pond, Water };
struct IcePoint {
    double height = 0;       // above the sea, metres
    IceKind kind = IceKind::Snow;
    double ridge = 0;        // what a pressure ridge adds here
    double ridgeHeight = 0;  // that ridge's sail height at this point of its length
    // How much of a pond and of young ice there is here, 0..1: what the
    // surface is tinted with, so a pond's shore is a gradient, not a step.
    double pond = 0, young = 0;
};
struct IceParameters { double concentration = 1, age = 0.5; };
IcePoint iceAt(double lon, double lat, const IceParameters& p, int season);
// The season a date is in, for the pack: 0 winter and spring, 1 melt, 2 freeze-up.
int iceSeason(int dayOfYear, bool north);

// The terrain of a frozen tile, finer than land's: a lead is metres wide.
constexpr int kIceMeshSize = 161;
constexpr size_t kMaxRidgeBlocks = 1500;

struct IceTile {
    std::vector<MeshPart> parts;
    ElevationGrid grid;              // kIceMeshSize square, what the player walks on
    std::vector<std::string> water;  // cells: '2' open water, '0' ice
    bool openWater = false;
    nlohmann::json stats;
};
IceTile buildIceTile(const Tile& tile, const Anchor& anchor, const SeaIce& ice);

// The pack past the streamed tiles, out to 40 km: a disc in the frame of
// (lon, lat) at sea level, curved with the Earth (PLAN §3 I2), snow where the
// observation says ice and water where it says water. It lies below every
// floe and every lead floor of the tiles, so they always win where they are.
constexpr double kFarPackRadius = 40000.0;
constexpr double kFarPackDepth = -0.45;
std::vector<MeshPart> buildFarPack(const SeaIce& ice, double lon, double lat);

}  // namespace r1
