#pragma once

// The Destination tab (Settings): a latitude and a longitude typed by the
// player, south and west negative. What a player types is read the way it is
// written on maps and keyboards -- a typographic minus (the one this game's
// own messages print), a decimal comma (French keyboards), spaces, a degree
// sign -- and anything else is refused with the field and the reason named
// (CLAUDE.md rule 3): "Invalid coordinates" for "-2.18 " sent the player to
// look for a bug in the minus sign.

#include <charconv>
#include <cmath>
#include <optional>
#include <string>

namespace r1 {

// One coordinate in decimal degrees: "-73.9857", " 40.7484 ", "−33.8688",
// "–2.18", "+151.2093", "-2,18", "48.02°". No exponent, no second number.
inline std::optional<double> parseCoordinate(const std::string& typed) {
    std::string text = typed;
    auto strip = [&](const std::string& what, bool front) {
        if (text.size() < what.size()) return false;
        const size_t at = front ? 0 : text.size() - what.size();
        if (text.compare(at, what.size(), what) != 0) return false;
        text.erase(at, what.size());
        return true;
    };
    auto trim = [&] {
        bool more = true;
        while (more) more = strip(" ", true) || strip("\t", true) || strip("\xC2\xA0", true);
        more = true;
        while (more) more = strip(" ", false) || strip("\t", false) || strip("\xC2\xA0", false);
    };
    trim();
    if (strip("\xC2\xB0", false)) trim();  // °
    bool negative = false;
    if (strip("-", true) || strip("\xE2\x88\x92", true) || strip("\xE2\x80\x93", true)) negative = true;  // - − –
    else strip("+", true);
    size_t digits = 0, separators = 0;
    for (char& c : text) {
        if (c >= '0' && c <= '9') ++digits;
        else if (c == '.' || c == ',') {c = '.'; ++separators;}
        else return std::nullopt;
    }
    if (digits == 0 || separators > 1) return std::nullopt;
    double value = 0;
    const auto read = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
    if (read.ec != std::errc() || read.ptr != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return negative ? -value : value;
}

struct DestinationReading {
    double lon = 0, lat = 0;
    std::string refusal;  // empty when the place holds
    explicit operator bool() const {return refusal.empty();}
};

inline DestinationReading readDestination(const std::string& latitude, const std::string& longitude) {
    DestinationReading out;
    const auto lat = parseCoordinate(latitude), lon = parseCoordinate(longitude);
    if (!lat) out.refusal = "Latitude \"" + latitude + "\" is not a number: decimal degrees, south negative (for example -33.8688).";
    else if (std::abs(*lat) > 90.0) {
        out.refusal = "Latitude " + latitude + " is out of range: -90 (south pole) to 90 (north pole).";
        if (std::abs(*lat) <= 180.0 && lon && std::abs(*lon) <= 90.0) out.refusal += " Are latitude and longitude swapped?";
    } else if (!lon) out.refusal = "Longitude \"" + longitude + "\" is not a number: decimal degrees, west negative (for example -73.9857).";
    else if (std::abs(*lon) > 180.0) out.refusal = "Longitude " + longitude + " is out of range: -180 (west) to 180 (east).";
    else {out.lat = *lat; out.lon = *lon;}
    return out;
}

}  // namespace r1
