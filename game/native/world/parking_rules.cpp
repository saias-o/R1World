#include "parking_rules.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace r1::parking {
namespace {
uint64_t mix(uint64_t x) {
    x ^= x >> 33, x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33, x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}
uint64_t nameHash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a: the same on every machine, unlike std::hash
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
} // namespace

std::vector<Spot> kerbSpots(const saida::traffic::Graph& graph, bool leftHand, float lateralAxisSign,
                            const std::string& tile) {
    using saida::traffic::Vec2;
    std::vector<Spot> out;
    // The lanes between two nodes in one direction: the kerb is beside the
    // outermost one on the side this country drives on.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> kerbLane;
    auto lateral = [&](const saida::traffic::Lane& l) {
        const float offset = std::isfinite(l.offset) ? l.offset : 0.f;
        return leftHand ? -offset : offset;  // to the right of travel, as Flow::positionOf places it
    };
    for (uint32_t i = 0; i < graph.lanes.size(); ++i) {
        const auto& l = graph.lanes[i];
        const auto key = std::make_pair(l.from, l.to);
        const auto found = kerbLane.find(key);
        if (found == kerbLane.end()) kerbLane.emplace(key, i);
        else if (leftHand ? lateral(l) < lateral(graph.lanes[found->second]) : lateral(l) > lateral(graph.lanes[found->second]))
            found->second = i;
    }
    // A node joined to exactly two others is a shape point: the street
    // bends there and goes on. Any other is a junction or a dead end.
    std::vector<std::vector<uint32_t>> around(graph.nodes.size());
    auto link = [&](uint32_t a, uint32_t b) {
        if (a < around.size() && std::find(around[a].begin(), around[a].end(), b) == around[a].end()) around[a].push_back(b);
    };
    for (const auto& l : graph.lanes) link(l.from, l.to), link(l.to, l.from);
    auto bend = [&](uint32_t n) { return n < around.size() && around[n].size() == 2; };
    const uint64_t base = nameHash(tile);
    for (const auto& [ends, i] : kerbLane) {
        const auto& l = graph.lanes[i];
        const float length = graph.laneLength(i);
        const float start = bend(l.from) ? kSpacing * .5f : kEndClearance;
        const float stop = length - (bend(l.to) ? kSpacing * .5f : kEndClearance);
        if (l.layer != 0 || l.speed > kFastest || l.priority == 8 || stop < start) continue;
        if (!bend(l.from) && !bend(l.to) && length < kShortest) continue;
        const float side = leftHand ? -1.f : 1.f;
        const float across = lateral(l) + side * (l.width * .5f - kKerbGap - kHalfWidth);
        const Vec2 dir = graph.direction(i);
        const Vec2 right{dir.y, -dir.x};
        for (float s = start; s <= stop; s += kSpacing) {
            const Vec2 p = graph.pointOn(i, s) + right * (across * lateralAxisSign);
            const uint64_t id = mix(base ^ mix(l.road) ^ mix(uint64_t(int64_t(std::lround(p.x * 2))) << 32 ^
                                                            uint64_t(uint32_t(int32_t(std::lround(p.y * 2))))));
            if (id % 100 >= uint64_t(kTaken)) continue;
            out.push_back({id, uint32_t(mix(id) >> 32), p.x, p.y, std::atan2(dir.x, dir.y), i, s});
        }
    }
    return out;
}

const char* modelFor(uint32_t seed) {
    static const char* cars[] = {"city", "city", "city", "sedan", "sedan", "suv", "suv", "offroad", "sport"};
    return cars[seed % (sizeof cars / sizeof *cars)];
}

} // namespace r1::parking
