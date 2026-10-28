#pragma once
#include "gen/map_tiles.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <unordered_set>
#include <vector>

namespace r1::world {
namespace fs = std::filesystem;

// Interactive map tiles are requested only when their pixels are on screen.
// A completed PNG is cached indefinitely, satisfying the tile server's
// minimum seven-day cache rule without conditional re-downloads.
// One remote tile layer of the menu globe, cached on disk for good: a place
// already looked at never touches the network again.
class MenuTileCache {

public:
    struct Source {
        std::string name;      // cache folder, and what the log calls it
        std::string extension; // ".png" / ".jpg", checked by signature
        std::function<std::string(const r1::MapTile&)> url;
        size_t parallel=1;     // the provider's connection policy
    };
private:
    // `refused`: the server answered without an image for that tile, which no
    // retry will change; anything else is the network, retried after a pause.
    struct Result { bool ready=false,refused=false; std::string error; };
    Source source;
    fs::path root;
    std::vector<r1::MapTile> visibleTiles;
    std::vector<std::pair<std::string,std::future<Result>>> flights;
    // `unread`: looked for on disk and not there. Only this cache's own
    // download puts a tile there, and it says so (pump), so the disk is not
    // asked again: a zoom sweeping levels of tiles not yet fetched asked it
    // for each of them every frame, a second a frame on a synced folder.
    std::unordered_set<std::string> present,absent,checkedAbsent,unread;
    std::chrono::steady_clock::time_point retryAfter{};
    std::string lastError;
    static std::string key(const r1::MapTile& t);
    // Either format, whatever the extension: EOX answers a plain area (open
    // sea, no data) with a tiny PNG. The decoder reads the signature.
    static bool validImage(const std::string& body);
public:
    MenuTileCache(fs::path project,Source s);
    fs::path path(const r1::MapTile& tile) const;
    // Asked every frame for every visible tile and its ancestors: the disk is
    // consulted once per tile, then remembered.
    bool ready(const r1::MapTile& tile);
    // A tile the server has no image for: left out of what a view waits for,
    // and remembered on disk (an empty "<y>.none" beside where the image would
    // be) so no later session asks again.
    fs::path marker(const r1::MapTile& tile) const;
    bool missingForGood(const r1::MapTile& tile);
    void setVisible(std::vector<r1::MapTile> tiles);
    size_t missing();
    bool unavailable() const;
    bool pump();
    ~MenuTileCache();
};

} // namespace r1::world
