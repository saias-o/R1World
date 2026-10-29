#include "session.hpp"

namespace r1::world {
void World::Impl::updateInteriors(double dt) {
    SAIDA_PROFILE_SCOPE("World/Interiors");
    struct Candidate { Loaded* tile; LiveInterior* room; double distance; };
    std::vector<Candidate> candidates;bool released=false;size_t active=0;
    const auto playerEcef=ecef(lon,lat,alt);
    for(auto& [key,t]:loaded) {
      const auto q=t.frame.local(playerEcef);const r1::P2 at{q.x,q.z};
      for(auto& room:t.interiors) {
        const double distance=room.contains(at)?0:r1::dist(at,room.plan.door);
        if(room.node&&!room.ready) {
            bool ready=true;
            room.node->traverse([&](saida::Node& node,const glm::mat4&) {
                if(auto* mesh=node.mesh())ready=ready&&mesh->loaded();
            });
            if(ready&&nearBuildingReady(t,room.plan.footprint)) {
                room.ready=true;room.node->setEnabled(true);
                saida::Log::info("[World interiors] ready ",room.plan.name);
            } else if(!ready&&engine.resources().assetLoadsSettled()) {
                room.node->queueFree();room.node=nullptr;room.leaves[0]=room.leaves[1]=nullptr;
                room.refused=true;released=true;
                saida::Log::error("[World interiors] geometry upload failed: ",room.plan.name);
                text("stream-status","This interior could not be loaded.");
                if(smoke){testFailed=true;engine.sceneTree().quit();return;}
            }
        }
        if(!room.ready&&distance<kClosedDoorNear&&!room.closedDoor) {
            const auto& p=room.plan;
            room.closedDoor=boxCollider(*t.node,"Unloaded interior door",{float(p.width),2.5f,.10f},{0,1.25f,0});
            room.closedDoor->transform().position={float(p.door.x),float(p.floor),float(p.door.y)};
            room.closedDoor->transform().rotation=glm::angleAxis(float(-std::atan2(p.along.y,p.along.x)),glm::vec3(0,1,0));
        }
        if(room.closedDoor&&((room.node&&room.ready)||distance>kClosedDoorFar)) {room.closedDoor->queueFree();room.closedDoor=nullptr;}
        if(distance>kInteriorRelease)room.refused=false; // Retry a capacity refusal on a later visit.
        if(room.node&&distance>kInteriorRelease) {
            room.node->queueFree();room.node=nullptr;room.leaves[0]=room.leaves[1]=nullptr;
            room.layout={};room.opening=0;room.ready=false;released=true;
            room.beasts.clear();room.animals.clear();room.animalsPlaced=false;
        }
        if(room.node)++active;
        if(!t.reducedDensity&&!room.node&&!room.refused&&distance<kInteriorLoad&&
           nearBuildingReady(t,room.plan.footprint))candidates.push_back({&t,&room,distance});
        if(room.node&&room.ready) {
            updateRoomAnimals(room,t,dt);
            auto p=room.plan.local(at);
            // Sensor on both sides, with a hold time and a wide safety zone.
            const bool sensor=!driving()&&!sailing()&&!piloting()&&std::abs(p.x)<room.plan.width/2+1.5&&std::abs(p.y)<3.5;
            room.hold=sensor?1.5:std::max(0.,room.hold-dt);
            room.opening=r1::slideDoor(room.opening,room.hold>0,dt);
            for(int side=0;side<2;++side) {
                const double sign=side?1.:-1.;const auto& plan=room.plan;
                const double angle=room.opening*r1::kPi*.5;
                double u=plan.doorStyle=="sliding"?sign*plan.width*(.25+.5*room.opening):sign*plan.width*(.5-.25*std::cos(angle));
                auto at=plan.point(u,plan.doorStyle=="sliding"?-.025:plan.width*.25*std::sin(angle));
                room.leaves[side]->transform().position={float(at.x),float(room.plan.floor),float(at.y)};
                room.leaves[side]->transform().rotation=glm::angleAxis(float(-std::atan2(plan.along.y,plan.along.x)+(plan.doorStyle=="swing"?sign*angle:0.)),glm::vec3(0,1,0));
            }
        }
      }
    }
    if(released)trim();
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.distance!=b.distance?a.distance<b.distance:a.room->plan.id<b.room->plan.id;});
    if(active>=kInteriorRooms&&!candidates.empty()) {
        LiveInterior* farthest=nullptr;double farDistance=candidates.front().distance+5;
        for(auto& [key,t]:loaded)for(auto& room:t.interiors)if(room.node) {
            auto q=t.frame.local(ecef(lon,lat,alt));r1::P2 at{q.x,q.z};
            if(room.contains(at))continue;
            const double d=r1::dist(at,room.plan.door);
            if(d>farDistance){farDistance=d;farthest=&room;}
        }
        if(farthest) {
            farthest->node->queueFree();farthest->node=nullptr;farthest->leaves[0]=farthest->leaves[1]=nullptr;
            farthest->layout={};farthest->opening=0;farthest->ready=false;--active;trim();
            farthest->beasts.clear();farthest->animals.clear();farthest->animalsPlaced=false;
        }
    }
    if(active>=kInteriorRooms||candidates.empty())return;
    auto& room=*candidates.front().room;auto& t=*candidates.front().tile;const auto& p=room.plan;
    if(p.recipe=="police") {
        const auto label=r1::police::identity(countryCrowd.at(t.data.value("lon",0.),t.data.value("lat",0.)).code,
            p.name.find("Gendar")!=std::string::npos||p.name.find("gendar")!=std::string::npos).label;
        const auto letters=r1::facadeLettering(label,2.2,.23);
        if(letters&&!modelsReady(*letters))return;
    }
    if(p.recipe!="home"&&p.recipe!="warehouse") {
        const auto letters=r1::facadeLettering(p.name,3.,.75);
        if(letters&&!modelsReady(*letters))return;
    }
    SAIDA_PROFILE_SCOPE("World/BuildInterior");
    room.layout=r1::layoutInterior(p);auto parts=r1::buildInteriorShell(p);auto door=r1::buildInteriorDoor(p);
    std::map<std::string,std::vector<r1::MeshPart>> furnishings;
    for(const auto& fixture:room.layout.fixtures) {
        const auto key=r1::interiorFixtureKey(fixture);
        if(!furnishings.count(key))furnishings.emplace(key,r1::buildInteriorFixture(fixture));
    }
    size_t vertices=0,indices=0;for(auto& part:parts){vertices+=part.mesh.vertexCount();indices+=part.mesh.indices.size();}
    for(const auto& [key,prototype]:furnishings)for(const auto& part:prototype){vertices+=part.mesh.vertexCount();indices+=part.mesh.indices.size();}
    for(auto& part:door){vertices+=2*part.mesh.vertexCount();indices+=2*part.mesh.indices.size();}
    const auto used=engine.resources().geometryUsage();const auto cap=engine.resources().geometryCapacity();
    if(vertices>24000||used.vertices+vertices+4096>cap.vertices||used.indices+indices+12000>cap.indices) {
        room.refused=true;saida::Log::warn("[World interiors] arena refused ",p.name," vertices=",vertices);
        text("stream-status","Interior unavailable: geometry budget reached.");return;
    }
    auto root=std::make_unique<saida::Node>("Interior "+std::to_string(p.id));
    auto upload=[&](saida::Node& parent,const std::vector<r1::MeshPart>& list) {
        for(size_t i=0;i<list.size();++i)if(!list[i].mesh.empty()) {
            auto up=uploadOf(list[i],i);auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(
                saida::prepareMesh({std::move(up.vertices),std::move(up.indices)})));
            if(!mesh)throw std::runtime_error("interior mesh allocation refused");
            auto node=std::make_unique<saida::MeshNode>(up.name,mesh,material(list[i].material));
            // The existing opaque roof and exterior walls cast the room's
            // sun shadow. Thin linings and ceiling rails must not cast a
            // second, nearly coincident shadow onto that same surface.
            node->castShadows()=up.name.find("door")!=std::string::npos;
            if(i<3&&up.name!="Interior surface joints")meshCollider(parent,std::move(node));
            else parent.addChild(std::move(node));
        }
    };
    try {
        upload(*root,parts);
        const double yaw=-std::atan2(p.along.y,p.along.x);
        // Stable resource keys let the engine share each prototype across
        // every instance and both active rooms. No furniture mesh is baked
        // repeatedly into a tile or regenerated for every shelf.
        struct SharedPart { saida::Mesh* mesh; saida::Material* paint; bool shadow; };
        std::map<std::string,std::vector<SharedPart>> prototypes;
        for(const auto& [key,prototype]:furnishings)for(size_t i=0;i<prototype.size();++i) {
            if(prototype[i].mesh.empty())continue;
            auto up=uploadOf(prototype[i],i);
            auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(
                saida::prepareMesh({std::move(up.vertices),std::move(up.indices)}),
                "generated/interior-prototypes/v25/"+key+"/"+std::to_string(i)));
            if(!mesh)throw std::runtime_error("furniture prototype allocation refused");
            prototypes[key].push_back({mesh,material(prototype[i].material),i==0||i==2});
        }
        size_t fixtureIndex=0;
        for(const auto& fixture:room.layout.fixtures) {
            if(fixture.height>.05) {
                auto* body=boxCollider(*root,fixture.kind+" collider "+std::to_string(fixtureIndex),
                    {float(fixture.size.x),float(fixture.height),float(fixture.size.y)},
                    {0,float(fixture.height*.5),0});
                const auto at=p.point(fixture.at.x,fixture.at.y);
                body->transform().position={float(at.x),float(p.floor),float(at.y)};
                body->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));
            }
            ++fixtureIndex;
            if(fixture.kind=="vehicle") {
                if(fleet.empty())throw std::runtime_error("garage requires the road vehicle fleet");
                auto instance=clonePlant(*fleet[fixture.variant%std::min<size_t>(5,fleet.size())].prototype);vehicleLod(*instance);
                const auto at=p.point(fixture.at.x,fixture.at.y);
                instance->transform().position={float(at.x),float(p.floor+.04),float(at.y)};
                instance->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));root->addChild(std::move(instance));continue;
            }
            for(const auto& [mesh,paint,shadow]:prototypes.at(r1::interiorFixtureKey(fixture))) {
                auto instance=std::make_unique<saida::MeshNode>(fixture.kind,mesh,paint);
                instance->castShadows()=shadow;
                auto at=p.point(fixture.at.x,fixture.at.y);
                instance->transform().position={float(at.x),float(p.floor),float(at.y)};
                instance->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));
                root->addChild(std::move(instance));
            }
        }
        for(const auto at:p.exteriorVehicles) {
            if(fleet.empty())throw std::runtime_error("garage requires the road vehicle fleet");
            const auto& spec=fleet[size_t(uint64_t(p.id)%std::min<size_t>(5,fleet.size()))];
            auto instance=clonePlant(*spec.prototype);vehicleLod(*instance);vehicleCollider(*instance,spec);
            instance->transform().position={float(at.x),float(at.y+.04),float(at.z)};
            instance->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));root->addChild(std::move(instance));
        }
        for(int side=0;side<2;++side) {
            auto leaf=std::make_unique<saida::Node>(p.doorStyle=="sliding"?"Automatic sliding leaf":"Automatic swing leaf");upload(*leaf,door);
            auto at=p.point((side?1:-1)*p.width*.25,-.025);
            leaf->transform().position={float(at.x),float(p.floor),float(at.y)};
            leaf->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));
            room.leaves[side]=root->addChild(std::move(leaf));
        }
        auto a=p.ring[p.edge],b=p.ring[(p.edge+1)%p.ring.size()];
        const double fasciaHeight=std::clamp(p.ceiling-p.floor-2.8,.3,1.1);
        const double letterHeight=std::min(.75,fasciaHeight-.12);
        if(p.recipe!="home"&&p.recipe!="warehouse")if(auto sign=lettering(p.name,r1::retailInterior(p.recipe)?r1::dist(a,b)-.8:3.,letterHeight,{.65f,.65f,.61f})) {
            const auto center=r1::retailInterior(p.recipe)?r1::P2{(a.x+b.x)/2,(a.y+b.y)/2}:p.door;
            sign->transform().position={float(center.x-p.inward.x*.29),
                float(p.floor+(r1::retailInterior(p.recipe)?2.8+(fasciaHeight-letterHeight)/2:2.22)),float(center.y-p.inward.y*.29)};
            // Glyphs face +Z, the shop's exterior is -inward. Reverse
            // their baseline relative to the polygon's CCW edge.
            sign->transform().rotation=glm::angleAxis(float(yaw+r1::kPi),glm::vec3(0,1,0));
            root->addChild(std::move(sign));
        } else saida::Log::warn("[World interiors] font cannot spell building name: ",p.name);
        if(p.recipe=="police") {
            const auto identity=r1::police::identity(countryCrowd.at(t.data.value("lon",0.),t.data.value("lat",0.)).code,
                p.name.find("Gendar")!=std::string::npos||p.name.find("gendar")!=std::string::npos);
            if(auto sign=lettering(identity.label,2.2,.23,{.65f,.65f,.62f})) {
                sign->transform().position={float(p.door.x-p.inward.x*.32),float(p.floor+2.8),float(p.door.y-p.inward.y*.32)};
                sign->transform().rotation=glm::angleAxis(float(yaw+r1::kPi),glm::vec3(0,1,0));root->addChild(std::move(sign));
            }
        }
        // A few ceiling lights, scoped to this streamed room.
        // A mall's supermarket is lit where it is, not only at the door:
        // over its checkouts and around its mapped position.
        std::vector<r1::P2> lit;
        for(double v:{3.,11.,20.})lit.push_back(p.point(0,v));
        if(!r1::retailInterior(p.recipe))for(size_t i=0;i<room.layout.rooms.size()&&i<8;++i)lit.push_back(r1::centroid(room.layout.rooms[i].ring));
        if(p.anchor) {
            const r1::P2 a=*p.anchor,d=p.door;const double l=r1::dist(a,d);
            const r1::P2 toward{(a.x-d.x)/l,(a.y-d.y)/l},across{-toward.y,toward.x};
            for(double s:{l/2+4,l/2+16})for(double w:{-9.,9.})
                lit.push_back({d.x+toward.x*s+across.x*w,d.y+toward.y*s+across.y*w});
            lit.push_back(a);
        }
        for(const auto at:lit) {
            if(!r1::pointInPolygon(at,p.ring))continue;
            auto light=std::make_unique<saida::LightNode>("Interior ceiling light",saida::LightType::Point);
            light->transform().position={float(at.x),float(p.ceiling-.25),float(at.y)};
            light->color={1.f,.93f,.82f};light->intensity=r1::retailInterior(p.recipe)?3.f:.8f;
            light->range=r1::retailInterior(p.recipe)?12.f:8.f;light->castShadows=false;
            root->addChild(std::move(light));
        }
        root->setEnabled(false);
        room.ready=false;room.node=t.node->addChild(std::move(root));
        planRoomAnimals(room,t);
        saida::Log::info("[World interiors] queued ",p.name," recipe=",p.recipe," vertices=",vertices,
            " furniture_instances=",room.layout.fixtures.size()," prototypes=",furnishings.size());
    } catch(const std::exception& e) {
        root.reset();trim();
        room.refused=true;room.leaves[0]=room.leaves[1]=nullptr;
        saida::Log::error("[World interiors] ",p.name,": ",e.what());
        text("stream-status","This interior could not be loaded.");
    }
}

bool World::Impl::retailTest() const { return smoke&&(std::getenv("R1WORLD_RETAIL_SMOKE")||std::getenv("R1WORLD_INTERIOR_SMOKE")); }

bool World::Impl::retailShot(const char* moment,const r1::InteriorPlan& plan,r1::P2 facing) {
    const char* path=std::getenv("R1WORLD_RETAIL_SHOT");
    const char* at=std::getenv("R1WORLD_RETAIL_SHOT_AT");
    if(!path||!at||std::string(at)!=moment)return false;
    if(facing.x==0&&facing.y==0)facing=plan.inward;
    yaw=std::atan2(facing.x,-facing.y)/rad;
    if(captureQueued)return true;
    saida::CaptureRequest shot;
    shot.pngPath=path;shot.frame=30;shot.fixedStep=1.f/60.f;shot.settleTimeoutFrames=900;
    captureQueued=true;
    saida::Log::info("[World retail E2E] photographing ",plan.name," ",moment," to ",path);
    engine.captureFrameThenExit(shot);
    return true;
}

void World::Impl::runRetailTest(double dt) {
    smokeStarted=false;retailTestTime+=dt;
    if(captureQueued)return;
    auto fail=[&](const char* why){saida::Log::error("[World retail E2E] FAIL ",why);testFailed=true;engine.sceneTree().quit();};
    if(retailTestTime>45){saida::Log::error("[World retail E2E] phase=",retailTestPhase," v=",retailTestV," waiting=",retailTestWait);fail("interior traversal timed out");return;}
    Loaded* owner=nullptr;LiveInterior* found=nullptr;
    const std::string recipe=std::getenv("R1WORLD_INTERIOR_SMOKE")?std::getenv("R1WORLD_INTERIOR_SMOKE"):"";
    for(auto& [key,t]:loaded)for(auto& r:t.interiors)
        if((retailTestId&&r.plan.id==retailTestId)||(!retailTestId&&r.node&&(recipe.empty()?r1::retailInterior(r.plan.recipe):r.plan.recipe==recipe))) {owner=&t;found=&r;break;}
    if(!found||(found->node&&!found->ready))return;
    auto& r=*found;retailTestId=r.plan.id;
    double walkDepth=0;
    for(double v=.5;v<=12;v+=.5) {
        if(!r1::pointInPolygon(r.plan.point(0,v+.4),r.plan.ring))break;
        walkDepth=v;
    }
    if(walkDepth<1.){fail("interior has no walkable entrance aisle");return;}
    auto position=[&](double u,double v){auto p=r.plan.point(u,v);return r1::Anchor::at(owner->data.at("lon"),owner->data.at("lat"),0).toGeodetic(p.x,r.plan.floor,p.y);};
    // A shop near a tile edge has its forecourt in the neighbour, which
    // may still be streaming: wait for it rather than walk off the world.
    auto resident=[&](double v){auto p=position(0,v);retailTestWait="a tile at the shop to stream in";return tile(p.x,p.y)!=nullptr;};
    auto move=[&](double v,bool check){auto p=position(0,v);
        if(check) {
            auto from=owner->frame.local(ecef(lon,lat,alt));
            auto target=owner->frame.local(ecef(p.x,p.y,p.z));
            auto direction=glm::transpose(origin.basis)*owner->frame.basis*(target-from);
            const auto end=moveFeet(direction.x,-direction.z,std::max(.001,dt));
            if(glm::length(ecef(end.x,end.y,alt)-ecef(p.x,p.y,alt))>.18) {
                saida::Log::error("[World retail E2E] stalled v=",v," altitude=",alt," jump=",jumpOffset);
                for(const auto& contact:feet->contacts())if(contact.node)
                    saida::Log::error("[World retail E2E] contact=",contact.node->name()," normal=",contact.normal.x,",",contact.normal.y,",",contact.normal.z);
                return false;
            }
            lon=end.x;lat=end.y;alt=end.z;
        } else {lon=p.x;lat=p.y;}
        if(!check)alt=height(lon,lat,alt);return true;};
    if(retailTestPhase==0) {if(!r.node||!resident(-18)||!resident(walkDepth))return;
        const std::string shotAt=std::getenv("R1WORLD_RETAIL_SHOT_AT")?std::getenv("R1WORLD_RETAIL_SHOT_AT"):"";
        if(shotAt=="room") {
            const r1::InteriorRoom* room=nullptr;
            const char* requestedRoom=std::getenv("R1WORLD_INTERIOR_ROOM");
            for(const auto& candidate:r.layout.rooms) {
                if(!room)room=&candidate;
                if(requestedRoom) {if(candidate.use==requestedRoom){room=&candidate;break;}continue;}
                if((r.plan.recipe=="school"&&candidate.use=="classroom")||
                   (r.plan.recipe=="home"&&candidate.use=="living")||
                   (r.plan.recipe=="garage"&&candidate.use=="garage")){room=&candidate;break;}
            }
            if(!room){fail("no furnished room to photograph");return;}
            auto center=r.plan.local(r1::centroid(room->ring));double front=1e30,back=-1e30;
            for(auto p:room->ring){auto q=r.plan.local(p);front=std::min(front,q.y);back=std::max(back,q.y);}
            auto anchor=r1::Anchor::at(owner->data.at("lon"),owner->data.at("lat"),0);
            auto cameraPoint=r.plan.point(center.x,front+.5),targetPoint=r.plan.point(center.x,back-.5);
            auto cameraGeo=anchor.toGeodetic(cameraPoint.x,r.plan.floor+1.65,cameraPoint.y);
            auto targetGeo=anchor.toGeodetic(targetPoint.x,r.plan.floor+1.2,targetPoint.y);
            auto eye=origin.local(ecef(cameraGeo.x,cameraGeo.y,cameraGeo.z));
            auto aim=origin.local(ecef(targetGeo.x,targetGeo.y,targetGeo.z));
            captureView.set=true;for(int i=0;i<3;++i){captureView.position[i]=eye[i];captureView.target[i]=aim[i];}
            retailShot("room",r.plan);worldCapture.pngPath=std::getenv("R1WORLD_RETAIL_SHOT");return;
        }
        if(shotAt=="outside"){move(-18,false);retailShot("outside",r.plan);return;}
        if(shotAt=="anchor") {
            if(!r.plan.anchor){fail("no supermarket mapped inside this store");return;}
            const r1::P2 a=*r.plan.anchor,d=r.plan.door;const double l=r1::dist(a,d);
            const r1::P2 toward{(a.x-d.x)/l,(a.y-d.y)/l};
            // Just behind the checkout line, on the store's side.
            const auto spot=r1::Anchor::at(owner->data.at("lon"),owner->data.at("lat"),0)
                .toGeodetic(d.x+toward.x*(l/2+6),r.plan.floor,d.y+toward.y*(l/2+6));
            lon=spot.x;lat=spot.y;alt=height(lon,lat,alt);
            retailShot("anchor",r.plan,toward);return;
        }
        move(-3,false);retailTestPhase=1;return;}
    if(retailTestPhase==1) {
        if(retailTestV<2&&(r.opening<.99||!feet->physicsWorld()))return;
        retailTestV=std::min(walkDepth,retailTestV+std::min(dt,.1)*5.);
        if(!move(retailTestV,true)){fail("open doorway or central aisle blocked");return;}
        if(retailTestV>=walkDepth){saida::Log::info("[World retail E2E] entered ",r.plan.name," recipe=",r.plan.recipe," and walked ",walkDepth," m inside");
            if(retailShot("inside",r.plan)){retailTestV=walkDepth;return;}
            retailTestPhase=2;}
        return;
    }
    if(retailTestPhase==2) {
        retailTestV=std::max(-2.,retailTestV-std::min(dt,.1)*5.);
        if(retailTestV<3&&r.opening<.99)return;
        if(!move(retailTestV,true)){fail("return through automatic door blocked");return;}
        if(retailTestV<=-2) {move(-6,false);retailTestPhase=3;}
        return;
    }
    if(retailTestPhase==3) {
        if(r.opening>.001)return;
        // An entrance can be only 30 cm from a facade corner. Probe the
        // middle of its longer opaque span, rather than beyond that edge.
        const double left=r.plan.local(r.plan.ring[r.plan.edge]).x;
        const double right=r.plan.local(r.plan.ring[(r.plan.edge+1)%r.plan.ring.size()]).x;
        const double half=r.plan.width/2;
        const double wallU=(-half-left>right-half)?(left-half)/2:(right+half)/2;
        auto door=position(0,0),wall=position(wallU,0);
        if(!blocked(door.x,door.y,door.z)){fail("closed door not solid");return;}
        if(!blocked(wall.x,wall.y,wall.z)) {
            saida::Log::error("[World retail E2E] wall u=",wallU," edge=",r.plan.edge," floor=",r.plan.floor);
            std::vector<r1::MeshPart> snapshot;
            const auto& prepared=std::any_cast<const PreparedTile&>(owner->served->prepared);
            for(const auto& part:prepared.parts)if(part.prepared)
                snapshot.push_back(generatorPart(part.name,part.prepared->geometry.vertices,part.prepared->geometry.indices));
            for(const auto& shell:prepared.buildingCollisions) {
                std::vector<saida::Vertex> vertices(shell->positions.size());
                for(size_t i=0;i<vertices.size();++i){vertices[i].pos=shell->positions[i];vertices[i].normal={0,1,0};}
                snapshot.push_back(generatorPart("building collision",vertices,shell->indices));
            }
            const auto bytes=r1::writeGlb(snapshot);
            std::ofstream geometry(game/"generated/interior-smoke-failure.glb",std::ios::binary);
            geometry.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
            std::ofstream planFile(game/"generated/interior-smoke-failure.json");planFile<<r.plan.json().dump();
            fail("facade not solid");return;
        }
        // Query presence alone does not prove that the character solver
        // stops at a mesh. Try crossing both closed door and facade.
        auto cannotCross=[&](double u) {
            const glm::dvec3 saved(lon,lat,alt);auto from=position(u,-.65),target=position(u,.65);
            lon=from.x;lat=from.y;alt=from.z;
            auto delta=origin.local(ecef(target.x,target.y,alt))-origin.local(ecef(lon,lat,alt));
            const auto end=moveFeet(delta.x,-delta.z,.1);
            const auto local=owner->frame.local(ecef(end.x,end.y,end.z));
            const bool stopped=r.plan.local({local.x,local.z}).y<.15;
            lon=saved.x;lat=saved.y;alt=saved.z;
            feet->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+.06)));
            return stopped;
        };
        if(!cannotCross(0)||!cannotCross(wallU)){fail("character crossed closed door or facade");return;}
        auto* physics=engine.sceneTree().world().physics();
        for(size_t i=0;i<r.layout.fixtures.size();++i) {
            const auto& fixture=r.layout.fixtures[i];if(fixture.height<=.05)continue;
            auto* body=dynamic_cast<saida::CollisionObjectNode*>(r.node->findByPath(fixture.kind+" collider "+std::to_string(i)));
            if(!body||!body->physicsWorld()){fail("furniture collider not synchronized");return;}
            const auto at=r.plan.point(fixture.at.x,fixture.at.y);
            const glm::vec3 center(owner->node->worldTransform()*glm::vec4(float(at.x),float(r.plan.floor+fixture.height*.5),float(at.y),1));
            const auto hits=physics->overlapSphere(center,.1f,obstacleFilter());
            if(std::find(hits.begin(),hits.end(),body->bodyId())==hits.end()) {
                saida::Log::error("[World retail E2E] fixture=",fixture.kind," index=",i," collider id=",body->bodyId().GetIndexAndSequenceNumber(),
                    " expected=",center.x,",",center.y,",",center.z);
                fail("furniture's drawn center has no matching engine collider");return;
            }
        }
        if(!resident(-95))return;
        move(-95,false);retailTestPhase=4;return;
    }
    if(retailTestPhase==4) {
        if(r.node){fail("interior not released outside radius");return;}
        move(-3,false);retailTestPhase=5;return;
    }
    if(retailTestPhase==5&&r.node&&r.opening>.99) {
        saida::Log::info("[World retail E2E] PASS engine character entry, aisle, exit, door/wall/furniture colliders, eviction and regeneration");
        engine.sceneTree().quit();
    }
}

} // namespace r1::world
