#include "interiors.hpp"
#include "palette.hpp"
#include "clip.hpp"
#include "spatial.hpp"
#include <algorithm>

namespace r1 {
bool retailInterior(const std::string& r) { return r=="market"||r=="mall"||r=="fashion"||r=="bakery"; }
std::string interiorRecipe(const Tags& t) {
    const auto b=tagOr(t,"building"),a=tagOr(t,"amenity"),s=tagOr(t,"shop");
    if(b=="roof"||b=="no"||a=="fuel")return "";
    if(a=="police"||tagOr(t,"police")=="station"||tagOr(t,"military")=="gendarmerie")return "police";
    if(a=="school"||a=="kindergarten"||a=="college"||a=="university"||b=="school"||b=="kindergarten"||b=="college"||b=="university")return "school";
    if(s=="car_repair"||s=="car"||s=="tyres"||tagOr(t,"craft")=="car_repair"||b=="garage"||b=="garages"||b=="hangar"||a=="fire_station")return "garage";
    if(a=="hospital"||a=="clinic"||a=="doctors"||b=="hospital")return "clinic";
    if(a=="prison"||b=="prison")return "prison";
    if(b=="mosque"||tagOr(t,"religion")=="muslim")return "mosque";
    if(a=="place_of_worship"||b=="church"||b=="cathedral"||b=="synagogue"||b=="temple"||b=="chapel")return "worship";
    if(retailUse(t))return retailRecipe(t);
    if(a=="restaurant"||a=="cafe"||a=="bar"||a=="pub")return "restaurant";
    if(has(t,"office")||b=="office"||b=="commercial"||a=="townhall"||a=="courthouse"||a=="bank"||b=="civic"||b=="public"||tagOr(t,"aeroway")=="terminal")return "office";
    if(b=="warehouse"||b=="industrial"||b=="shed"||b=="barn"||b=="farm_auxiliary"||b=="service"||b=="container"||b=="greenhouse")return "warehouse";
    return "home";
}
std::string interiorName(const Tags& t) {
    auto n=tagOr(t,"name",tagOr(t,"brand"));if(!n.empty())return n;
    auto r=interiorRecipe(t);if(retailInterior(r))return retailName(t);
    return r=="home"?"Logement":r=="police"?"Commissariat / Gendarmerie":r=="school"?"Ecole":
        r=="office"?"Bureaux":r=="garage"?"Garage":r=="clinic"?"Centre de soins":r=="prison"?"Prison":
        r=="worship"||r=="mosque"?"Lieu de culte":r=="restaurant"?"Restaurant":"Entrepot";
}
namespace {
bool observedUse(const Tags& t) {
    return has(t,"office")||has(t,"craft")||has(t,"shop")||tagOr(t,"military")=="gendarmerie"||
        (has(t,"amenity")&&interiorRecipe(t)!="home"&&!interiorRecipe(t).empty());
}
bool residential(const Tags& t) {
    const auto b=tagOr(t,"building");return b=="house"||b=="detached"||b=="terrace"||b=="residential"||b=="apartments"||b=="semidetached_house";
}
}
std::vector<OsmWay> interiorBuildings(const std::vector<OsmWay>& buildings,const OsmData& osm,
                                    const std::function<P3(double,double)>& ground) {
    auto out=buildings;
    // Every scan below visits only what can match, in the order of the full
    // scan (gen/spatial.hpp): the first tenant, the order of the fronts and
    // the nearest road point are those of the scan over everything.
    std::vector<P2> entrancePoints;
    BoxIndex entranceIndex(32.);
    for(const auto& n:osm.features) {
        auto kind=tagOr(n.tags,"entrance");
        if(kind.empty()||kind=="no"||kind=="exit"||kind=="emergency"||kind=="service")continue;
        auto at=ground(n.lon,n.lat);entranceIndex.add(entrancePoints.size(),{at.x,at.z,at.x,at.z});entrancePoints.push_back({at.x,at.z});
    }
    std::vector<const OsmNode*> tenants;
    BoxIndex tenantIndex(.002);
    for(const auto& n:osm.features)if(observedUse(n.tags)){tenantIndex.add(tenants.size(),{n.lon,n.lat,n.lon,n.lat});tenants.push_back(&n);}
    std::vector<std::pair<const OsmWay*,Box>> campuses;
    for(const auto& area:osm.landcover)if(observedUse(area.tags))campuses.push_back({&area,boxOf(area.points)});
    std::vector<P2> roadPoints;
    for(const auto& road:osm.roads)roadPoints.insert(roadPoints.end(),road.points.begin(),road.points.end());
    const NearestPoint roadIndex(std::move(roadPoints),.0005);
    // A point a hair outside a ring's box is outside the ring, whatever the rounding.
    constexpr double kOutside=1e-9;
    for(auto& b:out) {
        // Explicit building use wins over a campus or a nearby tenant.
        b.tags["r1:useSource"]=observedUse(b.tags)||tagOr(b.tags,"building")!="yes"?"measured:building-tags":"inferred:residential-default";
        if(!observedUse(b.tags)&&!residential(b.tags)&&!interiorRecipe(b.tags).empty()) {
            bool found=false;
            const Box box=boxOf(b.points).grown(kOutside);
            for(size_t i:tenantIndex.near(box)) {
                const auto& n=*tenants[i];
                if(!box.contains({n.lon,n.lat})||!pointInPolygon({n.lon,n.lat},b.points))continue;
                for(auto key:{"amenity","office","craft","military","shop","name","brand"})
                    if(has(n.tags,key)&&!has(b.tags,key))b.tags[key]=tagOr(n.tags,key);
                b.tags["r1:useSource"]="measured:tenant-node";found=true;break;
            }
            // Schools and gendarmeries are often mapped as a campus polygon.
            const P2 c=centroid(b.points);
            if(!found)for(const auto& [area,areaBox]:campuses) {
                if(!areaBox.grown(kOutside).contains(c)||!pointInPolygon(c,area->points))continue;
                for(auto key:{"amenity","office","craft","military"})if(has(area->tags,key)&&!has(b.tags,key))b.tags[key]=tagOr(area->tags,key);
                b.tags["r1:useSource"]="inferred:observed-campus";break;
            }
        }
        if(retailUse(b.tags)||interiorRecipe(b.tags).empty())continue;
        std::string fronts;
        Ring outline;for(auto q:b.points){auto at=ground(q.x,q.y);outline.push_back({at.x,at.z});}
        // An entrance a metre past the outline's box is a metre from every edge.
        for(size_t i:entranceIndex.near(boxOf(outline).grown(1.01))) {
            const P2 at=entrancePoints[i];
            double near=1e30;
            for(size_t e=1;e<outline.size();++e)near=std::min(near,clip::distance({outline[e-1],outline[e]},at));
            if(near<1.)fronts+=(fronts.empty()?"":";")+std::to_string(at.x)+","+std::to_string(at.y);
        }
        b.tags["r1:entranceSource"]=fronts.empty()?"inferred:nearest-road":"measured";
        if(fronts.empty()) {
            const auto c=centroid(b.points);auto at=ground(c.x,c.y);P2 target{at.x,at.z};double best=1e30;
            const double scale=std::cos(radians(c.y));
            const size_t nearest=roadIndex.nearest(c,std::min(scale,1.),[&](P2 q){return std::hypot(wrap(q.x-c.x)*scale,q.y-c.y);},best);
            if(nearest!=NearestPoint::npos){const auto q=roadIndex.points()[nearest];auto p=ground(q.x,q.y);target={p.x,p.z};}
            fronts=std::to_string(target.x)+","+std::to_string(target.y);
        }
        b.tags["r1:front"]=fronts;
    }
    return out;
}

InteriorLayout layoutBuildingInterior(const InteriorPlan& p) {
    InteriorLayout out;
    double lo=1e9,hi=-1e9,back=0;for(auto q:p.ring){q=p.local(q);lo=std::min(lo,q.x);hi=std::max(hi,q.x);back=std::max(back,q.y);}
    const auto inner=clip::bufferRing(p.ring,-.25);
    auto fits=[&](double u,double v,double w,double d) {
        Ring r{p.point(u-w/2,v-d/2),p.point(u+w/2,v-d/2),p.point(u+w/2,v+d/2),p.point(u-w/2,v+d/2)};
        return clip::area(clip::subtract({clip::kMetres.path(r)},inner))<.001;
    };
    uint32_t regionalSalt=0x524F4F4D;for(unsigned char c:p.region)regionalSalt=(regionalSalt^c)*16777619u;
    auto random=seeded(p.id,regionalSalt);
    auto add=[&](const std::string& kind,double u,double v,double w,double d,double h) {
        if(out.fixtures.size()>=128||!fits(u,v,w,d))return false;
        for(const auto& f:out.fixtures) {
            // Walls meet at corners; rejecting that contact leaves rooms open.
            if(kind=="partition"&&f.kind=="partition")continue;
            if(std::abs(u-f.at.x)<(w+f.size.x)/2+.06&&std::abs(v-f.at.y)<(d+f.size.y)/2+.06)return false;
        }
        out.fixtures.push_back({{u,v},{w,d},std::min(h,p.ceiling-p.floor-.15),kind,std::min(3,int(random.random()*4))});return true;
    };
    // A continuous 1.8 m hallway, with side rooms and 1.2 m open doorways.
    // Fit complete rooms before furnishing: concave wings cannot have floating walls.
    const bool oneSide=std::min(-.98-lo-.4,hi-.4-.98)<1.5;
    const double depth=p.recipe=="home"?std::clamp((back-.7)/(oneSide?4:2),2.75,5.8):p.recipe=="garage"?7.:6.;
    const double scale=std::max(1.,std::sqrt((hi-lo)*back/24000.));
    for(double v=.5;v+2.7<back;v+=depth*scale)for(int side:{-1,1}) {
        const double a=side<0?lo+.4:.98,b=side<0?-.98:hi-.4;
        // Limit room widths; very large footprints keep bounded work and instances.
        const double w=std::min(p.recipe=="home"?4.8:9.,b-a),d=std::min(depth-.2,back-v-.45),u=side<0?b-w/2:a+w/2;
        if(w<1.5||d<2.5||!fits(u,v+d/2,w,d))continue;
        std::string use=p.recipe;
        const int row=int((v-.5)/(depth*scale)+.1);
        if(p.recipe=="home")use=row==0?(side<0?"living":"kitchen"):(side<0?"bedroom":"bathroom");
        if(p.recipe=="home"&&oneSide)use=row%4==0?"living":row%4==1?"kitchen":row%4==2?"bedroom":"bathroom";
        if(p.recipe=="police")use=row==0?(side<0?"reception":"waiting"):(side<0?"office":"interview");
        if(p.recipe=="school")use=row==0&&side<0?"library":"classroom";
        if(p.recipe=="office")use=row==0&&side<0?"reception":row%3==1&&side>0?"meeting":"office";
        out.rooms.push_back({use,{p.point(u-w/2,v),p.point(u+w/2,v),p.point(u+w/2,v+d),p.point(u-w/2,v+d)}});
        // Screens by the hallway, split around each room's doorway. Bedrooms
        // and wet rooms get opaque full-height walls; reception stays open.
        const bool openDomestic=p.recipe=="home"&&(use=="living"||use=="kitchen");
        if(use!="garage"&&use!="worship"&&use!="mosque"&&use!="warehouse"&&use!="waiting"&&use!="reception"&&!openDomestic) {
            const double x=side<0?b:a;
            add("partition",x,v+.3,.10,.55,p.ceiling-p.floor-.12);
            if(d>2.)add("partition",x,v+1.8+(d-1.8)/2,.10,d-1.8,p.ceiling-p.floor-.12);
            if(v+d<back-.6)add("partition",u,v+d,w,.10,p.ceiling-p.floor-.12);
        }
        if(openDomestic&&v+d<back-.6)add("partition",u,v+d,w,.10,p.ceiling-p.floor-.12);
        if(p.recipe=="home"&&w<b-a-.15)
            add("partition",side<0?u-w/2:u+w/2,v+d/2,.10,d,p.ceiling-p.floor-.12);
        const double left=u-w/2+.25,right=u+w/2-.25,front=v+.35,rear=v+d-.35;
        auto place=[&](const char* kind,double x,double y,double fw,double fd,double h){return add(kind,x,y,fw,fd,h);};
        if(use=="living"||use=="waiting") {
            place("sofa",u,rear-.5,std::min(2.3,w-.5),.9,.85);
            place("coffee_table",u,v+d/2,1.05,.6,.42);
            if(use=="living") {
                place("tv",u,front+.2,1.1,.38,1.15);
                place("lamp",left+.24,rear-.45,.45,.45,1.65);
                place("plant",right-.25,front+.35,.48,.48,.95);
                place("bookcase",left+.3,v+d*.5,.55,.38,1.65);
            }
        } else if(use=="kitchen") {
            const double counter=std::min(2.4,w-1.15);
            if(w>=2.2) {
                place("kitchen",u-.42,rear-.35,counter,.65,2.0);
                place("fridge",u-.42+counter/2+.45,rear-.38,.65,.72,1.95);
            } else place("kitchen",u,rear-.35,std::min(2.4,w-.5),.65,2.0);
            place("dining",u,front+1.0,std::min(1.8,w-.5),1.5,.78);
        } else if(use=="bedroom"||use=="clinic"||use=="prison") {
            const double bedWidth=std::min(1.5,w-.5);
            place("bed",u,v+d/2,bedWidth,2.05,1.1);
            if(use=="bedroom")for(double side:{-1.,1.})place("bedside",u+side*(bedWidth/2+.32),v+d/2+.55,.44,.40,.86);
            place("wardrobe",u,rear-.3,std::min(1.3,w-.5),.5,1.95);
        } else if(use=="bathroom") {
            place("shower",left+.46,rear-.46,.9,.9,2.05);
            place("toilet",right-.25,front+.4,.5,.75,.8);
            place("basin",right-.35,rear-.3,.65,.55,1.25);
        } else if(use=="classroom") {
            place("blackboard",u,rear-.12,std::min(2.8,w-.5),.18,2.0);
            for(double y=front+.8;y<rear-1.;y+=1.6)for(double x=left+.65;x<right-.45;x+=1.65)place("school_desk",x,y,1.15,1.2,.76);
        } else if(use=="library"||use=="warehouse") {
            for(double x=left+.4;x<right-.3;x+=2.)place(use=="library"?"bookcase":"storage",x,v+d/2,.65,std::min(2.,d-1.),1.9);
        } else if(use=="garage") {
            // Real fleet assets, instantiated by the runtime, never a box car.
            place("vehicle",u,v+d/2,2.15,4.5,1.5);
            place("workbench",u,rear-.35,std::min(2.3,w-.5),.6,.95);
            place("tool_cabinet",left+.35,front+.4,.6,.65,1.2);
        } else if(use=="worship") {
            for(double y=front+.8;y<rear;y+=1.6)place("pew",u,y,std::min(3.,w-.5),.6,.9);
        } else if(use=="mosque") {
            for(double y=front+.65;y<rear-.5;y+=1.5)for(double x=left+.4;x<right-.35;x+=1.)place("prayer_mat",x,y,.75,1.2,.025);
        } else if(use=="restaurant") {
            for(double y=front+.8;y<rear-.5;y+=2.1)place("dining",u,y,std::min(1.8,w-.5),1.5,.78);
        } else {
            if(use=="reception")place("reception",u,v+d/2,std::min(2.3,w-.5),.75,1.1);
            else if(use=="meeting"||use=="interview")place("meeting",u,v+d/2,std::min(2.6,w-.5),2.1,.78);
            else for(double y=front+.75;y<rear-.4;y+=2.1)for(double x=left+.8;x<right-.5;x+=2.1)place("desk",x,y,1.45,1.55,1.25);
            place("filing",u,rear-.3,std::min(1.4,w-.5),.5,1.8);
        }
    }
    if(p.recipe=="home") {
        // Rugs can sit beneath furniture. They remain decorative surfaces,
        // with no collider and no obstruction of the continuous hallway.
        auto fixtures=out.fixtures;
        for(const auto& f:fixtures)if(f.kind=="coffee_table"&&out.fixtures.size()<128) {
            const double w=std::min(1.8,2*(std::abs(f.at.x)-.98));
            if(w>f.size.x&&fits(f.at.x,f.at.y,w,1.35))out.fixtures.push_back({f.at,{w,1.35},.034,"rug",f.variant});
        }
    }
    // Compact sheds and narrow/irregular buildings remain enterable, and get
    // fittings only where they fit, with the entrance always clear.
    if(out.rooms.empty())for(double side:{-1.,1.})add(p.recipe=="home"?"sofa":"storage",side*1.8,std::min(back-1.,3.),1.1,.65,.9);
    return out;
}

std::vector<MeshPart> buildInteriorDoor(const InteriorPlan& p) {
    if(p.doorStyle=="sliding")return buildDoorLeaf(p.width);
    Mesh leaf,metal;const double w=p.width/2;
    leaf.addBox({0,1.05,0},{w-.015,2.1,.055});
    metal.addBox({w*.3,1.05,-.055},{.12,.035,.065});
    return {{"Swing door panel",std::move(leaf),surfaceMaterial("Timber door",{.14,.085,.04},.7,"deck",true)},
        {"Swing door handle",std::move(metal),surfaceMaterial("Door steel",{.12,.13,.14},.25,std::nullopt,true)}};
}
} // namespace r1
