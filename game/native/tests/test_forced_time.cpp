#include "check.hpp"
#include "forced_time.hpp"

using namespace r1;

TEST(ForcedTime, what_a_player_types_is_read_or_refused) {
    CHECK(parseClockTime("14:30") == 14 * 60 + 30);
    CHECK(parseClockTime(" 7:05 ") == 7 * 60 + 5);
    CHECK(parseClockTime("14h30") == 14 * 60 + 30);
    CHECK(parseClockTime("14H") == 14 * 60);
    CHECK(parseClockTime("14") == 14 * 60);
    CHECK(parseClockTime("00:00") == 0);
    CHECK(parseClockTime("23:59") == 23 * 60 + 59);
    CHECK(!parseClockTime(""));
    CHECK(!parseClockTime("24:00"));
    CHECK(!parseClockTime("12:60"));
    CHECK(!parseClockTime("12:5"));
    CHECK(!parseClockTime("12:"));
    CHECK(!parseClockTime("midi"));
    CHECK(!parseClockTime("123"));
    CHECK(!parseClockTime("12:30:00"));
    CHECK(formatClockTime(14 * 60 + 5) == "14:05");
    CHECK(formatClockTime(0) == "00:00");
}

TEST(ForcedTime, the_instant_reads_the_hour_on_the_local_clock) {
    // 2026-10-07 22:40 UTC: 00:40 the next day in Paris (UTC+2), 07:40 the
    // next day in Tokyo, 18:40 the same day in New York (UTC-4).
    const double now = 1791412800.0;
    for (const int offset : {7200, 32400, -14400, 0, 20700}) {
        const double at = forcedInstant(now, offset, 14 * 60);
        const double local = std::fmod(at + offset, 86400.0);
        NEAR(local, 14 * 3600.0, 1e-6);
        // The same local day as now: never a day away from the real date.
        CHECK(std::floor((at + offset) / 86400.0) == std::floor((now + offset) / 86400.0));
    }
}
