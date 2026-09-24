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
        int threads = 0;  // 0: the machine's cores less two, kept for the frame
        std::function<void(ServedTile&)> prepare;
        std::function<void(const std::string&)> log;
        size_t keep = 36;  // cooked tiles kept in memory
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
    };
    Status status() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace r1
