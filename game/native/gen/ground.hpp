// Missing land cover: local footprint density, then country/regional priors.
// The raster is aligned to the globe, not a tile, so neighbours agree.
#pragma once

#include "palette.hpp"

namespace r1 {

class GroundInference {
public:
    struct Density { double coverage = 0, heightWeightedCoverage = 0; uint32_t buildings = 0; };
    GroundInference(const OsmData& osm, const Bounds& bounds,
                    const RegionProfile& profile, std::string country,
                    const std::map<int64_t,double>& predictedHeights);
    Density density(double lon, double lat) const;
    // A mapped class always wins; inferred classes keep their provenance.
    std::string at(double lon, double lat, const std::string* mapped = nullptr) const;
    nlohmann::json report() const;

private:
    static constexpr double step = .00005, metresPerDegree = 111320.;
    double halfWindow = 100, settledCoverage = .14, denseCoverage = .32;
    uint32_t minimumBuildings = 3;
    std::string country_, profile_, rural_ = "profile", settled_ = "urban", dense_ = "urban";
    int x0 = 0, y0 = 0, width = 0, height = 0;
    std::vector<uint32_t> occupancy_, buildings_;
    std::vector<double> mass_;
    uint32_t sum(const std::vector<uint32_t>& table, int left, int bottom, int right, int top) const;
};

}  // namespace r1
