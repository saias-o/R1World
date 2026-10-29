#include "session.hpp"

namespace r1::world {
void World::Impl::loadPaints() {
    // Albedos, from the Atlas (assets/world/atlas.json), where a test holds
    // them to rule 2 -- a colour the tests cannot see is a rule not held.
    for(const auto& albedo:r1::palette().carPaints)paints.push_back(glm::vec3(albedo[0],albedo[1],albedo[2]));
    if(paints.empty()){
        saida::Log::warn("[World traffic] the Atlas lists no car paint; the fleet will be one colour");
        paints.push_back(glm::vec3(.30f,.30f,.31f));
    }
}

const World::Impl::VehicleModel& World::Impl::vehicleSpec(const saida::Node& node) const {
    for(const auto& spec:fleet)if(node.name()=="vehicle-"+spec.name)return spec;
    return fleet.front(); // The entry scene starts with the city car.
}

void World::Impl::vehicleLod(saida::Node& node) {
    auto* body=r1::VehicleRuntime::body(node);
    auto& visual=body?static_cast<saida::Node&>(*body):node;
    auto* lod=visual.getBehaviour<saida::LODGroupBehaviour>();
    if(!lod)lod=visual.addBehaviour<saida::LODGroupBehaviour>();
    // Full source detail is for close cars. Switch by projected size,
    // keeping long vehicles detailed farther away, with engine hysteresis.
    const bool bike=node.name()=="vehicle-moto_city"||node.name()=="vehicle-moto_cross";
    lod->setLevels({{"Near",bike?.045f:.16f},{"Far",0.f}});
}

uint32_t World::Impl::motorcycleMix(uint32_t x) {
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;
}

void World::Impl::buildMotorcycleHelmetGeometry() {
    saida::MeshData shell,visor;
    constexpr int rings=8,segments=16;
    for(int ring=0;ring<=rings;++ring) {
        const float theta=float(ring)*float(kTwoPi*.25/rings);
        for(int segment=0;segment<=segments;++segment) {
            const float phi=float(segment)*float(kTwoPi/segments);
            const float x=.145f*std::sin(theta)*std::cos(phi);
            const float z=.165f*std::sin(theta)*std::sin(phi);
            const float y=.015f+.185f*std::cos(theta);
            glm::vec3 normal(x/(.145f*.145f),(y-.015f)/(.185f*.185f),z/(.165f*.165f));
            saida::Vertex v{};v.pos={x,y,z};v.normal=glm::normalize(normal);v.color=glm::vec3(1.f);
            shell.vertices.push_back(v);
        }
    }
    for(int ring=0;ring<rings;++ring)for(int segment=0;segment<segments;++segment) {
        const uint32_t a=uint32_t(ring*(segments+1)+segment),b=a+1;
        const uint32_t c=a+uint32_t(segments+1),d=c+1;
        shell.indices.insert(shell.indices.end(),{a,c,b,b,c,d});
    }
    constexpr int visorSegments=8;
    for(int x=0;x<=visorSegments;++x) {
        const float u=-1.f+2.f*float(x)/visorSegments;
        const float z=.142f+.014f*(1.f-u*u);
        for(float y:{.092f,.005f}) {
            saida::Vertex v{};v.pos={u*.112f,y,z};v.normal={0,0,1};v.color=glm::vec3(1.f);
            visor.vertices.push_back(v);
        }
    }
    for(int x=0;x<visorSegments;++x) {
        const uint32_t a=uint32_t(x*2);visor.indices.insert(visor.indices.end(),{a,a+2,a+1,a+1,a+2,a+3});
    }
    motorcycleHelmetShell=std::make_unique<saida::Mesh>(engine.resources().geometry(),shell.vertices,shell.indices);
    motorcycleHelmetVisor=std::make_unique<saida::Mesh>(engine.resources().geometry(),visor.vertices,visor.indices);
}

void World::Impl::addMotorcycleHelmet(saida::Node& rider,uint32_t seed) {
    if(!motorcycleHelmetShell||!motorcycleHelmetVisor)return;
    static constexpr std::array<glm::vec4,8> colors{{
        {.055f,.065f,.075f,1.f},{.82f,.84f,.82f,1.f},{.68f,.055f,.035f,1.f},
        {.045f,.12f,.30f,1.f},{.92f,.48f,.025f,1.f},{.12f,.22f,.11f,1.f},
        {.85f,.78f,.62f,1.f},{.20f,.20f,.22f,1.f}
    }};
    saida::MaterialDesc shellDesc;shellDesc.baseColor=colors[motorcycleMix(seed)%colors.size()];
    shellDesc.roughness=.32f;shellDesc.metallic=.04f;shellDesc.doubleSided=true;
    saida::MaterialDesc visorDesc;visorDesc.baseColor={.025f,.035f,.045f,1.f};
    visorDesc.roughness=.19f;visorDesc.metallic=.22f;visorDesc.doubleSided=true;
    auto helmet=std::make_unique<saida::Node>("Motorcycle helmet");
    saida::Transform offset;offset.position={0.f,.015f,0.f};
    helmet->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 Head",offset);
    auto nearLevel=std::make_unique<saida::Node>("Near");
    nearLevel->addChild(std::make_unique<saida::MeshNode>("Helmet shell",motorcycleHelmetShell.get(),
                                                     engine.resources().getMaterial(shellDesc)));
    nearLevel->addChild(std::make_unique<saida::MeshNode>("Helmet visor",motorcycleHelmetVisor.get(),
                                                     engine.resources().getMaterial(visorDesc)));
    auto farLevel=std::make_unique<saida::Node>("Far");
    farLevel->addChild(std::make_unique<saida::MeshNode>("Helmet shell",motorcycleHelmetShell.get(),
                                                    engine.resources().getMaterial(shellDesc)));
    helmet->addChild(std::move(nearLevel));helmet->addChild(std::move(farLevel));
    helmet->addBehaviour<saida::LODGroupBehaviour>()->setLevels({{"Near",.045f},{"Far",0.f}});
    rider.addChild(std::move(helmet));
}

std::unique_ptr<saida::Node> World::Impl::makeMotorcycleRider(const std::string& vehicle,HumanKind& kind,
                                                      uint32_t seed,bool helmet) {
    const bool motocross=vehicle=="moto_cross";
    auto rider=std::make_unique<saida::Node>("Rider");
    rider->transform().rotation=glm::quat(0,0,1,0);
    rider->transform().position=r1::rocketboxRidingSeat(*kind.rig,motocross,
        float(kVehicleScale),float(humanScale),float(kind.seatDrop));
    rider->transform().scale=glm::vec3(float(humanScale));
    auto* animator=rider->addBehaviour<saida::Animator>();
    animator->setRig(kind.rig);
    for(const auto& [name,clip]:kind.clips)animator->addClip(name,clip);
    animator->play("sit",true,0.f);animator->setPoseRate(6.f,saida::Animator::PoseRateMode::Hold);
    r1::holdRocketboxRidingBase(*animator);
    animator->addModifier<saida::GazeModifier>(*kind.rig,r1::rocketboxRidingLean())->lookAt({0.f,-.4f,1.2f});
    animator->addModifier<saida::GazeModifier>(*kind.rig,r1::rocketboxRidingLook())->lookAt({0.f,1.5f,100.f});
    auto* left=animator->addModifier<saida::TwoBoneIKModifier>(*kind.rig,r1::rocketboxArm(false));
    auto* right=animator->addModifier<saida::TwoBoneIKModifier>(*kind.rig,r1::rocketboxArm(true));
    for(int h=0;h<2;++h) {
        auto* arm=h?right:left;
        const auto grip=motorcycleContact(*rider,vehicle,h,true);
        const auto palm=r1::rocketboxBarGrip(*kind.rig,h==1);
        arm->setTipRotation(palm.rotation);arm->reach(grip-palm.rotation*palm.position);
        arm->setWeightNow(1.f);arm->setPole({grip.x*1.7f,grip.y-.15f,grip.z-.25f});
        auto* leg=animator->addModifier<saida::TwoBoneIKModifier>(*kind.rig,
            saida::TwoBoneIKModifier::Settings{h?"Bip01 R Thigh":"Bip01 L Thigh",h?"Bip01 R Calf":"Bip01 L Calf",h?"Bip01 R Foot":"Bip01 L Foot",22.f});
        const auto foot=motorcycleContact(*rider,vehicle,h,false);
        leg->reach(foot);leg->setWeightNow(1.f);leg->setPole({foot.x*2.f,foot.y+.35f,foot.z+.45f});
        leg->setTipRotation(r1::rocketboxFootRotation(*kind.rig,h==1));
    }
    animator->addModifier<r1::RocketboxRidingHands>(*kind.rig)->hold(true);
    for(const char* level:{"Near","Far"}) {
        auto holder=std::make_unique<saida::Node>(level);
        auto found=std::find_if(kind.levels.begin(),kind.levels.end(),
            [&](const auto& l){return l.first==level;});
        if(found!=kind.levels.end())for(const auto& meshSource:found->second) {
            auto mesh=std::make_unique<saida::MeshNode>(meshSource->name(),meshSource->mesh(),meshSource->material());
            mesh->transform()=meshSource->transform();holder->addChild(std::move(mesh));
        }
        rider->addChild(std::move(holder));
    }
    rider->addBehaviour<saida::LODGroupBehaviour>()->setLevels({{"Near",.045f},{"Far",0.f}});
    if(helmet)addMotorcycleHelmet(*rider,seed);
    return rider;
}

void World::Impl::customizeMotorcycleRider(saida::Node& vehicle,uint32_t seed,double latitude) {
    const std::string name=vehicleSpec(vehicle).name;
    if(!r1::motorcycle(name)||motorcycleRiderKinds.empty())return;
    const auto& kinds=(!motorcycleFemaleRiderKinds.empty()&&r1::femaleMotorcycleRider(seed))
        ?motorcycleFemaleRiderKinds:motorcycleRiderKinds;
    const size_t variant=kinds[motorcycleMix(seed)%kinds.size()];
    HumanKind* kind=humanKind(variant);
    if(!kind||!kind->rig)return;
    const bool helmet=motorcycleMix(seed^0x9e3779b9u)%100u<(latitude>=0.?80u:30u);
    if(auto* old=vehicle.findByPath("Rider"))vehicle.detachChild(old);
    vehicle.addChild(makeMotorcycleRider(name,*kind,seed,helmet));
}

void World::Impl::buildTrafficPrototype() {
    std::ifstream input(game/"assets/models/vehicles/fleet.json");
    if(!input)throw std::runtime_error("Missing road vehicle fleet manifest");
    json doc;input>>doc;
    // Stable 80/20 sex selection, independent of the number of available outfits.
    motorcycleRiderKinds.clear();motorcycleFemaleRiderKinds.clear();
    for(size_t i=0;i<crowdKinds.size();++i) {
        // Two female variants provide the requested distribution without
        // forcing the entire pedestrian wardrobe resident at game startup.
        if(crowdAppearances[i].sex=='f'&&motorcycleFemaleRiderKinds.size()>=2)continue;
        prototypeForStartup(crowdKinds[i].model);
        if(humanKind(i))(crowdAppearances[i].sex=='f'?motorcycleFemaleRiderKinds:motorcycleRiderKinds).push_back(i);
    }
    if(motorcycleRiderKinds.empty()) {
        prototypeForStartup(crowdKinds.front().model);
        if(humanKind(0))motorcycleRiderKinds.push_back(0);
    }
    buildMotorcycleHelmetGeometry();
    for(const auto& entry:doc.at("vehicles")) {
        VehicleModel spec;
        spec.name=entry.at("name");spec.length=entry.at("length");spec.width=entry.at("width");
        spec.height=entry.at("height");spec.wheelbase=entry.at("wheelbase");spec.wheelRadius=entry.at("wheelRadius");
        for(double* d:{&spec.length,&spec.width,&spec.height,&spec.wheelbase,&spec.wheelRadius})*d*=kVehicleScale;
        auto root=std::make_unique<saida::Node>("vehicle-"+spec.name);
        for(const auto& level:std::vector<std::pair<std::string,std::string>>{{"near","Near"},{"far","Far"}}) {
            auto* source=prototypeForStartup(entry.at(level.first).at("path").get<std::string>());
            if(!source)throw std::runtime_error("Vehicle asset failed: "+spec.name);
            auto child=clonePlant(*source);child->setName(level.second);
            child->transform().rotation=glm::quat(0,0,1,0); // Authored forward +Z.
            child->transform().scale=glm::vec3(float(kVehicleScale));
            root->addChild(std::move(child));
        }
        spec.prototype=prototypes->addChild(std::move(root));fleet.push_back(spec);
    }
    if(fleet.empty())throw std::runtime_error("The road vehicle fleet is empty");
    while(!car->children().empty())car->removeChild(car->children().front().get());
    for(const auto& child:fleet.front().prototype->children())car->addChild(clonePlant(*child));
    car->setName("vehicle-city");vehicleLod(*car);collectWheels(*car);
    saida::Log::info("[World traffic] fleet=",fleet.size()," shared vertices=",doc.at("totalVertices").get<size_t>());
}

size_t World::Impl::vehicleKind(uint32_t seed,const saida::traffic::Lane& lane,double motorcycleChance) const {
    // Stable per-agent selection: 5% globally, 50% in Africa, India and
    // Latin America. The remaining share keeps the former car mix intact.
    const uint32_t raw=(seed^(seed>>16))%100;
    const uint32_t bikeCut=uint32_t(std::clamp(motorcycleChance,0.,1.)*100.);
    std::string name=raw<bikeCut?((raw&1u)?"moto_cross":"moto_city"):"";
    const uint32_t roll=raw>=bikeCut?uint32_t((raw-bikeCut)*100/std::max(1u,100-bikeCut)):0;
    if(name.empty())name=roll<34?"city":roll<55?"sedan":roll<74?"suv":roll<86?"offroad":roll<94?"sport":roll<98?"truck":"bus";
    if((name=="truck"||name=="bus")&&lane.speed<11.f)name="city";
    for(size_t i=0;i<fleet.size();++i)if(fleet[i].name==name)return i;
    return 0;
}

double World::Impl::motorcycleChance(double lon,double lat) const {
    const auto country=countryCrowd.at(lon,lat);
    const auto& region=country.subregion;
    const bool africa=region.find("Africa")!=std::string::npos;
    const bool latinAmerica=country.code=="MX"||region=="Central America"||region=="South America"||region=="Caribbean";
    return africa||country.code=="IN"||latinAmerica?.50:.05;
}

void World::Impl::validateTraffic(double dt) {
    if(!playing||pending)return;
    trafficValidationTime_+=dt;trafficValidationReport_+=dt;
    if(trafficValidationReport_<kTrafficValidationReportSeconds)return;
    trafficValidationReport_=0;
    size_t live=0,moving=0,physical=0,grounded=0,incidents=0,requested=0,damaged=0;
    std::array<size_t,8> reasons{};size_t deadEnds=0;
    for(auto& [key,region]:loaded)for(size_t slot=0;slot<region.flow.agents().size();++slot) {
        const auto& agent=region.flow.agents()[slot];if(!agent.alive)continue;
        ++live;moving+=agent.speed>1.f;incidents+=agent.incident;requested+=agent.targetSpeed>1.f;
        if(agent.targetSpeed<.1f)++reasons[size_t(agent.stopReason)];
        deadEnds+=agent.nextLane==UINT32_MAX;
        if(agent.waiting>10.f) {
            const auto pose=region.flow.pose(agent);
            const auto& lane=region.graph.lanes[agent.lane];
            const auto observer=region.frame.local(ecef(lon,lat,alt));
            if(agent.external&&agent.waiting>60.f&&std::hypot(pose.position.x-observer.x,pose.position.y-observer.z)<60.)
                trafficValidationStuck_=true;
            saida::Log::info("[World traffic waiter] tile=",key," id=",agent.identity," lane=",agent.lane," next=",agent.nextLane,
                " s=",agent.s," remaining=",region.graph.laneLength(agent.lane)-agent.s," wait=",agent.waiting,
                " speed=",agent.speed," target=",agent.targetSpeed," reason=",size_t(agent.stopReason)," physical=",agent.external,
                " signal=",lane.signal," group=",unsigned(lane.signalGroup)," light=",lane.signal>=0?int(region.graph.signals[lane.signal].light(lane.signalGroup,trafficTime_)):-1,
                " x=",pose.position.x," z=",pose.position.y," yaw=",pose.yaw);
        }
        if(slot<region.cars.size()&&region.cars[slot]&&r1::VehicleRuntime::physical(*region.cars[slot])) {
            ++physical;const auto* drive=r1::VehicleRuntime::driver(*region.cars[slot]);
            grounded+=drive->grounded();damaged+=drive->damage>.1f;
        }
    }
    trafficValidationLive_=std::max(trafficValidationLive_,live);
    trafficValidationMoving_=std::max(trafficValidationMoving_,moving);
    if(trafficValidationTime_>=kTrafficValidationWarmupSeconds){++trafficValidationSteadySamples_;trafficValidationSteadyMoving_+=moving;}
    saida::Log::info("[World traffic validation] t=",trafficValidationTime_," live=",live," moving=",moving,
        " requested=",requested," physical=",physical," grounded=",grounded," incidents=",incidents," damaged=",damaged);
    saida::Log::info("[World traffic validation] stops following=",reasons[1]," junction=",reasons[2],
        " blockedExit=",reasons[3]," signal=",reasons[4]," stopSign=",reasons[5]," deadEnds=",deadEnds);
    if(trafficValidationTime_>=trafficValidationSeconds_) {
        // A snapshot can coincide with all-red clearance. Measure sustained
        // motion after warmup instead of requiring a particular final phase.
        const double steadyMoving=trafficValidationSteadySamples_?double(trafficValidationSteadyMoving_)/trafficValidationSteadySamples_:0.;
        testFailed=trafficValidationLive_<kTrafficValidationMinimumLive||steadyMoving<kTrafficValidationMinimumMoving||trafficValidationStuck_;
        saida::Log::info("[World traffic validation] ",testFailed?"FAIL":"PASS"," peak live=",trafficValidationLive_," peak moving=",trafficValidationMoving_," steady moving=",steadyMoving," local persistent stop=",trafficValidationStuck_);
        engine.sceneTree().quit();
    }
}

void World::Impl::prepareSignalMeshes() {
    if(signalMeshes_[0])return;
    auto& resources=engine.resources();
    signalMeshes_[0]=resources.queueMemoryMesh(saida::prepareMesh(r1::traffic_signal::pole()),"r1world/signal/pole");
    signalMeshes_[1]=resources.queueMemoryMesh(saida::prepareMesh(r1::traffic_signal::housing()),"r1world/signal/housing");
    signalMeshes_[2]=resources.queueMemoryMesh(saida::prepareMesh(r1::traffic_signal::lens()),"r1world/signal/lens");
}

bool World::Impl::frameTrafficSignal() {
    saida::Node* closest=nullptr;float distance=std::numeric_limits<float>::max();
    const auto observer=origin.local(ecef(lon,lat,alt));
    for(auto& [key,region]:loaded)for(const auto& child:region.node->children()) {
        if(child->name()!="Traffic signal")continue;
        const float candidate=glm::distance(glm::vec3(observer),glm::vec3(child->worldTransform()[3]));
        if(candidate<distance){distance=candidate;closest=child.get();}
    }
    if(!closest)return false;
    const auto matrix=closest->worldTransform();
    const auto position=glm::vec3(matrix*glm::vec4(1.4f,2.3f,-3.8f,1));
    const auto target=glm::vec3(matrix*glm::vec4(0,1.9f,r1::traffic_signal::kHeadOffset,1));
    player->setVisible(false);camera->transform().position=position;
    camera->transform().rotation=glm::quatLookAt(glm::normalize(target-position),glm::vec3(0,1,0));
    return true;
}

saida::Material* World::Impl::signalMaterial(uint32_t colour,bool lit) {
    static const std::array<glm::vec3,3> colours{{{1,.03f,.01f},{1,.5f,.01f},{.03f,1,.1f}}};
    constexpr float kLensGlassAlbedo=.08f,kLensEmissiveStrength=2.f;
    saida::MaterialDesc desc;
    desc.baseColor=glm::vec4(colours[colour]*kLensGlassAlbedo,1);
    desc.emissiveColor=glm::vec4(lit?colours[colour]*kLensEmissiveStrength:glm::vec3(0),1);
    desc.roughness=.35f;
    return engine.resources().getMaterial(desc);
}

void World::Impl::buildTrafficSignals(Loaded& tile,const json& doc) {
    if(!doc.contains("controls"))return;
    using namespace r1::traffic_signal;
    constexpr float kKerbClearance=.45f;
    constexpr double kMaximumRoadsideReach=10.;
    r1::RoadSurface roadSurface;
    if(auto surface=tile.data.find("roadSurface");surface!=tile.data.end())for(const auto& contour:*surface) {
        r1::Ring ring;
        for(const auto& point:contour)ring.push_back({point[0],point[1]});
        roadSurface.contours.push_back(std::move(ring));
    }
    prepareSignalMeshes();
    auto& resources=engine.resources();
    saida::MaterialDesc pole;pole.baseColor={.24f,.25f,.26f,1};pole.roughness=.4f;pole.metallic=.7f;
    auto* poleMaterial=engine.resources().getMaterial(pole);
    saida::MaterialDesc casing;casing.baseColor={.018f,.02f,.023f,1};casing.roughness=.5f;
    auto* housingMaterial=resources.getMaterial(casing);
    auto approachDirection=[&](const json& control) {
        return control.contains("stopDirection")?
            saida::traffic::Vec2{control["stopDirection"][0].get<float>(),control["stopDirection"][1].get<float>()}:
            tile.graph.direction(control.at("lane").get<uint32_t>());
    };
    std::map<std::pair<uint32_t,uint8_t>,const json*> approaches;
    for(const auto& control:doc.at("controls")) {
        if(control.at("kind")!="traffic_signals")continue;
        if(!control.value("stopLineDrawn",false)) {
            saida::Log::warn("[World signals] no drawable stop line for lane ",control.at("lane"),"; post omitted");
            continue;
        }
        const auto laneIndex=control.at("lane").get<uint32_t>();
        if(laneIndex>=tile.graph.lanes.size())continue;
        const auto& lane=tile.graph.lanes[laneIndex];if(lane.signal<0)continue;
        const auto key=std::make_pair(uint32_t(lane.signal),lane.signalGroup);
        auto [found,inserted]=approaches.emplace(key,&control);
        if(!inserted) {
            const auto& existing=*found->second;
            const auto direction=approachDirection(control);
            const float side=doc.value("leftHand",false)?-1.f:1.f;
            const auto& junction=tile.signalPositions[size_t(lane.signal)];
            auto lateral=[&](const json& candidate) {
                const auto& p=candidate.at("stopPosition");
                return (p[0].get<float>()-junction.x)*(-direction.y*side)+
                       (p[2].get<float>()-junction.y)*(direction.x*side);
            };
            if(lateral(control)>lateral(existing))found->second=&control;
        }
    }
    for(const auto& [key,control]:approaches) {
        const auto laneIndex=control->at("lane").get<uint32_t>();
        const auto& lane=tile.graph.lanes[laneIndex];
        const auto direction=approachDirection(*control);
        constexpr float kJunctionMatchTolerance=.1f,kCrossingDirectionCosine=.7f;
        const saida::traffic::Vec2 junction{tile.signalPositions[size_t(lane.signal)].x,tile.signalPositions[size_t(lane.signal)].y};
        float crossingHalfWidth=0.f;
        for(uint32_t crossing=0;crossing<tile.graph.lanes.size();++crossing) {
            const auto& other=tile.graph.lanes[crossing];
            if(std::abs(saida::traffic::dot(direction,tile.graph.direction(crossing)))>kCrossingDirectionCosine)continue;
            if(saida::traffic::length(tile.graph.nodes[other.from]-junction)>kJunctionMatchTolerance&&
               saida::traffic::length(tile.graph.nodes[other.to]-junction)>kJunctionMatchTolerance)continue;
            crossingHalfWidth=std::max(crossingHalfWidth,std::abs(other.offset)+other.width*.5f);
        }
        const float junctionDistance=saida::traffic::dot(junction-tile.graph.nodes[lane.to],direction);
        const float setback=std::isfinite(lane.stopLineDistance)?lane.stopLineDistance:
            std::max(tile.flow.driver.stopLineDistance,crossingHalfWidth+kKerbClearance-junctionDistance);
        const auto centre=control->contains("stopPosition")?
            saida::traffic::Vec2{control->at("stopPosition")[0].get<float>(),control->at("stopPosition")[2].get<float>()}:
            tile.flow.positionOf(laneIndex,tile.graph.laneLength(laneIndex)-setback);
        const float side=doc.value("leftHand",false)?-1.f:1.f;
        const auto location=roadSurface.roadside({centre.x,centre.y},{-direction.y*side,direction.x*side},kKerbClearance,kMaximumRoadsideReach);
        if(!location) {
            saida::Log::warn("[World signals] no clear roadside for lane ",laneIndex,"; post omitted");
            continue;
        }
        const auto geo=geodeticOf(tile,{float(location->x),control->at("position")[1].get<float>(),float(location->y)});
        if(!this->tile(geo.x,geo.y)) {
            saida::Log::warn("[World signals] roadside terrain unavailable for lane ",laneIndex,"; post omitted");
            continue;
        }
        if(this->tile(geo.x,geo.y)!=&tile)continue; // one owning tile, even at streamed boundaries
        // The foundation sits on terrain; pavement may cover its foot.
        // A universal pavement lift would leave verge-mounted posts floating.
        const float ground=float(tile.frame.local(ecef(geo.x,geo.y,height(geo.x,geo.y))).y);
        auto* assembly=tile.node->createChild<saida::Node>("Traffic signal");
        assembly->transform().position={float(location->x),ground,float(location->y)};
        assembly->transform().rotation=glm::angleAxis(std::atan2(direction.x,direction.y),glm::vec3(0,1,0));
        assembly->createChild<saida::MeshNode>("Galvanized pole and brackets",resources.getMesh(signalMeshes_[0]),poleMaterial);
        for(int head=0;head<2;++head) {
            const float scale=head==0?1.f:kRepeaterScale,height=head==0?kHeadHeight:kRepeaterHeight;
            auto* housing=assembly->createChild<saida::MeshNode>(head==0?"Signal housing and visors":"Repeater housing and visors",resources.getMesh(signalMeshes_[1]),housingMaterial);
            housing->transform().position={0,height,kHeadOffset};housing->transform().scale=glm::vec3(scale);
            for(uint32_t colour=0;colour<3;++colour) {
                auto* lamp=assembly->createChild<saida::MeshNode>("Signal lens",resources.getMesh(signalMeshes_[2]),signalMaterial(colour,false));
                lamp->transform().position={0,height+(1.f-float(colour))*kLensPitch*scale,kHeadOffset};
                lamp->transform().scale=glm::vec3(scale);
                tile.signalLamps.push_back({lamp,uint32_t(lane.signal),lane.signalGroup,colour,false});
            }
        }
    }
}

void World::Impl::readGraph(Loaded& tile) {
    auto found=tile.data.find("traffic");
    if(found==tile.data.end())return;   // cooked before traffic existed
    const auto& doc=*found;
    for(auto& node:doc.at("nodes")) {
        tile.graph.nodes.push_back({float(node[0]),float(node[2])});
        tile.groundUp.push_back(float(node[1]));
    }
    for(const auto& entry:doc.at("lanes")) {
        using namespace r1::traffic_format;
        saida::traffic::Lane lane{uint32_t(entry[From]),uint32_t(entry[To]),float(entry[Speed]),float(entry[Weight])};
        if(entry.size()>AllowedTurns) {
            lane.offset=entry[Offset];lane.width=entry[Width];lane.road=entry[Road];lane.number=entry[Number];lane.priority=entry[Priority];
            lane.signal=entry[Signal];lane.signalGroup=entry[Group];lane.stop=entry[Stop];lane.yield=entry[Yield];lane.allowedTurns=entry[AllowedTurns];
        }
        if(entry.size()>Layer)lane.layer=entry[Layer];
        if(entry.size()>StopDistance)lane.stopLineDistance=entry[StopDistance];
        tile.graph.lanes.push_back(lane);
    }
    if(doc.contains("signals"))for(const auto& entry:doc.at("signals")) {
        saida::traffic::Signal signal;signal.key=entry.at("key");signal.offset=entry.value("offset",0.);
        tile.graph.signals.push_back(signal);
        const auto& p=entry.at("position");tile.signalPositions.push_back({p[0].get<float>(),p[2].get<float>()});
    }
    if(doc.contains("restrictions"))for(const auto& entry:doc.at("restrictions"))
        tile.graph.restrictions.push_back({entry[0],entry[1],entry[2],entry[3]});
    if(tile.graph.lanes.empty())return;
    tile.graph.rightTurnCrossSign=1;
    tile.graph.build();
    saida::traffic::Rules rules;
    rules.leftHand=doc.value("leftHand",false);
    rules.lateralAxisSign=-1.f;
    // The despawn radius is the streamer's, not a taste: a car further
    // than the tile ring is a car in a tile that is about to be evicted.
    rules.despawn=260.f;
    // The simulator measures centre-to-centre gaps. Reserve a bus length
    // plus clearance so two long vehicles cannot overlap in a queue.
    rules.minGap=saida::traffic::DriverSettings{}.defaultLength+saida::traffic::DriverSettings{}.bumperGap;
    rules.junctionGuard=14.f;
    // Deterministic per tile, so the same street is the same street on
    // every visit and a capture can be compared with itself.
    tile.flow.reset(&tile.graph,rules,uint32_t(std::hash<std::string>{}(tile.data.at("key").get<std::string>()))|1u);
    tile.motorcycleChance=motorcycleChance(tile.data.value("lon",0.),tile.data.value("lat",0.));
    tile.flow.setSpawnSize([this,&tile](uint32_t seed,const saida::traffic::Lane& lane) {
        const auto& kind=fleet[vehicleKind(seed,lane,tile.motorcycleChance)];
        return saida::traffic::VehicleSize{float(kind.length),float(kind.width)};
    });
    tile.wanted=doc.value("cars",0);
    buildTrafficSignals(tile,doc);
}

glm::vec3 World::Impl::paintFor(uint32_t seed) const {
    return paints[seed%paints.size()];
}

void World::Impl::paintCar(saida::Node& node,glm::vec3 albedo) {
    node.traverse([&](saida::Node& n,const glm::mat4&){
        if(!n.material()||n.name().rfind("paint-",0)!=0)return;
        auto desc=n.material()->desc();
        desc.baseColor=glm::vec4(albedo,desc.baseColor.a);
        constexpr float kVehicleReflection=1.f;
        desc.environmentReflection=std::max(desc.environmentReflection,kVehicleReflection);
        if(auto* mesh=dynamic_cast<saida::MeshNode*>(&n))
            mesh->setMaterial(engine.resources().getMaterial(desc));
    });
}

glm::dvec3 World::Impl::cameraTileLocal(const Loaded& tile) const {
    const glm::vec3 p=camera->transform().position;
    return tile.frame.local(origin.origin+origin.basis*glm::dvec3(p.x,p.y,p.z));
}

bool World::Impl::trafficVisible(const Loaded& tile,const saida::traffic::Pose& pose) const {
    const auto eye=cameraTileLocal(tile);
    const glm::dvec3 delta(pose.position.x-eye.x,0,pose.position.y-eye.z);
    constexpr double kVisibilityDistance=300.;
    constexpr double kVisibilityCosine=.15;
    const double distance=glm::length(delta);
    if(distance>kVisibilityDistance)return false;
    if(distance<1.)return true;
    const auto forward=glm::transpose(tile.frame.basis)*origin.basis*glm::dvec3(camera->transform().rotation*glm::vec3(0,0,-1));
    return glm::dot(delta,forward)/distance>kVisibilityCosine;
}

saida::traffic::Pose World::Impl::physicalTrafficPose(Loaded& tile,size_t slot) {
    auto& root=*tile.cars[slot];r1::VehicleRuntime::synchronize(root);
    const auto position=root.transform().position;
    const auto forward=root.transform().rotation*glm::vec3(0,0,-1);
    auto* rigid=r1::VehicleRuntime::body(root);
    const auto velocity=glm::transpose(glm::mat3(tile.node->worldTransform()))*rigid->physicsWorld()->linearVelocity(rigid->bodyId());
    return {{position.x,position.z},std::atan2(forward.x,forward.z),std::max(0.f,glm::dot(velocity,forward))};
}

saida::traffic::Obstacle World::Impl::vehicleTrafficObstacle(saida::Node& car,const Loaded& tile) const {
    glm::vec3 position;glm::quat rotation;
    if(r1::VehicleRuntime::physical(car)) {
        auto* body=r1::VehicleRuntime::body(car);
        body->physicsWorld()->getBodyTransform(body->bodyId(),position,rotation);
    } else {position=glm::vec3(car.worldTransform()[3]);rotation=glm::quat_cast(glm::mat3(car.worldTransform()));}
    const auto inverse=glm::inverse(tile.node->worldTransform());
    const auto local=glm::vec3(inverse*glm::vec4(position,1));
    const auto direction=glm::mat3(inverse)*(rotation*glm::vec3(0,0,-1));
    const auto& spec=vehicleSpec(car);
    saida::traffic::Obstacle obstacle;obstacle.active=car.enabled();obstacle.position={local.x,local.z};
    obstacle.halfLength=float(spec.length*.5);obstacle.halfWidth=float(spec.width*.5);obstacle.yaw=std::atan2(direction.x,direction.z);
    return obstacle;
}

void World::Impl::updateTraffic(float delta) {
    SAIDA_PROFILE_SCOPE("World/Traffic");
    if(!debugTraffic||fleet.empty())return;
    const float trafficDelta=delta;
    trafficTime_+=trafficDelta;
    auto promoteParked=[&](saida::Node* node) {
        if(!node||!node->enabled()||r1::VehicleRuntime::physical(*node))return;
        const auto eye=origin.local(ecef(lon,lat,alt));
        if(glm::length(glm::dvec3(node->transform().position)-eye)<r1::VehicleRuntime::kPromotionDistance)
            if(auto* physics=engine.sceneTree().world().physics())r1::VehicleRuntime::activate(*node,*physics,glm::vec3(0));
    };
    if(carParked&&!carSinking)promoteParked(car);
    for(auto& entry:parked)promoteParked(entry.node);
    // §5 again, from the other side: the whole neighbourhood shares one
    // fleet budget, and the tiles nearest the player spend it first --
    // `nearby` returns them in that order. Far tiles keep their graph and
    // simply show nothing, which costs one integer.
    size_t remaining=kTrafficCars;
    for(auto tile:ring) {
        auto found=loaded.find(tile.key());
        if(found==loaded.end())continue;
        Loaded& l=found->second;
        if(l.graph.lanes.empty())continue;
        const glm::dvec3 eye=cameraTileLocal(l);
        const size_t localCapacity=l.flow.capacityNear({float(eye.x),float(eye.z)});
        const size_t share=std::min({l.wanted,localCapacity,remaining});
        remaining-=share;
        l.flow.setPopulation(uint32_t(share));
        l.flow.setVisibility([this,&l](const saida::traffic::Pose& pose){return playing&&!pending&&trafficVisible(l,pose);});
        l.flow.setTime(trafficTime_);
        for(auto& lamp:l.signalLamps) {
            const auto light=l.graph.signals[lamp.program].light(lamp.group,trafficTime_);
            const bool lit=uint32_t(light)==lamp.colour;
            if(lit!=lamp.lit){lamp.node->setMaterial(signalMaterial(lamp.colour,lit));lamp.lit=lit;}
        }
        for(size_t i=0;i<l.cars.size();++i)if(l.cars[i]&&trafficSlotLive(l,i)&&r1::VehicleRuntime::physical(*l.cars[i])) {
            const auto pose=physicalTrafficPose(l,i);
            const auto* deform=r1::VehicleRuntime::body(*l.cars[i])->getBehaviour<saida::DeformableMeshBehaviour>();
            l.flow.setPhysicalPose(i,pose,deform&&deform->absorbedEnergy()>=deform->maximumImpactEnergy*r1::VehicleRuntime::kDisablingDamageFraction);
        }
        std::vector<saida::traffic::Obstacle> roadObstacles;
        // A transferred body can still occupy the old tile's road. Traffic
        // on either side must see its footprint while ownership changes.
        for(auto& [otherKey,otherRegion]:loaded)if(&otherRegion!=&l)
            for(size_t other=0;other<otherRegion.cars.size();++other)
                if(otherRegion.cars[other]&&trafficSlotLive(otherRegion,other))
                    roadObstacles.push_back(vehicleTrafficObstacle(*otherRegion.cars[other],l));
        if(carParked&&car&&!driving())roadObstacles.push_back(vehicleTrafficObstacle(*car,l));
        for(auto& parkedCar:parked)roadObstacles.push_back(vehicleTrafficObstacle(*parkedCar.node,l));
        for(const auto& walker:l.crowd.walkers())if(walker.alive&&walker.link>=0&&walker.along>.75&&l.walks.links[walker.link].crossing)
            roadObstacles.push_back({{float(walker.x),float(walker.z)},float(r1::Crowd::kBodyRadius),true});
        l.flow.setObstacles(std::move(roadObstacles));
        // The observer is the camera, not the player. It stands eight and a
        // half metres behind him in a car, so "sixteen metres behind the
        // player" was eight metres behind the *camera* -- in shot, which is
        // half of why cars appeared in view.
        saida::traffic::Obstacle player;
        if(driving())player=vehicleTrafficObstacle(*car,l);
        // Which way the camera is looking, in this tile's frame, so cars
        // may appear close behind it and never close in front.
        const glm::vec3 ahead=camera->transform().rotation*glm::vec3(0,0,-1);
        const glm::dmat3 intoTile=glm::transpose(l.frame.basis)*origin.basis;
        const glm::dvec3 look=intoTile*glm::dvec3(ahead.x,ahead.y,ahead.z);
        const double flat=std::hypot(look.x,look.z);
        const saida::traffic::Vec2 facing=flat>1e-6
            ?saida::traffic::Vec2{float(look.x/flat),float(look.z/flat)}
            :saida::traffic::Vec2{0.f,0.f};
        l.flow.update(trafficDelta,{float(eye.x),float(eye.z)},facing,player);
        syncTrafficNodes(l,trafficDelta);
    }
    transferTraffic();
}

void World::Impl::resizeTrafficSlots(Loaded& tile,size_t count) {
    if(tile.cars.size()>=count)return;
    tile.cars.resize(count,nullptr);tile.carKinds.resize(count,0);tile.carWheels.resize(count);
    tile.carSpin.resize(count,0);tile.carIdentities.resize(count,0);
}

bool World::Impl::transferTrafficCar(Loaded& source,size_t index,Loaded& destination) {
    auto* root=source.cars[index];
    r1::VehicleRuntime::synchronize(*root);
    root->updateTransforms(source.node->worldTransform(),true);
    const auto into=glm::inverse(destination.node->worldTransform())*root->worldTransform();
    const auto forward=glm::mat3(into)*glm::vec3(0,0,-1);
    const auto agent=source.flow.agents()[index];
    const saida::traffic::Pose actual{{into[3].x,into[3].z},std::atan2(forward.x,forward.z),agent.speed};
    const auto slot=destination.flow.adopt(agent,actual,source.graph.lanes[agent.lane].road);
    if(!slot)return false;
    auto owned=source.node->detachChild(root);
    if(!owned){destination.flow.retire(*slot);return false;}
    source.flow.retire(index);
    root->transform().position=glm::vec3(into[3]);
    root->transform().rotation=glm::normalize(glm::quat_cast(glm::mat3(into)));
    resizeTrafficSlots(destination,*slot+1);
    if(destination.cars[*slot])destination.cars[*slot]->queueFree();
    destination.cars[*slot]=destination.node->addChild(std::move(owned));
    destination.carKinds[*slot]=source.carKinds[index];destination.carIdentities[*slot]=source.carIdentities[index];
    destination.carWheels[*slot]=std::move(source.carWheels[index]);destination.carSpin[*slot]=source.carSpin[index];
    source.cars[index]=nullptr;source.carIdentities[index]=0;
    return true;
}

void World::Impl::preserveTrafficCar(Loaded& source,size_t index) {
    auto* root=source.cars[index];
    r1::VehicleRuntime::synchronize(*root);
    root->updateTransforms(source.node->worldTransform(),true);
    const auto world=root->worldTransform();
    const auto point=origin.origin+origin.basis*glm::dvec3(world[3]);
    const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
    auto owned=source.node->detachChild(root);if(!owned)return;
    source.flow.retire(index);source.cars[index]=nullptr;source.carIdentities[index]=0;
    root=engine.sceneTree().world().addChild(std::move(owned));
    root->transform().position=glm::vec3(world[3]);root->transform().rotation=glm::quat_cast(glm::mat3(world));
    if(auto* drive=r1::VehicleRuntime::driver(*root))drive->parked=true;
    const auto forward=root->transform().rotation*glm::vec3(0,0,-1);
    parked.push_back({root,geo.x,geo.y,geo.z-r1::VehicleRuntime::kBodyClearance,std::atan2(double(forward.x),-double(forward.z))/rad});
}

void World::Impl::transferTraffic() {
    for(auto& [key,source]:loaded)for(size_t i=0;i<source.cars.size();++i) {
        auto* root=source.cars[i];if(!root||!trafficSlotLive(source,i))continue;
        const auto geo=geodeticOf(source,root->transform().position);
        auto* destination=tile(geo.x,geo.y);
        const auto& agent=source.flow.agents()[i];
        const float remaining=source.graph.laneLength(agent.lane)-agent.s;
        constexpr float kBoundaryProbeDistance=2.f;
        const auto terminal=source.flow.positionOf(agent.lane,source.graph.laneLength(agent.lane))+source.graph.direction(agent.lane)*kBoundaryProbeDistance;
        const auto ahead=geodeticOf(source,{terminal.x,root->transform().position.y,terminal.y});
        auto* forwardTile=tile(ahead.x,ahead.y);
        // Early handoff leaves the centre in the old tile for a few frames.
        // Ownership follows the road's exit; its entry is never a return
        // transfer just because the body has not crossed the seam yet.
        if(destination&&destination!=&source&&destination!=forwardTile)continue;
        if((!destination||destination==&source)&&agent.nextLane==UINT32_MAX&&remaining<source.flow.driver.transferTolerance) {
            destination=forwardTile;
        }
        if(!destination||destination==&source||destination->graph.lanes.empty())continue;
        transferTrafficCar(source,i,*destination);
    }
}

bool World::Impl::trafficSlotLive(const Loaded& tile,size_t slot) const {
    const auto& agents=tile.flow.agents();
    return debugTraffic&&slot<agents.size()&&agents[slot].alive;
}

void World::Impl::syncTrafficNodes(Loaded& tile,float delta) {
    const auto& agents=tile.flow.agents();
    resizeTrafficSlots(tile,agents.size());
    for(size_t i=0;i<tile.cars.size();++i) {
        const bool live=trafficSlotLive(tile,i);
        if(live&&tile.cars[i]&&tile.carIdentities[i]!=agents[i].identity&&
           (r1::motorcycle(fleet[tile.carKinds[i]].name)||
            tile.carKinds[i]!=vehicleKind(agents[i].vehicleSeed,tile.graph.lanes[agents[i].lane],tile.motorcycleChance))) {
            tile.cars[i]->setEnabled(false);tile.cars[i]->queueFree();tile.cars[i]=nullptr;
        }
        if(!tile.cars[i]) {
            if(!live)continue;                 // no node until a slot is used
            const auto& agent=agents[i];
            tile.carKinds[i]=vehicleKind(agent.vehicleSeed,tile.graph.lanes[agent.lane],tile.motorcycleChance);
            auto node=clonePlant(*fleet[tile.carKinds[i]].prototype);
            customizeMotorcycleRider(*node,agent.vehicleSeed,tile.data.value("lat",0.));
            vehicleLod(*node);vehicleCollider(*node,fleet[tile.carKinds[i]]);
            paintCar(*node,paintFor(agent.seed));
            tile.carWheels[i].clear();tile.carSpin[i]=0.;
            std::function<void(saida::Node&)> wheels=[&](saida::Node& n){
                if(n.name().rfind("wheel-",0)==0){tile.carWheels[i].push_back(&n);return;}
                for(auto& c:n.children())wheels(*c);
            };
            wheels(*node);
            tile.cars[i]=tile.node->addChild(std::move(node));
        }
        if(!live) {
            tile.cars[i]->setEnabled(false);
            continue;
        }
        tile.cars[i]->setEnabled(true);
        const auto& agent=agents[i];
        if(tile.carIdentities[i]!=agent.identity) {
            if(auto* occupant=tile.cars[i]->getBehaviour<VehicleOccupant>())occupant->escaped=false;
            r1::VehicleRuntime::reset(*tile.cars[i]);tile.carIdentities[i]=agent.identity;
            const auto& dimensions=fleet[tile.carKinds[i]];
            tile.flow.setVehicleDimensions(i,float(dimensions.length),float(dimensions.width));
            const auto profile=r1::vehicleDynamics(dimensions.name);
            const auto* vehicleDrive=r1::VehicleRuntime::driver(*tile.cars[i]);
            tile.flow.setVehicleLimits(i,{profile.acceleration,profile.braking,profile.grip,profile.maximumSpeed,
                float(dimensions.wheelbase),glm::radians(vehicleDrive->steeringAngle)});
            paintCar(*tile.cars[i],paintFor(uint32_t(agent.identity)));
        }
        // Agent seeds advance at junctions. Keep model and paint stable
        // for the pooled node instead of changing a car in full view.
        tile.carSpin[i]+=agent.speed*delta/fleet[tile.carKinds[i]].wheelRadius;
        for(auto* wheel:tile.carWheels[i])wheel->transform().rotation=
            glm::angleAxis(float(r1::motorcycle(fleet[tile.carKinds[i]].name)?tile.carSpin[i]:-tile.carSpin[i]),glm::vec3(1,0,0));
        const auto pose=tile.flow.pose(agent);
        const auto& lane=tile.graph.lanes[agent.lane];
        const float span=tile.graph.laneLength(agent.lane);
        const float t=span>0.f?agent.s/span:0.f;
        const float up=tile.groundUp[lane.from]*(1.f-t)+tile.groundUp[lane.to]*t;
        const saida::traffic::Vec2 dir{std::sin(pose.yaw),std::cos(pose.yaw)};
        // Engine z points south, so north is -z and the bearing the rest of
        // this file speaks is atan2(east, north).
        const double bearing=std::atan2(double(dir.x),-double(dir.y));
        auto& root=*tile.cars[i];
        const auto observer=tile.frame.local(ecef(lon,lat,alt));
        const float distance=std::hypot(float(observer.x)-pose.position.x,float(observer.z)-pose.position.y);
        if(!r1::VehicleRuntime::physical(root)) {
            root.transform().position=glm::vec3(pose.position.x,up+r1::VehicleRuntime::kBodyClearance,pose.position.y);
            const float rise=(tile.groundUp[lane.to]-tile.groundUp[lane.from])/std::max(.05f,span);
            root.transform().rotation=glm::angleAxis(float(-bearing),glm::vec3(0,1,0))*glm::angleAxis(std::atan(rise),glm::vec3(1,0,0));
            const float promotion=r1::VehicleRuntime::kPromotionDistance+std::abs(float(carSpeed))*r1::VehicleRuntime::kPredictionTime;
            if(distance<promotion)if(auto* physics=engine.sceneTree().world().physics())
                r1::VehicleRuntime::activate(root,*physics,glm::mat3(tile.node->worldTransform())*
                    (root.transform().rotation*glm::vec3(0,0,-1))*agent.speed);
        }
        if(r1::VehicleRuntime::physical(root)) {
            const auto command=tile.flow.command(i,pose);
            auto* drive=r1::VehicleRuntime::driver(root);drive->parked=false;drive->command(command.throttle,command.brake,command.steer);
            const auto lanePosition=tile.flow.positionOf(agent.lane,agent.s);
            const auto laneDirection=tile.graph.direction(agent.lane);
            const bool aligned=saida::traffic::length(pose.position-lanePosition)<.5f&&
                saida::traffic::dot(dir,laneDirection)>.98f;
            if(distance>r1::VehicleRuntime::kDemotionDistance&&!agent.incident&&!trafficVisible(tile,pose)&&
               aligned&&r1::VehicleRuntime::stableForTraffic(root)) {
                r1::VehicleRuntime::deactivate(root);tile.flow.releasePhysicalPose(i);
            }
        }
    }
}

size_t World::Impl::trafficWanted() const {
    size_t n=0;
    for(const auto& [key,tile]:loaded)n+=tile.wanted;
    return n;
}

size_t World::Impl::trafficLive() const {
    size_t n=0;
    for(const auto& [key,tile]:loaded)n+=tile.flow.live();
    return n;
}

} // namespace r1::world
