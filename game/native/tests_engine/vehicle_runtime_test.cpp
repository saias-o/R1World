#include "../vehicle_runtime.hpp"
#include "../traffic_signal.hpp"
#include "physics/StaticBodyNode.hpp"
#include "scene/Scene.hpp"
#include "graphics/Mesh.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace {
// Keep the original acceleration/saturation requirements as fractions of the
// configured limit. The city profile was retuned from 28 to 24 m/s; absolute
// 26 m/s assertions would then require exceeding its own speed limiter.
constexpr float kRoadSpeedFraction=26.f/28.f;
constexpr float kThreeSecondSpeedFraction=23.f/28.f;
void require(bool pass,const char* message) {
    if(!pass){std::fprintf(stderr,"FAIL vehicle-runtime: %s\n",message);std::exit(1);}
}
void floor(saida::Scene& scene,glm::vec3 centre,glm::quat rotation,glm::vec3 half) {
    auto* body=scene.createChild<saida::StaticBodyNode>();
    body->transform().position=centre;body->transform().rotation=rotation;
    auto* shape=body->createChild<saida::CollisionShapeNode>();
    shape->shapeType=saida::CollisionShapeType::Box;shape->halfExtents=half;
}
struct Rig {
    saida::Scene scene;
    saida::Node* root=scene.createChild<saida::Node>();
    saida::RigidBodyNode* body=nullptr;
    saida::SurfaceDriveBehaviour* drive=nullptr;
    r1::VehicleDimensions dimensions{};
    Rig(glm::vec3 position,glm::quat rotation,const char* kind="city") {
        root->transform().position=position;root->transform().rotation=rotation;
        static const auto fleet=[] {
            std::ifstream source(std::filesystem::path(__FILE__).parent_path()/"../../assets/models/vehicles/fleet.json");
            return nlohmann::json::parse(source).at("vehicles");
        }();
        const auto entry=std::find_if(fleet.begin(),fleet.end(),[&](const auto& item){return item.at("name")==kind;});
        require(entry!=fleet.end(),"test profile exists in the actual fleet manifest");
        constexpr float kGameVehicleScale=.68f;
        dimensions={entry->at("length").template get<float>()*kGameVehicleScale,
            entry->at("width").template get<float>()*kGameVehicleScale,entry->at("height").template get<float>()*kGameVehicleScale,
            entry->at("wheelbase").template get<float>()*kGameVehicleScale,entry->at("wheelRadius").template get<float>()*kGameVehicleScale};
        r1::VehicleRuntime::install(*root,dimensions,kind);
        body=r1::VehicleRuntime::body(*root);body->kinematic=false;
        drive=r1::VehicleRuntime::driver(*root);drive->parked=false;drive->command(0,1,0);
    }
    void step(float seconds,int rate=60,float powerLoss=-1.f) {
        for(int i=0;i<int(seconds*rate);++i) {
            r1::VehicleRuntime::synchronize(*root);
            if(powerLoss>=0.f)drive->damage=powerLoss;
            scene.update(1.f/rate);
        }
    }
    glm::vec3 position()const {
        glm::vec3 position;glm::quat rotation;
        body->physicsWorld()->getBodyTransform(body->bodyId(),position,rotation);return position;
    }
};
void handling() {
    require(r1::VehicleRuntime::bodyworkPowerLoss(.5f)==0.f,"ordinary dents retain full engine power");
    require(r1::VehicleRuntime::bodyworkPowerLoss(100.f)<=.35f,"cumulative cosmetic energy cannot kill the engine");
    for(const char* kind:{"city","sedan","suv","offroad","sport","truck","bus"})
        for(float degrees:{0.f,22.f})for(float damage:{0.f,.5f,1.f}) {
            const auto rotation=glm::angleAxis(glm::radians(degrees),glm::vec3(1,0,0));
            Rig rig({0,.06f,0},rotation,kind);
            floor(rig.scene,rotation*glm::vec3(0,-1,0),rotation,{500,1,500});
            rig.step(1);rig.drive->command(1,0,0);
            rig.step(5,60,r1::VehicleRuntime::bodyworkPowerLoss(damage));
            std::printf("handling %s slope=%.0f damage=%.1f speed=%.2f m/s\n",kind,degrees,damage,rig.drive->signedSpeed());
            const bool heavy=std::string(kind)=="truck"||std::string(kind)=="bus";
            require(rig.drive->signedSpeed()>(heavy?4.f:12.f),"every shipped model accelerates uphill despite bodywork damage");
            if(std::string(kind)=="city"&&damage==0.f)
                require(rig.drive->signedSpeed()>r1::vehicleDynamics(kind).maximumSpeed*kRoadSpeedFraction,"compact reaches road speed promptly on flat ground and hills");
            rig.drive->command(0,1,0);rig.step(5);const auto stopped=rig.position();
            rig.drive->command(1,0,0);rig.step(3,60,r1::VehicleRuntime::bodyworkPowerLoss(damage));
            require(stopped.z-rig.position().z>(heavy?1.f:8.f),"all models restart uphill after braking, including damaged cars");
        }
}
void hills() {
    for(int rate:{30,60,120})for(float degrees:{8.f,15.f,22.f}) {
        const auto rotation=glm::angleAxis(glm::radians(degrees),glm::vec3(1,0,0));
        Rig rig({0,.06f,0},rotation);
        floor(rig.scene,rotation*glm::vec3(0,-1,0),rotation,{100,1,100});
        rig.step(2,rate);const auto start=rig.position();
        rig.drive->command(1,0,0);rig.step(4,rate);const auto moving=rig.position();
        require(start.z-moving.z>(degrees<20.f?10.f:5.f),"the shipped car climbs from rest");
        rig.drive->command(0,1,0);rig.step(3,rate);const auto stopped=rig.position();
        rig.step(3,rate);
        require(glm::distance(stopped,rig.position())<.2f,"brakes hold the shipped car on a hill");
        rig.drive->command(1,0,0);rig.step(3,rate);
        require(stopped.z-rig.position().z>5,"the shipped car restarts on a hill");
        std::printf("PASS hill %.0f degrees at %d Hz\n",degrees,rate);
    }
}
void flatPerformance() {
    const float speedLimit=r1::vehicleDynamics("city").maximumSpeed;
    float referenceDistance=0.f;
    for(int rate:{10,15,30,60,90,120}) {
        Rig rig({0,.06f,0},glm::quat(1,0,0,0));
        floor(rig.scene,{0,-1,0},glm::quat(1,0,0,0),{500,1,500});
        rig.step(1,rate);rig.drive->command(1,0,0);rig.step(3,rate);
        require(rig.drive->signedSpeed()>speedLimit*kThreeSecondSpeedFraction,"compact reaches its configured three-second acceleration target");
        if(rate==10)referenceDistance=rig.position().z;
        else require(std::abs(referenceDistance-rig.position().z)<.2f,"game handling agrees across low FPS, desktop and VR frame rates");
        rig.step(3,rate);
        require(rig.drive->signedSpeed()>speedLimit*kRoadSpeedFraction&&rig.drive->signedSpeed()<=speedLimit+.1f,"road speed retains the configured speed limit");
        rig.drive->command(0,1,0);rig.step(3,rate);
        require(std::abs(rig.drive->signedSpeed())<.1f,"service brake stops the compact without reversing");
        std::printf("PASS flat response/braking at %d Hz\n",rate);
    }
}
void ramp() {
    constexpr float kRampDegrees=15.f;
    const auto slope=glm::angleAxis(glm::radians(kRampDegrees),glm::vec3(1,0,0));
    Rig rig({0,.06f,8},glm::quat(1,0,0,0));
    floor(rig.scene,{0,-1,50},glm::quat(1,0,0,0),{10,1,50});
    floor(rig.scene,slope*glm::vec3(0,-1,-50),slope,{10,1,50});
    rig.step(1);rig.drive->command(1,0,0);rig.step(6);
    std::printf("ramp position %.2f %.2f %.2f, grounded=%d speed=%.2f\n",rig.position().x,rig.position().y,rig.position().z,rig.drive->grounded(),rig.drive->signedSpeed());
    require(rig.position().z<-20&&rig.position().y>4,"rounded wheel contacts traverse a flat-to-ramp entry");
}
void sustainedHills() {
    bool passed=true;
    for(const char* kind:{"city","sedan","suv","offroad","sport","truck","bus"}) {
        float flatSpeed=0.f;
        for(float degrees:{0.f,15.f,22.f}) {
            const auto rotation=glm::angleAxis(glm::radians(degrees),glm::vec3(1,0,0));
            Rig rig({0,.06f,0},rotation,kind);
            floor(rig.scene,rotation*glm::vec3(0,-1,0),rotation,{2000,1,2000});
            rig.step(1);rig.drive->command(1,0,0);rig.step(20);
            const float speed=rig.drive->signedSpeed();
            if(degrees==0.f)flatSpeed=speed;
            const float ratio=speed/std::max(1.f,flatSpeed);
            std::printf("sustained %s slope=%.0f speed=%.2f flat-ratio=%.3f\n",kind,degrees,speed,ratio);
            // The game's powered grade assistance promises comparable road
            // speed, including heavy vehicles, not merely motion up the hill.
            passed=passed&&ratio>=.98f&&ratio<=1.02f;
        }
    }
    require(passed,"powered vehicles retain flat-road cruising speed on sustained hills");
}
void treeImpacts() {
    bool passed=true;
    for(const char* kind:{"city","sedan","suv","offroad","sport","truck","bus"})
        for(float speed:{8.f,14.f})for(float overlap:{0.f,.45f}) {
            Rig rig({0,.06f,15},glm::quat(1,0,0,0),kind);
            floor(rig.scene,{0,-1,0},glm::quat(1,0,0,0),{200,1,200});
            // Match World::addTrunk: a stationary .46 m wide, 4 m tall trunk.
            floor(rig.scene,{rig.dimensions.width*overlap,2,0},glm::quat(1,0,0,0),{.23f,2,.23f});
            rig.step(1);const float restingHeight=rig.position().y;
            rig.drive->command(0,0,0);rig.body->setLinearVelocity({0,0,-speed});
            float peakLift=0,peakUpSpeed=0,peakAngularSpeed=0,peakSpeed=0,peakEnergy=0,minimumUp=1;
            bool collided=false;
            const auto impactConnection=rig.body->impacted.connect([&](const saida::CollisionObjectNode::Impact& impact) {
                if(std::abs(impact.direction.y)<.5f&&impact.energy>100.f)collided=true;
            });
            for(int frame=0;frame<360;++frame) {
                rig.step(1.f/60.f);
                glm::vec3 position;glm::quat rotation;
                auto* physics=rig.body->physicsWorld();
                physics->getBodyTransform(rig.body->bodyId(),position,rotation);
                const auto velocity=physics->linearVelocity(rig.body->bodyId());
                const auto angular=physics->angularVelocity(rig.body->bodyId());
                const float lift=std::max(0.f,position.y-restingHeight);
                peakLift=std::max(peakLift,lift);peakUpSpeed=std::max(peakUpSpeed,velocity.y);
                peakAngularSpeed=std::max(peakAngularSpeed,glm::length(angular));
                peakSpeed=std::max(peakSpeed,glm::length(velocity));
                minimumUp=std::min(minimumUp,(rotation*glm::vec3(0,1,0)).y);
                // Translational + potential energy is a lower bound; increases
                // without throttle reveal energy injection even without inertia access.
                peakEnergy=std::max(peakEnergy,rig.body->mass*(.5f*glm::dot(velocity,velocity)+9.81f*lift));
            }
            rig.drive->command(0,1,0);rig.step(2);
            const auto stopped=rig.position();
            rig.drive->command(-1,0,0);rig.step(3);
            const float recoverySpeed=rig.drive->signedSpeed();
            const float recoveryDistance=glm::distance(stopped,rig.position());
            const bool stable=collided&&peakLift<.75f&&peakUpSpeed<3.f&&minimumUp>.5f&&peakAngularSpeed<5.f&&
                peakSpeed<=speed+.5f&&peakEnergy<=rig.body->mass*.5f*speed*speed*1.1f&&
                recoverySpeed<-2.f&&recoveryDistance>2.f;
            std::printf("%s tree %s impact=%.0f offset=%.2f lift=%.2f up=%.2f min-up=%.2f spin=%.2f "
                "speed=%.2f energy=%.0f J reverse=%.2f distance=%.2f contact=%d\n",stable?"PASS":"FAIL",kind,
                speed,overlap,peakLift,peakUpSpeed,minimumUp,peakAngularSpeed,peakSpeed,peakEnergy,
                recoverySpeed,recoveryDistance,collided);
            passed=passed&&stable;
        }
    require(passed,"front and offset tree impacts dissipate energy, stay upright and allow reversing away");
}
void motorcycles() {
    for(const char* kind:{"moto_city","moto_cross"})for(int rate:{30,60,120})for(float steer:{-1.f,1.f}) {
        Rig rig({0,.06f,0},glm::quat(1,0,0,0),kind);
        floor(rig.scene,{0,-1,0},glm::quat(1,0,0,0),{500,1,500});
        auto* rider=rig.body->createChild<saida::Node>();rider->transform().position={0,.5f,.2f};
        rig.step(1,rate);const auto start=rig.position();
        rig.drive->command(1,0,0);rig.step(3,rate);
        require(rig.position().z<start.z-15.f&&rig.drive->signedSpeed()>15.f,"motorcycles accelerate towards local -Z");
        rig.drive->command(.7f,0,steer*.5f);rig.step(2,rate);
        glm::vec3 at;glm::quat rotation;
        rig.body->physicsWorld()->getBodyTransform(rig.body->bodyId(),at,rotation);
        const auto forward=rotation*glm::vec3(0,0,-1),up=rotation*glm::vec3(0,1,0);
        const auto lateral=glm::normalize(glm::cross(forward,glm::vec3(0,1,0)));
        const float lean=glm::dot(up,lateral)*steer;
        std::printf("motorcycle %s %dHz steer=%.0f speed=%.2f lean=%.3f up=%.3f\n",kind,rate,steer,rig.drive->signedSpeed(),lean,up.y);
        require(up.y>.65f&&lean>.02f,"motorcycles bank into the supported turn without overturning");
        rig.scene.update(0.f);
        const auto expected=at+rotation*rider->transform().position;
        require(glm::distance(glm::vec3(rider->worldTransform()[3]),expected)<.001f,"body-attached rider follows the completed solver pose");
        rig.drive->command(0,1,0);rig.step(5,rate);
        require(std::abs(rig.drive->signedSpeed())<.5f,"both motorcycle profiles brake to a stop");
        rig.step(2,rate);
        rig.body->physicsWorld()->getBodyTransform(rig.body->bodyId(),at,rotation);
        require((rotation*glm::vec3(0,1,0)).y>.95f,"stopped motorcycle returns upright");
        rig.drive->command(-1,0,0);rig.step(3,rate);
        require(rig.drive->signedSpeed()<-.5f&&rig.drive->signedSpeed()>-2.5f,"reverse remains a controlled walking manoeuvre");
    }
}
void geometry() {
    for(const auto& mesh:{r1::traffic_signal::pole(),r1::traffic_signal::housing(),r1::traffic_signal::lens()}) {
        require(!mesh.indices.empty()&&mesh.vertices.size()<2500,"signal meshes stay within their shared geometry budget");
        for(size_t i=0;i<mesh.indices.size();i+=3) {
            const auto& a=mesh.vertices[mesh.indices[i]];const auto& b=mesh.vertices[mesh.indices[i+1]];const auto& c=mesh.vertices[mesh.indices[i+2]];
            require(glm::dot(glm::cross(b.pos-a.pos,c.pos-a.pos),a.normal)>0,"signal triangles face their authored normals");
        }
    }
}
void recovery() {
    for(const char* kind:{"city","suv","bus"})for(float roll:{90.f,-90.f,180.f}) {
        Rig rig({0,3,0},glm::angleAxis(glm::radians(roll),glm::vec3(0,0,1)),kind);
        floor(rig.scene,{0,-1,0},glm::quat(1,0,0,0),{100,1,100});
        rig.drive->command(0,0,0);rig.step(3);
        glm::vec3 p;glm::quat before;
        rig.body->physicsWorld()->getBodyTransform(rig.body->bodyId(),p,before);
        require(!r1::VehicleRuntime::stableForTraffic(*rig.root),"overturned cars keep physical authority");
        rig.drive->command(0,0,1);rig.step(1.f/60.f);
        glm::quat first;rig.body->physicsWorld()->getBodyTransform(rig.body->bodyId(),p,first);
        require(std::abs(glm::dot(first,before))>.99f,"direction input rights progressively, without teleporting rotation");
        rig.step(6);
        glm::quat after;rig.body->physicsWorld()->getBodyTransform(rig.body->bodyId(),p,after);
        const float up=(after*glm::vec3(0,1,0)).y;
        std::printf("recovery %s roll=%.0f up=%.3f\n",kind,roll,up);
        require(up>.9f&&rig.drive->grounded(),"holding a direction recovers a car from its side or roof");
        rig.drive->command(1,0,0);rig.step(3);
        require(rig.drive->signedSpeed()>3.f,"a recovered vehicle can drive away");
    }
    Rig rig({0,.06f,0},glm::quat(1,0,0,0));
    floor(rig.scene,{0,-1,0},glm::quat(1,0,0,0),{100,1,100});rig.step(1);
    require(r1::VehicleRuntime::stableForTraffic(*rig.root),"settled cars can return to distant traffic");
    rig.body->physicsWorld()->setAngularVelocity(rig.body->bodyId(),{0,2,0});
    require(!r1::VehicleRuntime::stableForTraffic(*rig.root),"spinning cars cannot snap back to a lane");
    for(float roll:{0.f,90.f,180.f})for(const auto direction:{glm::vec3(1,0,0),glm::vec3(-1,0,0),glm::vec3(0,1,0)}) {
        const auto rotation=glm::angleAxis(glm::radians(roll),glm::vec3(0,0,1));
        const auto point=r1::VehicleRuntime::exitOffset(rig.dimensions,rotation,direction);
        const auto local=glm::inverse(rotation)*point;
        const bool outside=std::abs(local.x)>rig.dimensions.width*.5f+.3f||
            local.y>rig.dimensions.height+.3f||local.y<-.3f||std::abs(local.z)>rig.dimensions.length*.5f+.3f;
        require(outside,"exit point clears the chassis upright, sideways and on its roof");
    }
}
void roadSmoothness() {
    for(int rate:{30,60,90,120}) {
        Rig rig({0,.06f,5},glm::quat(1,0,0,0));
        auto data=std::make_shared<saida::MeshCollisionData>();
        for(int row=0;row<=800;++row) {
            const float z=10.f-row*.5f,y=.005f*std::sin(z*2.f);
            data->positions.push_back({-5,y,z});data->positions.push_back({5,y,z});
            if(row>0) {
                const uint32_t a=(row-1)*2,b=row*2;
                data->indices.insert(data->indices.end(),{a,a+1,b,b,a+1,b+1});
            }
        }
        auto* road=rig.scene.createChild<saida::StaticBodyNode>();
        road->createChild<saida::CollisionShapeNode>()->setTriangleMeshData(data);
        rig.step(1,rate);rig.drive->command(1,0,0);
        float maxVertical=0,maxTiltSpin=0;
        for(int frame=0;frame<6*rate;++frame) {
            rig.step(1.f/rate,rate);
            const auto v=rig.body->physicsWorld()->linearVelocity(rig.body->bodyId());
            const auto w=rig.body->physicsWorld()->angularVelocity(rig.body->bodyId());
            maxVertical=std::max(maxVertical,std::abs(v.y));
            maxTiltSpin=std::max(maxTiltSpin,std::hypot(w.x,w.z));
        }
        std::printf("road mesh %d Hz vertical=%.3f tilt-spin=%.3f speed=%.2f\n",rate,maxVertical,maxTiltSpin,rig.drive->signedSpeed());
        require(maxVertical<.8f&&maxTiltSpin<1.f&&rig.drive->signedSpeed()>r1::vehicleDynamics("city").maximumSpeed*kRoadSpeedFraction,
            "small triangle-road irregularities do not amplify into chassis vibration or stall driving");
    }
}
}
int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc>1&&std::string(argv[1])=="--motorcycles"){motorcycles();return 0;}
    if(argc>1&&std::string(argv[1])=="--tree-impacts"){treeImpacts();return 0;}
    if(argc>1&&std::string(argv[1])=="--sustained-hills"){sustainedHills();return 0;}
    if(argc>1&&std::string(argv[1])=="--recovery"){recovery();return 0;}
    if(argc>1&&std::string(argv[1])=="--road"){roadSmoothness();return 0;}
    motorcycles();handling();flatPerformance();hills();ramp();sustainedHills();treeImpacts();recovery();roadSmoothness();geometry();
    std::puts("PASS vehicle-runtime handling, hills, braking, ramp entry, tree impacts and signal geometry");
}
