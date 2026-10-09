#include "check.hpp"
#include "weather.hpp"
#include <limits>

using namespace r1;
using nlohmann::json;

TEST(Weather, liquid_rates_sum_rain_and_showers_without_counting_snow) {
    NEAR(liquidRainMmPerHour(json{{"rain", .4}, {"showers", 1.2}, {"precipitation", 3}, {"snowfall", 1}, {"precipitationIntervalSeconds", 3600}}), 1.6, 1e-12);
    CHECK(liquidRainMmPerHour(json{{"rain", 0}, {"showers", 0}, {"precipitation", 3}}) == 0);
    NEAR(liquidRainMmPerHour(json{{"showers", 2}, {"rain", nullptr}, {"precipitationIntervalSeconds", 3600}}), 2, 1e-12);
    CHECK(liquidRainMmPerHour(json{{"rain", -1}, {"showers", -3}, {"precipitation", 7}}) == 0);
}
TEST(Weather, legacy_caches_never_turn_snow_into_liquid_rain) {
    for (int code : {71, 73, 75, 77, 85, 86})
        CHECK(liquidRainMmPerHour(json{{"precipitation", 2}, {"code", code}}) == 0);
    CHECK(liquidRainMmPerHour(json{{"precipitation", 2}, {"snowfall", .1}}) == 0);
    NEAR(liquidRainMmPerHour(json{{"precipitation", 2}, {"code", 61}, {"snowfall", 0}}), 8, 1e-12);
    CHECK(liquidRainMmPerHour(json()) == 0);
    CHECK(liquidRainMmPerHour(json{{"precipitation", nullptr}, {"code", nullptr}}) == 0);
    CHECK(liquidRainMmPerHour(json{{"rain", std::numeric_limits<double>::infinity()}}) == 0);
}
TEST(Weather, current_totals_are_converted_to_hourly_rates_with_a_bounded_interval) {
    NEAR(liquidRainMmPerHour(json{{"rain", .25}, {"showers", .5}, {"precipitationIntervalSeconds", 900}}), 3, 1e-12);
    NEAR(liquidRainMmPerHour(json{{"rain", .25}}), 1, 1e-12);
    NEAR(liquidRainMmPerHour(json{{"rain", .25}, {"precipitationIntervalSeconds", 3600}}), .25, 1e-12);
    for (double interval : {0.0, -900.0, 1.0, 86401.0, std::numeric_limits<double>::infinity()})
        CHECK(liquidRainMmPerHour(json{{"rain", 1}, {"precipitationIntervalSeconds", interval}}) == 0);
    CHECK(liquidRainMmPerHour(json{{"rain", 1}, {"precipitationIntervalSeconds", nullptr}}) == 0);
    CHECK(liquidRainMmPerHour(json{{"rain", std::numeric_limits<double>::max()}, {"showers", std::numeric_limits<double>::max()}}) == 0);
}
TEST(Weather, locality_wraps_the_date_line_and_rejects_missing_coordinates) {
    CHECK(weatherLocal(179.97, 12, -179.97, 12.03));
    CHECK(weatherLocal(-180, -89.9, 180, -89.9));
    CHECK(!weatherLocal(2.35, 48.86, 10.18, 36.81));
    CHECK(!weatherLocal(0, 91, 0, 91));
    CHECK(!weatherLocal(std::numeric_limits<double>::quiet_NaN(), 0, 0, 0));
}
TEST(Weather, rain_cloud_mask_is_bounded_and_keeps_the_observed_overcast) {
    NEAR(rainCloudCover(.1, 53, .3), .35, 1e-12);
    NEAR(rainCloudCover(.1, 61, 2), .65, 1e-12);
    for (int code : {80, 81, 82, 95, 96, 99}) NEAR(rainCloudCover(.1, code, 3), .9, 1e-12);
    NEAR(rainCloudCover(1, 61, 2), 1, 1e-12);
    NEAR(rainCloudCover(.1, 75, 0), .1, 1e-12);
    CHECK(stormWeatherCode(95) && !stormWeatherCode(85) && !stormWeatherCode(98));
}
TEST(Weather, wet_surfaces_dry_independently_of_falling_rain_with_real_seconds) {
    RainState one, subdivided;
    one.update(8, 4);
    for (int i = 0; i < 80; ++i) subdivided.update(.1, 4);
    NEAR(one.wetness, 1 - std::exp(-1.0), 1e-12);
    NEAR(subdivided.wetness, one.wetness, 1e-12);
    NEAR(one.intensity, .5, 1e-12);
    const double soaked = one.wetness;
    one.update(120, 0);
    CHECK(one.intensity == 0);
    NEAR(one.wetness, soaked * std::exp(-1.0), 1e-12);
    one.update(1, 5, false);
    CHECK(one.intensity == 0 && one.wetness == 0);
    one.update(1, std::numeric_limits<double>::quiet_NaN());
    CHECK(one.intensity == 0 && one.wetness == 0);
}
