#pragma once
#include "geography.hpp"
#include "tile_preparation.hpp"
#include "gen/service.hpp"
#include "gen/interiors.hpp"
#include "gen/crowd.hpp"
#include "gen/fauna.hpp"
#include "bird_animation.hpp"
#include "knockdown.hpp"
#include "saida/traffic/Traffic.hpp"
#include "saida/wildlife/Wildlife.hpp"
#include "nodes/GrassNode.hpp"
#include "nodes/WaterNode.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "physics/StaticBodyNode.hpp"
#include <chrono>
#include <nlohmann/json.hpp>

namespace saida {
class Animator;
class RigidBodyNode;
class MeshNode;
class Material;
}

namespace r1::world {
using json = nlohmann::json;
struct Plant {
    saida::Node* node=nullptr;
    bool grass=false;
    bool tree=false;
    bool understory=false;
    // A tree with a middle level: its full model is allowed only close by.
    saida::LODGroupBehaviour* lod=nullptr;
    bool close=false;
};
// One animal as drawn (World::makeBeast): a pooled node with its own animator
// over one model's shared meshes, rig and clips.
struct Beast {
    saida::Node* node=nullptr; saida::Node* visual=nullptr;
    saida::Animator* animator=nullptr; saida::RigidBodyNode* body=nullptr;
    r1::BirdPose* birdPose=nullptr;
    std::string model,clip; float playback=-1.f,poseRate=-1.f;
    uint64_t identity=0; double yaw=0,y=0; bool shown=false;
};
// An animal indoors (gen/fauna.cpp planIndoor), in its room's node.
struct IndoorBeast {
    Beast beast; size_t animal=0; int member=0;
    double clock=0,offset=0,dartClock=0; bool darting=false,gone=false;
};
struct LiveInterior {
    r1::InteriorPlan plan;
    r1::InteriorLayout layout;
    saida::Node* node=nullptr;
    saida::Node* leaves[2]{nullptr,nullptr};
    saida::Node* closedDoor=nullptr;
    r1::P2 low{1e30,1e30},high{-1e30,-1e30};
    double opening=0,hold=0;
    bool refused=false,ready=false;
    std::vector<r1::IndoorAnimal> animals; std::vector<IndoorBeast> beasts; bool animalsPlaced=false;
    bool contains(r1::P2 at) const {
        return at.x>=low.x&&at.x<=high.x&&at.y>=low.y&&at.y<=high.y&&r1::pointInPolygon(at,plan.ring);
    }
};
struct StoreParking { std::string name;std::vector<r1::Ring> rings; };
// A walkable deck over the water -- a pier -- in its tile's frame.
struct Deck {
    std::vector<glm::dvec2> points;
    glm::dvec2 low{1e30},high{-1e30};
    double y=0;
};
// A piece of road off the ground (gen/bridges.hpp), in its tile's frame: the
// axis a -> b at the road surface, the half width it carries, and whether it
// is an embankment (solid down to the ground, its slopes walkable) or a deck
// (passed under).
struct RaisedPiece {
    glm::dvec2 a{0},b{0}; double ya=0,yb=0; bool solid=false;
    double half[2]{0,0}; bool rail[2]{false,false};  // right of a -> b, then left
    glm::dvec2 low{1e30},high{-1e30};
};
// A moored boat as the manifest lists it; `node` is found once it has streamed.
struct Mooring {
    std::string name,kind,model;
    glm::dvec3 local{0};
    double heading=0,length=8,beam=3,top=10,accel=2,turn=30;
    saida::Node* node=nullptr;
    bool taken=false;
};
// An aircraft parked by the generator, as the manifest lists it.
struct AircraftSpot {
    std::string name,type;
    glm::dvec3 local{0};
    double heading=0;
    saida::Node* node=nullptr;
    bool taken=false;
};
// Ownership follows the vehicle node through traffic handoffs and parking.
struct VehicleOccupant : saida::Behaviour { bool escaped=false; };

// One of the crowd, as drawn: a pooled node with its own animator, the
// shared meshes of one avatar under it (World::makePerson). Over the clip,
// the engine's two modifiers: the head turned toward the player, and the
// spine's stagger when he walks into them.
struct Person {
    saida::Node* node=nullptr; saida::Animator* animator=nullptr;
    saida::CollisionObjectNode* body=nullptr;  // their capsule in the engine's physics
    saida::GazeModifier* gaze=nullptr; saida::ImpactModifier* impact=nullptr;
    const char* clip=""; float poseRate=-1.f;
    uint32_t bumps=0;            // the walker's bumps already staggered
    double yaw=0; bool shown=false;  // the heading drawn, eased toward the walker's
    size_t kind=0;
    // A fight (World::knockDown, World::personAttack): the engine's ragdoll
    // that takes the body when it falls, the arms that punch, and the clock
    // of the punch thrown at the player and of the time spent lying still.
    r1::Knockdown fall;
    saida::TwoBoneIKModifier* arms[2]{nullptr,nullptr};  // left, right
    uint32_t attacks=0; double punch=-1; int punchHand=1; bool struck=false;
};

struct BeachVisitor {
    Person person;
    saida::Node* furniture=nullptr;
    int spot=-1;
    double age=0;
};
struct BeachSpot { r1::P3 towel,water; bool parasol=false; };

struct Loaded {
    saida::Node* node=nullptr;
    // The cooked tile this was mounted from. Its manifest and its prop list
    // are read where they are, never copied.
    std::shared_ptr<const r1::ServedTile> served;
    const json& data; Frame frame; const json& props; size_t nextProp=0,nextLettering=0;
    // The tile's own geometry goes up a few parts a frame (World::uploadParts).
    saida::Node* geography=nullptr; size_t nextPart=0;
    std::vector<saida::Mesh*> partMeshes;
    std::string uploadError;
    uint64_t rejectedSerial = 0;
    struct Building {
        saida::Node* node=nullptr;
        saida::LODGroupBehaviour* lod=nullptr;
        struct Level {
            saida::Node* root=nullptr;
            std::vector<saida::Mesh*> meshes;
            size_t nextPart=0;
            bool started=false,refused=false,failed=false;
        };
        std::array<Level,3> levels;
        bool close=false;
    };
    std::vector<Building> buildings;
    std::vector<int> visualForFootprint;
    saida::GrassNode* grass=nullptr;  // its blades, when its ground grows any
    std::vector<saida::WaterNode*> waters;
    bool reducedDensity=false;
    std::chrono::steady_clock::time_point builtAt=std::chrono::steady_clock::now();
    Loaded(saida::Node* n,std::shared_ptr<const r1::ServedTile> s)
        :node(n),served(std::move(s)),data(served->cooked.manifest),
         frame(data.at("lon").get<double>(),data.at("lat").get<double>()),props(served->cooked.props){}
    std::vector<Footprint> footprints; std::vector<Plant> vegetation;
    // Every trunk of the tile is one static body, a compound of one box per
    // tree: a body per trunk spent up to 640 of the world's bodies on one tile
    // of measured canopy, and nine Paris tiles ran it out. A trunk joins it
    // when its tree is mounted (World::streamProps), never before: a tree
    // still waiting to stream was a wall nobody could see.
    saida::StaticBodyNode* trunks=nullptr;
    std::vector<LiveInterior> interiors;
    std::vector<StoreParking> parking;
    // This tile's road network and the cars on it. The graph must not move
    // once the flow points at it, which is why both live here rather than in
    // a side table: std::map never relocates a node it has already made.
    saida::traffic::Graph graph; saida::traffic::Flow flow;
    std::vector<float> groundUp;          // terrain height per graph node
    std::vector<saida::Node*> cars;       // keyed by agent index, pooled
    std::vector<size_t> carKinds;
    std::vector<std::vector<saida::Node*>> carWheels;
    std::vector<double> carSpin;
    std::vector<uint64_t> carIdentities;
    struct SignalLamp {
        saida::MeshNode* node=nullptr;
        uint32_t program=0,group=0,colour=0;
        bool lit=false;
    };
    std::vector<SignalLamp> signalLamps;
    std::vector<glm::vec2> signalPositions;
    size_t wanted=0;                      // traffic population this tile's density asks for
    double motorcycleChance=.05;
    // The people on its pavements (gen/crowd.hpp): the same shape as the
    // traffic, for the same reason -- the crowd points at the graph.
    r1::WalkGraph walks; r1::Crowd crowd;
    double crowdEyeX=1e9,crowdEyeZ=1e9,crowdLocalPopulation=0;
    std::vector<BeachSpot> beachSpots;
    std::vector<BeachVisitor> beachVisitors;
    std::vector<Person> people;           // keyed by walker slot, pooled
    std::vector<size_t> humanKinds;       // stable avatar per slot in this country
    // Its animals (gen/fauna.hpp): the ground they live on, who lives here,
    // and the engine's wildlife add-on living their lives. The add-on keeps
    // a pointer to this tile for its habitat, which std::map never moves.
    r1::HabitatGrid faunaGrid;
    std::vector<r1::FaunaResident> residents;
    struct FaunaKind { const r1::Species* species=nullptr; std::string model; r1::RangeAnswer range; };
    std::vector<FaunaKind> faunaKinds;          // by the add-on's kind index
    std::vector<const r1::Species*> groupSpecies;
    saida::wildlife::Herds herds;
    struct BirdSupport {const saida::Node* tree=nullptr;saida::wildlife::Perch perch;};
    std::vector<BirdSupport> birdPerches;
    bool birdPerchesDirty=true,birdTreesEnabled=true;
    std::vector<Beast> beasts;            // keyed by animal slot, pooled
    double faunaActiveAge=1e9;
    // The manifest unpacked once at mount, because a frame cannot afford to
    // read JSON. See World::unpack.
    double west=0,east=0,south=0,north=0;
    int gridSize=0; std::vector<float> elevation;
    r1::ElevationGrid groundGrid;
    // 0 land, 1 inland water, 2 sea-level water (see r1/harbours.Cells).
    int waterRows=0,waterCols=0; std::vector<uint8_t> water;
    bool ocean=false,seaIce=false;
    std::vector<Deck> decks; glm::dvec2 deckLow{1e30},deckHigh{-1e30};
    // The streets through water cells (gen/cook.cpp dryStreets): rings, even-odd.
    std::vector<Deck> dryStreets; glm::dvec2 dryLow{1e30},dryHigh{-1e30};
    std::vector<RaisedPiece> raised; glm::dvec2 raisedLow{1e30},raisedHigh{-1e30};
    std::vector<Mooring> boats;
    std::vector<AircraftSpot> aircraft;
    // Snow lying on the ground and the roofs (World::updateSnowCover): which
    // parts wear it, and what each wore before.
    bool snowed=false; size_t snowSeen=0;
    std::vector<std::pair<saida::MeshNode*,saida::Material*>> bare;
};

} // namespace r1::world
