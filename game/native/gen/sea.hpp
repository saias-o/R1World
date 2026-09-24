// Ships at sea, and the local sky.
//
// Ships: predicted offline from the shipped prior (six years of real AIS
// positions on a 0.1-degree grid, `assets/world/shipping/prior.bin`), moved by
// the season, the hour and the sea state, and realigned whenever the network
// answers: weather from Open-Meteo, and with an aisstream.io key the live
// positions around the player, which teach each cell what it gets wrong.
// Everything is approximate by design and says where its numbers came from.
//
// Sky: the local time zone and the current weather, cached per ~10 km cell.
// A stale forecast is never presented as current weather.
//
// Both run on their own threads and never make the frame wait.
#pragma once

#include "common.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>

namespace r1 {

class SeaService {
public:
    SeaService(std::string gameRoot, std::function<void(const std::string&)> log);
    ~SeaService();  // returns at once; a sync in flight finishes alone
    SeaService(const SeaService&) = delete;
    SeaService& operator=(const SeaService&) = delete;

    // Where the player is, and the game's instant (unix seconds).
    void ask(double lon, double lat, double gameUnix);
    // The ships around the last position asked, and their hulls; null until
    // the first plan. A new plan is a new pointer.
    std::shared_ptr<const nlohmann::json> latest() const;
    // What the sea last said about itself (offline, synced, no prior...).
    std::string lastLine() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

class ConditionsService {
public:
    ConditionsService(std::string gameRoot, std::function<void(const std::string&)> log);
    ~ConditionsService();
    ConditionsService(const ConditionsService&) = delete;
    ConditionsService& operator=(const ConditionsService&) = delete;

    void ask(double lon, double lat);
    // {timezone, utcOffsetSeconds, timeSource, weather|null, weatherSource};
    // null until the first answer. A new answer is a new pointer.
    std::shared_ptr<const nlohmann::json> latest() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace r1
