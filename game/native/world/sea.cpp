#include "session.hpp"

namespace r1::world {
double World::Impl::metresFrom(double x,double y) const {
    const glm::dvec3 d=origin.local(ecef(x,y,0.))-origin.local(ecef(lon,lat,0.));
    return std::hypot(d.x,d.z);
}

saida::Node* World::Impl::hullPrototype(const std::string& model,const json& doc) {
    auto& p=hullPrototypes[model];
    if(!p&&doc.is_object()) {
        try {p=prototypes->addChild(plantNode(doc));}
        catch(const std::exception& e){saida::Log::warn("[World sea] hull ",model," failed: ",e.what());}
    }
    return p;
}

void World::Impl::readSea() {
    auto latest=sea->latest();
    if(!latest||latest==seaDoc)return;
    seaDoc=latest;
    const json& doc=*latest;
    const json source=doc.value("source",json::object());
    const json hulls=doc.value("hulls",json::object());
    std::set<std::string> have;
    for(auto& s:seaShips)have.insert(s.id);
    size_t added=0;
    for(const auto& s:doc.value("ships",json::array())) {
        if(seaShips.size()>=kSeaShips)break;
        const std::string id=s.value("id","");
        if(id.empty()||have.count(id)||seaTaken.count(id))continue;
        const double x=s.value("lon",0.),y=s.value("lat",0.);
        if(!seaFirst&&metresFrom(x,y)<kSeaAppear)continue;
        if(tile(x,y)&&!navigable(x,y))continue;       // the cell's land, where the map knows it
        const std::string model=s.value("model","");
        auto* proto=hullPrototype(model,hulls.value(model,json()));
        if(!proto)continue;
        SeaShip ship;ship.id=id;
        ship.phase=double(std::hash<std::string>{}(id)%1000)*.0063;
        Vessel& v=ship.v;
        v.node=engine.sceneTree().world().addChild(clonePlant(*proto));
        v.kind=s.value("kind","commercial");v.length=s.value("length",20.);v.beam=s.value("beam",5.);
        v.top=s.value("top",8.);v.accel=s.value("accel",.5);v.turn=s.value("turn",10.);
        v.lon=x;v.lat=y;v.yaw=wrap(s.value("heading",0.));v.speed=s.value("speed",0.);
        v.alt=tile(x,y)?waterLevel(x,y):0.;
        if(std::getenv("R1WORLD_SEA_DEBUG")) {
            const glm::dvec3 at=origin.local(ecef(v.lon,v.lat,v.alt));
            saida::Log::info("[World sea] ship ",id," ",model," at ",at.x,",",at.y,",",at.z," alt ",v.alt);
        }
        seaShips.push_back(std::move(ship));++added;
    }
    seaFirst=false;
    const bool online=source.value("online",false);
    const std::string weather=source.value("weather",std::string("?"));
    const std::string synced=source.contains("synced")&&source["synced"].is_string()
        ?source["synced"].get<std::string>():std::string("never");
    const std::string said=weather+(online?" online":" offline");
    if(said!=seaSaid||added) {
        saida::Log::info("[World sea] ",seaShips.size()," ships (+",added,"), weather ",weather,", ",
                         online?"online":"offline",", last synced ",synced,
                         ", live ",source.value("live",0));
        seaSaid=said;
    }
}

void World::Impl::updateSea(double dt) {
    seaAsk+=dt;seaRead+=dt;
    if(seaAsk>5.) {
        seaAsk=0;double t=0;
        if(gameTime(t)){sea->ask(lon,lat,t);sky->ask(lon,lat);}
    }
    if(seaRead>1.){seaRead=0;readSea();}
    for(size_t i=0;i<seaShips.size();) {
        Vessel& v=seaShips[i].v;
        if(v.speed>0) {
            // Look a hull length and a half ahead; where the map says land,
            // come round to the first open bearing, stopping if there is none.
            auto clear=[&](double heading){
                const double s=std::sin(heading*rad),c=std::cos(heading*rad);
                const double lead=v.length*.75+v.speed*8.;
                auto bow=advance(v.lon,v.lat,s*lead,c*lead);
                return !tile(bow.x,bow.y)||navigable(bow.x,bow.y);
            };
            if(!clear(v.yaw)) {
                double turn=0;
                for(double d:{25.,-25.,50.,-50.,90.,-90.,135.,-135.,180.})
                    if(clear(v.yaw+d)){turn=d;break;}
                if(turn==0)v.speed=0;
                else v.yaw=wrap(v.yaw+std::clamp(turn,-v.turn*dt,v.turn*dt));
            }
            const double s=std::sin(v.yaw*rad),c=std::cos(v.yaw*rad);
            const auto next=advance(v.lon,v.lat,s*v.speed*dt,c*v.speed*dt);
            if(tile(next.x,next.y)&&!navigable(next.x,next.y))v.speed=0;
            else {
                v.lon=next.x;v.lat=next.y;
                v.alt=tile(v.lon,v.lat)?waterLevel(v.lon,v.lat):0.;
            }
        }
        if(metresFrom(v.lon,v.lat)>kSeaKeep) {
            if(v.node)v.node->queueFree();
            seaShips.erase(seaShips.begin()+long(i));
            continue;
        }
        ++i;
    }
}

} // namespace r1::world
