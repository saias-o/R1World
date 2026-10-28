// Console-only and offline: any attempted request fails before the network call.
#include "world/menu_tile_cache.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using r1::world::MenuTileCache;

int main() {
    const auto root = fs::temp_directory_path() /
        ("r1world-map-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); }
    } cleanup{root};
    auto forbidden = [](const r1::MapTile&) -> std::string {
        throw std::runtime_error("test attempted a network request");
    };
    const MenuTileCache::Source source{"osm", ".png", forbidden};
    const r1::MapTile cached{3, 2, 1}, absent{3, 2, 2}, missing{3, 2, 3}, shortFile{3, 2, 4};
    {
        MenuTileCache cache(root, source);
        assert(cache.path(cached) == root / "cache/map-tiles/osm/3/2/1.png");
        fs::create_directories(cache.path(cached).parent_path());
        std::ofstream(cache.path(cached), std::ios::binary) << "\x89PNG\r\n\x1a\nfixture";
        std::ofstream(cache.marker(absent)).close();
        std::ofstream(cache.path(shortFile)) << "short";
        assert(cache.ready(cached));
        assert(!cache.ready(missing) && !cache.ready(shortFile));
        assert(cache.missingForGood(absent) && !cache.missingForGood(missing));
        cache.setVisible({cached, absent, missing, shortFile});
        assert(cache.missing() == 2);
        cache.setVisible({cached, absent});
        assert(cache.missing() == 0 && !cache.pump() && !cache.unavailable());
    }
    // Both positive and negative disk entries survive a new session.
    MenuTileCache reopened(root, source);
    reopened.setVisible({cached, absent});
    assert(reopened.ready(cached) && reopened.missingForGood(absent));
    assert(reopened.missing() == 0 && !reopened.pump());
    MenuTileCache imagery(root, {"imagery", ".jpg", forbidden});
    assert(imagery.path(cached) == root / "cache/map-tiles/imagery/3/2/1.jpg");
    assert(!imagery.ready(cached) && !imagery.missingForGood(absent));
    std::cout << "PASS map cache: disk reuse, persistent absence, missing count and source isolation\n";
}
