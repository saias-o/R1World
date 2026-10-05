// Build the planet's relief layer (gen/relief) from the Terrain Tiles: the
// data the game installs so the far relief stands everywhere, offline too.
//
//   r1relief [--out <dir>] [--region <south> <west> <north> <east>] [--threads <n>] [--measure]
//
// Pack by pack (kPackDegrees square), it first surveys the pack at
// kSurveyZoom to find the square degrees that hold land, then reads only
// those at r1::kReliefZoom, codes them and writes the pack. Nothing else is kept:
// the images are read into memory and dropped. A pack already written is not
// built again, so an interrupted build resumes where it stopped. `--measure`
// writes nothing and reports what the packs would weigh.
#include "gen/net.hpp"
#include "gen/relief.hpp"
#include "gen/sources.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace {
// The survey's zoom: a pixel of about 1.2 km. The layer is read at
// r1::kReliefZoom (about 300 m at the equator, finer toward the poles).
constexpr int kSurveyZoom = 7;
// Survey samples per equatorial pixel in each angular direction.
constexpr double kSurveySamplesPerPixel = 2.0;
constexpr int kAttempts = 4;
constexpr double kRequestSeconds = 30.0;
constexpr int kDefaultThreads = 16;
constexpr int kMaxThreads = 64;  // bounds concurrent image memory and requests

struct Region { double south = -r1::kReliefLatitudeLimit, west = -180, north = r1::kReliefLatitudeLimit, east = 180; };

std::mutex gPrint;
void say(const std::string& line) {
    std::lock_guard<std::mutex> guard(gPrint);
    std::cout << line << std::endl;
}

struct Totals {
    std::atomic<int64_t> packs{0}, skipped{0}, landCells{0}, bytes{0}, images{0}, downloaded{0}, failures{0}, noData{0};
};

// One pack's images, fetched once each, kept in memory while the pack is built.
class Images {
public:
    Images(int zoom, Totals& totals) : zoom_(zoom), totals_(totals) {}
    const unsigned char* at(int x, int y) {
        const auto key = std::make_pair(x, y);
        if (auto it = images_.find(key); it != images_.end()) return it->second.data();
        std::string failures;
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            try {
                const auto response = r1::net::request("GET", r1::terrariumUrl(zoom_, x, y), {}, {}, kRequestSeconds);
                if (response.status != 200) throw std::runtime_error("HTTP " + std::to_string(response.status));
                ++totals_.images;
                totals_.downloaded += int64_t(response.body.size());
                return (images_[key] = r1::terrariumReliefPixels(response.body, zoom_)).data();
            } catch (const std::exception& e) {
                failures += std::string(failures.empty() ? "" : " | ") + e.what();
            }
        }
        throw r1::SourceUnavailable("Terrain Tiles z" + std::to_string(zoom_) + "/" + std::to_string(x) + "/" +
                                    std::to_string(y) + ": " + failures);
    }
    double height(double lon, double lat) {
        return r1::terrariumHeight(lon, lat, zoom_, [&](int x, int y) { return at(x, y); });
    }

private:
    int zoom_;
    Totals& totals_;
    std::map<std::pair<int, int>, std::vector<unsigned char>> images_;
};

// Whether the square degree holds any land, read from the survey's images.
bool holdsLand(Images& survey, int south, int west) {
    const double step = 360.0 / ((1 << kSurveyZoom) * r1::kTerrariumSize) / kSurveySamplesPerPixel;
    for (double lat = south + step / 2; lat < south + 1; lat += step)
        for (double lon = west + step / 2; lon < west + 1; lon += step)
            if (survey.height(lon, lat) > 0.0) return true;
    return false;
}

r1::ReliefCell readCell(Images& relief, int south, int west, Totals& totals) {
    r1::ReliefCell cell{south, west, {}};
    const int columns = r1::columnsFor(south);
    const int width = columns + 1;
    cell.heights.reserve(size_t(cell.rows()) * size_t(width));
    for (int r = 0; r < cell.rows(); ++r)
        for (int c = 0; c < width; ++c) {
            double h = relief.height(west + double(c) / columns, south + double(r) / r1::kRowsPerDegree);
            // No data in the source is no land.
            if (std::isnan(h)) { ++totals.noData; h = 0.0; }
            cell.heights.push_back(int16_t(std::lround(std::clamp(h, double(r1::kLowestHeight), double(r1::kHighestHeight)))));
        }
    return cell;
}

void buildPack(int south, int west, const std::string& out, bool measure, Totals& totals) {
    const std::string path = out + "/" + r1::packFileName(south, west);
    if (!measure && r1::validReliefPackFile(path)) { ++totals.skipped; return; }
    Images survey(kSurveyZoom, totals), relief(r1::kReliefZoom, totals);
    std::vector<std::optional<std::string>> cells(size_t(r1::kPackDegrees * r1::kPackDegrees));
    int land = 0;
    int64_t bytes = 0;
    for (int dy = 0; dy < r1::kPackDegrees; ++dy)
        for (int dx = 0; dx < r1::kPackDegrees; ++dx) {
            const int cs = south + dy, cw = west + dx;
            // A region selects whole packs. A partial pack would mistake its
            // unbuilt cells for sea and be skipped by the next global build.
            if (cs < -r1::kReliefLatitudeLimit || cs + 1 > r1::kReliefLatitudeLimit) continue;
            if (!holdsLand(survey, cs, cw)) continue;
            const r1::ReliefCell cell = readCell(relief, cs, cw, totals);
            if (!cell.hasLand()) continue;
            auto& code = cells[size_t(dy * r1::kPackDegrees + dx)];
            code = r1::encodeReliefCell(cell);
            ++land;
            bytes += int64_t(code->size());
        }
    if (!measure) {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            const auto bytes = r1::encodeReliefPack(cells);
            file.write(bytes.data(), std::streamsize(bytes.size()));
            file.close();
            if (!file) throw std::runtime_error("cannot write relief pack " + tmp);
        }
        if (!r1::validReliefPackFile(tmp)) throw std::runtime_error("invalid relief pack " + tmp);
        // Only an invalid existing file is replaced; complete packs returned above.
        fs::remove(path);
        fs::rename(tmp, path);
    }
    totals.landCells += land;
    totals.bytes += bytes;
    ++totals.packs;
    say("PACK " + r1::packFileName(south, west) + " land cells " + std::to_string(land) + ", " +
        std::to_string(bytes) + " bytes" + (land ? ", " + std::to_string(bytes / land) + " a cell" : ""));
}
}  // namespace

int main(int argc, char** argv) try {
    std::string out = r1::kReliefDirectory;
    Region region;
    int threads = kDefaultThreads;
    bool measure = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--threads" && i + 1 < argc) {
            const std::string value = argv[++i];
            size_t consumed = 0;
            threads = std::stoi(value, &consumed);
            if (consumed != value.size() || threads < 1 || threads > kMaxThreads)
                throw std::invalid_argument("threads must be an integer from 1 to " + std::to_string(kMaxThreads));
        }
        else if (a == "--measure") measure = true;
        else if (a == "--region" && i + 4 < argc) {
            region = {std::stod(argv[i + 1]), std::stod(argv[i + 2]), std::stod(argv[i + 3]), std::stod(argv[i + 4])};
            i += 4;
        } else {
            std::cerr << "usage: r1relief [--out <dir>] [--region <south> <west> <north> <east>] [--threads <n>] [--measure]\n";
            return 2;
        }
    }
    if (!std::isfinite(region.south) || !std::isfinite(region.west) ||
        !std::isfinite(region.north) || !std::isfinite(region.east) ||
        region.south < -90 || region.north > 90 || region.west < -180 || region.east > 180 ||
        region.south >= region.north || region.west >= region.east)
        throw std::invalid_argument("region must be ordered south/west/north/east within Earth coordinates");
    if (!measure) fs::create_directories(out);

    std::vector<std::pair<int, int>> packs;
    for (int south = -90; south < 90; south += r1::kPackDegrees)
        for (int west = -180; west < 180; west += r1::kPackDegrees)
            if (south + r1::kPackDegrees > region.south && south < region.north &&
                west + r1::kPackDegrees > region.west && west < region.east &&
                south + r1::kPackDegrees > -r1::kReliefLatitudeLimit && south < r1::kReliefLatitudeLimit)
                packs.push_back({south, west});

    Totals totals;
    std::atomic<size_t> next{0};
    const auto started = std::chrono::steady_clock::now();
    // Futures join on destruction too, including if starting a worker fails.
    std::vector<std::future<void>> workers;
    for (int t = 0; t < threads; ++t)
        workers.emplace_back(std::async(std::launch::async, [&] {
            for (size_t i; (i = next++) < packs.size();) {
                try {
                    buildPack(packs[i].first, packs[i].second, out, measure, totals);
                } catch (const std::exception& e) {
                    ++totals.failures;
                    say("PACK-FAILED " + r1::packFileName(packs[i].first, packs[i].second) + " " + e.what());
                }
            }
        }));
    for (auto& w : workers) w.get();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    say("DONE packs " + std::to_string(totals.packs.load() + totals.skipped.load()) + "/" + std::to_string(packs.size()) +
        " (" + std::to_string(totals.skipped.load()) + " already installed), land cells " +
        std::to_string(totals.landCells.load()) + ", " + std::to_string(totals.bytes.load()) + " bytes coded, " +
        std::to_string(totals.images.load()) + " images (" + std::to_string(totals.downloaded.load() / 1000000) +
        " MB) read, " + std::to_string(totals.noData.load()) + " samples without data, " +
        std::to_string(totals.failures.load()) + " packs failed, " + std::to_string(int(seconds)) + " s");
    return totals.failures ? 1 : 0;
} catch (const std::exception& e) {
    std::cerr << "r1relief: " << e.what() << '\n';
    return 2;
}
