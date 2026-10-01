#pragma once
#include "gen/interiors.hpp"

// Generation assertion only: furniture must leave the intended circulation
// space free. Runtime collisions are exercised through Jolt by the E2E driver.
inline bool furnitureOccupies(const r1::InteriorLayout& layout,r1::P2 local,double radius=.32) {
    for(const auto& fixture:layout.fixtures)if(fixture.height>.05)
        if(std::abs(local.x-fixture.at.x)<fixture.size.x*.5+radius&&
           std::abs(local.y-fixture.at.y)<fixture.size.y*.5+radius)return true;
    return false;
}
