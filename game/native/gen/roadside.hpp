#pragma once

#include "polygons.hpp"
#include <algorithm>
#include <optional>
#include <limits>

namespace r1 {

// Even-odd contours of the actual asphalt/cobble surface, including islands.
class RoadSurface {
public:
    std::vector<Ring> contours;
    bool contains(P2 point) const {
        bool inside=false;
        for(const auto& ring:contours)if(pointInPolygon(point,ring))inside=!inside;
        return inside;
    }
    // Find the first clear roadside along the outward ray. Lane widths cannot
    // describe overlapping carriageways or the rounded mouth of a junction.
    std::optional<P2> roadside(P2 origin,P2 outward,double clearance,double reach) const {
        constexpr double kTolerance=1e-7;
        const double magnitude=std::hypot(outward.x,outward.y);
        if(contours.empty()||magnitude<kTolerance||clearance<0||reach<=0)return std::nullopt;
        outward.x/=magnitude;outward.y/=magnitude;
        std::vector<double> candidates;
        auto cross=[](P2 a,P2 b){return a.x*b.y-a.y*b.x;};
        for(const auto& ring:contours)for(size_t i=0;i<ring.size();++i) {
            const P2 a=ring[i],b=ring[(i+1)%ring.size()],edge{b.x-a.x,b.y-a.y},delta{a.x-origin.x,a.y-origin.y};
            const double denominator=cross(outward,edge);
            if(std::abs(denominator)<kTolerance)continue;
            const double distance=cross(delta,edge)/denominator,along=cross(delta,outward)/denominator;
            if(distance<0||along<0||along>1)continue;
            // Clearance is perpendicular to the edge, even on a diagonal kerb.
            const double candidate=distance+clearance*std::hypot(edge.x,edge.y)/std::abs(denominator);
            if(candidate<=reach)candidates.push_back(candidate);
        }
        std::sort(candidates.begin(),candidates.end());
        for(double distance:candidates) {
            const P2 point{origin.x+outward.x*distance,origin.y+outward.y*distance};
            if(contains(point))continue;
            double margin=std::numeric_limits<double>::max();
            for(const auto& ring:contours)for(size_t i=0;i<ring.size();++i) {
                const P2 a=ring[i],b=ring[(i+1)%ring.size()];
                const double x=b.x-a.x,y=b.y-a.y,squared=x*x+y*y;
                const double t=squared>kTolerance?std::clamp(((point.x-a.x)*x+(point.y-a.y)*y)/squared,0.,1.):0.;
                margin=std::min(margin,std::hypot(point.x-a.x-t*x,point.y-a.y-t*y));
            }
            if(margin+kTolerance>=clearance)return point;
        }
        return std::nullopt;
    }
};

} // namespace r1
