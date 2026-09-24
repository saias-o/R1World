// The raw observations a tile is cooked from, and where they are kept.
//
// The cache is a promise (CLAUDE.md §6): a place already visited never touches
// the network again. Only observations are kept -- the Overpass answers and
// the elevation grids -- never geometry, which the game cooks again on every
// visit (PLAN §1). The layout is the one the Python worker wrote, so every
// place its players visited stays offline-ready:
//
//   cache/world/v<N>_<row>_<col>/osm.json              a tile's own Overpass answer
//   cache/world/v<N>_<row>_<col>/ground-elevation.json its terrain (IGN or GLO-90)
//   cache/world/sources/<sha256[:20]>.json             a neighbourhood's shared answer
//
// Any version's folder answers; new observations are written under kVersion.
#pragma once

#include "osm.hpp"

#include <functional>
#include <optional>

namespace r1 {

class SourceUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The Overpass question, as a number: an answer to an older one is used only
// when the network cannot give the current one, and the manifest says so.
constexpr int kOsmQueryVersion = 5;

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
    // `stale` is set when only an older question's answer was there.
    std::optional<nlohmann::json> osm(const Tile& tile, const std::optional<Shared>& shared, bool* stale = nullptr) const;
    // Which file `osm` would read.
    std::optional<std::string> osmPath(const Tile& tile, const std::optional<Shared>& shared, bool* stale = nullptr) const;
    std::optional<std::pair<ElevationGrid, std::string>> ground(const Tile& tile) const;

    // From the network, written to disk before they return. Throw
    // SourceUnavailable when no source answers.
    nlohmann::json fetchOsm(const Bounds& bounds, const std::string& path) const;
    std::pair<ElevationGrid, std::string> fetchGround(const Tile& tile) const;

    std::string tileFolder(const Tile& tile) const;  // where new observations go
    const std::string& root() const { return root_; }

private:
    std::string root_;
    std::function<void(const std::string&)> log_;
    std::optional<std::string> find(const Tile& tile, const char* name) const;
    // Every answer on disk that covers this tile: its own, and every
    // neighbourhood query whatever position the player asked it from.
    std::vector<std::string> candidates(const Tile& tile, const std::optional<Shared>& shared) const;
};

std::string sha256Hex(const std::string& data);
nlohmann::json readJson(const std::string& path);

}  // namespace r1
