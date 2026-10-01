#include "retail.hpp"
#include "buildings.hpp"
#include "spatial.hpp"
#include <limits>
#include <optional>
namespace r1 {
namespace {
Bounds extent(const Ring& ring) {
    Bounds b{1e30,1e30,-1e30,-1e30};
    for (auto p:ring) {
        b.west=std::min(b.west,p.x);b.east=std::max(b.east,p.x);
        b.south=std::min(b.south,p.y);b.north=std::max(b.north,p.y);
    }
    return b;
}
bool overlaps(const Bounds& a,const Bounds& b) {
    return a.west<=b.east&&a.east>=b.west&&a.south<=b.north&&a.north>=b.south;
}
bool parkingCandidate(const Tags& t) {
    if(tagOr(t,"amenity")=="parking")
        return tagOr(t,"parking")!="underground"&&tagOr(t,"parking")!="multi-storey"&&tagOr(t,"access")!="private";
    return tagOr(t,"landuse")=="retail" || (tagOr(t,"landuse")=="commercial"&&tagOr(t,"surface")=="asphalt");
}
}
std::vector<OsmWay> retailBuildings(const std::vector<const OsmWay*>& ways,const OsmData& osm,
                                    const std::function<P3(double,double)>& ground) {
    std::vector<const OsmNode*> tenants,entrances;
    for(const auto& n:osm.features) {
        if(retailUse(n.tags))tenants.push_back(&n);
        // Only a door the public walks in by: a staff, emergency or exit-only
        // door is no shop entrance (Le Fourchêne, Vannes, maps only these).
        const auto kind=tagOr(n.tags,"entrance"),access=tagOr(n.tags,"access");
        if(has(n.tags,"entrance")&&kind!="no"&&kind!="service"&&kind!="emergency"&&kind!="exit"&&
           access!="private"&&access!="no"&&access!="staff")entrances.push_back(&n);
    }
    // Each scan visits only what can match, in the order of the full scan
    // (gen/spatial.hpp). The sampled ground is the same wherever it is asked.
    BoxIndex tenantIndex(.002);
    for(size_t i=0;i<tenants.size();++i)tenantIndex.add(i,{tenants[i]->lon,tenants[i]->lat,tenants[i]->lon,tenants[i]->lat});
    // Entrances and the road points: only when a store needs them.
    std::vector<P2> entrancePoints;
    std::optional<BoxIndex> entranceIndex;
    auto entrancesNear=[&](const Box& box) {
        if(!entranceIndex) {
            entranceIndex.emplace(32.);
            for(auto* n:entrances){auto p=ground(n->lon,n->lat);entranceIndex->add(entrancePoints.size(),{p.x,p.z,p.x,p.z});entrancePoints.push_back({p.x,p.z});}
        }
        return entranceIndex->near(box);
    };
    std::optional<NearestPoint> roadIndex;
    std::vector<P2> parkings;
    // A point a hair outside a ring's box is outside the ring, whatever the rounding.
    constexpr double kOutside=1e-9;
    std::vector<OsmWay> out;out.reserve(ways.size());
    for(auto* w:ways) {
        out.push_back(*w);auto& b=out.back();
        const Box box=boxOf(b.points).grown(kOutside);
        if(!retailUse(b.tags))for(size_t i:tenantIndex.near(box))
            if(auto* n=tenants[i];box.contains({n->lon,n->lat})&&pointInPolygon({n->lon,n->lat},b.points)) {
                for(auto key:{"shop","name","brand"})if(has(n->tags,key)&&!has(b.tags,key))b.tags[key]=tagOr(n->tags,key);
                b.tags["r1:tenant"]=std::to_string(n->id);break;
            }
        if(!retailUse(b.tags))continue;
        // A mall holding a mapped supermarket: its position anchors that
        // store's sales floor. The one nearest the middle, if several (a
        // "Drive" pick-up counter is mapped at the edge).
        if(retailRecipe(b.tags)=="mall") {
            const OsmNode* anchor=nullptr;double nearest=1e30;const P2 middle=centroid(b.points);
            for(auto* n:tenants)if(tagOr(n->tags,"shop")=="supermarket"&&pointInPolygon({n->lon,n->lat},b.points)) {
                const double d=std::hypot((n->lon-middle.x)*std::cos(radians(middle.y)),n->lat-middle.y);
                if(d<nearest){nearest=d;anchor=n;}
            }
            if(anchor) {
                const auto p=ground(anchor->lon,anchor->lat);
                b.tags["r1:anchor"]=std::to_string(p.x)+","+std::to_string(p.z);
                b.tags["r1:anchorName"]=retailName(anchor->tags);
            }
        }
        P2 center=centroid(b.points),target=center;double best=1e30;
        // Every mapped entrance on the outline is a candidate: the portal
        // planner keeps the one that opens on a clear aisle.
        std::string fronts;
        Ring outline;for(auto q:b.points){auto a=ground(q.x,q.y);outline.push_back({a.x,a.z});}
        // An entrance a metre past the outline's box is a metre from every edge.
        for(size_t i:entrancesNear(boxOf(outline).grown(1.01))) {
            const P2 p=entrancePoints[i];double near=1e30;
            for(size_t e=1;e<outline.size();++e)near=std::min(near,clip::distance({outline[e-1],outline[e]},p));
            if(near<1.)fronts+=(fronts.empty()?"":";")+std::to_string(p.x)+","+std::to_string(p.y);
        }
        const bool entrance=!fronts.empty();
        if(!entrance) {
            best=1e30;
            // Ranking nearby observations needs no elevation sampling or ECEF
            // conversion for every vertex of every road in the neighbourhood.
            const double longitudeScale=111320.*std::cos(radians(center.y));
            auto metric=[&](P2 q){return std::hypot(wrap(q.x-center.x)*longitudeScale,(q.y-center.y)*111132.);};
            auto consider=[&](P2 q){double d=metric(q);
                if(d<best){best=d;target=q;}};
            if(!roadIndex) {
                for(auto& p:osm.landcover)if(tagOr(p.tags,"amenity")=="parking"&&tagOr(p.tags,"parking")!="underground")parkings.push_back(centroid(p.points));
                std::vector<P2> points;
                for(auto& r:osm.roads)points.insert(points.end(),r.points.begin(),r.points.end());
                roadIndex.emplace(std::move(points),.0005);
            }
            for(auto q:parkings)consider(q);
            if(best>120) {
                best=1e30;
                const size_t nearest=roadIndex->nearest(center,std::min(longitudeScale,111132.),metric,best);
                if(nearest!=NearestPoint::npos)target=roadIndex->points()[nearest];
            }
        }
        if(!entrance){auto p=ground(target.x,target.y);fronts=std::to_string(p.x)+","+std::to_string(p.z);}
        b.tags["r1:front"]=fronts;
        b.tags["r1:entranceSource"]=entrance?"measured":"inferred";
    }
    return out;
}
ParkingOutput buildRetailParking(const OsmData& osm,const std::vector<InteriorPlan>& shops,
    const std::vector<Ring>& footprints,const ElevationGrid& elevations,const Anchor& anchor) {
    ParkingOutput out;
    std::vector<const OsmWay*> lots;
    Bounds work{1e30,1e30,-1e30,-1e30};
    for(const auto& w:osm.landcover)if(parkingCandidate(w.tags)) {
        const auto bounds=extent(w.points);if(!overlaps(bounds,elevations.bounds))continue;
        lots.push_back(&w);work.west=std::min(work.west,bounds.west);work.east=std::max(work.east,bounds.east);
        work.south=std::min(work.south,bounds.south);work.north=std::max(work.north,bounds.north);
    }
    // Most city tiles have no eligible lot: they pay no polygon unions.
    if(lots.empty())return out;
    work.west-=.0002;work.east+=.0002;work.south-=.0002;work.north+=.0002;
    const Drape drape(elevations,anchor);
    auto project=[&](const Ring& r){Ring out;for(auto p:r){auto q=anchor.toEngine(p.x,p.y,0);out.push_back({q.x,q.z});}return out;};
    // The parking may belong to a shop owned by the neighbouring tile. Reuse
    // the same portal planner in this tile's frame, so markings cross seams.
    std::vector<InteriorPlan> nearby=shops;
    std::vector<const OsmWay*> foreign;
    for(const auto& w:osm.buildings)if(retailUse(w.tags)&&std::none_of(shops.begin(),shops.end(),[&](const auto& p){return p.id==w.id;}))foreign.push_back(&w);
    auto placed=retailBuildings(foreign,osm,[&](double x,double y){return anchor.toEngine(x,y,0);});
    for(const auto& w:placed) {
        auto ring=cleanFootprint(project(w.points));if(!ring)continue;
        InteriorPlan p;p.id=w.id;p.ring=*ring;p.name=retailName(w.tags);
        if(chooseRetailPortal(p,retailFronts(w.tags)))nearby.push_back(std::move(p));
    }
    if(nearby.empty())return out;
    std::sort(nearby.begin(),nearby.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    clip::Paths64 excluded;
    auto exclude=[&](const clip::Paths64& paths){excluded.insert(excluded.end(),paths.begin(),paths.end());};
    for(auto& r:footprints)exclude(clip::bufferRing(r,.8));
    for(auto& w:osm.buildings)if(overlaps(extent(w.points),work))exclude(clip::bufferRing(project(w.points),.8));
    for(auto& w:osm.roads)if(overlaps(extent(w.points),work))exclude(clip::bufferLine(project(w.points),roadWidth(w.tags)/2+.3));
    for(auto& w:osm.landcover)if(overlaps(extent(w.points),work)&&(tagOr(w.tags,"natural")=="water"||tagOr(w.tags,"landuse")=="grass"||tagOr(w.tags,"leisure")=="park"))
        exclude({clip::kMetres.path(project(w.points))});
    excluded=clip::unite(excluded);
    Mesh asphalt(UvMode::Planar),paint(UvMode::Planar);
    clip::Paths64 used;
    // Surveyed parking wins over inferred forecourts, independent of OSM order.
    for(bool measured:{true,false})for(auto* way:lots) {
        const auto& w=*way;
        const bool parking=tagOr(w.tags,"amenity")=="parking";
        if(parking!=measured)continue;
        if(measured&&(tagOr(w.tags,"parking")=="underground"||tagOr(w.tags,"parking")=="multi-storey"||tagOr(w.tags,"access")=="private"))continue;
        if(!measured&&tagOr(w.tags,"landuse")!="retail"&&
            !(tagOr(w.tags,"landuse")=="commercial"&&tagOr(w.tags,"surface")=="asphalt"))continue;
        Ring ring=project(w.points);if(ring.size()<3)continue;
        const auto c=centroid(ring);const InteriorPlan* owner=nullptr;double best=120;
        for(auto& s:nearby){double d=dist(c,s.door);if(pointInPolygon(s.door,ring))d=0;
            if(d<best){best=d;owner=&s;}}
        if(!owner)continue;
        const auto& s=*owner;
        clip::Paths64 region{clip::kMetres.path(ring)};
        if(!measured) {
            // An inferred parking stays inside a surveyed retail parcel, in
            // front of its entrance. Bare concrete alone is never evidence.
            Ring apron{s.point(-22,-5),s.point(22,-5),s.point(22,-34),s.point(-22,-34)};
            region=clip::intersect(region,{clip::kMetres.path(apron)});
        }
        region=clip::subtract(clip::subtract(region,excluded),used);
        if(clip::area(region)<35)continue;
        used=clip::unite(used,region);drape.lay(region,.085,asphalt);
        double lo=1e9,hi=-1e9,front=1e9,back=-1e9;
        for(auto& path:region)for(auto q:path){auto p=s.local(clip::kMetres.back(q));lo=std::min(lo,p.x);hi=std::max(hi,p.x);front=std::min(front,p.y);back=std::max(back,p.y);}
        auto rect=[&](double u,double v,double w,double d){return clip::Paths64{clip::kMetres.path({s.point(u,v),s.point(u+w,v),s.point(u+w,v+d),s.point(u,v+d)})};};
        clip::Paths64 markings;
        auto line=[&](P2 a,P2 b){auto paths=clip::bufferLine({a,b},.055);markings.insert(markings.end(),paths.begin(),paths.end());};
        int bays=0;
        // Paired 5 m bays around a 6 m circulation aisle; entrance spine kept free.
        for(double v=front+.5;v+5<back&&bays<240;v+=16)
            for(double offset:{0.,11.})for(double u=lo+.5;u+2.6<hi&&bays<240;u+=2.6) {
                double row=v+offset;if(row+5>back)continue;
                if(u<1.8&&u+2.6>-1.8)continue;
                auto bay=rect(u,row,2.6,5);
                if(clip::area(clip::subtract(bay,region))>.001)continue;
                line(s.point(u,row),s.point(u,row+5));line(s.point(u+2.6,row),s.point(u+2.6,row+5));
                line(s.point(u,row+(offset==0?0:5)),s.point(u+2.6,row+(offset==0?0:5)));
                ++bays;
            }
        // Clearly marked pedestrian route between rows and the shop entrance.
        for(double v=front;v< std::min(back,-4.);v+=.9){auto paths=rect(-1.1,v,2.2,.42);markings.insert(markings.end(),paths.begin(),paths.end());}
        drape.lay(clip::intersect(region,clip::unite(markings)),.108,paint);
        nlohmann::json areas=nlohmann::json::array();for(auto& path:region){nlohmann::json r=nlohmann::json::array();for(auto q:path){auto p=clip::kMetres.back(q);r.push_back({p.x,p.y});}areas.push_back(r);}
        out.manifest.push_back({{"osmId",w.id},{"storeId",s.id},{"storeName",s.name},{"associationSource","inferred:nearest-store"},
            {"areaSource",measured?"measured":"inferred:retail-forecourt"},{"layoutSource","synthesized"},{"spaces",bays},{"rings",areas}});
    }
    if(!asphalt.empty())out.parts.push_back({"Store parking asphalt",std::move(asphalt),surfaceMaterial("Parking asphalt",{.10,.105,.11},.92,"asphalt")});
    if(!paint.empty()){Material m;m.name="Parking markings";m.color={.48,.47,.41,1};out.parts.push_back({"Parking bays and pedestrian route",std::move(paint),m});}
    return out;
}
} // namespace r1
