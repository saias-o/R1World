#pragma once

#include "gen/common.hpp"
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace r1 {

// A local performance decision, independent of generator versions and observations.
// Immutable per-tile markers survive recooks, evictions and future game sessions.
// It reads a tile's own geometry only, and never touches its trees: they are
// shared models outside the tile's budget, and cards in their place would
// lower the world's best assets (CLAUDE.md rules 1 and 5).
class DensityPolicy {
public:
    static constexpr size_t kThreshold = 120000;
    static constexpr double kCrowdScale = .25;
    explicit DensityPolicy(const std::filesystem::path& game)
        : folder_(game / "cache" / "density-policy") {}

    bool reduced(const Tile& tile) {
        const auto id = identity(tile);
        if (reduced_.count(id)) return true;
        if (std::filesystem::exists(folder_ / id)) { reduced_.insert(id); return true; }
        return false;
    }
    bool observe(const Tile& tile, size_t vertices) {
        if (vertices <= kThreshold || reduced(tile)) return false;
        std::filesystem::create_directories(folder_);
        const auto id = identity(tile);
        std::ofstream marker(folder_ / id, std::ios::binary);
        marker << "crowd=0.25\ninteriors=blocked\nvertices=" << vertices << '\n';
        marker.flush();
        if (!marker) throw std::runtime_error("Cannot persist density policy for " + tile.key());
        reduced_.insert(id);
        return true;
    }
private:
    static std::string identity(const Tile& tile) {
        return std::to_string(tile.row) + "_" + std::to_string(tile.col);
    }
    std::filesystem::path folder_;
    std::set<std::string> reduced_;
};
} // namespace r1
