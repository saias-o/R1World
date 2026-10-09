// Forecast interpretation for the game. Open-Meteo precipitation includes
// snow; rain plus showers is the observed liquid portion, in millimetres
// accumulated during current.interval (normally the previous 15 minutes).
#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <array>
#include <optional>
#include <string_view>

namespace r1 {

enum class WeatherOverride { Real, Clear, LightRain, Storm };

inline std::optional<WeatherOverride> parseWeatherOverride(std::string_view key) {
    if (key == "real") return WeatherOverride::Real;
    if (key == "clear") return WeatherOverride::Clear;
    if (key == "light-rain") return WeatherOverride::LightRain;
    if (key == "storm") return WeatherOverride::Storm;
    return std::nullopt;
}
inline const char* weatherOverrideKey(WeatherOverride mode) {
    switch (mode) {
    case WeatherOverride::Clear: return "clear";
    case WeatherOverride::LightRain: return "light-rain";
    case WeatherOverride::Storm: return "storm";
    default: return "real";
    }
}
inline const char* weatherOverrideLabel(WeatherOverride mode) {
    switch (mode) {
    case WeatherOverride::Clear: return "Dégagé";
    case WeatherOverride::LightRain: return "Petite pluie";
    case WeatherOverride::Storm: return "Forte pluie / orage";
    default: return "Météo réelle";
    }
}

struct WeatherState {
    double cover = 0, rain = 0, visibility = 0, windSpeed = 0, windFrom = 0, snowfall = 0, snowDepth = 0;
    int code = -1;
    bool known = false;
};

inline WeatherState weatherPreset(WeatherOverride mode) {
    WeatherState sample;
    if (mode == WeatherOverride::Real) return sample;
    sample.known = true;
    sample.code = 0;
    if (mode == WeatherOverride::LightRain) {
        sample.cover = .60; sample.rain = 1.; sample.visibility = 16000.;
        sample.windSpeed = 2.; sample.windFrom = 240.; sample.code = 61;
    } else if (mode == WeatherOverride::Storm) {
        sample.cover = 1.; sample.rain = 12.; sample.visibility = 3500.;
        sample.windSpeed = 8.; sample.windFrom = 240.; sample.code = 95;
    }
    return sample;
}

// Captures choose their own atmosphere. A player override remains global even
// without a forecast; real observations are accepted only at their own place.
inline WeatherState resolveWeather(const WeatherState& observed, bool local,
                                   WeatherOverride mode,
                                   std::optional<std::array<double, 3>> inspection = std::nullopt) {
    if (inspection) {
        WeatherState sample;
        sample.known = true; sample.cover = (*inspection)[0];
        sample.rain = (*inspection)[1]; sample.visibility = (*inspection)[2];
        sample.code = sample.rain > 0. ? 61 : 0;
        return sample;
    }
    if (mode != WeatherOverride::Real) return weatherPreset(mode);
    return local ? observed : WeatherState{};
}

inline bool snowWeatherCode(int code) {
    return code == 71 || code == 73 || code == 75 || code == 77 || code == 85 || code == 86;
}
inline bool stormWeatherCode(int code) { return code == 95 || code == 96 || code == 99; }

inline double nonnegativeWeatherNumber(const nlohmann::json& weather, const char* key) {
    const auto found = weather.find(key);
    if (found == weather.end() || !found->is_number()) return 0.0;
    const double value = found->get<double>();
    return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

inline double liquidRainMmPerHour(const nlohmann::json& weather) {
    if (!weather.is_object()) return 0.0;
    const auto numeric = [&](const char* key) {
        const auto found = weather.find(key);
        return found != weather.end() && found->is_number();
    };
    // An explicit zero is authoritative. Old caches without these two fields
    // use the total only when it cannot be snow; no snow-water ratio is guessed.
    double interval = 900.0; // Older normalized caches also stored current-period totals.
    const auto period = weather.find("precipitationIntervalSeconds");
    if (period != weather.end()) {
        if (!period->is_number()) return 0.0;
        interval = period->get<double>();
        if (!std::isfinite(interval) || interval < 60.0 || interval > 86400.0) return 0.0;
    }
    const auto hourly = [&](double amount) {
        const double rate = amount * (3600.0 / interval);
        return std::isfinite(rate) ? rate : 0.0;
    };
    if (numeric("rain") || numeric("showers"))
        return hourly(nonnegativeWeatherNumber(weather, "rain") + nonnegativeWeatherNumber(weather, "showers"));
    int code = -1;
    if (numeric("code")) {
        const double raw = weather.at("code").get<double>();
        if (std::isfinite(raw) && raw >= 0 && raw <= 99) code = int(raw);
    }
    if (snowWeatherCode(code) || nonnegativeWeatherNumber(weather, "snowfall") > 0.0) return 0.0;
    return hourly(nonnegativeWeatherNumber(weather, "precipitation"));
}

inline bool weatherLocal(double observedLon, double observedLat, double lon, double lat, double radius = .12) {
    if (!std::isfinite(observedLon) || !std::isfinite(observedLat) || !std::isfinite(lon) ||
        !std::isfinite(lat) || !std::isfinite(radius) || radius <= 0 ||
        std::abs(observedLat) > 90 || std::abs(lat) > 90) return false;
    return std::abs(std::remainder(observedLon - lon, 360.0)) < radius && std::abs(observedLat - lat) < radius;
}

inline double rainCloudCover(double cover, int code, double liquidRain) {
    cover = std::isfinite(cover) ? std::clamp(cover, 0.0, 1.0) : 0.0;
    if (!std::isfinite(liquidRain) || liquidRain <= 0.0) return cover;
    const double minimum = stormWeatherCode(code) || (code >= 80 && code <= 82) ? .9
        : code >= 51 && code <= 57 ? .35 : .65;
    return std::max(cover, minimum);
}

struct RainState {
    double intensity = 0.0, wetness = 0.0;
    void reset() { intensity = wetness = 0.0; }
    // active means local weather in an active scene, rather than whether the
    // camera is sheltered. A roof hides drops without drying outdoor surfaces.
    void update(double dt, double liquidRain, bool active = true) {
        if (!active) { reset(); return; }
        liquidRain = std::isfinite(liquidRain) ? std::max(0.0, liquidRain) : 0.0;
        intensity = std::clamp(liquidRain / 8.0, 0.0, 1.0);
        if (!std::isfinite(dt) || dt <= 0.0) return;
        const double target = liquidRain > 0.0 ? 1.0 : 0.0;
        wetness += (target - wetness) * -std::expm1(-dt / (target > wetness ? 8.0 : 120.0));
        wetness = std::clamp(wetness, 0.0, 1.0);
    }
};

}  // namespace r1
