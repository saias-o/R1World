#include "canopy.hpp"

#include "net.hpp"
#include "rangecoder.hpp"
#include "sources.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../../engine/third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

namespace fs = std::filesystem;

namespace r1 {

// ── the grid ────────────────────────────────────────────────────────────────

int Canopy::cellOf(double lon, double lat) const {
    const double u = (lon - bounds.west) / (bounds.east - bounds.west), v = (lat - bounds.south) / (bounds.north - bounds.south);
    if (u < 0 || u > 1 || v < 0 || v > 1) return -1;
    const int col = std::min(kCells - 1, int(u * kCells)), row = std::min(kCells - 1, int(v * kCells));
    return row * kCells + col;
}
Canopy::Class Canopy::classAt(double lon, double lat) const {
    const int i = cellOf(lon, lat);
    return i < 0 ? None : Class(cells[size_t(i)]);
}
P2 Canopy::centre(int col, int row) const {
    return {bounds.west + (col + 0.5) * (bounds.east - bounds.west) / kCells,
            bounds.south + (row + 0.5) * (bounds.north - bounds.south) / kCells};
}
int Canopy::count(Class c) const { return int(std::count(cells.begin(), cells.end(), uint8_t(c))); }

CanopyGrid::CanopyGrid(const Bounds& bounds) : bounds_(bounds), cells_(size_t(Canopy::kCells * Canopy::kCells)) {}

void CanopyGrid::add(int col, int row, uint8_t metres) {
    Cell& c = cells_[size_t(row * Canopy::kCells + col)];
    ++c.pixels;
    if (metres >= 3) ++c.tall;
    c.top = std::max(c.top, metres);
}

Canopy CanopyGrid::finish() const {
    Canopy out;
    out.bounds = bounds_;
    for (size_t i = 0; i < cells_.size(); ++i) {
        const Cell& c = cells_[i];
        // A crown of 3 m across is 7 m2, about 5 % of a cell: less than that
        // is a lamp post or a lorry, not a tree.
        if (c.pixels && c.tall >= std::max(3u, c.pixels / 20)) out.cells[i] = Canopy::Tree;
    }
    for (int by = 0; by < Canopy::kBlocks; ++by)
        for (int bx = 0; bx < Canopy::kBlocks; ++bx) {
            std::vector<uint8_t> tops;
            for (int y = by * Canopy::kBlock; y < (by + 1) * Canopy::kBlock; ++y)
                for (int x = bx * Canopy::kBlock; x < (bx + 1) * Canopy::kBlock; ++x)
                    if (out.at(x, y) == Canopy::Tree) tops.push_back(cells_[size_t(y * Canopy::kCells + x)].top);
            if (tops.empty()) continue;
            std::nth_element(tops.begin(), tops.begin() + long(tops.size() / 2), tops.end());
            out.heights[size_t(by * Canopy::kBlocks + bx)] = std::min<uint8_t>(63, std::max<uint8_t>(3, tops[tops.size() / 2]));
        }
    return out;
}

// ── the code ────────────────────────────────────────────────────────────────
//
// A binary range coder (LZMA's) over adaptive probabilities. The tree plane is
// coded cell by cell with the ten cells before it as context, as JBIG codes a
// page; then each block's height, where the block has trees.

namespace {
using rangecoder::Decoder;
using rangecoder::Encoder;
using rangecoder::kHalf;

constexpr int N = Canopy::kCells;
// The ten cells before (row, col) in scan order: two rows up, then this row.
int context(const std::array<uint8_t, N * N>& plane, int row, int col) {
    static const int offsets[10][2] = {{-2, -1}, {-2, 0}, {-2, 1}, {-1, -2}, {-1, -1}, {-1, 0}, {-1, 1}, {-1, 2}, {0, -2}, {0, -1}};
    int ctx = 0;
    for (const auto& o : offsets) {
        const int r = row + o[0], c = col + o[1];
        ctx = (ctx << 1) | ((r >= 0 && c >= 0 && c < N) ? plane[size_t(r * N + c)] : 0);
    }
    return ctx;
}
// The version byte, then flags: 1 trees, 4 no source.
constexpr uint8_t kCanopyVersion = 2;
}  // namespace

std::string encodeCanopy(const Canopy& c) {
    std::array<uint8_t, N * N> tree{};
    for (size_t i = 0; i < tree.size(); ++i) tree[i] = c.cells[i] == Canopy::Tree;
    const bool anyTree = std::find(tree.begin(), tree.end(), 1) != tree.end();
    std::string out;
    out.push_back(char(kCanopyVersion));
    out.push_back(char((anyTree ? 1 : 0) | (c.noSource ? 4 : 0)));
    if (!anyTree) return out;
    Encoder e;
    std::vector<uint16_t> treeP(1 << 10, kHalf), heightP(64, kHalf);
    if (anyTree)
        for (int r = 0; r < N; ++r) for (int col = 0; col < N; ++col) e.bit(treeP[size_t(context(tree, r, col))], tree[size_t(r * N + col)]);
    if (anyTree)
        for (int b = 0; b < Canopy::kBlocks * Canopy::kBlocks; ++b) {
            bool has = false;
            const int by = b / Canopy::kBlocks, bx = b % Canopy::kBlocks;
            for (int y = by * Canopy::kBlock; y < (by + 1) * Canopy::kBlock && !has; ++y)
                for (int x = bx * Canopy::kBlock; x < (bx + 1) * Canopy::kBlock && !has; ++x) has = tree[size_t(y * N + x)];
            if (!has) continue;
            const int h = std::min(63, int(c.heights[size_t(b)]));
            for (int i = 5, node = 1; i >= 0; --i) { const int bit = (h >> i) & 1; e.bit(heightP[size_t(node)], bit); node = node * 2 + bit; }
        }
    e.finish();
    return out + e.out;
}

Canopy decodeCanopy(const std::string& bytes, const Bounds& bounds) {
    if (bytes.size() < 2 || uint8_t(bytes[0]) != kCanopyVersion) throw std::runtime_error("canopy record of an unknown version");
    Canopy c;
    c.bounds = bounds;
    const uint8_t flags = uint8_t(bytes[1]);
    c.noSource = flags & 4;
    if (!(flags & 1)) return c;
    Decoder d(bytes, 2);
    std::array<uint8_t, N * N> tree{};
    std::vector<uint16_t> treeP(1 << 10, kHalf), heightP(64, kHalf);
    if (flags & 1)
        for (int r = 0; r < N; ++r) for (int col = 0; col < N; ++col) tree[size_t(r * N + col)] = uint8_t(d.bit(treeP[size_t(context(tree, r, col))]));
    for (size_t i = 0; i < tree.size(); ++i) c.cells[i] = tree[i] ? Canopy::Tree : Canopy::None;
    if (flags & 1)
        for (int b = 0; b < Canopy::kBlocks * Canopy::kBlocks; ++b) {
            bool has = false;
            const int by = b / Canopy::kBlocks, bx = b % Canopy::kBlocks;
            for (int y = by * Canopy::kBlock; y < (by + 1) * Canopy::kBlock && !has; ++y)
                for (int x = bx * Canopy::kBlock; x < (bx + 1) * Canopy::kBlock && !has; ++x) has = tree[size_t(y * N + x)];
            if (!has) continue;
            int node = 1;
            for (int i = 0; i < 6; ++i) node = node * 2 + d.bit(heightP[size_t(node)]);
            c.heights[size_t(b)] = uint8_t(node - 64);
        }
    return c;
}

// ── the source ──────────────────────────────────────────────────────────────
//
// Each source file is a zoom-9 Web Mercator tile named by its quadkey: 65536
// x 65536 one-byte heights of 1.19 m, one row per deflated strip with a
// horizontal predictor, in a BigTIFF. Only its header, the offsets of the
// rows needed and those rows are read.

namespace {
const char* kSource = "https://dataforgood-fb-data.s3.amazonaws.com/forests/v1/alsgedi_global_v6_float/chm/";
constexpr double kEarth = 6378137.0;
constexpr int kZoom = 9;

double mercY(double lat) { return kEarth * std::log(std::tan(kPi / 4 + radians(lat) / 2)); }
double lonOf(double x) { return x / kEarth * 180.0 / kPi; }
double latOf(double y) { return (2 * std::atan(std::exp(y / kEarth)) - kPi / 2) * 180.0 / kPi; }

struct Quad { int x, y; };
Quad quadAt(double lon, double lat) {
    const double n = double(1 << kZoom);
    const double s = std::sin(radians(std::clamp(lat, -85.0, 85.0)));
    const double x = (lon + 180.0) / 360.0, y = 0.5 - std::log((1 + s) / (1 - s)) / (4 * kPi);
    return {std::clamp(int(x * n), 0, (1 << kZoom) - 1), std::clamp(int(y * n), 0, (1 << kZoom) - 1)};
}
std::string quadkey(Quad q) {
    std::string k;
    for (int i = kZoom; i > 0; --i) {
        const int m = 1 << (i - 1);
        k += char('0' + ((q.x & m) ? 1 : 0) + ((q.y & m) ? 2 : 0));
    }
    return k;
}

// One byte range of a file; nullopt when the file does not exist.
std::optional<std::string> range(const std::string& url, uint64_t from, uint64_t length) {
    const std::string header = "Range: bytes=" + std::to_string(from) + "-" + std::to_string(from + length - 1);
    for (int attempt = 0; attempt < 3; ++attempt) {
        net::Response r;
        try {
            r = net::request("GET", url, {}, {}, 120.0, {header});
        } catch (const net::Unreachable& e) {
            throw SourceUnavailable(std::string("canopy source unreachable: ") + e.what());
        }
        if (r.status == 404 || r.status == 403) return std::nullopt;
        if (r.status == 206 && r.body.size() == length) return r.body;
        if (r.status == 200 && r.body.size() >= from + length) return r.body.substr(size_t(from), size_t(length));
        if (r.status != 500 && r.status != 503) throw SourceUnavailable("canopy source answered HTTP " + std::to_string(r.status));
    }
    throw SourceUnavailable("canopy source kept failing");
}

template <class T> T readAt(const std::string& s, size_t at) { T v; std::memcpy(&v, s.data() + at, sizeof v); return v; }

struct SourceFile {
    std::string url;
    double tieX = 0, tieY = 0, pixel = 0;
    uint64_t width = 0, height = 0, offsetsAt = 0, countsAt = 0;
    int offsetType = 16, countType = 16;
};

// The header of a source file (little-endian BigTIFF, one image).
std::optional<SourceFile> openSource(Quad q) {
    SourceFile f;
    f.url = kSource + quadkey(q) + ".tif";
    const auto head = range(f.url, 0, 16);
    if (!head) return std::nullopt;
    if (head->substr(0, 2) != "II" || readAt<uint16_t>(*head, 2) != 43)
        throw SourceUnavailable("canopy source is not a little-endian BigTIFF");
    const uint64_t ifd = readAt<uint64_t>(*head, 8);
    const auto countBytes = range(f.url, ifd, 8);
    const uint64_t n = readAt<uint64_t>(*countBytes, 0);
    const auto entries = range(f.url, ifd + 8, n * 20);
    int compression = 0, predictor = 1, bits = 0;
    for (uint64_t i = 0; i < n; ++i) {
        const size_t e = size_t(i * 20);
        const uint16_t tag = readAt<uint16_t>(*entries, e), type = readAt<uint16_t>(*entries, e + 2);
        const uint64_t count = readAt<uint64_t>(*entries, e + 4);
        const uint64_t value = readAt<uint64_t>(*entries, e + 12);
        auto small = [&]() -> uint64_t {
            return type == 3 ? readAt<uint16_t>(*entries, e + 12) : type == 4 ? readAt<uint32_t>(*entries, e + 12) : value;
        };
        switch (tag) {
            case 256: f.width = small(); break;
            case 257: f.height = small(); break;
            case 258: bits = int(small()); break;
            case 259: compression = int(small()); break;
            case 317: predictor = int(small()); break;
            case 273: f.offsetsAt = value; f.offsetType = type; (void)count; break;
            case 279: f.countsAt = value; f.countType = type; break;
            case 33550: { const auto s = range(f.url, value, 24); f.pixel = readAt<double>(*s, 0); break; }
            case 33922: { const auto s = range(f.url, value, 48); f.tieX = readAt<double>(*s, 24); f.tieY = readAt<double>(*s, 32); break; }
            default: break;
        }
    }
    if (bits != 8 || compression != 8 || predictor != 2 || !f.pixel || !f.offsetsAt || !f.countsAt)
        throw SourceUnavailable("canopy source layout changed (bits " + std::to_string(bits) + ", compression " +
                                std::to_string(compression) + ", predictor " + std::to_string(predictor) + ")");
    return f;
}

std::vector<uint64_t> readArray(const SourceFile& f, uint64_t at, int type, uint64_t first, uint64_t count) {
    const int size = type == 16 ? 8 : type == 4 ? 4 : 2;
    const auto bytes = range(f.url, at + first * size, count * size);
    if (!bytes) throw SourceUnavailable("canopy row offsets vanished from " + f.url);
    std::vector<uint64_t> out(count);
    for (uint64_t i = 0; i < count; ++i)
        out[i] = size == 8 ? readAt<uint64_t>(*bytes, i * 8) : size == 4 ? readAt<uint32_t>(*bytes, i * 4) : readAt<uint16_t>(*bytes, i * 2);
    return out;
}
}  // namespace

std::vector<std::pair<Tile, Canopy>> fetchCanopyBand(const Tile& tile) {
    const Bounds band = tile.bounds();
    const int n = columns(tile.row);
    const double width = 360.0 / n;
    // Every source file the band crosses: one or two rows of them (the band
    // is 0.005 degrees tall), across the requested tile's longitudes.
    const Quad sw = quadAt(band.west, band.south), ne = quadAt(band.east, band.north);
    struct Covered { double west, east; };
    std::map<int, CanopyGrid> grids;  // tile column -> grid
    std::map<int, std::vector<Covered>> byRow;
    for (int qy = ne.y; qy <= sw.y; ++qy)
        for (int qx = sw.x; qx <= ne.x; ++qx) {
            const auto file = openSource({qx, qy});
            const double qWest = lonOf(-kPi * kEarth + qx * 2 * kPi * kEarth / (1 << kZoom));
            const double qEast = lonOf(-kPi * kEarth + (qx + 1) * 2 * kPi * kEarth / (1 << kZoom));
            byRow[qy].push_back({qWest, qEast});
            if (!file) continue;  // no land here: every tile it covers stays empty
            const SourceFile& f = *file;
            const double top = std::min(mercY(band.north), f.tieY), bottom = std::max(mercY(band.south), f.tieY - f.pixel * f.height);
            if (top <= bottom) continue;
            const uint64_t r0 = uint64_t(std::max(0.0, std::floor((f.tieY - top) / f.pixel)));
            const uint64_t r1 = std::min<uint64_t>(f.height - 1, uint64_t(std::floor((f.tieY - bottom) / f.pixel)));
            const auto offsets = readArray(f, f.offsetsAt, f.offsetType, r0, r1 - r0 + 1);
            const auto counts = readArray(f, f.countsAt, f.countType, r0, r1 - r0 + 1);
            uint64_t from = UINT64_MAX, to = 0;
            for (size_t i = 0; i < offsets.size(); ++i) { from = std::min(from, offsets[i]); to = std::max(to, offsets[i] + counts[i]); }
            const auto data = range(f.url, from, to - from);
            if (!data) throw SourceUnavailable("canopy rows vanished from " + f.url);
            // Which tile and which cell column each pixel column falls in.
            std::vector<int> tileOf(size_t(f.width), -1), cellOf(size_t(f.width), 0);
            for (uint64_t c = 0; c < f.width; ++c) {
                const double lon = lonOf(f.tieX + (double(c) + 0.5) * f.pixel);
                const int col = int(std::floor((wrap(lon) + 180.0) / width));
                if (col < 0 || col >= n) continue;
                const double u = (wrap(lon) + 180.0 - col * width) / width;
                tileOf[size_t(c)] = col;
                cellOf[size_t(c)] = std::min(Canopy::kCells - 1, int(u * Canopy::kCells));
            }
            std::vector<uint8_t> row(size_t(f.width));
            for (uint64_t r = r0; r <= r1; ++r) {
                const double lat = latOf(f.tieY - (double(r) + 0.5) * f.pixel);
                if (lat < band.south || lat >= band.north) continue;
                const int cellRow = std::min(Canopy::kCells - 1, int((lat - band.south) / (band.north - band.south) * Canopy::kCells));
                const size_t i = size_t(r - r0);
                const int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(row.data()), int(row.size()),
                                                        data->data() + (offsets[i] - from), int(counts[i]));
                if (got != int(row.size())) throw SourceUnavailable("a canopy row did not inflate");
                for (size_t c = 1; c < row.size(); ++c) row[c] = uint8_t(row[c] + row[c - 1]);
                int current = -1;
                CanopyGrid* grid = nullptr;
                for (size_t c = 0; c < row.size(); ++c) {
                    const int t = tileOf[c];
                    if (t < 0 || row[c] == 255) continue;
                    if (t != current) {
                        current = t;
                        const Bounds b = Tile{tile.row, t}.bounds();
                        grid = &grids.try_emplace(t, b).first->second;
                    }
                    grid->add(cellOf[c], cellRow, row[c]);
                }
            }
        }
    // A tile is converted when every source file under it was read (or
    // found to be sea): its longitudes lie inside each source row's span.
    auto covered = [&](double west, double east) {
        for (const auto& [qy, list] : byRow) {
            double lo = 1e9, hi = -1e9;
            for (const auto& s : list) { lo = std::min(lo, s.west); hi = std::max(hi, s.east); }
            if (west < lo - 1e-12 || east > hi + 1e-12) return false;
        }
        return true;
    };
    std::vector<std::pair<Tile, Canopy>> out;
    double lo = 1e9, hi = -1e9;
    for (const auto& [qy, list] : byRow) for (const auto& s : list) { lo = std::min(lo, s.west); hi = std::max(hi, s.east); }
    for (int col = int(std::floor((lo + 180.0) / width)); col <= int(std::floor((hi + 180.0) / width)); ++col) {
        if (col < 0 || col >= n) continue;
        const Tile t{tile.row, col};
        const Bounds b = t.bounds();
        if (!covered(b.west, b.east)) continue;
        auto it = grids.find(col);
        Canopy c = it != grids.end() ? it->second.finish() : Canopy{};
        c.bounds = b;
        if (it == grids.end()) c.noSource = true;
        out.push_back({t, c});
    }
    return out;
}

// ── the store ───────────────────────────────────────────────────────────────

namespace {
constexpr char kMagic[8] = {'R', '1', 'C', 'A', 'N', 'O', 'P', 'Y'};
std::mutex storeLock;
std::map<std::string, std::map<std::pair<int, int>, std::string>> regions;  // path -> (row, col) -> record

std::map<std::pair<int, int>, std::string>& region(const std::string& path) {
    auto it = regions.find(path);
    if (it != regions.end()) return it->second;
    auto& records = regions[path];
    std::ifstream in(path, std::ios::binary);
    if (!in) return records;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 8 || data.compare(0, 8, kMagic, 8) != 0) return records;
    // A record cut short by a crash is the file's end, not an error.
    for (size_t at = 8; at + 10 <= data.size();) {
        const int32_t row = readAt<int32_t>(data, at), col = readAt<int32_t>(data, at + 4);
        const uint16_t length = readAt<uint16_t>(data, at + 8);
        if (at + 10 + length > data.size()) break;
        records[{row, col}] = data.substr(at + 10, length);
        at += 10 + length;
    }
    return records;
}
}  // namespace

std::string canopyRegionPath(const std::string& root, const Tile& tile) {
    const P2 c = tile.center();
    return root + "/cache/world/canopy/" + std::to_string(int(std::floor(c.y))) + "_" + std::to_string(int(std::floor(c.x))) + ".r1c";
}

std::optional<Canopy> storedCanopy(const std::string& root, const Tile& tile) {
    const std::string path = canopyRegionPath(root, tile);
    std::lock_guard<std::mutex> guard(storeLock);
    auto& records = region(path);
    auto it = records.find({tile.row, tile.col});
    if (it == records.end()) return std::nullopt;
    if (it->second.size() < 2 || uint8_t(it->second[0]) != kCanopyVersion) return std::nullopt;
    return decodeCanopy(it->second, tile.bounds());
}

void storeCanopy(const std::string& root, const Tile& tile, const Canopy& canopy) {
    const std::string path = canopyRegionPath(root, tile);
    const std::string bytes = encodeCanopy(canopy);
    if (bytes.size() > 0xFFFF) throw std::runtime_error("canopy record too long");
    std::lock_guard<std::mutex> guard(storeLock);
    auto& records = region(path);
    const auto existing = records.find({tile.row, tile.col});
    if (existing != records.end() && existing->second.size() >= 2 &&
        uint8_t(existing->second[0]) == kCanopyVersion) return;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const bool fresh = !fs::exists(path, ec) || fs::file_size(path, ec) < 8;
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("cannot write " + path);
    if (fresh) out.write(kMagic, 8);
    const int32_t row = tile.row, col = tile.col;
    const uint16_t length = uint16_t(bytes.size());
    out.write(reinterpret_cast<const char*>(&row), 4);
    out.write(reinterpret_cast<const char*>(&col), 4);
    out.write(reinterpret_cast<const char*>(&length), 2);
    out.write(bytes.data(), std::streamsize(bytes.size()));
    records[{tile.row, tile.col}] = bytes;
}

}  // namespace r1
