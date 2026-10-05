// The relief past the streamed tiles, sampled for the engine's terrain rings
// (Saida's TerrainRingsNode): what makes a mountain stand on the horizon.
//
// The heights are measured (the Terrain Tiles, at the zoom each ring's spacing
// asks for); what each cell is made of is inferred from them and from the
// Atlas -- the sea where the model is at or below sea level, snow above the
// latitude's snowline, rock where the slope passes 35 degrees, and below the
// treeline a slope past 15 degrees, which nobody ploughs, is forest where the
// climate is wet (temperate, boreal, tropical) and scrub where it is
// mediterranean; the region's ground elsewhere -- every colour a measured
// albedo (CLAUDE.md rule 2).
//
// A slope is judged as it would be measured over the finest ring's cells:
// terrain is self-affine, so a slope read over a longer baseline is gentler
// than the ground it spans, by (spacing / reference)^(1 - Hurst). A cliff band
// a 128 m cell averages away is still rock.
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

class ObservationStore;
constexpr double kInstalledReliefSpacing = 256.0;
constexpr int kFarMinimumZoom = 3;
constexpr int kFarCachedZoomSteps = 3;

// The layers a set of rings is painted with, shared by the threads that
// sample its levels: a swatch gets a slot the first time a cell needs it.
class FarLayers {
public:
    static constexpr int kMax = 16;  // TerrainRingsNode::kMaxLayers
    static constexpr int kWater = 0, kSnow = 1, kRock = 2, kForest = 3, kScrub = 4;
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
    bool installedFallback = false;
    int seaCells = 0, snowCells = 0, rockCells = 0, forestCells = 0, scrubCells = 0;
};

// The Terrain Tiles zoom whose pixel is closest to `spacing` at `lat`,
// from 3 (a continent a pixel row) to 13 (the tiles' own).
int farZoom(double spacing, double lat);

// The altitude trees stop at, from the latitude's snowline: three quarters of
// it -- 2 140 m at 45 degrees (the Alps' 1 900 to 2 200), 950 m at 60
// (Scandinavia's 600 to 1 000), 4 100 m in the tropics (3 500 to 4 500).
double treeline(double lat);

// Self-affine terrain: the Hurst exponent mountain elevation models measure
// (0.7 to 0.8), and the baseline slopes are judged over (the finest ring's).
constexpr double kHurst = 0.75;
constexpr double kSlopeBaseline = 16.0;
// Steeper than this is bare rock (35 degrees); than this, below the treeline,
// is wooded or scrub (15 degrees), as tangents over kSlopeBaseline.
constexpr double kRockSlope = 0.70;
constexpr double kWoodedSlope = 0.27;

// A slope measured over `spacing` metres, as it would read over kSlopeBaseline.
double baselineSlope(double slope, double spacing);

// Samples one ring. `heightAt(lon, lat)` is the measured relief and may throw
// (SourceUnavailable): the ring is then not built. `borderAt`, when given, is
// what the next ring out reads: the ring's outer samples take it, so where two
// rings meet they stand on the same heights and no crack opens between them.
FarLevel sampleFarLevel(const Anchor& anchor, double originX, double originZ, double spacing, int resolution,
                        const std::function<double(double, double)>& heightAt, FarLayers& layers,
                        const std::function<double(double, double)>& borderAt = {});

// Coarse rings read installed relief, then cached images, without network.
// Fine rings prefer their zoom, then coarser cached images, then installed
// relief. The border uses the next ring's policy. Missing packs never mean sea.
FarLevel sampleWorldFarLevel(const ObservationStore& store, const Anchor& anchor,
                             double originX, double originZ, double spacing, int resolution,
                             FarLayers& layers, bool network = true);

}  // namespace r1
