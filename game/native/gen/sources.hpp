// The raw observations a tile is cooked from, and where they are kept.
//
// The cache is a promise (CLAUDE.md §7): a place already visited never touches
// the network again. Only observations are kept -- the Overpass answers and
// the elevation grids -- never geometry, which the game cooks again on every
// visit (PLAN §1). The layout is the one the Python worker wrote, so every
// place its players visited stays offline-ready:
//
//   cache/world/v<N>_<row>_<col>/osm.json              a tile's own Overpass answer
//   cache/world/v<N>_<row>_<col>/ground-elevation.json its terrain (IGN or GLO-90)
//   cache/world/sources/<sha256[:20]>.json             a neighbourhood's shared answer
//   cache/world/seaice/ascat_<row>_<col>.json          the sea ice around a block of tiles
//   cache/world/peaks/<lat>_<lon>.json                 the surveyed summits of a square degree
//   cache/world/terrain/<zoom>/<x>/<y>.png             the Terrain Tiles images the ground was read from
//
// Any version's folder answers; new observations are written under kVersion.
#pragma once

#include "osm.hpp"
#include "peaks.hpp"
#include "terrain.hpp"

#include <functional>
#include <optional>

namespace r1 {

class SourceUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The Overpass question, as a number: an answer to an older one is used only
// when the network cannot give the current one, and the manifest says so.
constexpr int kOsmQueryVersion = 14; // 14: complete nearby inland-water multipolygons.
// The question that brought the shops, stations and civic uses: an answer to
// it or a later one needs no retail layer of its own.
constexpr int kOsmRetailVersion = 13; // civic uses plus settlement nodes; enrich older cached answers cheaply
// The question that brings the surveyed summits (gen/peaks.hpp) in the
// neighbourhood's own answer: a summit is never one Overpass call more.
constexpr int kOsmPeaksVersion = 11;
// .retail.json: 2 added fuel stations, 3 adds civic/office/craft uses.
constexpr int kRetailLayerVersion = 4; // 4 adds settlement nodes without re-downloading buildings
// The question that brought the aero layer: an answer to it or a later one
// needs no layer of its own.
constexpr int kOsmAeroVersion = 6;
// The oldest answer a tile is cooked from without asking Overpass again.
// What version 6 added -- aeroways, military areas, the runways around -- is
// fetched for an older answer as a layer of its own (`fetchAero`), a few
// kilobytes beside its ten megabytes: re-asking the whole neighbourhood for
// it made every place already visited wait on Overpass at Go.
// What version 7 added -- the country the neighbourhood is in, traffic
// calming and surveyed signs, for the predictive model (gen/predict.hpp) --
// is not worth that wait: an older answer is cooked without it, and the
// manifest says so (`predicted.calmingNodesQueried`).
constexpr int kOsmBaseVersion = 5;

class ObservationStore {
public:
    explicit ObservationStore(std::string gameRoot, std::function<void(const std::string&)> log = {})
        : root_(std::move(gameRoot)), log_(std::move(log)) {}

    // The shared query of a group of tiles: its box and its file, or nothing
    // when the group spans too much to be one query (15 hundredths of a degree).
    struct Shared { Bounds region; std::string path; };
    std::optional<Shared> shared(const std::vector<Tile>& group) const;

    // Can this tile be cooked without contacting any remote source?
    bool cached(const Tile& tile, const std::optional<Shared>& shared) const;

    // From disk only; nullopt when absent. Of every answer on disk that
    // covers the tile, one to the current question wins, then the widest
    // (a neighbourhood query sees the ways that cross the tile's edges).
    // `stale` is set when only an answer older than kOsmBaseVersion was there.
    std::optional<nlohmann::json> osm(const Tile& tile, const std::optional<Shared>& shared, bool* stale = nullptr) const;
    // Which file `osm` would read.
    std::optional<std::string> osmPath(const Tile& tile, const std::optional<Shared>& shared, bool* stale = nullptr) const;
    // The relief of the eight tiles around, those on disk: never a download.
    // A bridge reaching past a tile is solved on it (gen/bridges.hpp).
    std::vector<ElevationGrid> groundAround(const Tile& tile) const;
    // The Overpass question about a neighbourhood's box, as it is sent.
    static std::string osmQuery(const Bounds& b);
    // The box the answer at `path` was asked about: where its observations
    // stop, and the world does not (gen/predict.cpp). Nothing if unknown.
    std::optional<Bounds> regionOf(const Tile& tile, const std::optional<Shared>& shared, const std::string& path) const;
    std::optional<std::pair<ElevationGrid, std::string>> ground(const Tile& tile) const;

    // From the network, written to disk before they return. Throw
    // SourceUnavailable when no source answers.
    nlohmann::json fetchOsm(const Bounds& bounds, const std::string& path) const;
    std::pair<ElevationGrid, std::string> fetchGround(const Tile& tile) const;
    // The ground in a fifth of a second: Copernicus only, one attempt, four
    // seconds at most, so a first visit is playable before IGN or Overpass.
    std::pair<ElevationGrid, std::string> quickGround(const Tile& tile) const;

    // The aero layer of a tile cooked from `mainPath`. `needed` is false when
    // the main answer already carries it (version 6 on); otherwise the result
    // is the layer on disk, if any neighbourhood's has been fetched.
    std::optional<std::string> aeroPath(const Tile& tile, const std::optional<Shared>& shared,
                                        const std::string& mainPath, bool& needed) const;
    // Where this tile's neighbourhood's aero layer is fetched to, and its box.
    Shared aeroTarget(const Tile& tile, const std::optional<Shared>& shared) const;
    nlohmann::json fetchAero(const Bounds& bounds, const std::string& path) const;
    std::optional<std::string> retailPath(const Tile& tile,const std::optional<Shared>& shared,
                                         const std::string& mainPath) const;
    Shared retailTarget(const Tile& tile,const std::optional<Shared>& shared) const;
    nlohmann::json fetchRetail(const Bounds& bounds,const std::string& path) const;
    // The sea ice read around (lon, lat) (gen/seaice.hpp): from disk, or
    // nullopt; from the network, written to disk before it returns.
    std::optional<nlohmann::json> seaIce(double lon, double lat) const;
    nlohmann::json fetchSeaIce(double lon, double lat) const;
    // The installed relief layer's ground over `bounds`, when no source
    // answers (gen/relief); nullopt where it has nothing.
    std::optional<std::pair<ElevationGrid, std::string>> offlineGround(const Bounds& bounds) const;
    // The surveyed summits a tile is raised to (gen/peaks.hpp): those its
    // neighbourhood's answer carries; for an answer older than
    // kOsmPeaksVersion, the square degrees an earlier generator kept on disk.
    // Never a download. `origin` says which answered, for the manifest.
    std::vector<Peak> peaks(const Tile& tile, const OsmData& osm, std::string* origin = nullptr) const;
    // The Terrain Tiles' height at (lon, lat) from the images of one zoom,
    // fetched on first use when `network` allows and kept on disk; throws
    // SourceUnavailable where an image is neither on disk nor fetchable, or
    // holds no data. The function keeps the images it read: one per caller
    // and thread, for the samples of one ring.
    std::function<double(double, double)> terrainSampler(int zoom, bool network) const;
    static std::string aeroSibling(const std::string& mainPath);
    // The question an answer on disk replied to (1 when it does not say).
    static int queryVersion(const std::string& path);
    // A retail layer that already carries the fuel stations.
    static bool retailCurrent(const std::string& layer);

    std::string tileFolder(const Tile& tile) const;  // where new observations go
    const std::string& root() const { return root_; }

private:
    std::string root_;
    std::function<void(const std::string&)> log_;
    std::optional<std::string> find(const Tile& tile, const char* name) const;
    // Every answer on disk that covers this tile: its own, and every
    // neighbourhood query whatever position the player asked it from.
    // With a `sibling` suffix (".retail.json"), the layers beside every
    // query covering the tile instead, whether or not that query's own answer
    // is on disk: a layer is written beside the player's group even when an
    // older answer already covers its tiles.
    std::vector<std::string> candidates(const Tile& tile, const std::optional<Shared>& shared,
                                        std::map<std::string, Bounds>* regions = nullptr,
                                        const std::string& sibling = {}) const;
};

// The Terrain Tiles' height at (lon, lat), bilinear between the pixel centres
// of the zoom's 256-pixel Web Mercator images, each read through `rgb(x, y)`
// (256 x 256 x 3 bytes). NaN where a pixel holds no data.
double terrariumHeight(double lon, double lat, int zoom, const std::function<const unsigned char*(int, int)>& rgb);

// A Terrain Tiles image's pixels, RGB, kTerrariumSize square; throws
// SourceUnavailable when the bytes are not such a PNG.
constexpr int kTerrariumSize = 256;
std::vector<unsigned char> terrariumPixels(const std::string& png);
// The same coastal-ringing correction used by runtime samplers (zoom <= 11).
std::vector<unsigned char> terrariumReliefPixels(const std::string& png, int zoom);
// Where the Terrain Tiles images are served: zoom/x/y.png under it.
std::string terrariumUrl(int zoom, int x, int y);

std::string sha256Hex(const std::string& data);
nlohmann::json readJson(const std::string& path);

}  // namespace r1
