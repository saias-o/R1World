#include "road_details.hpp"
#include "palette.hpp"

#include <algorithm>
#include <map>

namespace r1 {
namespace {
constexpr double kEdgeWidth = .15, kLaneWidth = .12;
constexpr double kRailHeight = .75, kPostSpacing = 4.0, kRailStep = 4.0;
constexpr double kRoadPaintLift = .063, kRailSetback = .5;

struct Axis {
    const RoadAxis& road;
    RoadProfile profile;
    std::vector<P2> xz;
    std::vector<P2> offsets;
    std::vector<double> along{0};
    Axis(const RoadAxis& r) : road(r), profile(roadProfile(r.tags)) {
        for (const P3& p : r.points) {
            xz.push_back({p.x, p.z});
            if (xz.size() > 1) along.push_back(along.back() + dist(xz[xz.size()-2], xz.back()));
        }
        for(size_t i=0;i<xz.size();++i) {
            auto tangent=[&](size_t k){const double n=std::max(1e-9,along[k+1]-along[k]);return P2{(xz[k+1].x-xz[k].x)/n,(xz[k+1].y-xz[k].y)/n};};
            const P2 a=tangent(i?i-1:0),b=tangent(std::min(i,xz.size()-2));
            P2 right{-a.y-b.y,a.x+b.x};const double n=std::hypot(right.x,right.y);
            right=n>1e-9?P2{right.x/n,right.y/n}:P2{-b.y,b.x};
            const double miter=1/std::max(.4,-right.x*b.y+right.y*b.x);
            offsets.push_back({right.x*miter,right.y*miter});
        }
    }
    double length() const { return along.back(); }
    P3 at(double s, double offset, const Drape* ground) const {
        s = std::clamp(s, 0.0, length());
        size_t i = size_t(std::upper_bound(along.begin(), along.end(), s) - along.begin());
        i = std::clamp<size_t>(i, 1, along.size()-1)-1;
        const double span = std::max(1e-9, along[i+1]-along[i]);
        const double t = (s-along[i])/span;
        const P3& a = road.points[i]; const P3& b = road.points[i+1];
        const P2 right{offsets[i].x+(offsets[i+1].x-offsets[i].x)*t,offsets[i].y+(offsets[i+1].y-offsets[i].y)*t};
        P3 p{a.x+(b.x-a.x)*t+right.x*offset,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t+right.y*offset};
        if (ground) p.y = ground->heightAt({p.x,p.z});
        return p;
    }
};

bool paints(const Axis& a) {
    const auto& tags = a.road.tags;
    const std::string h = tagOr(tags,"highway"), surface = tagOr(tags,"surface");
    if (!isMotorway(h) || tagOr(tags,"lane_markings") == "no" || h == "service" || h == "living_street") return false;
    if (!surface.empty() && surface != "asphalt" && surface != "concrete" && surface != "concrete:plates") return false;
    return a.profile.express || a.profile.lanesTagged || h == "primary" || h == "secondary" || h == "tertiary";
}

bool rails(const Axis& a) {
    const std::string barrier = tagOr(a.road.tags,"guardrail",tagOr(a.road.tags,"barrier"));
    if (barrier == "no" || barrier == "none" || a.road.bridge) return false;
    return barrier == "yes" || barrier == "guard_rail" || (a.profile.express && a.profile.direction);
}

void post(Mesh& mesh, P3 p, P2 forward) {
    const P2 right{-forward.y,forward.x};
    auto at = [&](double x,double z,double y) { return P3{p.x+right.x*x+forward.x*z,p.y+y,p.z+right.y*x+forward.y*z}; };
    // Folded steel post: two flanges and the web, rather than a solid cube.
    for (double z : {-.045,.045}) mesh.addQuad(at(-.045,z,0),at(.045,z,0),at(.045,z,.72),at(-.045,z,.72));
    mesh.addQuad(at(0,-.045,0),at(0,.045,0),at(0,.045,.72),at(0,-.045,.72));
}

void rail(Mesh& mesh, P3 a, P3 b) {
    const double span = std::hypot(b.x-a.x,b.z-a.z);
    if (span < .05) return;
    const P2 n{-(b.z-a.z)/span,(b.x-a.x)/span};
    // A corrugated W-beam catches the light along its folds.
    const double y[] = {kRailHeight-.155,kRailHeight-.08,kRailHeight,kRailHeight+.08,kRailHeight+.155};
    const double x[] = {0,.045,0,.045,0};
    auto at = [&](P3 p,int k) { return P3{p.x+n.x*x[k],p.y+y[k],p.z+n.y*x[k]}; };
    for (int k=0;k<4;++k) mesh.addQuad(at(a,k),at(b,k),at(b,k+1),at(a,k+1));
}
}

RoadDetails buildRoadDetails(const std::vector<RoadAxis>& roads, const clip::Paths64& tile,
                             const clip::Paths64& asphalt, const Drape* ground,
                             const std::string& country, const std::vector<RoadAxis>& neighbours) {
    RoadDetails out;
    Mesh white, yellow, steel;
    clip::Paths64 whitePaths, yellowPaths;
    std::vector<Axis> axes, others;
    for (const auto& r : roads) if (r.points.size()>1) axes.emplace_back(r);
    for (const auto& r : neighbours) if (r.points.size()>1) others.emplace_back(r);
    const auto& network = others.empty() ? axes : others;
    int marked=0, guarded=0,guardedTagged=0;
    double railMetres=0;
    auto owned = [&](P3 p) { return clip::contains(tile,P2{p.x,p.z}); };
    auto blocked = [&](const Axis& axis, P3 p) {
        for (const auto& other : network) {
            if (other.road.id==axis.road.id || !isMotorway(tagOr(other.road.tags,"highway"))) continue;
            if (clip::distance(other.xz,{p.x,p.z}) > other.profile.width/2+.35) continue;
            const double s=clip::project(other.xz,{p.x,p.z});
            const P3 surface=other.at(s,0,other.road.bridge || has(other.road.tags,"r1:raised") ? nullptr : ground);
            if (std::abs(surface.y-p.y)<1.0) return true;
        }
        return false;
    };
    // Shared coordinates with three distinct branches are at-grade junctions.
    std::map<P2,std::vector<P2>> branches;
    for (const auto& a : axes) for (size_t i=0;i<a.xz.size();++i) {
        auto add=[&](P2 p) {
            P2 d{p.x-a.xz[i].x,p.y-a.xz[i].y};const double len=std::hypot(d.x,d.y);
            if(len<.01)return;
            d={d.x/len,d.y/len};
            auto& dirs=branches[a.xz[i]];
            if(std::none_of(dirs.begin(),dirs.end(),[&](P2 v){return v.x*d.x+v.y*d.y>.99;}))dirs.push_back(d);
        };
        if(i)add(a.xz[i-1]);
        if(i+1<a.xz.size())add(a.xz[i+1]);
    }
    for (const Axis& a : axes) {
        const auto& p=a.profile;
        auto paint = [&](double begin,double end,double offset,double width,bool centre,bool edge) {
            for(double s=begin;s<end-.01;) {
                const auto next=std::upper_bound(a.along.begin(),a.along.end(),s+.001);
                const double stop=std::min({end,s+8.0,next==a.along.end()?end:*next});
                const P3 middle=a.at((s+stop)/2,offset,ground);
                bool junction=false;
                if(!p.express)for(size_t i=0;i<a.xz.size();++i)
                    if(branches[a.xz[i]].size()>2 && std::abs(a.along[i]-(s+stop)/2)<p.width)junction=true;
                if(!junction && (!edge || !blocked(a,middle))) {
                    const P3 pts[]={a.at(s,offset-width/2,ground),a.at(s,offset+width/2,ground),
                                    a.at(stop,offset+width/2,ground),a.at(stop,offset-width/2,ground)};
                    clip::Paths64 shape{clip::kMetres.path({{pts[0].x,pts[0].z},{pts[1].x,pts[1].z},{pts[2].x,pts[2].z},{pts[3].x,pts[3].z}})};
                    shape=clip::intersect(shape,tile);
                    auto& paths=centre && country=="US" ? yellowPaths : whitePaths;
                    paths.insert(paths.end(),shape.begin(),shape.end());
                    if(!ground) {
                        Mesh& mesh=centre && country=="US" ? yellow : white;
                        for(const auto& poly:clip::polygons(shape))for(const auto& tri:clip::triangles(poly)) {
                            P3 v[3];for(int k=0;k<3;++k){const double along=clip::project(a.xz,tri[k]);v[k]={tri[k].x,a.at(along,0,nullptr).y+kRoadPaintLift,tri[k].y};}
                            mesh.addUpTriangle(v[0],v[1],v[2]);
                        }
                    }
                }
                s=stop;
            }
        };
        if(paints(a)) {
            ++marked;
            const double left=-p.width/2+p.leftShoulder+.15, right=p.width/2-p.rightShoulder-.15;
            if(p.express || p.width>=6.5) {
                auto edge=[&](double offset,bool outer) {
                    if(country=="FR" && p.express && !p.link && outer) {
                        const double phase=double(uint64_t(a.road.id)%52);
                        for(double s=-phase;s<a.length();s+=52)paint(std::max(0.0,s),std::min(a.length(),s+39),offset,.225,false,true);
                    } else paint(0,a.length(),offset,p.express?.225:kEdgeWidth,false,true);
                };
                edge(left,p.direction<0);edge(right,p.direction>0);
            }
            for(int lane=1;lane<p.lanes;++lane) {
                const double offset=-p.width/2+p.leftShoulder+p.travelWidth()*lane/p.lanes;
                const bool centre=!p.direction && lane==(p.lanes+1)/2;
                // T1 modulation (3 m paint / 10 m gap).
                // Seed phase by way identity; full axes are retained across tiles.
                const double dash=3.0, gap=10.0, period=dash+gap;
                const double phase=double(uint64_t(a.road.id)%uint64_t(period));
                for(double s=-phase;s<a.length();s+=period)paint(std::max(0.0,s),std::min(a.length(),s+dash),offset,kLaneWidth,centre,false);
            }
        }
        if(rails(a)) {
            ++guarded;
            guardedTagged+=has(a.road.tags,"guardrail") || tagOr(a.road.tags,"barrier")=="guard_rail";
            for(int side=0;side<2;++side) {
                const std::string key=side?"guardrail:right":"guardrail:left";
                if(tagOr(a.road.tags,key.c_str())=="no")continue;
                const double offset=(side?1.0:-1.0)*(p.width/2+kRailSetback);
                for(double s=0;s<a.length();) {
                    const double begin=s;
                    const auto next=std::upper_bound(a.along.begin(),a.along.end(),begin+.001);
                    const double end=std::min({a.length(),(std::floor(begin/kRailStep+.0001)+1)*kRailStep,next==a.along.end()?a.length():*next});
                    s=end;
                    const P3 first=a.at(begin,offset,ground), last=a.at(end,offset,ground), middle=a.at((begin+end)/2,offset,ground);
                    if(!owned(middle) || blocked(a,first) || blocked(a,middle) || blocked(a,last))continue;
                    rail(steel,first,last);railMetres+=end-begin;
                    if(owned(first) && std::fmod(begin,kPostSpacing)<.01) {
                        const double len=std::hypot(last.x-first.x,last.z-first.z);
                        if(len>.05)post(steel,first,{(last.x-first.x)/len,(last.z-first.z)/len});
                    }
                }
            }
        }
    }
    if(ground) {
        ground->lay(clip::intersect(clip::unite(whitePaths),asphalt),kRoadPaintLift,white);
        ground->lay(clip::intersect(clip::unite(yellowPaths),asphalt),kRoadPaintLift,yellow);
        // Opposing, parallel carriageways retain their mapped separation.
        // Only the narrow gap becomes an inferred grass median, never asphalt.
        clip::Paths64 medians;
        for(const auto& a:axes) {
            if(!a.profile.express || !a.profile.direction || a.profile.link)continue;
            for(const auto& b:axes) {
                if(b.road.id<=a.road.id || !b.profile.express || !b.profile.direction || b.profile.link)continue;
                for(size_t i=0;i+1<a.xz.size();++i) {
                    const P2 first=a.xz[i],last=a.xz[i+1];
                    const double s0=clip::project(b.xz,first),s1=clip::project(b.xz,last);
                    const P3 q0=b.at(s0,0,ground),q1=b.at(s1,0,ground);
                    const double len=dist(first,last),other=std::hypot(q1.x-q0.x,q1.z-q0.z);
                    if(len<.1 || other<len*.8 || other>len*1.2)continue;
                    const double cosine=((last.x-first.x)*(q1.x-q0.x)+(last.y-first.y)*(q1.z-q0.z))/(len*other);
                    if(cosine<.94 || a.profile.direction*b.profile.direction*(s1>s0?1:-1)>0)continue;
                    const double d0=dist(first,{q0.x,q0.z}),d1=dist(last,{q1.x,q1.z});
                    const double half=a.profile.width/2+b.profile.width/2;
                    if(std::min(d0,d1)<half+.25 || std::max(d0,d1)>half+12)continue;
                    auto inner=[](P2 x,P2 y,double width,double len){return P2{x.x+(y.x-x.x)*width/len,x.y+(y.y-x.y)*width/len};};
                    medians.push_back(clip::kMetres.path({inner(first,{q0.x,q0.z},a.profile.width/2,d0),
                        inner(last,{q1.x,q1.z},a.profile.width/2,d1),inner({q1.x,q1.z},last,b.profile.width/2,d1),inner({q0.x,q0.z},first,b.profile.width/2,d0)}));
                }
            }
        }
        out.medians=clip::subtract(clip::intersect(clip::unite(medians),tile),asphalt);
        Mesh grass(UvMode::Planar);ground->lay(out.medians,.02,grass);
        if(!grass.empty())out.parts.push_back({"Expressway grass medians",smoothSurface(grass),surfaceMaterial("Median grass",{.14,.20,.08},.95,std::string("grass"))});
    }
    if(!white.empty())out.parts.push_back({"Road lane and edge markings",smoothSurface(white),surfaceMaterial("Road paint",{.50,.49,.46},.75,std::nullopt)});
    if(!yellow.empty())out.parts.push_back({"Road centre markings",smoothSurface(yellow),surfaceMaterial("Yellow road paint",{.50,.36,.04},.75,std::nullopt)});
    if(!steel.empty()) {
        auto material=surfaceMaterial("Galvanized guardrails",{.42,.44,.46},.32,std::nullopt,true);material.metallic=.95;
        out.parts.push_back({"Road guardrails",std::move(steel),material});
    }
    out.stats={{"markedWaysInferred",marked},{"guardedWays",guarded},{"guardedWaysTagged",guardedTagged},
               {"guardedWaysInferred",guarded-guardedTagged},{"guardrailMetres",pyround(railMetres,1)},
               {"medianAreaM2Inferred",pyround(clip::area(out.medians),2)},
               {"markingCountry",country},{"markingPolicy","OSM lanes/absence then class defaults"},
               {"guardrailPolicy","OSM override then divided expressway; junction gaps"}};
    return out;
}
} // namespace r1
