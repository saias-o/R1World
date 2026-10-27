#include "check.hpp"
#include "destination.hpp"

using namespace r1;

TEST(Destination, negative_coordinates_are_read_as_players_type_them) {
    CHECK(parseCoordinate("-73.9857") == -73.9857);
    CHECK(parseCoordinate(" -33.8688 ") == -33.8688);
    CHECK(parseCoordinate("\xE2\x88\x92" "33.8688") == -33.8688);  // −, as the game's messages print it
    CHECK(parseCoordinate("\xE2\x80\x93" "2.18") == -2.18);         // –
    CHECK(parseCoordinate("-2,18") == -2.18);                       // French decimal comma
    CHECK(parseCoordinate("+151.2093") == 151.2093);
    CHECK(parseCoordinate("48.02\xC2\xB0") == 48.02);               // °
    CHECK(parseCoordinate("-0") == 0.0);
    CHECK(parseCoordinate("-.5") == -.5);
    CHECK(parseCoordinate("180") == 180.0);
    CHECK(!parseCoordinate(""));
    CHECK(!parseCoordinate("-"));
    CHECK(!parseCoordinate("--2.18"));
    CHECK(!parseCoordinate("2.18-"));
    CHECK(!parseCoordinate("1e2"));
    CHECK(!parseCoordinate("inf"));
    CHECK(!parseCoordinate("nan"));
    CHECK(!parseCoordinate("0x10"));
    CHECK(!parseCoordinate("48.02, -2.18"));
    CHECK(!parseCoordinate("1.2.3"));
}

TEST(Destination, a_place_west_or_south_takes_the_player_there) {
    const auto newYork = readDestination("40.7484", "-73.9857");
    CHECK(newYork && newYork.lat == 40.7484 && newYork.lon == -73.9857);
    const auto sydney = readDestination("-33.8688", "151.2093");
    CHECK(sydney && sydney.lat == -33.8688 && sydney.lon == 151.2093);
    const auto brittany = readDestination("48,02", "\xE2\x88\x92" "2,18");
    CHECK(brittany && brittany.lat == 48.02 && brittany.lon == -2.18);
    CHECK(readDestination("-90", "-180") && readDestination("90", "180"));
}

TEST(Destination, a_refusal_names_the_field_and_the_reason) {
    const auto south = readDestination("-91", "0");
    CHECK(!south && south.refusal.find("Latitude -91 is out of range") == 0);
    const auto west = readDestination("0", "-180.5");
    CHECK(!west && west.refusal.find("Longitude -180.5 is out of range") == 0);
    const auto doubled = readDestination("--2.18", "48.02");
    CHECK(!doubled && doubled.refusal.find("Latitude \"--2.18\" is not a number") == 0);
    const auto empty = readDestination("48.02", "");
    CHECK(!empty && empty.refusal.find("Longitude \"\" is not a number") == 0);
    // A longitude typed in the latitude field says so.
    const auto swapped = readDestination("-122.27", "37.95");
    CHECK(!swapped && swapped.refusal.find("swapped") != std::string::npos);
    CHECK(readDestination("-91", "120").refusal.find("swapped") == std::string::npos);
}
