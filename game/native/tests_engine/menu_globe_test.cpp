#include "menu_globe.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    size_t checks=0;
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
    const r1::GlobeView view;
    assert(!view.pick(10,10,1440,900));
    assert(!view.project(view.lon+180,-view.lat,1440,900));
    assert(!view.pick(0,0,0,900));
    std::cout<<"[menu-globe] PASS "<<checks<<" checks: spherical picking/projection, polar/dateline orbits, street zoom, visible tile coverage\n";
}
