#include "map_tiles.hpp"

#include <algorithm>
#include <cmath>

namespace r1 {
namespace {
constexpr double pi=3.14159265358979323846;
}

MapPixel MapView::project(double lon,double lat,int level) {
    const double n=double(1<<level);
    lat=std::clamp(lat,-mercatorLimit,mercatorLimit);
    const double phi=lat*pi/180.;
    return {(lon+180.)/360.*n,(1.-std::asinh(std::tan(phi))/pi)*.5*n};
}

MapView::MapView(double centerLon,double centerLat,int level)
    :level_(std::clamp(level,0,19)),center_(project(centerLon,centerLat,level_)){}

MapPixel MapView::screen(double lon,double lat) const {
    const double n=double(1<<level_);
    const MapPixel p=project(lon,lat,level_);
    double dx=std::remainder(p.x-center_.x,n);
    return {width*.5+dx*tilePixels,height*.5+(p.y-center_.y)*tilePixels};
}

MapPoint MapView::pointAt(double x,double y) const {
    const double n=double(1<<level_);
    const double tx=center_.x+(x-width*.5)/tilePixels;
    const double ty=std::clamp(center_.y+(y-height*.5)/tilePixels,0.,n);
    const double wrapped=tx-n*std::floor(tx/n);
    return {wrapped/n*360.-180.,std::atan(std::sinh(pi*(1.-2.*ty/n)))*180./pi};
}

std::vector<MapTile> MapView::visible() const {
    const int n=1<<level_;
    const int x0=int(std::floor(center_.x-width*.5/tilePixels));
    const int x1=int(std::floor(center_.x+width*.5/tilePixels));
    const int y0=std::max(0,int(std::floor(center_.y-height*.5/tilePixels)));
    const int y1=std::min(n-1,int(std::floor(center_.y+height*.5/tilePixels)));
    std::vector<MapTile> tiles;
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
        const int wrapped=(x%n+n)%n;
        tiles.push_back({level_,wrapped,y,
            width*.5+(x-center_.x)*tilePixels,
            height*.5+(y-center_.y)*tilePixels});
    }
    return tiles;
}

}  // namespace r1
