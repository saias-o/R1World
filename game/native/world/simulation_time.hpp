#pragma once
#include <algorithm>
#include <cmath>

namespace r1::world {
// One accepted clock for every gameplay actor. Up to 250 ms is consumed in
// <=50 ms steps; a longer stall is deliberately dropped for all actors alike.
struct SimulationTime {
    static constexpr double kMaxStep=.05, kCatchUp=.25;
    double seconds=0, step=0, dropped=0;
    int steps=0;
    explicit SimulationTime(double elapsed) {
        if(!std::isfinite(elapsed)||elapsed<=0)return;
        seconds=std::min(elapsed,kCatchUp);dropped=elapsed-seconds;
        steps=std::max(1,int(std::ceil(seconds/kMaxStep)));
        step=seconds/steps;
    }
};
}
