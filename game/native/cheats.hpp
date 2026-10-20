// Cheat codes, as in GTA: T opens a line at the bottom of the screen, a word
// typed there and Enter does one thing. What a code does is the game's
// (native/world.cpp: the vehicle beside the player, the invincibility); which
// word is which code, and for how long, is held here where the tests read it.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace r1::cheats {

enum class Code : uint8_t { Unknown, Helicopter, Plane, SportsCar, Moto, Rocket, Invincible };

struct Entry {
    const char* word;  // what the player types, in lower case
    Code code;
    const char* said;  // what the screen answers
};

// GTA's own words where GTA has one: BUZZOFF and COMET are San Andreas and
// Vice City spawns, PAINKILLER is Vice City's invincibility.
constexpr Entry kCodes[] = {
    {"buzzoff", Code::Helicopter, "Helicopter beside you."},
    {"plane", Code::Plane, "Plane beside you."},
    {"comet", Code::SportsCar, "Sports car beside you."},
    {"moto", Code::Moto, "City motorcycle beside you."},
    {"rocket", Code::Rocket, "Dirt bike beside you."},
    {"painkiller", Code::Invincible, "Invincible for 5 minutes."},
};

// PAINKILLER: five minutes in which nothing takes a hit point.
constexpr double kInvincibleSeconds = 5 * 60;

// The typed line, without the case or the spaces around it: "  BuzzOff " is
// buzzoff. A word that is no code is Unknown, never a near match.
inline Code parse(std::string_view typed) {
    const auto first = typed.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return Code::Unknown;
    const auto last = typed.find_last_not_of(" \t\r\n");
    std::string word;
    for (char c : typed.substr(first, last - first + 1))
        word += char(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& entry : kCodes)
        if (word == entry.word) return entry.code;
    return Code::Unknown;
}

inline const char* said(Code code) {
    for (const auto& entry : kCodes)
        if (entry.code == code) return entry.said;
    return "Unknown code.";
}

// "4:59" -- what the HUD shows of an invincibility, rounded up so that the
// last second still reads 0:01 and never 0:00 while it holds.
inline std::string clock(double seconds) {
    const int whole = seconds > 0 ? int(seconds + .999) : 0;
    const int s = whole % 60;
    return std::to_string(whole / 60) + (s < 10 ? ":0" : ":") + std::to_string(s);
}

}  // namespace r1::cheats
