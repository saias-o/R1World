// The relief past the streamed tiles, sampled for the engine's terrain rings
// (Saida's TerrainRingsNode): what makes a mountain stand on the horizon.
//
// The heights are measured (the Terrain Tiles, at the zoom each ring's spacing
// asks for); what each cell is made of is inferred from them and from the
// Atlas -- the sea where the model is at or below sea level, snow above the
// latitude's snowline, rock where the slope passes 35 degrees, the region's
// ground elsewhere -- every colour a measured albedo (CLAUDE.md rule 2).
//
// The Earth's curvature is content, not a correction (PLAN §3 I2): each sample
// is the true surface point under its grid position in the rings' tangent
// frame, so the far side of a valley drops below the horizon as it does.
#pragma once

#include "common.hpp"
#include "palette.hpp"

#include <functional>
#include <map>
#include <mutex>

namespace r1 {

// The layers a set of rings is painted with, shared by the threads that
// sample its levels: a swatch gets a slot the first time a cell needs it.
class FarLayers {
public:
    static constexpr int kMax = 16;  // TerrainRingsNode::kMaxLayers
    static constexpr int kWater = 0, kSnow = 1, kRock = 2;
    FarLayers();
    // The slot of a region's ground; the last slot when they are all taken.
    int ground(const Swatch& swatch);
    std::vector<Swatch> swatches() const;
    uint64_t revision() const;

private:
    mutable std::mutex lock_;
    std::vector<Swatch> swatches_;
    std::map<std::string, int> byName_;
    uint64_t revision_ = 1;
};

// One ring's samples, laid out as the engine wants them: `heights` are
// (resolution + 1)^2 row by row along z, `layers` one per cell. Engine frame:
// x east, y up, z south, metres from the rings' anchor.
struct FarLevel {
    double originX = 0, originZ = 0, spacing = 0;
    std::vector<float> heights;
    std::vector<uint8_t> layers;
    int zoom = 0;
    int seaCells = 0, snowCells = 0, rockCells = 0;
};

// The Terrain Tiles zoom whose pixel is closest to `spacing` at `lat`,
// from 3 (a continent a pixel row) to 13 (the tiles' own).
int farZoom(double spacing, double lat);

// Samples one ring. `heightAt(lon, lat)` is the measured relief and may throw
// (SourceUnavailable): the ring is then not built. `borderAt`, when given, is
// what the next ring out reads: the ring's outer samples take it, so where two
// rings meet they stand on the same heights and no crack opens between them.
FarLevel sampleFarLevel(const Anchor& anchor, double originX, double originZ, double spacing, int resolution,
                        const std::function<double(double, double)>& heightAt, FarLayers& layers,
                        const std::function<double(double, double)>& borderAt = {});

}  // namespace r1
