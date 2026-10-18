#pragma once
#include <cstdint>

namespace r1 {
inline uint32_t motorcycleAppearance(uint32_t seed) {
    seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;seed*=0x846ca68bu;seed^=seed>>16;
    return seed;
}
inline bool femaleMotorcycleRider(uint32_t seed) {
    return motorcycleAppearance(seed^0x85ebca6bu)%5u==0u;
}
} // namespace r1
