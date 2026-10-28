#include "session.hpp"

namespace r1::world {
void World::Impl::loadHumans() {
    std::ifstream input(game/"assets/models/humans/humans.json");
    if(!input)throw std::runtime_error("Missing assets/models/humans/humans.json (python -m r1.humans)");
    input>>humans;
    humanScale=humans.at("scale").get<double>()*kPeopleVehicleResize;
    playerHeight=humans.at("player").at("height").get<double>()*humanScale;
    for(const auto& entry:humans.at("crowd")) {
        HumanKind kind;
        kind.name=entry.at("name");kind.model=entry.at("model");
        crowdAppearances.push_back({entry.at("sex").get<std::string>()[0],
                                    entry.value("skinTone",std::string("light"))});
        const auto& clips=entry.at("clips");
        for(const char* clip:{"idle","walk","wait","phone","talk","sit","run"})
            if(!clips.contains(clip))throw std::runtime_error("Crowd avatar "+kind.name+" has no clip "+clip);
        // Drawn at `scale`, a clip covers ground at its captured speed
        // times the scale, and a walker moving any faster would skate.
        kind.pace.walk=clips.at("walk").at("speed").get<double>()*humanScale;
        kind.pace.run=clips.at("run").at("speed").get<double>()*humanScale;
        // A seated pelvis sits about 14 cm above the seat it was captured
        // on (a 45 cm chair under a 59 cm pelvis), at the avatar's scale.
        kind.seatDrop=(entry.at("seat").at("pelvisHeight").get<double>()-.14)*humanScale;
        crowdKinds.push_back(std::move(kind));
    }
    if(crowdKinds.empty())throw std::runtime_error("humans.json lists no crowd");
    std::ifstream countries(game/"assets/world/countries.geojson");
    if(!countries)throw std::runtime_error("Missing assets/world/countries.geojson");
    json boundaries;countries>>boundaries;
    countryCrowd.load(boundaries);
    saida::Log::info("[World crowd] ",crowdKinds.size()," avatars, player ",playerHeight,
                     " m, shared vertices=",humans.value("sharedVertices",0));
}

void World::Impl::playPlayerClip(const char* clip) {
    if(std::strcmp(clip,"run")!=0&&std::strcmp(clip,"sprint")!=0) {
        for(auto* a:animators)a->play(clip);
        return;
    }
    saida::ClipView view;
    view.name=clip;view.source=clip;
    view.speed=float(1./kPeopleVehicleResize);
    for(auto* a:animators)a->playView(view);
}

World::Impl::HumanKind* World::Impl::humanKind(size_t index) {
    HumanKind& kind=crowdKinds[index%crowdKinds.size()];
    if(kind.ready)return kind.rig?&kind:nullptr;
    saida::Node* source=prototype(kind.model);
    if(!source)return nullptr;
    kind.ready=true;
    std::vector<saida::Animator*> animators;
    source->findBehavioursInChildren(animators);
    if(animators.empty()||!animators.front()->rig()) {
        saida::Log::error("[World crowd] avatar has no skeleton: ",kind.model);return nullptr;
    }
    kind.rig=const_cast<saida::Rig*>(animators.front()->rig());
    for(const auto& [name,clip]:animators.front()->clips())kind.clips.push_back({name,clip});
    std::function<void(saida::Node&,const saida::Transform&,const std::string&)> walk=
        [&](saida::Node& n,const saida::Transform& above,const std::string& level) {
            saida::Transform here;
            here.rotation=above.rotation*n.transform().rotation;
            here.scale=above.scale*n.transform().scale;
            here.position=above.position+above.rotation*(above.scale*n.transform().position);
            const std::string mine=n.name()=="Near"||n.name()=="Far"?n.name():level;
            if(n.mesh()&&!mine.empty()) {
                auto mesh=std::make_unique<saida::MeshNode>(n.name(),n.mesh(),n.material());
                mesh->transform()=here;
                auto found=std::find_if(kind.levels.begin(),kind.levels.end(),[&](const auto& l){return l.first==mine;});
                if(found==kind.levels.end()){kind.levels.emplace_back(mine,std::vector<std::unique_ptr<saida::MeshNode>>{});found=kind.levels.end()-1;}
                found->second.push_back(std::move(mesh));
            }
            for(auto& c:n.children())walk(*c,here,mine);
        };
    walk(*source,saida::Transform{},"");
    if(kind.levels.size()!=2) {
        saida::Log::error("[World crowd] ",kind.model," has ",kind.levels.size()," levels of detail, not Near and Far");
        kind.rig=nullptr;return nullptr;
    }
    return &kind;
}

saida::GazeModifier::Settings World::Impl::gazeSettings() {
    saida::GazeModifier::Settings s;
    s.chain={{"Bip01 Spine2",.15f,.25f},{"Bip01 Neck",.35f,.45f},{"Bip01 Head",1.f,.8f},
             {"Bip01 LEye",1.f,.35f},{"Bip01 REye",1.f,.35f}};
    s.maxYaw=1.45f;s.maxPitchUp=.4f;s.maxPitchDown=.6f;s.response=5.f;
    return s;
}

saida::ImpactModifier::Settings World::Impl::impactSettings() {
    saida::ImpactModifier::Settings s;
    s.chain={{"Bip01 Spine",.3f},{"Bip01 Spine1",.25f},{"Bip01 Spine2",.2f},{"Bip01 Neck",.1f},{"Bip01 Head",.15f}};
    s.frequency=1.4f;s.damping=.4f;s.maxAngle=.55f;
    return s;
}

bool World::Impl::makePerson(Loaded& tile,size_t slot,Person& person) {
    return makePerson(*tile.node,tile.humanKinds[slot%tile.humanKinds.size()],person,
                      slot<tile.crowd.walkers().size()&&tile.crowd.walkers()[slot].escapedVehicle);
}

bool World::Impl::makePerson(saida::Node& parent,size_t selected,Person& person,bool controlled) {
    HumanKind* kind=humanKind(selected);
    if(!kind)return false;
    person.kind=selected;
    std::unique_ptr<saida::Node> root;
    if(controlled)root=std::make_unique<saida::CharacterBodyNode>();
    else root=std::make_unique<saida::Node>("person-"+kind->name);
    auto* animator=root->addBehaviour<saida::Animator>();
    animator->setRig(kind->rig);
    for(const auto& [name,clip]:kind->clips)animator->addClip(name,clip);
    person.gaze=animator->addModifier<saida::GazeModifier>(*kind->rig,gazeSettings());
    person.impact=animator->addModifier<saida::ImpactModifier>(*kind->rig,impactSettings());
    if(!person.gaze->valid()||!person.impact->valid()) {
        if(bodiesRefused++==0)
            saida::Log::error("[World crowd] ",kind->model," lacks the spine, neck, head or eye bones the gaze and the "
                              "stagger turn (python -m r1.humans keeps them): its people will neither look nor stagger");
    }
    // Their arms for a fight, and their fall: built when first needed
    // (RagdollBehaviour), nothing simulated until someone is knocked down.
    person.arms[0]=animator->addModifier<saida::TwoBoneIKModifier>(*kind->rig,r1::rocketboxArm(false));
    person.arms[1]=animator->addModifier<saida::TwoBoneIKModifier>(*kind->rig,r1::rocketboxArm(true));
    auto* ragdoll=root->addBehaviour<saida::RagdollBehaviour>(ragdollProfile);
    for(const auto& [level,meshes]:kind->levels) {
        auto holder=std::make_unique<saida::Node>(level);
        for(const auto& m:meshes) {
            auto mesh=std::make_unique<saida::MeshNode>(m->name(),m->mesh(),m->material());
            mesh->transform()=m->transform();
            holder->addChild(std::move(mesh));
        }
        root->addChild(std::move(holder));
    }
    // Near while a person stands taller than about 3.5% of the screen,
    // some 35 m away; the far model's 500 triangles beyond.
    root->addBehaviour<saida::LODGroupBehaviour>()->setLevels({{"Near",.035f},{"Far",0.f}});
    // Their body in the engine's physics: a kinematic capsule that follows
    // the node wherever the crowd moves it, and that the player's feet
    // meet (World::Impl::moveFeet). In the node's units, drawn at scale.
    auto body=std::make_unique<saida::RigidBodyNode>();
    body->kinematic=true;
    body->transform().position=glm::vec3(0.f,float(playerHeight*.5/humanScale),0.f);
    auto capsule=std::make_unique<saida::CollisionShapeNode>();
    capsule->shapeType=saida::CollisionShapeType::Capsule;
    capsule->radius=float(r1::Crowd::kBodyRadius/humanScale);
    capsule->height=float(playerHeight/humanScale);
    capsule->axis=1;
    if(controlled) {
        capsule->offset=body->transform().position;
        root->addChild(std::move(capsule));
        person.body=static_cast<saida::CharacterBodyNode*>(root.get());
    } else {
        body->addChild(std::move(capsule));
        person.body=static_cast<saida::RigidBodyNode*>(root->addChild(std::move(body)));
    }
    person.fall.bind(*ragdoll,*person.body,r1::combat::kLieSeconds);
    root->transform().scale=glm::vec3(float(humanScale));
    person.node=parent.addChild(std::move(root));
    person.animator=animator;
    return true;
}

void World::Impl::readCrowd(Loaded& tile) {
    tile.walks=r1::WalkGraph::from(tile.data.value("crowd",json()));
    const uint32_t seed=uint32_t(std::hash<std::string>{}(tile.data.at("key").get<std::string>()))|1u;
    const r1::Country country=countryCrowd.at(tile.data.at("lon").get<double>(),tile.data.at("lat").get<double>());
    tile.humanKinds.clear();
    std::vector<r1::CrowdPace> paces;
    for(size_t slot=0;slot<kCrowdPeople;++slot) {
        const size_t selected=countryCrowd.choose(country,crowdAppearances,seed,slot);
        tile.humanKinds.push_back(selected);
        paces.push_back(crowdKinds[selected].pace);
    }
    tile.crowd.reset(&tile.walks,seed,std::move(paces));
    for(const auto& spot:tile.data.value("beachSpots",json::array())) {
        const auto& a=spot.at("towel");const auto& b=spot.at("water");
        tile.beachSpots.push_back({{a[0],a[1],a[2]},{b[0],b[1],b[2]},spot.value("parasol",false)});
    }
    if(!tile.beachSpots.empty())tile.beachVisitors.resize(12);
}

double World::Impl::localSolarHour() {
    double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
    return std::fmod(std::fmod(unixSeconds/3600.+lon/15.,24.)+24.,24.);
}

double World::Impl::crowdFactor() {
    double factor=r1::crowdHourFactor(localSolarHour());
    // Rain and falling snow send people indoors, about half of them.
    const auto weather=effectiveWeather();
    if(weather.known&&(weather.rain>.5||weather.snowfall>.3))factor*=.5;
    return factor;
}

void World::Impl::updateCrowd(float delta) {
    SAIDA_PROFILE_SCOPE("World/Crowd");
    if(crowdKinds.empty())return;
    size_t remaining=kCrowdPeople-beachLive;
    const double factor=crowdFactor();
    for(auto tile:ring) {
        auto found=loaded.find(tile.key());
        if(found==loaded.end())continue;
        Loaded& l=found->second;
        if(l.walks.links.empty()&&l.crowd.live()==0)continue;
        const glm::dvec3 eye=cameraTileLocal(l);
        if(std::hypot(eye.x-l.crowdEyeX,eye.z-l.crowdEyeZ)>=8.) {
            l.crowdEyeX=eye.x;l.crowdEyeZ=eye.z;
            l.crowdLocalPopulation=l.walks.populationNear(eye.x,eye.z,r1::Crowd::kSpawnFar);
        }
        const size_t normal=std::min<size_t>(size_t(std::lround(l.crowdLocalPopulation*factor)),remaining);
        const size_t share=l.reducedDensity?normal/4:normal;
        remaining-=share;
        l.crowd.setPopulation(int(share));
        r1::Crowd::Scene scene;
        scene.moveEscape=[&l](size_t slot,r1::P3 velocity,double dt) {
            const auto& w=l.crowd.walkers()[slot];
            if(slot>=l.people.size()||!l.people[slot].node)return r1::P3{w.x,w.y,w.z};
            auto* body=static_cast<saida::CharacterBodyNode*>(l.people[slot].body);
            const auto worldVelocity=glm::mat3(l.node->worldTransform())*glm::vec3(velocity.x,velocity.y,velocity.z);
            const auto end=body->moveAndSlide(worldVelocity,float(dt));
            const auto local=glm::inverse(l.node->worldTransform())*glm::vec4(end,1);
            return r1::P3{local.x,local.y,local.z};
        };
        scene.crossingAllowed=[this,&l](size_t link) {
            const auto& crossing=l.walks.links[link];
            const auto& start=l.walks.nodes[crossing.a];const auto& end=l.walks.nodes[crossing.b];
            const glm::vec2 middle(float((start.x+end.x)*.5),float((start.z+end.z)*.5));
            constexpr float kControlledCrossingDistance=35.f;
            float nearest=kControlledCrossingDistance;int signal=-1;
            for(size_t i=0;i<l.signalPositions.size();++i) {
                const float distance=glm::distance(middle,l.signalPositions[i]);
                if(distance<nearest){nearest=distance;signal=int(i);}
            }
            if(signal>=0&&!l.graph.signals[size_t(signal)].pedestriansMayStart(trafficTime_))return false;
            for(const auto& car:l.flow.agents())if(car.alive) {
                const auto pose=l.flow.pose(car);
                const float gap=glm::distance(middle,glm::vec2(pose.position.x,pose.position.y));
                constexpr float kCrossingSafetyTime=3.f;
                if(gap<car.length+pose.speed*kCrossingSafetyTime)return false;
            }
            return true;
        };
        scene.eyeX=eye.x;scene.eyeZ=eye.z;
        const glm::vec3 ahead=camera->transform().rotation*glm::vec3(0,0,-1);
        const glm::dvec3 look=glm::transpose(l.frame.basis)*origin.basis*glm::dvec3(ahead.x,ahead.y,ahead.z);
        const double flat=std::hypot(look.x,look.z);
        if(flat>1e-6){scene.faceX=look.x/flat;scene.faceZ=look.z/flat;}
        if(driving) {
            const glm::dvec3 p=l.frame.local(ecef(carLon,carLat,carAlt));
            scene.carX=p.x;scene.carZ=p.z;scene.carSpeed=std::abs(carSpeed);
        }
        // The player on foot: where he is, the speed he means (a tile's x
        // is east and its z south, near enough across one tile), and his
        // eyes, which is what people look at.
        glm::dvec3 head(0);
        const bool onFoot=!driving&&!sailing&&!piloting&&!swimming;
        if(onFoot) {
            const glm::dvec3 p=l.frame.local(ecef(lon,lat,alt));
            scene.playerX=p.x;scene.playerZ=p.z;
            scene.playerVX=footEast;scene.playerVZ=-footNorth;
            head=l.frame.local(ecef(lon,lat,alt+jumpOffset+playerHeight*.93));
        }
        l.crowd.update(std::min(.05,double(delta)),scene);
        syncPeople(l,eye,std::min(.05,double(delta)),onFoot?&head:nullptr);
    }
}

glm::dvec3 World::Impl::moveFeet(double east,double north,double dt) {
    const glm::dvec2 intended=onward(lon,lat,east,north);
    if(dt<=0)return {intended,alt};
    // The scene's frame is the origin's: x east, y up, z south, near
    // enough within the 350 m it is rebased at.
    const glm::vec3 from(origin.local(ecef(lon,lat,alt+jumpOffset+.06))),to(origin.local(ecef(intended.x,intended.y,alt+jumpOffset+.06)));
    glm::vec3 velocity=(to-from)/float(dt);
    // The engine sticks supported feet to slopes and steps. A constant
    // downward velocity on supported ground projects into downhill motion.
    velocity.y=jumpOffset<=0&&!feet->isOnFloor()?-2.f:0.f;
    feet->transform().position=from;
    const glm::vec3 end=feet->moveAndSlide(velocity,float(dt));
    // Built by its first physics step; one that never is means he walks
    // through everyone, which is said once and fails the smoke.
    if(feet->physicsWorld())feetUnbuilt=0;
    else if(++feetUnbuilt==30&&!feetRefused) {
        saida::Log::error("[World crowd] the player's feet are no physics body: he walks through people");
        feetRefused=true;
    }
    meetPeople();
    const auto horizontal=onward(lon,lat,end.x-from.x,-(end.z-from.z));
    return {horizontal,alt+end.y-from.y};
}

void World::Impl::meetPeople() {
    for(const auto& contact:feet->contacts()) {
        if(!contact.node)continue;
        if(hitByVehicle(contact))return;
        for(auto tile:ring) {
            auto found=loaded.find(tile.key());
            if(found==loaded.end())continue;
            Loaded& l=found->second;
            for(size_t slot=0;slot<l.people.size();++slot) {
                if(l.people[slot].body!=contact.node)continue;
                // From him toward them (the normal points back at him),
                // in the tile's frame, level.
                glm::dvec3 toward=glm::transpose(l.frame.basis)*origin.basis*glm::dvec3(-glm::vec3(contact.normal));
                toward.y=0;
                const double length=glm::length(toward);
                if(length<1e-6)break;
                toward/=length;
                // How fast they close: his speed toward them, and theirs toward him.
                const double closing=footEast*toward.x-footNorth*toward.z+l.crowd.speedAlong(slot,-toward.x,-toward.z);
                if(l.crowd.bump(slot,toward.x,toward.z,closing))staggerPlayer(l,toward,closing);
                break;
            }
        }
    }
}

void World::Impl::staggerPlayer(Loaded& tile,const glm::dvec3& toward,double speed) {
    const glm::dvec3 back=glm::transpose(origin.basis)*tile.frame.basis*(-toward);
    for(size_t i=0;i<animators.size()&&i<playerImpacts.size();++i) {
        if(!animators[i]->node())continue;
        glm::mat3 body(animators[i]->node()->worldTransform());
        for(int c=0;c<3;++c)body[c]=glm::normalize(body[c]);
        playerImpacts[i]->push(glm::transpose(body)*glm::vec3(back),float(std::min(5.,.6*speed)));
    }
    stagger=std::max(stagger,speed>=r1::Crowd::kShove?.45:.2);
    ++playerBumps;
}

void World::Impl::syncPeople(Loaded& tile,const glm::dvec3& eye,double dt,const glm::dvec3* head) {
    const auto& walkers=tile.crowd.walkers();
    if(tile.people.size()<walkers.size())tile.people.resize(walkers.size());
    for(size_t i=0;i<tile.people.size();++i) {
        Person& p=tile.people[i];
        const bool live=i<walkers.size()&&walkers[i].alive;
        if(!p.node) {
            if(!live)continue;
            if(!makePerson(tile,i,p))continue;
        }
        // Disabled rather than hidden: nobody animates a person who is not
        // there, and their capsule leaves the physics with them.
        if(!live){if(p.node->enabled())p.node->setEnabled(false);p.shown=false;p.punch=-1;continue;}
        if(!p.node->enabled())p.node->setEnabled(true);
        const r1::Walker& w=walkers[i];
        // On the ground, the ragdoll holds the body (World::Impl::knockDown).
        if(personDown(tile,i,p,w,dt))continue;
        // Off the paths -- going for the player, back to a path after a
        // fall -- they walk on the ground itself.
        double y=r1::Crowd::offGraph(w)&&!w.escapedVehicle?groundUnder(tile,w.x,w.z,w.y):w.y;
        if(w.activity==r1::Activity::Sit)y-=crowdKinds[p.kind].seatDrop;
        p.node->transform().position=glm::vec3(float(w.x),float(y),float(w.z));
        // A body turns rather than snaps: round a corner, or round to
        // face the player it is telling off. Someone new stands as placed.
        if(!p.shown){p.yaw=w.heading;p.shown=true;p.bumps=w.bumps;}
        else p.yaw+=std::remainder(w.heading-p.yaw,6.283185307179586)*(1-std::exp(-7.*dt));
        // Modelled facing +Z, like the player's body; this world's forward is -Z.
        const glm::quat facing=glm::angleAxis(float(-p.yaw),glm::vec3(0,1,0))*glm::quat(0,0,1,0);
        p.node->transform().rotation=facing;
        // Walked into: the stagger, along the blow, in the body's own axes.
        if(w.bumps!=p.bumps) {
            if(w.bumps>p.bumps&&p.impact)
                p.impact->push(glm::inverse(facing)*glm::vec3(float(w.bumpX),0.f,float(w.bumpZ)),
                               float(std::min(8.,1.1*w.bumpSpeed)));
            p.bumps=w.bumps;
        }
        // Looking at him: his eyes, in the body's own axes and size.
        if(p.gaze) {
            if(w.look>0&&head)
                p.gaze->lookAt(glm::inverse(facing)*(glm::vec3(*head)-p.node->transform().position)/float(humanScale));
            else p.gaze->release();
        }
        const char* clip=w.activity==r1::Activity::Fight&&w.closing?"run":r1::clipOf(w.activity);
        if(std::strcmp(clip,p.clip)!=0){p.animator->play(clip);p.clip=clip;}
        // Animation LOD: a pose every frame close by; further out the pose
        // is resampled at 15, 8 and 4 Hz and held in between, which costs
        // nothing (Animator::PoseRateMode::Hold).
        const double d=std::hypot(w.x-eye.x,w.z-eye.z);
        const float rate=d<12.?0.f:d<30.?15.f:d<60.?8.f:4.f;
        if(rate!=p.poseRate) {
            p.animator->setPoseRate(rate,saida::Animator::PoseRateMode::Hold);
            p.poseRate=rate;
        }
        personAttack(tile,i,p,w,dt);
    }
}

size_t World::Impl::crowdWanted() const {
    size_t n=0;
    for(const auto& [key,tile]:loaded)n+=size_t(tile.crowd.wanted());
    return n;
}

size_t World::Impl::crowdLive() const {
    size_t n=0;
    for(const auto& [key,tile]:loaded)n+=size_t(tile.crowd.live());
    return n;
}

} // namespace r1::world
