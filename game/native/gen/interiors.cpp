#include "interiors.hpp"
#include "palette.hpp"
#include "clip.hpp"
#include <algorithm>
#include <cctype>

namespace r1 {
P2 InteriorPlan::point(double u,double v) const { return {door.x+along.x*u+inward.x*v,door.y+along.y*u+inward.y*v}; }
P2 InteriorPlan::local(P2 p) const { return {(p.x-door.x)*along.x+(p.y-door.y)*along.y,(p.x-door.x)*inward.x+(p.y-door.y)*inward.y}; }
nlohmann::json InteriorPlan::json() const {
    nlohmann::json polygon=nlohmann::json::array(); for(auto p:ring)polygon.push_back({p.x,p.y});
    nlohmann::json cars=nlohmann::json::array();for(auto p:exteriorVehicles)cars.push_back({p.x,p.y,p.z});
    return {{"id",id},{"footprint",footprint},{"edge",edge},{"recipe",recipe},{"name",name},
        {"nameSource",nameSource},{"entranceSource",entranceSource},{"layoutSource","synthesized"},
        {"useSource",useSource},{"region",region},{"doorStyle",doorStyle},{"storeysFurnished",1},
        {"exteriorVehicles",cars},{"vehicleSource",exteriorVehicles.empty()?"none":"synthesized:clear-forecourt"},
        {"ring",polygon},{"door",{door.x,door.y}},{"along",{along.x,along.y}},
        {"inward",{inward.x,inward.y}},{"width",width},{"floor",floor},{"ceiling",ceiling},{"approach",approach},
        {"anchor",anchor?nlohmann::json{anchor->x,anchor->y}:nlohmann::json()},{"anchorName",anchorName},
        {"anchorSource",anchor?"measured:tenant-node":"none"}};
}
InteriorPlan InteriorPlan::read(const nlohmann::json& j) {
    InteriorPlan p; p.id=j.at("id");p.footprint=j.at("footprint");p.edge=j.at("edge");
    p.recipe=j.at("recipe");p.name=j.at("name");p.nameSource=j.at("nameSource");p.entranceSource=j.at("entranceSource");
    for(auto& v:j.at("ring"))p.ring.push_back({v[0],v[1]});
    p.door={j.at("door")[0],j.at("door")[1]};p.along={j.at("along")[0],j.at("along")[1]};
    p.inward={j.at("inward")[0],j.at("inward")[1]};p.width=j.at("width");p.floor=j.at("floor");
    p.ceiling=j.at("ceiling");p.approach=j.at("approach");
    if(j.contains("anchor")&&j["anchor"].is_array())p.anchor=P2{j["anchor"][0],j["anchor"][1]};
    p.anchorName=j.value("anchorName","");p.useSource=j.value("useSource","inferred:building");
    p.region=j.value("region","");p.doorStyle=j.value("doorStyle","sliding");
    if(j.contains("exteriorVehicles"))for(const auto& c:j["exteriorVehicles"])p.exteriorVehicles.push_back({c[0],c[1],c[2]});
    return p;
}
bool retailUse(const Tags& t) {
    const auto shop=tagOr(t,"shop"); const auto b=tagOr(t,"building");
    // A funeral home is tagged shop=* but is no walk-in store: no shelves,
    // no sliding doors, no fascia; it keeps its ordinary building.
    if(shop=="funeral_directors"||shop=="car_repair"||shop=="car"||shop=="tyres")return false;
    // A fuel station (shop=gas, shop=convenience on its canopy) and a roof
    // on posts are no walk-in store either: gen/fuel draws them.
    if(tagOr(t,"amenity")=="fuel"||b=="roof")return false;
    return (!shop.empty()&&shop!="no"&&shop!="vacant")||b=="retail"||b=="supermarket"||b=="mall";
}
std::string retailRecipe(const Tags& t) {
    auto s=tagOr(t,"shop",tagOr(t,"building"));
    if(s=="bakery"||s=="pastry")return "bakery";
    if(s=="clothes"||s=="shoes"||s=="fashion")return "fashion";
    if(s=="mall"||s=="department_store")return "mall";
    return "market";
}
std::string retailName(const Tags& t) {
    auto n=tagOr(t,"name",tagOr(t,"brand"));if(!n.empty())return n;
    auto r=retailRecipe(t);return r=="bakery"?"BOULANGERIE":r=="fashion"?"MODE":r=="mall"?"GALERIE MARCHANDE":"MARCHE";
}
std::vector<P2> retailFronts(const Tags& t) { return retailPoints(tagOr(t,"r1:front")); }
std::vector<P2> retailPoints(const std::string& text) {
    std::vector<P2> out;
    for(size_t at=0;at<text.size();) {
        const size_t end=std::min(text.find(';',at),text.size()),comma=text.find(',',at);
        if(comma<end)out.push_back({std::stod(text.substr(at,comma-at)),std::stod(text.substr(comma+1,end-comma-1))});
        at=end+1;
    }
    return out;
}
bool chooseRetailPortal(InteriorPlan& p,const std::vector<P2>& targets,
                        const std::function<bool(size_t)>& eligible,double* offset) {
    struct Door { size_t edge; P2 at,along; double distance,clear; };
    std::vector<Door> doors;
    // How far the doorway's width runs straight into the room, walls kept
    // at arm's length: the aisle a customer walks on entering.
    auto clearance=[&](P2 q,P2 along,P2 inward) {
        double depth=0;
        for(double v=.5;v<=12.;v+=.5) {
            for(double u:{-p.width/2-.15,0.,p.width/2+.15})
                if(!pointInPolygon({q.x+along.x*u+inward.x*v,q.y+along.y*u+inward.y*v},p.ring))return depth;
            depth=v;
        }
        return depth;
    };
    // Every metre of an edge is a door position when no target is on the
    // outline; otherwise only the points nearest the targets are.
    for(bool slide:{false,true}) {
        if(slide&&std::any_of(doors.begin(),doors.end(),[](const Door& d){return d.distance<=1.5;}))break;
        for(auto target:targets)for(size_t e=0;e<p.ring.size();++e) {
            P2 a=p.ring[e],b=p.ring[(e+1)%p.ring.size()];const double len=dist(a,b);
            const double margin=p.doorStyle=="sliding"?2.:p.width/2+.3;
            if(len<(p.doorStyle=="sliding"?4.8:2*margin)||(eligible&&!eligible(e)))continue;
            const P2 along{(b.x-a.x)/len,(b.y-a.y)/len};
            auto add=[&](double t){
                const P2 q{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};
                doors.push_back({e,q,along,dist(q,target),clearance(q,along,{-along.y,along.x})});
            };
            if(!slide)add(std::clamp(((target.x-a.x)*(b.x-a.x)+(target.y-a.y)*(b.y-a.y))/(len*len),margin/len,1.-margin/len));
            else for(double s=margin;s<=len-margin+1e-9;s+=1)add(s/len);
        }
    }
    if(doors.empty())return false;
    // A door on a target (a mapped entrance) beats any door elsewhere; among
    // those, the deepest aisle wins. Only an inferred target, which lies off
    // the outline, lets a clearer edge further away take the door.
    const bool onTarget=std::any_of(doors.begin(),doors.end(),[](const Door& d){return d.distance<=1.5;});
    double deepest=0;
    for(auto& d:doors)if(!onTarget||d.distance<=1.5)deepest=std::max(deepest,d.clear);
    // An inferred door only has to open on a walkable aisle, not the deepest.
    if(!onTarget)deepest=std::min(deepest,6.);
    const Door* best=nullptr;
    for(auto& d:doors)if((!onTarget||d.distance<=1.5)&&d.clear>=deepest-1e-9&&(!best||d.distance<best->distance))best=&d;
    p.edge=best->edge;p.door=best->at;p.along=best->along;p.inward={-p.along.y,p.along.x};
    if(offset)*offset=best->distance;
    return true;
}
namespace {
Material plain(const char* name,std::array<double,4> color,double rough=.7) {
    Material m;m.name=name;m.color=color;m.roughness=rough;m.doubleSided=true;return m;
}
Material mineral(const char* name,std::array<double,4> albedo,double rough) {
    auto m=plain(name,albedo,rough);
    for(size_t i=0;i<3;++i)m.color[i]*=2;
    m.baseColorTexture="assets/textures/interiors/mineral_albedo.jpg";
    m.uvScale=.5; // The original scan spans two metres.
    return m;
}
void box(Mesh& m,const InteriorPlan& p,double u,double v,double y,double w,double d,double h) {
    auto c=p.point(u,v);m.addBox({c.x,p.floor+y,c.y},{w,h,d},-std::atan2(p.along.y,p.along.x));
}

}
InteriorLayout layoutInterior(const InteriorPlan& p) {
    if(!retailInterior(p.recipe))return layoutBuildingInterior(p);
    InteriorLayout out;
    double reach=0,back=0;
    for(auto q:p.ring) {
        q=p.local(q);reach=std::max(reach,std::abs(q.x));back=std::max(back,q.y);
    }
    const auto inner=clip::bufferRing(p.ring,-.65);
    auto fitsHere=[&](double u,double v,double w,double d) {
        const Ring r{p.point(u-w/2,v-d/2),p.point(u+w/2,v-d/2),p.point(u+w/2,v+d/2),p.point(u-w/2,v+d/2)};
        return clip::area(clip::subtract({clip::kMetres.path(r)},inner))<.001;
    };
    // Anchor both sides to the entrance spine. Sampling the complete grid
    // distributes the bounded instance count throughout large floor plates.
    auto grid=[&](const std::string& recipe,const std::function<bool(double,double)>& keep,int modules=1) {
        const bool bakery=recipe=="bakery",fashion=recipe=="fashion",mall=recipe=="mall";
        const double width=mall?3.6:1.,depth=(bakery?1.1:2.5)*modules,spacing=mall?6.5:3.5;
        const double gridScale=std::max(1.,std::sqrt(2*reach*back/(4096*spacing*(depth+2.2))));
        std::vector<InteriorFixture> candidates;
        for(double v=5;v<back-2;v+=(depth+2.2)*gridScale)
            for(double u=2.1+width/2;u<reach;u+=spacing*gridScale)
                for(double side:{-1.,1.})if(keep(side*u,v)&&fitsHere(side*u,v,width,depth))
                    candidates.push_back({{side*u,v},{width,depth},fashion?1.65:bakery?1.1:1.75,
                        bakery?"display":fashion?"rail":mall?"kiosk":"shelf"});
        return candidates;
    };
    auto random=seeded(p.id,0x4C41594F5554LL);
    auto take=[&](const std::vector<InteriorFixture>& candidates,size_t most) {
        const size_t count=std::min(most,candidates.size());
        for(size_t i=0;i<count;++i) {
            auto f=candidates[i*candidates.size()/count];
            f.variant=std::min(3,int(random.random()*4));out.fixtures.push_back(f);
        }
    };
    if(p.recipe=="mall"&&p.anchor) {
        // The anchor's sales floor gets aisles; its checkouts line the edge
        // it shares with the gallery, which keeps its kiosks. A hypermarket's
        // gondola is three shelf modules long: three instances of the same
        // prototype, so the floor fills without one more vertex in the arena.
        auto sales=grid("market",[&](double u,double v){return p.inAnchor(p.point(u,v));},3);
        const auto gallery=grid("mall",[&](double u,double v){return !p.inAnchor(p.point(u,v));});
        std::vector<InteriorFixture> shelves,tills;
        for(auto f:sales) {
            const P2 at=p.point(f.at.x,f.at.y);
            if(dist(at,p.door)-dist(at,*p.anchor)<9&&fitsHere(f.at.x,f.at.y,1.5,1.1))
                tills.push_back({f.at,{1.5,1.1},.95,"checkout"});
            else shelves.push_back(f);
        }
        const size_t total=shelves.size()+gallery.size();
        const size_t forShelves=total?(128*shelves.size()+total/2)/total:0;
        const size_t first=out.fixtures.size();
        take(shelves,forShelves);
        std::vector<InteriorFixture> modules;
        for(size_t i=first;i<out.fixtures.size();++i)for(double dv:{-2.5,0.,2.5}) {
            auto f=out.fixtures[i];f.at.y+=dv;f.size.y=2.5;modules.push_back(f);
        }
        out.fixtures.resize(first);out.fixtures.insert(out.fixtures.end(),modules.begin(),modules.end());
        take(gallery,128-std::min<size_t>(128,forShelves));
        const size_t lanes=std::min<size_t>(12,tills.size());
        for(size_t i=0;i<lanes;++i)out.fixtures.push_back(tills[i*tills.size()/lanes]);
        return out;
    }
    take(grid(p.recipe,[](double,double){return true;}),128);
    for(double u:{-3.5,3.5})if(fitsHere(u,2.3,1.5,1.1))
        out.fixtures.push_back({{u,2.3},{1.5,1.1},.95,"checkout"});
    return out;
}
std::vector<MeshPart> buildShopfront(const InteriorPlan& p) {
    Mesh shell(UvMode::Slope),metal,accent,glass;
    const double h=p.ceiling-p.floor;
    // Hollow shell follows the actual footprint, with a real opening in one edge.
    for(size_t e=0;e<p.ring.size();++e) {
        P2 a=p.ring[e],b=p.ring[(e+1)%p.ring.size()];double len=dist(a,b);
        auto face=[&](double x0,double x1,double y0,double y1,Mesh& m){
            auto at=[&](double x,double y){return P3{a.x+(b.x-a.x)*x/len,p.floor+y,a.y+(b.y-a.y)*x/len};};
            m.addQuad(at(x0,y0),at(x1,y0),at(x1,y1),at(x0,y1));
        };
        if(e!=p.edge){face(0,len,0,h,shell);continue;}
        const double mid=dist(a,p.door),left=mid-p.width/2,right=mid+p.width/2;
        face(0,len,2.65,h,shell);
        // Low opaque plinth and open clear glazing, framed at regular bays.
        for(auto span:{std::pair<double,double>{0,left},{right,len}}){
            face(span.first,span.second,0,.28,shell);
            for(double x=span.first;x<span.second-.1;x+=2.4){
                double end=std::min(x+2.4,span.second);
                face(x,std::min(x+.07,end),.28,2.65,metal);
                // Narrow blue reflection bands suggest glass without hiding
                // the interior (engine currently has no sorted glass pass).
                face(x+.07,end,.35,.43,glass);face(x+.07,end,2.48,2.55,glass);
            }
        }
    }
    const double len=dist(p.ring[p.edge],p.ring[(p.edge+1)%p.ring.size()]);
    const double center=p.local({(p.ring[p.edge].x+p.ring[(p.edge+1)%p.ring.size()].x)/2,
                                (p.ring[p.edge].y+p.ring[(p.edge+1)%p.ring.size()].y)/2}).x;
    const double fasciaHeight=std::clamp(h-2.8,.3,1.1);
    box(accent,p,center,-.12,2.8+fasciaHeight/2,len,.28,fasciaHeight);
    box(metal,p,center,-.85,2.72,len,1.9,.12);
    for(double u:{-p.width/2,p.width/2})box(metal,p,u,0,1.3,.1,.14,2.6);
    box(metal,p,0,0,2.6,p.width+.2,.22,.13);
    auto wall=mineral("Retail mineral cladding",{.28,.27,.25,1},.85);
    auto paint=plain("Retail fascia",p.recipe=="bakery"?std::array<double,4>{.19,.065,.028,1}:
        p.recipe=="fashion"?std::array<double,4>{.035,.045,.052,1}:std::array<double,4>{.028,.105,.085,1});
    std::string brand=p.name;
    std::transform(brand.begin(),brand.end(),brand.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(brand.find("carrefour")!=std::string::npos)paint.color={.018,.043,.105,1};
    return {{"Shop shell",std::move(shell),wall},{"Shop frames and canopy",std::move(metal),plain("Anodized metal",{.065,.075,.08,1},.3)},
        {"Shop fascia",std::move(accent),paint},{"Shop glazing reflections",std::move(glass),plain("Glass reflections",{.10,.17,.20,1},.12)}};
}
std::vector<MeshPart> buildDoorLeaf(double width) {
    Mesh frame,glass; double w=width/2;
    for(double x:{-w/2,w/2})frame.addBox({x,1.28,0},{.055,2.5,.07});
    for(double y:{.05,1.05,2.52})frame.addBox({0,y,0},{w,.045,.075});
    glass.addBox({0,1.1,0},{w,.09,.025});
    frame.addBox({w*.3,1.2,-.07},{.035,.32,.04});
    return {{"Sliding door frame",std::move(frame),plain("Door aluminium",{.12,.14,.15,1},.25)},
            {"Sliding door safety strip",std::move(glass),plain("Safety strip",{.55,.58,.55,1},.4)}};
}
std::string interiorFixtureKey(const InteriorFixture& f) {
    return f.kind+"/"+std::to_string(f.size.x)+"/"+std::to_string(f.size.y)+"/"+
           std::to_string(f.height)+"/"+std::to_string(f.variant);
}
std::vector<MeshPart> buildInteriorFixture(InteriorFixture fixture) {
    if(fixture.kind!="checkout"&&fixture.kind!="display"&&fixture.kind!="rail"&&fixture.kind!="shelf"&&fixture.kind!="kiosk")
        return buildBuildingFixture(fixture);
    InteriorPlan p;p.id=fixture.variant;p.along={1,0};p.inward={0,1};fixture.at={0,0};
    Mesh fixtures,goods,trim,labels;
    auto random=seeded(p.id,0x494E544552494F52LL);
    auto product=[&](double u,double v,double y,double w,double d,double h) {
        const size_t first=goods.vertexCount();box(goods,p,u,v,y,w,d,h);
        const std::array<double,3> inks[]={{.65,.22,.12},{.12,.38,.55},{.72,.63,.36},{.18,.43,.21},{.64,.60,.53}};
        const auto color=inks[std::min(4,int(random.random()*5))];
        goods.colors.resize(goods.vertexCount());for(size_t i=first;i<goods.vertexCount();++i)goods.colors[i]=color;
    };
    for(auto f:std::vector<InteriorFixture>{fixture}) {
        const double u=f.at.x,v=f.at.y,w=f.size.x,d=f.size.y;
        if(f.kind=="checkout"||f.kind=="display") {
            box(fixtures,p,u,v,f.height/2,w,d,f.height);
            box(trim,p,u,v,f.height+.04,w+.08,d+.08,.08);
            if(f.kind=="checkout")product(u,v,f.height+.25,.4,.3,.4);
        } else if(f.kind=="rail") {
            for(double dv:{-d/2,d/2})box(trim,p,u,v+dv,.8,.06,.06,1.6);
            box(trim,p,u,v,1.6,.06,d,.06);
            for(double dv=-d/2+.2;dv<d/2;dv+=.25)product(u,v+dv,1.15,.7,.08,.75);
        } else {
            for(double dv:{-d/2,d/2})box(trim,p,u,v+dv,f.height/2,w,.06,f.height);
            for(double y=.18;y<f.height;y+=.45) {
                box(fixtures,p,u,v,y,w,d,.06);
                for(double dv=-d/2+.16;dv<d/2-.1;dv+=.23)for(double side:{-1.,1.}) {
                    const double height=.22+random.random()*.12;
                    product(u+side*w*.3,v+dv,y+.03+height/2,w*.27,.18,height);
                    // Printed fronts on individual packages, not one solid
                    // block for the whole shelf. Shared per merchandise variant.
                    const double x=u+side*w*.437;
                    const double low=y+.09,high=low+height*.36;
                    labels.addQuad({x,low,v+dv-.067},{x,high,v+dv-.067},
                                   {x,high,v+dv+.067},{x,low,v+dv+.067});
                }
            }
        }
    }
    return {{"Furniture panels",std::move(fixtures),plain("Powder coated shelving",{.22,.235,.23,1})},
        {"Merchandise",std::move(goods),plain("Merchandise",{.38,.38,.38,1})},
        {"Furniture metalwork",std::move(trim),plain("Fittings",{.14,.15,.15,1},.35)},
        {"Package labels",std::move(labels),plain("Printed paper",{.55,.53,.47,1},.8)}};
}
std::vector<MeshPart> buildInteriorShell(const InteriorPlan& p) {
    Mesh floor(UvMode::Planar),ceiling,lining,joints,lights;
    for(auto t:triangulate(p.ring)) {
        auto a=p.ring[t[0]],b=p.ring[t[1]],c=p.ring[t[2]];
        floor.addUpTriangle({a.x,p.floor,a.y},{b.x,p.floor,b.y},{c.x,p.floor,c.y});
        ceiling.addTriangle({a.x,p.ceiling,a.y},{b.x,p.ceiling,b.y},{c.x,p.ceiling,c.y});
    }
    auto a=p.point(-p.width/2-.4,0),b=p.point(p.width/2+.4,0),c=p.point(p.width/2+.4,-4),d=p.point(-p.width/2-.4,-4);
    floor.addUpQuad({a.x,p.floor,a.y},{b.x,p.floor,b.y},{c.x,p.approach,c.y},{d.x,p.approach,d.y});
    double lo=1e9,hi=-1e9,back=0;
    for(auto q:p.ring){q=p.local(q);lo=std::min(lo,q.x);hi=std::max(hi,q.x);back=std::max(back,q.y);}
    const clip::Paths64 region{clip::kMetres.path(p.ring)};
    auto stripe=[&](P2 a,P2 b,double y,double half){
        auto paths=clip::intersect(region,clip::bufferLine({a,b},half));
        for(auto& poly:clip::polygons(paths))for(auto t:clip::triangles(poly))
            joints.addUpTriangle({t[0].x,y,t[0].y},{t[1].x,y,t[1].y},{t[2].x,y,t[2].y});
    };
    // Metric porcelain joints and suspended ceiling rails; bounded for very
    // large malls, with the same floor/ceiling surface covering the full ring.
    const double pitch=std::max(p.recipe=="home"?2.:1.,std::max(hi-lo,back)/96.);
    for(double u=std::ceil(lo/pitch)*pitch;u<hi;u+=pitch) {
        if(p.recipe!="home") {
            stripe(p.point(u,0),p.point(u,back),p.floor+.003,.004);
            stripe(p.point(u,0),p.point(u,back),p.ceiling-.035,.012);
        }
    }
    for(double v=0;v<back;v+=pitch) {
        if(p.recipe!="home") {
            stripe(p.point(lo,v),p.point(hi,v),p.floor+.003,.004);
            stripe(p.point(lo,v),p.point(hi,v),p.ceiling-.035,.012);
        }
    }
    for(size_t e=0;e<p.ring.size();++e)if(e!=p.edge) {
        const auto a=p.ring[e],b=p.ring[(e+1)%p.ring.size()];const double len=dist(a,b);
        const P2 n{-(b.y-a.y)/len*.025,(b.x-a.x)/len*.025};
        lining.addQuad({a.x+n.x,p.floor,a.y+n.y},{b.x+n.x,p.floor,b.y+n.y},
                       {b.x+n.x,p.ceiling,b.y+n.y},{a.x+n.x,p.ceiling,a.y+n.y});
    }
    const double lightPitch=std::max(7.,std::max(hi-lo,back)/10.);
    for(double v=4;v<back;v+=lightPitch)for(double u=lo+3;u<hi;u+=lightPitch) {
        auto at=p.point(u,v);if(!pointInPolygon(at,p.ring))continue;
        box(lights,p,u,v,p.ceiling-p.floor-.08,p.recipe=="home"?.38:.28,p.recipe=="home"?.38:1.8,.035);
    }
    auto mat=mineral("Polished mineral floor",{.23,.235,.22,1},.4);
    if(p.recipe=="home")mat=surfaceMaterial("Residential timber floor",{.25,.18,.105},.76,"deck",true);
    if(p.recipe=="garage"||p.recipe=="warehouse")mat=mineral("Workshop concrete floor",{.13,.135,.13,1},.9);
    return {{"Interior floor and accessible threshold",std::move(floor),mat},
        {"Interior ceiling",std::move(ceiling),plain("Acoustic ceiling",{.30,.30,.28,1},.95)},
        {"Interior lining",std::move(lining),plain("Interior plaster",{.26,.265,.25,1},.9)},
        {"Interior surface joints",std::move(joints),plain("Joints",{.12,.125,.12,1})},
        {"Interior linear luminaires",std::move(lights),plain("Light diffuser",{.7,.68,.6,1},.9)}};
}
double slideDoor(double opening,bool near,double dt) {
    return std::clamp(opening+(near?1.6:-.7)*std::max(0.,dt),0.,1.);
}
} // namespace r1
