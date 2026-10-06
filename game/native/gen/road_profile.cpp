#include "streets.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <cstdlib>

namespace r1 {
namespace {
int laneCount(const Tags& tags, const char* key) {
    const auto* value = tag(tags, key);
    if (!value) return 0;
    char* end = nullptr;
    const long n = std::strtol(value->c_str(), &end, 10);
    return end == value->c_str() + value->size() && n >= 1 && n <= 12 ? int(n) : 0;
}
}

int roadDirection(const Tags& tags) {
    const std::string one = tagOr(tags, "oneway"), h = tagOr(tags, "highway");
    if (one == "-1" || one == "reverse") return -1;
    if (one == "yes" || one == "true" || one == "1") return 1;
    if (one == "no" || one == "false" || one == "0") return 0;
    if (h == "motorway" || h == "motorway_link" || tagOr(tags, "junction") == "roundabout" ||
        tagOr(tags, "junction") == "circular") return 1;
    return 0;
}

RoadProfile roadProfile(const Tags& tags) {
    RoadProfile p;
    const std::string h = tagOr(tags, "highway");
    p.link = h.size() > 5 && h.compare(h.size() - 5, 5, "_link") == 0;
    const std::string base = p.link ? h.substr(0, h.size() - 5) : h;
    p.express = base == "motorway" || base == "trunk";
    p.direction = roadDirection(tags);
    p.lanes = laneCount(tags, "lanes");
    if (!p.lanes) p.lanes = laneCount(tags, "lanes:forward") + laneCount(tags, "lanes:backward") + laneCount(tags, "lanes:both_ways");
    if(p.lanes>12)p.lanes=0;
    p.lanesTagged = p.lanes > 0;
    if (!p.lanes) p.lanes = p.express && !p.link ? 2 : p.direction || h == "service" || h == "living_street" ? 1 : 2;
    p.widthTagged=lengthTag(tag(tags,"width"),0)>0;
    if (!isMotorway(h)) { p.lanes = 1; p.width = lengthTag(tag(tags, "width"), roadWidthOf(h)); return p; }

    // ICTAAL: 3.5 m lanes, a 0.5 m left + 1 m right hard strip on ramps.
    // Tagged width wins even where the observed road is genuinely narrow.
    const double lane = p.express ? 3.5 : base == "service" ? 3.0 : 3.25;
    if (p.express && p.direction) {
        p.leftShoulder = p.link ? .5 : 1.0;
        p.rightShoulder = p.link ? 1.0 : base == "motorway" ? 2.5 : 2.0;
        if (p.direction < 0) std::swap(p.leftShoulder, p.rightShoulder);
    } else if (p.express) p.leftShoulder = p.rightShoulder = .5;
    for (int side = 0; side < 2; ++side) {
        double& width = side ? p.rightShoulder : p.leftShoulder;
        const std::string name = side ? "right" : "left";
        const std::string shoulder = tagOr(tags, ("shoulder:" + name).c_str(), tagOr(tags, "shoulder"));
        if(width==0 && (shoulder=="yes" || shoulder=="both" || shoulder==name))width=1.0;
        if (shoulder == "no" || shoulder == "none" || (shoulder == "left" && side) || (shoulder == "right" && !side)) width = 0;
        width = lengthTag(tag(tags, ("shoulder:" + name + ":width").c_str()), width);
        if(tagOr(tags,("shoulder:" + name + ":width").c_str())=="0")width=0;
    }
    const double inferred = p.lanesTagged || p.express || p.link ? p.lanes * lane + p.leftShoulder + p.rightShoulder
        : std::max(roadWidthOf(h), p.lanes * lane+p.leftShoulder+p.rightShoulder);
    p.width = lengthTag(tag(tags, "width"), inferred);
    // A surveyed width may include fewer hard strips than the class default.
    const double available = std::max(0.0, p.width - std::min(p.width, p.lanes * lane));
    const double strips = p.leftShoulder + p.rightShoulder;
    if (strips > available && strips > 0) { p.leftShoulder *= available / strips; p.rightShoulder *= available / strips; }
    return p;
}

double roadWidth(const Tags& tags) { return roadProfile(tags).width; }
} // namespace r1
