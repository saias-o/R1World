// Inland water outlines: surveyed areas and a conservative width for mapped
// river/canal centre lines whose banks were not included in the OSM answer.
#pragma once

#include "clip.hpp"
#include "osm.hpp"

namespace r1 {

// Engine x/z metres. The union is shared by the visible surface and the
// swimming/terrain water classification.
clip::Paths64 inlandWaterRegion(const std::vector<OsmWay>& waterways, const Anchor& anchor, bool dryIntermittent = false,
                               const std::vector<OsmWaterArea>& areas = {});
// The arid-season hypothesis applies only to explicitly intermittent centre
// lines; mapped permanent water areas retain their observed extent.
clip::Paths64 intermittentChannelRegion(const std::vector<OsmWay>& waterways, const Anchor& anchor);

}  // namespace r1
