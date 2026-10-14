#pragma once
#include "harbours.hpp"
#include "mesh.hpp"
#include "../weather.hpp"

namespace r1 {
// Geography is deterministic; attendance belongs to the live weather, never the cache.
struct BeachSample {
    double inland = 1e9;
    P2 shore{}, normal{};
    bool mapped = false;
};
class BeachPlan {
public:
    BeachPlan(const OsmData&, const Anchor&, const Bounds&, const Sea*);
    BeachSample sample(P2 p) const;
    bool sand(P2 p) const;
    bool protectedHarbour(P2 p) const { return clip::contains(ports_,p); }
    bool behindCoastalRoad(P2 p) const { return clip::contains(roadBack_,p); }
    ElevationGrid sculpt(const ElevationGrid&) const;
    std::vector<MeshPart> surfaces(const ElevationGrid&, const clip::Paths64& streets) const;
    nlohmann::json water(const ElevationGrid&, const clip::Paths64& streets) const;
    nlohmann::json population(const ElevationGrid&, const clip::Paths64& streets) const;
    nlohmann::json report() const;
    bool empty() const { return shores_.empty(); }
private:
    Anchor anchor_; Bounds bounds_;
    struct Shore { P2 a,b,inland; };
    std::vector<Shore> shores_;
    clip::Paths64 mapped_, sand_, wet_, obstacles_, paths_, sea_, tile_, ports_, roadBack_;
    std::vector<Ring> roads_;
};

double beachAttendance(double solarHour, double sunElevation, const WeatherState&);
struct BeachPose {
    P3 position;
    double heading = 0, recline = 0;
    bool walking = false, swimming = false;
};
// A complete staggered rest -> stand -> sea -> bathe -> return -> lie cycle.
BeachPose beachPose(P3 towel, P3 water, double seconds, unsigned slot);
std::vector<MeshPart> beachFurniture(bool parasol, unsigned palette);
} // namespace r1
