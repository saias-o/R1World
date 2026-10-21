#include "menu_globe.hpp"
#include <algorithm>
#include <cmath>

namespace r1 {
namespace {
constexpr double pi=3.14159265358979323846;
constexpr double radians=pi/180.;
double wrapLongitude(double x) { return x-360.*std::floor((x+180.)/360.); }
MapPoint geographic(glm::dvec3 p) {
    return {wrapLongitude(std::atan2(p.x,p.z)/radians),std::asin(std::clamp(p.y,-1.,1.))/radians};
}
}

glm::dvec3 GlobeView::direction(double lon,double lat) {
    lon*=radians;lat*=radians;
    return {std::cos(lat)*std::sin(lon),std::sin(lat),std::cos(lat)*std::cos(lon)};
}
double GlobeView::altitude() const {return (2.2+2.2*(1.-std::exp(-(zoom-1.)/8.)))/zoom;}
glm::dmat3 GlobeView::basis() const {
    const double l=lon*radians,p=lat*radians;
    return {{std::cos(l),0,-std::sin(l)},
            {-std::sin(p)*std::sin(l),std::cos(p),-std::sin(p)*std::cos(l)},direction(lon,lat)};
}
std::optional<MapPoint> GlobeView::pick(double x,double y,double width,double height) const {
    if(width<=0||height<=0)return std::nullopt;
    const auto b=basis();
    const glm::dvec3 ray=glm::normalize(b*glm::dvec3((x/width-.5)*2.*width/height*tangent,
        (centreY-y/height)*2.*tangent,-1.));
    const glm::dvec3 eye=b[2]*(1.+altitude());
    const double along=glm::dot(eye,ray),c=glm::dot(eye,eye)-1.;
    const double discriminant=along*along-c;
    if(discriminant<0)return std::nullopt;
    // This form avoids subtracting two almost equal distances at street level.
    const double near=c/(-along+std::sqrt(discriminant));
    if(near<=0)return std::nullopt;
    return geographic(glm::normalize(eye+ray*near));
}
std::optional<MapPixel> GlobeView::project(double longitude,double latitude,double width,double height) const {
    const auto b=basis();const auto p=direction(longitude,latitude);
    const glm::dvec3 eye=b[2]*(1.+altitude());
    if(glm::dot(p,eye-p)<=0)return std::nullopt;
    const auto v=glm::transpose(b)*(p-eye);
    return MapPixel{width*.5+v.x/-v.z*height/(2.*tangent),
                    height*centreY-v.y/-v.z*height/(2.*tangent)};
}
bool GlobeView::shows(double west,double south,double east,double north,double width,double height,glm::vec4 viewport) const {
    if(width<=0||height<=0||viewport.z<=0||viewport.w<=0)return true;
    double left=1e300,right=-1e300,top=1e300,bottom=-1e300;
    bool projected=false,hidden=false;
    for(int y=0;y<=4;++y)for(int x=0;x<=4;++x) {
        const auto p=project(west+(east-west)*x/4.,south+(north-south)*y/4.,width*viewport.z,height*viewport.w);
        if(!p){hidden=true;continue;}
        const double sx=p->x+width*viewport.x,sy=p->y+height*viewport.y;
        left=std::min(left,sx);right=std::max(right,sx);top=std::min(top,sy);bottom=std::max(bottom,sy);projected=true;
    }
    // Match the renderer's conservative horizon handling and two-pixel margin.
    if(hidden)return true;
    return projected&&right>=-2&&left<=width+2&&bottom>=-2&&top<=height+2;
}
int GlobeView::tileLevel(double height) const {
    const double scale=pi*std::max(.03,std::cos(lat*radians))*height/(256.*altitude()*tangent);
    return std::clamp(int(std::lround(std::log2(std::max(1.,scale)))),0,19);
}
std::vector<MapTile> GlobeView::tiles(double width,double height,int z,double span) const {
    const int n=1<<z;
    const double cx=(lon+180.)/360.*n;
    double minX=cx,maxX=cx,minY=n,maxY=0;
    for(int y=0;y<=6;++y)for(int x=0;x<=8;++x) {
        // A fifth beyond each side: a window wider than the interface shows
        // the globe past the document's letterbox.
        auto hit=pick(width*(.5+.7*span*(x/4.-1.)),height*(.5+.5*span*(y/3.-1.)),width,height);
        if(!hit)continue;
        const double tx=(hit->lon+180.)/360.*n;
        const double unwrapped=cx+std::remainder(tx-cx,double(n));
        const double ty=(1.-std::asinh(std::tan(std::clamp(hit->lat,-MapView::mercatorLimit,MapView::mercatorLimit)*radians))/pi)*.5*n;
        minX=std::min(minX,unwrapped);maxX=std::max(maxX,unwrapped);
        minY=std::min(minY,ty);maxY=std::max(maxY,ty);
    }
    std::vector<MapTile> result;
    for(int y=std::max(0,int(std::floor(minY)));y<=std::min(n-1,int(std::floor(maxY)));++y)
        for(int x=int(std::floor(minX));x<=int(std::floor(maxX));++x)
            result.push_back({z,(x%n+n)%n,y,0,0});
    return result;
}

} // namespace r1
