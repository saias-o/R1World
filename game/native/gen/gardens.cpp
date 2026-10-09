#include "gardens.hpp"
#include "buildings.hpp"
#include "predict.hpp"
#include "spatial.hpp"

namespace r1 {
namespace {
P2 plus(P2 a, P2 b) { return {a.x+b.x,a.y+b.y}; }
P2 times(P2 a, double k) { return {a.x*k,a.y*k}; }
double dot(P2 a, P2 b) { return a.x*b.x+a.y*b.y; }
P2 minus(P2 a, P2 b) { return {a.x-b.x,a.y-b.y}; }
Ring project(const OsmWay& w, const Anchor& a) {
    Ring r; for (auto p:w.points) { auto q=a.toEngine(p.x,p.y,0); r.push_back({q.x,q.z}); }
    if(r.size()>1 && r.front()==r.back())r.pop_back();
    return r;
}
bool privateHouse(const OsmWay& b, bool ruralKinds=false, bool smallTownFacade=false) {
    if(tagOr(b.tags,"wall")=="no")return false;
    const auto kind=tagOr(b.tags,"building");
    if(kind!="yes" && kind!="house" && kind!="detached" &&
       !(ruralKinds && (kind=="residential" || kind=="bungalow" || kind=="cabin")))return false;
    for(const char* k:{"amenity","shop","office","craft","industrial","aeroway","tourism","man_made"})
        if(has(b.tags,k)) {
            const std::string key=k;
            if(smallTownFacade && (key=="shop" || key=="craft" ||
                (key=="amenity" && (tagOr(b.tags,k)=="restaurant" || tagOr(b.tags,k)=="cafe"))))continue;
            return false;
        }
    return lengthTag(tag(b.tags,"building:levels"),1)<=2 && lengthTag(tag(b.tags,"height"),0)<=9;
}
bool privateGround(const Tags& t) {
    const auto use=tagOr(t,"landuse"), leisure=tagOr(t,"leisure"), natural=tagOr(t,"natural");
    if(!use.empty() && use!="residential" && use!="grass" && use!="village_green")return false;
    if(!leisure.empty() && leisure!="garden")return false;
    if(!natural.empty() && natural!="grassland")return false;
    return !has(t,"amenity") && !has(t,"water") && !has(t,"waterway");
}
const ResidentialStyle* styleAt(const std::string& country, P2 geo) {
    const ResidentialStyle* found=nullptr;
    for(const auto& s:palette().residential) {
        if(s.country!=country)continue;
        if(s.box && (geo.x<(*s.box)[0] || geo.x>(*s.box)[2] || geo.y<(*s.box)[1] || geo.y>(*s.box)[3]))continue;
        found=&s; // regional entries follow their national fallback
    }
    return found;
}
// The part of a tentative lot nearer its home than a neighbour: ownership
// from observations, never from the order a tile happened to be cooked in.
Ring nearer(Ring ring,P2 home,P2 other) {
    const P2 n=minus(other,home), middle=times(plus(home,other),.5);
    Ring out;
    for(size_t i=0;i<ring.size();++i) {
        P2 a=ring[i],b=ring[(i+1)%ring.size()];
        double da=dot(minus(a,middle),n),db=dot(minus(b,middle),n);
        if(da<=0)out.push_back(a);
        if((da<=0)!=(db<=0))out.push_back(plus(a,times(minus(b,a),da/(da-db))));
    }
    return out;
}
}

ResidentialPlan planResidential(const OsmData& osm,const Anchor& anchor) {
    ResidentialPlan out;
    const auto jurisdiction=jurisdictionAt(osm,anchor.lon,anchor.lat);
    // Closely packed merchant houses still form a village. An observed village
    // is evidence for their architecture, never permission to guess gardens in
    // a dense block or to apply the village prior inside a city.
    std::vector<P2> villages,cities,largeCities;
    for(const auto& n:osm.features) {
        const auto place=tagOr(n.tags,"place");
        if(place!="village" && place!="hamlet" && place!="city" && place!="town")continue;
        const auto p=anchor.toEngine(n.lon,n.lat,0);
        (place=="village" || place=="hamlet"?villages:cities).push_back({p.x,p.z});
        if(place=="city")largeCities.push_back({p.x,p.z});
    }
    struct Building { Ring ring; P2 centre; double area; Box box; };
    std::vector<Building> buildings;
    BoxIndex index(64);
    for(const auto& b:osm.buildings) {
        Ring r=project(b,anchor); const double area=std::abs(polygonArea(r));
        P2 c=r.empty()?P2{}:centroid(r); Box box=boxOf(r);
        index.add(buildings.size(),box);buildings.push_back({std::move(r),c,area,box});
    }
    struct Road { P2 a,b; double half; bool walking; };
    std::vector<Road> roads; BoxIndex roadIndex(64);
    for(const auto& w:osm.roads) {
        auto h=tagOr(w.tags,"highway");
        const bool walking=jurisdiction.code!="FR" && (h=="pedestrian" || h=="footway");
        const bool ruralPrimary=jurisdiction.code!="FR" && h=="primary" &&
            lengthTag(tag(w.tags,"lanes"),2)<=2 && roadWidth(w.tags)<=7.5;
        if(h!="residential" && h!="unclassified" && h!="tertiary" && h!="secondary" && h!="living_street" && !walking && !ruralPrimary)continue;
        if(taggedYes(w.tags,"bridge") || taggedYes(w.tags,"tunnel"))continue;
        Ring r=project(w,anchor);
        double half=roadWidth(w.tags)/2+.7;
        for(const auto& side:sidewalkSides(w.tags))
            half=std::max(half,roadWidth(w.tags)/2+lengthTag(tag(w.tags,("sidewalk:"+side.first+":width").c_str()),
                lengthTag(tag(w.tags,"sidewalk:width"),1.8))+.4);
        for(size_t i=1;i<r.size();++i) {
            roadIndex.add(roads.size(),boxOf({r[i-1],r[i]})); roads.push_back({r[i-1],r[i],half,walking});
        }
    }
    int dense=0,attached=0,useRejected=0;
    for(size_t i=0;i<osm.buildings.size();++i) {
        const auto& raw=osm.buildings[i]; const auto& b=buildings[i];
        const P3 geo=anchor.toGeodetic(b.centre.x,0,b.centre.y);
        const auto* style=styleAt(jurisdiction.code,{geo.x,geo.y});
        if(!style || !privateHouse(raw,jurisdiction.code!="FR",style->architectureOnly) ||
           b.area<45 || b.area>style->maxArea || b.ring.size()<3)continue;
        const auto& urbanCentres=style->architectureOnly?largeCities:cities;
        if(style->maxMass>14 && std::any_of(urbanCentres.begin(),urbanCentres.end(),[&](P2 p){return dist(p,b.centre)<1500;})) {
            ++dense;continue;
        }
        bool groundOkay=true;
        for(const auto& area:osm.landcover)
            if(!privateGround(area.tags) && pointInPolygon({geo.x,geo.y},area.points) &&
               !(style->architectureOnly && tagOr(area.tags,"landuse")=="commercial" && !has(area.tags,"amenity")))groundOkay=false;
        if(!groundOkay){++useRejected;continue;}
        double mass=0,area=0; bool joined=false;
        const auto neighbours=index.near(b.box.grown(70));
        for(size_t j:neighbours) {
            const auto& q=buildings[j];
            if(dist(b.centre,q.centre)>70)continue;
            // Cadastral extracts carry tiny sheds/porches as buildings too.
            // Counting each as a household makes a village look like a city.
            area+=q.area;
            const auto& tags=osm.buildings[j].tags;
            const double levels=lengthTag(tag(tags,"building:levels"),tagOr(tags,"building")=="apartments"?4:1);
            mass+=std::clamp(q.area*std::clamp(levels,1.,8.)/160.,.08,16.);
        }
        const bool denseBlock=area/(kPi*70*70)>style->maxCoverage || mass>style->maxMass;
        const bool village=style->villageRows &&
            std::any_of(villages.begin(),villages.end(),[&](P2 p){return dist(p,b.centre)<700;}) &&
            std::none_of(cities.begin(),cities.end(),[&](P2 p){return dist(p,b.centre)<1500;});
        if(denseBlock && !(village && area/(kPi*70*70)<=.4 && mass<=40)){++dense;continue;}
        const auto expanded=clip::bufferRing(b.ring,2.5);
        for(size_t j:neighbours)if(i!=j && buildings[j].area>=45 && privateHouse(osm.buildings[j]) &&
            buildings[j].box.overlaps(b.box.grown(2.5)))
            joined |= clip::area(clip::intersect(expanded,{clip::kMetres.path(buildings[j].ring)}))>.01;
        if(joined){++attached;if(!style->villageRows || (jurisdiction.code=="FR" && !village))continue;}
        const Road* road=nullptr; P2 frontage{}; double gap=45;
        for(size_t j:roadIndex.near(b.box.grown(45))) {
            const auto& r=roads[j]; const P2 d=minus(r.b,r.a); const double l2=dot(d,d);
            if(l2<1e-6)continue;
            const P2 p=plus(r.a,times(d,std::clamp(dot(minus(b.centre,r.a),d)/l2,0.,1.)));
            if(dist(p,b.centre)<gap){gap=dist(p,b.centre);road=&r;frontage=p;}
        }
        if(!road) {
            // The 45 m road rule proves a frontage for a garden, not the
            // architecture of a back-row village home. Keep its home gabarit
            // without inventing a driveway across neighbouring parcels.
            if(!village)continue;
            out.homes.push_back({raw.id,style,b.ring,b.centre,{},{},{},0,0,0,false});
            continue;
        }
        P2 along=times(minus(road->b,road->a),1/dist(road->a,road->b));
        P2 inward{-along.y,along.x};if(dot(minus(b.centre,frontage),inward)<0)inward=times(inward,-1);
        double width=0,front=1e9,back=-1e9;
        for(P2 p:b.ring) {
            const P2 d=minus(p,frontage);width=std::max(width,std::abs(dot(d,along)));
            front=std::min(front,dot(d,inward));back=std::max(back,dot(d,inward));
        }
        // A street wall is not a garden. At least 3 m behind the road's side,
        // enough for a front plot and an approach, with no guessed long estate.
        const bool gardenEligible=!style->architectureOnly && !road->walking && !denseBlock && mass<=14 && area/(kPi*70*70)<=.18 &&
            !joined && front>=road->half+style->setback && back<=60 && width<=18;
        out.homes.push_back({raw.id,style,b.ring,b.centre,frontage,along,inward,
                             std::clamp(width+style->lotPadding,6.,16.),road->half+style->frontOffset,back+5,gardenEligible});
    }
    // A small untyped outbuilding beside a proven rural home is an annex, not
    // an eight-storey tower. The home and private land-use checks also apply
    // to French cadastral extracts; a mapped dimension always wins.
    for(size_t i=0;i<osm.buildings.size();++i) {
        const auto& raw=osm.buildings[i];const auto& b=buildings[i];
        if(tagOr(raw.tags,"building")!="yes" || b.area<4 || b.area>=45 ||
           has(raw.tags,"height") || has(raw.tags,"building:levels") || !privateHouse(raw))continue;
        const auto at=anchor.toGeodetic(b.centre.x,0,b.centre.y);
        bool privateSite=true;
        for(const auto& area:osm.landcover)if(!privateGround(area.tags) && pointInPolygon({at.x,at.y},area.points))privateSite=false;
        if(!privateSite)continue;
        if(jurisdiction.code=="FR" && std::any_of(cities.begin(),cities.end(),
            [&](P2 p){return dist(p,b.centre)<1500;}))continue;
        const ResidentialHome* nearest=nullptr;double gap=25;
        for(const auto& h:out.homes)if(std::abs(polygonArea(h.ring))>=45 && dist(h.centre,b.centre)<gap) {
            nearest=&h;gap=dist(h.centre,b.centre);
        }
        if(!nearest)continue;
        // Copy before push_back can invalidate the matched home.
        const auto* style=nearest->style;
        out.homes.push_back({raw.id,style,b.ring,b.centre,{},{},{},0,0,0,false});
    }
    out.stats={{"revision",3},{"country",jurisdiction.code},{"countrySource",jurisdiction.basis},
        {"eligibleHomes",out.homes.size()},{"rejectedDense",dense},{"rejectedAttached",attached},
        {"rejectedLanduse",useRejected},{"inferred",true}};
    out.stats["settlementsQueried"]=osm.settlementsQueried;
    out.stats["homes"]=nlohmann::json::array();
    for(const auto& h:out.homes)out.stats["homes"].push_back({{"building",h.id},{"rule",h.style->key},
        {"source","inferred detached morphology"},{"gardenEligible",h.gardenEligible},{"centre",{h.centre.x,h.centre.y}},
        {"frontage",{h.frontage.x,h.frontage.y}},{"frontM",h.front},{"backM",h.back},{"halfWidthM",h.halfWidth}});
    return out;
}

nlohmann::json classifyBusShelters(std::vector<OsmWay>& buildings,const OsmData& osm,
                                  const Anchor& anchor,bool infer) {
    nlohmann::json report=nlohmann::json::array();
    for(auto& b:buildings) {
        if(infer && tagOr(b.tags,"building")=="service" && tagOr(b.tags,"power")=="substation" &&
           std::abs(polygonArea(project(b,anchor)))<40)b.tags["r1:utility-hut"]="yes";
        const bool measured=tagOr(b.tags,"amenity")=="shelter" &&
            (tagOr(b.tags,"shelter_type")=="public_transport" || tagOr(b.tags,"bus")=="yes");
        const bool unknown=tagOr(b.tags,"building")=="yes" || tagOr(b.tags,"building")=="roof";
        if(!measured && (!infer || !unknown || has(b.tags,"power") || has(b.tags,"service") ||
           has(b.tags,"shop") || has(b.tags,"office") || has(b.tags,"amenity") || has(b.tags,"building:levels") || has(b.tags,"height")))continue;
        Ring r=project(b,anchor);const double area=std::abs(polygonArea(r));
        if(r.size()<3 || area<2 || area>18)continue;
        const P2 centre=centroid(r);const OsmNode* stop=nullptr;double best=22;
        for(const auto& n:osm.features) {
            if(tagOr(n.tags,"highway")!="bus_stop" &&
               !(tagOr(n.tags,"public_transport")=="platform" && tagOr(n.tags,"bus")=="yes"))continue;
            if(tagOr(n.tags,"shelter")=="no")continue;
            const auto p=anchor.toEngine(n.lon,n.lat,0);const double d=dist(centre,{p.x,p.z});
            if(d<best){best=d;stop=&n;}
        }
        if(!stop && !measured)continue;
        P2 front=centre;double near=1e9,nearEdge=1e9;
        for(const auto& road:osm.roads) {
            if(!isMotorway(tagOr(road.tags,"highway")))continue;
            Ring line=project(road,anchor);if(line.size()<2)continue;
            const double d=clip::distance(line,centre);
            for(P2 p:r)nearEdge=std::min(nearEdge,clip::distance(line,p));
            if(d<near){near=d;front=clip::interpolate(line,clip::project(line,centre));}
        }
        if(!measured && nearEdge>7.05)continue; // not a garden shed set back from the road
        b.tags["r1:bus-shelter"]=measured?"measured":"inferred";
        b.tags["r1:shelter-front-x"]=std::to_string(front.x-centre.x);
        b.tags["r1:shelter-front-z"]=std::to_string(front.y-centre.y);
        report.push_back({{"building",b.id},{"busStop",stop?stop->id:0},{"source",measured?"OSM shelter":"small footprint beside a rural bus stop"},
            {"confidence",measured?1.:.7},{"rule",measured?"surveyed.bus_shelter":"fr.bus_stop.small_footprint"}});
    }
    return report;
}

GardenOutput buildGardens(const OsmData& osm,const ResidentialPlan& plan,const Tile& tile,
                         const Anchor& anchor,const ElevationGrid& elevations,
                         const clip::Paths64& paved,const clip::Paths64& water,size_t budget) {
    GardenOutput out; out.stats=plan.stats;
    out.stats["observationsQueried"]=osm.queryVersion>=12;
    out.stats["lots"]=nlohmann::json::array();
    out.stats["vertexBudget"]=budget;
    // This pass supplies residential frontage. In a dense settlement with no
    // eligible homes, leave even its existing urban boundary rendering alone:
    // adding garden detail must not consume a city tile's building budget.
    const bool denseSettlement=plan.homes.empty() && plan.stats.value("rejectedDense",0)>0;
    out.stats["excludedDenseSettlement"]=denseSettlement;
    if(denseSettlement || (plan.homes.empty() && std::none_of(osm.barriers.begin(),osm.barriers.end(),[](const auto& b){
        return tagOr(b.tags,"barrier")=="fence" || tagOr(b.tags,"barrier")=="wall";}))) {
        out.stats["inferredGardens"]=0;out.stats["measuredSegments"]=0;out.stats["silencedBySurvey"]=0;
        out.stats["droppedForBudget"]=0;out.stats["vertices"]=0;
        return out;
    }
    const Drape drape(elevations,anchor);
    const clip::Paths64 extent{drape.outline()};
    const Ring tileRing=clip::kMetres.ring(extent.front());
    const Box tileBox=boxOf(tileRing);
    auto clipSegment=[&](P2& a,P2& b) {
        double enter=0,leave=1;const double sign=polygonArea(tileRing)>0?1:-1;
        for(size_t i=0;i<tileRing.size();++i) {
            const P2 p=tileRing[i],q=tileRing[(i+1)%tileRing.size()];
            const double da=sign*cross2(p,q,a),db=sign*cross2(p,q,b);
            if(da<0 && db<0)return false;
            if(da<0)enter=std::max(enter,da/(da-db));
            if(db<0)leave=std::min(leave,da/(da-db));
        }
        if(enter>=leave)return false;
        const P2 origin=a,delta=minus(b,a);a=plus(origin,times(delta,enter));b=plus(origin,times(delta,leave));
        return true;
    };
    clip::Paths64 rawObstacles=paved;
    rawObstacles.insert(rawObstacles.end(),water.begin(),water.end());
    for(const auto& b:osm.buildings) {
        const auto buffered=clip::bufferRing(project(b,anchor),.4);
        rawObstacles.insert(rawObstacles.end(),buffered.begin(),buffered.end());
    }
    for(const auto& a:osm.landcover)if(!privateGround(a.tags))
        rawObstacles.push_back(clip::kMetres.path(project(a,anchor)));
    const clip::Paths64 obstacles=clip::unite(rawObstacles);
    Mesh lawn(UvMode::Planar),drive(UvMode::Planar),wire,posts,gates,stone(UvMode::Slope),wood,woodPosts,woodGates;
    const ResidentialStyle* local=plan.homes.empty()?nullptr:plan.homes.front().style;
    const bool mineral=local && local->plotGround=="mineral";
    auto count=[&] {return lawn.vertexCount()+drive.vertexCount()+wire.vertexCount()+posts.vertexCount()+gates.vertexCount()+stone.vertexCount()+wood.vertexCount()+woodPosts.vertexCount()+woodGates.vertexCount();};
    auto at=[&](P2 p,double lift) {return P3{p.x,drape.heightAt(p)+lift,p.y};};
    // Vertical planes made of thin strips: transparency without a texture,
    // blending pass or one draw call per fence. Posts remain real volumes.
    auto fence=[&](P2 a,P2 b,double height,bool wall,bool gate,bool timber=false) {
        if(!clipSegment(a,b))return;
        const double length=dist(a,b);if(length<.08)return;
        const P2 axis=times(minus(b,a),1/length);
        const int chunks=std::max(1,int(std::ceil(length/2.5)));
        for(int i=0;i<chunks;++i) {
            P2 p=plus(a,times(axis,length*i/chunks)),q=plus(a,times(axis,length*(i+1)/chunks));
            if(!clip::contains(extent,times(plus(p,q),.5)))continue;
            const double span=dist(p,q),ha=drape.heightAt(p),hb=drape.heightAt(q);
            if(wall) {
                stone.addQuad({p.x,ha,p.y},{q.x,hb,q.y},{q.x,hb+height,q.y},{p.x,ha+height,p.y});
                continue;
            }
            // End posts stand inside the fence segment, so their thickness
            // does not steal clearance from a surveyed gate opening.
            Mesh& supports=timber?woodPosts:posts;
            supports.addBox(at(plus(p,times(axis,i==0?.045:0)),height*.5),{.08,height,.08});
            if(i+1==chunks)supports.addBox(at(plus(q,times(axis,-.045)),height*.5),{.08,height,.08});
            Mesh& strips=timber?(gate?woodGates:wood):(gate?gates:wire);
            if(timber) {
                // One opaque timber panel per span; photographed planks supply
                // the detail without hundreds of bars or transparent layers.
                strips.addQuad({p.x,ha+.08,p.y},{q.x,hb+.08,q.y},{q.x,hb+height,q.y},{p.x,ha+height,p.y});
                continue;
            }
            for(double y:{.12,height*.5,height-.03}) {
                strips.addQuad({p.x,ha+y,p.y},{q.x,hb+y,q.y},{q.x,hb+y+.014,q.y},{p.x,ha+y+.014,p.y});
            }
            const int bars=std::max(1,int(std::ceil(span/(gate?.16:.60))));
            for(int j=0;j<bars;++j) {
                const double s=span*(j+.5)/bars,w=gate?.075:.022;
                const P2 left=plus(p,times(axis,s-w/2)),right=plus(p,times(axis,s+w/2));
                const double h=ha+(hb-ha)*s/span;
                strips.addQuad({left.x,h+.1,left.y},{right.x,h+.1,right.y},
                               {right.x,h+height,right.y},{left.x,h+height,left.y});
            }
        }
    };
    int measured=0,silenced=0,dropped=0,inferred=0;
    out.stats["refusedLots"]=nlohmann::json::array();
    auto refuse=[&](int64_t id,const char* reason){out.stats["refusedLots"].push_back({{"building",id},{"reason",reason}});};
    // Surveyed boundaries first, regardless of country. Gate nodes cut a real
    // opening, even when the point is inside a segment rather than its end.
    for(const auto& w:osm.barriers) {
        const auto kind=tagOr(w.tags,"barrier");
        if(kind!="fence" && kind!="wall")continue;
        Ring line=project(w,anchor);if(w.closed() && !line.empty())line.push_back(line.front());
        const double h=std::clamp(lengthTag(tag(w.tags,"height"),kind=="wall"?1.1:1.3),.3,4.);
        for(size_t i=1;i<line.size();++i) {
            const P2 a=line[i-1],b=line[i]; const double len=dist(a,b);if(len<.01)continue;
            const P2 axis=times(minus(b,a),1/len);
            std::vector<std::pair<double,double>> gaps;
            for(const auto& n:osm.features) {
                const auto barrier=tagOr(n.tags,"barrier");
                if(barrier!="gate" && barrier!="entrance" && barrier!="lift_gate")continue;
                const auto x=anchor.toEngine(n.lon,n.lat,0); const P2 p{x.x,x.z};
                if(clip::distance({a,b},p)>.35)continue;
                const double t=dot(minus(p,a),axis),width=std::clamp(lengthTag(tag(n.tags,"width"),3.2),.8,6.);
                gaps.push_back({std::max(0.,t-width/2),std::min(len,t+width/2)});
            }
            std::sort(gaps.begin(),gaps.end());double start=0;
            for(auto [lo,hi]:gaps) {if(lo>start)fence(plus(a,times(axis,start)),plus(a,times(axis,lo)),h,kind=="wall",false);start=std::max(start,hi);}
            if(start<len)fence(plus(a,times(axis,start)),b,h,kind=="wall",false);
            ++measured;
        }
    }
    std::vector<const ResidentialHome*> frontages;
    for(const auto& home:plan.homes)if(home.gardenEligible)frontages.push_back(&home);
    // Give the street's broad, visible plots priority over cadastral ID order.
    // The score is independent of the player and of tile cooking order.
    std::stable_sort(frontages.begin(),frontages.end(),[](const auto* a,const auto* b){
        if(a->halfWidth!=b->halfWidth)return a->halfWidth>b->halfWidth;
        return a->id<b->id;
    });
    for(const auto* frontage:frontages) {
        const auto& home=*frontage;
        if(!home.gardenEligible)continue;
        auto point=[&](double u,double v) {return plus(home.frontage,plus(times(home.along,u),times(home.inward,v)));};
        Ring lot{point(-home.halfWidth,home.front),point(home.halfWidth,home.front),
                 point(home.halfWidth,home.back),point(-home.halfWidth,home.back)};
        if(!boxOf(lot).overlaps(tileBox))continue;
        // Any measured boundary around this house silences its guessed plot.
        bool mapped=false;
        for(const auto& b:osm.barriers)
            if(clip::distance(project(b,anchor),home.centre)<home.halfWidth+12){mapped=true;break;}
        if(mapped){++silenced;continue;}
        if(inferred>=24 || count()+1600>budget){++dropped;refuse(home.id,"vertex-budget");continue;}
        for(const auto& b:osm.buildings) {
            if(b.id==home.id)continue;
            const Ring r=project(b,anchor);if(r.size()<3 || std::abs(polygonArea(r))<45 || !privateHouse(b))continue;
            const P2 c=centroid(r);
            if(dist(c,home.centre)<100)lot=nearer(std::move(lot),home.centre,c);
        }
        if(lot.size()<3)continue;
        const clip::Paths64 plot=clip::intersect({clip::kMetres.path(lot)},extent);
        const clip::Paths64 approach=clip::bufferLine({point(0,home.front-.2),home.centre},home.style->accessWidth*.5);
        const clip::Paths64 visible=clip::subtract(plot,obstacles);
        const clip::Paths64 access=clip::intersect(visible,approach);
        // A clipped plot must still meet its own access; otherwise skip the
        // whole enclosure, rather than trapping a house behind a neighbour.
        if(clip::area(clip::intersect(clip::subtract({clip::kMetres.path(lot)},obstacles),approach))<3){refuse(home.id,"access-obstructed");continue;}
        if(clip::area(visible)<.1){refuse(home.id,"no-private-ground");continue;}
        Mesh newLawn(UvMode::Planar),newDrive(UvMode::Planar);
        drape.lay(clip::subtract(visible,approach),.028,newLawn);drape.lay(access,.04,newDrive);
        if(count()+newLawn.vertexCount()+newDrive.vertexCount()+1400>budget){++dropped;refuse(home.id,"vertex-budget");continue;}
        // Work in temporary meshes for the entire lot: a budget refusal keeps
        // all parts of a house together, never just half of its fence.
        Mesh oldWire=wire,oldPosts=posts,oldGates=gates,oldStone=stone,oldWood=wood,oldWoodPosts=woodPosts,oldWoodGates=woodGates;
        auto clearFence=[&](P2 a,P2 b) {
            const double len=dist(a,b);const int steps=std::max(1,int(std::ceil(len/.8)));
            std::optional<P2> start;
            for(int i=0;i<=steps;++i) {
                const P2 p=plus(a,times(minus(b,a),double(i)/steps));
                const bool okay=!clip::contains(obstacles,p) && !clip::contains(approach,p) && clip::contains(extent,p);
                if(okay && !start)start=p;
                if(start && (!okay || i==steps)) {P2 end=okay?p:plus(a,times(minus(b,a),double(std::max(0,i-1))/steps));
                    fence(*start,end,home.style->fenceHeight,home.style->enclosure=="wall",false,home.style->enclosure=="timber");start.reset();}
            }
        };
        for(size_t i=0;i<lot.size();++i) {
            const P2 a=lot[i],b=lot[(i+1)%lot.size()];
            // Only frontage and short returns, not a guessed cadastral rear.
            const double va=dot(minus(a,home.frontage),home.inward),vb=dot(minus(b,home.frontage),home.inward);
            if(home.style->enclosure=="open")continue;
            const double limit=home.front+home.style->returns;
            if(std::min(va,vb)>limit)continue;
            P2 end=b;if(vb>limit && vb>va)end=plus(a,times(minus(b,a),(limit-va)/(vb-va)));
            P2 begin=a;if(va>limit && va>vb)begin=plus(b,times(minus(a,b),(limit-vb)/(va-vb)));
            clearFence(begin,end);
        }
        // Two gate leaves folded into the plot: a visible entrance that stays
        // open for the player, not a decorative wall across the driveway.
        const double opening=home.style->accessWidth*.5+.15;
        const P2 left=point(-opening,home.front+.05),right=point(opening,home.front+.05);
        if(home.style->enclosure!="open" && clip::contains(plot,left) && clip::contains(plot,right) &&
           !clip::contains(obstacles,left) && !clip::contains(obstacles,right)) {
            fence(left,plus(left,plus(times(home.along,.48),times(home.inward,1.5))),home.style->fenceHeight,false,true,home.style->enclosure=="timber");
            fence(right,plus(right,plus(times(home.along,-.48),times(home.inward,1.5))),home.style->fenceHeight,false,true,home.style->enclosure=="timber");
        }
        if(count()+newLawn.vertexCount()+newDrive.vertexCount()>budget) {
            wire=std::move(oldWire);posts=std::move(oldPosts);gates=std::move(oldGates);stone=std::move(oldStone);
            wood=std::move(oldWood);woodPosts=std::move(oldWoodPosts);woodGates=std::move(oldWoodGates);++dropped;refuse(home.id,"vertex-budget");continue;
        }
        // Re-lay only accepted regions into the material buckets.
        drape.lay(clip::subtract(visible,approach),.028,lawn);drape.lay(access,.04,drive);
        out.drives=clip::unite(out.drives,access);
        ++inferred;
        out.stats["lots"].push_back({{"building",home.id},{"rule",home.style->key},{"confidence",.55},
            {"source","inferred front garden, not a cadastral parcel"},{"lawnAreaM2",pyround(clip::area(visible)-clip::area(access),2)},
            {"accessWidthM",home.style->accessWidth},{"enclosure",home.style->enclosure},{"ground",home.style->plotGround}});
    }
    auto add=[&](const char* name,Mesh& mesh,Material material) {if(!mesh.empty())out.parts.push_back({name,std::move(mesh),std::move(material)});};
    add(mineral?"Residential mineral courts":"Residential lawns",lawn,mineral?
        surfaceMaterial("Mineral courtyard",local->country=="MA"?std::array<double,3>{.26,.20,.14}:std::array<double,3>{.20,.205,.20},.92,"terrace"):
        surfaceMaterial("Garden lawn",{.16,.205,.09},.92,"grass"));
    add("Residential driveways",drive,surfaceMaterial("Gravel driveway",{.22,.205,.175},.9,"terrace"));
    Material metal;metal.name="Green coated garden wire";metal.color={.035,.075,.04,1};metal.roughness=.72;metal.doubleSided=true;
    const Material timber=surfaceMaterial("Weathered timber fence",{.15,.105,.065},.92,"deck",true);
    add("Garden wire fences",wire,metal);add("Garden fence posts",posts,metal);
    add("Garden timber fences",wood,timber);add("Garden timber posts",woodPosts,timber);add("Timber residential gates",woodGates,timber);
    Material gate=metal;gate.name="Dark residential gates";gate.color={.055,.065,.055,1};
    add("Residential gates",gates,gate);
    add("Surveyed boundary walls",stone,surfaceMaterial("Boundary stone",{.22,.215,.20},.9,"ashlar_rough",true));
    size_t vertices=0;for(const auto& p:out.parts)vertices+=p.mesh.vertexCount();
    out.stats["inferredGardens"]=inferred;out.stats["measuredSegments"]=measured;
    out.stats["silencedBySurvey"]=silenced;out.stats["droppedForBudget"]=dropped;out.stats["vertices"]=vertices;
    out.stats["tile"]=tile.key();
    return out;
}
} // namespace r1
