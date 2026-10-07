#pragma once

// "Forcer l'heure à" (Options screen): the world held at one hour of the day,
// wherever the player goes. The hour is the one the HUD shows -- local civil
// time, from the place's time zone or its longitude estimate -- so asking for
// 14:00 shows 14:00, in Paris as in Tokyo. The Sun is still computed by
// `scripts/sun_cycle.js` for that instant and that place; only the instant is
// chosen.

#include <cctype>
#include <cmath>
#include <optional>
#include <string>

namespace r1 {

// Minutes after local midnight from what a player types: "14:30", "14h30",
// "14h", "14", "7:05". Anything else, or an hour outside 00:00-23:59, is no
// answer -- the caller says so rather than guessing.
inline std::optional<int> parseClockTime(const std::string& typed) {
    size_t i = typed.find_first_not_of(" \t");
    const size_t end = typed.find_last_not_of(" \t");
    if (i == std::string::npos) return std::nullopt;
    auto digits = [&](int& out) {
        const size_t start = i;
        out = 0;
        while (i <= end && i - start < 2 && std::isdigit(static_cast<unsigned char>(typed[i])))
            out = out * 10 + (typed[i++] - '0');
        return i - start;
    };
    int hours = 0, minutes = 0;
    if (digits(hours) == 0) return std::nullopt;
    if (i <= end) {
        if (typed[i] != ':' && typed[i] != 'h' && typed[i] != 'H') return std::nullopt;
        const bool colon = typed[i++] == ':';
        if (i <= end) {
            if (digits(minutes) != 2) return std::nullopt;
        } else if (colon) return std::nullopt;
    }
    if (i <= end || hours > 23 || minutes > 59) return std::nullopt;
    return hours * 60 + minutes;
}

inline std::string formatClockTime(int minutes) {
    const int h = minutes / 60, m = minutes % 60;
    return std::string(h < 10 ? "0" : "") + std::to_string(h) + ":" + (m < 10 ? "0" : "") + std::to_string(m);
}

// The Unix instant, on the place's current local day, at which its clock reads
// `minutes`. `utcOffsetSeconds` is the same offset the HUD clock uses.
inline double forcedInstant(double nowUnix, int utcOffsetSeconds, int minutes) {
    const double local = nowUnix + utcOffsetSeconds;
    const double midnight = std::floor(local / 86400.0) * 86400.0;
    return midnight + minutes * 60.0 - utcOffsetSeconds;
}

}  // namespace r1
