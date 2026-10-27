#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <optional>

namespace r1 {
// Only an actual upward-facing bark triangle can support a bird. Leaf cards
// are filtered by the caller's material; no inferred canopy centre is a perch.
inline std::optional<glm::vec3> birdBranchPoint(glm::vec3 a,glm::vec3 b,glm::vec3 c,glm::vec3 trunk,
                                               float minHeight=.8f,float minRadius=.5f) {
    const glm::vec3 cross=glm::cross(b-a,c-a);
    const float area=glm::length(cross);
    if(area<.00002f||cross.y/area<.3f)return std::nullopt;
    const auto p=(a+b+c)/3.f;
    if(p.y-trunk.y<minHeight||glm::length(glm::vec2(p.x-trunk.x,p.z-trunk.z))<minRadius)return std::nullopt;
    if(std::max({glm::length(a-b),glm::length(b-c),glm::length(c-a)})>2.f)return std::nullopt;
    return p+glm::vec3(0,.008f,0);
}
} // namespace r1
