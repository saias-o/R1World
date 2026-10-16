// Cheat codes (native/cheats.hpp): which word is which code, and how the
// invincibility's time reads on the HUD.
#include "check.hpp"

#include "cheats.hpp"

using namespace r1::cheats;

TEST(Cheats, each_word_is_its_code_whatever_the_case_and_the_spaces_around_it) {
    CHECK(parse("buzzoff") == Code::Helicopter);
    CHECK(parse("plane") == Code::Plane);
    CHECK(parse("comet") == Code::SportsCar);
    CHECK(parse("painkiller") == Code::Invincible);
    CHECK(parse("  BuzzOff \n") == Code::Helicopter);
    CHECK(parse("PAINKILLER") == Code::Invincible);
}

TEST(Cheats, a_word_that_is_no_code_is_unknown_never_a_near_match) {
    CHECK(parse("") == Code::Unknown);
    CHECK(parse("   ") == Code::Unknown);
    CHECK(parse("buzz off") == Code::Unknown);
    CHECK(parse("comets") == Code::Unknown);
    CHECK(parse("pain") == Code::Unknown);
    CHECK(std::string(said(Code::Unknown)) == "Code inconnu.");
}

TEST(Cheats, painkiller_lasts_five_minutes_and_its_clock_never_reads_zero_while_it_holds) {
    CHECK(kInvincibleSeconds == 300);
    CHECK(clock(300) == "5:00");
    CHECK(clock(299.2) == "5:00");
    CHECK(clock(61) == "1:01");
    CHECK(clock(9) == "0:09");
    CHECK(clock(.2) == "0:01");
    CHECK(clock(0) == "0:00");
}
