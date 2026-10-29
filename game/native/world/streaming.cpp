#include "session.hpp"

namespace r1::world {
double World::Impl::streamPriority(Tile t,double x,double y,double heading) const {
    const r1::P2 middle=t.gen().center();
    const double east=wrap(middle.x-x)*kMetresPerDegree*std::cos(y*rad);
    const double north=(middle.y-y)*kMetresPerDegree;
    const double ahead=east*std::sin(heading*rad)+north*std::cos(heading*rad);
    return std::hypot(east,north)-.8*std::max(0.,ahead);
}

bool World::Impl::streamMotion(double& heading,double& speed) const {
    const double velocity=driving()?carSpeed:sailing()?boat.speed:piloting()?plane.speed:0.;
    if(!playing||pending||warming||std::abs(velocity)<kStreamAheadMinSpeed)return false;
    heading=wrap((driving()?carYaw:sailing()?boat.yaw:plane.yaw)+(velocity<0?180.:0.));
    speed=std::abs(velocity);
    return true;
}

std::vector<Tile> World::Impl::orderedNearby(double x,double y) const {
    auto tiles=nearby(x,y);
    if(drop.phase==Drop::Zoom) {
        // What the picture shows first, then the closest, so that the
        // kDropTiles the fade waits for are always asked for.
        const auto shown=std::stable_partition(tiles.begin()+1,tiles.end(),[&](Tile t){return dropShows(t);});
        tiles.erase(tiles.begin()+std::max<ptrdiff_t>(shown-tiles.begin(),std::min<ptrdiff_t>(kDropTiles,tiles.size())),tiles.end());
        return tiles;
    }
    double heading=0,speed=0;
    if(streamMotion(heading,speed))
        std::stable_sort(tiles.begin()+1,tiles.end(),[&](Tile a,Tile b){
            return streamPriority(a,x,y,heading)<streamPriority(b,x,y,heading);
        });
    return tiles;
}

void World::Impl::request(double x,double y) {
    refused.clear();
    auto local=orderedNearby(x,y);
    std::vector<std::vector<Tile>> groups{local};
    std::vector<Tile> forecast;
    std::set<Tile> seen(local.begin(),local.end());
    double heading=0,speed=0;
    if(streamMotion(heading,speed)) {
        const double horizon=std::clamp(speed*kStreamAheadSeconds,600.,1500.);
        for(double fraction:{1./3.,2./3.,1.}) {
            const double distance=horizon*fraction;
            const auto q=advance(x,y,std::sin(heading*rad)*distance,
                                        std::cos(heading*rad)*distance);
            auto group=nearby(q.x,q.y);
            groups.push_back(group);
            for(Tile t:group)if(seen.insert(t).second)forecast.push_back(t);
        }
        std::stable_sort(forecast.begin(),forecast.end(),[&](Tile a,Tile b){
            return streamPriority(a,x,y,heading)<streamPriority(b,x,y,heading);
        });
    }
    std::vector<Tile> priority{local.front()};
    auto append=[&](Tile t){
        if(priority.size()<kStreamRequestLimit&&
           std::find(priority.begin(),priority.end(),t)==priority.end())priority.push_back(t);
    };
    for(size_t i=1;i<local.size();++i)append(local[i]);
    for(size_t i=0;i<std::min(kStreamForecastTiles,forecast.size());++i)append(forecast[i]);
    json tiles=json::array(),sourceGroups=json::array();
    for(Tile t:priority)tiles.push_back({t.r,t.c});
    for(const auto& group:groups) {
        json members=json::array();for(Tile t:group)members.push_back({t.r,t.c});
        sourceGroups.push_back(std::move(members));
    }
    json payload={{"tiles",tiles},{"groups",sourceGroups}};
    std::string signature=payload.dump();if(signature==requested)return;
    std::vector<r1::Tile> genTiles;for(Tile t:priority)genTiles.push_back(t.gen());
    std::vector<std::vector<r1::Tile>> genGroups;
    for(const auto& group:groups){genGroups.emplace_back();for(Tile t:group)genGroups.back().push_back(t.gen());}
    const auto admission=service->want(std::move(genTiles),std::move(genGroups));
    for(const auto t:local)if(std::find(admission.accepted.begin(),admission.accepted.end(),t.gen())==admission.accepted.end()) {
        saida::Log::error("[World streaming] local tile request refused: ",t.key());
        text("stream-status","Local terrain request exceeds streaming capacity.");
        if(smoke){testFailed=true;engine.sceneTree().quit();}
        return;
    }
    requested=signature;
}

bool World::Impl::go() {
    ++goCount;
    const auto typed=r1::readDestination(value("latitude"),value("longitude"));
    if(!typed) {
        text("status",typed.refusal);text("destination-status",typed.refusal);
        saida::Log::info("[World] destination refused: '",value("latitude"),"', '",value("longitude"),"': ",typed.refusal);
        return false;
    }
    const double lo=typed.lon,la=typed.lat;
    try {
        select(lo,la);pending=true;
        goStarted=std::chrono::steady_clock::now();poll=1;
        arrivalFrames=arrivalHitches=0;arrivalWorst=0;arrivalSaid=false;
        if(cinematicArrival())startDrop();
        else {request(pickLon,pickLat);text("status",kPreparing);}
        saida::Log::info("[World] destination ",lo,", ",la);
        text("destination-status",std::string());
        return true;
    } catch(const std::exception& e) {
        // Not the coordinates: say what failed rather than blame them.
        const std::string why=std::string("Could not start the trip: ")+e.what();
        text("status",why);text("destination-status",why);
        saida::Log::error("[World] destination ",lo,", ",la," failed: ",e.what());
        return false;
    }
}

void World::Impl::addTrunk(Loaded& l,const json& at) {
    if(!l.trunks) {
        auto body=std::make_unique<saida::StaticBodyNode>();body->setName("Tree trunks");
        l.trunks=static_cast<saida::StaticBodyNode*>(l.node->addChild(std::move(body)));
        l.trunks->setEnabled(debugTrees);
    }
    auto shape=std::make_unique<saida::CollisionShapeNode>();
    shape->shapeType=saida::CollisionShapeType::Box;shape->halfExtents={.23f,2.f,.23f};
    shape->offset={at[0].get<float>(),at[1].get<float>()+2.f,at[2].get<float>()};
    l.trunks->addChild(std::move(shape));
    l.trunks->markDirty();
}

void World::Impl::streamProps() {
    SAIDA_PROFILE_SCOPE("World/StreamProps");
    const auto start=std::chrono::steady_clock::now();
    // Scene/GPU APIs stay on the render thread. Yield between objects;
    // CPU model requests yield; node instantiation retains a soft budget.
    // A replacement gets its props while hidden, so its trees and signs
    // are there the frame it is shown (World::Impl::promoteIncoming).
    const auto order=drop.phase==Drop::Zoom?orderedNearby(drop.goalLon,drop.goalLat):ring;
    for(auto t:order)for(auto* tiles:{&loaded,&incoming}) {
        auto it=tiles->find(t.key());if(it==tiles->end())continue;
        auto& l=it->second;
        try { mountLettering(l); }
        catch(const std::exception& e) {
            l.nextLettering=l.data.value("lettering",json::array()).size();
            saida::Log::error("[World lettering] ",t.key(),": ",e.what());
            text("stream-status","A sign could not be loaded.");
            if(smoke){testFailed=true;engine.sceneTree().quit();return;}
        }
        if(msSince(start)>=kPropImportMs)return;
        while(l.nextProp<l.props.size()) {
            try {
                auto doc=l.props[l.nextProp];
                bool plant=doc.contains("groups")&&std::find(doc["groups"].begin(),doc["groups"].end(),"vegetation")!=doc["groups"].end();
                // This model decodes on the asset worker. Other ready
                // tiles can still request/instantiate their objects now.
                if(!modelsReady(doc))break;
                auto n=plantNode(doc);
                if(!n)throw std::runtime_error("Object import failed");
                if(auto* water=dynamic_cast<saida::WaterNode*>(n.get())) {
                    l.waters.push_back(water);water->setEnabled(debugWater);
                }
                if(plant) {
                    Plant entry;
                    entry.node=n.get();
                    entry.grass=n->isInGroup("grass");
                    entry.tree=n->isInGroup("tree");
                    entry.understory=n->isInGroup("understory");
                    if(entry.tree)n->setEnabled(debugTrees);
                    entry.lod=n->getBehaviour<saida::LODGroupBehaviour>();
                    if(toasterMode&&entry.lod)entry.lod->setFinestLevel(1);
                    l.vegetation.push_back(entry);
                    if(entry.tree)addBirdPerches(l,*n);
                    n->setVisible(false);
                }
                if(doc.contains("groups")&&std::find(doc["groups"].begin(),doc["groups"].end(),"tree")!=doc["groups"].end())
                    addTrunk(l,doc.at("transform").at("position"));
                if(n->isInGroup("landmark")) {
                    auto body=std::make_unique<saida::StaticBodyNode>();body->setName(n->name()+" collider");
                    auto shape=std::make_unique<saida::CollisionShapeNode>();shape->shapeType=saida::CollisionShapeType::Mesh;
                    body->addChild(std::move(shape));body->addChild(std::move(n));l.node->addChild(std::move(body));
                } else l.node->addChild(std::move(n));
            } catch(const std::exception& e) {
                saida::Log::warn("[World streaming] prop failed in ",t.key(),": ",e.what());
                if(smoke){testFailed=true;engine.sceneTree().quit();return;}
            }
            ++l.nextProp;
            if(msSince(start)>=kPropImportMs)return;
        }
    }
}

void World::Impl::shape(Loaded& tile) {
    tile.geography=tile.node->findByPath("Geography");
    tile.grass=dynamic_cast<saida::GrassNode*>(tile.node->findByPath("Grass"));
    for(const auto& child:tile.node->children())
        if(auto* water=dynamic_cast<saida::WaterNode*>(child.get()))tile.waters.push_back(water);
    const auto& prepared=std::any_cast<const PreparedTile&>(tile.served->prepared);
    if(tile.geography)for(size_t i=0;i<prepared.buildingCollisions.size();++i) {
        auto body=std::make_unique<saida::StaticBodyNode>();
        body->setName("Building shell collider "+std::to_string(i));
        auto collision=std::make_unique<saida::CollisionShapeNode>();
        collision->setTriangleMeshData(prepared.buildingCollisions[i]);
        body->addChild(std::move(collision));tile.geography->addChild(std::move(body));
    }
    tile.reducedDensity=densityPolicy.reduced(tile.served->cooked.tile)||
        tile.data.at("vertices").get<size_t>()>r1::DensityPolicy::kThreshold;
    const char* names[]={"Near","Mid","Far"};
    tile.buildings.reserve(tile.served->cooked.buildings.size());
    tile.visualForFootprint.assign(tile.data.at("footprints").size(),-1);
    for(const auto& source:tile.served->cooked.buildings) {
        Loaded::Building b;
        b.node=tile.node->createChild<saida::Node>("Building "+std::to_string(source.id));
        for(size_t level=0;level<3;++level) {
            b.levels[level].root=b.node->createChild<saida::Node>(names[level]);
            b.levels[level].root->setVisible(false);
        }
        b.lod=b.node->addBehaviour<saida::LODGroupBehaviour>();
        b.lod->setLevels({{"Near",.08f},{"Mid",.008f},{"Far",0.f}});
        b.lod->setBounds({{float(source.low.x),float(source.low.y),float(source.low.z)},
                          {float(source.high.x),float(source.high.y),float(source.high.z)}});
        b.lod->setCrossFade(.3f);
        for(int level=0;level<3;++level)b.lod->setLevelReady(level,false);
        if(source.footprint<tile.visualForFootprint.size())tile.visualForFootprint[source.footprint]=int(tile.buildings.size());
        tile.buildings.push_back(std::move(b));
    }
}

void World::Impl::activate(const std::string& key,Loaded& tile) {
    const r1::Tile gen=tile.served->cooked.tile;
    // The tile's own geometry, the one measure the budget is about.
    // Props are scene nodes on shared models (CLAUDE.md rule 5):
    // counting every visible tree's Near mesh put a dozen trees over
    // 120 000 and sent every tile's trees to cards for good.
    densityPolicy.observe(gen,tile.data.at("vertices").get<size_t>());
    tile.reducedDensity=densityPolicy.reduced(gen);
    if(tile.reducedDensity)saida::Log::info("[World density] ",key," crowd=1/4 interiors=blocked");
    ++minimapRevision;
    checkStanding=true;
    tile.footprints=std::any_cast<const PreparedTile&>(tile.served->prepared).footprints;
    unpack(tile);
    readGraph(tile);
    readCrowd(tile);
    readFauna(tile);
    mountAircraft(tile);
    placeTiles();
    reparkIfCovered(key);
}

bool World::Impl::replacementReady(const std::string& key,const Loaded& next) {
    if(!next.uploadError.empty())return false;
    const auto& parts=std::any_cast<const PreparedTile&>(next.served->prepared).parts;
    bool ready=(!next.geography||next.nextPart>=parts.size())&&buildingsReady(next);
    for(auto* mesh:next.partMeshes)ready=ready&&mesh->loaded();
    // As many of its props as the shown version has, or all of them.
    const auto old=loaded.find(key);
    const size_t letters=next.data.value("lettering",json::array()).size();
    if(old!=loaded.end())
        ready=ready&&next.nextProp>=std::min(old->second.nextProp,next.props.size())&&
              next.nextLettering>=std::min(old->second.nextLettering,letters);
    auto* physics=engine.sceneTree().world().physics();
    if(ready&&next.geography&&(!physics||physics->hasRoomForBody()))
        for(const auto& child:next.geography->children())
            if(auto* body=dynamic_cast<saida::CollisionObjectNode*>(child.get()))
                ready=ready&&!body->bodyId().IsInvalid();
    return ready;
}

bool World::Impl::promoteIncoming() {
    bool discarded=false;
    for(auto it=incoming.begin();it!=incoming.end();) {
        auto& next=it->second;
        if(!next.uploadError.empty()||(!replacementReady(it->first,next)&&msSince(next.builtAt)>=kReplaceWaitMs)) {
            failTileUpload(it->first,next,next.uploadError.empty()?"replacement readiness timeout; keeping previous terrain":next.uploadError);
            if(auto old=loaded.find(it->first);old!=loaded.end())old->second.rejectedSerial=next.served->serial;
            next.node->queueFree();it=incoming.erase(it);discarded=true;
        } else ++it;
    }
    std::vector<std::string> due;bool holding=false;double longest=0;
    for(const auto& [key,next]:incoming) {
        const double waited=msSince(next.builtAt);
        if(replacementReady(key,next)){due.push_back(key);longest=std::max(longest,waited);}
        else holding=true;
    }
    if(due.empty()||(holding&&longest<kBatchWaitMs))return discarded;
    for(const auto& key:due) {
        const double waited=msSince(incoming.at(key).builtAt);
        auto old=loaded.extract(key);
        auto handle=incoming.extract(key);
        handle.mapped().node->setVisible(true);
        Loaded& tile=loaded.insert(std::move(handle)).position->second;
        activate(key,tile);
        if(!old.empty()) {
            auto& source=old.mapped();
            for(size_t i=0;i<source.cars.size();++i)if(source.cars[i]&&trafficSlotLive(source,i))
                if(!transferTrafficCar(source,i,tile))preserveTrafficCar(source,i);
            source.node->queueFree();
        }
        saida::Log::info("[World streaming] replaced ",key," after ",int(waited)," ms cook_ms=",
                         tile.served->cooked.cookMs);
    }
    return true;
}

void World::Impl::stream() {
    SAIDA_PROFILE_SCOPE("World/StreamTiles");
    const auto anchor=streamAnchor();
    const double x=anchor.x,y=anchor.y;
    auto want=orderedNearby(x,y);std::set<std::string> wanted;for(auto t:want)wanted.insert(t.key());
    std::set<std::string> keep=wanted;
    // Retain the current tile during an in-progress teleport, so the player
    // does not stand on nothing while the destination cooks. It is kept out
    // of `wanted` on purpose: it is leaving, and charging the destination's
    // budget for it is what made a Paris -> Amsterdam teleport refuse its
    // own ninth tile. The arena has the room -- 841 k for the densest
    // neighbourhood, 107 k for the tile being left, 9 k for every prop
    // model on the planet, inside 1 048 576.
    if(playing)keep.insert(tileAt(lon,lat).key());
    if(playing&&!pending&&!warming)for(const auto& [key,region]:loaded) {
        bool visible=false;
        for(const auto& agent:region.flow.agents())if(agent.alive&&trafficVisible(region,region.flow.pose(agent))){visible=true;break;}
        if(visible)keep.insert(key);
    }
    pauseDepartingAnimalRemains(keep);
    bool removed=false;
    auto stale=[&](const Loaded& t){
        const auto current=service->find(t.served->cooked.tile);
        return current&&current->serial!=t.served->serial;
    };
    for(auto* tiles:{&loaded,&incoming})for(auto it=tiles->begin();it!=tiles->end();) {
        // A replacement cooked over again is dropped and started anew below.
        if(!keep.count(it->first)||(tiles==&incoming&&stale(it->second)))
            {it->second.node->queueFree();it=tiles->erase(it);removed=true;}else ++it;
    }
    if(promoteIncoming())removed=true;
    size_t count=0,indices=0;
    for(auto* tiles:{&loaded,&incoming})for(auto& [k,t]:*tiles) {
        if(wanted.count(k)||(!pending&&!warming&&keep.count(k))) {count+=residentVertices(t.data);indices+=residentIndices(t.data);}
        // Closer representations share the same arena. Include even the
        // departing tile's details until its Far transition releases them.
        const auto [extraV,extraI]=buildingExtraGeometry(t);count+=extraV;indices+=extraI;
    }
    // The far landmarks share the arena with the tiles (gen/landmarks.cpp).
    count+=farVertices()+farPack.vertices+nextFarPack.vertices;
    bool started=false;
    for(auto it=loaded.begin();it!=loaded.end();) {
        // A tile the service cooked again -- offline first, then from real
        // observations -- is replaced. Its new version goes up hidden while
        // the old one is still drawn: taking the old one down first left
        // its streets missing for the frames the new parts took to upload.
        if(!stale(it->second)||incoming.count(it->first)){++it;continue;}
        const auto current=service->find(it->second.served->cooked.tile);
        if(current->serial==it->second.rejectedSerial){++it;continue;}
        const json& data=current->cooked.manifest;
        const size_t vertices=residentVertices(data),tileIndices=residentIndices(data);
        if(count+vertices<=residentVertexBudget&&indices+tileIndices<=residentIndexBudget) {
            try {
                auto* ptr=engine.sceneTree().world().addChild(buildTile(it->first,*current));
                ptr->setVisible(false);
                shape(incoming.try_emplace(it->first,ptr,current).first->second);
                count+=vertices;indices+=tileIndices;
                started=true;++it;
            } catch(const std::exception& e) {
                // The mount below meets the same refusal and reports it.
                saida::Log::warn("[World streaming] replacement of ",it->first," refused: ",e.what());
                if(auto partial=incoming.find(it->first);partial!=incoming.end()) {
                    partial->second.node->queueFree();incoming.erase(partial);removed=true;
                }
                it->second.rejectedSerial=current->serial;
                text("stream-status",std::string("Terrain replacement refused: ")+e.what());
                if(smoke){testFailed=true;engine.sceneTree().quit();return;}
                ++it;
            }
        } else {
            saida::Log::warn("[World streaming] ",it->first,
                " replacement exceeds resident budget; keeping previous terrain");
            text("stream-status","Terrain update exceeds available geometry memory; previous terrain retained.");
            it->second.rejectedSerial=current->serial;
            if(smoke){testFailed=true;engine.sceneTree().quit();return;}
            ++it;
        }
    }
    if(removed){++minimapRevision;trim();}
    if(started)placeTiles();
    const auto tick=std::chrono::steady_clock::now();
    for(auto t:want) {
        if(loaded.count(t.key()))continue;
        auto served=service->find(t.gen());
        if(!served)continue;
        const json& data=served->cooked.manifest;
        try {
            const size_t vertices=residentVertices(data),tileIndices=residentIndices(data);
            if(count+vertices>residentVertexBudget||indices+tileIndices>residentIndexBudget) {
                text("stream-status","Detail limit reached in this area.");
                if(refused.empty()) {
                    refused=t.key();
                    // Loud on purpose. A skipped tile used to be a UI string
                    // and nothing else, so a neighbourhood that did not fit
                    // looked exactly like a slow download -- the worst shape
                    // a failure can take (CLAUDE.md rule 3).
                    saida::Log::warn("[World] ",t.key()," does not fit: ",count,
                                     " + ",vertices," of ",residentVertexBudget," resident vertices, ",
                                     indices," + ",tileIndices," of ",residentIndexBudget," indices");
                }
                continue;
            }
            const auto mountStarted=std::chrono::steady_clock::now();
            auto* ptr=engine.sceneTree().world().addChild(buildTile(t.key(),*served));
            auto [entry,inserted]=loaded.try_emplace(t.key(),ptr,served);
            shape(entry->second);
            // Counted at once: several tiles mount in one tick, and each
            // must see the ones mounted before it.
            count+=vertices;indices+=tileIndices;
            activate(t.key(),entry->second);
            lastMountMs=msSince(mountStarted);
            const auto arena=engine.resources().geometryUsage();
            saida::Log::info("[World streaming] mounted ",t.key()," mount_ms=",lastMountMs,
                             " arena=",arena.vertices,"v/",arena.indices,"i (largest free ",
                             arena.largestFreeVertices,"v/",arena.largestFreeIndices,"i)",
                             " cook_ms=",served->cooked.cookMs," since_go_ms=",
                             msSince(goStarted)," private_mb=",int(privateMegabytes()));
        }catch(const std::exception& e){
            text("status",std::string("Tile unavailable: ")+e.what());
            saida::Log::error("[World streaming] mount failed for ",t.key(),": ",e.what());
            if(smoke){testFailed=true;engine.sceneTree().quit();return;}
        }
        // Mounting is cheap now that the parts go up over the next frames;
        // several tiles fit in one tick, as long as the tick stays short.
        if(msSince(tick)>=kMountTickMs)break;
    }
    bool surroundingReady=std::all_of(want.begin(),want.end(),[&](Tile t){return loaded.count(t.key())>0;});
    if(pending && refused==tileAt(pickLon,pickLat).key() && !smoke) {
        // Not transient: the arena is a fixed size and this neighbourhood
        // does not fit in it. Saying so and handing the map back is a worse
        // outcome than spawning and a far better one than a counter that
        // never moves -- and it is the truth (§3 I5).
        pending=false;warming=false;showMap(true);
        text("status","This neighbourhood exceeds the available geometry memory. Pick a nearby spot.");
        return;
    }
    auto ready=[&](double x,double y){auto* t=tile(x,y);if(!t||!t->uploadError.empty())return false;
        if(t->geography&&t->nextPart<std::any_cast<const PreparedTile&>(t->served->prepared).parts.size())return false;
        if(!buildingsReady(*t))return false;
        for(auto* mesh:t->partMeshes)if(!mesh->loaded())return false;
        return !t->geography||(!t->geography->children().empty()&&
            !static_cast<saida::CollisionObjectNode*>(t->geography->children().back().get())->bodyId().IsInvalid());};
    if(pending && ready(pickLon,pickLat)) {
        const bool waterSpawn=deepWater(pickLon,pickLat);
        double x0=pickLon,y0=pickLat;bool found=waterSpawn||!footBlocked(x0,y0);
        for(int i=1;!found&&i<=160;++i) {
            double a=i*2.39996323,d=2.*std::sqrt(double(i));auto q=advance(pickLon,pickLat,d*cos(a),d*sin(a));
            if(ready(q.x,q.y)&&!blocked(q.x,q.y)){x0=q.x;y0=q.y;found=true;}
        }
        if(!found) {
            // A safe point can lie just across the tile boundary. Only
            // that exceptional spawn waits for the rest of the search area.
            if(!surroundingReady && refused.empty())return;
            pending=false;
            text("status","This point is inside a building. Pick a nearby street.");
            // Third time this shape of bug appears in this file, so it is
            // written down rather than rediscovered: a spawn that is
            // *refused* used to leave the E2E waiting for one that would
            // never come, and report it three minutes later as a data
            // timeout. The refusal is the answer; it has to be said out
            // loud, and the test has to fail on the reason.
            saida::Log::warn("[World] no standable ground within 25 m of ",
                             pickLon,", ",pickLat," (buildings)");
            if(smoke) {
                saida::Log::error("[World E2E] FAIL spawn refused: inside a building");
                testFailed=true;engine.sceneTree().quit();
            }
            // Said where the player looks: the arrival hands the map back.
            if(drop.phase!=Drop::None)showMap(true);
            return;
        }
        if(drop.phase==Drop::Zoom&&!drop.armed){armDrop(x0,y0,waterSpawn);return;}
        lon=x0;lat=y0;alt=waterSpawn?waterLevel(lon,lat):height(lon,lat);
        conditions=json::object();conditionsDoc.reset();observedWeather=Weather{};clearRain();
        origin=Frame(lon,lat,alt);placeTiles();moveSun();
        jumpOffset=jumpVelocity=0;followDistance=kOnFootFollow;wasJump=false;
        // Teleporting leaves the current vehicle. A water arrival starts
        // swimming at the selected coordinate, including in the open sea.
        // Carry the vehicle's view into the walking heading, just as a
        // normal exit does: the camera and movement must share one yaw.
        yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
        transitionTravel(waterSpawn?TravelMode::Swim:TravelMode::Foot,false);
        clearBoats();
        clearAircraft();
        parkCar();
        clearPolice();playing=true;pending=false;warming=false;showMap(false);request(lon,lat);
        saida::Log::info("[World] spawned ",lon,", ",lat," altitude=",alt,
                         waterSpawn?" swimming":onWater(lon,lat)?" on foot, wading in "+
                             std::to_string(waterLevel(lon,lat)-alt)+" m of water":" on foot",
                         tile(lon,lat)->data.value("provisional",false)?" (ground first, OSM on its way)"
                         :tile(lon,lat)->data.value("offlineApproximation",false)
                             ?" (simplified offline terrain)":"");
        saida::Log::info("[World streaming] go_to_play_ms=",
            msSince(goStarted),
            " resident=",loaded.size()," target=",want.size());
        if(smoke){
            if(smokeArrivalHeading) {
                if(std::abs(wrap(yaw-*smokeArrivalHeading))>1e-6||lookYaw!=0||lookIdle!=0) {
                    saida::Log::error("[World E2E] FAIL teleport retained a separate vehicle view heading");
                    testFailed=true;engine.sceneTree().quit();return;
                }
                smokeArrivalHeading.reset();
                saida::Log::info("[World E2E] PASS vehicle side view becomes the walking heading after teleport");
            }
            // A test driver must choose a free direction, like a human:
            // walking straight into the nearest real wall is not a failure.
            double best=0;
            for(int direction=0;direction<32;++direction){
                double heading=direction*11.25,clear=clearAhead(lon,lat,alt,heading,.5,12.);
                if(clear>best){best=clear;yaw=heading;}
            }
            if(!tellSun()){saida::Log::error("[World E2E] FAIL the sun cycle did not take the observer");testFailed=true;engine.sceneTree().quit();return;}
            saida::Log::info("[World E2E] sun follows the observer");
            smokeWaterSpawn=waterSpawn&&!smokeSail&&worldCapture.pngPath.empty();
            smokeStarted=!smokeSail&&!smokeFly&&!waterSpawn;
            smokeSailWait=smokeSail;smokeSailTime=0;
            smokeFlyWait=smokeFly;smokeFlyTime=0;smokeFlyPhase=0;
            smokeWalk=0;smokeStart=ecef(lon,lat,alt);smokeRan=smokeJumped=false;
            saida::Log::info("[World E2E] spawn complete with ",loaded.size()," tiles resident");
        }
    }
    if(pending) {
        const auto status=service->status();
        if(status.outOfMemory)text("status","Memory full: loading slowed, retrying automatically. Closing other applications helps.");
        else if(status.memoryPaused)text("status","Loading is waiting for free memory.");
        else if(!status.error.empty())text("status","Loading error, retrying automatically. "+status.error);
        else if(status.offline)text("status","A source is not responding: showing the data already received.");
        else text("status",kPreparing);
    }
}

} // namespace r1::world
