#include "far_relief.hpp"
#include "relief.hpp"
#include "sources.hpp"

#include <algorithm>
#include <cmath>

namespace r1 {

FarLayers::FarLayers(std::string climate) : climate_(std::move(climate)) {
    const Palette& p = palette();
    auto named = [&](const char* name) {
        for (const auto& c : p.groundClasses)
            if (c.name == name) return c.swatch;
        throw std::runtime_error(std::string("the Atlas has no ground class ") + name);
    };
    swatches_ = {named("water"), p.snow, named("rock"), named("forest"), named("scrub")};
    for (int i = 0; i < int(swatches_.size()); ++i) byName_[swatches_[size_t(i)].name] = i;
}

int FarLayers::ground(const Swatch& swatch) {
    std::lock_guard<std::mutex> guard(lock_);
    if (auto it = byName_.find(swatch.name); it != byName_.end()) return it->second;
    if (int(swatches_.size()) >= kMax) return kMax - 1;
    swatches_.push_back(swatch);
    ++revision_;
    return byName_[swatch.name] = int(swatches_.size()) - 1;
}

std::vector<Swatch> FarLayers::swatches() const {
    std::lock_guard<std::mutex> guard(lock_);
    return swatches_;
}

std::vector<Material> FarLayers::materials() const {
    std::lock_guard<std::mutex> guard(lock_);
    std::vector<Material> result;
    result.reserve(swatches_.size());
    for (size_t i = 0; i < swatches_.size(); ++i) {
        const auto& swatch = swatches_[i];
        std::optional<std::string> family;
        if (i == kSnow) family = "snow_clean";
        else if (i == kRock) family = "cliff";
        else if (i == kForest) family = groundFamily("forest", "", climate_);
        else if (i == kScrub) family = groundFamily("scrub", "", climate_);
        else if (i != kWater) family = groundFamily(kInferred, swatch.name, climate_);
        result.push_back(surfaceMaterial(swatch.name, swatch.color, swatch.roughness, family));
    }
    return result;
}

uint64_t FarLayers::revision() const {
    std::lock_guard<std::mutex> guard(lock_);
    return revision_;
}

namespace {
constexpr double kTreelineOfSnowline = 0.75;
}  // namespace

double treeline(double lat) { return kTreelineOfSnowline * snowline(lat); }

double baselineSlope(double slope, double spacing) {
    return slope * std::pow(std::max(spacing, kSlopeBaseline) / kSlopeBaseline, 1.0 - kHurst);
}

int farZoom(double spacing, double lat) {
    const double equatorPixel = 2.0 * M_PI * kA / 256.0;  // metres a pixel at zoom 0
    const double pixel = equatorPixel * std::max(0.05, std::cos(radians(lat)));
    const int zoom = int(std::lround(std::log2(pixel / std::max(spacing, 1.0))));
    return std::clamp(zoom, kFarMinimumZoom, 13);
}

FarLevel sampleWorldFarLevel(const ObservationStore& store, const Anchor& anchor,
                             double originX, double originZ, double spacing, int resolution,
                             FarLayers& layers, bool network) {
    bool usedFineFallback = false;
    const ReliefLayer& installed = installedRelief(store.root());
    auto sampler = [&](double sampleSpacing) -> std::function<double(double, double)> {
        const bool coarse = sampleSpacing >= kInstalledReliefSpacing;
        const int zoom = farZoom(sampleSpacing, anchor.lat);
        std::vector<std::function<double(double, double)>> images;
        for (int z = zoom; z >= std::max(kFarMinimumZoom, zoom - kFarCachedZoomSteps); --z)
            images.push_back(store.terrainSampler(z, network && !coarse && z == zoom));
        return [&, coarse, images = std::move(images)](double lon, double lat) {
            auto height = [&]() -> std::optional<double> {
                auto h = installed.height(lon, lat);
                if (h && !coarse) usedFineFallback = true;
                return h;
            };
            if (coarse) if (auto h = height()) return *h;
            std::string why;
            for (const auto& image : images) {
                try { return image(lon, lat); }
                catch (const std::exception& e) { why = e.what(); }
            }
            if (!coarse) if (auto h = height()) return *h;
            throw SourceUnavailable("no relief at " + std::to_string(lon) + "," + std::to_string(lat) + ": " + why);
        };
    };
    auto level = sampleFarLevel(anchor, originX, originZ, spacing, resolution,
                               sampler(spacing), layers, sampler(2.0 * spacing));
    level.zoom = farZoom(spacing, anchor.lat);
    level.installedFallback = usedFineFallback;
    return level;
}

FarLevel sampleFarLevel(const Anchor& anchor, double originX, double originZ, double spacing, int resolution,
                        const std::function<double(double, double)>& heightAt, FarLayers& layers,
                        const std::function<double(double, double)>& borderAt) {
    const int n = resolution + 1;
    FarLevel out;
    out.originX = originX;
    out.originZ = originZ;
    out.spacing = spacing;
    out.heights.resize(size_t(n) * size_t(n));
    out.layers.resize(size_t(resolution) * size_t(resolution));
    std::vector<double> measured(size_t(n) * size_t(n)), lons(size_t(n) * size_t(n)), lats(size_t(n) * size_t(n));

    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const double x = originX + i * spacing, z = originZ + j * spacing;
            const bool border = borderAt && (i == 0 || j == 0 || i == n - 1 || j == n - 1);
            const auto& relief = border ? borderAt : heightAt;
            // The surface point under (x, z): start under the tangent plane,
            // then move by what the first guess missed. One step leaves an
            // error of the order of the drop times the angle, millimetres.
            P3 g = anchor.toGeodetic(x, 0.0, z);
            double h = relief(g.x, g.y);
            P3 at = anchor.toEngine(g.x, g.y, std::max(h, 0.0));
            g.y -= (z - at.z) / kMetresPerDegree;  // engine z is south
            g.x += (x - at.x) / (kMetresPerDegree * std::max(1e-6, std::cos(radians(g.y))));
            h = relief(g.x, g.y);
            const size_t k = size_t(j) * size_t(n) + size_t(i);
            measured[k] = h;
            lons[k] = g.x;
            lats[k] = g.y;
            // The model's sea floor is not what is seen: the sea is at 0.
            out.heights[k] = float(anchor.toEngine(g.x, g.y, std::max(h, 0.0)).y);
        }

    for (int j = 0; j < resolution; ++j)
        for (int i = 0; i < resolution; ++i) {
            const size_t a = size_t(j) * size_t(n) + size_t(i), b = a + 1, c = a + size_t(n), d = c + 1;
            const double lon = 0.25 * (lons[a] + lons[b] + lons[c] + lons[d]);
            const double lat = 0.25 * (lats[a] + lats[b] + lats[c] + lats[d]);
            const double mean = 0.25 * (measured[a] + measured[b] + measured[c] + measured[d]);
            const double top = std::max({measured[a], measured[b], measured[c], measured[d]});
            const double slope = baselineSlope(
                std::max(std::abs(measured[b] - measured[a]) + std::abs(measured[d] - measured[c]),
                         std::abs(measured[c] - measured[a]) + std::abs(measured[d] - measured[b])) / (2.0 * spacing),
                spacing);
            int layer;
            if (top <= 0.0) { layer = FarLayers::kWater; ++out.seaCells; }
            else if (mean >= snowline(lat)) { layer = FarLayers::kSnow; ++out.snowCells; }
            else if (slope > kRockSlope) { layer = FarLayers::kRock; ++out.rockCells; }
            else {
                const RegionProfile& profile = profileFor(lon, lat);
                const std::string climate = climateAt(profile.climate, lat);
                const bool wooded = slope > kWoodedSlope && mean < treeline(lat);
                if (wooded && (climate == "temperate" || climate == "boreal" || climate == "tropical")) {
                    layer = FarLayers::kForest;
                    ++out.forestCells;
                } else if (wooded && climate == "mediterranean") {
                    layer = FarLayers::kScrub;
                    ++out.scrubCells;
                } else {
                    layer = layers.ground(profile.ground);
                }
            }
            out.layers[size_t(j) * size_t(resolution) + size_t(i)] = uint8_t(layer);
        }
    return out;
}

}  // namespace r1
