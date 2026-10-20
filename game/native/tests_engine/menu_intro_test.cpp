#include "menu_intro.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

int main() {
    using r1::MenuIntro;
    const auto first = MenuIntro::subtitleAt(0.0);
    const auto second = MenuIntro::subtitleAt(2.0);
    const auto third = MenuIntro::subtitleAt(4.5);
    assert(first == "Welcome to Earth.");
    assert(second == "The true beauty of life is measured by the vastness");
    assert(third == "of all that is left to discover, learn, and explore.");
    assert(MenuIntro::subtitleAt(-0.001).empty());
    assert(MenuIntro::subtitleAt(std::nextafter(2.0, 0.0)) == first);
    assert(MenuIntro::subtitleAt(std::nextafter(4.5, 0.0)) == second);
    assert(MenuIntro::subtitleAt(std::nextafter(8.0, 0.0)) == third);
    assert(MenuIntro::subtitleAt(8.0).empty());
    assert(MenuIntro::subtitleAt(60.0).empty());
    assert(MenuIntro::subtitleAt(std::numeric_limits<double>::quiet_NaN()).empty());

    MenuIntro intro;
    const auto start = MenuIntro::Clock::time_point{};
    const auto later = [&](int milliseconds) {return start + std::chrono::milliseconds(milliseconds);};
    assert(intro.subtitle(start).empty());
    intro.start(start);
    assert(intro.subtitle(start) == first);
    assert(intro.subtitle(later(2200)) == second);
    // Returning to the menu during or after the voice never resets its clock.
    intro.start(later(3000));
    assert(intro.subtitle(later(4500)) == third);
    intro.start(later(12000));
    assert(intro.subtitle(later(12000)).empty());
    MenuIntro restarted;
    restarted.start(later(12000));
    assert(restarted.subtitle(later(12000)) == first);
    std::cout << "PASS menu intro: cue boundaries, elapsed time, no replay and restart\n";
}
