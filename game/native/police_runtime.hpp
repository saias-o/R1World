// Included inside World, like beach_runtime.hpp: actors belong to the game;
// Wanted and routing remain independent of rendering and physics.
struct PoliceOfficer {
    Person person;
    saida::Node* gun=nullptr;
    double health=100, shot=0;
    bool sees=false;
    std::vector<r1::police::Routes::Point> path;
    size_t step=0;
};
struct PoliceUnit {
    saida::Node* car=nullptr;
    std::array<PoliceOfficer,2> officers;
    std::array<saida::LightNode*,2> lamps{};
    r1::police::Identity identity;
    glm::dvec3 home{0}; // ECEF, survives origin changes and tile eviction.
    std::vector<r1::police::Routes::Point> path;
    std::vector<saida::Node*> wheels;
    size_t step=0;
    double routeAge=10, spin=0, stuck=0, age=0, reach=0; // reach: target distance when they got out
    bool dismounted=false, returning=false;
    saida::AudioID siren=saida::kInvalidAudioID;
};
struct PoliceStation {glm::dvec3 at; r1::police::Identity identity;};
r1::police::Wanted wanted;
r1::police::Routes policeRoads,policeWalks;
std::vector<std::pair<std::string,saida::Node*>> policeTiles;
std::vector<PoliceStation> policeStations;
std::array<PoliceUnit,r1::police::kCars> policeUnits;
glm::dvec3 policeLastKnown{0};
int punchPolice=-1;
saida::Connection policeCarImpact;
saida::CollisionObjectNode* policeImpactBody=nullptr;
double policeCollisionAt=-10;
double policeClock=0,policeThink=0,policeSpawn=0,policeArrest=0,policeDriving=0;
bool policeObserved=false;
std::string policeRefusal;
saida::Node* policeHelicopter=nullptr;
saida::LightNode* policeSearchlight=nullptr;
std::vector<saida::Node*> policeMainRotors,policeTailRotors;
double policeRotor=0,policeAirShot=0;
saida::AudioID policeRotorSound=saida::kInvalidAudioID;

static glm::vec3 policeVector(r1::police::Routes::Point p){return {p.x,p.y,p.z};}
static r1::police::Routes::Point policePoint(glm::vec3 p){return {p.x,p.y,p.z};}
glm::vec3 policePlayerPosition()const {
    if(driving&&car)return nodePosition(*car);
    if(piloting&&plane.node)return nodePosition(*plane.node);
    if(playerDown)return downAnchor();
    return player->transform().position;
}
bool policeSight(glm::vec3 from,glm::vec3 to,saida::CollisionObjectNode* observer=nullptr,float range=140) {
    const auto delta=to-from;const float distance=glm::length(delta);
    auto* physics=engine.sceneTree().world().physics();
    if(!physics||distance>range)return false;
    if(distance<.1f)return true;
    saida::QueryFilter filter;
    if(observer){filter.ignore=observer->bodyId();filter.ignoreInner=observer->innerBodyId();}
    const auto hit=physics->raycast(from,delta/distance,distance,filter);
    if(!hit.hit||hit.distance>=distance-.6f)return true;
    auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
    if(node==feet)return true;
    if(driving&&car&&node==r1::VehicleRuntime::body(*car))return true;
    return playerDown&&playerFall.ragdoll()&&playerFall.ragdoll()->ragdoll()&&
        playerFall.ragdoll()->ragdoll()->partOf(hit.body)>=0;
}
void reportCrime(r1::police::Crime crime,glm::vec3 at,bool identified=false) {
    if(wasted||!playing)return;
    const glm::vec3 suspect=policePlayerPosition()+glm::vec3(0,float(playerHeight*.7),0);
    bool witness=identified||policeObserved;
    // A witness must see the offender. Sound scares the street, but a shot
    // behind a wall does not give dispatch the player's hidden coordinates.
    if(!witness)for(const auto& [key,tile]:loaded) {
        const auto& walkers=tile.crowd.walkers();
        for(size_t i=0;i<tile.people.size()&&i<walkers.size();++i) {
            const auto& person=tile.people[i];
            if(!person.node||!person.node->enabled()||!walkers[i].alive||walkers[i].dead)continue;
            const auto eye=nodePosition(*person.node)+glm::vec3(0,float(playerHeight*.9),0);
            if(glm::distance(eye,at)>75)continue;
            if(policeSight(eye,suspect,person.body,85)){witness=true;break;}
        }
        if(witness)break;
    }
    if(!witness)for(auto& unit:policeUnits)if(unit.car) {
        auto* body=r1::VehicleRuntime::body(*unit.car);
        if(policeSight(nodePosition(*unit.car)+glm::vec3(0,1.3f,0),suspect,body)){witness=true;break;}
        for(auto& officer:unit.officers)if(officer.person.node&&officer.health>0&&unit.dismounted&&
            policeSight(nodePosition(*officer.person.node)+glm::vec3(0,float(playerHeight*.9),0),suspect,officer.person.body))witness=true;
    }
    if(!witness)return;
    wanted.report(crime);
    policeLastKnown=origin.origin+origin.basis*glm::dvec3(policePlayerPosition());
    saida::Log::info("[World police] reported offence=",int(crime)," wanted=",wanted.percent,"%");
}

// Rebuilt only when the streamed graph changes or the ENU origin moves.
void policeBuildRoutes() {
    std::vector<std::pair<std::string,saida::Node*>> tiles;
    for(auto& [key,t]:loaded)tiles.emplace_back(key,t.node);
    if(tiles==policeTiles&&!policeRoads.points.empty())return;
    policeTiles=std::move(tiles);policeRoads={};policeWalks={};policeStations.clear();
    auto worldPoint=[](Loaded& t,double x,double y,double z){return policePoint(glm::vec3(t.node->worldTransform()*glm::vec4(float(x),float(y),float(z),1)));};
    for(auto& [key,t]:loaded) {
        std::vector<std::pair<int,int>> ends;
        for(size_t i=0;i<t.graph.lanes.size();++i) {
            const auto& lane=t.graph.lanes[i];const float length=t.graph.laneLength(uint32_t(i));
            const int pieces=std::max(1,int(std::ceil(length/12)));int first=-1,last=-1;
            for(int n=0;n<=pieces;++n) {
                const float f=float(n)/pieces;const auto p=t.flow.positionOf(uint32_t(i),f*length);
                const int id=policeRoads.add(worldPoint(t,p.x,t.groundUp[lane.from]*(1-f)+t.groundUp[lane.to]*f,p.y));
                if(last>=0)policeRoads.link(last,id);else first=id;last=id;
            }
            ends.push_back({first,last});
        }
        for(size_t i=0;i<t.graph.lanes.size();++i) {
            const auto to=t.graph.lanes[i].to;
            for(uint32_t n=0;n<t.graph.outCount(to);++n) {
                const auto next=t.graph.outLane(to,n);
                if(t.graph.allows(uint32_t(i),next))policeRoads.link(ends[i].second,ends[next].first);
            }
        }
        std::vector<int> walk;
        for(const auto& p:t.walks.nodes)walk.push_back(policeWalks.add(worldPoint(t,p.x,p.y,p.z)));
        for(const auto& link:t.walks.links){policeWalks.link(walk[size_t(link.a)],walk[size_t(link.b)]);policeWalks.link(walk[size_t(link.b)],walk[size_t(link.a)]);}
        const auto country=countryCrowd.at(t.data.value("lon",0.),t.data.value("lat",0.)).code;
        for(auto& room:t.interiors)if(room.plan.recipe=="police") {
            const auto& p=room.plan;const auto out=p.point(0,-3.);
            std::string name=p.name;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
            policeStations.push_back({t.frame.origin+t.frame.basis*glm::dvec3(out.x,p.floor,out.y),
                r1::police::identity(country,name.find("gendar")!=std::string::npos)});
        }
    }
    // Match graph seams, never across a different floor. Quantized buckets
    // bound the search; interiors are not used as navigation shortcuts.
    auto join=[](r1::police::Routes& routes,float reach) {
        std::map<std::pair<int,int>,std::vector<int>> cells;
        for(size_t i=0;i<routes.points.size();++i) {
            const auto p=routes.points[i];const int x=int(std::floor(p.x/4)),z=int(std::floor(p.z/4));
            for(int dx=-1;dx<=1;++dx)for(int dz=-1;dz<=1;++dz) {
                const auto found=cells.find({x+dx,z+dz});if(found==cells.end())continue;
                for(int j:found->second)if(std::abs(p.y-routes.points[size_t(j)].y)<.7f&&r1::police::Routes::distance(p,routes.points[size_t(j)])<reach) {
                    routes.link(int(i),j);routes.link(j,int(i));
                }
            }
            cells[{x,z}].push_back(int(i));
        }
    };
    join(policeRoads,1.2f);join(policeWalks,1.5f);
    for(auto& unit:policeUnits){unit.path.clear();unit.routeAge=10;for(auto& officer:unit.officers)officer.path.clear();}
}

saida::Node* policeBox(saida::Node& parent,const std::string& name,glm::vec3 at,glm::vec3 size,glm::vec3 colour) {
    r1::MeshPart part;part.name=name;part.mesh.addBox({0,0,0},{size.x,size.y,size.z});
    auto up=uploadOf(part,0);
    const std::string key="police/"+name+"/"+number(size.x,3)+"/"+number(size.y,3)+"/"+number(size.z,3);
    auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(saida::prepareMesh({std::move(up.vertices),std::move(up.indices)}),key));
    if(!mesh)throw std::runtime_error("Police equipment geometry allocation refused");
    saida::MaterialDesc desc;desc.baseColor=glm::vec4(colour,1);desc.roughness=.45f;
    auto node=std::make_unique<saida::MeshNode>(name,mesh,engine.resources().getMaterial(desc));
    node->transform().position=at;return parent.addChild(std::move(node));
}
void policeLivery(saida::Node& node,const r1::police::Identity& identity,float roof,bool authored) {
    if(!authored) {
        paintCar(node,{.035f,.055f,.11f});
        for(int side:{-1,1})if(auto sign=lettering(identity.label,1.6,.21,{.62f,.62f,.60f})) {
            sign->transform().position={side*.68f,.6f,0};
            sign->transform().rotation=glm::angleAxis(side*float(r1::kPi/2),glm::vec3(0,1,0));node.addChild(std::move(sign));
        }
    }
    policeBox(node,"Emergency light bar",{0,roof,0},{.8f,.09f,.18f},{.025f,.045f,.12f});
}
bool policeCreate(PoliceUnit& unit,glm::vec3 at,const PoliceStation* station) {
    const auto country=countryCrowd.at(lon,lat).code;
    unit.identity=station?station->identity:r1::police::identity(country);
    const auto letters=r1::facadeLettering(unit.identity.label,1.6,.21);
    if(letters&&!modelsReady(*letters))return false;
    const std::string kind=country=="FR"?(unit.identity.label=="GENDARMERIE"?"gendarmerie":"police"):"sedan";
    const auto spec=std::find_if(fleet.begin(),fleet.end(),[&](const auto& item){return item.name==kind;});
    if(spec==fleet.end()||motorcycleRiderKinds.empty())return false;
    auto node=clonePlant(*spec->prototype);vehicleLod(*node);vehicleCollider(*node,*spec);
    policeLivery(*node,unit.identity,float(spec->height*.82),country=="FR");
    auto* equipment=r1::VehicleRuntime::body(*node);
    for(size_t i=0;i<unit.lamps.size();++i) {
        auto* light=equipment->createChild<saida::LightNode>("Emergency beacon",saida::LightType::Point);
        light->transform().position={i?.3f:-.3f,float(spec->height*.82),0};
        light->color=i&&unit.identity.redBlue?glm::vec3(1,.03f,.02f):glm::vec3(.03f,.15f,1);
        light->range=9;light->intensity=0;light->castShadows=false;unit.lamps[i]=light;
    }
    unit.car=engine.sceneTree().world().addChild(std::move(node));unit.car->transform().position=at+glm::vec3(0,.08f,0);
    const int lane=policeRoads.nearest(policePoint(at));
    if(lane>=0&&!policeRoads.edges[size_t(lane)].empty()) {
        const auto next=policeVector(policeRoads.points[size_t(policeRoads.edges[size_t(lane)][0].to)])-at;
        unit.car->transform().rotation=glm::angleAxis(std::atan2(-next.x,-next.z),glm::vec3(0,1,0));
    }
    unit.home=station?station->at:origin.origin+origin.basis*glm::dvec3(at);
    unit.car->traverse([&](saida::Node& n,const glm::mat4&){if(n.name().rfind("wheel-",0)==0)unit.wheels.push_back(&n);});
    for(size_t i=0;i<unit.officers.size();++i) {
        auto& officer=unit.officers[i];const size_t kindIndex=motorcycleRiderKinds[(size_t(policeClock)+i)%motorcycleRiderKinds.size()];
        if(!makePerson(engine.sceneTree().world(),kindIndex,officer.person,true))throw std::runtime_error("Police officer avatar unavailable");
        auto& person=officer.person;person.node->setName(unit.identity.label+" officer");
        // A labelled ballistic vest over the existing scan, not a replacement avatar.
        auto vest=std::make_unique<saida::Node>("Police vest");
        policeBox(*vest,"Duty vest",{0,0,.02f},{.32f,.31f,.20f},{unit.identity.uniformR,unit.identity.uniformG,unit.identity.uniformB});
        if(auto badge=lettering(unit.identity.label,.28,.055,{.65f,.65f,.60f})) {
            badge->transform().position={0,.06f,.125f};vest->addChild(std::move(badge));
        }
        saida::Transform vestAt;vestAt.position={0,0,.01f};
        vest->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 Spine2",vestAt);person.node->addChild(std::move(vest));
        auto cap=std::make_unique<saida::Node>("Police cap");
        policeBox(*cap,"Duty cap",{0,.11f,0},{.18f,.075f,.20f},{unit.identity.uniformR,unit.identity.uniformG,unit.identity.uniformB});
        policeBox(*cap,"Cap visor",{0,.08f,.10f},{.19f,.015f,.14f},{.015f,.018f,.022f});
        cap->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 Head",saida::Transform{});person.node->addChild(std::move(cap));
        saida::Transform grip;auto* human=humanKind(kindIndex);
        if(!human||!r1::pistolGrip(*human->rig,grip))throw std::runtime_error("Police officer cannot hold pistol");
        auto gun=r1::buildPistol(engine.resources());if(!gun)throw std::runtime_error("Police pistol allocation refused");
        gun->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 R Hand",grip);officer.gun=person.node->addChild(std::move(gun));
        person.node->setEnabled(false);
    }
    if(auto* physics=engine.sceneTree().world().physics())r1::VehicleRuntime::activate(*unit.car,*physics,{});
    saida::Log::info("[World police] patrol deployed ",unit.identity.label," at ",at.x,",",at.z);
    return true;
}
void policeRemove(PoliceUnit& unit) {
    saida::AudioManager::get().stop(unit.siren);
    for(auto& officer:unit.officers)if(officer.person.node){officer.person.node->setEnabled(false);officer.person.node->queueFree();}
    if(unit.car){unit.car->setEnabled(false);unit.car->queueFree();}unit={};
}
void clearPolice() {
    wanted.clear();policeObserved=false;policeArrest=0;policeRefusal.clear();
    for(auto& unit:policeUnits)policeRemove(unit);
    if(policeHelicopter){policeHelicopter->setEnabled(false);policeHelicopter->queueFree();policeHelicopter=nullptr;}
    policeSearchlight=nullptr;policeMainRotors.clear();policeTailRotors.clear();
    saida::AudioManager::get().stop(policeRotorSound);policeRotorSound=saida::kInvalidAudioID;
}

bool hitPolice(const saida::RaycastHit& hit,double damage,glm::vec3 direction,bool headshot=true,float impulse=float(r1::combat::kPistol.impulse)) {
    auto* physics=engine.sceneTree().world().physics();if(!physics)return false;
    auto* body=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
    for(auto& unit:policeUnits)if(unit.car) {
        if(body==r1::VehicleRuntime::body(*unit.car)){reportCrime(r1::police::Crime::OfficerAssault,hit.point,true);return false;}
        for(auto& officer:unit.officers) {
            auto& person=officer.person;
            if(body!=person.body&&!(person.fall.ragdoll()&&person.fall.ragdoll()->ragdoll()&&person.fall.ragdoll()->ragdoll()->partOf(hit.body)>=0))continue;
            if(officer.health<=0)return true;
            if(headshot&&hit.point.y-nodePosition(*person.node).y>playerHeight*.83)damage*=r1::combat::kPistol.headMultiplier;
            officer.health=std::max(0.,officer.health-damage);
            reportCrime(officer.health<=0?r1::police::Crime::OfficerKilling:r1::police::Crime::OfficerAssault,hit.point,true);
            effects.burst(r1::CombatEffects::Blood,hit.point);limitRagdolls();
            if(beginFall(person.fall,direction*2.2f+glm::vec3(0,.6f,0),glm::cross(glm::vec3(0,1,0),direction)*2.f,"officer")) {
                person.fall.ragdoll()->applyImpulse(direction*impulse,hit.point);
                for(auto* arm:person.arms)if(arm)arm->release();officer.gun->setVisible(false);
            }
            return true;
        }
    }
    return false;
}

void policeArrestPlayer() {
    glm::dvec3 destination=ecef(lon,lat,alt);double best=1e30;
    for(const auto& station:policeStations){const double d=glm::length(station.at-destination);if(d<best){best=d;policeLastKnown=station.at;}}
    if(best<1e29)destination=policeLastKnown;
    if(driving)leaveCar();
    if(aiming)setAiming(false);
    clearPolice();
    const auto geo=r1::ecefToGeodetic(destination.x,destination.y,destination.z);
    lon=geo.x;lat=geo.y;alt=geo.z;jumpOffset=jumpVelocity=0;
    player->transform().position=glm::vec3(origin.local(destination));request(lon,lat);
    arsenal.disarm();punch={};showHeldPistol();
    cheatSay(best<1e29?"Interpellé. Munitions confisquées, vous êtes relâché au poste.":
                      "Interpellé. Munitions confisquées, vous êtes relâché sur place.");
    saida::Log::info("[World police] arrested, ammunition confiscated");
}

void policeFire(glm::vec3 muzzle,glm::vec3 target,saida::CollisionObjectNode* shooter,float range) {
    const auto direction=glm::normalize(target-muzzle);saida::QueryFilter filter;
    if(shooter){filter.ignore=shooter->bodyId();filter.ignoreInner=shooter->innerBodyId();}
    auto* physics=engine.sceneTree().world().physics();
    const auto hit=physics->raycast(muzzle,direction,range,filter);
    effects.muzzle(muzzle);sounds.play(r1::CombatSounds::Shot,glm::distance(camera->transform().position,muzzle),.7f);
    if(!hit.hit)return;
    auto* victim=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
    const bool ragdoll=playerDown&&playerFall.ragdoll()->ragdoll()&&playerFall.ragdoll()->ragdoll()->partOf(hit.body)>=0;
    if(victim==feet||ragdoll) {
        hurtPlayer(12,direction,shooter?"the police":"the police helicopter");
        if(policeSmoke){if(shooter)policeSmokeGroundHit=true;else policeSmokeAirHit=true;}
    }
    else if(driving&&victim==r1::VehicleRuntime::body(*car)) {
        effects.burst(r1::CombatEffects::Sparks,hit.point);
        if(auto* dent=victim->getBehaviour<saida::DeformableMeshBehaviour>()) {
            const auto local=glm::inverse(victim->worldTransform());
            dent->applyImpact(glm::vec3(local*glm::vec4(hit.point,1)),glm::normalize(glm::vec3(local*glm::vec4(direction,0))),dent->yieldEnergy*3);
        }
    } else effects.burst(r1::CombatEffects::Dust,hit.point);
}

void policeOfficer(PoliceUnit& unit,PoliceOfficer& officer,glm::vec3 target,double dt,bool think) {
    auto& p=officer.person;if(!p.node)return;
    if(!unit.dismounted){p.node->setEnabled(false);officer.sees=false;return;}
    p.node->setEnabled(true);
    auto* character=static_cast<saida::CharacterBodyNode*>(p.body);
    if(p.fall.ragdoll()->state()!=saida::RagdollBehaviour::State::Animated) {
        officer.sees=false;
        p.fall.update(dt,officer.health>0,[&](const glm::vec3& at){return sceneGround(at);});return;
    }
    if(officer.health<=0){officer.sees=false;return;}
    const auto from=nodePosition(*p.node);const auto suspect=policePlayerPosition()+glm::vec3(0,float(playerHeight*.7),0);
    if(think)officer.sees=wanted.percent>0&&policeSight(from+glm::vec3(0,float(playerHeight*.9),0),suspect,p.body);
    if(officer.sees){policeObserved=true;policeLastKnown=origin.origin+origin.basis*glm::dvec3(policePlayerPosition());if(!unit.returning)target=policePlayerPosition();}
    const float distance=glm::distance(from,target);
    const bool shooting=!unit.returning&&wanted.percent>0&&wanted.lethal()&&officer.sees&&distance<65&&!wasted;
    if(think) {
        officer.path=policeWalks.route(policePoint(from),policePoint(target),true);officer.step=0;
        while(officer.step<officer.path.size()&&glm::distance(from,policeVector(officer.path[officer.step]))<1.5f)++officer.step;
    }
    glm::vec3 goal=target;
    if(!officer.sees&&officer.step<officer.path.size())goal=policeVector(officer.path[officer.step]);
    else if(!officer.sees&&officer.path.empty())goal=from;
    glm::vec3 delta=goal-from;delta.y=0;const float gap=glm::length(delta);
    const bool moving=gap>(unit.returning?1.2f:wanted.percent>0?(shooting?14.f:1.25f):1.f);
    const float speed=float(wanted.percent>0?crowdKinds[p.kind].pace.run:crowdKinds[p.kind].pace.walk);
    glm::vec3 velocity(0,-3,0);
    if(moving&&gap>.01f)velocity+=delta/gap*speed;
    const auto position=character->moveAndSlide(velocity,float(dt));
    if(officer.step<officer.path.size()&&glm::distance(position,policeVector(officer.path[officer.step]))<1.5f)++officer.step;
    const auto facing=shooting?suspect-position:delta;
    if(glm::length(glm::vec2(facing.x,facing.z))>.1f)p.node->transform().rotation=glm::angleAxis(std::atan2(facing.x,facing.z),glm::vec3(0,1,0));
    const char* clip=moving?(wanted.percent>0?"run":"walk"):"idle";
    if(std::strcmp(p.clip,clip)){p.animator->play(clip);p.clip=clip;}
    p.animator->setPoseRate(glm::distance(from,camera->transform().position)<35?0.f:8.f,saida::Animator::PoseRateMode::Hold);
    officer.gun->setVisible(shooting);
    if(shooting) {
        const auto direction=glm::normalize(glm::vec3(glm::inverse(p.node->worldTransform())*glm::vec4(suspect-from,0)));
        const auto& pose=p.animator->globalPose().globalMatrices;const auto& rig=*p.animator->rig();
        const int shoulder=rig.findBoneIndex("Bip01 R UpperArm");
        if(shoulder>=0&&pose.size()>size_t(shoulder)) {
            const auto grip=glm::vec3(pose[size_t(shoulder)][3])+direction*playerReach*.90f;
            auto side=glm::cross(glm::vec3(0,1,0),direction);
            if(glm::length(side)<.01f)side={1,0,0};else side=glm::normalize(side);
            glm::mat3 basis;basis[0]=side;basis[1]=glm::cross(direction,side);basis[2]=direction;
            saida::Transform gunGrip;r1::pistolGrip(rig,gunGrip);
            p.arms[1]->reach(grip);p.arms[1]->setTipRotation(glm::quat_cast(basis)*glm::inverse(gunGrip.rotation));
            p.arms[0]->reach(grip-side*.035f-glm::vec3(0,.045f,0));
        }
        officer.shot-=dt;
        if(officer.shot<=0) {
            officer.shot=.9+double((&officer-&unit.officers[0]))*.21;
            const auto muzzle=glm::vec3(officer.gun->worldTransform()*glm::vec4(r1::kPistolMuzzle,1));
            // Spread is geometric: cover and missed shots take the round.
            const auto error=glm::vec3(float(std::sin(policeClock*17+from.x)),float(std::cos(policeClock*11)),0)*std::min(2.f,.12f+distance*.018f);
            policeFire(muzzle,suspect+error,p.body,80);
        }
    } else for(auto* arm:p.arms)if(arm)arm->release();
    if(wanted.percent>0&&!wanted.lethal()&&officer.sees&&distance<1.9f&&(!driving||std::abs(carSpeed)<.5)&&!piloting&&!sailing&&!playerDown&&std::hypot(footEast,footNorth)<.8&&!aiming)
        policeArrest=std::max(policeArrest,dt); // accumulated once per update below
}

void policeAir(double dt,bool think) {
    if(wanted.helicopter&&!policeHelicopter) {
        auto found=aircraftPrototypes.find("helicopter");
        if(found==aircraftPrototypes.end())return;
        const auto label=r1::police::identity(countryCrowd.at(lon,lat).code).label;
        const auto letters=r1::facadeLettering(label,1.6,.21);
        if(letters&&!modelsReady(*letters))return;
        auto node=clonePlant(*found->second);aircraftLod(*node);
        policeLivery(*node,r1::police::identity(countryCrowd.at(lon,lat).code),1.8f,false);
        const auto last=glm::vec3(origin.local(policeLastKnown));
        node->transform().position=last+glm::vec3(110,55,90);
        policeHelicopter=engine.sceneTree().world().addChild(std::move(node));
        policeHelicopter->traverse([&](saida::Node& n,const glm::mat4&){if(n.name()=="rotor-main")policeMainRotors.push_back(&n);if(n.name()=="rotor-tail")policeTailRotors.push_back(&n);});
        policeSearchlight=policeHelicopter->createChild<saida::LightNode>("Police searchlight",saida::LightType::Spot);
        policeSearchlight->direction={0,-1,0};policeSearchlight->range=180;policeSearchlight->spotInnerAngle=12;policeSearchlight->spotOuterAngle=22;
        policeSearchlight->intensity=20;policeSearchlight->castShadows=false;
        saida::Log::info("[World police] 100%: helicopter dispatched");
    }
    if(!policeHelicopter)return;
    auto& pose=policeHelicopter->transform();const auto last=glm::vec3(origin.local(policeLastKnown));
    auto goal=wanted.helicopter?last+glm::vec3(float(std::sin(policeClock*.22)*28),38,float(std::cos(policeClock*.22)*28)):
        pose.position+glm::vec3(100,10,100);
    goal.y=std::max(goal.y,sceneGround(goal)+28.f);
    auto delta=goal-pose.position;const float length=glm::length(delta);
    if(length>.1f) {
        const auto direction=delta/length;const float reach=std::min(length,float(dt*38));
        auto* physics=engine.sceneTree().world().physics();
        const auto hit=physics->raycast(pose.position,direction,reach+16);
        if(hit.hit)pose.position.y+=float(dt*12);else pose.position+=direction*reach;
        pose.rotation=glm::slerp(pose.rotation,glm::angleAxis(std::atan2(-delta.x,-delta.z),glm::vec3(0,1,0)),float(1-std::exp(-dt*2)));
    }
    policeRotor=std::fmod(policeRotor+dt*30,kTwoPi);
    for(auto* rotor:policeMainRotors)rotor->transform().rotation=glm::angleAxis(float(policeRotor),glm::vec3(0,1,0));
    for(auto* rotor:policeTailRotors)rotor->transform().rotation=glm::angleAxis(float(policeRotor*1.6),glm::vec3(1,0,0));
    if(think&&wanted.percent>0&&policeSight(pose.position,policePlayerPosition()+glm::vec3(0,.8f,0),nullptr,240)) {
        policeObserved=true;policeLastKnown=origin.origin+origin.basis*glm::dvec3(policePlayerPosition());
    }
    policeAirShot-=dt;
    const auto suspect=policePlayerPosition()+glm::vec3(0,float(playerHeight*.7),0);
    if(wanted.helicopter&&wanted.lethal()&&!wasted&&policeAirShot<=0&&policeSight(pose.position,suspect,nullptr,140)) {
        policeAirShot=.8;
        const auto muzzle=pose.position+glm::vec3(0,-1.2f,0);
        const float spread=.25f+glm::distance(muzzle,suspect)*.012f;
        policeFire(muzzle,suspect+glm::vec3(float(std::sin(policeClock*17))*spread,0,float(std::cos(policeClock*11))*spread),nullptr,150);
    }
    const auto ray=last-pose.position;
    if(glm::length(ray)>1)policeSearchlight->direction=glm::inverse(pose.rotation)*glm::normalize(ray);
    if(!wanted.helicopter&&glm::distance(pose.position,camera->transform().position)>300) {
        policeHelicopter->queueFree();policeHelicopter=nullptr;policeSearchlight=nullptr;policeMainRotors.clear();policeTailRotors.clear();
    }
}

void updatePolice(double elapsed) {
    SAIDA_PROFILE_SCOPE("World/Police");
    const double dt=std::clamp(elapsed,0.,.1);policeClock+=dt;policeThink+=dt;policeSpawn+=dt;
    auto* playerCarBody=driving&&car?r1::VehicleRuntime::body(*car):nullptr;
    if(playerCarBody!=policeImpactBody||(playerCarBody&&!policeCarImpact.connected())) {
        policeCarImpact.disconnect();policeImpactBody=playerCarBody;
        if(playerCarBody)policeCarImpact=playerCarBody->impacted.connect([this](const saida::CollisionObjectNode::Impact& impact) {
            if(!driving||!car||!impact.other||impact.closingSpeed<3||std::abs(carSpeed)<3||policeClock-policeCollisionAt<2)return;
            auto* body=r1::VehicleRuntime::body(*car);
            const auto velocity=body->physicsWorld()->linearVelocity(body->bodyId());
            if(glm::dot(velocity,-impact.direction)<2)return;
            policeCollisionAt=policeClock;
            reportCrime(r1::police::Crime::Collision,impact.point);
        });
    }
    const bool think=policeThink>=.5;
    if(think){policeThink=0;policeBuildRoutes();policeObserved=false;}
    const auto suspect=policePlayerPosition();
    if(driving&&playerCarBody&&std::abs(carSpeed)>3) {
        const auto toCar=glm::inverse(car->worldTransform());const auto& spec=vehicleSpec(*car);
        const auto velocity=playerCarBody->physicsWorld()?playerCarBody->physicsWorld()->linearVelocity(playerCarBody->bodyId()):glm::vec3(0);
        for(auto& unit:policeUnits)if(unit.dismounted)for(auto& officer:unit.officers) {
            auto& p=officer.person;
            if(!p.node||officer.health<=0||p.fall.ragdoll()->state()!=saida::RagdollBehaviour::State::Animated)continue;
            const auto at=nodePosition(*p.node),local=glm::vec3(toCar*glm::vec4(at,1));
            if(std::abs(local.x)>spec.width*.5+.25||std::abs(local.z)>spec.length*.5+std::abs(carSpeed)*dt||std::abs(local.y)>2)continue;
            if(glm::dot(at-suspect,velocity)<0)continue;
            const auto impact=r1::combat::carImpact(std::abs(carSpeed));
            saida::RaycastHit hit;hit.hit=true;hit.body=p.body->innerBodyId();hit.point=at+glm::vec3(0,.6f,0);
            hitPolice(hit,impact.damage,glm::normalize(velocity+glm::vec3(0,.01f,0)),false,float(impact.damage*3));
        }
    }
    const auto last=glm::vec3(origin.local(policeLastKnown));
    const bool active=wanted.percent>0;
    const auto search=last+glm::vec3(float(std::sin(policeClock*.15)*std::min(70.,wanted.unseen*2)),0,float(std::cos(policeClock*.15)*std::min(70.,wanted.unseen*2)));
    const double arrestBefore=policeArrest;policeArrest=0;
    size_t live=0;
    for(auto& unit:policeUnits)if(unit.car) {
        ++live;unit.age+=dt;unit.routeAge+=dt;r1::VehicleRuntime::synchronize(*unit.car);
        const auto at=nodePosition(*unit.car);auto* body=r1::VehicleRuntime::body(*unit.car);
        auto* drive=r1::VehicleRuntime::driver(*unit.car);
        if(think&&active&&!unit.dismounted&&policeSight(at+glm::vec3(0,1.2f,0),suspect+glm::vec3(0,.8f,0),body)) {
            policeObserved=true;policeLastKnown=origin.origin+origin.basis*glm::dvec3(suspect);
        }
        const auto home=glm::vec3(origin.local(unit.home));
        glm::vec3 target=active?(policeObserved?suspect:search):
            (std::fmod(unit.age,60.)>=30&&std::fmod(unit.age,60.)<50)?home+glm::vec3(float(std::sin(unit.age*.2)*12),0,float(std::cos(unit.age*.2)*12)):
            home+glm::vec3(float(std::sin(unit.age*.04)*130),0,float(std::cos(unit.age*.04)*130));
        const float distance=glm::distance(at,target),speed=std::abs(drive->signedSpeed());
        // The approach route ends at the closest reachable roadside: past it
        // is on foot, however far the suspect stands from the road.
        const bool roadside=!unit.path.empty()&&glm::distance(at,policeVector(unit.path.back()))<7;
        const bool stop=active&&(distance<22||roadside)&&(!driving||std::abs(carSpeed)<2.5);
        const bool noCrew=std::all_of(unit.officers.begin(),unit.officers.end(),[](const auto& o){return o.health<=0;});
        const bool disabled=noCrew||drive->damage>=r1::VehicleRuntime::kMaximumBodyworkPowerLoss;
        const bool footPatrol=!active&&glm::distance(at,home)<30&&std::fmod(unit.age,60.)>=30&&std::fmod(unit.age,60.)<50;
        if(((stop||footPatrol)&&speed<1.5f)||disabled) {
            if(!unit.dismounted) {
                unit.dismounted=true;unit.reach=distance;
                for(size_t i=0;i<unit.officers.size();++i) {
                    auto& p=unit.officers[i].person;
                    p.node->transform().position=at+unit.car->transform().rotation*glm::vec3(i?1.2f:-1.2f,0,.3f);
                }
            }
        }
        // Back aboard when the patrol is over, the suspect drives off, or the
        // target has moved well beyond where they got out (a foot patrol
        // called to a pursuit): the car covers that distance, not their feet.
        unit.returning=unit.dismounted&&!disabled&&((!active&&(std::fmod(unit.age,60.)<30||std::fmod(unit.age,60.)>=50))||
            (driving&&distance>95)||(active&&distance>unit.reach+40));
        if(unit.returning) {
            bool aboard=true;
            for(auto& officer:unit.officers)if(officer.health>0) {
                if(officer.person.fall.ragdoll()->state()!=saida::RagdollBehaviour::State::Animated||glm::distance(nodePosition(*officer.person.node),at)>3)aboard=false;
            }
            if(aboard){unit.dismounted=unit.returning=false;unit.routeAge=10;}
            else target=at;
        }
        if(think&&unit.routeAge>=2&&!unit.dismounted) {
            // Continue from the point the car is heading for: the nearest
            // point may lie behind it or on the opposite lane, and turning
            // back for it every two seconds keeps the car where it is.
            const bool heading=unit.step<unit.path.size()&&glm::distance(at,policeVector(unit.path[unit.step]))<30;
            unit.path=policeRoads.route(heading?unit.path[unit.step]:policePoint(at),policePoint(target),true);unit.step=0;unit.routeAge=0;
            if(!active&&unit.path.size()<3) {
                const auto ahead=at+unit.car->transform().rotation*glm::vec3(0,0,-120);
                unit.path=policeRoads.route(policePoint(at),policePoint(ahead),true);
            }
            while(unit.step<unit.path.size()&&glm::distance(at,policeVector(unit.path[unit.step]))<4)++unit.step;
        }
        float throttle=0,brake=1,steer=0;
        if(!unit.dismounted&&!disabled&&unit.step<unit.path.size()) {
            auto aim=policeVector(unit.path[unit.step]);
            while(unit.step+1<unit.path.size()&&glm::distance(at,aim)<std::max(3.f,speed*.45f)){aim=policeVector(unit.path[++unit.step]);}
            const auto local=glm::inverse(unit.car->transform().rotation)*(aim-at);
            const float angle=std::atan2(local.x,-local.z);
            steer=std::clamp(-angle*1.8f,-1.f,1.f);
            const float desired=stop?0.f:std::min(active?26.f:10.f,6.f+18.f*std::max(0.f,1-std::abs(angle)));
            throttle=speed<desired?1.f:0.f;brake=speed>desired+1?1.f:0.f;
            saida::QueryFilter filter;filter.ignore=body->bodyId();
            const auto forward=unit.car->transform().rotation*glm::vec3(0,0,-1);
            const auto obstacle=engine.sceneTree().world().physics()->raycast(at+glm::vec3(0,.6f,0),forward,3+speed*.7f,filter);
            if(obstacle.hit){throttle=0;brake=1;}
            // Once stuck, the reverse runs its course: resetting on the speed
            // it gains would pin the car against the obstacle forever.
            unit.stuck=unit.stuck>3||(speed<.3f&&desired>1)?unit.stuck+dt:0;
            if(unit.stuck>3&&unit.stuck<5){throttle=-.5f;brake=0;steer=-steer;}
            if(unit.stuck>6){unit.stuck=0;unit.routeAge=10;unit.path.clear();}
        }
        drive->parked=false;drive->command(throttle,brake,steer);
        unit.spin+=drive->signedSpeed()*dt/.23;
        for(auto* wheel:unit.wheels)wheel->transform().rotation=glm::angleAxis(float(-unit.spin),glm::vec3(1,0,0));
        for(size_t i=0;i<unit.lamps.size();++i)unit.lamps[i]->intensity=active&&std::fmod(policeClock*6+i,2.)<.8?7.f:0.f;
        for(auto& officer:unit.officers)policeOfficer(unit,officer,target,dt,think);
        const auto view=at-camera->transform().position;const float cameraDistance=glm::length(view);
        const bool hidden=cameraDistance>450||(cameraDistance>25&&glm::dot(view/cameraDistance,camera->transform().rotation*glm::vec3(0,0,-1))<-.2f);
        if((glm::distance(at,suspect)>650||(!active&&live>wanted.cars())||(noCrew&&unit.age>45))&&hidden)policeRemove(unit);
    }
    if(policeArrest>0)policeArrest=arrestBefore+dt;
    if(policeArrest>=2.5){policeArrestPlayer();return;}
    policeAir(dt,think);
    wanted.update(dt,policeObserved||wasted);
    if(think&&driving&&std::abs(carSpeed)>25){policeDriving+=.5;if(policeDriving>=5){reportCrime(r1::police::Crime::RecklessDriving,suspect);policeDriving=0;}}
    else if(!driving||std::abs(carSpeed)<20)policeDriving=0;
    if(think&&policeSpawn>4&&live<wanted.cars()&&!policeRoads.points.empty()&&!dropFlying()) {
        const PoliceStation* station=nullptr;float best=700;
        for(const auto& entry:policeStations){const float d=glm::distance(glm::vec3(origin.local(entry.at)),suspect);if(d<best){best=d;station=&entry;}}
        const auto preferred=station?glm::vec3(origin.local(station->at)):suspect;
        // A suspect in a square or a park stands beyond every road: dispatch
        // keeps the roads whose routes come nearest, and officers walk the rest.
        // A pursuit sends the car with the shortest drive to those roads.
        std::vector<float> drive;float closest=std::numeric_limits<float>::infinity();
        auto inRange=[&](size_t i){const float d=glm::distance(policeVector(policeRoads.points[i]),suspect);return d>=45&&d<=300&&!policeRoads.edges[i].empty();};
        if(active) {
            const auto approach=policeRoads.approaches(policePoint(last));
            for(size_t i=0;i<policeRoads.points.size();++i)if(inRange(i))closest=std::min(closest,approach[i]);
            std::vector<int> ends;
            for(size_t i=0;i<policeRoads.points.size();++i)
                if(glm::distance(policeVector(policeRoads.points[i]),last)<=closest+20)ends.push_back(int(i));
            drive=policeRoads.costsTo(ends);
        }
        int spawnIndex=-1;float score=1e9;
        size_t range=0,inView=0,occupied=0,farther=0;
        auto* physics=engine.sceneTree().world().physics();
        for(size_t i=0;i<policeRoads.points.size();++i) {
            if(!inRange(i))continue;
            ++range;
            const auto at=policeVector(policeRoads.points[i]);const float distance=glm::distance(at,suspect);
            if(active&&!std::isfinite(drive[i])){++farther;continue;}
            const float rank=active?drive[i]:station?glm::distance(at,preferred):std::abs(distance-130);
            if(rank>=score)continue;
            const auto view=glm::normalize(at-camera->transform().position);
            if(glm::dot(view,camera->transform().rotation*glm::vec3(0,0,-1))>-.2f){++inView;continue;}
            bool clear=true;for(const auto& unit:policeUnits)if(unit.car&&glm::distance(nodePosition(*unit.car),at)<18)clear=false;
            if(!clear||!physics||!physics->overlapSphere(at+glm::vec3(0,1.2f,0),.65f).empty()){++occupied;continue;}
            spawnIndex=int(i);score=rank;
        }
        // CLAUDE.md rule 3: a dispatch that cannot place a car says why, once
        // per reason.
        const char* refusal=spawnIndex>=0?"":range==0?"no road 45-300 m from the suspect":
            inView>=occupied?"every approaching road is in view":"every approaching road is occupied";
        if(*refusal&&policeRefusal!=refusal)
            saida::Log::error("[World police] dispatch refused (",live,"/",wanted.cars()," cars): ",refusal," (",range," roads, ",
                              farther," farther from the suspect, ",inView," in view, ",occupied," occupied; closest approach ",closest," m)");
        policeRefusal=refusal;
        if(spawnIndex>=0)for(auto& unit:policeUnits)if(!unit.car) {
            if(policeCreate(unit,policeVector(policeRoads.points[size_t(spawnIndex)]),station))policeSpawn=0;break;
        }
    }
    auto& audio=saida::AudioManager::get();
    for(auto& unit:policeUnits)if(unit.car) {
        if(active&&unit.siren==saida::kInvalidAudioID) {
            saida::AudioSettings settings;settings.loop=true;settings.volume=.45f;
            settings.spatialized=true;settings.minDistance=12;settings.maxDistance=230;
            const auto path=std::string("assets/audio/police/")+(unit.identity.redBlue?"siren_us.wav":"siren_eu.wav");
            unit.siren=audio.play(path,settings,"Master",unit.car);
        } else if(!active&&unit.siren!=saida::kInvalidAudioID){audio.stop(unit.siren);unit.siren=saida::kInvalidAudioID;}
        audio.setVolume(unit.siren,glm::distance(nodePosition(*unit.car),camera->transform().position)<230?.45f:0.f);
    }
    if(policeHelicopter) {
        if(policeRotorSound==saida::kInvalidAudioID) {
            saida::AudioSettings settings;settings.loop=true;settings.volume=.35f;
            settings.spatialized=true;settings.minDistance=20;settings.maxDistance=300;
            policeRotorSound=audio.play("assets/audio/police/rotor.wav",settings,"Master",policeHelicopter);
        }
        audio.setVolume(policeRotorSound,glm::distance(nodePosition(*policeHelicopter),camera->transform().position)<300?.35f:0.f);
    } else if(policeRotorSound!=saida::kInvalidAudioID){audio.stop(policeRotorSound);policeRotorSound=saida::kInvalidAudioID;}
    text("wanted-label","Recherche · "+std::to_string(int(std::ceil(wanted.percent)))+"%");
    style("wanted-fill","width",std::to_string(int(wanted.percent*2.46))+"px");
    text("wanted-state",policeArrest>0?"Interpellation…":wanted.percent<=0?"Aucune recherche":
        live==0&&!policeRefusal.empty()?"Aucune patrouille ne peut être envoyée":wanted.searching()?"Recherche · restez hors de vue":wanted.helicopter?"Poursuite · appui aérien":wanted.lethal()?"Poursuite armée":"Police en intervention");
}

std::string policeMapRml() {
    std::string markup;const Frame here(lon,lat,alt);
    auto marker=[&](glm::dvec3 ecef,const char* kind) {
        const auto at=here.local(ecef);const double x=148+at.x*296/900,z=128+at.z*296/900;
        if(x<3||x>290||z<3||z>250)return;
        markup+="<div class='mini-police "+std::string(kind)+"' style='left:"+number(x,0)+"px;top:"+number(z,0)+"px;'></div>";
    };
    for(const auto& station:policeStations)marker(station.at,"station");
    for(const auto& unit:policeUnits)if(unit.car)marker(origin.origin+origin.basis*glm::dvec3(nodePosition(*unit.car)),"patrol");
    if(policeHelicopter)marker(origin.origin+origin.basis*glm::dvec3(nodePosition(*policeHelicopter)),"air");
    if(wanted.searching())marker(policeLastKnown,"search");
    return markup;
}

bool policeSmoke=std::getenv("R1WORLD_POLICE_SMOKE")!=nullptr;
bool policeSmokeGroundHit=false,policeSmokeAirHit=false;
int policeSmokePhase=0;
double policeSmokeTime=0;
glm::vec3 policeSmokeStart{0};
void runPoliceSmoke(double dt) {
    if(dropFlying())return;
    policeSmokeTime+=dt;
    auto fail=[&](const char* reason){saida::Log::error("[World police E2E] FAIL ",reason," phase=",policeSmokePhase);testFailed=true;engine.sceneTree().quit();};
    auto unit=std::find_if(policeUnits.begin(),policeUnits.end(),[](auto& u){return u.car!=nullptr;});
    if(policeSmokeTime>60){
        const auto me=policePoint(policePlayerPosition());
        const int road=policeRoads.nearest(me,1000),walk=policeWalks.nearest(me,1000);
        saida::Log::info("[World police E2E] diagnostic nearest road=",road<0?-1.f:r1::police::Routes::distance(me,policeRoads.points[size_t(road)]),
            " walk=",walk<0?-1.f:r1::police::Routes::distance(me,policeWalks.points[size_t(walk)])," roads=",policeRoads.points.size()," walks=",policeWalks.points.size());
        for(auto& u:policeUnits)if(u.car) {
            const auto at=nodePosition(*u.car);
            saida::Log::info("[World police E2E] diagnostic car=",at.x,",",at.z," route=",u.step,"/",u.path.size(),
                " route_end_gap=",u.path.empty()?-1.f:glm::distance(at,policeVector(u.path.back())),
                " speed=",r1::VehicleRuntime::driver(*u.car)->signedSpeed()," stuck=",u.stuck," returning=",u.returning);
            for(auto& o:u.officers)if(o.person.node)
                saida::Log::info("[World police E2E] diagnostic foot=",u.dismounted," sees=",o.sees," health=",o.health," gap=",glm::distance(nodePosition(*o.person.node),policePlayerPosition())," car_gap=",glm::distance(at,policePlayerPosition())," wanted=",wanted.percent," last_gap=",glm::distance(glm::vec3(origin.local(policeLastKnown)),policePlayerPosition()));
        }
        fail(policeRefusal.empty()?"timed out":policeRefusal.c_str());return;
    }
    if(policeSmokePhase==0) {
        if(unit==policeUnits.end())return;
        if(wanted.percent!=0){fail("quiet patrol already wanted");return;}
        policeSmokeStart=nodePosition(*unit->car);policeSmokePhase=1;policeSmokeTime=0;
    } else if(policeSmokePhase==1&&policeSmokeTime>10) {
        if(unit==policeUnits.end()){fail("patrol disappeared");return;}
        const float moved=glm::distance(policeSmokeStart,nodePosition(*unit->car));
        saida::Log::info("[World police E2E] quiet patrol moved=",moved," route=",unit->path.size()," speed=",r1::VehicleRuntime::driver(*unit->car)->signedSpeed());
        if(moved<4){fail("car did not patrol");return;}
        reportCrime(r1::police::Crime::Collision,policePlayerPosition(),true);
        policeSmokePhase=2;policeSmokeTime=0;
    } else if(policeSmokePhase==2) {
        if(wanted.percent<100){if(policeSmokeTime>1){reportCrime(r1::police::Crime::Collision,policePlayerPosition(),true);policeSmokeTime=0;}return;}
        if(!policeHelicopter||wanted.percent<99){fail("100% did not deploy helicopter");return;}
        if(unit==policeUnits.end()){fail("response car missing");return;}
        // Dispatch must reach the original player position without moving him.
        health.restore();
        policeSmokePhase=3;policeSmokeTime=0;
    } else if(policeSmokePhase==3) {
        if(health.hp>=100)return;
        health.protect(60);
        if(!policeSmokeGroundHit||!policeSmokeAirHit)return;
        saida::Log::info("[World police E2E] PASS natural arrival: gradual escalation, ground and helicopter gunfire; hp=",health.hp);
        if(const char* path=std::getenv("R1WORLD_POLICE_CAPTURE")) {
            saida::CaptureRequest capture;capture.pngPath=path;engine.captureFrameThenExit(capture);policeSmoke=false;return;
        }
        bool struck=false;
        for(auto& u:policeUnits)if(u.dismounted)for(auto& officer:u.officers)if(!struck&&officer.person.node&&officer.person.node->enabled()&&officer.health>0) {
            saida::RaycastHit hit;hit.hit=true;hit.body=officer.person.body->innerBodyId();hit.point=nodePosition(*officer.person.node)+glm::vec3(0,.6f,0);
            struck=hitPolice(hit,20,{1,0,0},false);
            if(!struck||!officer.person.body->physicsSuspended()){fail("officer did not ragdoll");return;}
        }
        if(!struck){fail("no officer to hit");return;}
        policeSmokePhase=4;policeSmokeTime=0;
    } else if(policeSmokePhase==4&&policeSmokeTime>10&&!playerDown) {
        wanted.clear();wanted.report(r1::police::Crime::Assault);
        for(auto& u:policeUnits)if(u.dismounted)for(auto& officer:u.officers)if(officer.health>0)
            officer.person.node->transform().position=policePlayerPosition()+glm::vec3(.8f,0,0);
        policeSmokePhase=5;policeSmokeTime=0;
    } else if(policeSmokePhase==5&&wanted.percent==0) {
        if(arsenal.loaded()!=0||arsenal.reserve()!=0||arsenal.held()!=r1::combat::Weapon::Fists){fail("arrest did not disarm player");return;}
        saida::Log::info("[World police E2E] PASS officer ragdoll, recovery and arrest");
        engine.sceneTree().quit();
        policeSmoke=false;
    }
}
