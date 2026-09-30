#include "check.hpp"
#include "gen/map_tiles.hpp"

#include <cmath>

using namespace r1;

TEST(MapTiles, clicks_and_pins_agree_at_street_zoom) {
    const MapView view(2.3522,48.8566,19);
    const auto center=view.screen(2.3522,48.8566);
    NEAR(center.x,672.,1e-6);
    NEAR(center.y,280.,1e-6);
    const auto clicked=view.pointAt(820,390);
    const auto pin=view.screen(clicked.lon,clicked.lat);
    NEAR(pin.x,820.,1e-5);
    NEAR(pin.y,390.,1e-5);
    // At the closest zoom, a pixel is well under one metre in Paris.
    const auto next=view.pointAt(821,390);
    CHECK(std::abs(next.lon-clicked.lon)<0.00001);
}

TEST(MapTiles, only_the_visible_tiles_are_needed) {
    for(int level:{3,10,19}) {
        const MapView view(2.3522,48.8566,level);
        const auto tiles=view.visible();
        CHECK(!tiles.empty());
        CHECK(tiles.size()<=24);
        for(const auto& tile:tiles) {
            CHECK(tile.z==level);
            CHECK(tile.x>=0 && tile.x<(1<<level));
            CHECK(tile.y>=0 && tile.y<(1<<level));
        }
    }
    const MapView dateline(179.8,0,10);
    const auto p=dateline.screen(-179.8,0);
    CHECK(p.x>672 && p.x<1344);
}
