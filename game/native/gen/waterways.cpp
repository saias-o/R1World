#include "waterways.hpp"

#include "palette.hpp"

namespace r1 {

namespace {
double waterwayWidth(const Tags& tags) {
    const std::string kind = tagOr(tags, "waterway");
    double fallback = 0;
    if (kind == "river") fallback = 32.0;
    else if (kind == "canal") fallback = 10.0;
    else if (kind == "stream") fallback = 3.0;
    else if (kind == "ditch" || kind == "drain") fallback = 1.5;
    if (fallback == 0) return 0;
    const std::string* tagWidth = tag(tags, "width");
    if (!tagWidth) return fallback;
    char* end = nullptr;
    const double measured = std::strtod(tagWidth->c_str(), &end);
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (end && *end == 'm') ++end;
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return end != tagWidth->c_str() && end && !*end && std::isfinite(measured) && measured >= 0.5 && measured <= 500.0
               ? measured : fallback;
}
}  // namespace

clip::Paths64 inlandWaterRegion(const std::vector<OsmWay>& waterways, const Anchor& anchor, bool dryIntermittent,
                               const std::vector<OsmWaterArea>& areas) {
    clip::Paths64 shapes;
    for (const auto& area : areas) {
        if (hiddenWater(area.tags)) continue;
        auto project=[&](const std::vector<std::vector<P2>>& rings) {
            clip::Paths64 paths;
            for (const auto& ring : rings) {
                std::vector<P2> line;
                for (P2 p : ring) {const P3 q=anchor.toEngine(p.x,p.y,0.);line.push_back({q.x,q.z});}
                auto part=clip::bufferRing(line,0.);
                paths.insert(paths.end(),part.begin(),part.end());
            }
            return clip::unite(paths);
        };
        const auto part=clip::subtract(project(area.outers),project(area.inners));
        shapes.insert(shapes.end(),part.begin(),part.end());
    }
    for (const OsmWay& way : waterways) {
        if (hiddenWater(way.tags)) continue;
        if(dryIntermittent && !way.closed() && tagOr(way.tags,"intermittent")=="yes")continue;
        std::vector<P2> line;
        line.reserve(way.points.size());
        for (const P2& p : way.points) {
            const P3 q = anchor.toEngine(p.x, p.y, 0.0);
            line.push_back({q.x, q.z});
        }
        clip::Paths64 part;
        if (way.closed() && classifyWay(way.tags) == "water") {
            part = clip::bufferRing(line, 0.0);
        } else if (!way.closed()) {
            const double width = waterwayWidth(way.tags);
            if (width > 0) part = clip::bufferLineRoundJoins(line, width * 0.5);
        }
        shapes.insert(shapes.end(), part.begin(), part.end());
    }
    return clip::unite(shapes);
}

clip::Paths64 intermittentChannelRegion(const std::vector<OsmWay>& waterways,const Anchor& anchor) {
    std::vector<OsmWay> channels;
    for(const auto& way:waterways)if(!way.closed() && tagOr(way.tags,"intermittent")=="yes")channels.push_back(way);
    return inlandWaterRegion(channels,anchor);
}

}  // namespace r1
