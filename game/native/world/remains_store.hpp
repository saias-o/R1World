#pragma once
#include <array>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

namespace r1::world {
// One immutable record per death. Coalesce repeated changes to a body while
// another record is on disk; the frame never waits for a disk flush.
class RemainsWriter {
public:
    static constexpr size_t kPending = 64;
    RemainsWriter();
    ~RemainsWriter();
    bool submit(const std::filesystem::path& path, nlohmann::json snapshot);
    nlohmann::json read(const std::filesystem::path& path);
    void flush();
    std::string error() const;
private:
    void run();
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::map<std::filesystem::path, std::shared_ptr<const nlohmann::json>> pending_;
    std::filesystem::path writingPath_;
    std::shared_ptr<const nlohmann::json> writing_;
    std::string error_;
    bool stopping_ = false;
    std::thread worker_;
};

// ECEF cells avoid poles/antimeridian special cases. Only metadata grows with
// history; candidate scans and scene residency are local to these 2 km cells.
class RemainsIndex {
public:
    using Cell = std::array<int,3>;
    static Cell cell(double x,double y,double z);
    void add(size_t id,double x,double y,double z);
    std::vector<size_t> nearby(double x,double y,double z) const;
    void clear() { cells_.clear(); }
private:
    std::map<Cell,std::vector<size_t>> cells_;
};
}
