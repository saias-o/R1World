#include "seaice.hpp"

#include "palette.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>

namespace r1 {

namespace {

// ── the ASCAT grid ──────────────────────────────────────────────────────────
//
// NOAA CoastWatch/PolarWatch, "Ice Classification, Metop-C ASCAT, 4km, North
// Pole (Arctic), Daily": polar stereographic on WGS84, true scale at 60°N,
// central meridian 45°W, 1530 cells a side.
constexpr const char* kDataset = "noaacwSARciceclassnpoleDaily";
constexpr int kGridCells = 1530;
constexpr double kGridStep = 4280.900055;
constexpr double kGridFirst = 3272748.092178;  // x of column 0 is its negative; y of row 0
constexpr int kBlock = 16;

// Snyder (1987), 21-33 and 15-9: polar stereographic on the ellipsoid.
P2 ascatProjection(double lon, double lat) {
    const double e = std::sqrt(kE2);
    auto t = [&](double phi) {
        const double s = std::sin(phi);
        return std::tan(kPi / 4 - phi / 2) / std::pow((1 - e * s) / (1 + e * s), e / 2);
    };
    const double standard = radians(60.0);
    const double mc = std::cos(standard) / std::sqrt(1 - kE2 * std::sin(standard) * std::sin(standard));
    const double rho = kA * mc * t(radians(lat)) / t(standard);
    const double lambda = radians(lon + 45.0);
    return {rho * std::sin(lambda), -rho * std::cos(lambda)};
}

// The plane the pack is drawn on: a sphere's stereographic projection, true
// to scale at the pole, one for each hemisphere. It has no seam anywhere a
// player can stand, so neither has the ice.
P2 patternPlane(double lon, double lat) {
    const bool north = lat >= 0;
    const double phi = radians(std::abs(lat)), lambda = radians(lon);
    const double rho = 2 * kRMean * std::tan(kPi / 4 - phi / 2);
    return {rho * std::sin(lambda), north ? -rho * std::cos(lambda) : rho * std::cos(lambda)};
}

// ── deterministic noise ─────────────────────────────────────────────────────
//
// Integer hashing and value noise: additions, products and floors only, so
// the same ice comes out on every machine (§3 I3).
inline uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
inline uint64_t hash(int64_t i, int64_t j, uint64_t salt) { return mix(mix(mix(salt) ^ uint64_t(i)) ^ uint64_t(j)); }
inline double unit(uint64_t h) { return double(h >> 11) * (1.0 / 9007199254740992.0); }
inline double smooth(double t) { return t * t * (3 - 2 * t); }

double valueNoise(double x, double y, uint64_t salt) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int64_t ix = int64_t(fx), iy = int64_t(fy);
    const double u = smooth(x - fx), v = smooth(y - fy);
    auto at = [&](int64_t i, int64_t j) { return unit(hash(i, j, salt)) * 2 - 1; };
    const double low = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * u;
    const double high = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * u;
    return low + (high - low) * v;
}
// Three octaves, in -1..1.
double fbm(double x, double y, uint64_t salt) {
    return (valueNoise(x, y, salt) * 0.571 + valueNoise(x * 2.03, y * 2.03, salt + 1) * 0.286 +
            valueNoise(x * 4.07, y * 4.07, salt + 2) * 0.143);
}

enum : uint64_t {
    kWarpX = 11, kWarpY = 23, kSeed = 31, kPresent = 41, kEdge = 53, kOffset = 67, kDune = 79, kHummock = 97,
    kPond = 113, kRubble = 131, kAlong = 149, kWind = 163, kFloe = 181,
};

// The two nearest floe centres of a jittered grid, and where the point is
// from the boundary between them.
struct Voronoi { P2 a, b; int64_t ia = 0, ib = 0; double edge = 0; };
Voronoi voronoi(P2 p, double size, uint64_t salt) {
    const int64_t gx = int64_t(std::floor(p.x / size)), gy = int64_t(std::floor(p.y / size));
    double d1 = 1e300, d2 = 1e300;
    Voronoi v;
    for (int64_t j = gy - 2; j <= gy + 2; ++j)
        for (int64_t i = gx - 2; i <= gx + 2; ++i) {
            const uint64_t h = hash(i, j, salt);
            const P2 s{(double(i) + 0.2 + 0.6 * unit(h)) * size, (double(j) + 0.2 + 0.6 * unit(mix(h))) * size};
            const double d = (s.x - p.x) * (s.x - p.x) + (s.y - p.y) * (s.y - p.y);
            const int64_t id = (i << 32) ^ (j & 0xffffffff);
            if (d < d1) { d2 = d1; v.b = v.a; v.ib = v.ia; d1 = d; v.a = s; v.ia = id; }
            else if (d < d2) { d2 = d; v.b = s; v.ib = id; }
        }
    const double gap = std::max(1e-9, dist(v.a, v.b));
    v.edge = (d2 - d1) / (2 * gap);
    return v;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }
double clamp01(double x) { return std::max(0.0, std::min(1.0, x)); }

// ── the file ────────────────────────────────────────────────────────────────

int dayOfYear(const std::string& date) {
    if (date.size() < 10) return 60;
    const int y = std::atoi(date.substr(0, 4).c_str()), m = std::atoi(date.substr(5, 2).c_str()),
              d = std::atoi(date.substr(8, 2).c_str());
    static const int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (m < 1 || m > 12) return 60;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return before[m - 1] + d + (leap && m > 2 ? 1 : 0);
}

// Cells with no reading take the class of the nearest cell read: a
// breadth-first flood from every reading at once, in index order.
int fill(std::vector<uint8_t>& classes, int cols, int rows) {
    std::deque<int> queue;
    int filled = 0;
    for (int i = 0; i < cols * rows; ++i)
        if (classes[size_t(i)] >= kIceWater) queue.push_back(i);
    if (queue.empty()) return 0;
    while (!queue.empty()) {
        const int at = queue.front();
        queue.pop_front();
        const int r = at / cols, c = at % cols;
        const int around[4][2] = {{r - 1, c}, {r + 1, c}, {r, c - 1}, {r, c + 1}};
        for (const auto& [rr, cc] : around) {
            if (rr < 0 || rr >= rows || cc < 0 || cc >= cols) continue;
            uint8_t& next = classes[size_t(rr * cols + cc)];
            if (next >= kIceWater) continue;
            next = uint8_t(0x80 | classes[size_t(at)]);  // marked: reached, not read
            ++filled;
            queue.push_back(rr * cols + cc);
        }
        classes[size_t(at)] &= 0x7f;
    }
    for (auto& c : classes) c &= 0x7f;
    return filled;
}

double concentrationOf(int c) { return c >= kIceSeasonal ? 1.0 : 0.0; }
double ageOf(int c) { return c == kIcePerennial ? 1.0 : c == kIceMixed ? 0.5 : 0.0; }

}  // namespace

// ── observations ────────────────────────────────────────────────────────────

bool seaIceGridCovers(double lon, double lat) {
    if (lat < 40) return false;
    const P2 p = ascatProjection(lon, lat);
    const double edge = kGridFirst + kGridStep / 2;
    return std::abs(p.x) <= edge && std::abs(p.y) <= edge;
}

SeaIceWindow seaIceWindow(double lon, double lat) {
    const P2 p = ascatProjection(lon, lat);
    const int col = std::max(0, std::min(kGridCells - 1, int(std::lround((p.x + kGridFirst) / kGridStep))));
    const int row = std::max(0, std::min(kGridCells - 1, int(std::lround((kGridFirst - p.y) / kGridStep))));
    return {row / kBlock, col / kBlock};
}

std::string SeaIceWindow::file() const {
    return std::string("ascat_") + std::to_string(blockRow) + "_" + std::to_string(blockCol) + ".json";
}

std::string SeaIceWindow::url() const {
    const int r0 = std::max(0, (blockRow - 1) * kBlock), r1 = std::min(kGridCells - 1, (blockRow + 2) * kBlock - 1);
    const int c0 = std::max(0, (blockCol - 1) * kBlock), c1 = std::min(kGridCells - 1, (blockCol + 2) * kBlock - 1);
    return std::string("https://polarwatch.noaa.gov/erddap/griddap/") + kDataset + ".json?IceClass%5Blast%5D%5B0%5D%5B" +
           std::to_string(r0) + ":1:" + std::to_string(r1) + "%5D%5B" + std::to_string(c0) + ":1:" +
           std::to_string(c1) + "%5D";
}

nlohmann::json seaIceDocument(const nlohmann::json& erddap) {
    const auto& rows = erddap.at("table").at("rows");
    if (!rows.is_array() || rows.empty()) throw std::runtime_error("ASCAT answered no cells");
    std::vector<double> xs, ys;
    for (const auto& r : rows) { ys.push_back(r.at(2).get<double>()); xs.push_back(r.at(3).get<double>()); }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end(), std::greater<double>());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    const int cols = int(xs.size()), nrows = int(ys.size());
    if (size_t(cols) * size_t(nrows) != rows.size()) throw std::runtime_error("ASCAT answered a ragged window");
    std::string classes(size_t(cols * nrows), '0');
    for (const auto& r : rows) {
        const int j = int(std::lower_bound(ys.begin(), ys.end(), r.at(2).get<double>(), std::greater<double>()) - ys.begin());
        const int i = int(std::lower_bound(xs.begin(), xs.end(), r.at(3).get<double>()) - xs.begin());
        const auto& v = r.at(4);
        const int c = v.is_number() ? int(std::lround(v.get<double>())) : 0;
        classes[size_t(j * cols + i)] = char('0' + std::max(0, std::min(5, c)));
    }
    const std::string time = rows[0].at(0).get<std::string>();
    return {{"dataset", kDataset},
            {"source", "NOAA CoastWatch/PolarWatch, Metop-C ASCAT ice classification, 4 km"},
            {"date", time.substr(0, 10)}, {"x0", xs.front()}, {"y0", ys.front()}, {"step", kGridStep},
            {"cols", cols}, {"rows", nrows}, {"classes", classes}};
}

SeaIce seaIceFrom(const nlohmann::json& doc) {
    SeaIce ice;
    ice.measured = true;
    ice.source = doc.at("source").get<std::string>() + " (" + doc.at("date").get<std::string>() + ")";
    ice.date = doc.at("date").get<std::string>();
    ice.dayOfYear = dayOfYear(ice.date);
    ice.x0 = doc.at("x0"); ice.y0 = doc.at("y0"); ice.step = doc.at("step");
    ice.cols = doc.at("cols"); ice.rows = doc.at("rows");
    const std::string text = doc.at("classes").get<std::string>();
    if (text.size() != size_t(ice.cols * ice.rows)) throw std::runtime_error("sea-ice window of the wrong size");
    for (char c : text) ice.classes.push_back(uint8_t(c - '0'));
    ice.filled = fill(ice.classes, ice.cols, ice.rows);
    return ice;
}

SeaIce inferredSeaIce(double lat) {
    SeaIce ice;
    ice.north = lat >= 0;
    ice.measured = false;
    ice.source = "inferred: the ice that never leaves (north of 80\xC2\xB0N, south of 70\xC2\xB0S)";
    // No date is known, so none is invented: the pack is drawn as it is most
    // of the year, snow-covered and frozen over.
    ice.dayOfYear = ice.north ? 60 : 243;
    return ice;
}

int SeaIce::classAt(double lon, double lat) const {
    if (classes.empty()) {
        if (lat >= 85) return kIcePerennial;
        if (lat >= 80) return kIceMixed;
        if (lat <= -70) return kIceSeasonal;
        return kIceWater;
    }
    const P2 p = ascatProjection(lon, lat);
    const int i = int(std::lround((p.x - x0) / step)), j = int(std::lround((y0 - p.y) / step));
    return classes[size_t(std::max(0, std::min(rows - 1, j)) * cols + std::max(0, std::min(cols - 1, i)))];
}

SeaIce::Sample SeaIce::at(double lon, double lat) const {
    if (classes.empty()) {
        // The inferred ice fades over a degree rather than stopping at a line.
        Sample s;
        if (lat >= 0) {
            s.concentration = clamp01(lat - 79.5);
            s.age = clamp01((lat - 80.0) / 5.0) * 0.5 + (lat >= 85 ? 0.5 : 0.0);
        } else {
            s.concentration = clamp01(-lat - 69.5);
        }
        return s;
    }
    const P2 p = ascatProjection(lon, lat);
    const double u = std::max(0.0, std::min(double(cols - 1), (p.x - x0) / step));
    const double v = std::max(0.0, std::min(double(rows - 1), (y0 - p.y) / step));
    const int i = std::min(cols - 2, int(u)), j = std::min(rows - 2, int(v));
    const double fu = u - i, fv = v - j;
    Sample s;
    for (int dj = 0; dj < 2; ++dj)
        for (int di = 0; di < 2; ++di) {
            const double w = (di ? fu : 1 - fu) * (dj ? fv : 1 - fv);
            const int c = classes[size_t((j + dj) * cols + i + di)];
            s.concentration += w * concentrationOf(c);
            s.age += w * ageOf(c) * concentrationOf(c);
        }
    if (s.concentration > 1e-9) s.age /= s.concentration;
    return s;
}

bool SeaIce::any(const Bounds& b) const {
    for (int j = 0; j <= 4; ++j)
        for (int i = 0; i <= 4; ++i)
            if (at(b.west + (b.east - b.west) * i / 4, b.south + (b.north - b.south) * j / 4).concentration > 0.02)
                return true;
    return false;
}

// ── the pack ────────────────────────────────────────────────────────────────

int iceSeason(int day, bool north) {
    const int d = north ? day : (day + 182) % 366;
    if (165 <= d && d <= 235) return 1;
    if (236 <= d && d <= 300) return 2;
    return 0;
}

IcePoint iceAt(double lon, double lat, const IceParameters& ip, int season) {
    IcePoint out;
    const double conc = clamp01(ip.concentration), age = clamp01(ip.age);
    if (conc <= 0.02) { out.height = -0.35; out.kind = IceKind::Water; return out; }
    P2 p = patternPlane(lon, lat);
    // Floe edges wander: the plane is warped before it is cut into floes.
    const P2 q{p.x + 70 * fbm(p.x / 650, p.y / 650, kWarpX), p.y + 70 * fbm(p.x / 650, p.y / 650, kWarpY)};
    const Voronoi v = voronoi(q, 320, kSeed);
    const double present = std::pow(conc, 0.6);
    auto there = [&](int64_t id) { return conc >= 0.97 || unit(hash(id, 0, kPresent)) < present; };

    const double freeboard = lerp(0.14, 0.32, age);
    const double snow = season == 0 ? 0.18 + 0.15 * age : season == 2 ? 0.06 + 0.08 * age : 0.0;
    if (!there(v.ia)) {
        // Between floes: young ice once the sea freezes, open water in summer
        // with the small floes a pack breaks into.
        if (season == 0) {
            out.height = 0.04 + 0.02 * valueNoise(q.x / 9, q.y / 9, kFloe);
            out.kind = IceKind::Young; out.young = 1;
            return out;
        }
        const Voronoi small = voronoi(q, 45, kFloe);
        if (unit(hash(small.ia, 0, kFloe)) < 0.45 * conc && small.edge > 1.5) {
            out.height = std::min(0.10, 0.02 + 0.05 * (small.edge - 1.5)) + 0.04;
            out.kind = IceKind::Snow;
            return out;
        }
        out.height = -0.35; out.kind = IceKind::Water;
        return out;
    }
    // The floe's own surface: freeboard, snow, wind-shaped dunes along a
    // prevailing direction that turns slowly across the ocean, and the
    // rounded hummocks of ice that has survived a summer.
    const double wind = kPi * fbm(q.x / 20000, q.y / 20000, kWind);
    const double cw = std::cos(wind), sw = std::sin(wind);
    const double along = q.x * cw + q.y * sw, across = -q.x * sw + q.y * cw;
    const double dunes = (0.04 + 0.5 * snow) * fbm(along / 30, across / 9, kDune);
    const double hummock = std::max(0.0, fbm(q.x / 60, q.y / 60, kHummock));
    double surface = freeboard + snow + dunes + age * 1.8 * std::pow(hummock, 1.5) +
                     0.06 * (unit(hash(v.ia, 1, kOffset)) - 0.5);
    out.height = surface;
    // In summer the snow is gone, but the floe is still white: its top is a
    // crumbling layer of melting ice that scatters like snow.
    out.kind = IceKind::Snow;

    // Ponds: the low ground of a floe in summer, frozen over in autumn. The
    // shore is a metre or two of gradient: water, then slush, then snow.
    if (season != 0) {
        const double f = fbm(q.x / 26, q.y / 26, kPond), threshold = 0.22 + 0.22 * age;
        out.pond = smooth(clamp01((f - threshold + 0.03) / 0.08));
        if (f > threshold) out.kind = season == 1 ? IceKind::Pond : IceKind::FrozenPond;
        out.height = lerp(surface, freeboard + (season == 1 ? -0.10 : 0.02), out.pond);
        surface = out.height;
    }
    // The boundary with the next floe: a lead, a pressure ridge or a closed crack.
    if (!there(v.ib)) return out;
    const int64_t lo = std::min(v.ia, v.ib), hi = std::max(v.ia, v.ib);
    const uint64_t edge = hash(lo, hi, kEdge);
    const double u1 = unit(edge), u2 = unit(mix(edge)), u3 = unit(mix(mix(edge)));
    const double pLead = 0.28 + 0.5 * (1 - conc), pRidge = 0.42 + 0.12 * age;
    if (u1 < pLead) {
        const double half = (4 + 26 * u2 * u2 + 25 * (1 - conc)) / 2;
        const double pOpen = season == 0 ? 0.12 : season == 2 ? 0.35 : 0.9;
        const bool open = u3 < pOpen;
        const double lead = open ? -0.35 : 0.04;
        if (v.edge < half) {
            out.height = lead;
            out.kind = open ? IceKind::Water : IceKind::Young;
        } else if (v.edge < half + 1.5) {
            out.height = lerp(lead, surface, (v.edge - half) / 1.5);
        }
        out.young = smooth(clamp01((half + 1.5 - v.edge) / 2.0));
        return out;
    }
    if (u1 < pLead + pRidge) {
        // A ridge's sail runs along the floe boundary, heaped where the floes
        // met hardest and broken where they barely touched.
        const double dx = v.a.x - v.b.x, dy = v.a.y - v.b.y, len = std::max(1e-9, std::hypot(dx, dy));
        const double t = (-q.x * dy + q.y * dx) / len;
        const double strength = clamp01(0.55 + 0.75 * valueNoise(t / 22, u3 * 97, kAlong));
        const double sail = (0.9 + 2.0 * u2) * (1 + 0.45 * age) * strength;
        const double width = 2.6 * sail + 1.0 + 2.0 * age;
        if (sail > 0.05 && v.edge < width) {
            const double f = 1 - v.edge / width;
            const double shape = lerp(std::pow(f, 1.3), smooth(f), age);
            const double rubble = 0.18 * sail * valueNoise(q.x / 2.3, q.y / 2.3, kRubble) * f;
            out.ridge = sail * shape + rubble;
            out.ridgeHeight = sail;
            out.height = surface + out.ridge;
            if (out.kind == IceKind::Pond || out.kind == IceKind::FrozenPond) out.kind = IceKind::Snow;
            out.pond *= clamp01(1 - out.ridge / 0.3);
        }
    }
    return out;
}

// ── the tile ────────────────────────────────────────────────────────────────

namespace {
struct Swatches { const Palette::IceSwatch *snow, *bare, *young, *frozenPond, *pond; };
Swatches swatches() {
    const auto& s = palette().seaIce;
    auto get = [&](const char* k) {
        auto it = s.find(k);
        if (it == s.end()) throw std::runtime_error(std::string("atlas.json has no ground.seaIce.") + k);
        return &it->second;
    };
    return {get("snow"), get("bare"), get("young"), get("frozenPond"), get("pond")};
}
Material iceMaterial(const Palette::IceSwatch& s) {
    return surfaceMaterial(s.swatch.name, s.swatch.color, s.swatch.roughness,
                           s.family.empty() ? std::nullopt : std::optional<std::string>(s.family));
}

// A block of ice turned every way, snow on what faces up.
void addBlock(Mesh& snowFaces, Mesh& iceFaces, P3 c, P3 size, double yaw, double pitch, double roll) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const double cr = std::cos(roll), sr = std::sin(roll);
    auto turn = [&](double x, double y, double z) {
        // roll about x, pitch about z, then yaw about y
        const double y1 = y * cr - z * sr, z1 = y * sr + z * cr;
        const double x2 = x * cp - y1 * sp, y2 = x * sp + y1 * cp;
        return P3{c.x + x2 * cy + z1 * sy, c.y + y2, c.z - x2 * sy + z1 * cy};
    };
    const double hx = size.x / 2, hy = size.y / 2, hz = size.z / 2;
    const P3 p[8] = {turn(-hx, -hy, -hz), turn(hx, -hy, -hz), turn(hx, -hy, hz), turn(-hx, -hy, hz),
                     turn(-hx, hy, -hz),  turn(hx, hy, -hz),  turn(hx, hy, hz),  turn(-hx, hy, hz)};
    // Wound so each face's normal points out of the block.
    const int faces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
    for (const auto& f : faces) {
        const P3 n = faceNormal(p[f[0]], p[f[1]], p[f[2]]);
        if (n.y < -0.35) continue;  // underneath, in the ice: never seen
        (n.y > 0.6 ? snowFaces : iceFaces).addQuad(p[f[0]], p[f[1]], p[f[2]], p[f[3]]);
    }
}
}  // namespace

IceTile buildIceTile(const Tile& tile, const Anchor& anchor, const SeaIce& ice) {
    const Bounds b = tile.bounds();
    const int n = kIceMeshSize;
    const int season = iceSeason(ice.dayOfYear, ice.north);
    // The observation is 4 km coarse: read on a 5 x 5 lattice and blended,
    // which is also what makes two tiles agree along the edge they share.
    IceParameters lattice[5][5];
    for (int j = 0; j <= 4; ++j)
        for (int i = 0; i <= 4; ++i) {
            const auto s = ice.at(b.west + (b.east - b.west) * i / 4, b.south + (b.north - b.south) * j / 4);
            lattice[j][i] = {s.concentration, s.age};
        }
    auto parametersAt = [&](double lon, double lat) {
        const double u = clamp01((lon - b.west) / (b.east - b.west)) * 4, v = clamp01((lat - b.south) / (b.north - b.south)) * 4;
        const int i = std::min(3, int(u)), j = std::min(3, int(v));
        const double fu = u - i, fv = v - j;
        IceParameters p{0, 0};
        for (int dj = 0; dj < 2; ++dj)
            for (int di = 0; di < 2; ++di) {
                const double w = (di ? fu : 1 - fu) * (dj ? fv : 1 - fv);
                p.concentration += w * lattice[j + dj][i + di].concentration;
                p.age += w * lattice[j + dj][i + di].age;
            }
        return p;
    };

    IceTile out;
    out.grid.bounds = b;
    out.grid.size = n;
    out.grid.values.resize(size_t(n * n));
    std::vector<IcePoint> points(size_t(n * n));
    std::vector<P3> engine(size_t(n * n));
    double concentration = 0, age = 0;
    for (int r = 0; r < n; ++r) {
        const double lat = b.south + (b.north - b.south) * r / (n - 1);
        for (int c = 0; c < n; ++c) {
            const double lon = b.west + (b.east - b.west) * c / (n - 1);
            const IceParameters ip = parametersAt(lon, lat);
            concentration += ip.concentration; age += ip.age;
            const IcePoint pt = iceAt(lon, lat, ip, season);
            points[size_t(r * n + c)] = pt;
            out.grid.values[size_t(r * n + c)] = pt.height;
            engine[size_t(r * n + c)] = anchor.toEngine(lon, lat, pt.height);
        }
    }
    concentration /= n * n; age /= n * n;

    // The surface: one mesh over the whole grid, drawn with the snow, each
    // vertex tinted to what its point is -- a frozen pond, young ice in a
    // lead, bare ice where a ridge is too steep for snow -- so every change
    // of surface is a gradient rather than a staircase of triangles. Only
    // the floor of an open lead is its own mesh, under the water. A cell is
    // open water when two of its corners are: the cells the game swims in.
    const Swatches s = swatches();
    auto tintOf = [&](const Palette::IceSwatch& target) {
        std::array<double, 3> t{};
        for (int i = 0; i < 3; ++i) t[size_t(i)] = target.swatch.color[size_t(i)] / s.snow->swatch.color[size_t(i)];
        return t;
    };
    const auto bareTint = tintOf(*s.bare), youngTint = tintOf(*s.young), frozenTint = tintOf(*s.frozenPond),
               pondTint = tintOf(*s.pond);
    Mesh surface(UvMode::None), water;
    surface.positions = engine;
    surface.normals.assign(engine.size(), P3{0, 0, 0});
    for (const P3& p : engine) surface.texcoords.push_back({p.x, -p.z});
    int waterCells = 0, youngCells = 0, ridgeCells = 0, pondCells = 0;
    out.water.assign(size_t(n - 1), std::string(size_t(n - 1), '0'));
    auto uv = [](P3 p) { return UV{p.x, -p.z}; };
    for (int r = 0; r < n - 1; ++r)
        for (int c = 0; c < n - 1; ++c) {
            const size_t sw = size_t(r * n + c), se = sw + 1, ne = sw + size_t(n) + 1, nw = sw + size_t(n);
            int wet = 0, young = 0, ponds = 0;
            double ridge = 0;
            for (size_t k : {sw, se, ne, nw}) {
                wet += points[k].kind == IceKind::Water;
                young += points[k].kind == IceKind::Young;
                ponds += points[k].kind == IceKind::Pond || points[k].kind == IceKind::FrozenPond;
                ridge = std::max(ridge, points[k].ridge);
            }
            if (wet >= 2) { out.water[size_t(r)][size_t(c)] = '2'; ++waterCells; }
            youngCells += young >= 2; pondCells += ponds >= 2; ridgeCells += ridge > 0.3;
            for (auto tri : {std::array<size_t, 3>{sw, se, ne}, std::array<size_t, 3>{sw, ne, nw}}) {
                int drowned = 0;
                for (size_t k : tri) drowned += points[k].kind == IceKind::Water;
                P3 a = engine[tri[0]], b2 = engine[tri[1]], c2 = engine[tri[2]];
                if (drowned >= 2) {
                    const UV uvs[3] = {uv(a), uv(b2), uv(c2)};
                    water.addUpTriangle(a, b2, c2, uvs);
                    continue;
                }
                const P3 u{b2.x - a.x, b2.y - a.y, b2.z - a.z}, v{c2.x - a.x, c2.y - a.y, c2.z - a.z};
                P3 f{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
                if (f.y < 0) { std::swap(tri[1], tri[2]); f = {-f.x, -f.y, -f.z}; }
                for (size_t k : tri) {
                    surface.normals[k].x += f.x; surface.normals[k].y += f.y; surface.normals[k].z += f.z;
                }
                for (size_t k : tri) surface.indices.push_back(uint32_t(k));
            }
        }
    out.openWater = waterCells > 0;
    for (size_t k = 0; k < engine.size(); ++k) {
        P3& nn = surface.normals[k];
        const double length = std::sqrt(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
        nn = length > 1e-12 ? P3{nn.x / length, nn.y / length, nn.z / length} : P3{0, 1, 0};
        // Snow does not hold on a slope past some 30 degrees; a pond shows
        // through its shore; young ice fills a lead. In the freeze-up the
        // first snow already veils the frozen ponds.
        const double steep = clamp01((0.87 - nn.y) / 0.12);
        const double veil = season == 2 ? 0.5 : 1.0;
        const auto& pondTarget = season == 1 ? pondTint : frozenTint;
        const double young = points[k].kind == IceKind::Water ? 1.0 : points[k].young;
        std::array<double, 3> tint{};
        for (int i = 0; i < 3; ++i) {
            double t = lerp(1.0, bareTint[size_t(i)], std::max(steep, points[k].kind == IceKind::Bare ? 1.0 : 0.0));
            t = lerp(t, pondTarget[size_t(i)], points[k].pond * veil);
            t = lerp(t, youngTint[size_t(i)], young);
            tint[size_t(i)] = t;
        }
        surface.colors.push_back(tint);
    }
    out.parts.push_back({"Ground \xE2\x80\x94 " + s.snow->swatch.name, std::move(surface), iceMaterial(*s.snow)});
    if (!water.empty()) {
        const Swatch w = groundSwatch("water", *palette().generic);
        out.parts.push_back({"Ground \xE2\x80\x94 Water (lead)", smoothSurface(water),
                             surfaceMaterial(w.name, w.color, w.roughness, std::nullopt)});
    }

    // Pressure-ridge blocks: broken slabs as thick as the ice they came from,
    // heaped on the sail. The count is known before any is placed, so a busy
    // tile is thinned evenly rather than cut off at its last rows.
    const double spacing = 1.25;
    double expected = 0;
    std::vector<double> areas(size_t((n - 1) * (n - 1)), 0.0);
    for (int r = 0; r < n - 1; ++r)
        for (int c = 0; c < n - 1; ++c) {
            const size_t sw = size_t(r * n + c);
            double ridge = 0;
            for (size_t k : {sw, sw + 1, sw + size_t(n), sw + size_t(n) + 1}) ridge = std::max(ridge, points[k].ridge);
            if (ridge <= 0.3) continue;
            const P3 a = engine[sw], e = engine[sw + 1], nn = engine[sw + size_t(n)];
            const double area = std::abs((e.x - a.x) * (nn.z - a.z) - (e.z - a.z) * (nn.x - a.x));
            areas[size_t(r * (n - 1) + c)] = area;
            expected += area / (spacing * spacing) * 0.7;
        }
    const double keep = expected > kMaxRidgeBlocks ? kMaxRidgeBlocks / expected : 1.0;
    Mesh blockSnow(UvMode::Planar), blockIce(UvMode::Slope);
    int blocks = 0;
    const double thick = lerp(0.45, 1.0, age);
    for (int r = 0; r < n - 1; ++r)
        for (int c = 0; c < n - 1; ++c) {
            const double area = areas[size_t(r * (n - 1) + c)];
            if (area <= 0) continue;
            PyRandom rng = seeded((__int128(tile.row) << 32) | tile.col, (r * (n - 1) + c) * 7 + 3);
            const double want = area / (spacing * spacing) * keep;
            int count = int(want);
            if (rng.random() < want - count) ++count;
            for (int k = 0; k < count; ++k) {
                const double fu = rng.random(), fv = rng.random();
                const double lon = b.west + (b.east - b.west) * (c + fu) / (n - 1);
                const double lat = b.south + (b.north - b.south) * (r + fv) / (n - 1);
                const IcePoint pt = iceAt(lon, lat, parametersAt(lon, lat), season);
                const double crest = pt.ridgeHeight > 0 ? pt.ridge / pt.ridgeHeight : 0;
                const double u = rng.random();
                if (pt.ridge < 0.25 || u > 0.25 + 0.6 * crest) continue;
                const double t = thick * (0.6 + 0.7 * rng.random());
                const P3 size{0.8 + 1.7 * rng.random(), t, 0.6 + 1.2 * rng.random()};
                const double yaw = rng.random() * kTau;
                const double pitch = (rng.random() - 0.5) * 1.9 * (0.4 + 0.6 * crest);
                const double roll = (rng.random() - 0.5) * 0.8;
                addBlock(blockSnow, blockIce, anchor.toEngine(lon, lat, pt.height - t * 0.2), size, yaw, pitch, roll);
                ++blocks;
            }
        }
    if (!blockSnow.empty()) out.parts.push_back({"Pressure ridge \xE2\x80\x94 snow", blockSnow, iceMaterial(*s.snow)});
    if (!blockIce.empty()) out.parts.push_back({"Pressure ridge \xE2\x80\x94 ice", blockIce, iceMaterial(*s.bare)});

    const double cells = double((n - 1) * (n - 1));
    const char* seasons[] = {"winter", "melt", "freeze-up"};
    out.stats = {{"source", ice.source}, {"measured", ice.measured}, {"date", ice.date},
                 {"class", ice.classAt(tile.center().x, tile.center().y)},
                 {"concentration", pyround(concentration, 3)}, {"age", pyround(age, 3)},
                 {"season", seasons[season]}, {"cellsFilled", ice.filled},
                 {"openWater", pyround(waterCells / cells, 4)}, {"youngIce", pyround(youngCells / cells, 4)},
                 {"ridged", pyround(ridgeCells / cells, 4)}, {"ponds", pyround(pondCells / cells, 4)},
                 {"ridgeBlocks", blocks},
                 {"pack", "synthesised: floes, leads, ridges and dunes are drawn, not observed"}};
    return out;
}

std::vector<MeshPart> buildFarPack(const SeaIce& ice, double lon, double lat) {
    const Anchor anchor = Anchor::at(lon, lat, 0.0);
    const int rings = 44, segments = 128;
    const double inner = 150.0;
    std::vector<P3> points;
    std::vector<double> frozen;
    for (int i = 0; i < rings; ++i) {
        const double r = inner * std::pow(kFarPackRadius / inner, double(i) / (rings - 1));
        for (int k = 0; k < segments; ++k) {
            const double a = kTau * k / segments;
            const P3 g = anchor.toGeodetic(r * std::sin(a), 0.0, -r * std::cos(a));
            // Sunk a little more with distance: depth precision falls with
            // it, and a kilometre out the tiles must still win outright.
            // Two metres at 4 km lowers the horizon by three hundredths of
            // a degree.
            points.push_back(anchor.toEngine(g.x, g.y, kFarPackDepth - 0.5 * r / 1000.0));
            frozen.push_back(ice.at(g.x, g.y).concentration);
        }
    }
    Mesh snow, water;
    auto uv = [](P3 p) { return UV{p.x, -p.z}; };
    for (int i = 0; i + 1 < rings; ++i)
        for (int k = 0; k < segments; ++k) {
            const size_t a = size_t(i * segments + k), b = size_t(i * segments + (k + 1) % segments);
            const size_t c = b + size_t(segments), d = a + size_t(segments);
            const bool ice4 = frozen[a] + frozen[b] + frozen[c] + frozen[d] >= 2.0;
            Mesh& m = ice4 ? snow : water;
            const UV first[3] = {uv(points[a]), uv(points[b]), uv(points[c])};
            m.addUpTriangle(points[a], points[b], points[c], first);
            const UV second[3] = {uv(points[a]), uv(points[c]), uv(points[d])};
            m.addUpTriangle(points[a], points[c], points[d], second);
        }
    std::vector<MeshPart> parts;
    if (!snow.empty()) {
        const auto* s = swatches().snow;
        parts.push_back({"Far pack \xE2\x80\x94 snow", smoothSurface(snow), iceMaterial(*s)});
    }
    if (!water.empty()) {
        const Swatch w = groundSwatch("water", *palette().generic);
        parts.push_back({"Far pack \xE2\x80\x94 water", smoothSurface(water),
                         surfaceMaterial(w.name, w.color, w.roughness, std::nullopt)});
    }
    return parts;
}

}  // namespace r1
