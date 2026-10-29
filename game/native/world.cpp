// Application entry point. Session implementation: native/world/.
#include "world/world.hpp"
#include "world/geography.hpp"
#include "world/limits.hpp"
#include "Engine.hpp"
#include "core/Log.hpp"
#include "core/Time.hpp"
#include "scene/SceneSerializer.hpp"
#include "graphics/ResourceManager.hpp"
#include "runtime/CaptureArgs.hpp"
#include "runtime/ProfileArgs.hpp"
#include "gen/palette.hpp"
#include "gen/fauna.hpp"
#include "menu_globe.hpp"
#ifdef SAIDA_ENABLE_XR
#include "saida/xrgesture/OpenXRInput.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace r1::world;

int main(int argc,char** argv) {
    try {
        if(argc>1&&std::string(argv[1])=="--geo-contract") {
            json result=json::array();
            for(double la:{-90.,-89.999,-33.,0.,48.8566,89.999,90.})for(double lo:{-180.,-179.999,2.3522,179.999,180.}) {
                Frame a(lo,la,123.);auto p=ecef(wrap(lo+.001),std::clamp(la+.001,-90.,90.),456.);
                auto q=a.origin+a.basis*a.local(p);auto step=advance(lo,la,500,500);
                result.push_back({{"lon",lo},{"lat",la},{"tile",tileAt(lo,la).key()},
                                  {"roundTripError",glm::length(p-q)},{"step",{step.x,step.y}}});
            }
            std::cout<<result.dump()<<std::endl;return 0;
        }
        fs::path game=fs::absolute(fs::path(argv[0])).parent_path().parent_path().parent_path();
        bool smoke=false,sail=false,fly=false,vr=false;double startLon=2.3522,startLat=48.8566;bool hop=false;double hopLon=0,hopLat=0;
        for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--project"&&i+1<argc)game=fs::absolute(argv[++i]);else if(a=="--smoke")smoke=true;else if(a=="--sail")sail=true;else if(a=="--fly")fly=true;else if(a=="--vr")vr=true;else if(a=="--spawn"&&i+2<argc){startLon=std::stod(argv[++i]);startLat=std::stod(argv[++i]);}else if(a=="--spawn2"&&i+2<argc){hop=true;hopLon=std::stod(argv[++i]);hopLat=std::stod(argv[++i]);}}
        if(!std::isfinite(startLon)||!std::isfinite(startLat)||std::abs(startLat)>90||std::abs(startLon)>180)throw std::runtime_error("Invalid --spawn coordinate");
        // This development executable uses the existing engine's baked paths
        // for shaders/fonts, and the project's root for content.
        saida::PhysicsCapacity physics;physics.bodies=kPhysicsBodies;
        auto menuGlobe=r1::registerMenuGlobe();
        #ifndef SAIDA_ENABLE_XR
        if(vr)throw std::runtime_error("--vr: this build of the engine has no OpenXR (SAIDA_ENABLE_XR=OFF)");
#endif
        std::vector<std::shared_ptr<saida::xr::InputExtension>> xrInputs;
#ifdef SAIDA_ENABLE_XR
        std::shared_ptr<saida::xrgesture::MetaMicrogestureInput> gestureInput;
        if(vr) {gestureInput=std::make_shared<saida::xrgesture::MetaMicrogestureInput>();xrInputs.push_back(gestureInput);}
#endif
        saida::Engine engine(nullptr,(game/"R1World.saidaproj").string(),vr,{},physics,std::move(xrInputs));
        engine.resources().assetLoader().setBudget(kDecodedAssetBudget);
        if(!saida::SceneSerializer::loadIntoScene(engine.scene(),engine.resources(),(game/"scenes/earth.scene").string()))throw std::runtime_error("Cannot load Earth scene");
        engine.mountWorld();saida::Time::setScale(1);
        saida::CaptureRequest capture;saida::runtime::CaptureViewpoint view;std::string error;
        if(!saida::runtime::parseCaptureArgs(argc,argv,capture,view,error))throw std::runtime_error(error);
        engine.setCameraFovOverride(view.fovDegrees);
        std::string profile;
        if(!saida::runtime::parseProfileArgs(argc,argv,profile,error))throw std::runtime_error(error);
        engine.profileTo(profile);
        // The Atlas, the ground classes and the surfaces: data the generator
        // reads before any tile is cooked, and refuses to run without.
        r1::loadPalette(game.string());
        r1::loadFauna(game.string());
        World world(engine,game,smoke,capture,startLon,startLat,hop,hopLon,hopLat,view,sail,fly,vr);
#ifdef SAIDA_ENABLE_XR
        if(gestureInput)world.setXrGestureInput(gestureInput->state());
#endif
        world.setMenuGlobe(menuGlobe);
        for(int i=1;i<argc;++i)if(std::string(argv[i])=="--dive")world.diveSmoke();
        for(int i=1;i+1<argc;++i)if(std::string(argv[i])=="--at") {
            const double at=std::stod(argv[i+1]);
            if(!std::isfinite(at))throw std::runtime_error("Invalid --at instant");
            world.inspectInstant(at);
        }
        for(int i=1;i+1<argc;++i)if(std::string(argv[i])=="--camera-altitude") {
            const double metres=std::stod(argv[i+1]);
            if(!std::isfinite(metres))throw std::runtime_error("Invalid --camera-altitude");
            world.captureFromAltitude(metres);
        }
        for(int i=1;i<argc;++i)if(std::string(argv[i])=="--camera-geo") {
            if(i+3>=argc)throw std::runtime_error("--camera-geo requires longitude latitude eye-height-above-ground");
            std::array<double,3> geo{std::stod(argv[i+1]),std::stod(argv[i+2]),std::stod(argv[i+3])};
            if(!std::isfinite(geo[0])||!std::isfinite(geo[1])||!std::isfinite(geo[2])||
               std::abs(geo[0])>180||std::abs(geo[1])>90||geo[2]<=0||geo[2]>10000)
                throw std::runtime_error("Invalid --camera-geo coordinate or eye height");
            if(!smoke||capture.pngPath.empty()||!view.set)throw std::runtime_error("--camera-geo requires --smoke, --screenshot and a capture viewpoint");
            world.captureAtGeography(geo);
            saida::Log::info("[World inspection] fixed camera longitude=",std::setprecision(12),geo[0]," latitude=",geo[1]," eye_agl_m=",geo[2]);
        }
        for(int i=1;i<argc;++i)if(std::string(argv[i])=="--weather") {
            if(i+3>=argc)throw std::runtime_error("--weather requires cloud fraction, rain mm/h and visibility metres");
            std::array<double,3> conditions{};
            for(size_t k=0;k<conditions.size();++k) {
                const std::string value=argv[i+1+int(k)];size_t end=0;
                conditions[k]=std::stod(value,&end);
                if(end!=value.size()||!std::isfinite(conditions[k]))throw std::runtime_error("Invalid --weather value");
            }
            if(conditions[0]<0.||conditions[0]>1.||conditions[1]<0.||conditions[2]<0.)
                throw std::runtime_error("--weather requires cloud in [0,1], nonnegative rain and visibility");
            world.inspectConditions(conditions);
        }
        for(int i=1;i<argc;++i)if(std::string(argv[i])=="--temperature") {
            if(i+1>=argc)throw std::runtime_error("--temperature requires degrees Celsius");
            std::string value=argv[i+1];size_t end=0;double temperature=std::stod(value,&end);
            if(end!=value.size())throw std::runtime_error("Invalid --temperature value");
            world.inspectHeat(temperature);
        }
        const saida::Engine::FrameFn followPhysics=[&](float dt){world.afterSimulation(dt);};
        engine.setOnFrame([&](float dt){
            world.update(dt);
            // Driving and a photo exposure need the pose after physics. An empty hook also
            // skips the engine's full transform propagation for that stage.
            engine.setAfterSimulation(world.needsAfterSimulation()?followPhysics:saida::Engine::FrameFn{});
        });
        if(!smoke&&!capture.pngPath.empty())engine.captureFrameThenExit(capture);
        engine.run();return engine.captureFailed()||world.failed()?1:0;
    }catch(const std::exception& e){saida::Log::error("R1World: ",e.what());return 1;}
}
