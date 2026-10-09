// The world generator's shared vocabulary: tiles, the WGS84 frames, and the
// seeded draws every inference is made with.
//
// This library is plain C++17 with no engine in it, so the same code cooks a
// tile inside the game, in the headless `r1cook` tool, and on any platform the
// game is exported to. It replaced the Python worker the game used to run
// beside it; where a comment says "as Python did", that worker's behaviour is
// what this reproduces, and the tests hold it to it.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace r1 {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;
inline double radians(double d) { return d * kPi / 180.0; }
inline double degrees(double r) { return r * 180.0 / kPi; }

struct P2 { double x = 0, y = 0; };
struct P3 { double x = 0, y = 0, z = 0; };
inline bool operator==(const P2& a, const P2& b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(const P2& a, const P2& b) { return !(a == b); }
inline bool operator<(const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); }
inline bool operator==(const P3& a, const P3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline double dist(P2 a, P2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// Python's `a % b` for floats: the result takes the sign of the divisor.
inline double pymod(double a, double b) {
    double m = std::fmod(a, b);
    if (m != 0.0 && ((m < 0.0) != (b < 0.0))) m += b;
    return m;
}
// Python's round(x, n), to within the last bit: the weld keys and the
// coordinates it rounds only need it to be deterministic, and it is.
inline double pyround(double x, int digits) {
    const double k = std::pow(10.0, digits);
    return std::nearbyint(x * k) / k;
}
// Python's round(x) (half to even), as an integer.
inline long long pyround(double x) { return (long long)std::nearbyint(x); }

using Tags = std::map<std::string, std::string>;
inline const std::string* tag(const Tags& t, const char* key) {
    auto it = t.find(key);
    return it == t.end() ? nullptr : &it->second;
}
inline std::string tagOr(const Tags& t, const char* key, const std::string& fallback = "") {
    auto it = t.find(key);
    return it == t.end() ? fallback : it->second;
}
inline bool has(const Tags& t, const char* key) { return t.count(key) > 0; }
// `tags.get(k) not in {None, "no"}`: tunnels and bridges.
inline bool taggedYes(const Tags& t, const char* key) {
    auto it = t.find(key);
    return it != t.end() && it->second != "no";
}
// Water nobody sees: a culvert, a canal under a boulevard, a covered
// reservoir. OSM maps it where it runs, and above it is the street.
inline bool hiddenWater(const Tags& t) {
    return taggedYes(t, "tunnel") || tagOr(t, "covered") == "yes" || tagOr(t, "location") == "underground";
}

// ── seeded draws ────────────────────────────────────────────────────────────
//
// CPython's Mersenne Twister, reproduced exactly: `random.Random(n)` seeding,
// `random()`, `randrange()` and `gauss()`. Exactness is not nostalgia. The
// Python worker cooked every tile the players have already seen, and with the
// same draws the C++ cook gives every building the same height, roof and
// colour it had — so the port can be checked against it building by building,
// and nobody's street changes because the language did.
class PyRandom {
public:
    explicit PyRandom(__int128 seed) { seedWith(seed); }
    double random() {
        uint32_t a = next() >> 5, b = next() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }
    uint32_t getrandbits(int k) { return k <= 0 ? 0 : next() >> (32 - k); }
    // randrange(n) for 0 < n < 2^32, by rejection like `_randbelow`.
    uint32_t randrange(uint32_t n) {
        int k = 0;
        for (uint32_t v = n; v; v >>= 1) ++k;
        uint32_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }
    double gauss(double mu, double sigma) {
        double z;
        if (hasNext_) { z = next_; hasNext_ = false; }
        else {
            const double x2pi = random() * kTau;
            const double g2rad = std::sqrt(-2.0 * std::log(1.0 - random()));
            z = std::cos(x2pi) * g2rad;
            next_ = std::sin(x2pi) * g2rad; hasNext_ = true;
        }
        return mu + z * sigma;
    }

private:
    std::array<uint32_t, 624> mt_{};
    int index_ = 625;
    bool hasNext_ = false; double next_ = 0;
    void initGenrand(uint32_t s) {
        mt_[0] = s;
        for (index_ = 1; index_ < 624; ++index_)
            mt_[index_] = 1812433253u * (mt_[index_ - 1] ^ (mt_[index_ - 1] >> 30)) + uint32_t(index_);
    }
    void seedWith(__int128 n) {
        if (n < 0) n = -n;
        unsigned __int128 u = (unsigned __int128)n;
        std::vector<uint32_t> key;
        do { key.push_back(uint32_t(u & 0xffffffffu)); u >>= 32; } while (u);
        initGenrand(19650218u);
        size_t i = 1, j = 0;
        const size_t len = key.size();
        for (size_t k = std::max<size_t>(624, len); k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + key[j] + uint32_t(j);
            ++i; ++j;
            if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
            if (j >= len) j = 0;
        }
        for (size_t k = 623; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - uint32_t(i);
            ++i;
            if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
        }
        mt_[0] = 0x80000000u;
        index_ = 624;
    }
    uint32_t next() {
        if (index_ >= 624) {
            static const uint32_t mag01[2] = {0u, 0x9908b0dfu};
            int kk = 0;
            for (; kk < 624 - 397; ++kk) {
                uint32_t y = (mt_[kk] & 0x80000000u) | (mt_[kk + 1] & 0x7fffffffu);
                mt_[kk] = mt_[kk + 397] ^ (y >> 1) ^ mag01[y & 1u];
            }
            for (; kk < 623; ++kk) {
                uint32_t y = (mt_[kk] & 0x80000000u) | (mt_[kk + 1] & 0x7fffffffu);
                mt_[kk] = mt_[kk + (397 - 624)] ^ (y >> 1) ^ mag01[y & 1u];
            }
            uint32_t y = (mt_[623] & 0x80000000u) | (mt_[0] & 0x7fffffffu);
            mt_[623] = mt_[396] ^ (y >> 1) ^ mag01[y & 1u];
            index_ = 0;
        }
        uint32_t y = mt_[index_++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= (y >> 18);
        return y;
    }
};

// A generator tied to one feature and one purpose: `salt`
// separates the purposes, `id` the features, and there is no third entropy.
inline PyRandom seeded(__int128 id, __int128 salt) { return PyRandom((id * 1000003) ^ salt); }

// ── tiles ───────────────────────────────────────────────────────────────────
//
// Metre-sized latitude rings with no polar cutoff. The key's
// version is the generator's: bump it and every tile is cooked again, from the
// raw observations already on disk.
constexpr int kRows = 36000;
constexpr double kStep = 180.0 / kRows;
constexpr int kVersion = 39;  // 39: buildings stream independent visual LODs with stable CPU shells
// The version the Python worker wrote its caches under. Its raw observations
// (osm.json, elevation) are reused, never its geometry.
constexpr int kFirstVersion = 1;

inline double wrap(double lon) { return pymod(lon + 180.0, 360.0) - 180.0; }
inline int columns(int row) {
    return std::max(1, (int)pyround(72000.0 * std::cos(radians(-90.0 + (row + 0.5) * kStep))));
}

struct Bounds { double south = 0, west = 0, north = 0, east = 0; };

struct Tile {
    int row = 0, col = 0;
    std::string key(int version = kVersion) const {
        return "v" + std::to_string(version) + "_" + std::to_string(row) + "_" + std::to_string(col);
    }
    Bounds bounds() const {
        const double w = 360.0 / columns(row);
        return {-90.0 + row * kStep, -180.0 + col * w, -90.0 + (row + 1) * kStep, -180.0 + (col + 1) * w};
    }
    P2 center() const { Bounds b = bounds(); return {(b.west + b.east) / 2, (b.south + b.north) / 2}; }
    bool operator==(const Tile& o) const { return row == o.row && col == o.col; }
    bool operator!=(const Tile& o) const { return !(*this == o); }
    bool operator<(const Tile& o) const { return row < o.row || (row == o.row && col < o.col); }
};

inline Tile tileAt(double lon, double lat) {
    const int row = std::min(kRows - 1, std::max(0, (int)std::floor((lat + 90.0) / kStep)));
    return {row, std::min(columns(row) - 1, (int)std::floor((wrap(lon) + 180.0) / 360.0 * columns(row)))};
}

// ── WGS84 frames ──────────────────────────────────────────────────────────
constexpr double kA = 6378137.0;
constexpr double kF = 1.0 / 298.257223563;
constexpr double kB = kA * (1.0 - kF);
constexpr double kE2 = kF * (2.0 - kF);
constexpr double kEP2 = kE2 / (1.0 - kE2);
constexpr double kRMean = 6371008.8;
// Metres in a degree of the equator (2 pi kA / 360, rounded): the spherical
// shortcut every local metric estimate here takes.
constexpr double kMetresPerDegree = 111320.0;

inline P3 geodeticToEcef(double lonDeg, double latDeg, double alt = 0.0) {
    const double lon = radians(lonDeg), lat = radians(latDeg);
    const double sinLat = std::sin(lat), cosLat = std::cos(lat);
    const double n = kA / std::sqrt(1.0 - kE2 * sinLat * sinLat);
    return {(n + alt) * cosLat * std::cos(lon), (n + alt) * cosLat * std::sin(lon), (n * (1.0 - kE2) + alt) * sinLat};
}
inline P3 ecefToGeodetic(double x, double y, double z) {
    const double lon = std::atan2(y, x), p = std::hypot(x, y);
    if (p < 1e-9) return {degrees(lon), z >= 0 ? 90.0 : -90.0, std::abs(z) - kB};
    const double theta = std::atan2(z * kA, p * kB);
    const double st = std::sin(theta), ct = std::cos(theta);
    const double lat = std::atan2(z + kEP2 * kB * st * st * st, p - kE2 * kA * ct * ct * ct);
    const double sl = std::sin(lat);
    const double n = kA / std::sqrt(1.0 - kE2 * sl * sl);
    return {degrees(lon), degrees(lat), p / std::cos(lat) - n};
}

// A local tangent frame. Engine coordinates are (x=E, y=U, z=-N).
struct Anchor {
    double lon = 0, lat = 0, alt = 0, x = 0, y = 0, z = 0;
    std::array<double, 3> e{}, n{}, u{};
    static Anchor at(double lonDeg, double latDeg, double alt = 0.0) {
        Anchor a; a.lon = lonDeg; a.lat = latDeg; a.alt = alt;
        P3 o = geodeticToEcef(lonDeg, latDeg, alt); a.x = o.x; a.y = o.y; a.z = o.z;
        const double lo = radians(lonDeg), la = radians(latDeg);
        const double sl = std::sin(lo), cl = std::cos(lo), sp = std::sin(la), cp = std::cos(la);
        a.e = {-sl, cl, 0.0}; a.n = {-sp * cl, -sp * sl, cp}; a.u = {cp * cl, cp * sl, sp};
        return a;
    }
    P3 ecefToEnu(double px, double py, double pz) const {
        const double dx = px - x, dy = py - y, dz = pz - z;
        return {e[0] * dx + e[1] * dy + e[2] * dz, n[0] * dx + n[1] * dy + n[2] * dz,
                u[0] * dx + u[1] * dy + u[2] * dz};
    }
    P3 enuToEcef(double pe, double pn, double pu) const {
        return {x + e[0] * pe + n[0] * pn + u[0] * pu, y + e[1] * pe + n[1] * pn + u[1] * pu,
                z + e[2] * pe + n[2] * pn + u[2] * pu};
    }
    P3 toEngine(double lonDeg, double latDeg, double alt = 0.0) const {
        P3 c = geodeticToEcef(lonDeg, latDeg, alt);
        P3 enu = ecefToEnu(c.x, c.y, c.z);
        return {enu.x, enu.z, -enu.y};
    }
    // (lon, lat, alt) of an engine point.
    P3 toGeodetic(double ex, double ey, double ez) const {
        P3 c = enuToEcef(ex, -ez, ey);
        return ecefToGeodetic(c.x, c.y, c.z);
    }
};

}  // namespace r1
