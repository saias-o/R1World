#pragma once

#include "gen/map_tiles.hpp"
#include <glm/glm.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace r1 {

// The menu camera uses Earth radii, longitude east and latitude north.
// Picking and drawing share this projection, including the off-centre framing.
struct GlobeView {
    double lon=2.3522,lat=32.,zoom=1.;
    static constexpr double tangent=.414213562373095;
    static constexpr double centreY=.58;
    static glm::dvec3 direction(double lon,double lat);
    double altitude() const;
    glm::dmat3 basis() const;
    std::optional<MapPoint> pick(double x,double y,double width,double height) const;
    std::optional<MapPixel> project(double lon,double lat,double width,double height) const;
    int tileLevel(double height) const;
    // The level-z tiles covering what the viewport shows of the globe, or with
    // span<1 only its central part (prefetching the next level).
    std::vector<MapTile> tiles(double width,double height,int z,double span=1.) const;
};

// Detail layers over the 8K day map, which holds level 5 (8192 px around).
// Sentinel-2 imagery from level 7 (at 6 the day map still holds) to its native
// 10 m at 14 (upsampled beyond);
// the OpenStreetMap map fades in over one doubling from kOsmZoom (level ~13).
constexpr int kImageryFirstLevel=7;
constexpr int kImageryLastLevel=14;
constexpr double kOsmZoom=2048.;

struct GlobeTile { MapTile tile; std::filesystem::path path; };

// One tile layer as the renderer needs it. The game knows what is on disk; the
// renderer knows what is on the GPU, so it picks what to draw: each visible
// tile, or while it loads the best stand-in already resident -- its four
// children (zooming out) or its nearest ancestor (zooming in) -- and fades a
// tile in over its stand-in instead of letting the detail jump.
struct GlobeLayer {
    int z=-1;                    // level shown, -1 when the layer is off
    int coarsest=0;              // lowest level a stand-in may come from
    float opacity=1;             // whole-layer fade (the map over imagery)
    std::vector<MapTile> visible;   // level-z tiles covering the view
    std::vector<GlobeTile> cached;  // on disk: visible, stand-ins, prefetch
};

struct MenuGlobeState {
    bool visible=true;
    GlobeView view;
    MapPoint selected{2.3522,48.8566};
    // Where the Sun is at the zenith, from the world's clock (sun_cycle.js).
    MapPoint subsolar{0,0};
    // Normalized viewport placement of the RmlUi document, including letterboxing.
    glm::vec4 viewport{0,0,1,1};
    // Imagery is lit like the globe; the map is drawn over it as a map.
    GlobeLayer imagery;
    GlobeLayer map;
};

// Register before constructing Engine. The renderer owns GPU resources; this
// shared model contains no device objects and can outlive the game controller.
std::shared_ptr<MenuGlobeState> registerMenuGlobe();

} // namespace r1
