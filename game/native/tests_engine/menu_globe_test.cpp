#include "menu_globe.hpp"
#include "gen/common.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    size_t checks=0;
    {
        // One delayed neighbour must not hold back the sharp tile, while the
        // nearest cached parent covers the hole (deduplicated, coarse first).
        std::vector<r1::MapTile> wanted={{9,256,170},{9,257,170},{9,257,171}};
        std::vector<r1::MapTile> ready={{7,64,42},{9,256,170},{8,128,85}};
        auto coverage=r1::globeTileCoverage(wanted,ready);
        assert((coverage==std::vector<size_t>{2,1}));
        // Adding the missing images makes the coarse fallback unnecessary.
        ready.push_back(wanted[1]);ready.push_back(wanted[2]);
        coverage=r1::globeTileCoverage(wanted,ready);
        assert(coverage.size()==3);
        for(auto i:coverage)assert(ready[i].z==9);
        // Reopening/zooming out reuses children even if the parent was never
        // downloaded. Distant tiles must never leak into the returned view.
        ready={{9,256,170},{9,257,170},{9,256,171},{9,257,171},{9,20,20}};
        coverage=r1::globeTileCoverage({{8,128,85}},ready);
        assert(coverage.size()==4);
        for(auto i:coverage)assert(i<4);
        assert(r1::globeTileCoverage(wanted,{}).empty());
        assert(r1::globeTileCoverage({},ready).empty());
        checks+=10;
    }
    for(double lon:{-179.99,-95.,2.3522,139.7671,179.99})
        for(double lat:{-89.8,-48.,0.,48.8566,89.8})
            for(double zoom:{1.,2.,16.,2048.,131072.}) {
                r1::GlobeView view{lon,lat,zoom};
                auto center=view.pick(720,522,1440,900);
                assert(center);
                assert(std::abs(std::remainder(center->lon-lon,360.))<1e-6);
                assert(std::abs(center->lat-lat)<1e-6);
                for(double x:{600.,720.,840.})for(double y:{400.,522.,600.}) {
                    const auto hit=view.pick(x,y,1440,900);
                    if(!hit)continue;
                    const auto back=view.project(hit->lon,hit->lat,1440,900);
                    assert(back&&std::abs(back->x-x)<.005&&std::abs(back->y-y)<.005);
                    ++checks;
                }
                const auto tiles=view.tiles(1440,900,view.tileLevel(900));
                assert(tiles.size()<=144); // the view, plus a fifth beyond each side
                for(const auto& tile:tiles)assert(tile.x>=0&&tile.y>=0&&tile.x<(1<<tile.z)&&tile.y<(1<<tile.z));
                if(zoom>=16&&std::abs(lat)<80) {
                    assert(!tiles.empty());
                    for(int y=0;y<=6;++y)for(int x=0;x<=8;++x) {
                        auto hit=view.pick(x*180.,y*150.,1440,900);
                        if(!hit||std::abs(hit->lat)>r1::MapView::mercatorLimit)continue;
                        const int z=view.tileLevel(900),n=1<<z;
                        const int tx=int((hit->lon+180.)/360.*n);
                        const int ty=int((1.-std::asinh(std::tan(hit->lat*3.141592653589793/180.))/3.141592653589793)*.5*n);
                        bool found=false;for(const auto& t:tiles)found=found||(t.x==tx&&t.y==ty);
                        assert(found);++checks;
                    }
                }
            }
    // Arrival readiness must cover every pixel, without pinning the unseen
    // northern row that formerly filled memory before the western ground.
    const r1::GlobeView arrival{2.3522,48.85435,48000.};
    size_t shown=0;
    for(int r=27770;r<=27772;++r) {
        const int n=r1::columns(r),c=int((arrival.lon+180.)/360.*n);
        for(int dc=-1;dc<=1;++dc) {
            const auto b=r1::Tile{r,c+dc}.bounds();
            shown+=arrival.shows(b.west,b.south,b.east,b.north,1600,900);
        }
    }
    assert(shown==5);
    for(double lon:{-179.999,2.3522,179.999})for(double lat:{-89.8,0.,48.85435,89.8})
        for(double width:{640.,1600.,2560.})for(const glm::vec4 vp:{glm::vec4(0,0,1,1),glm::vec4(.05f,.1f,.9f,.8f)}) {
            const r1::GlobeView close{lon,lat,48000.};
            for(int y=0;y<=12;++y)for(int x=0;x<=16;++x) {
                const auto hit=close.pick(width*(x/16.-vp.x),900*(y/12.-vp.y),width*vp.z,900*vp.w);
                assert(hit);
                const auto b=r1::tileAt(hit->lon,hit->lat).bounds();
                assert(close.shows(b.west,b.south,b.east,b.north,width,900,vp));
                ++checks;
            }
        }
    const r1::GlobeView view;
    assert(!view.pick(10,10,1440,900));
    assert(!view.project(view.lon+180,-view.lat,1440,900));
    assert(!view.pick(0,0,0,900));
    std::cout<<"[menu-globe] PASS "<<checks<<" checks: spherical picking/projection, polar/dateline orbits, street zoom, visible tile coverage\n";
}
