#include "check.hpp"
#include "weather.hpp"

#include <array>
#include <cmath>
#include <string>

using namespace r1;

namespace {
WeatherState snowyObservation() {
    WeatherState sample;
    sample.known = true;
    sample.cover = .8; sample.rain = 0; sample.visibility = 6000;
    sample.windSpeed = 14; sample.windFrom = 35;
    sample.snowfall = .7; sample.snowDepth = .12; sample.code = 75;
    return sample;
}
void sameWeather(const WeatherState& actual, const WeatherState& expected) {
    CHECK(actual.known == expected.known && actual.code == expected.code);
    NEAR(actual.cover, expected.cover, 1e-12);
    NEAR(actual.rain, expected.rain, 1e-12);
    NEAR(actual.visibility, expected.visibility, 1e-12);
    NEAR(actual.windSpeed, expected.windSpeed, 1e-12);
    NEAR(actual.windFrom, expected.windFrom, 1e-12);
    NEAR(actual.snowfall, expected.snowfall, 1e-12);
    NEAR(actual.snowDepth, expected.snowDepth, 1e-12);
}
}

TEST(ForcedWeather, saved_keys_round_trip_and_unknown_values_are_refused) {
    for (const auto mode : {WeatherOverride::Real, WeatherOverride::Clear, WeatherOverride::LightRain, WeatherOverride::Storm})
        CHECK(parseWeatherOverride(weatherOverrideKey(mode)) == mode);
    CHECK(parseWeatherOverride("real") == WeatherOverride::Real);
    CHECK(parseWeatherOverride("clear") == WeatherOverride::Clear);
    CHECK(parseWeatherOverride("light-rain") == WeatherOverride::LightRain);
    CHECK(parseWeatherOverride("storm") == WeatherOverride::Storm);
    for (const std::string key : {"", "rain", "light_rain", "Clear", "STORM", " storm", "storm ", "real\n", "snow", "null"})
        CHECK(!parseWeatherOverride(key));
}

TEST(ForcedWeather, forced_presets_are_finite_and_never_carry_observed_snow) {
    CHECK(!weatherPreset(WeatherOverride::Real).known);
    for (const auto mode : {WeatherOverride::Clear, WeatherOverride::LightRain, WeatherOverride::Storm}) {
        const auto sample = weatherPreset(mode);
        CHECK(sample.known);
        CHECK(std::isfinite(sample.cover) && sample.cover >= 0 && sample.cover <= 1);
        CHECK(std::isfinite(sample.rain) && sample.rain >= 0);
        CHECK(std::isfinite(sample.visibility) && sample.visibility >= 0);
        CHECK(std::isfinite(sample.windSpeed) && sample.windSpeed >= 0);
        CHECK(std::isfinite(sample.windFrom) && sample.windFrom >= 0 && sample.windFrom < 360);
        CHECK(sample.snowfall == 0 && sample.snowDepth == 0 && !snowWeatherCode(sample.code));
    }
    const auto clear = weatherPreset(WeatherOverride::Clear);
    CHECK(clear.cover == 0 && clear.rain == 0 && clear.windSpeed == 0 && clear.code == 0);
    const auto light = weatherPreset(WeatherOverride::LightRain), storm = weatherPreset(WeatherOverride::Storm);
    CHECK(light.rain > .5 && light.rain <= 2 && !stormWeatherCode(light.code));
    CHECK(storm.rain >= 8 && storm.rain > light.rain && stormWeatherCode(storm.code));
    CHECK(storm.cover == 1 && storm.visibility > 0 && storm.visibility < light.visibility);
    CHECK(storm.windSpeed > light.windSpeed);
}

TEST(ForcedWeather, forced_weather_works_without_forecast_after_a_teleport) {
    const WeatherState missing;
    const auto clear = resolveWeather(missing, false, WeatherOverride::Clear);
    CHECK(clear.known && clear.rain == 0 && clear.cover == 0);
    const auto light = resolveWeather(missing, false, WeatherOverride::LightRain);
    CHECK(light.known && light.rain > .5 && light.rain <= 2);
    const auto storm = resolveWeather(missing, false, WeatherOverride::Storm);
    CHECK(storm.known && storm.rain >= 8 && stormWeatherCode(storm.code));
    RainState rain;
    rain.update(8, storm.rain, storm.known);
    CHECK(rain.intensity == 1);
    NEAR(rain.wetness, 1 - std::exp(-1.), 1e-12);
    sameWeather(resolveWeather(missing, false, WeatherOverride::Real), WeatherState{});
}

TEST(ForcedWeather, forced_choice_ignores_locality_and_clears_snow_without_destroying_observations) {
    const auto observed = snowyObservation();
    for (const auto mode : {WeatherOverride::Clear, WeatherOverride::LightRain, WeatherOverride::Storm}) {
        const auto local = resolveWeather(observed, true, mode);
        const auto distant = resolveWeather(observed, false, mode);
        sameWeather(local, distant);
        CHECK(local.known && local.snowfall == 0 && local.snowDepth == 0);
    }
    sameWeather(observed, snowyObservation());
    sameWeather(resolveWeather(observed, true, WeatherOverride::Real), observed);
}

TEST(ForcedWeather, real_weather_accepts_only_the_current_places_observation) {
    const auto observed = snowyObservation();
    sameWeather(resolveWeather(observed, true, WeatherOverride::Real), observed);
    sameWeather(resolveWeather(observed, false, WeatherOverride::Real), WeatherState{});
    CHECK(!resolveWeather(WeatherState{}, true, WeatherOverride::Real).known);
}

TEST(ForcedWeather, inspection_atmosphere_has_priority_over_every_player_choice) {
    const auto observed = snowyObservation();
    for (const auto mode : {WeatherOverride::Real, WeatherOverride::Clear, WeatherOverride::LightRain, WeatherOverride::Storm}) {
        for (bool local : {false, true}) {
            const auto sample = resolveWeather(observed, local, mode, std::array<double, 3>{.2, .75, 10000});
            CHECK(sample.known && sample.code == 61);
            NEAR(sample.cover, .2, 1e-12);
            NEAR(sample.rain, .75, 1e-12); // --weather is already mm/h, without interval conversion.
            NEAR(sample.visibility, 10000, 1e-12);
            CHECK(sample.snowfall == 0 && sample.snowDepth == 0 && sample.windSpeed == 0);
        }
        const auto clearCapture = resolveWeather(observed, true, mode, std::array<double, 3>{0, 0, 0});
        CHECK(clearCapture.known && clearCapture.code == 0 && clearCapture.cover == 0 && clearCapture.rain == 0);
    }
}
