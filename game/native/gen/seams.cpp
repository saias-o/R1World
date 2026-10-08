#include "seams.hpp"

#include "relief.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace r1 {

int groundRank(const std::string& source) {
    if (source.rfind("IGN", 0) == 0) return 5;
    if (source.rfind("Mapzen", 0) == 0) return source.find(" z13 ") != std::string::npos ? 4 : 3;
    if (source.rfind("Copernicus", 0) == 0) return 2;
    if (source == kInstalledReliefSource) return 1;
    return 0;
}

namespace {

constexpr int kSeamTaps = 129;
constexpr int kNodes = kTerrainMeshSize;

double nodeLon(const Bounds& b, int i) { return b.west + (b.east - b.west) * i / (kNodes - 1); }
double nodeLat(const Bounds& b, int j) { return b.south + (b.north - b.south) * j / (kNodes - 1); }
double rowCentre(int row) { return -90.0 + (row + 0.5) * kStep; }

// Both rows use the union of their node longitudes, including tile corners.
// Use global node indices so neighbours calculate exactly the same knots.
std::vector<double> seamKnots(const Bounds& b, int below) {
    std::vector<double> knots{b.west, b.east};
    for (int row : {below, below + 1}) {
        if (row < 0 || row >= kRows) continue;
        const int count = columns(row) * (kNodes - 1);
        const double step = 360.0 / count;
        const int first = int(std::floor((b.west + 180.0) / step));
        const int last = int(std::ceil((b.east + 180.0) / step));
        for (int k = first; k <= last; ++k) {
            const double lon = -180.0 + k * step;
            if (lon > b.west + 1e-10 && lon < b.east - 1e-10) knots.push_back(lon);
        }
    }
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end(), [](double a, double c) {
        return std::abs(a - c) < 1e-10;
    }), knots.end());
    return knots;
}

// One height and the grade of the survey that gave it.
struct Height { double value; int rank; };

// The better survey, or half-way between two of the same grade. Symmetric,
// so the two tiles of a seam reach the same number.
Height meet(const Height& a, const Height& b) {
    if (a.rank != b.rank) return a.rank > b.rank ? a : b;
    return {(a.value + b.value) * 0.5, a.rank};
}

class Stitcher {
public:
    Stitcher(const Tile& tile, const RankedGround& own, const GroundOf& around)
        : tile_(tile), own_(own), around_(around) {}

    const RankedGround* ground(const Tile& t) {
        if (t == tile_) return &own_;
        auto it = known_.find(t);
        if (it == known_.end()) it = known_.emplace(t, around_ ? around_(t) : std::nullopt).first;
        return it->second ? &*it->second : nullptr;
    }
    int neighbours() const {
        int n = 0;
        for (const auto& [t, g] : known_) n += g.has_value();
        return n;
    }

    // The straight edge `t` draws along its north or south side, at `lon`.
    static double edge(const Tile& t, const RankedGround& g, bool north, double lon) {
        const Bounds b = t.bounds();
        while (lon < b.west - 180.0) lon += 360.0;
        while (lon > b.west + 180.0) lon -= 360.0;
        const double lat = north ? b.north : b.south;
        const double u = std::clamp((lon - b.west) / (b.east - b.west), 0.0, 1.0) * (kNodes - 1);
        const int i = std::min(kNodes - 2, int(u));
        const double f = u - i;
        return g.grid.sample(nodeLon(b, i), lat) * (1 - f) + g.grid.sample(nodeLon(b, i + 1), lat) * f;
    }

    // What row `row` draws along its north (or south) edge at `lon`.
    std::optional<Height> rowEdge(int row, bool north, double lon) {
        if (row < 0 || row >= kRows) return std::nullopt;
        const Tile t = tileAt(lon, rowCentre(row));
        const RankedGround* g = ground(t);
        if (!g) return std::nullopt;
        return Height{edge(t, *g, north, lon), g->rank};
    }

    // A smoothing tap can extend beyond both available tiles at a corner.
    // Hold the closest known edge from either row, rather than this cook's
    // own edge: the fallback must be independent of which side is cooking.
    std::optional<Height> nearestEdge(int below, double lon) {
        std::optional<Height> best;
        double distance = 1e30;
        for (int row : {below, below+1}) {
            if (row<0 || row>=kRows)continue;
            const int count=columns(row), col=tileAt(lon,rowCentre(row)).col;
            for (int dc : {-1,0,1}) {
                const Tile t{row,(col+dc+count)%count};
                const auto* g=ground(t);if(!g)continue;
                const Bounds b=t.bounds();
                const double x=(b.west+b.east)/2+wrap(lon-(b.west+b.east)/2);
                const double at=std::clamp(x,b.west,b.east),d=std::abs(x-at);
                const Height h{edge(t,*g,row==below,at),g->rank};
                if(d<distance-1e-10){best=h;distance=d;}
                else if(best && std::abs(d-distance)<1e-10)best=meet(*best,h);
            }
        }
        return best;
    }

    // The seam between row `below` and the row above it, before smoothing.
    double seamPoint(int below, double lon) {
        const auto low = rowEdge(below, true, lon), high = rowEdge(below + 1, false, lon);
        if (low && high) return meet(*low, *high).value;
        if (low) return low->value;
        if (high) return high->value;
        const auto nearest=nearestEdge(below,lon);
        return nearest ? nearest->value : 0.;
    }

    // The seam, smoothed: a curve both rows' straight edges can follow.
    double seam(int below, double lon) {
        const double lat = -90.0 + (below + 1) * kStep;
        const double reach = kSeamSmoothing / (kMetresPerDegree * std::max(1e-3, std::cos(radians(lat))));
        double sum = 0, weights = 0;
        for (int k = 0; k < kSeamTaps; ++k) {
            const double t = -1.0 + 2.0 * k / (kSeamTaps - 1);
            const double w = 1.0 - std::abs(t);
            if (w <= 0) continue;
            sum += w * seamPoint(below, lon + t * reach);
            weights += w;
        }
        return sum / weights;
    }

    // The seam on the east (or west) side, at the tile's own node `j`. The two
    // tiles of a row share these nodes, so they meet exactly; the ends are
    // bent to the row seams so that the four sides close at the corners.
    double side(bool east, int j, double southEnd, double northEnd) {
        const Bounds b = tile_.bounds();
        const int columns = r1::columns(tile_.row);
        const Tile other{tile_.row, (tile_.col + (east ? 1 : columns - 1)) % columns};
        const double lon = east ? b.east : b.west;
        auto at = [&](int node) {
            const Height mine{own_.grid.sample(lon, nodeLat(b, node)), own_.rank};
            const RankedGround* g = other != tile_ ? ground(other) : nullptr;
            if (!g) return mine.value;
            const Bounds ob = other.bounds();
            return meet(mine, {g->grid.sample(east ? ob.west : ob.east, nodeLat(b, node)), g->rank}).value;
        };
        const double v = double(j) / (kNodes - 1);
        return at(j) + (1 - v) * (southEnd - at(0)) + v * (northEnd - at(kNodes - 1));
    }

private:
    Tile tile_;
    const RankedGround& own_;
    const GroundOf& around_;
    std::map<Tile, std::optional<RankedGround>> known_;
};

// 1 at the edge, 0 from kSeamBand inward, smooth between.
double fade(double t) {
    if (t >= kSeamBand) return 0.0;
    const double s = 1.0 - t / kSeamBand;
    return s * s * (3.0 - 2.0 * s);
}

}  // namespace

ElevationGrid stitchedGround(const Tile& tile, const RankedGround& own, const GroundOf& around, SeamReport* report) {
    const Bounds b = tile.bounds();
    Stitcher s(tile, own, around);
    std::vector<double> south(kNodes), north(kNodes), west(kNodes), east(kNodes);
    for (int i = 0; i < kNodes; ++i) {
        south[size_t(i)] = s.seam(tile.row - 1, nodeLon(b, i));
        north[size_t(i)] = s.seam(tile.row, nodeLon(b, i));
    }
    for (int j = 0; j < kNodes; ++j) {
        west[size_t(j)] = s.side(false, j, south.front(), north.front());
        east[size_t(j)] = s.side(true, j, south.back(), north.back());
    }
    // The corners are the row seams' own values, to the last bit.
    west.front() = south.front(); west.back() = north.front();
    east.front() = south.back(); east.back() = north.back();

    auto raw = [&](int i, int j) { return own.grid.sample(nodeLon(b, i), nodeLat(b, j)); };
    std::vector<double> dS(kNodes), dN(kNodes), dW(kNodes), dE(kNodes);
    for (int k = 0; k < kNodes; ++k) {
        dS[size_t(k)] = south[size_t(k)] - raw(k, 0);
        dN[size_t(k)] = north[size_t(k)] - raw(k, kNodes - 1);
        dW[size_t(k)] = west[size_t(k)] - raw(0, k);
        dE[size_t(k)] = east[size_t(k)] - raw(kNodes - 1, k);
    }
    const size_t last = size_t(kNodes - 1);
    ElevationGrid out{b, kNodes, {}};
    for (double lon : seamKnots(b, tile.row - 1)) out.southEdge.push_back({lon, s.seam(tile.row - 1, lon)});
    for (double lon : seamKnots(b, tile.row)) out.northEdge.push_back({lon, s.seam(tile.row, lon)});
    out.values.reserve(size_t(kNodes) * kNodes);
    double largest = 0;
    for (int j = 0; j < kNodes; ++j)
        for (int i = 0; i < kNodes; ++i) {
            const double u = double(i) / (kNodes - 1), v = double(j) / (kNodes - 1);
            const double fs = fade(v), fn = fade(1 - v), fw = fade(u), fe = fade(1 - u);
            // A Coons patch of the four sides' corrections, each fading inward:
            // it reproduces every side exactly and the corners once.
            const double d = fs * dS[size_t(i)] + fn * dN[size_t(i)] + fw * dW[size_t(j)] + fe * dE[size_t(j)] -
                             (fs * fw * dS[0] + fs * fe * dS[last] + fn * fw * dN[0] + fn * fe * dN[last]);
            const bool corner = (i == 0 || i == kNodes - 1) && (j == 0 || j == kNodes - 1);
            double h = raw(i, j) + d;
            if (corner) h = j == 0 ? south[size_t(i)] : north[size_t(i)];
            else if (j == 0) h = south[size_t(i)];
            else if (j == kNodes - 1) h = north[size_t(i)];
            else if (i == 0) h = west[size_t(j)];
            else if (i == kNodes - 1) h = east[size_t(j)];
            largest = std::max(largest, std::abs(h - raw(i, j)));
            out.values.push_back(h);
        }
    for (bool northSide : {false,true})
        for (const auto& p : northSide ? out.northEdge : out.southEdge)
            largest = std::max(largest, std::abs(p.y-own.grid.sample(p.x,northSide ? b.north : b.south)));
    if (report) *report = {largest, s.neighbours()};
    return out;
}

JoinedGround joinedGround(const Tile& tile, const RankedGround& own, const GroundOf& neighbour) {
    std::map<Tile, std::optional<RankedGround>> memo{{tile, own}};
    const GroundOf known = [&](const Tile& t) {
        auto it = memo.find(t);
        if (it == memo.end()) it = memo.emplace(t, neighbour ? neighbour(t) : std::nullopt).first;
        return it->second;
    };
    JoinedGround out;
    out.own = stitchedGround(tile, own, known, &out.seams);
    const double width = tile.bounds().east - tile.bounds().west;
    for (int dr = -1; dr <= 1; ++dr) {
        const int row = tile.row + dr;
        if (row < 0 || row >= kRows) continue;
        for (int dc = -1; dc <= 1; ++dc) {
            const Tile n = tileAt(tile.center().x + dc * width, rowCentre(row));
            if (n == tile) continue;
            if (const auto g = known(n)) out.around.push_back(stitchedGround(n, *g, known));
        }
    }
    return out;
}

}  // namespace r1
