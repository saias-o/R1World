// Summits a survey measured, and the relief raised to them.
//
// Every elevation source smooths what it measured: a 30 or 90 m model keeps a
// broad summit and loses a narrow one. The Sugarloaf is 396 m; the Terrain
// Tiles say 298, Copernicus at 30 m 329. OSM's `natural=peak` nodes carry the
// surveyed height (`ele`) where the summit actually is. What is measured is
// never overwritten by what is inferred (PLAN §3 I5): where the relief stands
// below a surveyed summit, the summit wins, and the manifest says by how much.
//
// The raise reads nothing but the vertex it moves, the peak, and the distance
// between them, so two tiles that share an edge raise it identically.
#pragma once

#include "osm.hpp"

#include <optional>

namespace r1 {

struct Peak {
    int64_t id = 0;
    double lon = 0, lat = 0, ele = 0;
    std::string name;
};

// The surveyed summits come in the neighbourhood's own Overpass answer
// (sources.hpp, kOsmPeaksVersion). A generator before it asked them by square
// degree, `<root>/cache/world/peaks/<lat>_<lon>.json` (south-west corner,
// floored): those lists are still read for the places they cover.
struct PeakCell {
    int lat = 0, lon = 0;
    bool operator<(const PeakCell& o) const { return lat < o.lat || (lat == o.lat && lon < o.lon); }
    bool operator==(const PeakCell& o) const { return lat == o.lat && lon == o.lon; }
    std::string file() const { return std::to_string(lat) + "_" + std::to_string(lon) + ".json"; }
};

// A summit's raise reaches this far (metres): the cells a tile reads are those
// its box, grown by this, touches.
constexpr double kPeakReach = 150.0;
// Further below a surveyed summit than this, the relief is not smoothed: the
// tag is doubted (a height in feet, a summit misplaced), and refused aloud.
constexpr double kPeakMaxRaise = 250.0;

std::vector<PeakCell> peakCells(const Bounds& bounds);

// `ele` in metres, as OSM writes it: "1901", "1901 m", "1901.5". Anything else
// (feet, ranges, words) is nullopt -- a height in feet is not a height in
// metres, and guessing which would be inferring a measurement.
std::optional<double> parseElevation(const std::string& text);

// The summits among an answer's tagged nodes (`natural=peak|volcano` with an
// `ele`), sorted by id; `refused` counts the ones whose `ele` is not a height
// in metres.
std::vector<Peak> peaksFromFeatures(const std::vector<OsmNode>& features, int* refused = nullptr);
// A square degree's list as an earlier generator kept it on disk.
std::vector<Peak> peaksFromDocument(const nlohmann::json& document);

// The ground raised to the surveyed summits, on the terrain mesh's grid
// (kTerrainMeshSize square). `known` are the grids the peaks' own relief may
// be read on (this tile's and its neighbours'): a summit is doubted when the
// grid under it stands more than kPeakMaxRaise below. `report` (optional)
// receives what each summit did, for the manifest.
ElevationGrid raiseToPeaks(const ElevationGrid& grid, const std::vector<Peak>& peaks,
                           const std::vector<const ElevationGrid*>& known, nlohmann::json* report = nullptr);

}  // namespace r1
