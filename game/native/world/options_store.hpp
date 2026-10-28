#pragma once

#include "weather.hpp"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace r1::world {

// A persistence snapshot, not a second authority on live engine settings.
struct SavedOptions {
    std::optional<int> forcedMinutes;
    r1::WeatherOverride forcedWeather=r1::WeatherOverride::Real;
    bool toasterMode=false;
    int giQuality=0;
    bool fullscreen=false;
};

// Owns the existing file format and diagnostics. Applying settings to the
// window, sky, scene and UI is the session's responsibility.
class OptionsStore {
public:
    enum class Level { Info, Warning, Error };
    using Reporter = std::function<void(Level, const std::string&)>;

    explicit OptionsStore(std::filesystem::path project, Reporter reporter={});
    SavedOptions load(SavedOptions current={}, bool testRun=false) const;
    bool save(const SavedOptions& options, bool testRun=false) const;

private:
    void report(Level level, const std::string& message) const;
    std::filesystem::path path_;
    Reporter reporter_;
};

} // namespace r1::world
