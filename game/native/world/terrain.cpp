#include "session.hpp"

namespace r1::world {
Loaded* World::Impl::tile(double x,double y) {
    auto i=loaded.find(tileAt(x,y).key());return i==loaded.end()?nullptr:&i->second;
}

bool World::Impl::onSeaIce(double x,double y) {
    auto* t=tile(x,y);return t&&t->seaIce;
}

glm::dvec2 World::Impl::onward(double x,double y,double east,double north) const {
    const glm::dvec3 d=origin.basis[0]*east-origin.basis[2]*north;
    const double l=x*rad,p=y*rad;
    const double e=glm::dot(d,glm::dvec3(-std::sin(l),std::cos(l),0.));
    const double n=glm::dot(d,glm::dvec3(-std::sin(p)*std::cos(l),-std::sin(p)*std::sin(l),std::cos(p)));
    const double length=std::hypot(east,north),flat=std::hypot(e,n);
    if(flat<1e-12)return {x,y};
    return advance(x,y,e/flat*length,n/flat*length);
}

void World::Impl::rebaseOrigin() {
    const Frame next(lon,lat,alt);
    const auto rocketRotation=glm::mat3(glm::transpose(next.basis)*origin.basis);
    const auto rocketTranslation=glm::vec3(next.local(origin.origin));
    for(auto& r:rockets)if(r.mesh) {
        r.position=rocketRotation*r.position+rocketTranslation;r.direction=rocketRotation*r.direction;
        r.mesh->transform().position=r.position;r.mesh->transform().rotation=glm::quat(glm::vec3(0,0,1),r.direction);
        r.trail->transform().position=rocketRotation*r.trail->transform().position+rocketTranslation;
        r.trail->initialVelocity=-r.direction*5.f;
        r.flame->transform().position=r.position-r.direction*.7f;r.flame->initialVelocity=-r.direction*14.f;
    }
    rocketEffects.rebase(rocketRotation,rocketTranslation);
    rebaseAnimalRemains(next);
    auto rebaseVehicle=[&](saida::Node* node) {
        if(!node)return;
        r1::VehicleRuntime::synchronize(*node);
        const auto worldPoint=origin.origin+origin.basis*glm::dvec3(node->transform().position);
        const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(next.basis)*origin.basis));
        engine.sceneTree().world().rebaseSubtreeTo(*node,glm::vec3(next.local(worldPoint)),
            glm::normalize(rotation*node->transform().rotation));
    };
    rebaseVehicle(car);for(auto& entry:parked)rebaseVehicle(entry.node);
    for(auto& unit:policeUnits)if(unit.car){rebaseVehicle(unit.car);for(auto& officer:unit.officers)rebaseVehicle(officer.person.node);}
    rebaseVehicle(policeHelicopter);policeRoads.points.clear();policeThink=.5;
    auto turn=[&](double heading){
        const glm::dvec3 d=origin.basis[0]*std::sin(heading*rad)-origin.basis[2]*std::cos(heading*rad);
        return wrap(std::atan2(glm::dot(d,next.basis[0]),-glm::dot(d,next.basis[2]))/rad);
    };
    const double delta=wrap(turn(yaw)-yaw);
    if(drop.phase!=Drop::None) {
        drop.canopy.heading=turn(drop.canopy.heading);drop.canopyWanted=turn(drop.canopyWanted);
        const double speed=drop.fall.speed(),heading=turn(std::atan2(drop.fall.east,drop.fall.north)/rad);
        drop.fall.east=speed*std::sin(heading*rad);drop.fall.north=speed*std::cos(heading*rad);
    }
    yaw=turn(yaw);carYaw=turn(carYaw);swimHeading=turn(swimHeading);
    boat.yaw=turn(boat.yaw);plane.yaw=turn(plane.yaw);
    for(auto& entry:parked)entry.yaw=turn(entry.yaw);
    if(std::abs(delta)>1e-9) {
        const auto spin=glm::angleAxis(float(-delta*rad),glm::vec3(0,1,0));
        player->transform().rotation=spin*player->transform().rotation;
    }
    engine.sceneTree().world().notifyOriginRebase(next.local(origin.origin),glm::normalize(glm::quat_cast(glm::transpose(next.basis)*origin.basis)));
    origin=next;placeTiles();moveSun();
}

void World::Impl::unpack(Loaded& tile) {
    if(auto rooms=tile.data.find("interiors");rooms!=tile.data.end())for(const auto& entry:*rooms) {
        LiveInterior room;room.plan=r1::InteriorPlan::read(entry);
        for(auto p:room.plan.ring){room.low.x=std::min(room.low.x,p.x);room.low.y=std::min(room.low.y,p.y);
            room.high.x=std::max(room.high.x,p.x);room.high.y=std::max(room.high.y,p.y);}
        if(room.plan.footprint<tile.footprints.size())tile.footprints[room.plan.footprint].interior=int(tile.interiors.size());
        tile.interiors.push_back(std::move(room));
    }
    if(auto streaming=tile.data.find("interiorStreaming");streaming!=tile.data.end()&&streaming->contains("unavailable")) {
        std::map<std::string,size_t> reasons;for(const auto& p:streaming->at("unavailable"))++reasons[p.at("reason").get<std::string>()];
        for(const auto& [reason,count]:reasons)saida::Log::info("[World interiors] ",count," buildings unavailable: ",reason);
    }
    if(auto retail=tile.data.find("retail");retail!=tile.data.end())for(const auto& area:retail->at("parking")) {
        StoreParking lot;lot.name=area.at("storeName");
        for(const auto& path:area.at("rings")){r1::Ring r;for(const auto& p:path)r.push_back({p[0],p[1]});lot.rings.push_back(std::move(r));}
        tile.parking.push_back(std::move(lot));
    }
    const auto& bounds=tile.data.at("bounds");
    tile.west=bounds.at("west");tile.east=bounds.at("east");
    tile.south=bounds.at("south");tile.north=bounds.at("north");
    const auto& grid=tile.data.at("elevations");
    tile.gridSize=int(grid.size());
    tile.elevation.resize(size_t(tile.gridSize)*size_t(tile.gridSize));
    tile.groundGrid = r1::ElevationGrid{{tile.south,tile.west,tile.north,tile.east},tile.gridSize,{}};
    for(int row=0;row<tile.gridSize;++row) {
        const auto& line=grid[size_t(row)];
        for(int col=0;col<tile.gridSize;++col) {
            tile.elevation[size_t(row)*size_t(tile.gridSize)+size_t(col)]=float(line[size_t(col)]);
            tile.groundGrid.values.push_back(line[size_t(col)].get<double>());
        }
    }
    if(auto edges=tile.data.find("groundEdges");edges!=tile.data.end()) {
        for(const auto& p:edges->at("south"))tile.groundGrid.southEdge.push_back({p[0],p[1]});
        for(const auto& p:edges->at("north"))tile.groundGrid.northEdge.push_back({p[0],p[1]});
    }
    auto water=tile.data.find("water");
    if(water!=tile.data.end()&&water->is_array()&&!water->empty()) {
        tile.waterRows=int(water->size());
        const std::string first=(*water)[0].get<std::string>();
        tile.waterCols=int(first.size());
        tile.water.assign(size_t(tile.waterRows)*size_t(tile.waterCols),0);
        for(int row=0;row<tile.waterRows;++row) {
            const std::string line=(*water)[size_t(row)].get<std::string>();
            for(int col=0;col<tile.waterCols&&col<int(line.size());++col)
                tile.water[size_t(row)*size_t(tile.waterCols)+size_t(col)]=uint8_t(std::max(0,line[size_t(col)]-'0'));
        }
    }
    tile.ocean=tile.data.value("surface",std::string())=="ocean";
    tile.seaIce=tile.data.value("surface",std::string())=="sea-ice";
    auto decks=tile.data.find("decks");
    if(decks!=tile.data.end())
        for(const auto& entry:*decks) {
            Deck deck;deck.y=entry.at("y");
            for(const auto& point:entry.at("points")) {
                glm::dvec2 q{double(point[0]),double(point[1])};
                deck.points.push_back(q);deck.low=glm::min(deck.low,q);deck.high=glm::max(deck.high,q);
            }
            tile.deckLow=glm::min(tile.deckLow,deck.low);tile.deckHigh=glm::max(tile.deckHigh,deck.high);
            tile.decks.push_back(std::move(deck));
        }
    if(auto dry=tile.data.find("dryStreets");dry!=tile.data.end())
        for(const auto& ring:*dry) {
            Deck piece;
            for(const auto& point:ring) {
                glm::dvec2 q{double(point[0]),double(point[1])};
                piece.points.push_back(q);piece.low=glm::min(piece.low,q);piece.high=glm::max(piece.high,q);
            }
            if(piece.points.size()<3)continue;
            tile.dryLow=glm::min(tile.dryLow,piece.low);tile.dryHigh=glm::max(tile.dryHigh,piece.high);
            tile.dryStreets.push_back(std::move(piece));
        }
    auto raised=tile.data.find("raised");
    if(raised!=tile.data.end())
        for(const auto& entry:*raised) {
            RaisedPiece r;
            r.a={double(entry.at("a")[0]),double(entry.at("a")[2])};r.ya=entry.at("a")[1];
            r.b={double(entry.at("b")[0]),double(entry.at("b")[2])};r.yb=entry.at("b")[1];
            for(int s=0;s<2;++s){r.half[s]=entry.at("half")[s];r.rail[s]=entry.at("rail")[s];}
            r.solid=entry.at("solid");
            // An embankment's slopes reach out 2 in 3 below its road.
            const double reach=std::max(r.half[0],r.half[1])+(r.solid?1.5*20.:1.2);
            r.low=glm::min(r.a,r.b)-glm::dvec2(reach);r.high=glm::max(r.a,r.b)+glm::dvec2(reach);
            tile.raisedLow=glm::min(tile.raisedLow,r.low);tile.raisedHigh=glm::max(tile.raisedHigh,r.high);
            tile.raised.push_back(r);
        }
    auto boats=tile.data.find("boats");
    if(boats!=tile.data.end())
        for(const auto& entry:*boats) {
            Mooring m;
            m.name=entry.value("name",std::string());m.kind=entry.value("kind",std::string("motor"));
            m.model=entry.value("model",std::string());
            m.local={entry.value("x",0.),entry.value("y",0.),entry.value("z",0.)};
            m.heading=entry.value("heading",0.);m.length=entry.value("length",8.);
            m.beam=entry.value("beam",3.);m.top=entry.value("top",10.);
            m.accel=entry.value("accel",2.);m.turn=entry.value("turn",30.);
            tile.boats.push_back(std::move(m));
        }
    auto aircraft=tile.data.find("aircraft");
    if(aircraft!=tile.data.end())
        for(const auto& entry:*aircraft) {
            AircraftSpot spot;
            spot.name=entry.value("name",std::string());spot.type=entry.value("type",std::string());
            spot.local={entry.value("x",0.),entry.value("y",0.),entry.value("z",0.)};
            spot.heading=entry.value("heading",0.);
            tile.aircraft.push_back(std::move(spot));
        }
}

bool World::Impl::inside(const Loaded& tile,double x,double y,double margin) const {
    return x>=tile.west-margin&&x<=tile.east+margin
         &&y>=tile.south-margin&&y<=tile.north+margin;
}

bool World::Impl::onDeck(const Loaded& t,double x,double y,double* level) const {
    if(t.decks.empty())return false;
    const glm::dvec3 p=t.frame.local(ecef(x,y,0.));
    const glm::dvec2 q(p.x,p.z);
    if(q.x<t.deckLow.x||q.x>t.deckHigh.x||q.y<t.deckLow.y||q.y>t.deckHigh.y)return false;
    for(const Deck& d:t.decks) {
        if(q.x<d.low.x||q.x>d.high.x||q.y<d.low.y||q.y>d.high.y)continue;
        bool in=false;const auto& poly=d.points;
        for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++)
            if((poly[i].y>q.y)!=(poly[j].y>q.y)&&q.x<(poly[j].x-poly[i].x)*(q.y-poly[i].y)/(poly[j].y-poly[i].y)+poly[i].x)in=!in;
        if(in){if(level)*level=d.y;return true;}
    }
    return false;
}

bool World::Impl::onDryStreet(const Loaded& t,double x,double y) const {
    if(t.dryStreets.empty())return false;
    const glm::dvec3 p=t.frame.local(ecef(x,y,0.));
    const glm::dvec2 q(p.x,p.z);
    if(q.x<t.dryLow.x||q.x>t.dryHigh.x||q.y<t.dryLow.y||q.y>t.dryHigh.y)return false;
    bool in=false;
    for(const Deck& d:t.dryStreets) {
        if(q.x<d.low.x||q.x>d.high.x||q.y<d.low.y||q.y>d.high.y)continue;
        const auto& poly=d.points;
        for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++)
            if((poly[i].y>q.y)!=(poly[j].y>q.y)&&q.x<(poly[j].x-poly[i].x)*(q.y-poly[i].y)/(poly[j].y-poly[i].y)+poly[i].x)in=!in;
    }
    return in;
}

double World::Impl::height(double x,double y,double standing) {
    auto* t=tile(x,y);if(!t)throw std::runtime_error("Missing terrain");
    const double ground=terrainHeight(*t,x,y);
    const bool any=std::isnan(standing);
    double best=ground;
    const auto worldPoint=ecef(x,y,ground);
    for(const auto& [key,other]:loaded) {
      const auto local=other.frame.local(worldPoint);const r1::P2 q{local.x,local.z};
      for(const auto& room:other.interiors) {
        const auto& p=room.plan;
        const double reach=std::max(4.5,p.approachRun()+.5);
        if(q.x<room.low.x-reach||q.x>room.high.x+reach||q.y<room.low.y-reach||q.y>room.high.y+reach)continue;
        const auto door=p.local(q);
        double level=0;
        if(room.contains(q))level=p.floor;
        else if(std::abs(door.x)<p.width/2+.4&&door.y>=-p.approachRun()&&door.y<=0)
            level=p.floor+(p.approach-p.floor)*(-door.y/p.approachRun());
        else continue;
        // Store floors replace the terrain inside, even on a slope.
        return level-other.frame.local(ecef(x,y,0.)).y;
      }
    }
    // A piece belongs to the tile its middle is in: near a tile's edge,
    // the deck under a foot may be its neighbour's.
    for(const auto& [key,other]:loaded)
        raisedAt(other,x,y,[&](double level,bool,bool parapet){
            if(parapet||level<=best)return;
            if(any||level<=standing+kStepUp)best=level;
        });
    return best;
}

double World::Impl::terrainHeight(const Loaded& tile,double x,double y) const {
    double deck=0;if(onDeck(tile,x,y,&deck))return deck;
    return gridHeight(tile,x,y);
}

double World::Impl::gridHeight(const Loaded& tile,double x,double y) {
    const Loaded* t=&tile;
    const int n=t->gridSize;
    double u=std::clamp((wrap(x)-t->west)/(t->east-t->west),0.,1.)*(n-1);
    double v=std::clamp((y-t->south)/(t->north-t->south),0.,1.)*(n-1);
    if((v<1&&!t->groundGrid.southEdge.empty())||(v>n-2&&!t->groundGrid.northEdge.empty()))
        return r1::terrainElevation(wrap(x),y,t->groundGrid);
    int ix=std::min(n-2,int(u)),iy=std::min(n-2,int(v));u-=ix;v-=iy;
    const float* g=t->elevation.data();
    const size_t low=size_t(iy)*size_t(n),high=low+size_t(n);
    // The two triangles the ground is drawn and collided with (gen/terrain
    // buildTerrain: south-west to north-east diagonal), never a bilinear
    // patch: on an embankment the two disagree by decimetres, and a car
    // riding the patch met the drawn ground as an obstacle nobody could see.
    const double sw=g[low+size_t(ix)],se=g[low+size_t(ix)+1],nw=g[high+size_t(ix)],ne=g[high+size_t(ix)+1];
    return u>=v?sw*(1-u)+se*(u-v)+ne*v:sw*(1-v)+ne*u+nw*(v-u);
}

bool World::Impl::onWater(double x,double y,double standing) {
    const auto* t=tile(x,y);
    if(!t||onDeck(*t,x,y)||!(t->ocean||waterCode(*t,x,y)!=0)||onDryStreet(*t,x,y))return false;
    // A bridge's parapet stands between whoever is on it and the river.
    if(!std::isnan(standing)&&(height(x,y,standing)>terrainHeight(*t,x,y)+.5))return false;
    return true;
}

int World::Impl::waterCode(const Loaded& t,double x,double y) const {
    if(t.water.empty())return 0;
    const int row=std::clamp(int((y-t.south)/(t.north-t.south)*t.waterRows),0,t.waterRows-1);
    const int col=std::clamp(int((x-t.west)/(t.east-t.west)*t.waterCols),0,t.waterCols-1);
    return t.water[size_t(row)*size_t(t.waterCols)+size_t(col)];
}

bool World::Impl::navigable(double x,double y) {
    return onWater(x,y);
}

double World::Impl::waterLevel(double x,double y) {
    auto* t=tile(x,y);
    if(!t||t->ocean)return 0.;
    return waterCode(*t,x,y)==1?height(x,y):0.;
}

std::optional<double> World::Impl::waterBed(double x,double y) {
    const auto* t=tile(x,y);
    if(!t||t->ocean||waterCode(*t,x,y)==1)return std::nullopt;
    const int n=t->gridSize;
    const double u=std::clamp((wrap(x)-t->west)/(t->east-t->west),0.,1.)*(n-1);
    const double v=std::clamp((y-t->south)/(t->north-t->south),0.,1.)*(n-1);
    const int ix=std::min(n-2,int(u)),iy=std::min(n-2,int(v));
    const float* g=t->elevation.data()+size_t(iy)*size_t(n)+size_t(ix);
    if(g[0]==0.f&&g[1]==0.f&&g[n]==0.f&&g[n+1]==0.f)return std::nullopt;
    return gridHeight(*t,x,y);
}

bool World::Impl::deepWater(double x,double y,double standing) {
    if(!onWater(x,y,standing))return false;
    auto bed=waterBed(x,y);
    if(const auto* t=tile(x,y);t&&std::isfinite(standing)&&standing>gridHeight(*t,x,y)+.1)
        bed=std::max(bed.value_or(standing),standing);
    return !bed||waterLevel(x,y)-*bed>=playerHeight*kSwimDepth;
}

saida::StaticBodyNode* World::Impl::boxCollider(saida::Node& parent,const std::string& name,
                                            glm::vec3 size,glm::vec3 center) {
    auto body=std::make_unique<saida::StaticBodyNode>();body->setName(name);
    auto shape=std::make_unique<saida::CollisionShapeNode>();
    shape->shapeType=saida::CollisionShapeType::Box;shape->halfExtents=size*.5f;shape->offset=center;
    body->addChild(std::move(shape));return static_cast<saida::StaticBodyNode*>(parent.addChild(std::move(body)));
}

void World::Impl::meshCollider(saida::Node& parent,std::unique_ptr<saida::MeshNode> mesh) {
    auto body=std::make_unique<saida::StaticBodyNode>();body->setName(mesh->name()+" collider");
    auto shape=std::make_unique<saida::CollisionShapeNode>();shape->shapeType=saida::CollisionShapeType::Mesh;
    body->addChild(std::move(shape));body->addChild(std::move(mesh));parent.addChild(std::move(body));
}

void World::Impl::vehicleCollider(saida::Node& node,const VehicleModel& spec) {
    if(!r1::VehicleRuntime::body(node))if(auto* lod=node.getBehaviour<saida::LODGroupBehaviour>())node.removeBehaviour(lod);
    r1::VehicleRuntime::install(node,{float(spec.length),float(spec.width),float(spec.height),float(spec.wheelbase),float(spec.wheelRadius)},spec.name);
    vehicleLod(node);
}

saida::QueryFilter World::Impl::obstacleFilter() const {
    saida::QueryFilter filter;filter.ignoreInner=feet->innerBodyId();
    if(ridingBike())if(auto* body=r1::VehicleRuntime::body(*car))filter.ignore=body->bodyId();
    if(car)if(auto* body=dynamic_cast<saida::CollisionObjectNode*>(car->findByPath("Vehicle collider")))filter.ignore=body->bodyId();
    if(piloting&&plane.node)if(auto* hull=aircraftCollider(plane.node))filter.ignore=hull->bodyId();
    return filter;
}

bool World::Impl::solidAt(double x,double y,double level,float radius) const {
    auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
    const glm::vec3 at(origin.local(ecef(x,y,level)));
    return !physics->overlapSphere(at,radius,obstacleFilter()).empty();
}

bool World::Impl::blocked(double x,double y,double standing) {
    return onWater(x,y,standing)||obstructed(x,y,standing);
}

bool World::Impl::footBlocked(double x,double y,double standing) {
    return deepWater(x,y,standing)||obstructed(x,y,standing);
}

bool World::Impl::obstructed(double x,double y,double standing) {
    const double ground=std::isnan(standing)?height(x,y):standing;
    if(solidAt(x,y,ground+.6)||solidAt(x,y,ground+1.3))return true;
    // A mesh encloses air: an arrival below an opaque roof is not an outdoor
    // spawn, even when its capsule would initially touch no triangles.
    if(std::isnan(standing)||parkingCar) {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
        const glm::vec3 above(origin.local(ecef(x,y,ground+500.)));
        const auto hit=physics->raycast(above,{0,-1,0},500.f,obstacleFilter());
        if(hit.hit&&hit.point.y>origin.local(ecef(x,y,ground)).y+2.)return true;
    }
    return false;
}

double World::Impl::groundAt(double x,double y,double fallback) {
    return tile(x,y)?height(x,y,fallback):fallback;
}

} // namespace r1::world
