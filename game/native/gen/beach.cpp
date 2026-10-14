#include "beach.hpp"
#include "streets.hpp"
#include "terrain.hpp"
#include <sstream>
#include <iomanip>

namespace r1 {
namespace {
double smooth(double a,double b,double x) { x=std::clamp((x-a)/(b-a),0.,1.);return x*x*(3-2*x); }
P2 mix(P2 a,P2 b,double t) { return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t}; }
Ring project(const Ring& r,const Anchor& anchor) {
    Ring out;for(auto p:r){auto q=anchor.toEngine(p.x,p.y,0);out.push_back({q.x,q.z});}return out;
}
}
BeachPlan::BeachPlan(const OsmData& osm,const Anchor& anchor,const Bounds& bounds,const Sea* sea)
    :anchor_(anchor),bounds_(bounds) {
    tile_={clip::kMetres.path(project({{bounds.west,bounds.south},{bounds.east,bounds.south},
        {bounds.east,bounds.north},{bounds.west,bounds.north}},anchor))};
    if(sea)for(auto& p:sea->region)sea_.push_back(clip::kMetres.path(project(clip::kDegrees.ring(p),anchor)));
    auto add=[&](P2 a,P2 b,P2 normal) {
        if(std::hypot(b.x-a.x,b.y-a.y)<.01)return;
        // Nearest shore within 100 m agrees across neighbouring tiles; remote
        // coastline detail cannot affect this apron or its grade context.
        const auto lo=anchor.toEngine(bounds.west,bounds.north,0),hi=anchor.toEngine(bounds.east,bounds.south,0);
        if(std::max(a.x,b.x)<lo.x-150||std::min(a.x,b.x)>hi.x+150||
           std::max(a.y,b.y)<lo.z-150||std::min(a.y,b.y)>hi.z+150)return;
        shores_.push_back({a,b,normal});
        auto dry=clip::bufferRound({a,b},20),wet=clip::bufferRound({a,b},4);
        sand_.insert(sand_.end(),dry.begin(),dry.end());wet_.insert(wet_.end(),wet.begin(),wet.end());
    };
    for(const auto& coast:osm.coastlines) {
        const auto line=project(coast.points,anchor);
        for(size_t i=1;i<line.size();++i) {
            auto a=line[i-1],b=line[i];double len=std::hypot(b.x-a.x,b.y-a.y);
            // OSM has land on the left in geographic XY; engine Z points south.
            if(len>.01)add(a,b,{(b.y-a.y)/len,-(b.x-a.x)/len});
        }
    }
    // The offline coastline must not turn a tile's clipping edges into beaches.
    if(shores_.empty()&&sea)for(const auto& ring:clip::polygons(sea_)) {
        const auto& line=ring.outer;
        for(size_t i=0;i<line.size();++i) {
            auto a=line[i],b=line[(i+1)%line.size()];
            auto ga=anchor.toGeodetic(a.x,0,a.y),gb=anchor.toGeodetic(b.x,0,b.y);
            const double eps=1e-7;
            if((std::abs(ga.x-bounds.west)<eps&&std::abs(gb.x-bounds.west)<eps)||
               (std::abs(ga.x-bounds.east)<eps&&std::abs(gb.x-bounds.east)<eps)||
               (std::abs(ga.y-bounds.south)<eps&&std::abs(gb.y-bounds.south)<eps)||
               (std::abs(ga.y-bounds.north)<eps&&std::abs(gb.y-bounds.north)<eps))continue;
            double len=std::hypot(b.x-a.x,b.y-a.y);if(len<.01)continue;
            P2 n{(b.y-a.y)/len,-(b.x-a.x)/len},m=mix(a,b,.5);
            if(clip::contains(sea_,P2{m.x+n.x,m.y+n.y})){n.x=-n.x;n.y=-n.y;}
            add(a,b,n);
        }
    }
    if(shores_.empty())return;
    ports_=harbourProtection(osm,anchor);
    sand_=clip::subtract(clip::unite(sand_),ports_);wet_=clip::subtract(clip::unite(wet_),ports_);
    obstacles_=ports_;
    for(const auto& w:osm.landcover)if(w.closed()&&
        (tagOr(w.tags,"natural")=="beach"||tagOr(w.tags,"natural")=="sand"||tagOr(w.tags,"natural")=="dune"))
        mapped_.push_back(clip::kMetres.path(project(w.points,anchor)));
    mapped_=clip::subtract(clip::unite(mapped_),ports_);
    for(const auto& b:osm.buildings)if(b.closed()) {
        auto band=clip::bufferRing(project(b.points,anchor),2);
        obstacles_.insert(obstacles_.end(),band.begin(),band.end());
    }
    for(const auto& w:osm.roads)if(w.points.size()>1) {
        auto line=project(w.points,anchor);roads_.push_back(line);
        auto band=clip::bufferLine(line,roadWidth(w.tags)/2+1);
        obstacles_.insert(obstacles_.end(),band.begin(),band.end());
        if(tagOr(w.tags,"highway")=="path"||tagOr(w.tags,"highway")=="footway")paths_=clip::unite(paths_,band);
    }
    obstacles_=clip::unite(obstacles_);
    const auto candidate=clip::subtract(clip::unite(sand_,mapped_),obstacles_);
    // Roads and paths divide the apron. Only components connected to the
    // waterline remain beaches; a landward parcel cannot inherit mapped sand
    // across the promenade. Perpendicular access paths keep both shore sides.
    clip::Paths64 connected;
    for(const auto& polygon:clip::polygons(candidate)) {
        clip::Paths64 region{clip::kMetres.path(polygon.outer)};
        for(const auto& hole:polygon.holes)region.push_back(clip::kMetres.path(hole));
        if(clip::area(clip::intersect(region,wet_))>.01)
            connected.insert(connected.end(),region.begin(),region.end());
        else if(!roads_.empty())roadBack_.insert(roadBack_.end(),region.begin(),region.end());
    }
    obstacles_=clip::unite(obstacles_,roadBack_);
    mapped_=clip::subtract(mapped_,roadBack_);
    sand_=clip::intersect(connected,tile_);
    wet_=clip::intersect(clip::subtract(wet_,obstacles_),tile_);
    // Short inferred access trails only connect to real roads, with clear corridors.
    double nextAccess=60;
    for(const auto& s:shores_) {
        double len=std::hypot(s.b.x-s.a.x,s.b.y-s.a.y);
        for(;nextAccess<len;nextAccess+=120) {
            const double at=nextAccess;
            auto end=mix(s.a,s.b,at/len);end.x+=s.inland.x*12;end.y+=s.inland.y*12;
            if(!clip::contains(sand_,end))continue;
            P2 start{};double best=90;
            for(const auto& road:roads_) {
                auto p=clip::interpolate(road,clip::project(road,end));double d=std::hypot(p.x-end.x,p.y-end.y);
                if(d<best){best=d;start=p;}
            }
            if(best>=90||best<8)continue;
            auto trimmed=mix(start,end,std::min(1.,8./best));
            auto path=clip::bufferLine({trimmed,end},1.1);
            if(clip::area(clip::intersect(path,obstacles_))>.05||clip::area(clip::intersect(path,sea_))>.05)continue;
            paths_=clip::unite(paths_,clip::bufferLine({start,end},1.1));
        }
        nextAccess-=len;
    }
}
BeachSample BeachPlan::sample(P2 p) const {
    BeachSample out;double best=1e30;
    for(const auto& s:shores_) {
        double dx=s.b.x-s.a.x,dz=s.b.y-s.a.y;
        double t=std::clamp(((p.x-s.a.x)*dx+(p.y-s.a.y)*dz)/(dx*dx+dz*dz),0.,1.);
        auto q=mix(s.a,s.b,t);double d=std::hypot(p.x-q.x,p.y-q.y);
        if(d<best){best=d;out.shore=q;out.normal=s.inland;
            out.inland=((p.x-q.x)*s.inland.x+(p.y-q.y)*s.inland.y)>=0?d:-d;}
    }
    out.mapped=clip::contains(mapped_,p);return out;
}
bool BeachPlan::sand(P2 p) const { return clip::contains(sand_,p); }
ElevationGrid BeachPlan::sculpt(const ElevationGrid& input) const {
    auto out=input;if(empty())return out;
    auto height=[&](double lon,double lat,double old) {
        auto p=anchor_.toEngine(lon,lat,0);P2 at{p.x,p.z};auto s=sample(at);
        if(std::abs(s.inland)>100||clip::contains(obstacles_,at))return old;
        // Keep cliffs and surveyed relief inland. The apron blends to the source;
        // dunes only rise inside mapped broad sand, clear of roads and structures.
        double width=s.mapped?75.:20.;
        if(s.inland>width)return old;
        double h=s.inland*.085;
        if(s.inland>0&&s.mapped) {
            double dune=2.4*smooth(16,32,s.inland)*(1-smooth(50,75,s.inland));
            double ripple=.65+.35*std::sin(lon*7200+lat*3800);
            if(clip::contains(paths_,at))dune=0;
            h+=dune*ripple;
        }
        if(s.inland<0)return std::min(old,std::max(-6.,h));
        return h+(old-h)*smooth(width*.6,width,s.inland);
    };
    for(int r=0;r<out.size;++r)for(int c=0;c<out.size;++c) {
        double lon=out.bounds.west+(out.bounds.east-out.bounds.west)*c/(out.size-1);
        double lat=out.bounds.south+(out.bounds.north-out.bounds.south)*r/(out.size-1);
        out.values[size_t(r*out.size+c)]=height(lon,lat,out.at(r,c));
    }
    for(auto& p:out.southEdge)p.y=height(p.x,out.bounds.south,p.y);
    for(auto& p:out.northEdge)p.y=height(p.x,out.bounds.north,p.y);
    return out;
}
std::vector<MeshPart> BeachPlan::surfaces(const ElevationGrid& g,const clip::Paths64& streets) const {
    std::vector<MeshPart> out;if(empty())return out;Drape drape(g,anchor_);
    auto lay=[&](const char* name,const clip::Paths64& region,std::array<double,3> color,double lift,double rough) {
        Mesh m(UvMode::Planar);drape.lay(clip::subtract(region,streets),lift,m);
        if(!m.empty())out.push_back({name,smoothSurface(m),surfaceMaterial(name,color,rough,"sand_beach")});
    };
    lay("Beach sand",sand_,{.36,.29,.19},.025,.82);
    if(!out.empty()) {
        auto& m=out.back().mesh;
        for(auto p:m.positions) {
            const double dry=smooth(-1,10,sample({p.x,p.z}).inland);
            m.colors.push_back({.53+.47*dry,.50+.50*dry,.47+.53*dry});
        }
    }
    lay("Beach access trail",clip::intersect(paths_,sand_),{.27,.22,.15},.04,.9);
    return out;
}
nlohmann::json BeachPlan::water(const ElevationGrid& g,const clip::Paths64& streets) const {
    if(empty()||sand_.empty())return nullptr;
    // Drape subdivides at terrain triangles; per-vertex signed depth gives curved
    // coasts one water draw, without a shoreline loop in every fragment.
    auto coverage=clip::unite(sea_,wet_);
    coverage=clip::subtract(coverage,clip::unite(clip::subtract(obstacles_,ports_),streets));
    Mesh m;Drape(g,anchor_).lay(coverage,0,m);
    if(m.empty())return nullptr;
    std::ostringstream surface,depths;surface<<std::fixed<<std::setprecision(3);depths<<std::fixed<<std::setprecision(3);
    for(auto i:m.indices) {
        auto p=m.positions[i];auto s=sample({p.x,p.z});
        // The thin swash sheet climbs the actual sand plane, and recedes in shader.
        const bool harbour=protectedHarbour({p.x,p.z});
        surface<<p.x<<' '<<(s.inland>0&&!harbour?std::max(0.,p.y+.035):0.)<<' '<<p.z<<' ';
        depths<<(harbour?20.:-s.inland*.085)<<' ';
    }
    auto node=seaNode(bounds_,anchor_,"Coastal surf");
    node["surface"]=surface.str();node["surfaceDepths"]=depths.str();
    node["shoreMode"]=1;node["shoreFoam"]=.85;node["foamWidth"]=.12;
    node["swashAmount"]=.28;node["swashSpeed"]=.75;node["edgeFade"]=.045;
    // Offshore spectrum is identical to neighbouring sea tiles.
    node["waveType"]=1;node["waveIntensity"]=1.;
    node["transparency"]=.35;
    return node;
}
nlohmann::json BeachPlan::population(const ElevationGrid& g,const clip::Paths64& streets) const {
    auto out=nlohmann::json::array();if(empty())return out;
    Drape ground(g,anchor_);
    const auto blocked=clip::unite(obstacles_,streets);
    // Global geographic lattice: stable across regeneration and tile boundaries.
    constexpr double step=.00012;
    for(int64_t r=int64_t(std::ceil(bounds_.south/step));r*step<bounds_.north;++r)
    for(int64_t c=int64_t(std::ceil(bounds_.west/step));c*step<bounds_.east;++c) {
        auto q=anchor_.toEngine(c*step,r*step,0);P2 p{q.x,q.z};auto s=sample(p);
        if(s.inland<8||s.inland>(s.mapped?60:17)||!sand(p)||clip::contains(paths_,p)||clip::contains(streets,p))continue;
        const double y=ground.heightAt(p);
        if(y<.25||y>6||std::abs(ground.heightAt({p.x+1,p.y})-y)>.18||std::abs(ground.heightAt({p.x,p.y+1})-y)>.18)continue;
        P2 sea{s.shore.x-s.normal.x*12,s.shore.y-s.normal.y*12};
        auto route=clip::bufferLine({p,sea},1.4);
        if(clip::area(clip::intersect(route,blocked))>.01)continue;
        if(!clip::contains(sea_,sea))continue;
        out.push_back({{"towel",{p.x,y+.06,p.y}},{"water",{sea.x,-.85,sea.y}},
            {"parasol",((uint64_t(r)*73856093u)^(uint64_t(c)*19349663u))%3==0}});
        if(out.size()>=80)return out;
    }
    return out;
}
nlohmann::json BeachPlan::report() const {
    return {{"source","inferred coastal apron; mapped OSM sand extent retained"},{"shoreSegments",shores_.size()},
        {"sandAreaM2",clip::area(sand_)},{"mappedSandAreaM2",clip::area(clip::intersect(mapped_,tile_))},
        {"harbourProtectedAreaM2",clip::area(clip::intersect(ports_,tile_))},
        {"roadBackExcludedAreaM2",clip::area(clip::intersect(roadBack_,tile_))},
        {"accessAreaM2",clip::area(clip::intersect(paths_,sand_))},{"populationSource","inferred; live daylight, temperature and weather"}};
}
double beachAttendance(double hour,double sun,const WeatherState& w) {
    if(!w.known||!w.temperatureKnown||!std::isfinite(hour)||!std::isfinite(sun)||sun<=3||hour<=8||hour>=19||
       !std::isfinite(w.temperature)||w.temperature<=18||w.rain>0||w.snowfall>0||w.snowDepth>0||
       w.windSpeed>=9||stormWeatherCode(w.code)||w.cover>=.95)return 0;
    return smooth(18,30,w.temperature)*smooth(8,10,hour)*(1-smooth(17,19,hour))*
        (1-.85*std::clamp(w.cover,0.,1.))*(1-smooth(5,9,w.windSpeed));
}
BeachPose beachPose(P3 towel,P3 water,double seconds,unsigned slot) {
    const double dx=water.x-towel.x,dz=water.z-towel.z,len=std::hypot(dx,dz);
    const double travel=len/.85,rest=65+(slot*17)%65,bathe=25+(slot*13)%35;
    const double cycle=rest+6+travel*2+bathe;
    double t=std::fmod(std::max(0.,seconds)+slot*31.,cycle);
    BeachPose p;p.position=towel;p.heading=std::atan2(dx,-dz);
    if(t<rest){p.recline=1;return p;}t-=rest;
    if(t<3){p.recline=1-smooth(0,3,t);return p;}t-=3;
    double along=0;
    if(t<travel){along=t/std::max(travel,.01);p.walking=true;}
    else if((t-=travel)<bathe){along=1;p.swimming=true;}
    else if((t-=bathe)<travel){along=1-t/std::max(travel,.01);p.heading+=kPi;p.walking=true;}
    else {p.recline=smooth(0,3,t-travel);}
    p.position={towel.x+dx*along,towel.y+(water.y-towel.y)*along,towel.z+dz*along};return p;
}
std::vector<MeshPart> beachFurniture(bool parasol,unsigned palette) {
    const std::array<std::array<double,3>,4> colors{{{.035,.16,.22},{.32,.095,.06},{.26,.21,.075},{.12,.19,.10}}};
    auto color=colors[palette%4];Mesh cloth(UvMode::Planar),trim(UvMode::Planar),frame;
    cloth.addUpQuad({-.48,.015,-.9},{.48,.015,-.9},{.48,.015,.9},{-.48,.015,.9});
    for(double z:{-.78,.72})trim.addUpQuad({-.48,.017,z},{.48,.017,z},{.48,.017,z+.07},{-.48,.017,z+.07});
    if(parasol) {
        frame.addBox({1.35,1.02,.35},{.035,2.04,.035});
        for(int i=0;i<12;++i) {
            double a=i*2*kPi/12,b=(i+1)*2*kPi/12;
            P3 top{1.35,2.18,.35},mid{1.35+.65*std::cos(a),2.05,.35+.65*std::sin(a)},
                next{1.35+.65*std::cos(b),2.05,.35+.65*std::sin(b)},
                edge{1.35+1.2*std::cos(a),1.82,.35+1.2*std::sin(a)},
                edgeNext{1.35+1.2*std::cos(b),1.82,.35+1.2*std::sin(b)};
            auto& m=i%2?trim:cloth;m.addUpTriangle(top,mid,next);m.addUpQuad(mid,edge,edgeNext,next);
            m.addQuad(edge,edgeNext,{edgeNext.x,edgeNext.y-.08,edgeNext.z},{edge.x,edge.y-.08,edge.z});
        }
    }
    return {{"Beach woven cloth",std::move(cloth),surfaceMaterial("Beach cloth",color,.94,std::nullopt,true)},
        {"Beach cream stripes",std::move(trim),surfaceMaterial("Beach cream",{.36,.32,.25},.94,std::nullopt,true)},
        {"Beach parasol pole",std::move(frame),surfaceMaterial("Beach pole",{.19,.16,.10},.7,std::nullopt)}};
}
} // namespace r1
