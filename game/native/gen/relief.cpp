#include "relief.hpp"

#include "rangecoder.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <cmath>
#include <climits>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace r1 {

namespace {
using rangecoder::Decoder;
using rangecoder::Encoder;
using rangecoder::kHalf;

constexpr uint8_t kReliefVersion = 1;
constexpr char kPackMagic[4] = {'R', '1', 'R', 'F'};
constexpr int kPackCells = kPackDegrees * kPackDegrees;
constexpr size_t kPackHeader = sizeof(kPackMagic) + 1 + size_t(kPackCells) * 2 * sizeof(uint32_t);

// The ground's roughness around a sample, as the bit length of the
// neighbours' differences: flat sea and plain in the first contexts, cliffs
// in the last.
constexpr int kContexts = 12;
// Bit length of the largest error: the full height range.
constexpr int kMaxLength = 15;
// At most one zero/sign bit and two bits per magnitude bit; each event
// costs at most the probability precision, plus the coder's final state.
constexpr size_t kMaxCellBits = size_t(kRowsPerDegree + 1) * size_t(kRowsPerDegree + 1) *
    (2 * kMaxLength + 1) * rangecoder::kProbBits;
constexpr size_t kMaxCellBytes = (kMaxCellBits + CHAR_BIT - 1) / CHAR_BIT +
    rangecoder::kFlushBytes + sizeof(kReliefVersion);

struct Model {
    std::vector<uint16_t> zero = std::vector<uint16_t>(kContexts, kHalf);
    std::vector<uint16_t> sign = std::vector<uint16_t>(kContexts, kHalf);
    std::vector<uint16_t> length = std::vector<uint16_t>(size_t(kContexts) * kMaxLength, kHalf);
    std::vector<uint16_t> mantissa = std::vector<uint16_t>(size_t(kMaxLength + 1) * kMaxLength, kHalf);
};

int bitLength(uint32_t v) {
    int n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

// LOCO-I's median edge detector: the west or south neighbour across an edge,
// their plane elsewhere.
int predict(int west, int south, int southWest) {
    if (southWest >= std::max(west, south)) return std::min(west, south);
    if (southWest <= std::min(west, south)) return std::max(west, south);
    return west + south - southWest;
}

// The prediction and the context of sample (row, column), from the samples
// already coded (rows below, and this row to the west).
template <class At>
std::pair<int, int> predictionAt(const At& at, int row, int column, int columns) {
    if (row == 0) return {column == 0 ? 0 : at(row, column - 1), 0};
    const int south = at(row - 1, column);
    if (column == 0) return {south, 0};
    const int west = at(row, column - 1), southWest = at(row - 1, column - 1);
    const int southEast = column + 1 < columns ? at(row - 1, column + 1) : south;
    const uint32_t rough = uint32_t(std::abs(west - southWest) + std::abs(south - southWest) + std::abs(south - southEast));
    return {predict(west, south, southWest), std::min(kContexts - 1, bitLength(rough))};
}

void writeU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xFF));
}
uint32_t readU32(const std::string& in, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(in[at + size_t(i)])) << (8 * i);
    return v;
}

int floorTo(int v, int step) { return v >= 0 ? v / step * step : -((-v + step - 1) / step) * step; }

std::vector<std::pair<uint32_t, uint32_t>> readPackIndex(std::ifstream& in) {
    in.seekg(0, std::ios::end);
    const auto fileSize = in.tellg();
    if (fileSize < std::streamoff(kPackHeader)) throw std::runtime_error("short relief pack header");
    in.seekg(0);
    std::string header(kPackHeader, '\0');
    if (!in.read(header.data(), std::streamsize(header.size())) ||
        header.compare(0, sizeof(kPackMagic), std::string(kPackMagic, sizeof(kPackMagic))) != 0 ||
        uint8_t(header[sizeof(kPackMagic)]) != kReliefVersion)
        throw std::runtime_error("invalid relief pack version or magic");
    std::vector<std::pair<uint32_t, uint32_t>> index;
    index.reserve(kPackCells);
    uint64_t end = kPackHeader;
    for (int i = 0; i < kPackCells; ++i) {
        const size_t at = sizeof(kPackMagic) + 1 + size_t(i) * 2 * sizeof(uint32_t);
        const uint32_t offset = readU32(header, at), length = readU32(header, at + sizeof(uint32_t));
        if ((!length && offset) || (length && (offset != end || length > kMaxCellBytes)))
            throw std::runtime_error("invalid relief cell span");
        end += length;
        if (end > uint64_t(fileSize)) throw std::runtime_error("relief pack shorter than its index");
        index.emplace_back(offset, length);
    }
    if (end != uint64_t(fileSize)) throw std::runtime_error("unindexed relief pack bytes");
    return index;
}
}  // namespace

int columnsFor(int south) {
    const double nearest = south >= 0 ? south : std::min(0, south + 1);  // the edge nearest the equator
    return std::max(1, int(std::ceil(kRowsPerDegree * std::cos(radians(std::abs(nearest))))));
}

double ReliefCell::sample(double lon, double lat) const {
    const int width = columns();
    const double u = std::clamp((lon - west) * (width - 1), 0.0, double(width - 1));
    const double v = std::clamp((lat - south) * kRowsPerDegree, 0.0, double(rows() - 1));
    const int c0 = std::min(int(u), width - 2), r0 = std::min(int(v), rows() - 2);
    const double fu = u - c0, fv = v - r0;
    const size_t lowAt = size_t(r0) * size_t(width) + size_t(c0), highAt = lowAt + size_t(width);
    const double low = heights[lowAt] * (1.0 - fu) + heights[lowAt + 1] * fu;
    const double high = heights[highAt] * (1.0 - fu) + heights[highAt + 1] * fu;
    return low * (1.0 - fv) + high * fv;
}

bool ReliefCell::hasLand() const {
    return std::any_of(heights.begin(), heights.end(), [](int16_t h) { return h > 0; });
}

std::string encodeReliefCell(const ReliefCell& cell) {
    const int rows = cell.rows(), columns = cell.columns();
    if (cell.heights.size() != size_t(rows) * size_t(columns))
        throw std::invalid_argument("relief cell: " + std::to_string(cell.heights.size()) + " heights for " +
                                    std::to_string(rows) + " x " + std::to_string(columns));
    auto at = [&](int r, int c) {
        return std::clamp<int>(cell.heights[size_t(r) * size_t(columns) + size_t(c)], kLowestHeight, kHighestHeight);
    };
    Model m;
    Encoder e;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            const auto [guess, ctx] = predictionAt(at, r, c, columns);
            const int error = at(r, c) - guess;
            e.bit(m.zero[size_t(ctx)], error == 0);
            if (error == 0) continue;
            e.bit(m.sign[size_t(ctx)], error < 0);
            const uint32_t magnitude = uint32_t(std::abs(error));
            const int n = bitLength(magnitude);
            for (int i = 1; i < n; ++i) e.bit(m.length[size_t(ctx * kMaxLength + i)], 1);
            if (n < kMaxLength) e.bit(m.length[size_t(ctx * kMaxLength + n)], 0);
            for (int i = n - 2; i >= 0; --i) e.bit(m.mantissa[size_t(n * kMaxLength + i)], int((magnitude >> i) & 1u));
        }
    e.finish();
    return std::string(1, char(kReliefVersion)) + e.out;
}

ReliefCell decodeReliefCell(const std::string& bytes, int south, int west) {
    if (bytes.empty() || uint8_t(bytes[0]) != kReliefVersion)
        throw std::runtime_error("relief cell " + std::to_string(south) + "," + std::to_string(west) +
                                 ": not version " + std::to_string(kReliefVersion));
    ReliefCell cell{south, west, {}};
    const int rows = cell.rows(), columns = cell.columns();
    cell.heights.assign(size_t(rows) * size_t(columns), 0);
    auto at = [&](int r, int c) { return int(cell.heights[size_t(r) * size_t(columns) + size_t(c)]); };
    Model m;
    Decoder d(bytes, 1);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            const auto [guess, ctx] = predictionAt(at, r, c, columns);
            int error = 0;
            if (!d.bit(m.zero[size_t(ctx)])) {
                const bool negative = d.bit(m.sign[size_t(ctx)]);
                int n = 1;
                while (n < kMaxLength && d.bit(m.length[size_t(ctx * kMaxLength + n)])) ++n;
                uint32_t magnitude = 1;
                for (int i = n - 2; i >= 0; --i)
                    magnitude = (magnitude << 1) | uint32_t(d.bit(m.mantissa[size_t(n * kMaxLength + i)]));
                error = negative ? -int(magnitude) : int(magnitude);
            }
            const int h = guess + error;
            if (h < kLowestHeight || h > kHighestHeight) throw std::runtime_error("relief sample outside height range");
            cell.heights[size_t(r) * size_t(columns) + size_t(c)] = int16_t(h);
        }
    return cell;
}

std::string packFileName(int south, int west) {
    return std::to_string(south) + "_" + std::to_string(west) + ".r1relief";
}

std::string encodeReliefPack(const std::vector<std::optional<std::string>>& cells) {
    if (cells.size() != size_t(kPackCells)) throw std::invalid_argument("relief pack: not one entry a cell");
    std::string out(kPackMagic, sizeof(kPackMagic));
    out.push_back(char(kReliefVersion));
    uint32_t offset = uint32_t(kPackHeader);
    for (const auto& c : cells) {
        if (c && (c->empty() || c->size() > kMaxCellBytes)) throw std::invalid_argument("invalid relief cell size");
        const uint32_t length = c ? uint32_t(c->size()) : 0u;
        writeU32(out, length ? offset : 0u);
        writeU32(out, length);
        offset += length;
    }
    for (const auto& c : cells)
        if (c) out += *c;
    return out;
}

bool validReliefPackFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    try { readPackIndex(in); return true; }
    catch (const std::exception&) { return false; }
}

ReliefLayer::ReliefLayer(std::string directory) : directory_(std::move(directory)) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory_, ec))
        if (entry.path().extension() == ".r1relief") { installed_ = true; break; }
}

const ReliefLayer::Pack* ReliefLayer::pack(int south, int west) const {
    const auto key = std::make_pair(south, west);
    if (auto it = packs_.find(key); it != packs_.end()) return it->second ? &*it->second : nullptr;
    std::optional<Pack> loaded;
    const std::string path = directory_ + "/" + packFileName(south, west);
    std::ifstream in(path, std::ios::binary);
    if (in) loaded = Pack{path, readPackIndex(in)};
    return (packs_[key] = std::move(loaded)) ? &*packs_[key] : nullptr;
}

std::shared_ptr<const ReliefCell> ReliefLayer::cell(int south, int west) const {
    const auto key = std::make_pair(south, west);
    std::pair<uint32_t, uint32_t> span;
    std::string path;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto it = cells_.begin(); it != cells_.end(); ++it)
            if (it->first == key) {
                cells_.splice(cells_.begin(), cells_, it);
                return it->second;
            }
        const int packSouth = floorTo(south, kPackDegrees), packWest = floorTo(west, kPackDegrees);
        const Pack* p = pack(packSouth, packWest);
        if (!p) throw std::out_of_range("no relief pack");
        span = p->index[size_t((south - packSouth) * kPackDegrees + (west - packWest))];
        path = p->path;
    }
    std::shared_ptr<const ReliefCell> decoded;
    if (span.second) {
        std::ifstream in(path, std::ios::binary);
        std::string bytes(span.second, '\0');
        in.seekg(std::streamoff(span.first));
        if (!in.read(bytes.data(), std::streamsize(bytes.size())))
            throw std::runtime_error("relief pack " + path + " is shorter than its index");
        decoded = std::make_shared<const ReliefCell>(decodeReliefCell(bytes, south, west));
    }
    std::lock_guard<std::mutex> guard(lock_);
    for (auto it = cells_.begin(); it != cells_.end(); ++it)
        if (it->first == key) {
            cells_.splice(cells_.begin(), cells_, it);
            return it->second;
        }
    cells_.emplace_front(key, decoded);
    if (cells_.size() > kCachedCells) cells_.pop_back();
    return decoded;
}

const ReliefLayer& installedRelief(const std::string& gameRoot) {
    static std::mutex lock;
    static std::map<std::string, std::unique_ptr<ReliefLayer>> layers;
    std::lock_guard<std::mutex> guard(lock);
    auto& layer = layers[gameRoot];
    if (!layer) layer = std::make_unique<ReliefLayer>(gameRoot + "/" + kReliefDirectory);
    return *layer;
}

std::optional<ElevationGrid> installedGround(const Bounds& bounds, const std::string& gameRoot) {
    const ReliefLayer& layer = installedRelief(gameRoot);
    ElevationGrid grid{bounds, kTerrainMeshSize, {}};
    grid.values.reserve(size_t(kTerrainMeshSize) * kTerrainMeshSize);
    for (int r = 0; r < kTerrainMeshSize; ++r)
        for (int c = 0; c < kTerrainMeshSize; ++c) {
            const auto h = layer.height(bounds.west + (bounds.east - bounds.west) * c / (kTerrainMeshSize - 1),
                                        bounds.south + (bounds.north - bounds.south) * r / (kTerrainMeshSize - 1));
            if (!h) return std::nullopt;
            grid.values.push_back(*h);
        }
    return grid;
}

std::optional<double> ReliefLayer::height(double lon, double lat) const {
    if (!installed_ || !std::isfinite(lon) || !std::isfinite(lat) || std::abs(lat) >= kReliefLatitudeLimit) return std::nullopt;
    const double x = wrap(lon);
    const int south = int(std::floor(lat)), west = std::min(179, int(std::floor(x)));
    try {
        const auto c = cell(south, west);
        const double north = south + 1.0;
        const double lastRow = north - 1.0 / kRowsPerDegree;
        if (lat > lastRow && north < kReliefLatitudeLimit) {
            // Adjacent latitude grids have different column counts. Using
            // one shared edge curve avoids a step at the degree boundary.
            const auto next = cell(south + 1, west);
            const double edge = next ? next->sample(x, north) : 0.0;
            const double base = c ? c->sample(x, lastRow) : 0.0;
            const double blend = (lat - lastRow) * kRowsPerDegree;
            return base * (1.0 - blend) + edge * blend;
        }
        return c ? c->sample(x, lat) : 0.0;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

}  // namespace r1
