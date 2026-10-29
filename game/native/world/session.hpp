#pragma once
#include "world.hpp"
#include "travel_state.hpp"
#include "remains_store.hpp"
#include "player_profile.hpp"
// Game-side integration with Saida's public C++ API.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Engine.hpp"
#include "core/Paths.hpp"
#include "core/Time.hpp"
#include "core/Input.hpp"
#include "core/Window.hpp"
#include "core/Log.hpp"
#include "audio/AudioManager.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/GLTFLoader.hpp"
#include "core/Profiler.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipView.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/MemoryProfiler.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "nodes/GrassNode.hpp"
#include "nodes/WaterNode.hpp"
#include "nodes/CameraNode.hpp"
#ifdef SAIDA_ENABLE_XR
#include "xr/toolkit/XROrigin.hpp"
#include "xr/toolkit/XRHand.hpp"
#include "xr_gesture_input.hpp"
#include "saida/xrgesture/OpenXRInput.hpp"
#endif
#include "nodes/MeshNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "nodes/TerrainRingsNode.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "behaviours/BoneAttachmentBehaviour.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "nodes/WebCanvasNode.hpp"
#include "scripting/ScriptBehaviour.hpp"
#include "runtime/CaptureArgs.hpp"
#include "runtime/ProfileArgs.hpp"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include "saida/traffic/Traffic.hpp"
#include "saida/wildlife/Wildlife.hpp"
#include "vehicle_runtime.hpp"
#include "motorcycle.hpp"
#include "vehicle_radio.hpp"
#include "traffic_signal.hpp"
#include "gen/traffic_format.hpp"
#include "gen/roadside.hpp"
#include "gen/crowd.hpp"
#include "gen/fauna.hpp"
#include "gen/beach.hpp"
#include "gen/appearance.hpp"
#include "gen/landmarks.hpp"
#include "gen/palette.hpp"
#include "gen/places.hpp"
#include "gen/map_tiles.hpp"
#include "gen/interiors.hpp"
#include "gen/predict.hpp"
#include "gen/net.hpp"
#include "gen/sea.hpp"
#include "gen/far_relief.hpp"
#include "gen/sources.hpp"
#include "gen/service.hpp"
#include "gen/terrain.hpp"
#include "minimap.hpp"
#include "menu_globe.hpp"
#include "menu_intro.hpp"
#include "arrival.hpp"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include "weather.hpp"
#include "forced_time.hpp"
#include "destination.hpp"
#include "density_policy.hpp"
#include "combat.hpp"
#include "combat_kit.hpp"
#include "rocket_kit.hpp"
#include "knockdown.hpp"
#include "animal_physics.hpp"
#include "bird_animation.hpp"
#include "bird_habitat.hpp"
#include "police.hpp"
#include "cheats.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <future>
#include <mutex>
#include <thread>
#include <optional>
#include "geography.hpp"
#include "limits.hpp"
#include "timing.hpp"
#include "tile_preparation.hpp"
#include "loaded_tile.hpp"
#include "menu_tile_cache.hpp"
#include "options_store.hpp"

namespace r1::world {
namespace fs = std::filesystem;
using json = nlohmann::json;

// Private session state. Scene, actor and service lifetimes remain in their
// established order; only the narrow World facade is visible to the application.
class World::Impl : public Rml::EventListener {
    saida::Engine& engine; fs::path game;
    r1::DensityPolicy densityPolicy{game};
    r1::VehicleRadio radio{game};
    bool radioKeys[4]{};
    void updateRadio(double dt);
    saida::WebCanvasNode* ui; saida::WebCanvasNode* minimapUi; saida::CameraNode* camera;
    saida::Node* player=nullptr;
    // The player's car. A member of the entry scene beside him, not a prop of
    // the tile it happens to stand on: it is his, so a teleport takes it along
    // instead of evicting it with the neighbourhood he left.
    saida::Node* car=nullptr;
    // The same player body, rig, modifiers and weapon move onto the solver body.
    // Traffic ownership ends permanently when the player takes a bike.
    saida::Node* playerBikeRider=nullptr;
    std::vector<saida::Node*> frontWheels,rearWheels;
    // Every car the player has stepped out of, left standing where he left it.
    // GTA's rule, and the reason the car he is in is a *pointer* rather than
    // the one node the scene shipped: taking over a traffic car hands him that
    // car, not a copy of it moved under his feet.
    struct Parked {saida::Node* node; double lon,lat,alt,yaw;};
    std::vector<Parked> parked;
    TravelState travel;
    bool driving() const {return travel.mode()==TravelMode::Car;}
    bool sailing() const {return travel.mode()==TravelMode::Boat;}
    bool piloting() const {return travel.mode()==TravelMode::Aircraft;}
    bool swimming() const {return travel.mode()==TravelMode::Swim;}
    void transitionTravel(TravelMode next,bool carryView=true);
    bool wasEnterKey=false,carParked=false,carSinking=false,wheelsReported=false;
    double carLon=0,carLat=0,carAlt=0,carYaw=0,carSpeed=0,wheelSpin=0,steerShown=0;
    double carWaterLevel=0,carSinkDepth=0;
    // Where the player is looking, measured from behind the car. The walk
    // steers the view and the view steers the walk, which is one angle; a
    // car steers itself, so looking out of the side window has to be a
    // second one. It eases back to zero when the mouse goes quiet.
    double lookYaw=0,lookIdle=0;
    // The boat the player is at the helm of, and the ones he has left moored
    // where he stepped ashore. Like the car, a boat he takes is the node that
    // was moored there, handed over, never a copy.
    struct Vessel {
        saida::Node* node=nullptr; std::string kind="motor";
        double length=8,beam=3,top=10,accel=2,turn=30;
        double lon=0,lat=0,alt=0,yaw=0,speed=0;
    };
    Vessel boat; std::vector<Vessel> leftBoats;
    double seaTime=0,swimTime=0,swimHeading=0,swimLean=0;
    struct SeaShip {std::string id; Vessel v; double phase=0;};
    std::vector<SeaShip> seaShips; std::set<std::string> seaTaken;
    std::map<std::string,saida::Node*> hullPrototypes;
    // The sea and the sky, on their own threads (gen/sea.hpp). A new answer
    // is a new pointer, so "has it changed" is one comparison.
    std::unique_ptr<r1::SeaService> sea;
    std::unique_ptr<r1::ConditionsService> sky;
    std::shared_ptr<const json> seaDoc,conditionsDoc;
    double seaAsk=5.,seaRead=0; bool seaFirst=true; std::string seaSaid;
    double conditionsRead=0;
    json conditions=json::object();
    bool smokeSeaWait=false; double captureSeaWait=0;
    bool smokeSail=false,smokeSailWait=false,smokeSailing=false,smokeSailBrake=false,smokeSwimming=false;
    // The aircraft the player is flying, and the ones he has left standing.
    // Like a boat, an aircraft he takes is the node that stood on its stand.
    struct Aircraft {
        saida::Node* node=nullptr; const r1::AircraftType* type=nullptr;
        double lon=0,lat=0,alt=0;              // the point under the wheels or the skids
        double yaw=0,pitch=0,roll=0;           // degrees; nose up and right wing down are positive
        double speed=0,climb=0;                // m/s along the heading, and upward
        double lever=0,thrust=0;               // a plane's throttle, and what its engines give yet
        double rotor=0,mainSpin=0,tailSpin=0;  // a helicopter's rotor speed (0..1) and blade angles
        bool airborne=false;
        std::vector<saida::Node*> gear,mainRotors,tailRotors;
        bool helicopter() const {return type&&type->klass=="helicopter";}
    };
    Aircraft plane; std::vector<Aircraft> leftAircraft;
    std::map<std::string,saida::Node*> aircraftPrototypes;
    // The last surface seen under the aircraft: what it keeps to over tiles
    // that have not streamed yet, rather than inventing a ground.
    double planeGround=0; bool planeStopped=false;
    bool smokeFly=false,smokeFlyWait=false; int smokeFlyPhase=0;
    // --vr: a headset is the camera. The rig stands on the ground the player
    // stands on; the head turns freely above it. Nothing else changes.
    bool vr=false;
#ifdef SAIDA_ENABLE_XR
    saida::XROrigin* vrRig=nullptr;
    r1::XrGestureInput vrGestures;
#endif
    double smokeFlyTime=0,smokeFlyAlt=0,smokeFlyTop=0,smokeFlyClear=0,smokeF=0,smokeR=0,smokeUp=0;
    glm::dvec3 smokeFlyStart{0}; std::set<std::string> smokeFlown;
    glm::dvec3 smokeSwimStart{0}; saida::Node* smokeSwimBoat=nullptr;
    double smokeSailTime=0,smokeSailTop=0,smokeSailClear=0; glm::dvec3 smokeSailStart{0};
    std::vector<saida::Animator*> animators;
    // The player among people (World::moveFeet): his feet in the
    // engine's physics, his body's stagger, the speed he means on foot
    // (east, north, m/s), and what is left of a broken stride.
    saida::CharacterBodyNode* feet=nullptr; int feetUnbuilt=0; bool feetRefused=false;
    // Bodies the physics world refused for capacity (CLAUDE.md rule 3): the
    // engine logs the first of each run, the game names the budget, the
    // player is told and the smoke fails.
    uint64_t physicsRefused=0;
    std::vector<saida::ImpactModifier*> playerImpacts;
    double footEast=0,footNorth=0,stagger=0; size_t playerBumps=0;
    double jumpOffset=0,jumpVelocity=0,followDistance=kOnFootFollow;
    bool wasJump=false;
    bool smokeRan=false,smokeJumped=false;
    bool smokeDriving=false,smokeDroveOnce=false,smokeApproach=false,smokeBrake=false;
    bool smokeTookOver=false;
    double smokeApproachTime=0;
    double smokeDriveTime=0,smokeTopSpeed=0,smokeClear=0; glm::dvec3 smokeDriveStart{0};
    size_t smokeTrafficSeen=0; bool smokeTrafficMoved=false;
    uint64_t generation=0; std::vector<Rml::Element*> listeners;
    std::map<std::string,Loaded> loaded;
    // A shown tile cooked again goes up here, hidden, while the old one is
    // still drawn; the two trade places in one frame (World::promoteIncoming).
    std::map<std::string,Loaded> incoming;
    std::map<std::string,std::string> zoneCountries,countryNames;
    uint64_t minimapRevision=0,minimapDrawnRevision=~uint64_t(0);
    double minimapLon=1e9,minimapLat=1e9;
    saida::Node* prototypes=nullptr;
    std::map<std::string,saida::Node*> naturePrototypes;
    struct VehicleModel {
        std::string name;
        double length=3.6,width=1.94,height=1.5,wheelbase=2.38,wheelRadius=.285;
        saida::Node* prototype=nullptr;
    };
    std::vector<VehicleModel> fleet;
    std::vector<glm::vec3> paints;
    std::vector<size_t> motorcycleRiderKinds,motorcycleFemaleRiderKinds;
    std::unique_ptr<saida::Mesh> motorcycleHelmetShell,motorcycleHelmetVisor;

    // ── people ──────────────────────────────────────────────────────────────
    //
    // assets/models/humans/humans.json (r1/humans.py): the player and the
    // crowd, Rocketbox scans with their clips retargeted onto them. A crowd
    // avatar is imported once into the shared prototypes; each person is a
    // node of its own with its own animator over the avatar's shared meshes,
    // rig and clips, so each avatar costs one upload however many walk.
    json humans;
    double humanScale=.8*kPeopleVehicleResize,playerHeight=1.46*kPeopleVehicleResize;
    struct HumanKind {
        std::string name,model;
        std::vector<std::pair<std::string,std::vector<std::unique_ptr<saida::MeshNode>>>> levels;
        saida::Rig* rig=nullptr;
        std::vector<std::pair<std::string,const saida::AnimationClip*>> clips;
        r1::CrowdPace pace;
        double seatDrop=0;  // origin below a seat's top when seated
        bool ready=false;
    };
    std::vector<HumanKind> crowdKinds;
    std::vector<r1::CrowdAppearance> crowdAppearances;
    r1::CountryCrowd countryCrowd;
    void loadHumans();
    // Shorter strides need a faster cadence at the player's existing speed.
    // Clip views retime playback without rebuilding the imported animation.
    void playPlayerClip(const char* clip);
    // A crowd avatar's shared parts, taken from its prototype on first use:
    // its meshes by level of detail, its rig and its clips.
    HumanKind* humanKind(size_t index);
    // The Rocketbox biped's chains (r1/humans.py keeps its eyes for this).
    // A person's gaze, not a turret's: the eyes go first and furthest, the
    // head most of the way, the spine barely; past 83 degrees either side
    // the head holds at its limit rather than turn round.
    static saida::GazeModifier::Settings gazeSettings();
    // A blow bends the back from the waist up, the head last and loosest,
    // and the body swings back upright in under a second.
    static saida::ImpactModifier::Settings impactSettings();
    // People whose skeleton lacks a bone the gaze or the stagger needs:
    // said once each (CLAUDE.md rule 3), and a failed smoke.
    size_t bodiesRefused=0;
    bool makePerson(Loaded& tile,size_t slot,Person& person);
    bool makePerson(saida::Node& parent,size_t selected,Person& person,bool controlled=false);
    // Coastal visitors: state remains attached to streamed tiles during migration.
    size_t beachLive=0;
    double beachClock=0,beachSunAge=1e9,beachSun=0;
    saida::Node* makeBeachFurniture(Loaded& tile,bool parasol,unsigned color);
    void updateBeach(float dt);
    // Police actors belong to the game;
    // Wanted and routing remain independent of rendering and physics.
    struct PoliceOfficer {
        Person person;
        saida::Node* gun=nullptr;
        saida::Node* launcher=nullptr;
        r1::rocket::FireControl rocket;
        r1::police::WeaponChoice weapon;
        double health=100, shot=0;
        bool sees=false;
        std::vector<r1::police::Routes::Point> path;
        size_t step=0;
    };
    struct PoliceUnit {
        saida::Node* car=nullptr;
        r1::rocket::FireControl rocket;
        r1::police::WeaponChoice weapon;
        double shot=0;
        std::array<PoliceOfficer,2> officers;
        saida::Node* rocketMount=nullptr;
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
    double policeAirRespawn=0;
    r1::rocket::FireControl policeAirRocket;
    r1::police::WeaponChoice policeAirWeapon;
    r1::police::FireEscalation policeEscalation;
    double policeRocketSpacing=0;
    saida::Node* policeRocketTarget=nullptr;
    saida::AudioID policeRotorSound=saida::kInvalidAudioID;

    static glm::vec3 policeVector(r1::police::Routes::Point p);
    static r1::police::Routes::Point policePoint(glm::vec3 p);
    glm::vec3 policePlayerPosition()const;
    glm::vec3 policeAimPoint()const;
    bool policeSight(glm::vec3 from,glm::vec3 to,saida::CollisionObjectNode* observer=nullptr,float range=140);
    void reportCrime(r1::police::Crime crime,glm::vec3 at,bool identified=false);

    // Rebuilt only when the streamed graph changes or the ENU origin moves.
    void policeBuildRoutes();

    saida::Node* policeBox(saida::Node& parent,const std::string& name,glm::vec3 at,glm::vec3 size,glm::vec3 colour);
    void policeLivery(saida::Node& node,const r1::police::Identity& identity,float roof,bool authored);
    bool policeCreate(PoliceUnit& unit,glm::vec3 at,const PoliceStation* station);
    void policeRemove(PoliceUnit& unit);
    void clearPolice();

    bool hitPolice(const saida::RaycastHit& hit,double damage,glm::vec3 direction,bool headshot=true,float impulse=float(r1::combat::kPistol.impulse));

    void policeArrestPlayer();

    void policeFire(glm::vec3 muzzle,glm::vec3 target,saida::CollisionObjectNode* shooter,float range);

    void policeOfficer(PoliceUnit& unit,PoliceOfficer& officer,glm::vec3 target,double dt,bool think);

    void policeAir(double dt,bool think);

    void updatePolice(double elapsed);

    std::string policeMapRml();

    bool policeSmoke=std::getenv("R1WORLD_POLICE_SMOKE")!=nullptr;
    bool policeSmokeGroundHit=false,policeSmokeAirHit=false;
    int policeSmokePhase=0;
    double policeSmokeTime=0;
    glm::vec3 policeSmokeStart{0};
    void runPoliceSmoke(double dt);
    // Land animals: residents are cooked with each tile (gen/fauna.hpp),
    // the engine's wildlife add-on (engine/plugins/wildlife) lives their lives,
    // and this draws them: one pooled node per slot with its own animator over a
    // model's shared meshes, rig and clips (CLAUDE.md §5: a hundred cows upload
    // one cow), its two levels of detail, and the clip its state calls for at the
    // speed it moves, so hooves stay planted.
    //
    // assets/animals/animals.json (tools/r1/animals.py) says how each model was
    // measured: where its hooves touch the ground, which way its head points,
    // which clip plays what and how fast each gait carries the body.

    // How many animals live at once around the player, nearest tiles first, and
    // how many slots one tile pools: each is a node, an animator and a kinematic
    // hit/collision box, including small residents. Birds share it: central
    // Paris holds some 40 pigeons within sight, and the beasts must still come.
    static constexpr size_t kFaunaAnimals=64;
    struct AnimalKind {
        std::string model,lod0,lod1;
        double ground=0,height=0,length=0,width=0,forward=0;
        std::map<std::string,std::vector<std::string>> roles;
        double walkSpeed=0,runSpeed=0;
        std::vector<std::pair<std::string,std::vector<std::unique_ptr<saida::MeshNode>>>> levels;
        saida::Rig* rig=nullptr;
        std::vector<std::pair<std::string,const saida::AnimationClip*>> clips;
        bool ready=false,failed=false,bird=false;
        std::string refusal;
    };
    std::map<std::string,AnimalKind> animalKinds;
    std::set<std::string> animalRefusalsSaid;
    bool faunaSmokeSaid=false;

    void loadAnimals();
    // A refusal is said once, in the log and to the player, and fails a smoke
    // (CLAUDE.md §3); the animal is not drawn rather than drawn wrong.
    void refuseAnimal(AnimalKind& kind,const std::string& why);
    // The meshes of one prototype, flattened with the transforms above them --
    // the root joints' parent frame included (GLTFLoader::skinnedPrimitiveFrame).
    static void collectSkinned(const saida::Node& source,std::vector<std::unique_ptr<saida::MeshNode>>& out);
    // A model's shared parts, from its two prototypes on first use; null while
    // they stream in or once refused.
    AnimalKind* animalKind(const std::string& model);
    // One animal as drawn: a node with its animator over a kind's shared meshes,
    // both levels in the rig's imported parent frame, and a kinematic hit box.
    bool makeBeast(saida::Node& parent,const std::string& model,double scale,Beast& beast);
    // Wildlife owns living residents; these nodes belong
    // to the world independently of pooled slots, interiors and terrain tiles.
    struct AnimalRemains {
        Beast beast;
        Frame frame;
        saida::Node* anchor=nullptr;
        saida::RagdollBehaviour* fall=nullptr;
        saida::StaticBodyNode* restBody=nullptr;
        saida::RagdollProfile profile;
        std::string resident;
        bool charred=false,stored=false,refused=false,distant=false;
        glm::vec3 resumeVelocity{0},resumeSpin{0}; // in this remains' geographic frame
        json saved;
        fs::path savedPath;
    };
    std::vector<AnimalRemains> animalRemains;
    std::set<std::string> animalDeaths;
    fs::path animalSaveRoot;
    RemainsWriter animalWriter;
    RemainsIndex animalIndex;
    std::set<size_t> mountedRemains;
    bool animalSaveBlocked=false,animalSaveErrorSaid=false;

    static std::string animalResidentId(uint64_t group,size_t member);
    static std::string indoorAnimalId(const LiveInterior& room,size_t animal,int member);
    static json animalTransformJson(const saida::Transform& t);
    static saida::Transform animalReadTransform(const json& j);
    void animalSaveError(const std::string& why);
    void placeAnimalRemains(AnimalRemains& c,const Frame& observer,bool rebase=false);
    void rebaseAnimalRemains(const Frame& next);
    void charAnimal(Beast& b);
    json snapshotAnimal(const AnimalRemains& c) const;
    std::chrono::steady_clock::time_point nextAnimalSave{};
    void saveAnimalRemains(bool flush=false);
    void loadAnimalRemains();

    bool unmountAnimalRemains(size_t index);
    bool reserveAnimalSimulation(AnimalRemains* except=nullptr);
    void buildAnimalRestBody(AnimalRemains& c);
    void updateAnimalRemains();
    bool wakeAnimalRemains(AnimalRemains& c,const glm::vec3& velocity={},const glm::vec3& spin={});
    void pauseDepartingAnimalRemains(const std::set<std::string>& kept);
    bool leaveAnimalBody(Beast& beast,const Frame& frame,const std::string& resident,
                        const glm::vec3& velocity,const glm::vec3& impulse,const glm::vec3& point,bool charred);
    bool hitAnimal(const saida::RaycastHit& hit,const glm::vec3& direction,float momentum);
    void blastAnimals(glm::vec3 at,saida::CollisionObjectNode* direct);
    // Habitat and geographic adaptation of the optional
    // wildlife flight model; no game coordinates leak into the engine add-on.
    std::map<const saida::Mesh*,std::vector<glm::vec3>> birdBranchCache;
    void addBirdPerches(Loaded& tile,const saida::Node& tree);
    void refreshBirdPerches(Loaded& tile);
    bool birdFlightClear(const Loaded& tile,saida::wildlife::Vec2 from,float fy,saida::wildlife::Vec2 to,float ty) const;
    // The clip a state calls for, and its playback rate at `speed` m/s: a gait
    // plays as fast as the body moves over its measured stride speed.
    std::pair<std::string,float> beastClip(const AnimalKind& kind,saida::wildlife::State state,double speed,double scale,uint64_t identity,bool aloft);

    void readFauna(Loaded& tile);
    // Which groups are out now: their hour, their season, the rain. Each group
    // draws against a stable share, so the same group is out for the same half
    // hour on every machine.
    void faunaActivity(Loaded& tile,double hour,int month,const r1::WeatherState& weather);
    int localMonth();
    void updateFauna(float delta);
    // Every 20 s, what lives around the player: groups out, spawned, animals
    // alive and drawn, and the nearest home -- so a walk without animals says why.
    double faunaSaidAge=0;
    void sayFauna(double dt);
    // R1WORLD_FAUNA_SHOT=<png> with --smoke photographs the nearest animal out
    // (R1WORLD_FAUNA_SHOT_SPECIES narrows it to a species id) from its side, a
    // few body lengths away, then ends the run: a test says the animal is built,
    // only a picture says it stands on its feet, its size, facing the way it
    // walks (CLAUDE.md rule 1).
    std::string faunaShotTile; size_t faunaShotSlot=0; uint64_t faunaShotIdentity=0,faunaShotMoved=0; double faunaShotClock=0;
    bool faunaShotWanted() const;
    void faunaShot(double dt);
    void syncBeasts(Loaded& tile,const glm::dvec3& eye,double dt);
    // The smoke builds every kind the mounted tiles' residents use, both levels,
    // and says how many live; a model that cannot be drawn fails it by name.
    void checkFaunaSmoke();

    // ── indoors ─────────────────────────────────────────────────────────────────
    //
    // A furnished home sometimes has a spider on a wall, a trail of ants in the
    // kitchen, a mouse, a wasp at the window, a cat asleep: rare, drawn from the
    // building and the day (gen/fauna.cpp planIndoor). They stand in the room's
    // node and go with it.
    void planRoomAnimals(LiveInterior& room,const Loaded& t);
    void updateRoomAnimals(LiveInterior& room,const Loaded& t,double dt);

    void readCrowd(Loaded& tile);
    // The hour where the player stands, by the Sun: 12 when it is highest.
    double localSolarHour();
    double crowdFactor();
    void updateCrowd(float delta);
    // A step of the player's among people, `east` and `north` metres: his
    // feet are the engine's character body, a standing capsule moved and slid
    // against the people's capsules (CharacterBodyNode::moveAndSlide), so he
    // stops at whoever is in the way and slides round them, and whoever walks
    // into him moves him. What his feet touched is then a bump each
    // (World::meetPeople). Buildings, furnishings and vehicles are streamed engine bodies too.
    glm::dvec3 moveFeet(double east,double north,double dt);
    // Every person the feet touched: the crowd answers the bump if it is one
    // (r1::Crowd::bump), and the player's own body feels it.
    void meetPeople();
    // Running into someone, the player's own body takes the blow -- thrown
    // back off whoever he hit -- and his stride is broken for a moment,
    // longer the harder he hit.
    void staggerPlayer(Loaded& tile,const glm::dvec3& toward,double speed);
    void syncPeople(Loaded& tile,const glm::dvec3& eye,double dt,const glm::dvec3* head);
    // ── the fight ───────────────────────────────────────────────────────────
    //
    // The Fun update (PLAN §8): GTA III's street on the real one. Fists and a
    // pistol; passers-by who fall, run, or square up and fight back; a car
    // that throws whoever it hits over its bonnet; a player who can be hurt,
    // knocked down, and wake up at the nearest mapped hospital. The rules are
    // native/combat.hpp; the engine gives the falls (RagdollBehaviour), the
    // arms (TwoBoneIKModifier) and the pistol in the hand (BoneAttachment).
    r1::combat::Arsenal arsenal; r1::combat::Health health; r1::combat::Punch punch;
    std::unique_ptr<PlayerProfileStore> profileStore;
    PlayerProfile profile;
    bool playerMenu=false,photoMode=false,wasPlayerMenuKey=false,wasPhotoKey=false,wasPhotoShutter=false,wasPhotoCategory=false;
    bool profileDirty=false;
    bool photoShutterRequested=false;
    double profileSaveAge=0,photoNoticeAge=0;
    PhotoCategory photoCategory=PhotoCategory::World;
    int portfolioFilter=-1;
    size_t portfolioIndex=0;
    std::string playerTab="portfolio",profileError;
    std::string portfolioSpecies;
    struct PhotoResult { bool done=false,success=false;std::string error;Photograph photo; };
    std::shared_ptr<PhotoResult> photoResult;
    void loadPlayerProfile();
    bool savePlayerProfile();
    void updatePhotography(double dt);
    void finishPhotograph();
    bool setPhotoMode(bool show);
    void takePhotograph();
    std::vector<PhotoAnimal> photographedAnimals();
    bool framePhotoSmokeAnimal();
    void showPlayerMenu(bool show);
    void refreshPlayerMenu();
    void refreshPortfolio();
    bool playerMenuAction(const std::string& id);
    void photoNotice(const std::string& message);
    int photoSmokePhase=0;
    double photoSmokeAge=0;
    void runPhotoSmoke(double dt);
    r1::CombatEffects effects; r1::CombatSounds sounds;
    saida::RagdollProfile ragdollProfile=r1::rocketboxRagdoll();
    saida::Node* playerBody=nullptr; saida::Transform playerBodyRest;
    r1::Knockdown playerFall;
    saida::TwoBoneIKModifier* playerArms[2]{nullptr,nullptr};
    saida::TwoBoneIKModifier* playerBikeLegs[2]{nullptr,nullptr};  // left, right
    saida::GazeModifier* playerBikeLean=nullptr;
    saida::GazeModifier* playerBikeLook=nullptr;
    r1::RocketboxRidingAim* playerBikeAim=nullptr;
    r1::RocketboxRidingHands* playerBikeHands=nullptr;
    saida::Node* pistol=nullptr; saida::Transform pistolGrip,launcherSupportGrip;
    float playerReach=.55f;  // shoulder to wrist, the rig's metres
    bool aiming=false,wasFire=false,wasReloadKey=false; double aimBlend=0;
    bool wasWeaponKey[3]{};
    double fightClock=0,arrivalClock=0;
    bool playerDown=false,wasted=false; double downClock=0;
    // The cheat line (World::updateCheats): open, the keys are its own.
    int outOfMemorySaid=0;
    bool cheatOpen=false,wasCheatKey=false,wasCheatEnter=false,wasCheatEscape=false; double cheatSaidAt=-1e9;
    std::string punchTileKey; size_t punchSlot=0; bool punchAimed=false;  // whom a punch is thrown at
    double hurtShown=-1e9,recoil=0;
    size_t ragdollRefused=0;
    std::string respawnSaid;
    bool smokeFought=false,smokeFighting=false; double smokeFightTime=0; int smokeFightPhase=0;
    std::string smokeFightTile; size_t smokeFightSlot=0;

    // The player's scan arrives as one MeshNode per material, each with an
    // Animator of its own (GLTFLoader): one pose evaluated three times, and
    // three skeletons a ragdoll or a held pistol would have to keep together.
    // Like a passer-by (World::makePerson), the body becomes one node with one
    // Animator over the scan's meshes.
    void unifyPlayerBody();
    void setupCombat();

    // A tile-frame point's ground: what a passer-by off the paths stands on.
    double groundUnder(const Loaded& tile,double x,double z,double fallback);
    // Scene-frame point to geodetic, and the height of the ground there.
    glm::dvec3 sceneToGeo(const glm::vec3& p) const;
    float sceneGround(const glm::vec3& p);
    Loaded* tileByKey(const std::string& key);
    static glm::vec3 nodePosition(const saida::Node& n);
    // A scene direction to the tile frame (both are tangent frames a few km apart at most).
    glm::dvec3 toTile(const Loaded& tile,const glm::vec3& v) const;

    // ── bodies on the ground ──

    // Off their feet: the crowd stops walking them, their capsule leaves the
    // physics, and the engine's ragdoll takes the body, moving at `velocity`
    // (scene frame), turning at `spin`. Past kLiveRagdolls tumbling at once,
    // the one down longest is frozen where it lies. A world too full for the
    // bodies is said (CLAUDE.md rule 3), and the person only staggers.
    bool beginFall(r1::Knockdown& fall,const glm::vec3& velocity,const glm::vec3& spin,const char* who);
    bool knockDown(Loaded& tile,size_t slot,const glm::vec3& velocity,const glm::vec3& spin);
    void limitRagdolls();
    // A body down: its node is the ragdoll's. Once still for a moment, the
    // living get up -- the pose blends back to the animation where the body
    // lies -- and go back to the paths, fists up or running.
    bool personDown(Loaded& tile,size_t slot,Person& p,const r1::Walker& w,double dt);

    // ── a passer-by's punch ──

    // A fighter's punch (r1::Crowd counts them): the arm goes out at the
    // player's head and lands if he is still in reach.
    void personAttack(Loaded& tile,size_t slot,Person& p,const r1::Walker& w,double dt);

    // ── the player hurt ──

    void hurtPlayer(double damage,const glm::vec3& push,const char* by,bool knockDown=true);
    // Off his feet: the ragdoll takes his body, his feet leave the physics.
    void knockPlayerDown(const glm::vec3& velocity,const glm::vec3& spin,bool killed);
    void landed(double speed);
    // A car the engine says his feet touched, closing on him: thrown.
    bool hitByVehicle(const saida::CharacterBodyNode::Contact& contact);
    // Where the camera looks while he is down: his pelvis.
    glm::vec3 downAnchor() const;
    // Down and still, he gets up where he lies; dead, he wakes at a hospital.
    void updatePlayerDown(double dt);
    // The nearest mapped hospital, clinic or doctor's (gen/interior_uses.cpp
    // names them all "clinic"), across the resident tiles: he wakes outside
    // its door. With none mapped near, where he fell, and the screen says so.
    void respawn();
    void showWasted(bool show);

    // ── the player's weapons ──

    void showHeldPistol();
    void setAiming(bool on);
    void selectWeapon(r1::combat::Weapon wanted);
    // The physics filter of a shot or a punch: everything but his own feet.
    saida::QueryFilter combatFilter() const;
    // A passer-by, by the capsule or ragdoll part a query hit.
    bool personByBody(const saida::RaycastHit& hit,Loaded*& outTile,size_t& outSlot);
    // Witnesses: everyone near a blow or a shot runs from it.
    void alarm(const glm::vec3& at,double radius,double seconds);
    // A person struck (fist or round): health, then a stagger, a fall or death.
    void strikePerson(Loaded& tile,size_t slot,double damage,const glm::vec3& direction,const glm::vec3& point,
                      float impulse,bool always);

    void throwPunch();
    void landPunch();

    bool evacuateVehicle(saida::CollisionObjectNode& hitBody);

    int escapeSmokePhase=0; double escapeSmokeTime=0,escapeSmokeDistance=0;
    std::string escapeSmokeTile; size_t escapeSmokeSlot=0;
    void runVehicleEscapeSmoke(double dt);

    void fire();

    // The arms each frame: the aim holds the pistol on the crosshair, a punch
    // reaches for its target, and both ease out when done.
    void poseArms(const glm::vec3* inspectionLook=nullptr);

    // A car driven into people: whoever stands in its path is thrown over
    // the bonnet, harder the faster it goes.
    void runOverPeople(double dt);

    void updateCombat(double dt);

    // The fight's HUD: health and weapon, changed only when they change (the
    // interface is rasterised whole on every change, WebCanvasNode).
    void combatHud();

    // World-owned gameplay. Saida owns collision queries, GPU particles and meshes.
    struct RocketFlight {
        saida::Node* mesh=nullptr;
        saida::ParticleSystemNode* trail=nullptr;
        saida::ParticleSystemNode* flame=nullptr;
        glm::vec3 position{0},direction{0,0,1};
        saida::QueryFilter filter;
        float life=0,rest=0;
        bool police=false;
    };
    std::array<RocketFlight,r1::rocket::kProjectiles> rockets;
    r1::RocketEffects rocketEffects;
    saida::Node* launcher=nullptr;
    bool rocketPoliceDamage=false;
    double blastShake=0;
    int rocketLaunches=0,rocketImpacts=0,rocketDestroyed=0;

    static saida::CollisionObjectNode* aircraftCollider(saida::Node* node);
    static void armAircraft(saida::Node& node,const r1::AircraftType& type);
    void buildRockets();
    void clearRockets();
    bool rocketRoom()const;
    bool launchRocket(glm::vec3 muzzle,glm::vec3 target,saida::QueryFilter filter,bool police);
    void fireRocket();
    glm::vec3 rocketTarget()const;
    bool policeRocket(glm::vec3 muzzle,saida::CollisionObjectNode* shooter,bool aerial);
    // Disable and detach immediately, then defer node deletion to the scene. All
    // ownership lists are cleared in this transaction before another rocket runs.
    void retireRocketVehicle(saida::Node* node);
    void destroyRocketCar(saida::Node* node);
    void destroyRocketAircraft(saida::Node* node);
    bool rocketExposed(glm::vec3 at,glm::vec3 point,saida::CollisionObjectNode* body=nullptr,const saida::Ragdoll* ragdoll=nullptr);
    void explodeRocket(glm::vec3 at,saida::CollisionObjectNode* direct,bool police);
    void updateRockets(double elapsed);
    // Opt-in end-to-end checks against the real World, physics and SaidaFX.
    bool rocketSmoke=std::getenv("R1WORLD_ROCKET_SMOKE")!=nullptr;
    int rocketSmokePhase=0,rocketSmokeCount=0;
    double rocketSmokeTime=0;
    saida::Node* rocketSmokeWall=nullptr;
    void runRocketSmoke(double dt);

    // ── cheat codes ─────────────────────────────────────────────────────────
    //
    // GTA's line: T opens it over the HUD, a word and Enter does what
    // native/cheats.hpp says it does, Escape closes it. Open, it has the
    // keyboard -- the engine routes keys and text to the canvas it is told
    // has the focus -- and the game reads none of its own keys (World::held),
    // or "comet" would walk the player forward and toggle the debug panel.
    // A code that cannot be done here says why, on screen and in the log
    // (CLAUDE.md rule 3).
    // The menu's text fields are typed in the same way: the keys are GLFW's,
    // named after their US place, so on AZERTY the comma is M and "48,02"
    // closed the settings at the comma.
    bool typing() const;
    bool held(int key) const;
    bool clicked(int button) const;
    // The process's private memory, in MB: what a bad_alloc runs out of.
    static double privateMegabytes();
    void openCheat();
    void closeCheat();
    void cheatSay(const std::string& said);
    void updateCheats(double dt);
    void runCheat(const std::string& typed);
    // Where a vehicle `length` long and `width` wide stands parallel to the
    // player: on loaded ground near his level, out of the water, clear of
    // walls and bodies at every corner, middle and end, and clear of him.
    // Ahead first for an aircraft, so he faces its tail within reach of it;
    // to his right first for a car, where parkCar puts one; then a spiral
    // out to 40 m. `wings`: when nowhere has room for a whole aircraft -- a
    // city street is narrower than a jet's span -- one whose fuselage is
    // clear may stand with its wing tips over a pavement; it taxis out of
    // what it overhangs (World::moveAircraft lets a hull leave an obstacle).
    bool roomBeside(double length,double width,bool ahead,bool wings,double& outLon,double& outLat);
    // BUZZOFF and PLANE: a helicopter or a business jet beside him, left
    // standing like one he stepped out of -- F takes it, and the oldest of
    // those beyond kLeftAircraft is cleared as always.
    std::string spawnAircraft(const std::string& name);
    // COMET: the fleet's sports car in its reddest paint, parked beside him
    // like a car he left; F takes it.
    std::string spawnSportsCar();
    // MOTO and ROCKET: spawn the plain city bike or the motocross bike from
    // the same light-weight fleet used by traffic, ready for F to take it.
    std::string spawnParkedVehicle(const std::string& name);

    // ── the smoke's fight ──
    //
    // After the walk: the nearest passer-by is knocked down by the code path a
    // punch or a car takes, and must tumble in the engine's physics, come to
    // rest, and freeze; a round fired at the ground must land. A refusal at
    // any step names itself (CLAUDE.md rule 3).
    void startSmokeFight();
    void runSmokeFight(double dt);
    size_t crowdWanted() const;
    size_t crowdLive() const;
    // Tiles are cooked in this process, on the service's threads (gen/).
    std::unique_ptr<r1::WorldService> service;
    std::map<std::string,saida::AssetID> textures;
    size_t residentVertexBudget=0,residentIndexBudget=0;
    bool buildingTrimPending=false;
    // A tile was mounted: it may be the one the player stands on, cooked
    // again with the buildings its first, provisional cook did not have.
    bool checkStanding=false;
    Frame origin; double lon=2.3522,lat=48.8566,alt=0,yaw=0,pitch=-12;
    double pickLon=2.3522,pickLat=48.8566,zoom=1,mapCenterLon=2.3522,mapCenterLat=48.8566;
    // OpenStreetMap allows two connections (tile usage policy); EOX serves the
    // Sentinel-2 cloudless 2016 mosaic, CC BY 4.0, under its attribution.
    MenuTileCache menuTiles{game,{"osm",".png",[](const r1::MapTile& t){
        return "https://tile.openstreetmap.org/"+std::to_string(t.z)+"/"+std::to_string(t.x)+"/"+std::to_string(t.y)+".png";},2}};
    MenuTileCache imageryTiles{game,{"s2cloudless-2016",".jpg",[](const r1::MapTile& t){
        return "https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless_3857/default/g/"+std::to_string(t.z)+"/"+std::to_string(t.y)+"/"+std::to_string(t.x)+".jpg";},6}};
    std::shared_ptr<r1::MenuGlobeState> globe;
    r1::GlobeView globeTarget,globeDragView;
    glm::dvec2 globePress{};
    bool globeDragging=false,globeMoved=false;
    bool menu=true,playing=false,pending=false,warming=false,wasMenuKey=false,wasF11Key=false;
    bool performanceDebug=false,debugTraffic=true,debugGrass=true,debugWater=true,debugTrees=true;
    saida::WebCanvasNode* performanceUi=nullptr;
    bool performanceKeys[5]{};
    double performanceSeconds=0; size_t performanceFrames=0;
    static constexpr double kPerformanceRefreshSeconds=.5;
    bool performanceSmoke=false; int performanceSmokePhase=0; double performanceSmokeWait=0;
    std::chrono::steady_clock::time_point goStarted;
    double lastMountMs=0;
    // How smooth the arrival was: frames over 33 ms in the ten seconds after
    // Go, and the worst of them. The one number "it freezes" becomes.
    std::chrono::steady_clock::time_point lastFrame{};
    int arrivalFrames=0,arrivalHitches=0; double arrivalWorst=0; bool arrivalSaid=true;
    // What the last update spent, by step, so a slow arrival frame says why.
    struct FrameCost { double stream=0,parts=0,warm=0,props=0,distant=0,world=0; } cost;
    template<class F> double timed(F&& f) {
        const auto t=std::chrono::steady_clock::now();f();
        return msSince(t);
    }
    double poll=0,hud=0,smokeWalk=0; std::string requested;
    // The nine tiles around the player, computed once a frame. `nearby` sorts
    // and allocates, and three callers wanted the same answer.
    std::vector<Tile> ring;
    // Index work accumulated across refreshes, including render-time LOD changes.
    uint64_t indexedAtFrame=0; double churn=0,churnFrames=0,dirtyFrames=0;
    bool smoke=false,smokeStarted=false,smokeWaterSpawn=false; glm::dvec3 smokeStart;
    const bool idleSmoke=std::getenv("R1WORLD_IDLE_SMOKE")!=nullptr;
    const std::string perfSmoke=std::getenv("R1WORLD_PERF_SMOKE")?std::getenv("R1WORLD_PERF_SMOKE"):"";
    bool perfReady=false;
    double perfReadyWait=0;
    const bool qualitySwitchSmoke=std::getenv("R1WORLD_QUALITY_SWITCH_SMOKE")!=nullptr;
    int qualitySwitches=0;
    double qualitySwitchWait=0;
    // R1WORLD_WADE_SMOKE="<lon> <lat>": walk from the spawn to that point and
    // fail the moment he swims. Across a breakwater, a rock or the swash the
    // walk must stay a walk while less than half of him is under the water.
    const char* wadeSmoke=std::getenv("R1WORLD_WADE_SMOKE");
    std::optional<glm::dvec2> wadeTo;
    double wadeOverWater=0,wadeDeepest=0;
    bool idleSmokeStopped=false;
    glm::dvec3 idleSmokeStart{};
    double idleSmokeDrift=0;
    bool testFailed=false,testResume=false; double testElapsed=0,resumeWait=0;
    // Set when a tile the spawn is waiting for will not fit in the resident
    // budget. Without it the spawn simply never completes: `surroundingReady`
    // wants all nine tiles, one of them never arrives, and the only report is
    // a data timeout three minutes later blaming the network for a decision
    // this process made in one line. Cleared at the start of every spawn.
    std::string refused;
    // Actions answer the press immediately. Its matching click must not run
    // them again, even after a long hold or a slow frame; keyboard-generated
    // clicks without a matching press still activate the control.
    double clock=0; std::string pressedId;
    std::string cityInput,citySubmitted,cityInFlight;
    double cityChangedAt=0,cityLastRequestAt=-100;
    std::future<std::vector<r1::PlaceChoice>> cityFuture;
    std::vector<r1::PlaceChoice> cityChoices;
    std::map<std::string,std::vector<r1::PlaceChoice>> cityCache;
    // The driver used to test one cold spawn and stop. Teleporting from a
    // place you are already standing in is a different path -- tiles are
    // evicted, the resource arena is trimmed and the origin moves half a
    // planet -- and it is the path a player takes every time after the
    // first. --spawn2 makes the run do it.
    double hopLon=0,hopLat=0,hopWait=0; bool hopWanted=false,hopDone=false,hopArmed=false;
    std::optional<double> smokeArrivalHeading;
    saida::ScriptBehaviour* sunScript=nullptr; bool sunReported=false;
    double subsolarAge=1e9; bool subsolarRefused=false;
    // Options screen. `forcedMinutes` is "Forcer l'heure à" (minutes after
    // local midnight), kept in cache/options.json between runs; `forcedSent`
    // is the instant last handed to the Sun, so the script is told only when
    // the hour or the time zone under the player changes.
    std::optional<int> forcedMinutes; std::optional<double> forcedSent; bool forcedSaid=false,optionsOpen=false;
    r1::WeatherOverride forcedWeather=r1::WeatherOverride::Real;
    bool toasterMode=false;
    int giQuality=0; // -1: off, 0..4: engine Auto/Low/Medium/High/Ultra
    inline static constexpr const char* giQualityKeys[]={"off","auto","low","medium","high","ultra"};
    int standardWidth=1600,standardHeight=900;
    saida::CaptureRequest worldCapture;
    saida::runtime::CaptureViewpoint captureView;
    bool captureVehicleDent=false,motorcycleCaptureStarted=false,motorcycleCaptureFired=false;
    double motorcycleCaptureWait=0;
    double captureVehicleWait=0;
    bool captureQueued=false;
    // --at <unix seconds>: an inspection capture, lit at that instant under a
    // clear sky and without the HUD, so two builds photograph the same place
    // in the same light (tools/gallery.py). Without it a capture keeps the
    // real instant it started at and today's weather.
    std::optional<double> inspectAt;
    // Cloud fraction, rain mm/h, visibility metres (zero keeps clear-air
    // extinction). Inspection weather never replaces live observations.
    std::array<double,3> inspectWeather{0.,0.,0.};
    std::optional<double> inspectTemperature;
    // --camera-altitude <metres>: the viewpoint's heights are above the sea
    // rather than above the player's feet. A capture laid beside a photograph
    // stands where the photographer stood, whatever this build's relief says
    // the ground there is (tools/gallery.py's reference views).
    std::optional<double> captureAltitude;
    // Photo camera coordinates, independent of gameplay's safe spawn and
    // keepStanding relocation. z is the eye's height above surveyed ground.
    std::optional<std::array<double,3>> captureGeo;
    double captureWait=0,captureReport=0;
    std::string number(double n,int precision=6);
    // Every write re-lays and re-renders the whole interface -- the map is
    // 1.3 million pixels, some 240 ms -- so a write that changes nothing is
    // not made. The status line used to be rewritten every 16 ms while a
    // destination loaded: that was the freeze after Go.
    std::map<std::string,std::string> written;
    void style(const std::string& id,const std::string& key,const std::string& value);
    void text(const std::string& id,const std::string& s);
    void miniText(const std::string& id,const std::string& s);
    void layoutMinimap();
    void updateMinimap();
    std::string value(const std::string& id);
    // Compared with what the field shows, not with what was last written: the
    // player may have typed in it since.
    void field(const std::string& id,const std::string& value);
    void field(const std::string& id,double x);
    void showCityChoices(const std::vector<r1::PlaceChoice>& choices,const std::string& feedback={});
    void updateCityLookup();
    // Press-only checks immediate activation; a full gesture also checks that
    // its release/click does not activate the same control a second time.
    void testClick(const std::string& id,bool release=true);
    void select(double x,double y);
    void updateGlobePin();
    // The menu's music, through the engine's audio: looping, quiet, fading out
    // when the world takes over and back in -- where it left off -- on return.
    static constexpr const char* kMenuMusic="assets/audio/menu/la.mp3";
    static constexpr float kMenuMusicVolume=.25f;
    static constexpr double kMenuMusicFade=1.5; // seconds, either way
    saida::AudioID menuMusic=saida::kInvalidAudioID; float menuMusicGain=0; bool menuMusicSaid=false;
    r1::MenuIntro menuIntro;
    void updateMenuIntro();
    void updateMenuMusic(double dt);
    // Left alone at the whole-Earth view, the globe turns slowly eastward under
    // the Sun. Any click, wheel or typing stops it; it resumes only after 20 s
    // untouched and only at the minimum zoom -- never while a place is sought.
    static constexpr double kGlobeSpinDegreesPerSecond=2.5,kGlobeIdleSeconds=20;
    double globeIdle=0; bool globeSpinning=true;
    void touchGlobe();
    void spinGlobe(double dt);
    void updateGlobe(double dt);
    void showMap(bool show);
    void resetPerformanceSample();
    void performanceText(const std::string& id,const std::string& value);
    void placePerformanceCanvas();
    void ensurePerformanceCanvas();
    void performanceStatus();
    void applyPerformanceTile(Loaded& tile);
    void handlePerformanceKeys(const bool (&keys)[5]);
    void samplePerformance(double seconds);
    void pressPerformanceKey(size_t key);
    void runPerformanceSmoke(double dt);
    int mapZoomLevel() const;
    // One detail layer: the tiles of the level shown, what is on disk to
    // stand in for them, and the downloads in the order they matter -- what is
    // seen (centre first), where the wheel is taking the view, the next level
    // in, the level out -- so a zoom lands on tiles that are already there.
    void fillLayer(MenuTileCache& cache,r1::GlobeLayer& layer,int z,int coarsest,int finest,
                   const r1::GlobeView& target,int targetZ);
    void renderMapTiles();
    void zoomMap(double z);
    double streamPriority(Tile t,double x,double y,double heading) const;
    bool streamMotion(double& heading,double& speed) const;
    std::vector<Tile> orderedNearby(double x,double y) const;
    void request(double x,double y);
    int goCount=0;
    // Go, from the menu or the Destination tab: the typed place
    // (native/destination.hpp), or the reason it is not one, on both screens.
    bool go();
    // ── the arrival (native/arrival.hpp) ────────────────────────────────────
    //
    // Go flies the globe down to the place picked for ten seconds while the
    // tiles there cook and mount behind it; at ten its satellite picture fades
    // into them, the player falling through it, Fortnite-style; at twenty he
    // opens his parachute fifty metres above the ground he chose and lands it
    // himself. He steers from the moment the world shows. The fade never
    // finds the world unready: the zoom waits for it instead, and says what
    // for, on screen and in the log (CLAUDE.md §3).
    enum class Drop { None, Zoom, Fall, Canopy };
    struct Arrival {
        Drop phase=Drop::None;
        r1::arrival::Clock clock;
        r1::arrival::GlobeFlight flight;
        r1::arrival::Freefall fall;
        r1::arrival::Canopy canopy;
        // He hangs in the air over the ground chosen, the world under him
        // mounting: gameplay runs behind the globe from here on.
        bool armed=false,water=false,ready=false;
        double goalLon=0,goalLat=0,openLon=0,openLat=0,ground=0;
        // The globe was flown to a fall planned from the point picked; the
        // ground found there can be a few metres off, a hill higher. Once it
        // is known the globe moves onto it, over kDropAlignSeconds.
        double plannedLon=0,plannedLat=0,plannedAbove=0;
        double shiftLon=0,shiftLat=0,shiftAltitude=0,aligned=0;
        double lookYaw=0,lookPitch=0;   // his look while the autopilot flies him
        double held=0,heldSaid=-1e9;    // real seconds the zoom has waited
        std::string waitingFor,place;
        // Opened: when, how high above what was under him, and above the
        // ground chosen -- the fifty metres promised are above that.
        double agl=0,openedAt=0,openedAbove=0,openedOverGoal=0,upright=0,canopyCamera=0;
        double canopyWanted=0;          // the heading the canopy is turning to
        double ui=1e9;                  // seconds since the caption was written
        bool named=false;               // `place` is a name, not the coordinates
        bool groundReady=false;         // all visible ground/buildings/collisions are ready
        // Smoothness by phase -- the globe, the world fading in, the fall, the
        // canopy -- said when he lands: frames, frames over 33 ms, the worst.
        int frames[4]{},hitches[4]{}; double worst[4]{};
        std::chrono::steady_clock::time_point started;
        int stage() const {
            using namespace r1::arrival;
            return phase==Drop::Canopy?3:clock.seconds>=kFadeSeconds?2:clock.seconds>=kZoomSeconds?1:0;
        }
        std::string smoothness() const {
            static const char* names[4]={"globe","fade","fall","canopy"};
            std::string out;
            for(int k=0;k<4;++k)out+=std::string(k?", ":"")+names[k]+" "+std::to_string(hitches[k])+"/"+std::to_string(frames[k])
                                     +" over 33 ms (worst "+std::to_string(int(worst[k]))+" ms)";
            return out;
        }
    } drop;
    static constexpr const char* kParachuteModel="assets/models/external/parachute/parachute.glb";
    static constexpr const char* kWindSound="assets/audio/arrival/wind_freefall.wav";
    static constexpr const char* kFlutterSound="assets/audio/arrival/canopy_flutter.wav";
    static constexpr const char* kOpeningSound="assets/audio/arrival/canopy_open.wav";
    // What the fade waits for beyond the ground, the buildings and the
    // collisions -- the streets OSM has yet to send, the trees and street
    // furniture, the textures -- at most this long; then it goes, and says so.
    static constexpr double kDropExtrasWait=25.;
    // Tiles of the neighbourhood ready, the landing one among them, before
    // the world shows.
    static constexpr size_t kDropTiles=4;
    static constexpr double kDropAlignSeconds=.8;
    // The pelvis, as a fraction of his height (humans.json: 0.896 of 1.826).
    static constexpr double kPelvis=.49;
    // The shoulders, where the risers meet, likewise.
    static constexpr double kShoulders=.82;
    // The canopy is his equipment: drawn at the people's scale (humans.json).
    static constexpr double kParachuteScale=.8*kPeopleVehicleResize;
    saida::Node* canopyNode=nullptr; double canopyFold=-1;
    glm::vec3 canopyRest{0},canopyGround{0}; glm::quat canopyTurn{1,0,0,0};
    bool canopyMissingSaid=false;
    saida::WebCanvasNode* arrivalUi=nullptr;
    saida::AudioID windSound=saida::kInvalidAudioID,flutterSound=saida::kInvalidAudioID;
    bool dropSoundSaid=false;
    float sceneFov=62.f;
    // A camera handed from one rig to the other eases between them.
    struct CameraPose { glm::vec3 position{0}; glm::quat rotation{1,0,0,0}; float fov=62.f; };
    std::optional<CameraPose> cameraFrom; double cameraHandoff=1;
    // --dive: the smoke flies the whole arrival (World::runSmokeDive).
    bool smokeDive=false,smokeDiveFaded=false,smokeDiveShotQueued=false;
    glm::dvec2 smokeDiveSteerFrom{0}; bool smokeDiveSteerSet=false; double smokeDiveSlid=-1,smokeDiveHealth=0;

    bool dropFlying() const;
    bool dropGlobe() const;
    bool dropSteered() const;
    // Toaster mode's close fog and short far plane are for the ground. From
    // the air they hid the drop zone whole: the globe hands over a kilometre
    // up, past the 800 m far plane, into 650 m of fog. The arrival flies
    // clear, and the instant he lands the fog is back (World::landDrop).
    bool toasterHaze() const;
    bool giSettled();
    // A headset has no globe to fly; the smoke spawns at once unless --dive.
    bool cinematicArrival() const;
    static double smooth(double x);
    static double smoother(double x);
    // Where the world streams from: the point picked while it loads, the
    // ground chosen while he falls toward it -- steering or not, the nine
    // tiles cooked for the fade stay while he is within kDropStreamReach of
    // it (a row further would evict three of them in front of him) --
    // otherwise him.
    static constexpr double kDropStreamReach=450.;
    glm::dvec2 streamAnchor() const;
    // Metres east and north from one point to another, in the origin's frame
    // (the frame headings are measured in).
    glm::dvec2 metresTo(double x,double y,double tx,double ty) const;
    // The globe's zoom for an altitude in Earth radii (GlobeView::altitude
    // decreases with the zoom, so this is a bisection).
    static double zoomForAltitude(double altitude);
    std::string dropLabel();
    // What is under him in the air: the ground, a deck, the water or a roof,
    // as an aircraft sees it (World::surfaceUnder); over a tile not yet in,
    // the ground chosen.
    double dropSurface(double x,double y,double from);
    void startDrop();
    // A drop given up or replaced: nothing of it is left on screen or heard.
    void endDrop();
    // The ground under the point picked is ready: he is put in the air above
    // it, where the fall begins, and the world behind the globe comes alive.
    void armDrop(double x,double y,bool water);
    // Is the world behind the globe what the fade must find? The ground,
    // buildings and collisions of what the fade shows -- the tiles within
    // the globe's last picture, and the one he lands on -- then,
    // waited for at most kDropExtrasWait, their streets, trees and textures.
    // The rest of the neighbourhood streams during the fall. A machine short of memory cooks fewer
    // tiles at once (the service waits for memory): the fade must not wait
    // for nine where it shows four. Names what is missing (CLAUDE.md §3).
    bool dropShows(Tile t) const;
    // Empty when tile `t` has its ground, collisions and buildings mounted;
    // otherwise what it still lacks.
    std::string tileReadiness(Tile t,const std::string& sources) const;
    bool dropReady();
    // Every frame of the arrival, whether or not the world shows yet.
    void advanceDrop(double dt);
    void applyDropUi();
    void ensureArrivalCanvas();
    void arrivalText(const std::string& id,const std::string& value);
    void captionDrop(double dt);
    void updateDropSounds(double dt);
    // ── in the air ──
    void flyDrop(double dt,double f,double r);
    void openCanopy();
    void flyCanopy(double dt,const r1::arrival::Steer& in);
    void landDrop(bool wet,double ground);
    // ── his body in the air ──
    void releaseDropPose();
    void poseDrop(double dt,const r1::arrival::Steer& in);
    // The canopy over his shoulders: out of the container as a snake of
    // fabric, then a wing; its lines meet where his risers would.
    void placeCanopy(double dt);
    // After the landing the wing comes down in front of him, then is packed.
    void foldCanopy(double dt);
    // ── the camera in the air ──
    void placeDropCamera(double dt);
    // Behind the opaque globe nothing of the world is drawn: a city seen from
    // the air cost 75 ms a frame there for no picture -- 26 of them the relief
    // to the horizon, which the far plane does not cut (World::updateFarRelief
    // hides it) -- and texture uploads go one a frame, so slow frames filled
    // the decoder until it refused models.
    // Streaming, mounting and levels of detail go on; the world is drawn
    // again half a second before the fade, so its first frames are warm.
    // Hidden until half a second before the fade, and for as long as the zoom
    // waits for the world: the warm-up begins when it is ready.
    bool behindGlobe() const;
    void hideBehindGlobe();
    // The globe's pose: flown to the drop zone, then, fading, the world's camera.
    void flyGlobe();
    // ── --dive: the smoke flies the whole arrival ──
    void smokeDiveFade();
    void runSmokeDive();
    // R1WORLD_DIVE_SHOT=<png> at R1WORLD_DIVE_SHOT_AT=<seconds after Go>:
    // a picture of that instant of the arrival, as it is, then the run ends.
    void smokeDiveShot();
    // The smoke's keys during the arrival: two seconds of right from 13 s.
    void smokeDiveKeys(double& f,double& r);
    void smokeDiveLanded(double sink,double away,bool steered,double tau);
    // The Sun is a function of where the player stands, and in this game that
    // moves: a fixed anchor would light Sydney at Paris's hour, on the wrong
    // side of the sky, in the wrong season. `scripts/sun_cycle.js` owns the
    // model -- porting it a third time into C++ is exactly what the parity test
    // exists to prevent -- so the game only tells it where the observer is, and
    // the script keeps the beam, the ambient, the horizon and the two exposures
    // consistent with that place. Called wherever the floating origin is
    // rebased, which is every spawn and every 350 m walked.
    bool findSun();
    bool tellSun();
    // The air thins with altitude over the Earth, not over the scene's
    // tangent plane: the engine's fog is given the sphere that fits the
    // ground under the player (centre of curvature, Gaussian radius), so a
    // ray to the horizon climbs out of the haze as it does.
    void tellPlanet();
    void moveSun();
    Loaded* tile(double x,double y);
    bool onSeaIce(double x,double y);
    // ── headings ────────────────────────────────────────────────────────────
    //
    // A heading is an angle in the origin's frame, the frame the camera and
    // every model are drawn in. A step along it is turned into the local east
    // and north where the step starts. Far from a pole the two are the same
    // to a hundredth of a degree; near one they are not, and a heading kept
    // against the local north walks a circle around the pole and turns back
    // at it. This keeps a straight line straight, over the pole included.
    glm::dvec2 onward(double x,double y,double east,double north) const;
    // The origin follows the player every 350 m. What the player owns is
    // written in its frame, so every heading turns with it: at the pole the
    // new frame can face the other way from the old one.
    void rebaseOrigin();
    // ── the tile's data, in the form the frame reads it ─────────────────────
    //
    // The tile's manifest is the contract with the generator. What
    // it must not be is the thing a hot loop reads: every `data["bounds"]` is a
    // hash lookup, every `poly[i][0]` is a bounds-checked variant unwrap, and
    // `water[row].get<std::string>()` allocates a string per query. Walking or
    // driving asks these questions tens of thousands of times a second --
    // Height sampling used to repeat JSON accesses across nine tiles for
    // numbers that never change once a tile is mounted.
    //
    // So they are unpacked once, at mount, into plain arrays. Nothing here is a
    // different answer to any question: it is the same data, read the way a
    // frame can afford to read it.
    static void unpack(Loaded& tile);
    bool inside(const Loaded& tile,double x,double y,double margin=0.) const;
    // Is this coordinate on a pier deck? Decks are drawn over the water and
    // walked on, so they answer before the water and before the terrain.
    bool onDeck(const Loaded& t,double x,double y,double* level=nullptr) const;
    // Is this coordinate on a street that crosses a water cell? A cell is
    // water or not as a whole; a street through it is dry, always.
    bool onDryStreet(const Loaded& t,double x,double y) const;
    // The surfaces off the ground at (x, y): bridge decks and embankments
    // (gen/bridges.hpp). Each is handed to `seen(level, solid, parapet)`:
    // `solid` says whoever is below it meets a wall, not a ceiling; a
    // parapet is the edge of a deck, a wall for whoever stands on it.
    template<class F> void raisedAt(const Loaded& t,double x,double y,F&& seen) const {
        if(t.raised.empty())return;
        const glm::dvec3 p=t.frame.local(ecef(x,y,0.));
        const glm::dvec2 q(p.x,p.z);
        if(q.x<t.raisedLow.x||q.x>t.raisedHigh.x||q.y<t.raisedLow.y||q.y>t.raisedHigh.y)return;
        for(const RaisedPiece& r:t.raised) {
            if(q.x<r.low.x||q.x>r.high.x||q.y<r.low.y||q.y>r.high.y)continue;
            const glm::dvec2 d=r.b-r.a;const double l2=glm::dot(d,d);
            if(l2<=0)continue;
            // Each piece is a capsule: round past its ends, so the outside of
            // a bend is covered where two pieces meet.
            const double f=std::clamp(glm::dot(q-r.a,d)/l2,0.,1.);
            const glm::dvec2 across=q-(r.a+f*d);
            const int side=glm::dot(across,glm::dvec2(-d.y,d.x))>=0?0:1;
            const double top=r.ya+(r.yb-r.ya)*f,off=glm::length(across),half=r.half[side];
            if(off<=half)seen(top,r.solid,false);
            else if(r.solid)seen(top-(off-half)/1.5,true,false);
            // A band wider than a fast car's step in one frame.
            else if(r.rail[side]&&off<=half+1.2)seen(top,false,true);
        }
    }
    // What a walker or a car at altitude `standing` stands on at (x, y): the
    // highest surface within a step of him, so a bridge carries whoever is on
    // it and passes over whoever is under it. With no `standing`, the highest.
    double height(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN());
    double terrainHeight(const Loaded& tile,double x,double y) const;
    // The tile's own ground grid, decks aside; clamped to the tile.
    static double gridHeight(const Loaded& tile,double x,double y);
    // The tile's water grid and fully oceanic tiles must answer the same
    // question for walkers, cars and boats. Piers remain dry walkable decks.
    // `standing`: whoever is at that altitude on a bridge over the water is dry.
    bool onWater(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN());
    int waterCode(const Loaded& t,double x,double y) const;
    // Where a boat may be: an ocean tile, or a water cell of a land tile that
    // is not under a pier. The same cells the terrain is drawn from, so a boat
    // stops at the shore the eye sees.
    bool navigable(double x,double y);
    // The surface a boat floats on: the sea's 0 m, or a lake's own level.
    double waterLevel(double x,double y);
    // The ground under the water, where something says what it is. The relief
    // sources stop at the shore: open sea is 0.0 m, exactly, at every grid
    // point (Mapzen), and inland water is drawn on its own ground. Only a
    // shore profile the beach generator sculpted, a rock or a shoal gives the
    // sea a bed. A grid quad whose four corners all read 0.0 has none.
    std::optional<double> waterBed(double x,double y);
    // He swims when half of him is under the surface, and walks otherwise:
    // a rock, a sandbar or the swash is ground. Water whose bed nobody
    // knows is deep, as it always was. `standing` is where his feet rest,
    // as the character solver found it: a breakwater, a slipway or a rock
    // the physics carries above the ground grid is his bed there, though
    // neither the grid nor `height` knows it.
    bool deepWater(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN());
    // Collider authoring only: detection and response belong to Saida/Jolt.
    static saida::StaticBodyNode* boxCollider(saida::Node& parent,const std::string& name,
                                            glm::vec3 size,glm::vec3 center);
    static void meshCollider(saida::Node& parent,std::unique_ptr<saida::MeshNode> mesh);
    void vehicleCollider(saida::Node& node,const VehicleModel& spec);
    saida::QueryFilter obstacleFilter() const;
    bool solidAt(double x,double y,double level,float radius=.32f) const;
    // Spawn/exit/parking occupancy uses the engine's scene queries. Movement
    // itself uses the character solver, without a second footprint test.
    bool blocked(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN());
    // On foot, only water out of his depth stops him: he wades in the rest.
    bool footBlocked(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN());
    // A wall, a body or a roof over him, water aside.
    bool obstructed(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN());
    // ── the car ─────────────────────────────────────────────────────────────
    //
    // Everything below shares the walk's vocabulary on purpose: the same
    // `advance` on the ellipsoid, the same `blocked` against streamed
    // engine bodies and water, the same `height` off the tile's own grid. A car
    // that used a second notion of where the ground is would disagree with the
    // player about it the first time he stepped out.

    // Terrain height, or `fallback` where no tile answers yet. `height` throws
    // by design -- walking off the edge of the loaded world is a refusal the
    // walk reports -- and the car asks four times a frame to sit on a slope, so
    // it asks the question that has an answer.
    double groundAt(double x,double y,double fallback);
    // Yaw, then the slope the wheels are actually standing on. Measured off the
    // terrain grid over the car's own wheelbase and track rather than inferred
    // from a normal: it is the same surface the collision reads (§3 I5).
    glm::quat carRotation();
    // The four wheels of whatever car the player is now in. Re-run on every
    // take-over, because the wheels that turn must belong to the car he is
    // actually sitting in.
    //
    // Outermost match only, and it is not a detail: the importer wraps each
    // named node around a mesh node that inherits the name, so descending into
    // a wheel finds the same wheel again and every turn would be applied to it
    // twice. Eight wheels on a saloon is what said so.
    void collectWheels(saida::Node& root);
    void placeCar();
    // Somewhere within `radius` metres of (x0,y0) that is on loaded terrain,
    // out of the water and out of a building. The golden-angle spiral is the
    // spawn search's, for the same reason it is used there: it covers a disc
    // evenly without preferring a compass direction.
    // `standing`: the level the spot must be on -- beside a car on a bridge
    // is on the bridge, not on the ground six metres below its parapet.
    bool freeSpot(double x0,double y0,double radius,double& outLon,double& outLat,
                  double standing=std::numeric_limits<double>::quiet_NaN());
    // How far one can go from (x, y) at `level` along a heading before a
    // wall, a parapet, water or the edge of the loaded world: the level is
    // followed step by step, up a ramp and onto a deck.
    double clearAhead(double x,double y,double level,double heading,double step,double limit);
    // A building owned by a neighbouring tile can stream in over the spot the
    // car was parked on before that tile mounted: move the car out, and say so.
    void reparkIfCovered(const std::string& key);
    // Park the car beside a dry spawn. A swimmer cannot arrive with a car.
    void parkCar();
    // ── taking a car ────────────────────────────────────────────────────────
    //
    // Any car, not one car. The one the spawn parks is simply the first, and
    // from there the player takes whichever is nearest — his own, one he left
    // in a side street an hour ago, or one that was driving past a second ago.
    //
    // The car he takes is the car he drives: the traffic node is handed over
    // rather than copied, and the node he was using stays exactly where he
    // stepped out of it. Copying would be less code and it would teleport the
    // car he just parked to wherever he happens to be standing, which is the
    // one thing a player notices immediately.
    void abandonCurrentCar();
    void keepParked(const Parked& entry);
    void clearAbandonedCars();
    // Where a point in this tile's frame is on the planet, to within the metre
    // or two that separates it from the player. Going through `advance` rather
    // than inverting the frame keeps this file to one direction of geodesy:
    // the offset is read in the tile's tangent plane, where east is +x and
    // north is -z, and walked from the player's own coordinate.
    glm::dvec2 geodeticOf(const Loaded& tile,const glm::vec3& local) const;
    // The nearest car the player can reach: his own, one he abandoned, or one
    // in traffic. Returns false when there is none.
    struct Reach {saida::Node* node=nullptr; Loaded* tile=nullptr; size_t agent=0; int kind=0;};
    bool nearestCar(Reach& out,double& distance);
    // Hand a traffic car over: it leaves the flow, leaves its tile, and becomes
    // the car the player is in. The flow must forget it first -- a car being
    // driven by the player and by the simulation at once is two cars.
    bool takeTrafficCar(const Reach& target);
    void takeAbandonedCar(const Reach& target);

    // Why the car last stopped against something, for whoever reads the log.
    std::string carStop="nothing";
    std::string carStopSaid; // the obstacle the journal last named, cleared once the car moves on
    bool parkingCar=false;
    double carDistance() const;
    void removeTrafficRider(saida::Node& vehicle);
    bool ridingBike() const;
    glm::vec3 motorcycleContact(saida::Node& rider,const std::string& vehicle,int hand,bool grip);
    void bikeHandlePose();
    void mountPlayerOnBike();
    void dismountPlayerFromBike();
    bool enterCar();
    // Exit at any speed or attitude. A blocked door falls back to the clear
    // space above the chassis; an airborne exit falls from its actual height.
    bool leaveCar();
    void sinkCar(double x,double y);
    void updateSinkingCar(double dt);
    void captureCarPose();
public:
    bool needsAfterSimulation() const;
    void afterSimulation(float);
private:
    void driveCar(double dt,double throttle,double steerInput,bool handbrake);
    // ── boats ───────────────────────────────────────────────────────────────
    //
    // Every boat the harbours moored can be taken: the manifest says where each
    // lies and how it handles, and the node that stood there is handed over,
    // exactly as a traffic car is. From the helm, the throttle and the rudder
    // are the walk's two axes; the boat carries its speed and heading from one
    // frame to the next, and it stops -- out loud -- where the water does.
    enum class BoatSource {None, Moored, Left, Sea};
    struct BoatReach {Loaded* tile=nullptr; size_t index=0; BoatSource source=BoatSource::None;};
    enum class HopResult {NoTarget, Refused, Boarded};
    static double segmentDistance(glm::dvec2 p,glm::dvec2 a,glm::dvec2 b);
    // Distance from the player to a hull's side, in metres, in `frame`.
    double hullDistance(const Frame& frame,glm::dvec3 centre,double heading,double length,double beam) const;
    saida::Node* mooredNode(Loaded& t,Mooring& m);
    bool nearestBoat(BoatReach& out,double& distance,double extra=0);
    bool enterBoat();
    bool board(const BoatReach& target);
    void keepBoat(Vessel vessel);
    struct BoatExit {glm::dvec2 where; bool swimming;};
    std::optional<BoatExit> boatExit();
    // At a quay the player steps ashore. Offshore, he drops into the water and
    // can swim back to a bank or to the same hull. Speed still makes either
    // manoeuvre unsafe.
    bool leaveBoat();
    // From one hull to another: alongside, at matched speeds. The boat left
    // behind stays where she is, like any boat stepped off.
    HopResult hopAboard();
    void clearBoats();
    // One step of the boat. Water resists with the square of the speed, so a
    // hull settles at its top speed rather than being clamped to it; the rudder
    // needs way on to bite, and bites the other way going astern.
    void sailBoat(double dt,double throttle,double rudder);
    // Afloat: a heave and a slow pitch and roll that shrink with the hull, so
    // a dinghy bobs and a container ship does not.
    glm::quat boatSway(const Vessel& v,double phase,double& heave) const;
    void placeBoats();

    // ── aircraft ────────────────────────────────────────────────────────────
    //
    // Every aircraft the generator parked can be taken, the way a moored boat
    // or a traffic car is: the node on the stand is handed over, never copied.
    // Three handlings, one per class (assets/models/aircraft/fleet.json):
    //
    //  - an airliner is heavy: its engines take seconds to spool, it rolls
    //    slowly, it needs a long run to rotate;
    //  - a business jet is quick in everything and turns on a wingtip;
    //  - a helicopter spins up, hovers by itself when the stick is let go,
    //    climbs and descends on its own keys and goes where it points.
    //
    // Arcade, as the car is (README, "The aircraft"): turns are coordinated
    // and faster than real ones, and nothing crashes. A building stops an
    // aircraft where it touches it, out loud, and the ground takes a hard
    // landing as a stop.
    static void aircraftLod(saida::Node& node);
    void buildAircraftPrototypes();
    // The tile's parked aircraft, as scene nodes in its frame. Children of the
    // tile, so evicting the tile clears them with it.
    // Lettering (gen/predict facadeLettering): the sign font's glyphs, inked
    // with its coverage so no baked plate shows, in `colour`, facing +Z.
    std::unique_ptr<saida::Node> lettering(const std::string& text,double width,double height,glm::vec3 colour);
    // A tile's own lettering: fuel canopies and totems (gen/fuel).
    void mountLettering(Loaded& t);
    void mountAircraft(Loaded& t);
    static void collectAircraftParts(Aircraft& a);
    static double fuselageHalf(const r1::AircraftType& t);
    double roofAt(double x,double y,double above,saida::CollisionObjectNode* ignore=nullptr);
    bool buildingAt(double x,double y,double level);
    // What an aircraft rests on at (x, y): the terrain, a deck, the water's
    // level, or a roof it is above. Where no tile answers yet, the last
    // surface it saw -- said on screen, never guessed at.
    double surfaceUnder(double x,double y,double above,bool* onRoof=nullptr,bool* wet=nullptr);
    // The aircraft's extremities -- nose, tail and the two tips of its wings
    // or its rotor -- against the buildings, at the height of its underside.
    bool aircraftHits(double x,double y,double alt,double heading,bool sweep=false);
    enum class PlaneSource {None, Parked, Left};
    struct PlaneReach {Loaded* tile=nullptr; size_t index=0; PlaneSource source=PlaneSource::None;};
    bool nearestAircraft(PlaneReach& out,double& distance);
    bool enterAircraft();
    void keepAircraft(Aircraft a);
    // F leaves the controls at any moment, as in GTA. On the ground the pilot
    // steps down beside the nose, on the left where the doors are; on water he
    // drops into it. In the air or on a roof he jumps: he falls from the
    // aircraft's height to the ground beside it, and an aircraft left in the
    // air falls too (World::dropLeftAircraft).
    bool leaveAircraft();
    // An aircraft left in the air falls, levelling, until it rests on what is
    // under it: the ground, a roof or the water. It stops there, no crash (the
    // player's call). Over ground not loaded yet it waits for it.
    void dropLeftAircraft(Aircraft& a,double dt);
    void clearAircraft();
    // One horizontal step. A building stops the aircraft where it touches it
    // and says so -- no crash (the request was explicit). The edge of the
    // loaded world stops it on the ground; in the air it flies on over the
    // last surface it saw, and the HUD says the ground ahead is unknown.
    bool moveAircraft(double dt,double horizontal);
    // A plane. W/S move the throttle lever, and the engines follow it at
    // their own pace; on the ground S at idle brakes, then pushes back. A/D
    // steer the nose wheel on the ground and bank in the air; the turn
    // follows the bank. Space pulls the nose up, Shift pushes it down, and
    // with neither the nose settles back to level.
    void flyPlane(double dt,double f,double r,double pitchIn);
    // A helicopter. The rotor takes three seconds to come up to speed. Space
    // and Shift climb and descend and it holds its height with neither;
    // W/S fly it forward and back, A/D turn it on the spot. It sits down on
    // the ground, on water or on a roof, wherever it is lowered.
    void flyHelicopter(double dt,double f,double r,double up);
    glm::quat aircraftRotation(const Aircraft& a) const;
    // Turned about the centre of gravity, so a bank rolls the aircraft about
    // itself rather than about its wheels.
    void placeAircraft(double dt);

    // ── ships at sea ────────────────────────────────────────────────────────
    //
    // The sea service predicts the ships around the player -- six years of
    // real AIS, the day, the weather, and whatever live AIS it last heard
    // (gen/sea.cpp) -- on its own thread. The game sails them.
    // A ship keeps its own state once it is out: a later prediction that no
    // longer lists it does not pull it from under the player's eyes, it sails
    // on and is dropped beyond kSeaKeep; a new one appears only beyond
    // kSeaAppear, so nothing pops into view. Each can be boarded.
    bool gameTime(double& out);
    bool localConditions() const;
    // Keep observations separate: returning to real weather restores the
    // local forecast, while a forced atmosphere applies at every coordinate.
    using Weather=r1::WeatherState;
    Weather observedWeather;
    Weather effectiveWeather() const;
    bool fogFar=false;  // what the fog was last told about the horizon
    bool fogToaster=false;  // ...and whether it was toaster mode's (World::toasterHaze)
    static double number(const json& j,const char* key,double fallback);
    void readConditions();
    // The measured visibility is the fog (sun_cycle.js). From 24 km up it is
    // clear air: the weather models stop there (24.14 km, fifteen miles), and
    // the photographed horizons of clear days read 150 km and more, so a
    // reading at the models' ceiling is the clear default, not 24 km of haze.
    static constexpr double kVisibilityCeiling=24000.;
    bool applyWeather();

    saida::ParticleSystemNode* rainfall=nullptr;
    r1::RainState rainState;
    double rainShelterAge=1e9;
    glm::vec3 rainShelterEye{0.f};
    bool rainSheltered=false;
    glm::vec3 weatherWind() const;
    void clearRain();
    void updateRain(double dt);
    // Streets and buildings that arrive after the player (a provisional
    // tile cooked again) can land on him: he is moved to the nearest free
    // ground, and told why.
    void keepStanding();
    // ── snow in the air ─────────────────────────────────────────────────────
    //
    // Falling snow when the forecast says it snows, drifting snow when the
    // wind lifts what lies on the ground (on the pack, or wherever the model
    // says snow lies), both carried by the measured wind. They are drawn
    // around the camera and nowhere else, and dimmed with the daylight: a
    // particle is not lit by the scene.
    saida::ParticleSystemNode* snowfall=nullptr;
    saida::ParticleSystemNode* drift=nullptr;
    double daylightNow=1,daylightAge=1e9;
    // Snow on the ground: when the model says it lies (3 cm or more, Open-
    // Meteo's snow depth for the place), the ground and the roofs of every
    // resident tile wear the photographed snow, and take their own surfaces
    // back when it melts. The streets are left clear -- they are ploughed --
    // and so are the ground classes that are already snow, ice or water.
    saida::Material* snowMaterial(bool doubleSided);
    static bool snowable(const std::string& name);
    void updateSnowCover();
    saida::ParticleSystemNode* emitter(const char* name);
    void updateSnow(double dt);
    // The offset the HUD clock reads: the place's zone when Open-Meteo gave
    // one, otherwise the longitude's whole hours, labelled as an estimate.
    int utcOffset(bool& known);
    std::string localClock();
    // "Forcer l'heure à" (Options). The hour is the one the HUD clock shows,
    // so the instant depends on the zone under the player: it is recomputed
    // every frame, and the Sun is told only when it moves -- a new zone, a
    // new local day, or the option itself. With no forced hour the script
    // follows the real clock, which is its own default.
    void applyForcedTime();
    // A test run neither reads nor rewrites the player's options: the smoke
    // test and the gallery light the world by the clock or by --at.
    bool testRun() const;
    void loadOptions();
    void saveOptions();
    void sayForcedTime(const std::string& problem={});
    void sayForcedWeather(const std::string& problem={});
    void sayToasterMode(const std::string& problem={});
    void sayFullscreen(const std::string& problem={});
    void setFullscreenMode(bool enabled);
    void toggleFullscreenMode();
    void applyGIQuality();
    void sayGIQuality(const std::string& problem={});
    void setGIQuality(int quality);
    void applyToasterResolution();
    // Textures and meshes resident at once. Toaster mode targets the
    // reference MX450 and its 2 GiB of VRAM (plan §3 I4): 512 MiB leaves room
    // for the geometry arena, render targets and other GPU allocations.
    // Standard targets an ordinary gaming PC with 8 GiB: Paris alone keeps
    // some 950 MiB live, and 3 GiB keeps what was just seen from being
    // evicted and uploaded again.
    void applyGpuBudget();
    void applyToasterLods();
    void setToasterMode(bool enabled);
    void forceWeather(r1::WeatherOverride mode);
    void showOptions(bool show);
    void forceTime(std::optional<int> minutes);
    void applyTypedTime();
    std::string weatherLabel();
    double metresFrom(double x,double y) const;
    saida::Node* hullPrototype(const std::string& model,const json& doc);
    void readSea();
    void updateSea(double dt);

    // ── traffic ─────────────────────────────────────────────────────────────
    //
    // The simulation is `engine/plugins/traffic`, which knows nothing about
    // this project: no geodesy, no tiles, no scene. Everything below is the
    // half that only R1World can do — read the lane graph the generator cooked,
    // give each tile its own flow, put a scene node on each agent, and hand
    // the player's car back as an obstacle.
    //
    // Cars are tile-local by construction. A flow drives in its tile's tangent
    // frame and its nodes are children of the tile's node, so the floating
    // origin moves them for free (placeTiles) and evicting the tile evicts its
    // traffic without a single pointer to update.
    void loadPaints();
    // Disabled scene ownership keeps shared prototype resources resident.
    const VehicleModel& vehicleSpec(const saida::Node& node) const;
    static void vehicleLod(saida::Node& node);
    static uint32_t motorcycleMix(uint32_t x);
    void buildMotorcycleHelmetGeometry();
    void addMotorcycleHelmet(saida::Node& rider,uint32_t seed);
    std::unique_ptr<saida::Node> makeMotorcycleRider(const std::string& vehicle,HumanKind& kind,
                                                      uint32_t seed,bool helmet);
    void customizeMotorcycleRider(saida::Node& vehicle,uint32_t seed,double latitude);
    void buildTrafficPrototype();
    size_t vehicleKind(uint32_t seed,const saida::traffic::Lane& lane,double motorcycleChance=.05) const;
    double motorcycleChance(double lon,double lat) const;
    double trafficTime_=0;
    bool trafficValidation_=std::getenv("R1WORLD_TRAFFIC_VALIDATE")!=nullptr;
    double trafficValidationTime_=0,trafficValidationReport_=0;
    size_t trafficValidationMoving_=0,trafficValidationLive_=0;
    size_t trafficValidationSteadySamples_=0,trafficValidationSteadyMoving_=0;
    bool trafficValidationStuck_=false;
    double trafficValidationSeconds_=std::getenv("R1WORLD_TRAFFIC_SECONDS")?std::clamp(std::atof(std::getenv("R1WORLD_TRAFFIC_SECONDS")),60.,300.):60.;
    static constexpr double kTrafficValidationReportSeconds=5.;
    static constexpr double kTrafficValidationWarmupSeconds=30.;
    static constexpr size_t kTrafficValidationMinimumLive=20,kTrafficValidationMinimumMoving=10;
    void validateTraffic(double dt);
    std::array<saida::AssetID,3> signalMeshes_{};
    void prepareSignalMeshes();
    bool frameTrafficSignal();
    saida::Material* signalMaterial(uint32_t colour,bool lit);
    void buildTrafficSignals(Loaded& tile,const json& doc);
    void readGraph(Loaded& tile);
    glm::vec3 paintFor(uint32_t seed) const;
    void paintCar(saida::Node& node,glm::vec3 albedo);
    // The camera's position in one tile's tangent frame. It is the observer the
    // traffic spawns around, because it is the thing that sees.
    glm::dvec3 cameraTileLocal(const Loaded& tile) const;
    bool trafficVisible(const Loaded& tile,const saida::traffic::Pose& pose) const;
    saida::traffic::Pose physicalTrafficPose(Loaded& tile,size_t slot);
    saida::traffic::Obstacle vehicleTrafficObstacle(saida::Node& car,const Loaded& tile) const;
    void updateTraffic(float delta);
    static void resizeTrafficSlots(Loaded& tile,size_t count);
    bool transferTrafficCar(Loaded& source,size_t index,Loaded& destination);
    void preserveTrafficCar(Loaded& source,size_t index);
    void transferTraffic();
    bool trafficSlotLive(const Loaded& tile,size_t slot) const;
    // Reuse each slot's graph; dormant slots retain resources without rendering.
    void syncTrafficNodes(Loaded& tile,float delta);
    size_t trafficWanted() const;
    size_t trafficLive() const;

    // A photographed surface's texture, registered once per path and colour space.
    saida::AssetID texture(const std::string& path,bool srgb);
    saida::Material* material(const r1::Material& m);
    // The tile's node, empty but for its sea when it is all ocean: its own
    // ground, streets, works and buildings are uploaded a few parts a frame
    // by `uploadParts`, straight from what the worker prepared.
    std::unique_ptr<saida::Node> buildTile(const std::string& key,const r1::ServedTile& served);
    // The player and the car push the grass aside where they pass.
    static constexpr float kWalkerBend=0.45f,kCarBend=1.6f;
    void bendGrass();
    // The tile's grass blades (gen/grass), on its ground as drawn, in the
    // tile's own frame: the engine makes them around the camera.
    static std::unique_ptr<saida::GrassNode> grassNode(const r1::CookedTile& cooked);
    // A mount used to upload a whole tile in one frame -- 30 to 60 ms, the
    // hitch every new tile was felt as. Now the ground goes first, a few
    // milliseconds of parts a frame, and the rest follows over the next frames.
    // Physics follows each uploaded mesh; arrivals wait for its first sync.
    void uploadParts();
    void failTileUpload(const std::string& key, Loaded& tile, const std::string& reason);
    static const std::vector<PartUpload>& buildingParts(const Loaded& t,size_t index,int level);
    static const std::vector<r1::MeshPart>& buildingMaterials(const Loaded& t,size_t index,int level);
    static size_t residentVertices(const json& data);
    static size_t residentIndices(const json& data);
    static std::pair<size_t,size_t> buildingExtraGeometry(const Loaded& t);
    static bool buildingsReady(const Loaded& t);
    static bool nearBuildingReady(const Loaded& t,size_t footprint);
    // Only requested representations occupy the arena. The tiny Far stays as
    // a fallback while a closer level uploads; physical shells never change.
    void updateBuildingLods();
    void placeTiles();
    void trim();
    // Only two nearby interiors occupy the shared arena. A room containing
    // the player is pinned until they leave; hysteresis prevents door thrashing.
    void updateInteriors(double dt);
    int retailTestPhase=0;int64_t retailTestId=0;double retailTestTime=0,retailTestV=-3;const char* retailTestWait="";
    bool retailTest() const;
    // R1WORLD_RETAIL_SHOT=<png> with R1WORLD_RETAIL_SHOT_AT=outside|inside|
    // anchor photographs the store the traversal chose, facing into it (or,
    // for "anchor", from behind a mall's checkouts toward its supermarket),
    // then ends the run: the test says the door opens, only a picture says
    // it looks like a shop (rule 1 of CLAUDE.md).
    bool retailShot(const char* moment,const r1::InteriorPlan& plan,r1::P2 facing={0,0});
    void runRetailTest(double dt);
    std::unique_ptr<saida::Node> clonePlant(saida::Node& source);
    // A clone of a prototype with its empty nodes removed: every mesh it holds
    // comes back as one node carrying the transform it would have inherited.
    //
    // Uniform scale is required to compose nested TRS without introducing shear.
    static bool uniformScale(const glm::vec3& s);
    bool flattenable(saida::Node& node) const;
    void collectMeshes(saida::Node& source,const saida::Transform& above,
                       std::vector<std::unique_ptr<saida::Node>>& out);
    // Each shared model parsed once: instances share its mesh and material
    // pointers, avoiding thousands of repeated GLB and texture decodes.
    std::map<std::string,saida::AssetHandle> prototypeLoads;
    // A model the decoder's budget refused is not a model that does not
    // load: the budget was full of decoded images waiting their turn for the
    // GPU (one is prepared a frame). It is asked for again a second later, up
    // to kBudgetRetries times; then the refusal stands and is said.
    static constexpr int kBudgetRetries=30;
    struct BudgetRetry { std::chrono::steady_clock::time_point at; int tries=0; };
    std::map<std::string,BudgetRetry> budgetRetries;
    saida::Node* prototype(const std::string& path);
    bool modelsReady(const json& doc);
    saida::Node* prototypeForStartup(const std::string& path);
    // Warm only what the selected tile actually uses. Warming the entire
    // catalogue pinned every biome's scanned trees, boats and signs under
    // disabled prototype nodes; after time in the menu, the cooker could no
    // longer allocate the next required ground tile.
    std::vector<std::string> warmList;
    std::string warmTile;
    uint64_t warmSerial=0;
    void warm();
    std::unique_ptr<saida::Node> plantNode(json doc);
    void updateNature();
    // ── landmarks seen from afar ─────────────────────────────────────────────
    //
    // A landmark's full model belongs to its tile, and its tile is resident
    // only within a few hundred metres (gen/landmarks.cpp). Beyond that the
    // shipped list (assets/world/landmarks/landmarks.json) gives two lighter
    // levels and the ground they stand on: level 1 across a district, level 2
    // across a city, nothing past the haze. A far model is shown exactly when
    // the near one is not -- its tile is not resident, or has not streamed
    // it yet -- so the two are never drawn together and never both missing.
    struct FarLevel { std::string path; double until=0; size_t vertices=0; };
    struct FarLandmark {
        std::string slug,tile; double lon=0,lat=0,alt=0;
        std::vector<FarLevel> levels;
        saida::Node* node=nullptr; int shown=-1,requested=-1;
        saida::AssetHandle loading;
        // Where the near model sits in its tile's prop list, found once per mount.
        const saida::Node* seenTile=nullptr; long nearIndex=-1;
    };
    std::vector<FarLandmark> farLandmarks;
    // Levels 1 and 2 of every landmark, from the list shipped with its models
    // (assets/world/landmarks/landmarks.json, read by the generator).
    void readFarLandmarks();
    bool nearModel(FarLandmark& f);
    void placeFar(FarLandmark& f);
    size_t farVertices() const;
    void updateFar();
    // ── the pack to the horizon ─────────────────────────────────────────────
    //
    // The streamed tiles end some 600 m out; on the sea ice the horizon is
    // 4.7 km away at eye height, and on a clear Arctic day the air hides
    // nothing short of it. Past the tiles the pack goes on (gen/seaice.cpp's
    // far pack): snow or water as the observation says, curved with the
    // Earth, rebuilt when the player has walked 4 km from its centre or the
    // tile under him was cooked from a newer reading.
    struct FarPack {
        saida::Node* node=nullptr; double lon=0,lat=0; size_t vertices=0;
        std::shared_ptr<const r1::SeaIce> ice;
        bool refused=false;
    } farPack,nextFarPack;
    void placeFarPack();
    void updateFarPack();
    // ── the relief to the horizon ─────────────────────────────────────────────
    //
    // Past the streamed tiles the engine draws nested rings of terrain
    // (Saida's TerrainRingsNode): nine rings of 128 cells, 16 m to 4 km, out to
    // 262 km. Fine rings read Terrain Tiles; from 256 m they read the installed
    // relief without network. Both fall back to local observations, sampled
    // on a thread of their own (gen/far_relief); a ring is sampled again when
    // the player has moved two of its cells. The resident tiles are holes in
    // the rings, so the two never overlap and never leave a gap. The images
    // are kept on disk: a place visited sees its horizon offline.
    static constexpr double kFarBaseSpacing=16.0;
    static constexpr int kFarLevels=9;
    static constexpr double kFarReanchor=20000.0;  // metres before the rings' tangent frame moves
    static constexpr auto kFarRetry=std::chrono::seconds(60);
    struct FarJob {
        std::mutex lock;
        std::vector<std::pair<int,r1::FarLevel>> done;
        std::vector<std::string> said;
        bool busy=false;
    };
    struct FarRelief {
        saida::TerrainRingsNode* node=nullptr;
        double lon=0,lat=0; r1::Anchor anchor;
        std::shared_ptr<r1::FarLayers> layers;
        uint64_t layersSeen=0,generation=0;
        std::array<glm::dvec2,saida::TerrainRingsNode::kMaxLevels> origins{};
        std::array<bool,saida::TerrainRingsNode::kMaxLevels> asked{};
        std::array<std::chrono::steady_clock::time_point,saida::TerrainRingsNode::kMaxLevels> retryAt{};
        std::shared_ptr<FarJob> job;
        double reach=0;
    } farRelief;
    void placeFarRelief();
    void updateFarRelief();
    // The trunk of a tree that has just been mounted. The compound is rebuilt
    // at the next physics sync, once however many trees joined it this frame.
    void addTrunk(Loaded& l,const json& at);
    void streamProps();
    // The nodes the parts go up under (World::uploadParts).
    void shape(Loaded& tile);
    // Everything a tile in `loaded` does beyond being drawn: its density, its
    // traffic and crowd, its aircraft. After the emplace, never before: the
    // flow holds a pointer to the graph, and a graph built in a temporary
    // would be moved out from under it.
    void activate(const std::string& key,Loaded& tile);
    // A replacement is shown once every part of its ground is draw-ready and
    // solid, in the frame its predecessor is taken down: the street is never
    // missing in between. Replacements go up together -- a tile's ground is
    // joined to its neighbours' (gen/seams), so one swapped alone would meet
    // the old version of the next one with a step -- unless the first ready
    // has waited a second for the others. One that cannot get ready says why
    // and is shown as it is rather than leaving the old version up for good.
    static constexpr double kReplaceWaitMs=10000.,kBatchWaitMs=1000.;
    bool replacementReady(const std::string& key,const Loaded& next);
    bool promoteIncoming();
    void stream();
public:
#ifdef SAIDA_ENABLE_XR
    void setXrGestureInput(const saida::xrgesture::RuntimeInputState& state);
#endif
    Impl(saida::Engine& e,fs::path g,bool test,saida::CaptureRequest capture,double startLon,double startLat,bool hop,double hopX,double hopY,saida::runtime::CaptureViewpoint view,bool sail=false,bool fly=false,bool headset=false);
    ~Impl();
    void setMenuGlobe(std::shared_ptr<r1::MenuGlobeState> model);
    bool failed() const;
    void diveSmoke();
    void inspectInstant(double unixSeconds);
    void inspectConditions(std::array<double,3> conditions);
    void inspectHeat(double temperature);
    void captureFromAltitude(double metres);
    void captureAtGeography(std::array<double,3> camera);
    void ProcessEvent(Rml::Event& event) override;
    bool smokeZoom();
    // The Options screen, as a player uses it: open it, force an hour, see the
    // Sun take it, have a bad hour refused without losing the good one, go
    // back to the real clock, close it. Presses only, like every other click.
    bool smokeOptions();
    // The driver's own two phases, kept out of update() so the flow reads:
    // walk, drive, then either teleport or finish.
    void startSmokeDrive();
    void beginSmokeDriving();
    // Getting into a car that belongs to the traffic, which is the whole of
    // "n'importe quelle voiture" and the one part of it a player cannot be
    // talked out of noticing when it is broken.
    //
    // The driver stands the player beside a live traffic car rather than
    // walking him to it: the car is moving, so walking to it is a pursuit
    // problem and this test is not about pursuit. Standing beside it is
    // exactly where stepping out of a car leaves him, so the reach it tests is
    // the reach a player gets.
    void runSmokeTakeover();

    // `--sail`: find a moored boat, stand beside it, take the helm, sail it
    // out at full throttle, be refused a landing at speed, then stop. Every
    // step fails on its own reason (rule 3 of CLAUDE.md).
    void runSmokeSail(float delta);
    void runSmokeSwim(float delta);
    // Then the ships at sea: predicted offline from the local base, they must
    // be out within half a minute, or the run says why not.
    void runSmokeSea(float delta);

    // `--fly`: whatever flies where the map was clicked, one plane and one
    // helicopter. The plane lines up on the longest clear run it can find,
    // takes off, is refused the door in the air, is then flown low at a
    // building and must stop against it -- not crash -- come down and let its
    // pilot out. The helicopter spins up, climbs, flies forward, sets down
    // and lets its pilot out (or, set down on a roof, keeps him in). Every
    // step fails on its own reason (rule 3 of CLAUDE.md).
    // R1WORLD_FLY_SHOT=<png> with R1WORLD_FLY_SHOT_AT=<moment> photographs
    // the chase view at that moment of the run, then ends it: "stand", "climb",
    // "stop", "heli-stand" or "heli". A test can say the aircraft flew; only
    // a picture says it looked like flying (rule 1 of CLAUDE.md).
    void flyShot(const char* moment);
    void flyFail(const std::string& why);
    static std::string flyCategory(const r1::AircraftType& t);
    // The parked aircraft of a category not flown yet: business jets first
    // (they take off in the shortest run), then airliners, then helicopters.
    bool smokeNextAircraft(Loaded*& found,size_t& index);
    // How far the aircraft could go along a heading before a building, the
    // water (on the ground) or the edge of the loaded world.
    double clearRun(double heading,double reach,double step,double above);
    void runSmokeFly(float delta);

    // Diagnostic scene composition; this traversal is only run by the smoke test.
    void measureWalk();
    void reportChurn(const char* phase);
    // The cheat line, as a player uses it: open, type, Enter. Each code must
    // have done its thing beside him, or the smoke fails naming which.
    bool smokeCheated=false;
    // Then the line opened and closed over real frames, as a player presses T
    // again and again: the engine's update must keep the focus it was given,
    // and the private memory must not grow with the presses. The world is
    // still loading when the walk ends -- it grew by 200 to 370 MB over the
    // next seconds whatever the line did (16 October 2026: the focus alone, a
    // style alone, the line) -- so the presses wait until the memory holds
    // still: two 2-second readings within 16 MB. A world that never settles
    // within 90 s has its keyboard checked, and the memory is said unread.
    static constexpr int kSmokeCheatPresses=60;
    int smokeCheatCycles=0; double smokeCheatMemory=0;
    bool smokeCheatSettled=false; int smokeSettleStill=0; double smokeSettleSince=-1,smokeSettleRead=0,smokeSettleLast=0;
    double smokeMountedShotDelay=-1;
    void cycleSmokeCheat(double dt);
    bool smokeCheats();
    void smokeFinishPhase();
    void sayBodies();
    // A tile's trunks are one compound body: the query the car stops on and
    // the feet's solver meet every tile's first mounted trunk, a metre off the ground.
    bool trunksSolid();
    // The height the walk and the car follow is the ground drawn and collided
    // with. A patch that disagreed by 30 cm stopped the car on open grass.
    // Coastal tiles are left out: the sea's levelling (gen/harbours
    // Cells::adjust) moves the drawn ground and not the manifest's grid.
    bool groundMatchesDrawing();
    // Two tiles meet where their ground meets (gen/seams): a step at an edge
    // stopped the car on a bare field. Well under the 0.28 m the car meets as
    // an obstacle, with room for what smoothing a staggered seam leaves on a
    // quay wall. Tiles meeting the sea are left out, as above, and the ground
    // dug under a bridge is measured apart: each tile solves its bridges on
    // its own window (gen/bridges), so a dig on a seam may differ by a little.
    bool seamsClosed();
    // True when the smoke has just failed on a refused body.
    bool checkPhysics();
    void update(float delta);
private:
    // False stops the frame exactly where the former monolithic update returned.
    bool refreshInterface();
    bool updateSession(float delta, double frameSeconds);
    void updateWorldScene(float delta);
    double updatePlayer(float delta, bool sampleMouse=true);
    void updateCamera(double dt);
    bool updateCapture(double dt);
    bool updateHud(float delta);
    void updateSmoke(float delta, double dt);

};

} // namespace r1::world
