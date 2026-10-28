#include "geography.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace r1::world {
double wrap(double x) { return x - 360.*std::floor((x+180.)/360.); }
Tile tileAt(double lon,double lat) {
    const r1::Tile t=r1::tileAt(lon,lat);
    return {t.row,t.col};
}
// Metres along the sphere between two points.
double metresBetween(double lon1,double lat1,double lon2,double lat2) {
    const double p1=lat1*rad,p2=lat2*rad,dp=p2-p1,dl=wrap(lon2-lon1)*rad;
    const double h=std::sin(dp/2)*std::sin(dp/2)+std::cos(p1)*std::cos(p2)*std::sin(dl/2)*std::sin(dl/2);
    return 2*r1::kRMean*std::asin(std::min(1.,std::sqrt(h)));
}
std::vector<Tile> polarNearby(double lon,double lat) {
    const Tile center=tileAt(lon,lat);
    std::vector<std::pair<double,Tile>> found;
    for(int r=std::max(0,center.r-2);r<=std::min(kLastRow,center.r+2);++r) {
        const int n=columns(r);
        for(int c=0;c<n;++c) {
            const Tile t{r,c};
            const r1::Bounds b=t.gen().bounds();
            const double la=std::clamp(lat,b.south,b.north);
            double lo=wrap(lon);
            if(lo<b.west||lo>b.east)lo=std::abs(wrap(lo-b.west))<std::abs(wrap(lo-b.east))?b.west:b.east;
            const double d=t==center?-1.:metresBetween(lon,lat,lo,la);
            if(d<=kPolarReach)found.push_back({d,t});
        }
    }
    std::stable_sort(found.begin(),found.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    std::vector<Tile> out;
    for(const auto& f:found)if(out.size()<kPolarTiles)out.push_back(f.second);
    return out;
}
std::vector<Tile> nearby(double lon,double lat) {
    Tile center=tileAt(lon,lat);
    if(columns(center.r)<kPolarColumns)return polarNearby(lon,lat);
    std::vector<Tile> out{center}; std::set<Tile> seen{center};
    for(int dr=-1;dr<=1;++dr) {
        int r=std::clamp(center.r+dr,0,kLastRow),n=columns(r);
        int c=int(std::floor((wrap(lon)+180.)/360.*n));
        for(int dc=-1;dc<=1;++dc) {
            Tile t{r,(c+dc+n)%n}; if(seen.insert(t).second)out.push_back(t);
        }
    }
    // Center first, then the closest terrain: the next boundary matters more
    // than the arbitrary row/column iteration order. Wrap at the date line.
    auto distance=[&](Tile t) {
        const r1::P2 middle=t.gen().center();
        return std::hypot(wrap(middle.x-lon)*std::cos(lat*rad),middle.y-lat);
    };
    std::stable_sort(out.begin()+1,out.end(),[&](Tile a,Tile b){return distance(a)<distance(b);});
    return out;
}
glm::dvec3 ecef(double lon,double lat,double alt) {
    const r1::P3 p=r1::geodeticToEcef(lon,lat,alt);
    return {p.x,p.y,p.z};
}
// Great-circle step; longitude wraps across the date line. Written as a
// rotation of the unit vector rather than through asin: at the pole asin
// sees cos(d) round to exactly 1 for a step of a few centimetres, and the
// player could never walk off it. atan2 keeps every step, there as anywhere.
glm::dvec2 advance(double lon,double lat,double east,double north) {
    const double length=std::hypot(east,north),d=length/r1::kRMean;
    if(d==0)return {lon,lat};
    const double l=lon*rad,p=lat*rad;
    const glm::dvec3 here(std::cos(p)*std::cos(l),std::cos(p)*std::sin(l),std::sin(p));
    const glm::dvec3 e(-std::sin(l),std::cos(l),0.),n(-std::sin(p)*std::cos(l),-std::sin(p)*std::sin(l),std::cos(p));
    const glm::dvec3 q=here*std::cos(d)+(e*east+n*north)*(std::sin(d)/length);
    return {wrap(std::atan2(q.y,q.x)/rad),std::atan2(q.z,std::hypot(q.x,q.y))/rad};
}

} // namespace r1::world
