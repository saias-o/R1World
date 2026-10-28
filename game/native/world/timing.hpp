#pragma once
#include <chrono>

namespace r1::world {
inline double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}

} // namespace r1::world
