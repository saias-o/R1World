#include "session.hpp"

namespace r1::world {
#ifdef SAIDA_ENABLE_XR
void World::Impl::setXrGestureInput(const saida::xrgesture::RuntimeInputState& state) { vrGestures.setRuntimeState(state); }
#endif

World::Impl::Impl(saida::Engine& e,fs::path g,bool test,saida::CaptureRequest capture,double startLon,double startLat,bool hop,double hopX,double hopY,saida::runtime::CaptureViewpoint view,bool sail,bool fly,bool headset):engine(e),game(g),smokeSail(sail),smokeFly(fly),vr(headset),pickLon(startLon),pickLat(startLat),mapCenterLon(startLon),mapCenterLat(startLat),smoke(test),hopLon(hopX),hopLat(hopY),hopWanted(hop),worldCapture(capture),captureView(view) {
    performanceSmoke=smoke&&std::getenv("R1WORLD_DEBUG_SMOKE");
    if(!perfSmoke.empty()&&(!smoke||(perfSmoke!="walk"&&perfSmoke!="drive")))
        throw std::runtime_error("R1WORLD_PERF_SMOKE requires --smoke and walk or drive");
    if(qualitySwitchSmoke&&(!smoke||perfSmoke!="walk"))
        throw std::runtime_error("R1WORLD_QUALITY_SWITCH_SMOKE requires --smoke and R1WORLD_PERF_SMOKE=walk");
    // Four physical patrol cars, their shared Scopia LODs, eight equipped
    // officers and regional lettering reserve room alongside tile detail.
    residentVertexBudget=size_t(double(e.resources().geometryCapacity().vertices)*kTileGeometryShare)-r1::police::kGeometryVertices;
    residentIndexBudget=size_t(double(e.resources().geometryCapacity().indices)*kTileGeometryShare)-r1::police::kGeometryIndices;
    ui=dynamic_cast<saida::WebCanvasNode*>(e.sceneTree().firstInGroup("world-ui"));
    minimapUi=dynamic_cast<saida::WebCanvasNode*>(e.sceneTree().firstInGroup("minimap-ui"));
    camera=dynamic_cast<saida::CameraNode*>(e.sceneTree().firstInGroup("camera"));
    if(!ui||!minimapUi||!camera)throw std::runtime_error("Earth scene is missing its camera, map or minimap");
    sceneFov=camera->fovDegrees;
#ifdef SAIDA_ENABLE_XR
    // First child of the world: the engine finds the rig by a depth-first
    // group walk every frame, which then stops at once.
    if(vr) {
        vrRig=static_cast<saida::XROrigin*>(e.sceneTree().world().addChildAt(std::make_unique<saida::XROrigin>(),0));
        // The tracked hands, in the same world space as the head. Skin is
        // a measured albedo (CLAUDE.md §2), not the engine's debug colours.
        for(auto side:{saida::XRHand::Left,saida::XRHand::Right}) {
            auto hand=std::make_unique<saida::XRHandNode>(side);
            hand->setColor({.34f,.24f,.19f,1.f});
            hand->buildVisuals(e.resources());
            e.sceneTree().world().addChildAt(std::move(hand),1);
        }
    }
#endif
    minimapUi->setEnabled(false);
    {
        std::ifstream input(game/"assets/world/timezone-countries.json");
        if(!input)throw std::runtime_error("Missing assets/world/timezone-countries.json");
        const json places=json::parse(input);
        zoneCountries=places.at("zones").get<std::map<std::string,std::string>>();
        countryNames=places.at("countries").get<std::map<std::string,std::string>>();
    }
    loadOptions();
    if(const char* requested=std::getenv("R1WORLD_GI_QUALITY")) {
        const auto end=std::end(giQualityKeys);
        const auto found=std::find(std::begin(giQualityKeys),end,std::string(requested));
        if(found==end)throw std::runtime_error("R1WORLD_GI_QUALITY must be off, auto, low, medium, high or ultra");
        giQuality=int(found-std::begin(giQualityKeys))-1;
    }
    applyGIQuality();
    engine.window().size(standardWidth,standardHeight);
    if(toasterMode)applyToasterResolution();
    player=e.sceneTree().firstInGroup("player");
    if(!player)throw std::runtime_error("Earth scene is missing the player");
    player->findBehavioursInChildren(animators);
    if(animators.empty())throw std::runtime_error("Player model has no Animator");
    loadHumans();
    loadAnimals();
    // The body is drawn at the people's scale; its height is the manifest's.
    for(auto& child:player->children())child->transform().scale=glm::vec3(float(humanScale));
    unifyPlayerBody();
    for(auto* a:animators) {
        for(auto clip:{"idle","run","sprint","jump"})
            if(!a->clips().count(clip))throw std::runtime_error(std::string("Player animation missing: ")+clip);
        a->play("idle");
        if(!a->rig())throw std::runtime_error("Player model has no skeleton");
        playerImpacts.push_back(a->addModifier<saida::ImpactModifier>(*a->rig(),impactSettings()));
        if(!playerImpacts.back()->valid())throw std::runtime_error("Player skeleton lacks the spine the stagger bends");
    }
    player->setEnabled(false);
    saida::Log::info("[World player] model ready, animators=",animators.size());
    car=e.sceneTree().firstInGroup("vehicle");
    if(!car)throw std::runtime_error("Earth scene is missing the player's car");
    car->setEnabled(false);
    for(auto& child:car->children())child->transform().scale=glm::vec3(float(kVehicleScale));
    // The kit names its four wheels, and the names are the contract: a
    // re-export that renamed them would leave a car sliding on frozen
    // wheels, which is a silent regression. It is said once, out loud.
    collectWheels(*car);
    saida::Log::info("[World car] model ready, wheels=",frontWheels.size()+rearWheels.size());
    // His feet among people: the engine's character body, a capsule as
    // wide as theirs and as tall as him, its position where he stands.
    feet=e.sceneTree().world().createChild<saida::CharacterBodyNode>();
    feet->setName("player-feet");
    {
        auto capsule=std::make_unique<saida::CollisionShapeNode>();
        capsule->shapeType=saida::CollisionShapeType::Capsule;
        capsule->radius=float(r1::Crowd::kBodyRadius);
        capsule->height=float(playerHeight);
        capsule->axis=1;
        capsule->offset=glm::vec3(0.f,float(playerHeight*.5),0.f);
        feet->addChild(std::move(capsule));
    }
    feet->setEnabled(false);
    setupCombat();
    prototypes=e.sceneTree().world().createChild<saida::Node>("Shared prototypes");
    prototypes->setEnabled(false);
    loadPaints();
    buildTrafficPrototype();vehicleCollider(*car,vehicleSpec(*car));
    buildAircraftPrototypes();
    saida::Log::info("[World traffic] ready, ",paints.size()," paints");
    applyGpuBudget();
    r1::WorldService::Options options;
    options.gameRoot=game.string();
    // Nine visible tiles plus three forecast tiles. The former 36-tile
    // cache retained gigabytes of prepared Paris geometry off screen.
    options.keep=12;
    options.requestLimit=kStreamRequestLimit;
    options.availableMemory=[] { return saida::MemoryProfiler::availableHostBytes(); };
    // Nine tiles can be resident together. Leave 5% of their share for
    // landmarks that remain visible across a tile boundary.
    options.tileVertexTarget=std::min(r1::kTileVertexBudget,residentVertexBudget*95/900);
    options.enrichRetail=true;
    options.fetchCanopy=true;
    options.prepare=[](r1::ServedTile& tile){tile.prepared=prepareTile(tile.cooked);releaseCookedGeometry(tile.cooked);};
    options.log=[](const std::string& line){saida::Log::info("[World service] ",line);};
    service=std::make_unique<r1::WorldService>(std::move(options));
    sea=std::make_unique<r1::SeaService>(game.string(),[](const std::string& line){saida::Log::info("[World sea] ",line);});
    sky=std::make_unique<r1::ConditionsService>(game.string(),[](const std::string& line){saida::Log::info("[World sky] ",line);});
    readFarLandmarks();
    e.window().setCursorCaptured(false);
}

World::Impl::~Impl(){saveAnimalRemains(true);animalWriter.flush();saveAnimalRemains(true);animalWriter.flush();
    if(!animalWriter.error().empty())animalSaveError(animalWriter.error());clearPolice();for(auto* e:listeners)if(ui->isLiveElement(e,generation)){
    for(const char* type:{"click","mousedown","mouseup","mousemove","mousescroll","dblclick"})e->RemoveEventListener(type,this);}}

void World::Impl::setMenuGlobe(std::shared_ptr<r1::MenuGlobeState> model) {
    globe=std::move(model);globeTarget=globe->view;globe->toaster=toasterMode;
}

bool World::Impl::failed() const {return testFailed;}

void World::Impl::diveSmoke() {
    if(!smoke)throw std::runtime_error("--dive requires --smoke");
    smokeDive=true;
    // The dive skips smokeOptions(), so apply its test preset here too.
    if(const char* quality=std::getenv("R1WORLD_SMOKE_QUALITY")) {
        const std::string preset=quality;
        if(preset!="standard"&&preset!="toaster")throw std::runtime_error("R1WORLD_SMOKE_QUALITY must be standard or toaster");
        setToasterMode(preset=="toaster");
    }
    // R1WORLD_DIVE_HOUR: the local hour the pictures are taken at, held
    // for this run only (World::Impl::forceTime would save it as the player's).
    if(const char* hour=std::getenv("R1WORLD_DIVE_HOUR")) {
        const double h=std::atof(hour);
        if(!std::isfinite(h)||h<0||h>=24)throw std::runtime_error("R1WORLD_DIVE_HOUR must be an hour in [0,24)");
        forcedMinutes=int(h*60);
        saida::Log::info("[World E2E] the arrival is flown at ",r1::formatClockTime(*forcedMinutes)," local");
    }
}

void World::Impl::inspectInstant(double unixSeconds) {inspectAt=unixSeconds;}

void World::Impl::inspectConditions(std::array<double,3> conditions) {
    if(!inspectAt||worldCapture.pngPath.empty())throw std::runtime_error("--weather requires --at and --screenshot");
    inspectWeather=conditions;
}

void World::Impl::inspectHeat(double temperature) {
    if(!inspectAt||worldCapture.pngPath.empty()||!std::isfinite(temperature)||temperature < -100||temperature>65)
        throw std::runtime_error("--temperature requires --at, --screenshot and a finite temperature in [-100,65]");
    inspectTemperature=temperature;
}

void World::Impl::captureFromAltitude(double metres) {captureAltitude=metres;}

void World::Impl::captureAtGeography(std::array<double,3> camera) {captureGeo=camera;}

} // namespace r1::world
