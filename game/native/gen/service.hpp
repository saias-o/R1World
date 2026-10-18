// The world service: observations fetched, tiles cooked, on worker threads,
// in the order the game asks for them, inside the game's own process.
//
// The game says which tiles it wants, most urgent first, and which tiles form
// a neighbourhood (one Overpass query serves nine tiles). Workers take the
// most urgent tile nobody is cooking, get its observations from disk or the
// network, cook it and publish it. The game picks cooked tiles up with
// `find`, on its own thread, whenever it has the frame time to mount one.
//
// Nothing here draws or blocks the frame. `prepare` runs on the worker that
// cooked the tile, so whatever the game must do to a tile before the GPU sees
// it (turning doubles into its vertex format) is not paid on the frame either.
#pragma once

#include "cook.hpp"

#include <any>
#include <functional>
#include <memory>

namespace r1 {

struct ServedTile {
    CookedTile cooked;
    std::any prepared;    // what `prepare` made of it
    uint64_t serial = 0;  // unique per cook: an upgraded tile has a new one
};

class WorldService {
public:
    struct Options {
        std::string gameRoot;
        int threads = 0;  // 0: up to two cooks, keeping room for the frame
        std::function<void(ServedTile&)> prepare;
        std::function<void(const std::string&)> log;
        size_t keep = 36;  // hard cache/request limit; priority beyond it waits for the next want()
        size_t tileVertexTarget = kTileVertexBudget;
        // Explicit feature migration: supplement old observations without
        // re-downloading them. Cached retail layers never revalidate.
        bool enrichRetail = false;
        // Fetch and convert the measured canopy (gen/canopy) of tiles that
        // have none stored. Off in tests; the store is read either way.
        bool fetchCanopy = false;
        // Optional source overrides for deterministic streaming tests.
        std::function<void(const Bounds&, const std::string&)> fetchOsm;
        std::function<void(const Bounds&, const std::string&)> fetchAero;
        std::function<std::pair<ElevationGrid, std::string>(const Tile&)> quickGround;
        std::function<std::pair<ElevationGrid, std::string>(const Tile&)> fetchGround;
        // Host headroom supplied by the runtime (no engine dependency here).
        std::function<std::optional<uint64_t>()> availableMemory;
        uint64_t memoryReserve = 512ull * 1024 * 1024; // frame/UI/asset uploads
        uint64_t memoryPerCook = 256ull * 1024 * 1024; // Paris transient working set
    };
    explicit WorldService(Options options);
    // Returns at once: a worker still waiting on the network finishes alone.
    ~WorldService();
    WorldService(const WorldService&) = delete;
    WorldService& operator=(const WorldService&) = delete;

    // The tiles wanted, most urgent first, and the neighbourhoods they form.
    void want(std::vector<Tile> priority, std::vector<std::vector<Tile>> groups);
    // The cooked tile, or null while it is not ready.
    std::shared_ptr<const ServedTile> find(const Tile& tile) const;

    struct Status {
        std::string error;     // the last refusal, empty when none
        bool offline = false;  // the network did not answer lately
        int cooking = 0, fetching = 0, threads = 0;
        // Cooks that ran out of memory, and how many may run at once since:
        // each one halves it (CLAUDE.md rule 3: said, never a crash later).
        int outOfMemory = 0, allowed = 0;
        bool memoryPaused = false;
    };
    Status status() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace r1
