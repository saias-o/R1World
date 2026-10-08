// Game-side integration with Saida's public C++ API.
#define NOMINMAX
#include <windows.h>
#include "Engine.hpp"
#include "core/Paths.hpp"
#include "core/Time.hpp"
#include "core/Input.hpp"
#include "core/Window.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/GLTFLoader.hpp"
#include "core/Profiler.hpp"
#include "scene/animation/Animator.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Material.hpp"
#include "nodes/GrassNode.hpp"
#include "nodes/WaterNode.hpp"
#include "nodes/CameraNode.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "nodes/TerrainRingsNode.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
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
#include "gen/crowd.hpp"
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
#include "forced_time.hpp"
#include "density_policy.hpp"
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
#include <future>
#include <mutex>
#include <thread>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;
double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
constexpr double rad = 3.141592653589793 / 180.;
constexpr double kTwoPi = 6.283185307179586;
double wrap(double x) { return x - 360.*std::floor((x+180.)/360.); }
// The tile grid is the generator's (gen/common.hpp), never a copy of it: a
// second copy rounded a row boundary differently and put the player on a
// tile the generator had not cooked under him.
using r1::columns;
constexpr int kLastRow=r1::kRows-1;
// Metres in a degree of the equator: the spherical shortcut local estimates use.
constexpr double kMetresPerDegree=r1::kMetresPerDegree;
// Keep room for shared models, UI and the player within the configured arena.
constexpr double kTileGeometryShare = .85;

// ── driving ─────────────────────────────────────────────────────────────────
//
// Arcade driving uses geographic coordinates and the streamed height field.
// Physical vehicles can use Scene::rebaseOrigin with VehicleBehaviour; this
// controller intentionally keeps the game's ground-following arcade handling.
constexpr double kCarTopSpeed=28.;      // m/s -- 100 km/h; 130 read as too fast to place a car in a street
constexpr double kCarReverseSpeed=-7.;
constexpr double kCarAccel=7.;          // m/s^2 -- 0-100 km/h in about 4 s
constexpr double kCarBrake=12.;
constexpr double kCarHandbrake=16.;
constexpr double kCarRollResist=.55;    // m/s^2 of coasting loss at rest
constexpr double kCarDrag=.0016;        // + v^2 term; together they settle near the top speed
constexpr double kCarTurnRadius=3.6;    // m, the tightest circle the front wheels cut
constexpr double kCarLateralAccel=16.;  // m/s^2 -- grip a real tyre does not have, on purpose
constexpr double kCarSteerAngle=34.;    // degrees of visible lock on the front wheels
constexpr double kCarReach=4.5;         // m -- how close you stand to open the door
constexpr double kCarExitSpeed=2.;      // m/s -- above it, stepping out is refused out loud
constexpr double kCarSinkRate=1.2;      // m/s after a car leaves the road for water
constexpr double kCarSinkDepth=4.5;     // keep the car below the surface until a teleport
constexpr double kCarKerb=.3;           // m above the ground a street surface is still driven onto
// The player's height is read from assets/models/humans/humans.json, drawn
// at its `scale`; the head stays this fraction of it clear of the water.
constexpr double kSwimHeadAbove=.33;
// Road vehicles are drawn at 80% of the size r1/vehicle_fleet.py authors
// them at, as people are at 80% of their scan (r1/humans.py): the player's
// call, made looking at them in the streets. Dimensions read from the fleet
// manifest are scaled with them, so doors, cameras and gaps agree.
constexpr double kVehicleScale=.8;
// §5: "on ne voit pas les poignées de porte à 130 km/h". 15 km/h is where
// the plan's own table stops calling it walking.
constexpr double kFastDetail=4.2;       // m/s
// What a walker or a car climbs without noticing: a kerb, a ramp's next
// metre. A surface higher than this above him is a wall or a ceiling.
constexpr double kStepUp=1.2;
constexpr double kStreamAheadSeconds=45.; // prepare the road before the car reaches it
// The floating origin follows the player (PLAN §3 I1).
constexpr double kRebaseDistance=350.;
// Soft per-frame budgets, in milliseconds: soft because one mesh upload or one
// model import cannot be split.
constexpr double kPartUploadMs=3.,kPropImportMs=2.,kMountTickMs=4.;
constexpr size_t kStreamRequestLimit=25; // the world service's bounded priority list
constexpr double kOnFootFollow=4.5,kDrivingFollow=8.5;
// One message for the whole wait after Go: every change of the status line
// re-renders the interface, a quarter of a second on the map screen.
constexpr const char* kPreparing="Préparation du terrain de départ… Vous pouvez changer de destination.";
// ── boats ───────────────────────────────────────────────────────────────────
// A boat is sailed the way the car is driven: a longitude, a latitude and a
// heading moved in the tangent plane, with one authority on position for the
// streamer, the origin and the Sun. Its handling -- top speed, acceleration,
// turn rate -- comes with it from the tile manifest (gen/harbours.cpp), because
// a container ship and a speedboat are not one boat with two paint jobs.
constexpr double kBoatReach=3.5;        // m from the hull's side to take the helm
// Ships at sea (gen/sea.cpp): how many the neighbourhood shows, how far
// away a new one may appear (never in plain view), and how far one sails on
// before it is dropped.
constexpr size_t kSeaShips=40;
constexpr double kSeaAppear=1200.;
constexpr double kSeaKeep=6500.;
constexpr double kHopSpeed=2.;          // m/s between two hulls to step across
constexpr double kBoatExitSpeed=1.5;    // m/s -- above it, leaving the hull is refused
constexpr size_t kLeftBoats=4;
// ── aircraft ────────────────────────────────────────────────────────────────
// Parked where the generator put them (gen/airports.cpp) and flown the way the
// car is driven: a longitude, a latitude, an altitude and three angles moved in
// the tangent plane. Each class handles as itself, from
// assets/models/aircraft/fleet.json; all of it is arcade and says so.
constexpr double kAircraftReach=4.;     // m from the fuselage's side to climb aboard
constexpr double kAircraftExitSpeed=2.; // m/s -- stopped: the smoke steps down, not out
constexpr size_t kLeftAircraft=4;
constexpr double kGravity=9.81;
// A body falling from a height, the player or an aircraft left in the air,
// falls no faster than a skydiver.
constexpr double kFallTerminal=55.;     // m/s
// How many traffic cars the whole neighbourhood may show at once. Not a memory
// budget -- every one of them is the same shared mesh (§5) -- but a draw-call
// one: a car is five primitives, so forty cars is two hundred draws, which is
// what the reference machine can spare beside a city.
constexpr size_t kTrafficCars=80;
// How many people the neighbourhood may show at once. Each is one skinned
// draw per material (two or three) and one animator: sixty is what the
// reference machine spends on a street without it showing in the frame time,
// and the animators far away pose at a lower rate (World::syncPeople).
constexpr size_t kCrowdPeople=60;
// How many cars the player may leave standing around before the oldest is
// cleared. They cost a node each and nothing else -- the mesh is shared -- but
// a city paved with the player's abandoned cars is its own kind of wrong.
constexpr size_t kAbandonedCars=6;
// Interiors (PLAN §8): built within kInteriorLoad metres of the door, released
// beyond kInteriorRelease, kInteriorRooms at most, the player's first. An
// entrance whose room is not built is a closed door within kClosedDoorNear,
// cleared beyond kClosedDoorFar.
constexpr double kInteriorLoad=65.,kInteriorRelease=85.,kClosedDoorNear=90.,kClosedDoorFar=95.;
// A tree shows its full model within this distance of the player, its middle
// level beyond. Forest trees stand about 12 m apart (gen/scatter.cpp: the
// inferred forest's spacing, the 11.6 m canopy cell), so a spacing and a
// quarter holds the nearest few: pi * 15^2 / 12^2 is about five.
constexpr double kTreeNearRadius=15.;
constexpr size_t kInteriorRooms=2;

struct Tile {
    int r,c;
    // The generator's own key (gen/common.hpp), so the tile the game streams
    // and the tile the generator cooks cannot disagree about their version.
    std::string key() const { return r1::Tile{r,c}.key(); }
    r1::Tile gen() const { return {r,c}; }
    bool operator<(const Tile& b) const { return std::tie(r,c)<std::tie(b.r,b.c); }
    bool operator==(const Tile& b) const { return r==b.r&&c==b.c; }
};
Tile tileAt(double lon,double lat) {
    const r1::Tile t=r1::tileAt(lon,lat);
    return {t.row,t.col};
}
// Metres along the sphere between two points.
double metresBetween(double lon1,double lat1,double lon2,double lat2) {
    const double p1=lat1*rad,p2=lat2*rad,dp=p2-p1,dl=wrap(lon2-lon1)*rad;
    const double h=std::sin(dp/2)*std::sin(dp/2)+std::cos(p1)*std::cos(p2)*std::sin(dl/2)*std::sin(dl/2);
    return 2*r1::kRMean*std::asin(std::min(1.,std::sqrt(h)));
}
// Within some 18 km of a pole a ring is a few wedges, and the three rows
// around the player no longer hold what he sees: standing on the pole, the
// ground a hundred metres away across it is in a column no row offset
// reaches. There the neighbourhood is every tile whose nearest point is
// within reach, in metres, nearest first -- twelve at the pole itself.
constexpr int kPolarColumns=200;
constexpr double kPolarReach=650.;
constexpr size_t kPolarTiles=12;

// ── physics bodies ──────────────────────────────────────────────────────────
//
// The physics world reserves its body table once, when it is built
// (saida::PhysicsCapacity), like the geometry arena (§5): its capacity is the
// sum of what the world can hold at once, not a number picked, and a body past
// it is refused out loud (World::checkPhysics). A compound is one body however
// many shapes it holds, which is why a tile's trunks are one body.
//
// A tile's static bodies: one per uploaded mesh part, one per landmark, one
// for all its trunks, and the doors of the unloaded interiors near the player.
// The densest tile measured, by the Hôtel de Ville (v25_27771_23995,
// 2026-10-01), holds 835; World::sayBodies logs the densest tile against this
// budget. The nine tiles round it hold 5 626 trunks, which used to be a body
// each.
constexpr uint32_t kTileStaticBodies=1536;
// Its moving ones: a traffic slot keeps its car's box once used, a crowd slot
// its person's capsule, each at most the whole neighbourhood's share.
constexpr uint32_t kTileMovingBodies=uint32_t(kTrafficCars+kCrowdPeople);
// The ring near a pole, plus the tile kept under the player through a teleport.
constexpr uint32_t kResidentTiles=uint32_t(kPolarTiles)+1;
// Two open interiors of at most 128 fixtures each (gen/interior_uses.cpp) with
// their walls, door leaves, tills and forecourt cars.
constexpr uint32_t kInteriorBodies=2*192;
// The player's feet and car, and the cars left standing (boats and aircraft
// carry no collider).
constexpr uint32_t kPlayerBodies=2+uint32_t(kAbandonedCars);
constexpr uint32_t kPhysicsBodies=kResidentTiles*(kTileStaticBodies+kTileMovingBodies)+kInteriorBodies+kPlayerBodies;
std::vector<Tile> polarNearby(double lon,double lat) {
    const Tile center=tileAt(lon,lat);
    std::vector<std::pair<double,Tile>> found;
    for(int r=std::max(0,center.r-2);r<=std::min(kLastRow,center.r+2);++r) {
        const int n=columns(r);
        for(int c=0;c<n;++c) {
            const Tile t{r,c};
            const r1::Bounds b=t.gen().bounds();
            const double la=std::clamp(lat,b.south,b.north);
            double lo=wrap(lon);
            if(lo<b.west||lo>b.east)lo=std::abs(wrap(lo-b.west))<std::abs(wrap(lo-b.east))?b.west:b.east;
            const double d=t==center?-1.:metresBetween(lon,lat,lo,la);
            if(d<=kPolarReach)found.push_back({d,t});
        }
    }
    std::stable_sort(found.begin(),found.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    std::vector<Tile> out;
    for(const auto& f:found)if(out.size()<kPolarTiles)out.push_back(f.second);
    return out;
}
std::vector<Tile> nearby(double lon,double lat) {
    Tile center=tileAt(lon,lat);
    if(columns(center.r)<kPolarColumns)return polarNearby(lon,lat);
    std::vector<Tile> out{center}; std::set<Tile> seen{center};
    for(int dr=-1;dr<=1;++dr) {
        int r=std::clamp(center.r+dr,0,kLastRow),n=columns(r);
        int c=int(std::floor((wrap(lon)+180.)/360.*n));
        for(int dc=-1;dc<=1;++dc) {
            Tile t{r,(c+dc+n)%n}; if(seen.insert(t).second)out.push_back(t);
        }
    }
    // Center first, then the closest terrain: the next boundary matters more
    // than the arbitrary row/column iteration order. Wrap at the date line.
    auto distance=[&](Tile t) {
        const r1::P2 middle=t.gen().center();
        return std::hypot(wrap(middle.x-lon)*std::cos(lat*rad),middle.y-lat);
    };
    std::stable_sort(out.begin()+1,out.end(),[&](Tile a,Tile b){return distance(a)<distance(b);});
    return out;
}
glm::dvec3 ecef(double lon,double lat,double alt=0) {
    const r1::P3 p=r1::geodeticToEcef(lon,lat,alt);
    return {p.x,p.y,p.z};
}
struct Frame {
    glm::dvec3 origin; glm::dmat3 basis;
    Frame(double lon=0,double lat=0,double alt=0):origin(ecef(lon,lat,alt)) {
        double l=lon*rad,p=lat*rad;
        basis=glm::dmat3(glm::dvec3(-sin(l),cos(l),0),
                        glm::dvec3(cos(p)*cos(l),cos(p)*sin(l),sin(p)),
                        glm::dvec3(sin(p)*cos(l),sin(p)*sin(l),-cos(p)));
    }
    glm::dvec3 local(glm::dvec3 p) const { return glm::transpose(basis)*(p-origin); }
};
// Great-circle step; longitude wraps across the date line. Written as a
// rotation of the unit vector rather than through asin: at the pole asin
// sees cos(d) round to exactly 1 for a step of a few centimetres, and the
// player could never walk off it. atan2 keeps every step, there as anywhere.
glm::dvec2 advance(double lon,double lat,double east,double north) {
    const double length=std::hypot(east,north),d=length/r1::kRMean;
    if(d==0)return {lon,lat};
    const double l=lon*rad,p=lat*rad;
    const glm::dvec3 here(std::cos(p)*std::cos(l),std::cos(p)*std::sin(l),std::sin(p));
    const glm::dvec3 e(-std::sin(l),std::cos(l),0.),n(-std::sin(p)*std::cos(l),-std::sin(p)*std::sin(l),std::cos(p));
    const glm::dvec3 q=here*std::cos(d)+(e*east+n*north)*(std::sin(d)/length);
    return {wrap(std::atan2(q.y,q.x)/rad),std::atan2(q.z,std::hypot(q.x,q.y))/rad};
}
struct Plant {
    saida::Node* node=nullptr;
    bool grass=false;
    bool tree=false;
    // A tree with a middle level: its full model is allowed only close by.
    saida::LODGroupBehaviour* lod=nullptr;
    bool close=false;
};
struct Footprint {
    std::vector<glm::dvec2> points;
    glm::dvec2 low{1e30},high{-1e30};
    double top=1e30;  // the highest point over it, in its tile's frame: what an aircraft clears
    int interior=-1;
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
// A cooked part as the GPU takes it, made on the worker that cooked it
// (WorldService's `prepare`), so the frame only uploads.
struct PartUpload {
    std::string name;
    std::vector<saida::Vertex> vertices;
    std::vector<uint32_t> indices;
    size_t material=0;  // index into the cooked tile's parts
    std::shared_ptr<const saida::PreparedMesh> prepared;
};
// Everything the frame needs of a cooked tile, made on the worker that cooked
// it: the parts as the GPU takes them, and building metadata for inspection.
struct PreparedTile {
    std::vector<PartUpload> parts;
    std::vector<Footprint> footprints;
};

// A generator part in the engine's vertex format.
PartUpload uploadOf(const r1::MeshPart& part,size_t index) {
    const r1::Mesh& m=part.mesh;
    const double k=part.material.uvScale;
    const auto tangents=r1::tangents(m,k);
    PartUpload up{part.name,{},m.indices,index};
    up.vertices.resize(m.positions.size());
    for(size_t v=0;v<m.positions.size();++v) {
        saida::Vertex& x=up.vertices[v];
        x.pos=glm::vec3(m.positions[v].x,m.positions[v].y,m.positions[v].z);
        x.normal=glm::vec3(m.normals[v].x,m.normals[v].y,m.normals[v].z);
        x.color=m.colors.empty()?glm::vec3(1.f):glm::vec3(m.colors[v][0],m.colors[v][1],m.colors[v][2]);
        x.texCoord=glm::vec2(m.texcoords[v].u*k,m.texcoords[v].v*k);
        x.tangent=glm::vec4(tangents[v][0],tangents[v][1],tangents[v][2],tangents[v][3]);
    }
    return up;
}

PreparedTile prepareTile(const r1::CookedTile& tile) {
    PreparedTile prepared;
    auto& out=prepared.parts;
    out.reserve(tile.parts.size());
    for(size_t i=0;i<tile.parts.size();++i) {
        auto part=uploadOf(tile.parts[i],i);
        part.prepared=saida::prepareMesh({std::move(part.vertices),std::move(part.indices)});
        out.push_back(std::move(part));
    }
    const json& tops=tile.manifest.contains("footprintTops")?tile.manifest.at("footprintTops"):json::array();
    for(const auto& polygon:tile.manifest.at("footprints")) {
        Footprint shape;
        if(prepared.footprints.size()<tops.size())shape.top=tops[prepared.footprints.size()].get<double>();
        for(const auto& point:polygon) {
            const glm::dvec2 p{point[0].get<double>(),point[1].get<double>()};
            shape.points.push_back(p);shape.low=glm::min(shape.low,p);shape.high=glm::max(shape.high,p);
        }
        prepared.footprints.push_back(std::move(shape));
    }
    return prepared;
}

// One of the crowd, as drawn: a pooled node with its own animator, the
// shared meshes of one avatar under it (World::makePerson). Over the clip,
// the engine's two modifiers: the head turned toward the player, and the
// spine's stagger when he walks into them.
struct Person {
    saida::Node* node=nullptr; saida::Animator* animator=nullptr;
    saida::RigidBodyNode* body=nullptr;  // their capsule in the engine's physics
    saida::GazeModifier* gaze=nullptr; saida::ImpactModifier* impact=nullptr;
    const char* clip=""; float poseRate=-1.f;
    uint32_t bumps=0;            // the walker's bumps already staggered
    double yaw=0; bool shown=false;  // the heading drawn, eased toward the walker's
    size_t kind=0;
};

struct Loaded {
    saida::Node* node=nullptr;
    // The cooked tile this was mounted from. Its manifest and its prop list
    // are read where they are, never copied.
    std::shared_ptr<const r1::ServedTile> served;
    const json& data; Frame frame; const json& props; size_t nextProp=0,nextLettering=0;
    // The tile's own geometry goes up a few parts a frame (World::uploadParts).
    saida::Node* geography=nullptr; size_t nextPart=0;
    std::vector<saida::Mesh*> partMeshes;
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
    // still waiting to stream -- all of them while driving -- was a wall
    // nobody could see.
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
    size_t wanted=0;                      // cars this tile's density asks for
    // The people on its pavements (gen/crowd.hpp): the same shape as the
    // traffic, for the same reason -- the crowd points at the graph.
    r1::WalkGraph walks; r1::Crowd crowd;
    double crowdEyeX=1e9,crowdEyeZ=1e9,crowdLocalPopulation=0;
    std::vector<Person> people;           // keyed by walker slot, pooled
    std::vector<size_t> humanKinds;       // stable avatar per slot in this country
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

// Interactive map tiles are requested only when their pixels are on screen.
// A completed PNG is cached indefinitely, satisfying the tile server's
// minimum seven-day cache rule without conditional re-downloads.
class MenuTileCache {
    struct Result { bool ready=false; std::string error; };
    fs::path root;
    std::vector<r1::MapTile> visibleTiles;
    std::future<Result> flight;
    std::chrono::steady_clock::time_point retryAfter{};
    std::string lastError;
public:
    explicit MenuTileCache(fs::path project):root(std::move(project)/"cache/map-tiles/osm"){}
    fs::path path(const r1::MapTile& tile) const {
        return root/std::to_string(tile.z)/std::to_string(tile.x)/(std::to_string(tile.y)+".png");
    }
    bool ready(const r1::MapTile& tile) const {
        std::error_code ec;
        const auto p=path(tile);
        return fs::is_regular_file(p,ec)&&fs::file_size(p,ec)>8;
    }
    void setVisible(std::vector<r1::MapTile> tiles){visibleTiles=std::move(tiles);}
    size_t missing() const {
        return size_t(std::count_if(visibleTiles.begin(),visibleTiles.end(),[&](const auto& tile){return !ready(tile);}));
    }
    bool unavailable() const {return !lastError.empty()&&std::chrono::steady_clock::now()<retryAfter;}
    bool pump() {
        const auto now=std::chrono::steady_clock::now();
        bool changed=false;
        if(flight.valid()) {
            if(flight.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
            const Result result=flight.get();
            if(result.ready){lastError.clear();changed=true;}
            else {
                lastError=result.error;
                retryAfter=now+std::chrono::seconds(30);
                saida::Log::info("[World map] detailed tiles unavailable: ",lastError);
                changed=true;
            }
        }
        if(now<retryAfter)return changed;
        for(const auto& tile:visibleTiles) {
            if(ready(tile))continue;
            const fs::path target=path(tile);
            flight=std::async(std::launch::async,[tile,target]() -> Result {
                try {
                    const std::string url="https://tile.openstreetmap.org/"+std::to_string(tile.z)+"/"+
                                          std::to_string(tile.x)+"/"+std::to_string(tile.y)+".png";
                    const auto response=r1::net::request("GET",url,{}, {},10.);
                    constexpr char png[]="\x89PNG\r\n\x1a\n";
                    if(response.status!=200||response.body.size()<8||std::memcmp(response.body.data(),png,8)!=0)
                        return {false,"HTTP "+std::to_string(response.status)+" or invalid PNG"};
                    fs::create_directories(target.parent_path());
                    const fs::path temporary=target.string()+".part";
                    {std::ofstream output(temporary,std::ios::binary);
                     output.write(response.body.data(),std::streamsize(response.body.size()));
                     if(!output)throw std::runtime_error("cannot cache map tile");}
                    fs::rename(temporary,target);
                    return {true,{}};
                } catch(const std::exception& e) {return {false,e.what()};}
            });
            break;
        }
        return changed;
    }
};

class World : public Rml::EventListener {
    saida::Engine& engine; fs::path game;
    r1::DensityPolicy densityPolicy{game};
    saida::WebCanvasNode* ui; saida::WebCanvasNode* minimapUi; saida::CameraNode* camera;
    saida::Node* player=nullptr;
    // The player's car. A member of the entry scene beside him, not a prop of
    // the tile it happens to stand on: it is his, so a teleport takes it along
    // instead of evicting it with the neighbourhood he left.
    saida::Node* car=nullptr;
    std::vector<saida::Node*> frontWheels,rearWheels;
    // Every car the player has stepped out of, left standing where he left it.
    // GTA's rule, and the reason the car he is in is a *pointer* rather than
    // the one node the scene shipped: taking over a traffic car hands him that
    // car, not a copy of it moved under his feet.
    struct Parked {saida::Node* node; double lon,lat,alt,yaw;};
    std::vector<Parked> parked;
    bool driving=false,wasEnterKey=false,carParked=false,carSinking=false,wheelsReported=false;
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
    bool sailing=false,swimming=false; Vessel boat; std::vector<Vessel> leftBoats;
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
    bool piloting=false; Aircraft plane; std::vector<Aircraft> leftAircraft;
    std::map<std::string,saida::Node*> aircraftPrototypes;
    // The last surface seen under the aircraft: what it keeps to over tiles
    // that have not streamed yet, rather than inventing a ground.
    double planeGround=0; bool planeStopped=false;
    bool smokeFly=false,smokeFlyWait=false; int smokeFlyPhase=0;
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

    // ── people ──────────────────────────────────────────────────────────────
    //
    // assets/models/humans/humans.json (r1/humans.py): the player and the
    // crowd, Rocketbox scans with their clips retargeted onto them. A crowd
    // avatar is imported once into the shared prototypes; each person is a
    // node of its own with its own animator over the avatar's shared meshes,
    // rig and clips, so each avatar costs one upload however many walk.
    json humans;
    double humanScale=.8,playerHeight=1.46;
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
    void loadHumans() {
        std::ifstream input(game/"assets/models/humans/humans.json");
        if(!input)throw std::runtime_error("Missing assets/models/humans/humans.json (python -m r1.humans)");
        input>>humans;
        humanScale=humans.at("scale").get<double>();
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
            warmList.push_back(kind.model);
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
    // A crowd avatar's shared parts, taken from its prototype on first use:
    // its meshes by level of detail, its rig and its clips.
    HumanKind* humanKind(size_t index) {
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
    // The Rocketbox biped's chains (r1/humans.py keeps its eyes for this).
    // A person's gaze, not a turret's: the eyes go first and furthest, the
    // head most of the way, the spine barely; past 83 degrees either side
    // the head holds at its limit rather than turn round.
    static saida::GazeModifier::Settings gazeSettings() {
        saida::GazeModifier::Settings s;
        s.chain={{"Bip01 Spine2",.15f,.25f},{"Bip01 Neck",.35f,.45f},{"Bip01 Head",1.f,.8f},
                 {"Bip01 LEye",1.f,.35f},{"Bip01 REye",1.f,.35f}};
        s.maxYaw=1.45f;s.maxPitchUp=.4f;s.maxPitchDown=.6f;s.response=5.f;
        return s;
    }
    // A blow bends the back from the waist up, the head last and loosest,
    // and the body swings back upright in under a second.
    static saida::ImpactModifier::Settings impactSettings() {
        saida::ImpactModifier::Settings s;
        s.chain={{"Bip01 Spine",.3f},{"Bip01 Spine1",.25f},{"Bip01 Spine2",.2f},{"Bip01 Neck",.1f},{"Bip01 Head",.15f}};
        s.frequency=1.4f;s.damping=.4f;s.maxAngle=.55f;
        return s;
    }
    // People whose skeleton lacks a bone the gaze or the stagger needs:
    // said once each (CLAUDE.md rule 3), and a failed smoke.
    size_t bodiesRefused=0;
    bool makePerson(Loaded& tile,size_t slot,Person& person) {
        const size_t selected=tile.humanKinds[slot%tile.humanKinds.size()];
        HumanKind* kind=humanKind(selected);
        if(!kind)return false;
        person.kind=selected;
        auto root=std::make_unique<saida::Node>("person-"+kind->name);
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
        // meet (World::moveFeet). In the node's units, drawn at scale.
        auto body=std::make_unique<saida::RigidBodyNode>();
        body->kinematic=true;
        body->transform().position=glm::vec3(0.f,float(playerHeight*.5/humanScale),0.f);
        auto capsule=std::make_unique<saida::CollisionShapeNode>();
        capsule->shapeType=saida::CollisionShapeType::Capsule;
        capsule->radius=float(r1::Crowd::kBodyRadius/humanScale);
        capsule->height=float(playerHeight/humanScale);
        capsule->axis=1;
        body->addChild(std::move(capsule));
        person.body=static_cast<saida::RigidBodyNode*>(root->addChild(std::move(body)));
        root->transform().scale=glm::vec3(float(humanScale));
        person.node=tile.node->addChild(std::move(root));
        person.animator=animator;
        return true;
    }
    void readCrowd(Loaded& tile) {
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
    }
    // The hour where the player stands, by the Sun: 12 when it is highest.
    double localSolarHour() {
        double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
        return std::fmod(std::fmod(unixSeconds/3600.+lon/15.,24.)+24.,24.);
    }
    double crowdFactor() {
        double factor=r1::crowdHourFactor(localSolarHour());
        // Rain and falling snow send people indoors, about half of them.
        if(weather.known&&(weather.rain>.5||weather.snowfall>.3))factor*=.5;
        return factor;
    }
    void updateCrowd(float delta) {
        if(crowdKinds.empty())return;
        size_t remaining=kCrowdPeople;
        const double factor=crowdFactor();
        for(auto tile:ring) {
            auto found=loaded.find(tile.key());
            if(found==loaded.end())continue;
            Loaded& l=found->second;
            if(l.walks.links.empty())continue;
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
    // A step of the player's among people, `east` and `north` metres: his
    // feet are the engine's character body, a standing capsule moved and slid
    // against the people's capsules (CharacterBodyNode::moveAndSlide), so he
    // stops at whoever is in the way and slides round them, and whoever walks
    // into him moves him. What his feet touched is then a bump each
    // (World::meetPeople). Buildings, furnishings and vehicles are streamed engine bodies too.
    glm::dvec3 moveFeet(double east,double north,double dt) {
        const glm::dvec2 intended=onward(lon,lat,east,north);
        if(dt<=0)return {intended,alt};
        // The scene's frame is the origin's: x east, y up, z south, near
        // enough within the 350 m it is rebased at.
        const glm::vec3 from(origin.local(ecef(lon,lat,alt+jumpOffset+.06))),to(origin.local(ecef(intended.x,intended.y,alt+jumpOffset+.06)));
        glm::vec3 velocity=(to-from)/float(dt);
        velocity.y=jumpOffset<=0?-2.f:0.f; // Maintain support on steps and furniture.
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
    // Every person the feet touched: the crowd answers the bump if it is one
    // (r1::Crowd::bump), and the player's own body feels it.
    void meetPeople() {
        for(const auto& contact:feet->contacts()) {
            if(!contact.node)continue;
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
    // Running into someone, the player's own body takes the blow -- thrown
    // back off whoever he hit -- and his stride is broken for a moment,
    // longer the harder he hit.
    void staggerPlayer(Loaded& tile,const glm::dvec3& toward,double speed) {
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
    void syncPeople(Loaded& tile,const glm::dvec3& eye,double dt,const glm::dvec3* head) {
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
            if(!live){if(p.node->enabled())p.node->setEnabled(false);p.shown=false;continue;}
            if(!p.node->enabled())p.node->setEnabled(true);
            const r1::Walker& w=walkers[i];
            double y=w.y;
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
            const char* clip=r1::clipOf(w.activity);
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
        }
    }
    size_t crowdWanted() const {
        size_t n=0;
        for(const auto& [key,tile]:loaded)n+=size_t(tile.crowd.wanted());
        return n;
    }
    size_t crowdLive() const {
        size_t n=0;
        for(const auto& [key,tile]:loaded)n+=size_t(tile.crowd.live());
        return n;
    }
    // Tiles are cooked in this process, on the service's threads (gen/).
    std::unique_ptr<r1::WorldService> service;
    std::map<std::string,saida::AssetID> textures;
    size_t residentVertexBudget=0,residentIndexBudget=0;
    // A tile was mounted: it may be the one the player stands on, cooked
    // again with the buildings its first, provisional cook did not have.
    bool checkStanding=false;
    Frame origin; double lon=2.3522,lat=48.8566,alt=0,yaw=0,pitch=-12;
    double pickLon=2.3522,pickLat=48.8566,zoom=1,mapCenterLon=2.3522,mapCenterLat=48.8566;
    MenuTileCache menuTiles;
    bool menu=true,playing=false,pending=false,warming=false,wasMenuKey=false;
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
    bool testFailed=false,testResume=false; double testElapsed=0,resumeWait=0;
    // Set when a tile the spawn is waiting for will not fit in the resident
    // budget. Without it the spawn simply never completes: `surroundingReady`
    // wants all nine tiles, one of them never arrives, and the only report is
    // a data timeout three minutes later blaming the network for a decision
    // this process made in one line. Cleared at the start of every spawn.
    std::string refused;
    // Hovering a ui-hit element makes UIInteractionSystem consume the mouse,
    // which clears the button's previous state; isMouseButtonReleased() is then
    // never true over the menu and RmlUi never receives the Up that makes a
    // click. VerticalSlice's menu answers this by acting on mousedown as well
    // (ui/main_menu.js), and so does this one. The pair is deduplicated: a
    // click arriving just after its own press must not run the action twice --
    // 'zoom-in' would double the zoom twice for one press.
    double clock=0,pressedAt=-1; std::string pressedId;
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
    // Options screen. `forcedMinutes` is "Forcer l'heure à" (minutes after
    // local midnight), kept in cache/options.json between runs; `forcedSent`
    // is the instant last handed to the Sun, so the script is told only when
    // the hour or the time zone under the player changes.
    std::optional<int> forcedMinutes; std::optional<double> forcedSent; bool forcedSaid=false,optionsOpen=false;
    saida::CaptureRequest worldCapture;
    saida::runtime::CaptureViewpoint captureView;
    bool captureQueued=false;
    // --at <unix seconds>: an inspection capture, lit at that instant under a
    // clear sky and without the HUD, so two builds photograph the same place
    // in the same light (tools/gallery.py). Without it a capture keeps the
    // real instant it started at and today's weather.
    std::optional<double> inspectAt;
    // Cloud fraction, rain mm/h, visibility metres (zero keeps clear-air
    // extinction). Inspection weather never replaces live observations.
    std::array<double,3> inspectWeather{0.,0.,0.};
    // --camera-altitude <metres>: the viewpoint's heights are above the sea
    // rather than above the player's feet. A capture laid beside a photograph
    // stands where the photographer stood, whatever this build's relief says
    // the ground there is (tools/gallery.py's reference views).
    std::optional<double> captureAltitude;
    // Photo camera coordinates, independent of gameplay's safe spawn and
    // keepStanding relocation. z is the eye's height above surveyed ground.
    std::optional<std::array<double,3>> captureGeo;
    double captureWait=0,captureReport=0;
    std::string number(double n,int precision=6) {std::ostringstream s;s<<std::fixed<<std::setprecision(precision)<<n;return s.str();}
    // Every write re-lays and re-renders the whole interface -- the map is
    // 1.3 million pixels, some 240 ms -- so a write that changes nothing is
    // not made. The status line used to be rewritten every 16 ms while a
    // destination loaded: that was the freeze after Go.
    std::map<std::string,std::string> written;
    void style(const std::string& id,const std::string& key,const std::string& value) {
        auto& last=written["style:"+id+":"+key];
        if(last==value)return;
        if(auto* e=ui->findElementById(id)){e->SetProperty(key,value);ui->notifyJsMutation();last=value;}
    }
    void text(const std::string& id,const std::string& s) {
        auto& last=written["text:"+id];
        if(last==s)return;
        // setElementText escapes a straight apostrophe to &apos;, which the
        // interface then shows as written ("l&apos;eau"): French is written
        // with the typographic one anyway.
        std::string shown=s;
        for(size_t at;(at=shown.find('\''))!=std::string::npos;)shown.replace(at,1,"’");
        if(ui->setElementText(id,shown))last=s;
    }
    void miniText(const std::string& id,const std::string& s) {
        auto& last=written["mini:text:"+id];
        if(last==s)return;
        std::string shown=s;
        for(size_t at;(at=shown.find('\''))!=std::string::npos;)shown.replace(at,1,"’");
        if(minimapUi->setElementText(id,shown))last=s;
    }
    void layoutMinimap() {
        const float scale=ui->screenSize().x/1440.f;
        const glm::vec2 edge=ui->screenPosition();
        const glm::vec3 position(edge.x+32.f*scale,edge.y+(900.f-28.f-344.f)*scale,0.f);
        auto& transform=minimapUi->transform();
        if(glm::distance(transform.position,position)>.1f)transform.position=position;
        if(std::abs(transform.scale.x-scale)>.001f)transform.scale=glm::vec3(scale);
    }
    void updateMinimap() {
        layoutMinimap();
        std::vector<const r1::MiniMapTile*> tiles;
        tiles.reserve(loaded.size());
        for(const auto& [key,entry]:loaded)tiles.push_back(&entry.served->cooked.minimap);
        const std::string zone=localConditions()?conditions.value("timezone",std::string()):"";
        miniText("mini-time",localClock());
        miniText("mini-place",r1::miniMapPlace(tiles,zoneCountries,countryNames,zone,lon,lat));
        if(minimapDrawnRevision!=minimapRevision||metresBetween(minimapLon,minimapLat,lon,lat)>8.) {
            const std::string roads=r1::miniMapRoadRml(tiles,lon,lat);
            auto& last=written["mini:roads"];
            if(last!=roads&&minimapUi->setElementRml("mini-roads",roads))last=roads;
            minimapLon=lon;minimapLat=lat;minimapDrawnRevision=minimapRevision;
        }
        const double heading=driving?carYaw:sailing?boat.yaw:piloting?plane.yaw:yaw;
        const std::string rotation="rotate("+number(heading,0)+"deg)";
        auto& previous=written["mini:heading"];
        if(previous!=rotation)if(auto* marker=minimapUi->findElementById("mini-player")) {
            marker->SetProperty("transform",rotation);minimapUi->notifyJsMutation();previous=rotation;
        }
    }
    std::string value(const std::string& id) {
        auto* e=dynamic_cast<Rml::ElementFormControl*>(ui->findElementById(id));return e?e->GetValue():"";
    }
    // Compared with what the field shows, not with what was last written: the
    // player may have typed in it since.
    void field(const std::string& id,const std::string& value) {
        auto* e=dynamic_cast<Rml::ElementFormControl*>(ui->findElementById(id));
        if(!e||e->GetValue()==value)return;
        e->SetValue(value);
        ui->notifyJsMutation();
    }
    void field(const std::string& id,double x) {field(id,number(x));}
    void showCityChoices(const std::vector<r1::PlaceChoice>& choices,const std::string& feedback={}) {
        cityChoices=choices;
        style("city-results","display",choices.empty()&&feedback.empty()?"none":"block");
        style("city-feedback","display",feedback.empty()?"none":"block");
        if(!feedback.empty())text("city-feedback",feedback);
        for(size_t i=0;i<5;++i) {
            const std::string id="city-choice-"+std::to_string(i);
            style(id,"display",i<choices.size()?"block":"none");
            if(i<choices.size())text(id,choices[i].label);
        }
    }
    void updateCityLookup() {
        if(menu) {
            std::string query=value("city-query");
            const auto first=query.find_first_not_of(" \t\r\n");
            query=first==std::string::npos?"":query.substr(first,query.find_last_not_of(" \t\r\n")-first+1);
            if(query!=cityInput) {
                cityInput=query;cityChangedAt=clock;
                showCityChoices({},query.size()<2?"":"Recherche…");
            }
        }
        if(cityFuture.valid()&&cityFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            const std::string completed=cityInFlight;
            cityInFlight.clear();
            try {
                auto choices=cityFuture.get();
                if(cityCache.size()>=64)cityCache.clear();
                cityCache[completed]=choices;
                if(cityInput==completed)showCityChoices(choices,choices.empty()?"Aucune ville trouvée.":"");
            } catch(const std::exception& e) {
                saida::Log::info("[World places] search unavailable: ",e.what());
                if(cityInput==completed)showCityChoices({},"Recherche indisponible. Saisissez les coordonnées.");
            }
        }
        if(!menu)return;
        const std::string& query=cityInput;
        if(query.size()<2||query==citySubmitted||cityFuture.valid()||clock-cityChangedAt<.45||
           clock-cityLastRequestAt<1.0)return;
        citySubmitted=query;
        if(const auto cached=cityCache.find(query);cached!=cityCache.end()) {
            showCityChoices(cached->second,cached->second.empty()?"Aucune ville trouvée.":"");
            return;
        }
        cityInFlight=query;cityLastRequestAt=clock;
        cityFuture=std::async(std::launch::async,[query]{return r1::searchPlaceChoices(query);});
    }
    // release=false is what a real click actually delivers: the engine consumes
    // the mouse while a ui-hit element is hovered, so the Up frame never reaches
    // RmlUi. The driver must be able to reproduce that, or it tests a path no
    // player can take.
    void testClick(const std::string& id,bool release=true) {
        auto* element=ui->findElementById(id);
        if(!element)throw std::runtime_error("Missing UI target "+id);
        auto p=element->GetAbsoluteOffset();
        ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Move,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::None);
        ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Down,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::Left);
        if(release)ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Up,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::Left);
    }
    void select(double x,double y) {
        pickLon=wrap(x);pickLat=std::clamp(y,-90.,90.);
        field("latitude",pickLat);field("longitude",pickLon);
        const r1::MapPixel pixel=zoom>1?r1::MapView(mapCenterLon,mapCenterLat,mapZoomLevel()).screen(pickLon,pickLat)
                                        :r1::MapPixel{(pickLon+180)/360*r1::MapView::width,(90-pickLat)/180*r1::MapView::height};
        double px=pixel.x,py=pixel.y;
        style("pin","left",number(px,2)+"px");style("pin","top",number(py,2)+"px");
    }
    void showMap(bool show) {
        menu=show;style("menu","display",show?"block":"none");style("hud","display",show?"none":"block");
        optionsOpen=false;style("options-screen","display","none");
        minimapUi->setEnabled(!show);
        if(performanceUi)performanceUi->setEnabled(performanceDebug&&!show);
        style("resume","display",playing?"inline-block":"none");
        engine.window().setCursorCaptured(!show);
    }
    void resetPerformanceSample() {
        performanceSeconds=0;performanceFrames=0;
        if(performanceDebug)performanceText("debug-fps","FPS : mesure en cours…");
    }
    void performanceText(const std::string& id,const std::string& value) {
        if(!performanceUi)return;
        auto& previous=written["debug:"+id];
        if(previous!=value&&performanceUi->setElementText(id,value))previous=value;
    }
    void placePerformanceCanvas() {
        if(!performanceUi||!performanceDebug||menu)return;
        const glm::vec2 scale=ui->screenSize()/glm::vec2(ui->width(),ui->height());
        const glm::vec2 at=ui->screenPosition()+glm::vec2((1440.f-360.f-32.f)*scale.x,130.f*scale.y);
        performanceUi->transform().position={at.x,at.y,0};
        performanceUi->transform().scale={scale.x,scale.y,1};
    }
    void ensurePerformanceCanvas() {
        if(performanceUi)return;
        // A small independent texture avoids re-rendering the 1440x900 map/HUD
        // twice a second. It is not even allocated until debug is first opened.
        json doc={{"type","WebCanvasNode"},{"name","Performance debug"},{"width",360},{"height",218},
            {"mode",0},{"url",(game/"ui/performance.html").string()},{"hotReload",false},
            {"interactive",false},{"renderOrder",1002},{"enabled",false},
            {"transform",{{"position",{32,130,0}},{"scale",{1,1,1}},{"rotation",{1,0,0,0}}}}};
        auto canvas=saida::SceneSerializer::nodeFromJson(doc.dump(),engine.resources());
        performanceUi=dynamic_cast<saida::WebCanvasNode*>(canvas.get());
        if(!performanceUi||!performanceUi->lastLoadOk())throw std::runtime_error("Performance HUD failed to load");
        engine.sceneTree().world().addChild(std::move(canvas));
    }
    void performanceStatus() {
        performanceText("debug-traffic",std::string("V · Trafic routier : ")+(debugTraffic?"ON":"OFF"));
        performanceText("debug-grass",std::string("Y · Herbe volumétrique : ")+(debugGrass?"ON":"OFF"));
        performanceText("debug-water",std::string("M · Eau (WaterNode) : ")+(debugWater?"ON":"OFF"));
        performanceText("debug-trees",std::string("T · Arbres : ")+(debugTrees?"ON":"OFF"));
    }
    void applyPerformanceTile(Loaded& tile) {
        if(tile.grass)tile.grass->setEnabled(debugGrass);
        for(auto* water:tile.waters)water->setEnabled(debugWater);
        for(auto& plant:tile.vegetation)if(plant.tree)plant.node->setEnabled(debugTrees);
        if(tile.trunks)tile.trunks->setEnabled(debugTrees);
        for(size_t i=0;i<tile.cars.size();++i)
            if(tile.cars[i])tile.cars[i]->setEnabled(trafficSlotLive(tile,i));
    }
    void handlePerformanceKeys(const bool (&keys)[5]) {
        bool pressed[5];
        for(size_t i=0;i<5;++i){pressed[i]=keys[i]&&!performanceKeys[i];performanceKeys[i]=keys[i];}
        // Latch even in menus so typing, holding a key, or resuming cannot toggle.
        if(!playing||menu)return;
        bool changed=pressed[0];
        if(pressed[0]) {
            performanceDebug=!performanceDebug;
            if(performanceDebug)ensurePerformanceCanvas();
            if(performanceUi)performanceUi->setEnabled(performanceDebug);
            if(!performanceDebug)debugTraffic=debugGrass=debugWater=debugTrees=true;
        }
        if(performanceDebug) {
            if(pressed[1]){debugTraffic=!debugTraffic;changed=true;}
            if(pressed[2]){debugGrass=!debugGrass;changed=true;}
            if(pressed[3]){debugWater=!debugWater;changed=true;}
            if(pressed[4]){debugTrees=!debugTrees;changed=true;}
        }
        if(!changed)return;
        for(auto* tiles:{&loaded,&incoming})for(auto& [key,tile]:*tiles)applyPerformanceTile(tile);
        performanceStatus();resetPerformanceSample();
        saida::Log::info("[World debug] enabled=",performanceDebug," traffic=",debugTraffic,
                         " grass=",debugGrass," water=",debugWater," trees=",debugTrees);
    }
    void samplePerformance(double seconds) {
        if(!performanceDebug||!playing||menu){performanceSeconds=0;performanceFrames=0;return;}
        if(seconds<=0||!std::isfinite(seconds))return;
        performanceSeconds+=seconds;++performanceFrames;
        if(performanceSeconds<kPerformanceRefreshSeconds)return;
        // Whole-frame wall time includes GPU waits and pacing. Do not enable the
        // full profiler or copy its scopes/history just to display two numbers.
        performanceText("debug-fps",number(double(performanceFrames)/performanceSeconds,1)+" FPS · "+
                         number(performanceSeconds*1000./double(performanceFrames),2)+" ms/image");
        performanceSeconds=0;performanceFrames=0;
    }
    void pressPerformanceKey(size_t key) {
        bool keys[5]{};handlePerformanceKeys(keys);
        keys[key]=true;handlePerformanceKeys(keys);handlePerformanceKeys(keys);
        keys[key]=false;handlePerformanceKeys(keys);
    }
    void runPerformanceSmoke(double dt) {
        if(captureQueued)return;
        performanceSmokeWait+=dt;
        auto fail=[&](const char* reason){
            saida::Log::error("[World debug E2E] FAIL ",reason);testFailed=true;engine.sceneTree().quit();
        };
        if(performanceSmokePhase==0) {
            if(performanceSmokeWait<2.)return;
            size_t trees=0,grass=0,water=0;
            for(const auto& [key,t]:loaded) {
                grass+=t.grass!=nullptr;water+=t.waters.size();
                for(const auto& plant:t.vegetation)trees+=plant.tree;
            }
            if((!trees||!grass||!trafficLive())&&performanceSmokeWait<15.)return;
            if(!trees||!grass||!trafficLive()){fail("fixture needs grass, trees and road traffic");return;}
            saida::Log::info("[World debug E2E] fixture trees=",trees," grass=",grass," water=",water);
            pressPerformanceKey(1);
            if(!debugTraffic||performanceDebug){fail("V acted outside debug");return;}
            pressPerformanceKey(0); // Held R is delivered twice by the helper.
            if(!performanceDebug){fail("R did not toggle once while held");return;}
            resetPerformanceSample();samplePerformance(.01);samplePerformance(.49);
            if(written["debug:debug-fps"]!="4.0 FPS · 250.00 ms/image") {
                fail("FPS did not count frames over elapsed wall time");return;
            }
            for(size_t key=1;key<5;++key)pressPerformanceKey(key);
            performanceSmokePhase=1;performanceSmokeWait=0;return;
        }
        if(performanceSmokePhase==1) {
            if(performanceSmokeWait<1.)return; // New props stream while switches are OFF.
            for(auto* tiles:{&loaded,&incoming})for(const auto& [key,t]:*tiles) {
                if(t.grass&&t.grass->enabled()){fail("grass stayed enabled");return;}
                for(auto* water:t.waters)if(water->enabled()){fail("water stayed enabled");return;}
                for(const auto& plant:t.vegetation)if(plant.tree&&plant.node->enabled()){fail("tree stayed enabled");return;}
                if(t.trunks&&(t.trunks->enabled()||t.trunks->physicsWorld())){fail("tree collisions stayed enabled");return;}
                for(auto* car:t.cars)if(car&&car->enabled()){fail("traffic car stayed enabled");return;}
                const auto before=t.flow.agents();
                updateTraffic(.05f);
                const auto& after=t.flow.agents();
                for(size_t i=0;i<before.size();++i)
                    if(before[i].lane!=after[i].lane||before[i].s!=after[i].s||before[i].speed!=after[i].speed||
                       before[i].seed!=after[i].seed||before[i].alive!=after[i].alive) {
                        fail("traffic simulation continued while OFF");return;
                    }
            }
            if(!engine.sceneTree().world().grassFields().empty()||!engine.sceneTree().world().waterNodes().empty()) {
                fail("disabled grass/water still published to renderer");return;
            }
            pressPerformanceKey(2);
            if(!debugGrass||debugTraffic||debugWater||debugTrees){fail("Y changed another switch");return;}
            pressPerformanceKey(0);
            if(performanceDebug||!debugGrass||!debugTraffic||!debugWater||!debugTrees||performanceUi->enabled()) {
                fail("leaving debug did not restore normal play");return;
            }
            performanceSmokePhase=2;performanceSmokeWait=0;return;
        }
        if(performanceSmokePhase==2) {
            if(performanceSmokeWait<1.)return;
            if(!trafficLive()||engine.sceneTree().world().grassFields().empty()){fail("features did not resume");return;}
            pressPerformanceKey(0);placePerformanceCanvas();
            performanceSmokePhase=3;performanceSmokeWait=0;return;
        }
        if(performanceSmokeWait<1.)return;
        if(!performanceUi->lastLoadOk()||written["debug:debug-fps"].find("ms/image")==std::string::npos) {
            fail("FPS HUD did not render its sample");return;
        }
        saida::Log::info("[World debug E2E] PASS held keys, independent toggles, streamed nodes, physics removal, traffic pause/resume, FPS averaging and HUD");
        if(const char* png=std::getenv("R1WORLD_DEBUG_SHOT")) {
            if(!captureQueued) {
                saida::CaptureRequest shot;shot.pngPath=png;shot.frame=15;
                captureQueued=true;engine.captureFrameThenExit(shot);
            }
        } else engine.sceneTree().quit();
    }
    int mapZoomLevel() const {return std::clamp(int(std::lround(std::log2(zoom)))+2,3,19);}
    void renderMapTiles() {
        std::ostringstream markup;
        if(zoom>1) {
            const auto tiles=r1::MapView(mapCenterLon,mapCenterLat,mapZoomLevel()).visible();
            menuTiles.setVisible(tiles);
            for(const auto& tile:tiles) {
                if(!menuTiles.ready(tile))continue;
                markup<<"<img class='map-tile' src='../cache/map-tiles/osm/"<<tile.z<<"/"<<tile.x<<"/"<<tile.y
                      <<".png' style='left:"<<number(tile.left,1)<<"px;top:"<<number(tile.top,1)<<"px;'/>";
            }
            const size_t missing=menuTiles.missing();
            style("map-detail-status","display",missing?"block":"none");
            if(missing)text("map-detail-status",menuTiles.unavailable()?"Carte détaillée indisponible hors ligne":"Chargement de la carte détaillée…");
        } else {
            menuTiles.setVisible({});
            style("map-detail-status","display","none");
        }
        auto& previous=written["map:tiles"];
        const std::string html=markup.str();
        if(previous!=html&&ui->setElementRml("map-tiles",html))previous=html;
    }
    void zoomMap(double z) {
        zoom=std::clamp(z,1.,131072.);
        mapCenterLon=pickLon;mapCenterLat=pickLat;
        style("earth","display",zoom>1?"none":"block");
        renderMapTiles();
        select(pickLon,pickLat);
    }
    double streamPriority(Tile t,double x,double y,double heading) const {
        const r1::P2 middle=t.gen().center();
        const double east=wrap(middle.x-x)*kMetresPerDegree*std::cos(y*rad);
        const double north=(middle.y-y)*kMetresPerDegree;
        const double ahead=east*std::sin(heading*rad)+north*std::cos(heading*rad);
        return std::hypot(east,north)-.8*std::max(0.,ahead);
    }
    bool streamMotion(double& heading,double& speed) const {
        const double velocity=driving?carSpeed:sailing?boat.speed:piloting?plane.speed:0.;
        if(!playing||pending||warming||std::abs(velocity)<kFastDetail)return false;
        heading=wrap((driving?carYaw:sailing?boat.yaw:plane.yaw)+(velocity<0?180.:0.));
        speed=std::abs(velocity);
        return true;
    }
    std::vector<Tile> orderedNearby(double x,double y) const {
        auto tiles=nearby(x,y);
        double heading=0,speed=0;
        if(streamMotion(heading,speed))
            std::stable_sort(tiles.begin()+1,tiles.end(),[&](Tile a,Tile b){
                return streamPriority(a,x,y,heading)<streamPriority(b,x,y,heading);
            });
        return tiles;
    }
    void request(double x,double y) {
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
        for(size_t i=1;i<std::min(size_t(3),local.size());++i)append(local[i]);
        for(size_t i=0;i<std::min(size_t(3),forecast.size());++i)append(forecast[i]);
        for(size_t i=3;i<local.size();++i)append(local[i]);
        for(size_t i=3;i<forecast.size();++i)append(forecast[i]);
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
        service->want(std::move(genTiles),std::move(genGroups));requested=signature;
    }
    int goCount=0;
    void go() {
        ++goCount;
        try {
            size_t a,b;std::string x=value("longitude"),y=value("latitude");
            double lo=std::stod(x,&a),la=std::stod(y,&b);
            if(a!=x.size()||b!=y.size()||!std::isfinite(lo)||!std::isfinite(la)||la< -90||la>90||lo< -180||lo>180)throw std::runtime_error("coordinate");
            select(lo,la);pending=true;request(pickLon,pickLat);
            goStarted=std::chrono::steady_clock::now();poll=1;
            arrivalFrames=arrivalHitches=0;arrivalWorst=0;arrivalSaid=false;
            text("status",kPreparing);
            saida::Log::info("[World] destination ",lo,", ",la);
        } catch(...) {text("status","Coordonnées invalides : latitude −90 à 90, longitude −180 à 180 (point décimal).");}
    }
    // The Sun is a function of where the player stands, and in this game that
    // moves: a fixed anchor would light Sydney at Paris's hour, on the wrong
    // side of the sky, in the wrong season. `scripts/sun_cycle.js` owns the
    // model -- porting it a third time into C++ is exactly what the parity test
    // exists to prevent -- so the game only tells it where the observer is, and
    // the script keeps the beam, the ambient, the horizon and the two exposures
    // consistent with that place. Called wherever the floating origin is
    // rebased, which is every spawn and every 350 m walked.
    bool findSun() {
        if(!sunScript) {
            auto* node=engine.sceneTree().firstInGroup("sun");
            if(!node)return false;
            for(auto& b:node->behaviours())
                if(auto* script=dynamic_cast<saida::ScriptBehaviour*>(b.get()))sunScript=script;
        }
        return sunScript!=nullptr;
    }
    bool tellSun() {
        if(!findSun())return false;
        nlohmann::json result;
        if(!worldCapture.pngPath.empty()) {
            if(sunScript->callExport("setInspectionMode",inspectAt?json::array({true,*inspectAt}):json::array({true}),result)
                    !=saida::ScriptCallStatus::Succeeded)return false;
        }
        if(sunScript->callExport("setObserver",json::array({lon,lat,alt}),result)
               !=saida::ScriptCallStatus::Succeeded)return false;
        if(!result.is_boolean()||!result.get<bool>())return false;
        return applyWeather();
    }
    // The air thins with altitude over the Earth, not over the scene's
    // tangent plane: the engine's fog is given the sphere that fits the
    // ground under the player (centre of curvature, Gaussian radius), so a
    // ray to the horizon climbs out of the haze as it does.
    void tellPlanet() {
        const double p=lat*rad,l=lon*rad,w=1.0-r1::kE2*std::sin(p)*std::sin(p);
        const double radius=std::sqrt(r1::kA/std::sqrt(w)*r1::kA*(1.0-r1::kE2)/(w*std::sqrt(w)));
        const glm::dvec3 normal(std::cos(p)*std::cos(l),std::cos(p)*std::sin(l),std::sin(p));
        auto& settings=engine.sceneTree().world().settings();
        settings.fogPlanetRadius=float(radius);
        settings.fogPlanetCentre=glm::vec3(origin.local(ecef(lon,lat,0.)-normal*radius));
    }
    void moveSun() {
        tellPlanet();
        // A failure here is a scene that keeps lighting the wrong hemisphere
        // rather than a crash, so it has to be said out loud once.
        if(tellSun()||sunReported)return;
        sunReported=true;
        saida::Log::error("[World] the sun cycle did not accept the observer or weather; "
                          "the light stays on the scene's opening instant");
    }
    Loaded* tile(double x,double y) {
        auto i=loaded.find(tileAt(x,y).key());return i==loaded.end()?nullptr:&i->second;
    }
    bool onSeaIce(double x,double y) {
        auto* t=tile(x,y);return t&&t->seaIce;
    }
    // ── headings ────────────────────────────────────────────────────────────
    //
    // A heading is an angle in the origin's frame, the frame the camera and
    // every model are drawn in. A step along it is turned into the local east
    // and north where the step starts. Far from a pole the two are the same
    // to a hundredth of a degree; near one they are not, and a heading kept
    // against the local north walks a circle around the pole and turns back
    // at it. This keeps a straight line straight, over the pole included.
    glm::dvec2 onward(double x,double y,double east,double north) const {
        const glm::dvec3 d=origin.basis[0]*east-origin.basis[2]*north;
        const double l=x*rad,p=y*rad;
        const double e=glm::dot(d,glm::dvec3(-std::sin(l),std::cos(l),0.));
        const double n=glm::dot(d,glm::dvec3(-std::sin(p)*std::cos(l),-std::sin(p)*std::sin(l),std::cos(p)));
        const double length=std::hypot(east,north),flat=std::hypot(e,n);
        if(flat<1e-12)return {x,y};
        return advance(x,y,e/flat*length,n/flat*length);
    }
    // The origin follows the player every 350 m. What the player owns is
    // written in its frame, so every heading turns with it: at the pole the
    // new frame can face the other way from the old one.
    void rebaseOrigin() {
        const Frame next(lon,lat,alt);
        auto turn=[&](double heading){
            const glm::dvec3 d=origin.basis[0]*std::sin(heading*rad)-origin.basis[2]*std::cos(heading*rad);
            return wrap(std::atan2(glm::dot(d,next.basis[0]),-glm::dot(d,next.basis[2]))/rad);
        };
        const double delta=wrap(turn(yaw)-yaw);
        yaw=turn(yaw);carYaw=turn(carYaw);swimHeading=turn(swimHeading);
        boat.yaw=turn(boat.yaw);plane.yaw=turn(plane.yaw);
        for(auto& entry:parked)entry.yaw=turn(entry.yaw);
        if(std::abs(delta)>1e-9) {
            const auto spin=glm::angleAxis(float(-delta*rad),glm::vec3(0,1,0));
            player->transform().rotation=spin*player->transform().rotation;
        }
        origin=next;placeTiles();moveSun();
    }
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
    static void unpack(Loaded& tile) {
        if(auto rooms=tile.data.find("interiors");rooms!=tile.data.end())for(const auto& entry:*rooms) {
            LiveInterior room;room.plan=r1::InteriorPlan::read(entry);
            for(auto p:room.plan.ring){room.low.x=std::min(room.low.x,p.x);room.low.y=std::min(room.low.y,p.y);
                room.high.x=std::max(room.high.x,p.x);room.high.y=std::max(room.high.y,p.y);}
            if(room.plan.footprint<tile.footprints.size())tile.footprints[room.plan.footprint].interior=int(tile.interiors.size());
            tile.interiors.push_back(std::move(room));
        }
        if(auto streaming=tile.data.find("interiorStreaming");streaming!=tile.data.end()&&streaming->contains("unavailable")) {
            std::map<std::string,size_t> reasons;for(const auto& p:streaming->at("unavailable"))++reasons[p.at("reason").get<std::string>()];
            for(const auto& [reason,count]:reasons)saida::Log::info("[World interiors] ",count," buildings unavailable: ",reason);
        }
        if(auto retail=tile.data.find("retail");retail!=tile.data.end())for(const auto& area:retail->at("parking")) {
            StoreParking lot;lot.name=area.at("storeName");
            for(const auto& path:area.at("rings")){r1::Ring r;for(const auto& p:path)r.push_back({p[0],p[1]});lot.rings.push_back(std::move(r));}
            tile.parking.push_back(std::move(lot));
        }
        const auto& bounds=tile.data.at("bounds");
        tile.west=bounds.at("west");tile.east=bounds.at("east");
        tile.south=bounds.at("south");tile.north=bounds.at("north");
        const auto& grid=tile.data.at("elevations");
        tile.gridSize=int(grid.size());
        tile.elevation.resize(size_t(tile.gridSize)*size_t(tile.gridSize));
        tile.groundGrid = r1::ElevationGrid{{tile.south,tile.west,tile.north,tile.east},tile.gridSize,{}};
        for(int row=0;row<tile.gridSize;++row) {
            const auto& line=grid[size_t(row)];
            for(int col=0;col<tile.gridSize;++col) {
                tile.elevation[size_t(row)*size_t(tile.gridSize)+size_t(col)]=float(line[size_t(col)]);
                tile.groundGrid.values.push_back(line[size_t(col)].get<double>());
            }
        }
        if(auto edges=tile.data.find("groundEdges");edges!=tile.data.end()) {
            for(const auto& p:edges->at("south"))tile.groundGrid.southEdge.push_back({p[0],p[1]});
            for(const auto& p:edges->at("north"))tile.groundGrid.northEdge.push_back({p[0],p[1]});
        }
        auto water=tile.data.find("water");
        if(water!=tile.data.end()&&water->is_array()&&!water->empty()) {
            tile.waterRows=int(water->size());
            const std::string first=(*water)[0].get<std::string>();
            tile.waterCols=int(first.size());
            tile.water.assign(size_t(tile.waterRows)*size_t(tile.waterCols),0);
            for(int row=0;row<tile.waterRows;++row) {
                const std::string line=(*water)[size_t(row)].get<std::string>();
                for(int col=0;col<tile.waterCols&&col<int(line.size());++col)
                    tile.water[size_t(row)*size_t(tile.waterCols)+size_t(col)]=uint8_t(std::max(0,line[size_t(col)]-'0'));
            }
        }
        tile.ocean=tile.data.value("surface",std::string())=="ocean";
        tile.seaIce=tile.data.value("surface",std::string())=="sea-ice";
        auto decks=tile.data.find("decks");
        if(decks!=tile.data.end())
            for(const auto& entry:*decks) {
                Deck deck;deck.y=entry.at("y");
                for(const auto& point:entry.at("points")) {
                    glm::dvec2 q{double(point[0]),double(point[1])};
                    deck.points.push_back(q);deck.low=glm::min(deck.low,q);deck.high=glm::max(deck.high,q);
                }
                tile.deckLow=glm::min(tile.deckLow,deck.low);tile.deckHigh=glm::max(tile.deckHigh,deck.high);
                tile.decks.push_back(std::move(deck));
            }
        if(auto dry=tile.data.find("dryStreets");dry!=tile.data.end())
            for(const auto& ring:*dry) {
                Deck piece;
                for(const auto& point:ring) {
                    glm::dvec2 q{double(point[0]),double(point[1])};
                    piece.points.push_back(q);piece.low=glm::min(piece.low,q);piece.high=glm::max(piece.high,q);
                }
                if(piece.points.size()<3)continue;
                tile.dryLow=glm::min(tile.dryLow,piece.low);tile.dryHigh=glm::max(tile.dryHigh,piece.high);
                tile.dryStreets.push_back(std::move(piece));
            }
        auto raised=tile.data.find("raised");
        if(raised!=tile.data.end())
            for(const auto& entry:*raised) {
                RaisedPiece r;
                r.a={double(entry.at("a")[0]),double(entry.at("a")[2])};r.ya=entry.at("a")[1];
                r.b={double(entry.at("b")[0]),double(entry.at("b")[2])};r.yb=entry.at("b")[1];
                for(int s=0;s<2;++s){r.half[s]=entry.at("half")[s];r.rail[s]=entry.at("rail")[s];}
                r.solid=entry.at("solid");
                // An embankment's slopes reach out 2 in 3 below its road.
                const double reach=std::max(r.half[0],r.half[1])+(r.solid?1.5*20.:1.2);
                r.low=glm::min(r.a,r.b)-glm::dvec2(reach);r.high=glm::max(r.a,r.b)+glm::dvec2(reach);
                tile.raisedLow=glm::min(tile.raisedLow,r.low);tile.raisedHigh=glm::max(tile.raisedHigh,r.high);
                tile.raised.push_back(r);
            }
        auto boats=tile.data.find("boats");
        if(boats!=tile.data.end())
            for(const auto& entry:*boats) {
                Mooring m;
                m.name=entry.value("name",std::string());m.kind=entry.value("kind",std::string("motor"));
                m.model=entry.value("model",std::string());
                m.local={entry.value("x",0.),entry.value("y",0.),entry.value("z",0.)};
                m.heading=entry.value("heading",0.);m.length=entry.value("length",8.);
                m.beam=entry.value("beam",3.);m.top=entry.value("top",10.);
                m.accel=entry.value("accel",2.);m.turn=entry.value("turn",30.);
                tile.boats.push_back(std::move(m));
            }
        auto aircraft=tile.data.find("aircraft");
        if(aircraft!=tile.data.end())
            for(const auto& entry:*aircraft) {
                AircraftSpot spot;
                spot.name=entry.value("name",std::string());spot.type=entry.value("type",std::string());
                spot.local={entry.value("x",0.),entry.value("y",0.),entry.value("z",0.)};
                spot.heading=entry.value("heading",0.);
                tile.aircraft.push_back(std::move(spot));
            }
    }
    bool inside(const Loaded& tile,double x,double y,double margin=0.) const {
        return x>=tile.west-margin&&x<=tile.east+margin
             &&y>=tile.south-margin&&y<=tile.north+margin;
    }
    // Is this coordinate on a pier deck? Decks are drawn over the water and
    // walked on, so they answer before the water and before the terrain.
    bool onDeck(const Loaded& t,double x,double y,double* level=nullptr) const {
        if(t.decks.empty())return false;
        const glm::dvec3 p=t.frame.local(ecef(x,y,0.));
        const glm::dvec2 q(p.x,p.z);
        if(q.x<t.deckLow.x||q.x>t.deckHigh.x||q.y<t.deckLow.y||q.y>t.deckHigh.y)return false;
        for(const Deck& d:t.decks) {
            if(q.x<d.low.x||q.x>d.high.x||q.y<d.low.y||q.y>d.high.y)continue;
            bool in=false;const auto& poly=d.points;
            for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++)
                if((poly[i].y>q.y)!=(poly[j].y>q.y)&&q.x<(poly[j].x-poly[i].x)*(q.y-poly[i].y)/(poly[j].y-poly[i].y)+poly[i].x)in=!in;
            if(in){if(level)*level=d.y;return true;}
        }
        return false;
    }
    // Is this coordinate on a street that crosses a water cell? A cell is
    // water or not as a whole; a street through it is dry, always.
    bool onDryStreet(const Loaded& t,double x,double y) const {
        if(t.dryStreets.empty())return false;
        const glm::dvec3 p=t.frame.local(ecef(x,y,0.));
        const glm::dvec2 q(p.x,p.z);
        if(q.x<t.dryLow.x||q.x>t.dryHigh.x||q.y<t.dryLow.y||q.y>t.dryHigh.y)return false;
        bool in=false;
        for(const Deck& d:t.dryStreets) {
            if(q.x<d.low.x||q.x>d.high.x||q.y<d.low.y||q.y>d.high.y)continue;
            const auto& poly=d.points;
            for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++)
                if((poly[i].y>q.y)!=(poly[j].y>q.y)&&q.x<(poly[j].x-poly[i].x)*(q.y-poly[i].y)/(poly[j].y-poly[i].y)+poly[i].x)in=!in;
        }
        return in;
    }
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
    double height(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        auto* t=tile(x,y);if(!t)throw std::runtime_error("Missing terrain");
        const double ground=terrainHeight(*t,x,y);
        const bool any=std::isnan(standing);
        double best=ground;
        const auto worldPoint=ecef(x,y,ground);
        for(const auto& [key,other]:loaded) {
          const auto local=other.frame.local(worldPoint);const r1::P2 q{local.x,local.z};
          for(const auto& room:other.interiors) {
            const auto& p=room.plan;
            const double reach=std::max(4.5,p.approachRun()+.5);
            if(q.x<room.low.x-reach||q.x>room.high.x+reach||q.y<room.low.y-reach||q.y>room.high.y+reach)continue;
            const auto door=p.local(q);
            double level=0;
            if(room.contains(q))level=p.floor;
            else if(std::abs(door.x)<p.width/2+.4&&door.y>=-p.approachRun()&&door.y<=0)
                level=p.floor+(p.approach-p.floor)*(-door.y/p.approachRun());
            else continue;
            // Store floors replace the terrain inside, even on a slope.
            return level-other.frame.local(ecef(x,y,0.)).y;
          }
        }
        // A piece belongs to the tile its middle is in: near a tile's edge,
        // the deck under a foot may be its neighbour's.
        for(const auto& [key,other]:loaded)
            raisedAt(other,x,y,[&](double level,bool,bool parapet){
                if(parapet||level<=best)return;
                if(any||level<=standing+kStepUp)best=level;
            });
        return best;
    }
    double terrainHeight(const Loaded& tile,double x,double y) const {
        double deck=0;if(onDeck(tile,x,y,&deck))return deck;
        return gridHeight(tile,x,y);
    }
    // The tile's own ground grid, decks aside; clamped to the tile.
    static double gridHeight(const Loaded& tile,double x,double y) {
        const Loaded* t=&tile;
        const int n=t->gridSize;
        double u=std::clamp((wrap(x)-t->west)/(t->east-t->west),0.,1.)*(n-1);
        double v=std::clamp((y-t->south)/(t->north-t->south),0.,1.)*(n-1);
        if((v<1&&!t->groundGrid.southEdge.empty())||(v>n-2&&!t->groundGrid.northEdge.empty()))
            return r1::terrainElevation(wrap(x),y,t->groundGrid);
        int ix=std::min(n-2,int(u)),iy=std::min(n-2,int(v));u-=ix;v-=iy;
        const float* g=t->elevation.data();
        const size_t low=size_t(iy)*size_t(n),high=low+size_t(n);
        // The two triangles the ground is drawn and collided with (gen/terrain
        // buildTerrain: south-west to north-east diagonal), never a bilinear
        // patch: on an embankment the two disagree by decimetres, and a car
        // riding the patch met the drawn ground as an obstacle nobody could see.
        const double sw=g[low+size_t(ix)],se=g[low+size_t(ix)+1],nw=g[high+size_t(ix)],ne=g[high+size_t(ix)+1];
        return u>=v?sw*(1-u)+se*(u-v)+ne*v:sw*(1-v)+ne*u+nw*(v-u);
    }
    // The tile's water grid and fully oceanic tiles must answer the same
    // question for walkers, cars and boats. Piers remain dry walkable decks.
    // `standing`: whoever is at that altitude on a bridge over the water is dry.
    bool onWater(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        const auto* t=tile(x,y);
        if(!t||onDeck(*t,x,y)||!(t->ocean||waterCode(*t,x,y)!=0)||onDryStreet(*t,x,y))return false;
        // A bridge's parapet stands between whoever is on it and the river.
        if(!std::isnan(standing)&&(height(x,y,standing)>terrainHeight(*t,x,y)+.5))return false;
        return true;
    }
    int waterCode(const Loaded& t,double x,double y) const {
        if(t.water.empty())return 0;
        const int row=std::clamp(int((y-t.south)/(t.north-t.south)*t.waterRows),0,t.waterRows-1);
        const int col=std::clamp(int((x-t.west)/(t.east-t.west)*t.waterCols),0,t.waterCols-1);
        return t.water[size_t(row)*size_t(t.waterCols)+size_t(col)];
    }
    // Where a boat may be: an ocean tile, or a water cell of a land tile that
    // is not under a pier. The same cells the terrain is drawn from, so a boat
    // stops at the shore the eye sees.
    bool navigable(double x,double y) {
        return onWater(x,y);
    }
    // The surface a boat floats on: the sea's 0 m, or a lake's own level.
    double waterLevel(double x,double y) {
        auto* t=tile(x,y);
        if(!t||t->ocean)return 0.;
        return waterCode(*t,x,y)==1?height(x,y):0.;
    }
    // Collider authoring only: detection and response belong to Saida/Jolt.
    static saida::StaticBodyNode* boxCollider(saida::Node& parent,const std::string& name,
                                            glm::vec3 size,glm::vec3 center) {
        auto body=std::make_unique<saida::StaticBodyNode>();body->setName(name);
        auto shape=std::make_unique<saida::CollisionShapeNode>();
        shape->shapeType=saida::CollisionShapeType::Box;shape->halfExtents=size*.5f;shape->offset=center;
        body->addChild(std::move(shape));return static_cast<saida::StaticBodyNode*>(parent.addChild(std::move(body)));
    }
    static void meshCollider(saida::Node& parent,std::unique_ptr<saida::MeshNode> mesh) {
        auto body=std::make_unique<saida::StaticBodyNode>();body->setName(mesh->name()+" collider");
        auto shape=std::make_unique<saida::CollisionShapeNode>();shape->shapeType=saida::CollisionShapeType::Mesh;
        body->addChild(std::move(shape));body->addChild(std::move(mesh));parent.addChild(std::move(body));
    }
    void vehicleCollider(saida::Node& node,const VehicleModel& spec) {
        if(node.findByPath("Vehicle collider"))return;
        boxCollider(node,"Vehicle collider",{float(spec.width),float(spec.height),float(spec.length)},
                    {0,float(spec.height*.5),0});
    }
    saida::QueryFilter obstacleFilter() const {
        saida::QueryFilter filter;filter.ignoreInner=feet->innerBodyId();
        if(car)if(auto* body=dynamic_cast<saida::CollisionObjectNode*>(car->findByPath("Vehicle collider")))filter.ignore=body->bodyId();
        return filter;
    }
    bool solidAt(double x,double y,double level,float radius=.32f) const {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
        const glm::vec3 at(origin.local(ecef(x,y,level)));
        return !physics->overlapSphere(at,radius,obstacleFilter()).empty();
    }
    // What `blocked` met there, by its node and that node's parent, for the log.
    std::string obstacleName(double x,double y,double ground) const {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return "no physics";
        for(double above:{.6,1.3})for(auto id:physics->overlapSphere(glm::vec3(origin.local(ecef(x,y,ground+above))),.32f,obstacleFilter()))
            if(auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(id)))
                return node->name()+(node->parent()?" in "+node->parent()->name():"");
        return "water or a roof";
    }
    // Spawn/exit/parking occupancy uses the engine's scene queries. Movement
    // itself uses the character solver, without a second footprint test.
    bool blocked(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        if(onWater(x,y,standing))return true;
        const double ground=std::isnan(standing)?height(x,y):standing;
        if(solidAt(x,y,ground+.6)||solidAt(x,y,ground+1.3))return true;
        // A mesh encloses air: an arrival below an opaque roof is not an outdoor
        // spawn, even when its capsule would initially touch no triangles.
        if(std::isnan(standing)||parkingCar) {
            auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
            const glm::vec3 above(origin.local(ecef(x,y,ground+500.)));
            const auto hit=physics->raycast(above,{0,-1,0},500.f,obstacleFilter());
            if(hit.hit&&hit.point.y>origin.local(ecef(x,y,ground)).y+2.)return true;
        }
        return false;
    }
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
    double groundAt(double x,double y,double fallback) {
        return tile(x,y)?height(x,y,fallback):fallback;
    }
    // What the wheels roll on: the ground, or the street laid on it -- the
    // carriageway 6 cm up, a path or a pavement 21 cm (gen/streets). Asked of
    // the collision the engine holds, since that is what `blocked` meets: a car
    // riding the bare terrain under a path met the path as an obstacle.
    double drivenGround(double x,double y,double fallback) {
        const double ground=groundAt(x,y,fallback);
        auto* physics=engine.sceneTree().world().physics();if(!physics)return ground;
        const glm::vec3 start(origin.local(ecef(x,y,ground+kCarKerb)));
        const auto hit=physics->raycast(start,{0,-1,0},float(kCarKerb+.1),obstacleFilter());
        if(!hit.hit||hit.normal.y<.7f)return ground;
        return std::max(ground,ground+kCarKerb-double(hit.distance));
    }
    // Yaw, then the slope the wheels are actually standing on. Measured off the
    // terrain grid over the car's own wheelbase and track rather than inferred
    // from a normal: it is the same surface the collision reads (§3 I5).
    glm::quat carRotation() {
        const auto& spec=vehicleSpec(*car);
        const double halfLength=spec.wheelbase*.5,halfWidth=spec.width*.43;
        auto sample=[&](double east,double north){
            auto q=onward(carLon,carLat,east,north);return groundAt(q.x,q.y,carAlt);
        };
        double s=sin(carYaw*rad),c=cos(carYaw*rad);
        double ahead=sample(s*halfLength,c*halfLength),behind=sample(-s*halfLength,-c*halfLength);
        double right=sample(c*halfWidth,-s*halfWidth),left=sample(-c*halfWidth,s*halfWidth);
        auto yawQ=glm::angleAxis(float(-carYaw*rad),glm::vec3(0,1,0));
        auto pitchQ=glm::angleAxis(float(std::atan2(ahead-behind,2*halfLength)),glm::vec3(1,0,0));
        auto rollQ=glm::angleAxis(float(std::atan2(right-left,2*halfWidth)),glm::vec3(0,0,1));
        return yawQ*pitchQ*rollQ;
    }
    // The four wheels of whatever car the player is now in. Re-run on every
    // take-over, because the wheels that turn must belong to the car he is
    // actually sitting in.
    //
    // Outermost match only, and it is not a detail: the importer wraps each
    // named node around a mesh node that inherits the name, so descending into
    // a wheel finds the same wheel again and every turn would be applied to it
    // twice. Eight wheels on a saloon is what said so.
    void collectWheels(saida::Node& root) {
        frontWheels.clear();rearWheels.clear();
        std::function<void(saida::Node&)> collect=[&](saida::Node& n){
            if(n.name().rfind("wheel-",0)==0) {
                (n.name().find("front")!=std::string::npos?frontWheels:rearWheels).push_back(&n);
                return;
            }
            for(auto& child:n.children())collect(*child);
        };
        collect(root);
        const size_t expected=root.findByPath("Far")?4:2;
        if(frontWheels.size()!=expected||rearWheels.size()!=expected)
            saida::Log::warn("[World car] this car has ",frontWheels.size()," front and ",
                             rearWheels.size()," rear wheels named wheel-*; they will not turn");
    }
    void placeCar() {
        // Abandoned cars are written in the origin's frame like everything else
        // the player owns, so they have to be rewritten when the origin moves.
        // There are at most six of them.
        for(auto& entry:parked) {
            entry.node->transform().position=glm::vec3(origin.local(ecef(entry.lon,entry.lat,entry.alt+.06)));
            entry.node->transform().rotation=glm::angleAxis(float(-entry.yaw*rad),glm::vec3(0,1,0));
        }
        if(!carParked||!car)return;
        car->transform().position=glm::vec3(origin.local(ecef(carLon,carLat,carAlt+.06)));
        car->transform().rotation=carSinking
            ?glm::angleAxis(float(-carYaw*rad),glm::vec3(0,1,0)):carRotation();
        // Wheels turn because a car whose wheels do not turn is a car sliding
        // on ice, and this one is visible from three metres behind it.
        auto spin=glm::angleAxis(float(-wheelSpin),glm::vec3(1,0,0));
        auto lock=glm::angleAxis(float(-steerShown*kCarSteerAngle*rad),glm::vec3(0,1,0));
        for(auto* w:rearWheels)w->transform().rotation=spin;
        for(auto* w:frontWheels)w->transform().rotation=lock*spin;
    }
    // Somewhere within `radius` metres of (x0,y0) that is on loaded terrain,
    // out of the water and out of a building. The golden-angle spiral is the
    // spawn search's, for the same reason it is used there: it covers a disc
    // evenly without preferring a compass direction.
    // `standing`: the level the spot must be on -- beside a car on a bridge
    // is on the bridge, not on the ground six metres below its parapet.
    bool freeSpot(double x0,double y0,double radius,double& outLon,double& outLat,
                  double standing=std::numeric_limits<double>::quiet_NaN()) {
        auto free=[&](double x,double y){
            if(!tile(x,y)||blocked(x,y,standing))return false;
            return std::isnan(standing)||std::abs(height(x,y,standing)-standing)<kStepUp;
        };
        if(free(x0,y0)){outLon=x0;outLat=y0;return true;}
        for(int i=1;i<=120;++i) {
            double a=i*2.39996323,d=radius*std::sqrt(double(i)/120.);
            auto q=advance(x0,y0,d*cos(a),d*sin(a));
            if(free(q.x,q.y)){outLon=q.x;outLat=q.y;return true;}
        }
        return false;
    }
    // How far one can go from (x, y) at `level` along a heading before a
    // wall, a parapet, water or the edge of the loaded world: the level is
    // followed step by step, up a ramp and onto a deck.
    double clearAhead(double x,double y,double level,double heading,double step,double limit) {
        double clear=0;
        for(double d=step;d<=limit;d+=step) {
            auto q=onward(x,y,sin(heading*rad)*d,cos(heading*rad)*d);
            if(!tile(q.x,q.y)||blocked(q.x,q.y,level))break;
            level=height(q.x,q.y,level);
            clear=d;
        }
        return clear;
    }
    // A building owned by a neighbouring tile can stream in over the spot the
    // car was parked on before that tile mounted: move the car out, and say so.
    void reparkIfCovered(const std::string& key) {
        if(!carParked||driving||!car)return;
        parkingCar=true;
        const bool covered=blocked(carLon,carLat,carAlt);
        double x=carLon,y=carLat;
        const bool found=covered&&freeSpot(carLon,carLat,20.,x,y,carAlt);
        parkingCar=false;
        if(!covered)return;
        if(!found){saida::Log::warn("[World car] ",key," covered the parked car and no free spot is within 20 m");return;}
        saida::Log::info("[World car] ",key," streamed in over the parked car; moved to ",x,", ",y);
        carLon=x;carLat=y;carAlt=groundAt(x,y,carAlt);placeCar();
    }
    // Park the car beside a dry spawn. A swimmer cannot arrive with a car.
    void parkCar() {
        carParked=false;carSinking=false;carSinkDepth=0;
        carSpeed=0;wheelSpin=0;steerShown=0;
        // Half a planet away from where they were left, so they are not kept.
        clearAbandonedCars();
        if(!car)return;
        car->setEnabled(false);
        if(swimming)return;
        // No road has ever reached the pack ice: nobody drives to the Pole.
        if(onSeaIce(lon,lat)) {
            saida::Log::info("[World car] on the sea ice: the player arrives on foot, with no car");
            return;
        }
        // Three metres to his right, which is where a car that dropped him off
        // would be, and a spiral out from there when that spot is a wall.
        auto beside=onward(lon,lat,cos(yaw*rad)*3.,-sin(yaw*rad)*3.);
        double x,y;
        // A car's spot is judged as a car: a shop a walker may enter is a
        // wall to it, or the car would be parked inside the store.
        parkingCar=true;
        const bool found=freeSpot(beside.x,beside.y,14.,x,y,alt);
        parkingCar=false;
        if(!found) {
            // Loud, and said to the player too. A car that silently is not
            // there looks exactly like a car that failed to load, which is the
            // shape of failure this file has already been bitten by three
            // times (see `refused`, and the spawn refusal below).
            saida::Log::warn("[World car] no free kerb within 14 m of ",lon,", ",lat,
                             "; the player spawns on foot with no car");
            text("stream-status","Aucune place pour la voiture ici — départ à pied.");
            return;
        }
        carLon=x;carLat=y;carAlt=groundAt(x,y,alt);carYaw=yaw;carParked=true;
        car->setEnabled(true);placeCar();
        saida::Log::info("[World car] parked at ",carLon,", ",carLat);
    }
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
    void abandonCurrentCar() {
        if(!car||!carParked)return;
        parked.push_back({car,carLon,carLat,carAlt,carYaw});
        while(parked.size()>kAbandonedCars) {
            // The scene's own car is in this list like any other once it has
            // been left; nothing here is special-cased, so nothing here can
            // disagree with itself about which car is the player's.
            parked.front().node->queueFree();
            parked.erase(parked.begin());
        }
        car=nullptr;carParked=false;carSinking=false;carSinkDepth=0;
    }
    void clearAbandonedCars() {
        for(auto& entry:parked)entry.node->queueFree();
        parked.clear();
    }
    // Where a point in this tile's frame is on the planet, to within the metre
    // or two that separates it from the player. Going through `advance` rather
    // than inverting the frame keeps this file to one direction of geodesy:
    // the offset is read in the tile's tangent plane, where east is +x and
    // north is -z, and walked from the player's own coordinate.
    glm::dvec2 geodeticOf(const Loaded& tile,const glm::vec3& local) const {
        const glm::dvec3 here=tile.frame.local(ecef(lon,lat,alt));
        return advance(lon,lat,double(local.x)-here.x,-(double(local.z)-here.z));
    }
    // The nearest car the player can reach: his own, one he abandoned, or one
    // in traffic. Returns false when there is none.
    struct Reach {saida::Node* node=nullptr; Loaded* tile=nullptr; size_t agent=0; int kind=0;};
    bool nearestCar(Reach& out,double& distance) {
        distance=kCarReach;
        bool found=false;
        const glm::dvec3 me=ecef(lon,lat,alt);
        if(carParked&&car&&!carSinking) {
            const double d=glm::length(me-ecef(carLon,carLat,carAlt));
            if(d<distance){distance=d;out={car,nullptr,0,0};found=true;}
        }
        for(size_t i=0;i<parked.size();++i) {
            const auto& entry=parked[i];
            const double d=glm::length(me-ecef(entry.lon,entry.lat,entry.alt));
            if(d<distance){distance=d;out={entry.node,nullptr,i,1};found=true;}
        }
        for(auto& [key,tile]:loaded) {
            for(size_t i=0;i<tile.cars.size();++i) {
                if(!tile.cars[i]||!trafficSlotLive(tile,i))continue;
                const auto here=geodeticOf(tile,tile.cars[i]->transform().position);
                const double d=glm::length(me-ecef(here.x,here.y,alt));
                if(d<distance){distance=d;out={tile.cars[i],&tile,i,2};found=true;}
            }
        }
        return found;
    }
    // Hand a traffic car over: it leaves the flow, leaves its tile, and becomes
    // the car the player is in. The flow must forget it first -- a car being
    // driven by the player and by the simulation at once is two cars.
    void takeTrafficCar(const Reach& target) {
        Loaded& tile=*target.tile;
        const glm::vec3 local=target.node->transform().position;
        const auto here=geodeticOf(tile,local);
        tile.flow.retire(target.agent);
        tile.cars[target.agent]=nullptr;
        auto owned=tile.node->detachChild(target.node);
        if(!owned) {
            saida::Log::warn("[World car] the traffic car could not be detached from ",
                             tile.data.value("key",std::string()));
            return;
        }
        abandonCurrentCar();
        car=engine.sceneTree().world().addChild(std::move(owned));
        carParked=true;
        carLon=here.x;carLat=here.y;carAlt=groundAt(carLon,carLat,alt);
        // Keep its heading: the car was driving somewhere, and snapping it to
        // the player's facing is a jolt the eye catches at two metres.
        const glm::vec3 forward=car->transform().rotation*glm::vec3(0,0,-1);
        carYaw=std::atan2(double(forward.x),-double(forward.z))/rad;
        carSpeed=0;wheelSpin=0;steerShown=0;
        collectWheels(*car);
        saida::Log::info("[World car] took a traffic car at ",carLon,", ",carLat);
    }
    void takeAbandonedCar(const Reach& target) {
        const Parked entry=parked[target.agent];
        parked.erase(parked.begin()+long(target.agent));
        abandonCurrentCar();
        car=entry.node;carParked=true;
        carLon=entry.lon;carLat=entry.lat;carAlt=entry.alt;carYaw=entry.yaw;
        carSpeed=0;wheelSpin=0;steerShown=0;
        collectWheels(*car);
    }

    // Why the car last stopped against something, for whoever reads the log.
    std::string carStop="nothing";
    std::string carStopSaid; // the obstacle the journal last named, cleared once the car moves on
    bool parkingCar=false;
    double carDistance() const {
        return glm::length(ecef(lon,lat,alt)-ecef(carLon,carLat,carAlt));
    }
    bool enterCar() {
        if(driving||swimming)return false;
        Reach target;double distance=0;
        if(!nearestCar(target,distance))return false;
        if(target.kind==2)takeTrafficCar(target);
        else if(target.kind==1)takeAbandonedCar(target);
        if(!carParked||!car)return false;
        driving=true;carSpeed=0;lookYaw=0;lookIdle=0;
        // The car is where the player is now, and the walk's own lon/lat drive
        // it from here: one position, one streamer, one sun. Nothing downstream
        // has to learn that the player is sitting down.
        lon=carLon;lat=carLat;alt=carAlt;yaw=carYaw;
        player->setEnabled(false);
        for(auto* a:animators)a->play("idle");
        jumpOffset=jumpVelocity=0;
        text("stream-status","Au volant. F : descendre · Espace : frein à main.");
        saida::Log::info("[World car] entered at ",carLon,", ",carLat);
        return true;
    }
    // Returns false when the car refuses to let him out, and says why both
    // times: a door that does nothing is the failure mode this file exists to
    // keep out of the game (rule 3 of CLAUDE.md).
    bool leaveCar() {
        if(!driving)return false;
        if(std::abs(carSpeed)>kCarExitSpeed) {
            text("stream-status","Trop rapide pour descendre — ralentissez.");
            saida::Log::info("[World car] exit refused at ",std::abs(carSpeed)," m/s");
            return false;
        }
        // Beside the car, never inside the shell it collides with.
        double x=0,y=0;bool found=false;
        for(double side:{2.6,-2.6}) {
            auto q=onward(carLon,carLat,cos(carYaw*rad)*side,-sin(carYaw*rad)*side);
            if(tile(q.x,q.y)&&!blocked(q.x,q.y,carAlt)&&std::abs(height(q.x,q.y,carAlt)-carAlt)<kStepUp){x=q.x;y=q.y;found=true;break;}
        }
        if(!found)found=freeSpot(carLon,carLat,9.,x,y,carAlt);
        if(!found) {
            text("stream-status","Impossible de descendre ici — avancez un peu.");
            saida::Log::warn("[World car] no standable ground within 9 m of ",carLon,", ",carLat);
            return false;
        }
        driving=false;carSpeed=0;steerShown=0;
        // Step out looking where you were looking, not snapped back to the
        // bonnet: the offset becomes the walk's own yaw.
        yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
        lon=x;lat=y;alt=groundAt(x,y,carAlt);
        player->setEnabled(true);
        player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
        followDistance=std::min(followDistance,kOnFootFollow);
        text("stream-status","À pied. F : remonter dans la voiture.");
        saida::Log::info("[World car] left at ",lon,", ",lat);
        request(lon,lat);
        return true;
    }
    void sinkCar(double x,double y) {
        carWaterLevel=waterLevel(x,y);carSinkDepth=0;carSinking=true;
        carLon=x;carLat=y;carAlt=carWaterLevel;carSpeed=0;steerShown=0;
        driving=false;swimming=true;swimTime=0;swimLean=0;
        lon=x;lat=y;alt=carWaterLevel;yaw=wrap(carYaw+lookYaw);swimHeading=yaw;
        lookYaw=0;lookIdle=0;jumpOffset=jumpVelocity=0;
        followDistance=std::min(followDistance,kOnFootFollow);
        player->setEnabled(true);
        text("stream-status","La voiture coule — nagez vers la rive.");
        saida::Log::info("[World car] sank at ",lon,", ",lat);
        request(lon,lat);
    }
    void updateSinkingCar(double dt) {
        if(!carSinking||!carParked||!car)return;
        carSinkDepth=std::min(kCarSinkDepth,carSinkDepth+kCarSinkRate*dt);
        carAlt=carWaterLevel-carSinkDepth;
    }
    // One step of the car, in the tangent plane. Throttle and steering are the
    // same two axes the walk reads; what differs is that a car carries its
    // speed and its heading from one frame to the next, which is the whole of
    // what makes driving feel unlike walking.
    void driveCar(double dt,double throttle,double steerInput,bool handbrake) {
        if(onWater(lon,lat,alt)){sinkCar(lon,lat);return;}
        if(throttle>0)carSpeed=std::min(kCarTopSpeed,carSpeed+kCarAccel*throttle*dt);
        else if(throttle<0) {
            // Reverse is what the brake becomes once the car has stopped, which
            // is the pedal a driver expects and one key fewer to explain.
            if(carSpeed>.2)carSpeed=std::max(0.,carSpeed-kCarBrake*dt);
            else carSpeed=std::max(kCarReverseSpeed,carSpeed+kCarAccel*throttle*dt);
        }
        if(handbrake)carSpeed-=std::copysign(std::min(std::abs(carSpeed),kCarHandbrake*dt),carSpeed);
        if(throttle==0&&!handbrake) {
            double loss=(kCarRollResist+kCarDrag*carSpeed*carSpeed)*dt;
            carSpeed-=std::copysign(std::min(std::abs(carSpeed),loss),carSpeed);
        }
        double v=std::abs(carSpeed);
        if(v>1e-3) {
            // Two limits: the front wheels cannot cut a circle tighter than
            // kCarTurnRadius, and the heading cannot swing faster than
            // kCarLateralAccel allows at this speed. The second still stops a
            // hairpin at 130 km/h from being one key press -- 27 degrees a
            // second up there -- while leaving 90 at town speed.
            double turn=std::min(v/std::max(kCarTurnRadius,vehicleSpec(*car).wheelbase*1.45),kCarLateralAccel/v);
            carYaw=wrap(carYaw+steerInput*turn/rad*dt*(carSpeed<0?-1:1));
            wheelSpin+=carSpeed/vehicleSpec(*car).wheelRadius*dt;
        }
        steerShown+=(steerInput-steerShown)*(1-std::exp(-9*dt));
        yaw=carYaw;
        if(v<=1e-3){carLon=lon;carLat=lat;carAlt=alt;return;}
        auto next=onward(lon,lat,sin(carYaw*rad)*carSpeed*dt,cos(carYaw*rad)*carSpeed*dt);
        if(!tile(next.x,next.y))stream(); // Mount a prefetched tile before stopping at its edge.
        if(!tile(next.x,next.y)) {
            // Outrunning the streamer. Said rather than shown as a stutter: at
            // 130 km/h a car reaches the edge of the loaded world in seconds,
            // and a wall of nothing has to announce itself as one.
            carSpeed=0;
            text("stream-status","Bord du terrain chargé — les données suivantes arrivent.");
            return;
        }
        if(onWater(next.x,next.y,alt)){carStop="water";sinkCar(next.x,next.y);return;}
        // Arcade handling keeps geographic pose; obstacle detection is a Jolt
        // query along the travelled step, including the streamed traffic bodies.
        // Each step is tested standing on the ground it reaches, not at the
        // altitude the car left: up a ramp, the old altitude is inside it.
        const double length=std::abs(carSpeed)*dt;
        const int steps=std::max(1,int(std::ceil(length/.25)));
        double level=alt;
        for(int i=1;i<=steps;++i) {const double d=length*i/steps;
            const auto q=onward(lon,lat,sin(carYaw*rad)*std::min(d,length)*(carSpeed<0?-1:1),
                                            cos(carYaw*rad)*std::min(d,length)*(carSpeed<0?-1:1));
            level=drivenGround(q.x,q.y,level);
            if(blocked(q.x,q.y,level)) {
                const std::string stop="an obstacle ("+obstacleName(q.x,q.y,level)+")";
                // Once per obstacle, not once per frame the throttle is held.
                if(stop!=carStopSaid)saida::Log::info("[World car] stopped by ",stop," at ",q.x,", ",q.y," level ",level);
                carStop=carStopSaid=stop;carSpeed=0;
                text("stream-status","Obstacle — la voiture s'arrête.");return;}
        }
        lon=next.x;lat=next.y;alt=drivenGround(lon,lat,level);
        carStopSaid.clear();
        carLon=lon;carLat=lat;carAlt=alt;
        request(lon,lat);
    }

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
    static double segmentDistance(glm::dvec2 p,glm::dvec2 a,glm::dvec2 b) {
        const glm::dvec2 d=b-a;const double len=glm::dot(d,d);
        const double f=len>0?std::clamp(glm::dot(p-a,d)/len,0.,1.):0.;
        return glm::length(p-(a+f*d));
    }
    // Distance from the player to a hull's side, in metres, in `frame`.
    double hullDistance(const Frame& frame,glm::dvec3 centre,double heading,double length,double beam) const {
        const glm::dvec3 me=frame.local(ecef(lon,lat,alt));
        const glm::dvec2 axis(std::sin(heading*rad),-std::cos(heading*rad));
        const glm::dvec2 c(centre.x,centre.z);
        return segmentDistance({me.x,me.z},c-axis*(length*.5),c+axis*(length*.5))-beam*.5;
    }
    saida::Node* mooredNode(Loaded& t,Mooring& m) {
        if(!m.node&&!m.taken)m.node=t.node->findByPath(m.name);
        return m.node;
    }
    bool nearestBoat(BoatReach& out,double& distance,double extra=0) {
        distance=kBoatReach+extra;bool found=false;
        for(auto& [key,t]:loaded)
            for(size_t i=0;i<t.boats.size();++i) {
                Mooring& m=t.boats[i];
                if(m.taken||!mooredNode(t,m))continue;
                const double d=hullDistance(t.frame,m.local,m.heading,m.length,m.beam);
                if(d<distance){distance=d;out={&t,i,BoatSource::Moored};found=true;}
            }
        for(size_t i=0;i<leftBoats.size();++i) {
            const Vessel& v=leftBoats[i];
            const Frame here(v.lon,v.lat,v.alt);
            const double d=hullDistance(here,glm::dvec3(0),v.yaw,v.length,v.beam);
            if(d<distance){distance=d;out={nullptr,i,BoatSource::Left};found=true;}
        }
        for(size_t i=0;i<seaShips.size();++i) {
            const Vessel& v=seaShips[i].v;
            const Frame here(v.lon,v.lat,v.alt);
            const double d=hullDistance(here,glm::dvec3(0),v.yaw,v.length,v.beam);
            if(d<distance){distance=d;out={nullptr,i,BoatSource::Sea};found=true;}
        }
        return found;
    }
    bool enterBoat() {
        if(sailing||driving)return false;
        BoatReach target;double distance=0;
        if(!nearestBoat(target,distance))return false;
        return board(target);
    }
    bool board(const BoatReach& target) {
        if(target.source==BoatSource::None)return false;
        if(target.source==BoatSource::Sea) {
            // A ship under way is taken under way: the helm keeps her speed.
            boat=seaShips[target.index].v;
            seaTaken.insert(seaShips[target.index].id);
            seaShips.erase(seaShips.begin()+long(target.index));
        } else if(target.source==BoatSource::Left) {
            boat=leftBoats[target.index];
            leftBoats.erase(leftBoats.begin()+long(target.index));
        } else {
            Loaded& t=*target.tile;Mooring& m=t.boats[target.index];
            auto owned=t.node->detachChild(m.node);
            if(!owned) {
                saida::Log::warn("[World boat] ",m.name," could not be detached from its tile");
                return false;
            }
            m.taken=true;m.node=nullptr;
            const auto where=geodeticOf(t,glm::vec3(m.local));
            boat=Vessel{};
            boat.node=engine.sceneTree().world().addChild(std::move(owned));
            boat.kind=m.kind;boat.length=m.length;boat.beam=m.beam;
            boat.top=m.top;boat.accel=m.accel;boat.turn=m.turn;
            boat.lon=where.x;boat.lat=where.y;boat.yaw=m.heading;
            boat.alt=waterLevel(boat.lon,boat.lat);
        }
        sailing=true;swimming=false;lookYaw=0;lookIdle=0;
        lon=boat.lon;lat=boat.lat;alt=boat.alt;yaw=boat.yaw;
        player->setEnabled(false);
        for(auto* a:animators)a->play("idle");
        jumpOffset=jumpVelocity=0;
        text("stream-status","À la barre. F : débarquer près d'un quai · S : marche arrière.");
        saida::Log::info("[World boat] took the helm of a ",boat.kind," (",boat.length," m) at ",lon,", ",lat);
        return true;
    }
    void keepBoat(Vessel vessel) {
        vessel.speed=0;
        leftBoats.push_back(std::move(vessel));
        while(leftBoats.size()>kLeftBoats) {
            if(leftBoats.front().node)leftBoats.front().node->queueFree();
            leftBoats.erase(leftBoats.begin());
        }
    }
    struct BoatExit {glm::dvec2 where; bool swimming;};
    std::optional<BoatExit> boatExit() {
        const double s=std::sin(boat.yaw*rad),c=std::cos(boat.yaw*rad);
        std::optional<BoatExit> water;
        for(double off:{1.,2.,3.5})
            for(double along=0;along<=boat.length*.5;along+=2.)
                for(double sign:{1.,-1.})for(double side:{1.,-1.}) {
                    const double a=along*sign,o=(boat.beam*.5+off)*side;
                    const auto q=onward(boat.lon,boat.lat,s*a+c*o,c*a-s*o);
                    if(!tile(q.x,q.y))continue;
                    if(navigable(q.x,q.y)) {
                        if(!water)water=BoatExit{q,true};
                    } else if(!blocked(q.x,q.y))return BoatExit{q,false};
                }
        return water;
    }
    // At a quay the player steps ashore. Offshore, he drops into the water and
    // can swim back to a bank or to the same hull. Speed still makes either
    // manoeuvre unsafe.
    bool leaveBoat() {
        if(!sailing)return false;
        if(std::abs(boat.speed)>kBoatExitSpeed) {
            text("stream-status","Trop rapide pour débarquer — ralentissez.");
            saida::Log::info("[World boat] landing refused at ",std::abs(boat.speed)," m/s");
            return false;
        }
        const auto exit=boatExit();
        if(!exit) {
            text("stream-status","Eau et rive hors du terrain chargé — attendez les données.");
            saida::Log::info("[World boat] no loaded exit beside ",boat.lon,", ",boat.lat);
            return false;
        }
        sailing=false;keepBoat(boat);boat=Vessel{};
        yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
        lon=exit->where.x;lat=exit->where.y;swimming=exit->swimming;
        swimTime=0;swimHeading=yaw;swimLean=0;
        alt=swimming?waterLevel(lon,lat):height(lon,lat);
        player->setEnabled(true);
        player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
        followDistance=std::min(followDistance,kOnFootFollow);
        text("stream-status",swimming?"À l'eau. ZQSD/WASD : nager · F : remonter à bord."
                                     :"À terre. F : reprendre le bateau.");
        saida::Log::info(swimming?"[World boat] swimming at ":"[World boat] ashore at ",lon,", ",lat);
        request(lon,lat);
        return true;
    }
    // From one hull to another: alongside, at matched speeds. The boat left
    // behind stays where she is, like any boat stepped off.
    HopResult hopAboard() {
        if(!sailing)return HopResult::NoTarget;
        BoatReach target;double distance=0;
        if(!nearestBoat(target,distance,boat.beam*.5))return HopResult::NoTarget;
        const Vessel* other=target.source==BoatSource::Sea?&seaShips[target.index].v
                            :target.source==BoatSource::Left?&leftBoats[target.index]:nullptr;
        const glm::dvec2 mine(std::sin(boat.yaw*rad)*boat.speed,std::cos(boat.yaw*rad)*boat.speed);
        const glm::dvec2 theirs=other?glm::dvec2(std::sin(other->yaw*rad)*other->speed,
                                                  std::cos(other->yaw*rad)*other->speed):glm::dvec2(0);
        const double relativeSpeed=glm::length(mine-theirs);
        if(relativeSpeed>kHopSpeed) {
            text("stream-status","Réglez votre vitesse sur la sienne pour passer à bord.");
            saida::Log::info("[World boat] hop refused at ",relativeSpeed," m/s relative");
            return HopResult::Refused;
        }
        Vessel left=boat;left.speed=0;
        sailing=false;boat=Vessel{};
        if(!board(target)) {boat=left;sailing=true;return HopResult::Refused;}
        keepBoat(std::move(left));
        saida::Log::info("[World boat] stepped across to a ",boat.kind," (",boat.length," m)");
        return HopResult::Boarded;
    }
    void clearBoats() {
        for(auto& v:leftBoats)if(v.node)v.node->queueFree();
        leftBoats.clear();
        for(auto& s:seaShips)if(s.v.node)s.v.node->queueFree();
        seaShips.clear();seaTaken.clear();seaFirst=true;seaAsk=5.;
        if(boat.node)boat.node->queueFree();
        boat=Vessel{};sailing=false;
    }
    // One step of the boat. Water resists with the square of the speed, so a
    // hull settles at its top speed rather than being clamped to it; the rudder
    // needs way on to bite, and bites the other way going astern.
    void sailBoat(double dt,double throttle,double rudder) {
        const double drag=boat.accel/(boat.top*boat.top);
        double thrust=0;
        if(throttle>0)thrust=boat.accel*throttle;
        else if(throttle<0)thrust=boat.speed>.3?-2.*boat.accel:boat.accel*throttle*.5;
        double coast=throttle==0?.12*boat.accel*(boat.speed>0?1.:boat.speed<0?-1.:0.):0.;
        boat.speed+=(thrust-drag*boat.speed*std::abs(boat.speed)-coast)*dt;
        if(throttle==0&&std::abs(boat.speed)<.05)boat.speed=0;
        boat.speed=std::clamp(boat.speed,-.3*boat.top,boat.top);
        const double way=std::clamp(std::abs(boat.speed)/(.25*boat.top),.15,1.);
        boat.yaw=wrap(boat.yaw+rudder*boat.turn*way*dt*(boat.speed<-.05?-1.:1.));
        yaw=boat.yaw;
        if(std::abs(boat.speed)<1e-3)return;
        const double s=std::sin(boat.yaw*rad),c=std::cos(boat.yaw*rad);
        auto next=onward(lon,lat,s*boat.speed*dt,c*boat.speed*dt);
        const double lead=boat.length*.5*(boat.speed>=0?1.:-1.);
        auto bow=onward(next.x,next.y,s*lead,c*lead);
        if(!tile(next.x,next.y)||!tile(bow.x,bow.y))stream();
        if(!tile(next.x,next.y)||!tile(bow.x,bow.y)) {
            boat.speed=0;
            text("stream-status","Bord du terrain chargé — les données suivantes arrivent.");
            return;
        }
        if(!navigable(next.x,next.y)||!navigable(bow.x,bow.y)) {
            // Aground is a refusal like any other: the boat stops, and the
            // player is told which way is out.
            boat.speed=0;
            text("stream-status","Rivage ou haut-fond — le bateau s'arrête. S : marche arrière.");
            return;
        }
        lon=next.x;lat=next.y;alt=waterLevel(lon,lat);
        boat.lon=lon;boat.lat=lat;boat.alt=alt;
        request(lon,lat);
    }
    // Afloat: a heave and a slow pitch and roll that shrink with the hull, so
    // a dinghy bobs and a container ship does not.
    glm::quat boatSway(const Vessel& v,double phase,double& heave) const {
        const double size=std::min(1.,9./std::max(1.,v.length));
        heave=.12*size*std::sin(seaTime*1.1+phase);
        const double pitch=2.5*size*std::sin(seaTime*1.3+phase*1.7);
        const double roll=3.5*size*std::sin(seaTime*.9+phase*.6);
        return glm::angleAxis(float(-v.yaw*rad),glm::vec3(0,1,0))
              *glm::angleAxis(float(pitch*rad),glm::vec3(1,0,0))
              *glm::angleAxis(float(roll*rad),glm::vec3(0,0,1));
    }
    void placeBoats() {
        auto place=[&](const Vessel& v,double phase){
            if(!v.node)return;
            double heave=0;const glm::quat sway=boatSway(v,phase,heave);
            v.node->transform().position=glm::vec3(origin.local(ecef(v.lon,v.lat,v.alt+heave)));
            v.node->transform().rotation=sway;
        };
        if(sailing)place(boat,0.);
        for(size_t i=0;i<leftBoats.size();++i)place(leftBoats[i],double(i+1)*1.9);
        for(auto& s:seaShips)place(s.v,s.phase);
    }

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
    static void aircraftLod(saida::Node& node) {
        auto* lod=node.addBehaviour<saida::LODGroupBehaviour>();
        lod->setLevels({{"Near",.03f},{"Far",0.f}});
    }
    void buildAircraftPrototypes() {
        for(const auto& type:r1::palette().aircraft) {
            auto root=std::make_unique<saida::Node>("aircraft-"+type.name);
            for(const auto& level:std::vector<std::pair<std::string,std::string>>{{type.nearModel,"Near"},{type.farModel,"Far"}}) {
                auto* source=prototypeForStartup(level.first);
                if(!source)throw std::runtime_error("Aircraft model failed: "+level.first);
                auto child=clonePlant(*source);child->setName(level.second);
                child->transform().rotation=glm::quat(0,0,1,0); // Authored nose +Z.
                root->addChild(std::move(child));
            }
            aircraftPrototypes[type.name]=prototypes->addChild(std::move(root));
        }
        saida::Log::info("[World aircraft] ",aircraftPrototypes.size()," aircraft types ready");
    }
    // The tile's parked aircraft, as scene nodes in its frame. Children of the
    // tile, so evicting the tile clears them with it.
    // Lettering (gen/predict facadeLettering): the sign font's glyphs, inked
    // with its coverage so no baked plate shows, in `colour`, facing +Z.
    std::unique_ptr<saida::Node> lettering(const std::string& text,double width,double height,glm::vec3 colour) {
        auto letters=r1::facadeLettering(text,width,height);if(!letters)return nullptr;
        auto sign=plantNode(*letters);if(!sign)return nullptr;
        saida::MaterialDesc desc;
        desc.baseColor={colour.x,colour.y,colour.z,1.f};
        desc.roughness=.55f;desc.alphaCutoff=.35f;
        desc.albedoId=texture("assets/textures/interiors/fascia_letters.png",true);
        auto* ink=engine.resources().getMaterial(desc);
        std::function<void(saida::Node&)> paint=[&](saida::Node& node) {
            if(auto* mesh=dynamic_cast<saida::MeshNode*>(&node)){mesh->setMaterial(ink);mesh->castShadows()=false;}
            for(auto& child:node.children())paint(*child);
        };
        paint(*sign);
        return sign;
    }
    // A tile's own lettering: fuel canopies and totems (gen/fuel).
    void mountLettering(Loaded& t) {
        const auto started=std::chrono::steady_clock::now();
        auto list=t.data.find("lettering");if(list==t.data.end())return;
        while(t.nextLettering<list->size()) {
            const auto& entry=(*list)[t.nextLettering];
            const std::string text=entry.at("text");
            const auto letters=r1::facadeLettering(text,entry.at("width"),entry.at("height"));
            if(letters&&!modelsReady(*letters))return;
            ++t.nextLettering;
            const auto& c=entry.at("colour");
            auto sign=lettering(text,entry.at("width"),entry.at("height"),{float(c[0]),float(c[1]),float(c[2])});
            if(!sign){saida::Log::warn("[World lettering] font cannot spell: ",text);continue;}
            const auto& at=entry.at("at");const auto& n=entry.at("normal");
            sign->transform().position={float(at[0]),float(at[1]),float(at[2])};
            sign->transform().rotation=glm::angleAxis(float(std::atan2(n[0].get<double>(),n[1].get<double>())),glm::vec3(0,1,0));
            t.node->addChild(std::move(sign));
            if(msSince(started)>=kPropImportMs)return;
        }
    }
    void mountAircraft(Loaded& t) {
        for(auto& spot:t.aircraft) {
            auto found=aircraftPrototypes.find(spot.type);
            if(found==aircraftPrototypes.end()) {
                saida::Log::warn("[World aircraft] ",t.data.value("key",std::string())," parks an unknown type: ",spot.type);
                continue;
            }
            auto node=clonePlant(*found->second);
            node->setName(spot.name);
            aircraftLod(*node);
            node->transform().position=glm::vec3(spot.local);
            node->transform().rotation=glm::angleAxis(float(-spot.heading*rad),glm::vec3(0,1,0));
            spot.node=t.node->addChild(std::move(node));
        }
    }
    static void collectAircraftParts(Aircraft& a) {
        a.gear.clear();a.mainRotors.clear();a.tailRotors.clear();
        // Outermost match only, as with the car's wheels: the importer wraps a
        // named node around a mesh node that inherits its name.
        std::function<void(saida::Node&)> walk=[&](saida::Node& n){
            if(n.name()=="gear"){a.gear.push_back(&n);return;}
            if(n.name()=="rotor-main"){a.mainRotors.push_back(&n);return;}
            if(n.name()=="rotor-tail"){a.tailRotors.push_back(&n);return;}
            for(auto& c:n.children())walk(*c);
        };
        if(a.node)walk(*a.node);
    }
    static double fuselageHalf(const r1::AircraftType& t) {return std::max(1.2,t.length*.05);}
    double roofAt(double x,double y,double above) {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return -1e30;
        const glm::vec3 start(origin.local(ecef(x,y,above+1.)));
        const auto hit=physics->raycast(start,{0,-1,0},500.f,obstacleFilter());
        if(!hit.hit||hit.normal.y<.5f)return -1e30;
        const double surface=above+1.+double(hit.point.y-start.y);
        return surface>groundAt(x,y,above)+1.?surface:-1e30;
    }
    bool buildingAt(double x,double y,double level) {
        return solidAt(x,y,level,.45f);
    }
    // What an aircraft rests on at (x, y): the terrain, a deck, the water's
    // level, or a roof it is above. Where no tile answers yet, the last
    // surface it saw -- said on screen, never guessed at.
    double surfaceUnder(double x,double y,double above,bool* onRoof=nullptr,bool* wet=nullptr) {
        if(onRoof)*onRoof=false;
        if(wet)*wet=false;
        if(!tile(x,y))return planeGround;
        double surface=height(x,y,above);
        if(onWater(x,y)){surface=waterLevel(x,y);if(wet)*wet=true;}
        const double roof=roofAt(x,y,above);
        if(roof>surface){surface=roof;if(onRoof)*onRoof=true;if(wet)*wet=false;}
        planeGround=surface;
        return surface;
    }
    // The aircraft's extremities -- nose, tail and the two tips of its wings
    // or its rotor -- against the buildings, at the height of its underside.
    bool aircraftHits(double x,double y,double alt,double heading,bool sweep=false) {
        const auto& t=*plane.type;
        const double s=std::sin(heading*rad),c=std::cos(heading*rad);
        const double half=t.length*.5,span=t.span*.5,body=alt+.4;
        for(const auto& [a,o]:{std::pair{0.,0.},std::pair{half,0.},std::pair{-half,0.},std::pair{0.,span},std::pair{0.,-span}}) {
            const auto q=onward(x,y,s*a+c*o,c*a-s*o);
            if(buildingAt(q.x,q.y,body))return true;
            if(sweep) {
                const auto from=onward(plane.lon,plane.lat,s*a+c*o,c*a-s*o);
                const glm::vec3 start(origin.local(ecef(from.x,from.y,body))),end(origin.local(ecef(q.x,q.y,body)));
                const glm::vec3 delta=end-start;const float length=glm::length(delta);
                auto* physics=engine.sceneTree().world().physics();
                if(physics&&length>.001f&&physics->raycast(start,delta/length,length,obstacleFilter()).hit)return true;
            }
        }
        return false;
    }
    enum class PlaneSource {None, Parked, Left};
    struct PlaneReach {Loaded* tile=nullptr; size_t index=0; PlaneSource source=PlaneSource::None;};
    bool nearestAircraft(PlaneReach& out,double& distance) {
        distance=kAircraftReach;bool found=false;
        for(auto& [key,t]:loaded)
            for(size_t i=0;i<t.aircraft.size();++i) {
                const AircraftSpot& spot=t.aircraft[i];
                const auto* type=r1::aircraftType(spot.type);
                if(spot.taken||!spot.node||!type)continue;
                const double d=hullDistance(t.frame,spot.local,spot.heading,type->length,2*fuselageHalf(*type));
                if(d<distance){distance=d;out={&t,i,PlaneSource::Parked};found=true;}
            }
        for(size_t i=0;i<leftAircraft.size();++i) {
            const Aircraft& a=leftAircraft[i];
            if(a.airborne)continue;  // still falling
            const Frame here(a.lon,a.lat,a.alt);
            const double d=hullDistance(here,glm::dvec3(0),a.yaw,a.type->length,2*fuselageHalf(*a.type));
            if(d<distance){distance=d;out={nullptr,i,PlaneSource::Left};found=true;}
        }
        return found;
    }
    bool enterAircraft() {
        if(piloting||driving||sailing)return false;
        PlaneReach target;double distance=0;
        if(!nearestAircraft(target,distance))return false;
        Aircraft a;
        if(target.source==PlaneSource::Left) {
            a=leftAircraft[target.index];
            leftAircraft.erase(leftAircraft.begin()+long(target.index));
        } else {
            Loaded& t=*target.tile;AircraftSpot& spot=t.aircraft[target.index];
            auto owned=t.node->detachChild(spot.node);
            if(!owned) {
                saida::Log::warn("[World aircraft] ",spot.name," could not be detached from its tile");
                text("stream-status","Cet appareil ne peut pas être pris.");
                return false;
            }
            spot.taken=true;spot.node=nullptr;
            const auto where=geodeticOf(t,glm::vec3(spot.local));
            a.node=engine.sceneTree().world().addChild(std::move(owned));
            a.type=r1::aircraftType(spot.type);
            a.lon=where.x;a.lat=where.y;a.yaw=spot.heading;
            a.alt=surfaceUnder(a.lon,a.lat,alt+1.);
        }
        a.speed=0;a.climb=0;a.lever=0;a.thrust=0;a.airborne=false;a.pitch=0;a.roll=0;
        collectAircraftParts(a);
        plane=std::move(a);piloting=true;swimming=false;planeStopped=false;
        lookYaw=0;lookIdle=0;
        lon=plane.lon;lat=plane.lat;alt=plane.alt;yaw=plane.yaw;
        player->setEnabled(false);
        for(auto* anim:animators)anim->play("idle");
        jumpOffset=jumpVelocity=0;
        text("stream-status",plane.helicopter()
            ?"Aux commandes. Espace/Maj : monter/descendre · Z/S : avancer/reculer · Q/D : tourner · F : descendre."
            :"Aux commandes. Z/S : gaz · Q/D : virer · Espace/Maj : cabrer/piquer · F : descendre.");
        saida::Log::info("[World aircraft] took the controls of a ",plane.type->name," at ",lon,", ",lat,
                         plane.helicopter()?" (helicopter)":"");
        return true;
    }
    void keepAircraft(Aircraft a) {
        a.speed=0;a.climb=0;a.lever=0;a.thrust=0;
        leftAircraft.push_back(std::move(a));
        while(leftAircraft.size()>kLeftAircraft) {
            if(leftAircraft.front().node)leftAircraft.front().node->queueFree();
            leftAircraft.erase(leftAircraft.begin());
        }
    }
    // F leaves the controls at any moment, as in GTA. On the ground the pilot
    // steps down beside the nose, on the left where the doors are; on water he
    // drops into it. In the air or on a roof he jumps: he falls from the
    // aircraft's height to the ground beside it, and an aircraft left in the
    // air falls too (World::dropLeftAircraft).
    bool leaveAircraft() {
        if(!piloting)return false;
        const auto& t=*plane.type;
        const double s=std::sin(plane.yaw*rad),c=std::cos(plane.yaw*rad),half=fuselageHalf(t);
        std::optional<glm::dvec2> dry,wet;
        for(double along:{t.length*.3,t.length*.42,0.,-t.length*.25})
            for(double side:{-1.,1.})
                for(double off:{2.,4.}) {
                    if(dry)break;
                    const double o=(half+off)*side;
                    const auto q=onward(plane.lon,plane.lat,s*along+c*o,c*along-s*o);
                    if(!tile(q.x,q.y))continue;
                    if(navigable(q.x,q.y)){if(!wet)wet=q;}
                    else if(!blocked(q.x,q.y))dry=q;
                }
        double x=0,y=0;
        // Over a city the ground beside the fuselage may all be roofs: in the
        // air he can come down further off, in the open.
        const double around=plane.airborne?t.span*.5+80.:t.span*.5+8.;
        if(!dry&&!wet&&freeSpot(plane.lon,plane.lat,around,x,y))dry=glm::dvec2(x,y);
        if(!dry&&!wet) {
            text("stream-status","Impossible de descendre ici — le sol en dessous n'est pas encore chargé.");
            saida::Log::warn("[World aircraft] no loaded ground beside the aircraft at ",plane.lon,", ",plane.lat);
            return false;
        }
        const glm::dvec2 at=dry?*dry:*wet;
        const double from=plane.alt,climb=plane.climb;
        piloting=false;keepAircraft(plane);plane=Aircraft{};
        yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
        lon=at.x;lat=at.y;
        swimTime=0;swimHeading=yaw;swimLean=0;
        alt=dry?height(lon,lat):waterLevel(lon,lat);
        // Above what he lands on, he falls, into the water too; he swims once
        // he is in it (the landing, in the frame's walk).
        const double drop=from-alt;
        const bool falls=drop>.5;
        swimming=!dry&&!falls;
        jumpOffset=falls?drop:0.;
        jumpVelocity=falls?std::clamp(climb,-kFallTerminal,15.):0.;
        player->setEnabled(true);
        player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
        followDistance=std::min(followDistance,kOnFootFollow);
        text("stream-status",falls?"Saut !":swimming?"À l'eau. F : remonter à bord.":"À pied. F : reprendre l'appareil.");
        saida::Log::info("[World aircraft] ",falls?"jumped":"stepped down"," at ",lon,", ",lat,
                         falls?", falling "+std::to_string(int(drop))+" m":"",!dry?" into the water":"");
        request(lon,lat);
        return true;
    }
    // An aircraft left in the air falls, levelling, until it rests on what is
    // under it: the ground, a roof or the water. It stops there, no crash (the
    // player's call). Over ground not loaded yet it waits for it.
    void dropLeftAircraft(Aircraft& a,double dt) {
        if(!a.airborne||!tile(a.lon,a.lat))return;
        a.climb=std::max(a.climb-kGravity*dt,-kFallTerminal);
        a.alt+=a.climb*dt;
        double under=onWater(a.lon,a.lat)?waterLevel(a.lon,a.lat):height(a.lon,a.lat);
        under=std::max(under,roofAt(a.lon,a.lat,a.alt+1.));
        a.pitch*=std::exp(-dt);a.roll*=std::exp(-dt);
        if(a.alt>under)return;
        a.alt=under;a.climb=0;a.airborne=false;a.pitch=0;a.roll=0;
        saida::Log::info("[World aircraft] a ",a.type->name," left in the air came down at ",a.lon,", ",a.lat);
    }
    void clearAircraft() {
        for(auto& a:leftAircraft)if(a.node)a.node->queueFree();
        leftAircraft.clear();
        if(plane.node)plane.node->queueFree();
        plane=Aircraft{};piloting=false;
    }
    // One horizontal step. A building stops the aircraft where it touches it
    // and says so -- no crash (the request was explicit). The edge of the
    // loaded world stops it on the ground; in the air it flies on over the
    // last surface it saw, and the HUD says the ground ahead is unknown.
    bool moveAircraft(double dt,double horizontal) {
        if(std::abs(horizontal)<1e-4)return true;
        const double s=std::sin(plane.yaw*rad),c=std::cos(plane.yaw*rad);
        const auto next=onward(plane.lon,plane.lat,s*horizontal*dt,c*horizontal*dt);
        if(!tile(next.x,next.y))stream();
        if(!tile(next.x,next.y)&&!plane.airborne) {
            plane.speed=0;
            text("stream-status","Bord du terrain chargé — les données suivantes arrivent.");
            return false;
        }
        const double heading=horizontal<0?wrap(plane.yaw+180.):plane.yaw;
        if(aircraftHits(next.x,next.y,plane.alt,heading,true)&&!aircraftHits(plane.lon,plane.lat,plane.alt,heading)) {
            plane.speed=0;
            if(!planeStopped) {
                text("stream-status","Bâtiment — l'appareil s'arrête.");
                saida::Log::info("[World aircraft] stopped by a building at ",plane.lon,", ",plane.lat,
                                 ", ",plane.alt-planeGround," m up");
            }
            planeStopped=true;
            return false;
        }
        if(!plane.airborne&&!plane.helicopter()&&tile(next.x,next.y)&&onWater(next.x,next.y)&&!onWater(plane.lon,plane.lat)) {
            plane.speed=0;
            text("stream-status","Bord de l'eau — l'appareil s'arrête.");
            return false;
        }
        planeStopped=false;
        plane.lon=next.x;plane.lat=next.y;
        return true;
    }
    // A plane. W/S move the throttle lever, and the engines follow it at
    // their own pace; on the ground S at idle brakes, then pushes back. A/D
    // steer the nose wheel on the ground and bank in the air; the turn
    // follows the bank. Space pulls the nose up, Shift pushes it down, and
    // with neither the nose settles back to level.
    void flyPlane(double dt,double f,double r,double pitchIn) {
        Aircraft& p=plane;const auto& t=*p.type;
        p.lever=std::clamp(p.lever+f*.45*dt,0.,1.);
        p.thrust+=(p.lever-p.thrust)*(1-std::exp(-t.spool*3.*dt));
        const double ratio=p.speed/t.top,drag=t.accel*ratio*std::abs(ratio);
        if(!p.airborne) {
            bool wet=false;
            surfaceUnder(p.lon,p.lat,p.alt+2.,nullptr,&wet);
            const double v=p.speed;
            const double sign=v>0?1.:v<0?-1.:0.;
            double a=t.accel*p.thrust-drag-(wet?1.5:.3)*sign;
            if(f<0&&p.lever<=0.)a=v>.3?-t.brake:(v>-3.?-1.5:0.);
            p.speed=v+a*dt;
            if(sign>0&&p.speed<0&&!(f<0&&p.lever<=0.))p.speed=0;
            if(sign<0&&p.speed>0)p.speed=0;
            const double speed=std::abs(p.speed);
            if(speed>.2)p.yaw=wrap(p.yaw+r*std::min(r1::degrees(speed/t.turnRadius),30.)*dt*(p.speed<0?-1.:1.));
            if(pitchIn>0&&p.speed>=t.rotate)p.pitch=std::min(t.maxPitch*.6,p.pitch+t.pitchRate*dt);
            else p.pitch=std::max(0.,p.pitch-t.pitchRate*dt);
            p.roll-=p.roll*(1-std::exp(-4.*dt));
            if(p.pitch>2.5&&p.speed>=t.rotate) {
                // Off the ground with the climb the rotation already gives,
                // or the next hump of the terrain would set it back down.
                p.airborne=true;p.climb=std::max(1.5,p.speed*std::sin(p.pitch*rad));p.alt+=.2;
                text("stream-status","Décollage.");
                saida::Log::info("[World aircraft] lift-off at ",p.speed," m/s");
            }
            moveAircraft(dt,p.speed);
            if(!p.airborne)p.alt=surfaceUnder(p.lon,p.lat,p.alt+2.);
            return;
        }
        const double bank=r*t.maxBank;
        p.roll+=std::clamp(bank-p.roll,-t.rollRate*dt,t.rollRate*dt);
        if(pitchIn!=0)p.pitch+=pitchIn*t.pitchRate*dt;
        else p.pitch-=std::clamp(p.pitch,-.3*t.pitchRate*dt,.3*t.pitchRate*dt);
        p.pitch=std::clamp(p.pitch,-t.maxPitch,t.maxPitch);
        // Climbing costs speed and diving gives it -- half of what gravity
        // would, which is the arcade in it.
        p.speed=std::max(0.,p.speed+(t.accel*p.thrust-drag-kGravity*std::sin(p.pitch*rad)*.5)*dt);
        // A coordinated turn, half again as quick as a real one.
        const double v=std::max(p.speed,t.stall*.6);
        p.yaw=wrap(p.yaw+r1::degrees(kGravity*std::tan(p.roll*rad)/v)*1.5*dt);
        double vertical=p.speed*std::sin(p.pitch*rad);
        if(p.speed<t.stall) {
            // The wing gives way: the aircraft sinks and its nose drops.
            const double k=(t.stall-p.speed)/t.stall;
            vertical-=14.*k;
            p.pitch-=18.*k*dt;
        }
        p.climb+=(vertical-p.climb)*(1-std::exp(-3.*dt));
        moveAircraft(dt,p.speed*std::cos(p.pitch*rad));
        p.alt+=p.climb*dt;
        bool onRoof=false,wet=false;
        const double ground=surfaceUnder(p.lon,p.lat,p.alt,&onRoof,&wet);
        if(p.alt<=ground) {
            p.alt=ground;p.airborne=false;
            const bool hard=p.climb<-7.||std::abs(p.roll)>25.||p.pitch<-8.;
            if(hard){p.speed=0;p.lever=0;p.thrust=0;}
            text("stream-status",hard?"Atterrissage brutal — l'appareil s'arrête."
                                 :wet?"Amerrissage.":onRoof?"Posé sur le toit.":"Atterrissage.");
            saida::Log::info("[World aircraft] ",hard?"hard landing":"landed"," at ",p.climb," m/s vertical, ",
                             p.speed," m/s",wet?" on water":onRoof?" on a roof":"");
            p.climb=0;p.roll=0;p.pitch=std::max(0.,p.pitch);
        }
    }
    // A helicopter. The rotor takes three seconds to come up to speed. Space
    // and Shift climb and descend and it holds its height with neither;
    // W/S fly it forward and back, A/D turn it on the spot. It sits down on
    // the ground, on water or on a roof, wherever it is lowered.
    void flyHelicopter(double dt,double f,double r,double up) {
        Aircraft& h=plane;const auto& t=*h.type;
        h.rotor=std::min(1.,h.rotor+dt/3.);
        const bool lift=h.rotor>.85;
        double want=lift?t.climb*up:(h.airborne?-3.:0.);
        if(!h.airborne&&want<0)want=0;
        h.climb+=(want-h.climb)*(1-std::exp(-3.*dt));
        const double target=h.airborne?(f>=0?f*t.top:f*t.top*.3):0.;
        h.speed+=std::clamp(target-h.speed,-t.accel*dt,t.accel*dt);
        if(!h.airborne)h.speed*=std::exp(-6.*dt);
        if(lift)h.yaw=wrap(h.yaw+r*t.rollRate*dt*(h.airborne?1.:.35));
        const double pitchTarget=h.airborne?-t.maxPitch*std::clamp(target/t.top,-.4,1.):0.;
        const double rollTarget=h.airborne?r*t.maxBank*std::clamp(.3+std::abs(h.speed)/15.,0.,1.):0.;
        h.pitch+=(pitchTarget-h.pitch)*(1-std::exp(-2.5*dt));
        h.roll+=(rollTarget-h.roll)*(1-std::exp(-2.5*dt));
        moveAircraft(dt,h.speed);
        h.alt+=h.climb*dt;
        bool onRoof=false,wet=false;
        const double ground=surfaceUnder(h.lon,h.lat,h.alt+.5,&onRoof,&wet);
        if(h.alt<=ground) {
            if(h.airborne) {
                text("stream-status",onRoof?"Posé sur le toit.":wet?"Posé sur l'eau.":"Posé.");
                saida::Log::info("[World aircraft] helicopter set down",onRoof?" on a roof":wet?" on water":"",
                                 " at ",h.lon,", ",h.lat);
            }
            h.alt=ground;h.airborne=false;h.climb=std::max(0.,h.climb);
        } else if(h.alt>ground+.3&&!h.airborne) {
            h.airborne=true;
            text("stream-status","Décollage.");
            saida::Log::info("[World aircraft] helicopter lifted off");
        }
        h.mainSpin=std::fmod(h.mainSpin+h.rotor*h.rotor*28.*dt,kTwoPi);
        h.tailSpin=std::fmod(h.tailSpin+h.rotor*h.rotor*44.*dt,kTwoPi);
    }
    glm::quat aircraftRotation(const Aircraft& a) const {
        return glm::angleAxis(float(-a.yaw*rad),glm::vec3(0,1,0))
              *glm::angleAxis(float(a.pitch*rad),glm::vec3(1,0,0))
              *glm::angleAxis(float(-a.roll*rad),glm::vec3(0,0,1));
    }
    // Turned about the centre of gravity, so a bank rolls the aircraft about
    // itself rather than about its wheels.
    void placeAircraft(double dt) {
        auto place=[&](Aircraft& a){
            if(!a.node||!a.type)return;
            const glm::quat q=aircraftRotation(a);
            const glm::vec3 cg=glm::vec3(origin.local(ecef(a.lon,a.lat,a.alt+a.type->cg)));
            a.node->transform().position=cg-q*glm::vec3(0,float(a.type->cg),0);
            a.node->transform().rotation=q;
            const bool down=!a.airborne||a.alt-groundAt(a.lon,a.lat,a.alt)<30.;
            for(auto* g:a.gear)g->setVisible(down);
            for(auto* m:a.mainRotors)m->transform().rotation=glm::angleAxis(float(a.mainSpin),glm::vec3(0,1,0));
            for(auto* m:a.tailRotors)m->transform().rotation=glm::angleAxis(float(a.tailSpin),glm::vec3(1,0,0));
        };
        if(piloting)place(plane);
        for(auto& a:leftAircraft) {
            // A helicopter left behind winds down rather than stopping dead.
            a.rotor=std::max(0.,a.rotor-dt/8.);
            a.mainSpin=std::fmod(a.mainSpin+a.rotor*a.rotor*28.*dt,kTwoPi);
            a.tailSpin=std::fmod(a.tailSpin+a.rotor*a.rotor*44.*dt,kTwoPi);
            dropLeftAircraft(a,dt);
            place(a);
        }
    }

    // ── ships at sea ────────────────────────────────────────────────────────
    //
    // The sea service predicts the ships around the player -- six years of
    // real AIS, the day, the weather, and whatever live AIS it last heard
    // (gen/sea.cpp) -- on its own thread. The game sails them.
    // A ship keeps its own state once it is out: a later prediction that no
    // longer lists it does not pull it from under the player's eyes, it sails
    // on and is dropped beyond kSeaKeep; a new one appears only beyond
    // kSeaAppear, so nothing pops into view. Each can be boarded.
    bool gameTime(double& out) {
        if(!sunScript)return false;
        json result;
        if(sunScript->callExport("gameTime",json::array(),result)!=saida::ScriptCallStatus::Succeeded
           ||!result.is_number())return false;
        out=result.get<double>();
        return true;
    }
    bool localConditions() const {
        return conditions.is_object()&&conditions.contains("lon")&&conditions.contains("lat")
            &&std::abs(conditions.value("lon",1000.)-lon)<.12
            &&std::abs(conditions.value("lat",1000.)-lat)<.12;
    }
    // The weather as last read: what the sky, the fog and the snow are drawn from.
    struct Weather {
        double cover=0,rain=0,visibility=0,windSpeed=0,windFrom=0,snowfall=0,snowDepth=0;
        int code=-1; bool known=false;
    } weather;
    bool fogFar=false;  // what the fog was last told about the horizon
    static double number(const json& j,const char* key,double fallback) {
        auto it=j.find(key);return it!=j.end()&&it->is_number()?it->get<double>():fallback;
    }
    void readConditions() {
        auto latest=sky->latest();
        if(!latest||latest==conditionsDoc)return;
        conditionsDoc=latest;conditions=*latest;
        if(!localConditions())return;
        const auto w=conditions.value("weather",json());
        weather=Weather{};
        if(inspectAt)return;  // a fixed instant is photographed under a clear sky
        if(w.is_object()) {
            weather.known=true;
            weather.cover=std::clamp(number(w,"cloudCover",0.)/100.,0.,1.);
            weather.rain=std::max(0.,number(w,"precipitation",0.));
            weather.visibility=number(w,"visibility",0.);
            weather.windSpeed=std::max(0.,number(w,"windSpeed",0.));
            weather.windFrom=number(w,"windFrom",0.);
            weather.snowfall=std::max(0.,number(w,"snowfall",0.));
            weather.snowDepth=std::max(0.,number(w,"snowDepth",0.));
            weather.code=int(number(w,"code",-1.));
        }
        applyWeather();
    }
    // The measured visibility is the fog (sun_cycle.js). From 24 km up it is
    // clear air: the weather models stop there (24.14 km, fifteen miles), and
    // the photographed horizons of clear days read 150 km and more, so a
    // reading at the models' ceiling is the clear default, not 24 km of haze.
    static constexpr double kVisibilityCeiling=24000.;
    bool applyWeather() {
        if(!sunScript)return false;
        fogFar=farPack.node!=nullptr;
        const double seen=inspectAt?inspectWeather[2]:
            weather.visibility>0&&weather.visibility<kVisibilityCeiling?weather.visibility:0.;
        const double cloud=inspectAt?inspectWeather[0]:weather.cover;
        const double rain=inspectAt?inspectWeather[1]:weather.rain;
        json result;
        const bool accepted=sunScript->callExport("setWeather",json::array({cloud,rain,seen,height(lon,lat)}),result)
            ==saida::ScriptCallStatus::Succeeded&&result.is_boolean()&&result.get<bool>();
        if(inspectAt&&accepted)saida::Log::info("[World inspection] cloud=",cloud," rain_mm_h=",rain," visibility_m=",seen);
        return accepted;
    }
    // Streets and buildings that arrive after the player (a provisional
    // tile cooked again) can land on him: he is moved to the nearest free
    // ground, and told why.
    void keepStanding() {
        checkStanding=false;
        if(!playing||pending||driving||sailing||piloting||swimming||!tile(lon,lat))return;
        // The finer survey (IGN) may have replaced the quick one under him.
        if(jumpOffset<=0)alt=height(lon,lat,alt);
        if(!blocked(lon,lat,alt))return;
        double x,y;
        if(!freeSpot(lon,lat,40.,x,y))return;
        saida::Log::info("[World] a building arrived where the player stood: moved to ",x,", ",y);
        lon=x;lat=y;alt=height(lon,lat);jumpOffset=jumpVelocity=0;
        text("stream-status","Les bâtiments sont arrivés : vous voilà dans la rue.");
    }
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
    saida::Material* snowMaterial(bool doubleSided) {
        const r1::Swatch& s=r1::palette().snow;
        return material(r1::surfaceMaterial(s.name,s.color,s.roughness,std::string("snow"),doubleSided));
    }
    static bool snowable(const std::string& name) {
        const bool ground=name.rfind("Ground \xE2\x80\x94 ",0)==0,roof=name.rfind("Roofs \xE2\x80\x94 ",0)==0;
        if(!ground&&!roof)return false;
        for(const char* keep:{"Water","Snow","Glacier","Frosted","sea ice","Melt pond","Young grey"})
            if(name.find(keep)!=std::string::npos)return false;
        return true;
    }
    void updateSnowCover() {
        const bool want=weather.known&&localConditions()&&weather.snowDepth>=.03;
        for(auto& [key,t]:loaded) {
            if(!t.geography)continue;
            if(t.snowed&&!want) {
                for(auto& [node,own]:t.bare)node->setMaterial(own);
                t.bare.clear();t.snowed=false;t.snowSeen=0;
                continue;
            }
            if(!want)continue;
            if(!t.snowed){t.snowed=true;t.snowSeen=0;}
            const auto& children=t.geography->children();
            for(;t.snowSeen<children.size();++t.snowSeen) {
                auto* mesh=dynamic_cast<saida::MeshNode*>(children[t.snowSeen].get());
                if(!mesh||!snowable(mesh->name())||!mesh->material())continue;
                t.bare.push_back({mesh,mesh->material()});
                mesh->setMaterial(snowMaterial(mesh->material()->desc().doubleSided));
            }
        }
    }
    saida::ParticleSystemNode* emitter(const char* name) {
        auto n=std::make_unique<saida::ParticleSystemNode>();
        n->setName(name);
        n->effectClass=saida::ParticleSystemNode::EffectClass::Snow;
        n->applyEffectPreset();
        return static_cast<saida::ParticleSystemNode*>(engine.sceneTree().world().addChild(std::move(n)));
    }
    void updateSnow(double dt) {
        const bool here=playing&&!pending&&weather.known&&localConditions();
        const bool falling=here&&((weather.code>=71&&weather.code<=77)||weather.code==85||weather.code==86||weather.snowfall>.02);
        const bool lying=onSeaIce(lon,lat)||weather.snowDepth>.05;
        const bool blowing=here&&lying&&weather.windSpeed>6.;
        daylightAge+=dt;
        if((falling||blowing)&&daylightAge>.5&&sunScript) {
            json result;daylightAge=0;
            if(sunScript->callExport("daylight",json::array(),result)==saida::ScriptCallStatus::Succeeded&&result.is_number())
                daylightNow=std::clamp(result.get<double>(),0.,1.);
        }
        // The wind blows towards where it does not come from; x east, z south.
        const double to=(weather.windFrom+180.)*rad;
        const glm::vec3 wind(float(std::sin(to)*weather.windSpeed),0.f,float(-std::cos(to)*weather.windSpeed));
        const glm::vec3 eye=camera->transform().position;
        const float light=float(.04+.9*daylightNow);
        if(falling) {
            if(!snowfall){snowfall=emitter("snowfall");saida::Log::info("[World weather] snow falling, ",weather.snowfall," cm/h");}
            snowfall->setEnabled(true);
            snowfall->maxParticles=2400;snowfall->lifetime=9.f;snowfall->radius=22.f;
            snowfall->spawnRate=float(std::clamp(160.+420.*weather.snowfall,160.,700.));
            snowfall->drag=.35f;
            // Terminal drift = acceleration / drag: the flakes move with the wind.
            snowfall->gravity=wind*snowfall->drag+glm::vec3(0,-.45f,0);
            snowfall->emissive=light;
            snowfall->transform().position=eye+glm::vec3(0,9.f,0)-wind*4.f;
        } else if(snowfall)snowfall->setEnabled(false);
        if(blowing) {
            if(!drift){drift=emitter("drifting snow");saida::Log::info("[World weather] snow drifting, wind ",weather.windSpeed," m/s");}
            drift->setEnabled(true);
            drift->maxParticles=1600;drift->lifetime=1.8f;drift->radius=14.f;
            drift->spawnRate=float(std::clamp((weather.windSpeed-6.)*140.,40.,900.));
            drift->startSize=.035f;drift->startSpeed=.3f;drift->stretch=3.f;drift->drag=1.5f;
            drift->startColor=glm::vec4(.95f,.97f,1.f,.45f);drift->endColor=glm::vec4(.95f,.97f,1.f,0.f);
            drift->gravity=wind*drift->drag+glm::vec3(0,-.04f,0);
            drift->emissive=light;
            const glm::vec3 feet=player->transform().position;
            drift->transform().position=glm::vec3(eye.x,feet.y+.3f,eye.z)-wind*.8f;
        } else if(drift)drift->setEnabled(false);
    }
    // The offset the HUD clock reads: the place's zone when Open-Meteo gave
    // one, otherwise the longitude's whole hours, labelled as an estimate.
    int utcOffset(bool& known) {
        known=localConditions()&&conditions.value("timeSource","")=="zone";
        return known?conditions.value("utcOffsetSeconds",0):int(std::round(lon/15.))*3600;
    }
    std::string localClock() {
        double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
        bool known=false;const int offset=utcOffset(known);
        const std::time_t local=std::time_t(unixSeconds)+offset;
        std::tm parts{};gmtime_s(&parts,&local);
        std::ostringstream out;out<<std::put_time(&parts,"%H:%M");
        return out.str()+(known?" · heure locale":" · fuseau estimé")+(forcedMinutes?" (forcée)":"");
    }
    // "Forcer l'heure à" (Options). The hour is the one the HUD clock shows,
    // so the instant depends on the zone under the player: it is recomputed
    // every frame, and the Sun is told only when it moves -- a new zone, a
    // new local day, or the option itself. With no forced hour the script
    // follows the real clock, which is its own default.
    void applyForcedTime() {
        std::optional<double> wanted;
        if(forcedMinutes) {
            bool known=false;const int offset=utcOffset(known);
            wanted=r1::forcedInstant(double(std::time(nullptr)),offset,*forcedMinutes);
        }
        if(wanted==forcedSent||!findSun())return;
        json result;
        const bool accepted=sunScript->callExport("setForcedTime",json::array({wanted?json(*wanted):json(nullptr)}),result)
                ==saida::ScriptCallStatus::Succeeded&&result.is_boolean()&&result.get<bool>();
        if(!accepted) {
            if(!forcedSaid) {
                saida::Log::error("[World options] scripts/sun_cycle.js refused setForcedTime(",
                                  wanted?number(*wanted,0):std::string("null"),")");
                sayForcedTime("Le Soleil a refusé l'heure forcée : voir game.log.");
                forcedSaid=true;
            }
            return;
        }
        forcedSent=wanted;forcedSaid=false;
    }
    fs::path optionsFile() const {return game/"cache/options.json";}
    // A test run neither reads nor rewrites the player's options: the smoke
    // test and the gallery light the world by the clock or by --at.
    bool testRun() const {return smoke||!worldCapture.pngPath.empty();}
    void loadOptions() {
        std::ifstream input(optionsFile());
        if(!input)return;
        if(testRun()){saida::Log::info("[World options] cache/options.json ignored by a test run");return;}
        try {
            const json options=json::parse(input);
            const std::string typed=options.value("forcedTime",std::string());
            if(typed.empty())return;
            forcedMinutes=r1::parseClockTime(typed);
            if(forcedMinutes)saida::Log::info("[World options] time forced to ",r1::formatClockTime(*forcedMinutes)," local");
            else saida::Log::error("[World options] cache/options.json: forcedTime '",typed,"' is not HH:MM, ignored");
        } catch(const std::exception& e) {
            saida::Log::error("[World options] cache/options.json unreadable, ignored: ",e.what());
        }
    }
    void saveOptions() {
        if(testRun())return;
        json options=json::object();
        if(forcedMinutes)options["forcedTime"]=r1::formatClockTime(*forcedMinutes);
        std::error_code ignored;fs::create_directories(optionsFile().parent_path(),ignored);
        std::ofstream output(optionsFile());
        if(!(output<<options.dump(2)<<"\n")) {
            saida::Log::error("[World options] could not write ",optionsFile().string());
            sayForcedTime("Option appliquée, mais impossible de l'enregistrer : voir game.log.");
        }
    }
    void sayForcedTime(const std::string& problem={}) {
        if(!problem.empty()){text("options-status",problem);return;}
        text("options-status",forcedMinutes
            ?"Partout dans le monde, il est "+r1::formatClockTime(*forcedMinutes)+" à l'heure locale. Le temps ne s'écoule plus."
            :"Heure réelle : chaque lieu est à son heure du moment.");
    }
    void showOptions(bool show) {
        optionsOpen=show;
        style("options-screen","display",show?"block":"none");
        if(!show)return;
        field("forced-time",forcedMinutes?r1::formatClockTime(*forcedMinutes):std::string());
        sayForcedTime();
    }
    void forceTime(std::optional<int> minutes) {
        forcedMinutes=minutes;forcedSaid=false;
        saida::Log::info("[World options] ",minutes?"time forced to "+r1::formatClockTime(*minutes)+" local":std::string("real time"));
        saveOptions();
        sayForcedTime();
        applyForcedTime();
    }
    void applyTypedTime() {
        const std::string typed=value("forced-time");
        if(const auto minutes=r1::parseClockTime(typed)){forceTime(minutes);field("forced-time",r1::formatClockTime(*minutes));return;}
        saida::Log::info("[World options] refused forced time '",typed,"': not HH:MM between 00:00 and 23:59");
        sayForcedTime("Heure invalide « "+typed+" » : écrivez HH:MM, de 00:00 à 23:59 (par exemple 14:00).");
    }
    std::string weatherLabel() {
        if(!localConditions())return "Météo locale indisponible";
        const auto weather=conditions.value("weather",json());
        if(!weather.is_object())return "Météo locale indisponible";
        const int code=weather.value("code",-1);
        const char* state=code==0?"dégagé":code<=3?"nuageux":code<=48?"brouillard"
                          :code<=67?"pluie":code<=77?"neige":code<=82?"averses":"orage";
        return std::string("prévision : ")+state+" · "+number(weather.value("temperature",0.),0)+" °C"
             +" · "+number(weather.value("cloudCover",0.),0)+" % nuages";
    }
    double metresFrom(double x,double y) const {
        const glm::dvec3 d=origin.local(ecef(x,y,0.))-origin.local(ecef(lon,lat,0.));
        return std::hypot(d.x,d.z);
    }
    saida::Node* hullPrototype(const std::string& model,const json& doc) {
        auto& p=hullPrototypes[model];
        if(!p&&doc.is_object()) {
            try {p=prototypes->addChild(plantNode(doc));}
            catch(const std::exception& e){saida::Log::warn("[World sea] hull ",model," failed: ",e.what());}
        }
        return p;
    }
    void readSea() {
        auto latest=sea->latest();
        if(!latest||latest==seaDoc)return;
        seaDoc=latest;
        const json& doc=*latest;
        const json source=doc.value("source",json::object());
        const json hulls=doc.value("hulls",json::object());
        std::set<std::string> have;
        for(auto& s:seaShips)have.insert(s.id);
        size_t added=0;
        for(const auto& s:doc.value("ships",json::array())) {
            if(seaShips.size()>=kSeaShips)break;
            const std::string id=s.value("id","");
            if(id.empty()||have.count(id)||seaTaken.count(id))continue;
            const double x=s.value("lon",0.),y=s.value("lat",0.);
            if(!seaFirst&&metresFrom(x,y)<kSeaAppear)continue;
            if(tile(x,y)&&!navigable(x,y))continue;       // the cell's land, where the map knows it
            const std::string model=s.value("model","");
            auto* proto=hullPrototype(model,hulls.value(model,json()));
            if(!proto)continue;
            SeaShip ship;ship.id=id;
            ship.phase=double(std::hash<std::string>{}(id)%1000)*.0063;
            Vessel& v=ship.v;
            v.node=engine.sceneTree().world().addChild(clonePlant(*proto));
            v.kind=s.value("kind","commercial");v.length=s.value("length",20.);v.beam=s.value("beam",5.);
            v.top=s.value("top",8.);v.accel=s.value("accel",.5);v.turn=s.value("turn",10.);
            v.lon=x;v.lat=y;v.yaw=wrap(s.value("heading",0.));v.speed=s.value("speed",0.);
            v.alt=tile(x,y)?waterLevel(x,y):0.;
            if(std::getenv("R1WORLD_SEA_DEBUG")) {
                const glm::dvec3 at=origin.local(ecef(v.lon,v.lat,v.alt));
                saida::Log::info("[World sea] ship ",id," ",model," at ",at.x,",",at.y,",",at.z," alt ",v.alt);
            }
            seaShips.push_back(std::move(ship));++added;
        }
        seaFirst=false;
        const bool online=source.value("online",false);
        const std::string weather=source.value("weather",std::string("?"));
        const std::string synced=source.contains("synced")&&source["synced"].is_string()
            ?source["synced"].get<std::string>():std::string("never");
        const std::string said=weather+(online?" online":" offline");
        if(said!=seaSaid||added) {
            saida::Log::info("[World sea] ",seaShips.size()," ships (+",added,"), weather ",weather,", ",
                             online?"online":"offline",", last synced ",synced,
                             ", live ",source.value("live",0));
            seaSaid=said;
        }
    }
    void updateSea(double dt) {
        seaAsk+=dt;seaRead+=dt;
        if(seaAsk>5.) {
            seaAsk=0;double t=0;
            if(gameTime(t)){sea->ask(lon,lat,t);sky->ask(lon,lat);}
        }
        if(seaRead>1.){seaRead=0;readSea();}
        for(size_t i=0;i<seaShips.size();) {
            Vessel& v=seaShips[i].v;
            if(v.speed>0) {
                // Look a hull length and a half ahead; where the map says land,
                // come round to the first open bearing, stopping if there is none.
                auto clear=[&](double heading){
                    const double s=std::sin(heading*rad),c=std::cos(heading*rad);
                    const double lead=v.length*.75+v.speed*8.;
                    auto bow=advance(v.lon,v.lat,s*lead,c*lead);
                    return !tile(bow.x,bow.y)||navigable(bow.x,bow.y);
                };
                if(!clear(v.yaw)) {
                    double turn=0;
                    for(double d:{25.,-25.,50.,-50.,90.,-90.,135.,-135.,180.})
                        if(clear(v.yaw+d)){turn=d;break;}
                    if(turn==0)v.speed=0;
                    else v.yaw=wrap(v.yaw+std::clamp(turn,-v.turn*dt,v.turn*dt));
                }
                const double s=std::sin(v.yaw*rad),c=std::cos(v.yaw*rad);
                const auto next=advance(v.lon,v.lat,s*v.speed*dt,c*v.speed*dt);
                if(tile(next.x,next.y)&&!navigable(next.x,next.y))v.speed=0;
                else {
                    v.lon=next.x;v.lat=next.y;
                    v.alt=tile(v.lon,v.lat)?waterLevel(v.lon,v.lat):0.;
                }
            }
            if(metresFrom(v.lon,v.lat)>kSeaKeep) {
                if(v.node)v.node->queueFree();
                seaShips.erase(seaShips.begin()+long(i));
                continue;
            }
            ++i;
        }
    }

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
    void loadPaints() {
        // Albedos, from the Atlas (assets/world/atlas.json), where a test holds
        // them to rule 2 -- a colour the tests cannot see is a rule not held.
        for(const auto& albedo:r1::palette().carPaints)paints.push_back(glm::vec3(albedo[0],albedo[1],albedo[2]));
        if(paints.empty()){
            saida::Log::warn("[World traffic] the Atlas lists no car paint; the fleet will be one colour");
            paints.push_back(glm::vec3(.30f,.30f,.31f));
        }
    }
    // Disabled scene ownership keeps shared prototype resources resident.
    const VehicleModel& vehicleSpec(const saida::Node& node) const {
        for(const auto& spec:fleet)if(node.name()=="vehicle-"+spec.name)return spec;
        return fleet.front(); // The entry scene starts with the city car.
    }
    static void vehicleLod(saida::Node& node) {
        auto* lod=node.addBehaviour<saida::LODGroupBehaviour>();
        // Full source detail is for close cars. Switch by projected size,
        // keeping long vehicles detailed farther away, with engine hysteresis.
        lod->setLevels({{"Near",.16f},{"Far",0.f}});
    }
    void buildTrafficPrototype() {
        std::ifstream input(game/"assets/models/vehicles/fleet.json");
        if(!input)throw std::runtime_error("Missing road vehicle fleet manifest");
        json doc;input>>doc;
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
        car->setName("vehicle-city");vehicleLod(*car);
        saida::Log::info("[World traffic] fleet=",fleet.size()," shared vertices=",doc.at("totalVertices").get<size_t>());
    }
    size_t vehicleKind(uint32_t seed,const saida::traffic::Lane& lane) const {
        // Common cars dominate. Long vehicles start on faster through roads.
        const uint32_t roll=(seed^(seed>>16))%100;
        std::string name=roll<34?"city":roll<55?"sedan":roll<74?"suv":roll<86?"offroad":roll<94?"sport":roll<98?"truck":"bus";
        if((name=="truck"||name=="bus")&&lane.speed<11.f)name="city";
        for(size_t i=0;i<fleet.size();++i)if(fleet[i].name==name)return i;
        return 0;
    }
    void readGraph(Loaded& tile) {
        auto found=tile.data.find("traffic");
        if(found==tile.data.end())return;   // cooked before traffic existed
        const auto& doc=*found;
        for(auto& node:doc.at("nodes")) {
            tile.graph.nodes.push_back({float(node[0]),float(node[2])});
            tile.groundUp.push_back(float(node[1]));
        }
        for(auto& lane:doc.at("lanes"))
            tile.graph.lanes.push_back({uint32_t(lane[0]),uint32_t(lane[1]),
                                        float(lane[2]),float(lane[3])});
        if(tile.graph.lanes.empty())return;
        tile.graph.build();
        saida::traffic::Rules rules;
        rules.leftHand=doc.value("leftHand",false);
        // The despawn radius is the streamer's, not a taste: a car further
        // than the tile ring is a car in a tile that is about to be evicted.
        rules.despawn=260.f;
        // The simulator measures centre-to-centre gaps. Reserve a bus length
        // plus clearance so two long vehicles cannot overlap in a queue.
        rules.minGap=12.5f;
        rules.junctionGuard=14.f;
        // Deterministic per tile, so the same street is the same street on
        // every visit and a capture can be compared with itself.
        tile.flow.reset(&tile.graph,rules,uint32_t(std::hash<std::string>{}(tile.data.at("key").get<std::string>()))|1u);
        tile.wanted=doc.value("cars",0);
    }
    glm::vec3 paintFor(uint32_t seed) const {
        return paints[seed%paints.size()];
    }
    void paintCar(saida::Node& node,glm::vec3 albedo) {
        node.traverse([&](saida::Node& n,const glm::mat4&){
            if(!n.material()||n.name().rfind("paint-",0)!=0)return;
            auto desc=n.material()->desc();
            desc.baseColor=glm::vec4(albedo,desc.baseColor.a);
            if(auto* mesh=dynamic_cast<saida::MeshNode*>(&n))
                mesh->setMaterial(engine.resources().getMaterial(desc));
        });
    }
    // The camera's position in one tile's tangent frame. It is the observer the
    // traffic spawns around, because it is the thing that sees.
    glm::dvec3 cameraTileLocal(const Loaded& tile) const {
        const glm::vec3 p=camera->transform().position;
        return tile.frame.local(origin.origin+origin.basis*glm::dvec3(p.x,p.y,p.z));
    }
    void updateTraffic(float delta) {
        if(!debugTraffic||fleet.empty())return;
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
            const size_t share=std::min<size_t>(l.wanted,remaining);
            remaining-=share;
            l.flow.setPopulation(uint32_t(share));
            // The observer is the camera, not the player. It stands eight and a
            // half metres behind him in a car, so "sixteen metres behind the
            // player" was eight metres behind the *camera* -- in shot, which is
            // half of why cars appeared in view.
            const glm::dvec3 eye=cameraTileLocal(l);
            saida::traffic::Obstacle player;
            if(driving) {
                const glm::dvec3 p=l.frame.local(ecef(carLon,carLat,carAlt));
                player.position={float(p.x),float(p.z)};
                player.radius=float(vehicleSpec(*car).length*.5+.4);
                player.active=true;
            }
            // Which way the camera is looking, in this tile's frame, so cars
            // may appear close behind it and never close in front.
            const glm::vec3 ahead=camera->transform().rotation*glm::vec3(0,0,-1);
            const glm::dmat3 intoTile=glm::transpose(l.frame.basis)*origin.basis;
            const glm::dvec3 look=intoTile*glm::dvec3(ahead.x,ahead.y,ahead.z);
            const double flat=std::hypot(look.x,look.z);
            const saida::traffic::Vec2 facing=flat>1e-6
                ?saida::traffic::Vec2{float(look.x/flat),float(look.z/flat)}
                :saida::traffic::Vec2{0.f,0.f};
            l.flow.update(std::min(.05f,delta),{float(eye.x),float(eye.z)},facing,player);
            syncTrafficNodes(l,std::min(.05f,delta));
        }
    }
    bool trafficSlotLive(const Loaded& tile,size_t slot) const {
        const auto& agents=tile.flow.agents();
        return debugTraffic&&slot<agents.size()&&agents[slot].alive;
    }
    // Reuse each slot's graph; dormant slots retain resources without rendering.
    void syncTrafficNodes(Loaded& tile,float delta) {
        const auto& agents=tile.flow.agents();
        if(tile.cars.size()<agents.size()) {
            tile.cars.resize(agents.size(),nullptr);
            tile.carKinds.resize(agents.size(),0);
            tile.carWheels.resize(agents.size());
            tile.carSpin.resize(agents.size(),0.);
        }
        for(size_t i=0;i<tile.cars.size();++i) {
            const bool live=trafficSlotLive(tile,i);
            if(!tile.cars[i]) {
                if(!live)continue;                 // no node until a slot is used
                const auto& agent=agents[i];
                tile.carKinds[i]=vehicleKind(agent.seed,tile.graph.lanes[agent.lane]);
                auto node=clonePlant(*fleet[tile.carKinds[i]].prototype);
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
            // Agent seeds advance at junctions. Keep model and paint stable
            // for the pooled node instead of changing a car in full view.
            tile.carSpin[i]+=agent.speed*delta/fleet[tile.carKinds[i]].wheelRadius;
            for(auto* wheel:tile.carWheels[i])wheel->transform().rotation=
                glm::angleAxis(float(-tile.carSpin[i]),glm::vec3(1,0,0));
            const auto pose=tile.flow.pose(agent);
            const auto& lane=tile.graph.lanes[agent.lane];
            const float span=tile.graph.laneLength(agent.lane);
            const float t=span>0.f?agent.s/span:0.f;
            const float up=tile.groundUp[lane.from]*(1.f-t)+tile.groundUp[lane.to]*t;
            const auto dir=tile.graph.direction(agent.lane);
            // Engine z points south, so north is -z and the bearing the rest of
            // this file speaks is atan2(east, north).
            const double bearing=std::atan2(double(dir.x),-double(dir.y));
            tile.cars[i]->transform().position=glm::vec3(pose.position.x,up+.06f,pose.position.y);
            tile.cars[i]->transform().rotation=glm::angleAxis(float(-bearing),glm::vec3(0,1,0));
        }
    }
    size_t trafficWanted() const {
        size_t n=0;
        for(const auto& [key,tile]:loaded)n+=tile.wanted;
        return n;
    }
    size_t trafficLive() const {
        size_t n=0;
        for(const auto& [key,tile]:loaded)n+=tile.flow.live();
        return n;
    }

    // A photographed surface's texture, registered once per path and colour space.
    saida::AssetID texture(const std::string& path,bool srgb) {
        if(path.empty())return saida::kAssetInvalid;
        auto [it,inserted]=textures.try_emplace(path+(srgb?"#srgb":"#linear"),saida::kAssetInvalid);
        if(inserted)it->second=engine.resources().getOrRegister((game/path).string(),saida::AssetType::Texture,srgb);
        return it->second;
    }
    saida::Material* material(const r1::Material& m) {
        saida::MaterialDesc d;
        d.baseColor=glm::vec4(m.color[0],m.color[1],m.color[2],m.color[3]);
        d.metallic=float(m.metallic);d.roughness=float(m.roughness);d.doubleSided=m.doubleSided;
        d.variation={float(m.variation[0]),float(m.variation[1]),float(m.variation[2]),float(m.variation[3])};
        d.normalStrength=float(m.normalStrength);
        d.heightId=texture(m.heightTexture,false);d.parallaxDepth=float(m.parallaxDepth);
        d.environmentReflection=float(m.environmentReflection);
        d.albedoId=texture(m.baseColorTexture,true);
        d.normalId=texture(m.normalTexture,false);
        d.metallicRoughnessId=texture(m.metallicRoughnessTexture,false);
        return engine.resources().getMaterial(d);
    }
    // The tile's node, empty but for its sea when it is all ocean: its own
    // ground, streets, works and buildings are uploaded a few parts a frame
    // by `uploadParts`, straight from what the worker prepared.
    std::unique_ptr<saida::Node> buildTile(const std::string& key,const r1::ServedTile& served) {
        auto root=std::make_unique<saida::Node>(key);
        const r1::CookedTile& cooked=served.cooked;
        if(!cooked.ocean.is_null()) {
            // Nothing is drawn under the open ocean: water that lets the
            // background through would show the clear colour, not a sea bed.
            auto ocean=cooked.ocean;ocean["transparency"]=0.0;
            auto sea=saida::SceneSerializer::nodeFromJson(ocean.dump(),engine.resources());
            if(!sea)throw std::runtime_error("sea node refused by the scene loader");
            sea->setEnabled(debugWater);
            root->addChild(std::move(sea));
        } else root->createChild<saida::Node>("Geography");
        if(!cooked.grass.empty()) {
            auto grass=grassNode(cooked);grass->setEnabled(debugGrass);root->addChild(std::move(grass));
        }
        return root;
    }
    // The player and the car push the grass aside where they pass.
    static constexpr float kWalkerBend=0.45f,kCarBend=1.6f;
    void bendGrass() {
        if(!debugGrass)return;
        const auto at=[](saida::Node* n){return glm::vec3(n->worldTransform()[3]);};
        const bool walking=player&&player->isActiveInHierarchy();
        const bool driving=car&&car->isActiveInHierarchy();
        for(auto& [key,t]:loaded) {
            if(!t.grass)continue;
            const glm::mat4 toLocal=glm::inverse(t.grass->worldTransform());
            auto bender=[&](saida::Node* n,float radius){
                return glm::vec4(glm::vec3(toLocal*glm::vec4(at(n),1.f)),radius);};
            t.grass->benders[0]=walking?bender(player,kWalkerBend):glm::vec4(0.f);
            t.grass->benders[1]=driving?bender(car,kCarBend):glm::vec4(0.f);
        }
    }
    // The tile's grass blades (gen/grass), on its ground as drawn, in the
    // tile's own frame: the engine makes them around the camera.
    static std::unique_ptr<saida::GrassNode> grassNode(const r1::CookedTile& cooked) {
        const r1::GrassCover& g=cooked.grass;
        auto node=std::make_unique<saida::GrassNode>();
        saida::GrassNode::Field f;
        f.groundSamples=g.groundSamples;f.heights=g.heights;
        for(const auto& p:g.southBoundary)f.southBoundary.push_back({p[0],p[1]});
        for(const auto& p:g.northBoundary)f.northBoundary.push_back({p[0],p[1]});
        const auto& m=g.uvFromEngine;
        f.uvFromLocal=glm::mat3(glm::vec3(float(m[0]),float(m[3]),0.f),glm::vec3(float(m[1]),float(m[4]),0.f),
                                glm::vec3(float(m[2]),float(m[5]),1.f));
        f.coverSize=g.coverSize;f.cover=g.cover;
        // The ground's texture coordinates are (x, -z) in repeats (gen/terrain):
        // the blades carry its light and dark patches past where they end.
        const float repeat=float(cooked.grassUvScale);
        f.materialUvFromLocal=glm::mat3(glm::vec3(repeat,0.f,0.f),glm::vec3(0.f,-repeat,0.f),glm::vec3(0.f,0.f,1.f));
        const auto& v=cooked.grassVariation;
        f.variation=glm::vec4(float(v[0]),float(v[1]),float(v[2]),float(v[3]));
        if(!node->setField(std::move(f)))throw std::runtime_error("grass field refused by the engine");
        return node;
    }
    // A mount used to upload a whole tile in one frame -- 30 to 60 ms, the
    // hitch every new tile was felt as. Now the ground goes first, a few
    // milliseconds of parts a frame, and the rest follows over the next frames.
    // Physics follows each uploaded mesh; arrivals wait for its first sync.
    void uploadParts() {
        const auto start=std::chrono::steady_clock::now();
        const auto priority=orderedNearby((pending||warming)?pickLon:lon,(pending||warming)?pickLat:lat);
        // A tile's replacement uploads right after it: the old one is shown
        // until then, so it is the one standing in for it on screen.
        std::vector<std::pair<const std::string*,Loaded*>> order;
        for(const auto tile:priority)for(auto* tiles:{&loaded,&incoming}) {
            auto found=tiles->find(tile.key());
            if(found!=tiles->end())order.push_back({&found->first,&found->second});
        }
        for(auto [name,tile]:order) {
            const auto& key=*name;auto& t=*tile;
            if(!t.geography)continue;
            const auto& parts=std::any_cast<const PreparedTile&>(t.served->prepared).parts;
            while(t.nextPart<parts.size()) {
                const PartUpload& part=parts[t.nextPart++];
                auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(part.prepared));
                if(!mesh) {
                    // Said, never silent: a tile missing a part looks like OSM missing it.
                    saida::Log::warn("[World streaming] the geometry arena refused ",part.name," of ",key);
                    continue;
                }
                t.partMeshes.push_back(mesh);
                meshCollider(*t.geography,std::make_unique<saida::MeshNode>(part.name,mesh,
                    material(t.served->cooked.parts[part.material].material)));
                if(msSince(start)>=kPartUploadMs)return;
            }
        }
    }
    void placeTiles() {
        for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles) {
            // Keep the precise destination: composing a translation from the
            // old million-metre float pose loses centimetres at arrival.
            const auto position=glm::vec3(origin.local(t.frame.origin));
            const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*t.frame.basis));
            engine.sceneTree().world().rebaseSubtreeTo(*t.node,position,rotation);
        }
        for(auto& f:farLandmarks)if(f.node)placeFar(f);
        placeFarRelief();
        placeFarPack();
    }
    void trim() {
        engine.sceneTree().applyDeferred();
        engine.resources().trimUnused(engine.sceneTree().world().resourceUsage());
    }
    // Only two nearby interiors occupy the shared arena. A room containing
    // the player is pinned until they leave; hysteresis prevents door thrashing.
    void updateInteriors(double dt) {
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
                if(ready) {
                    room.ready=true;room.node->setEnabled(true);
                    saida::Log::info("[World interiors] ready ",room.plan.name);
                } else if(engine.resources().assetLoadsSettled()) {
                    room.node->queueFree();room.node=nullptr;room.leaves[0]=room.leaves[1]=nullptr;
                    room.refused=true;released=true;
                    saida::Log::error("[World interiors] geometry upload failed: ",room.plan.name);
                    text("stream-status","Impossible de charger cet intérieur.");
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
            }
            if(room.node)++active;
            if(!t.reducedDensity&&!room.node&&!room.refused&&distance<kInteriorLoad)candidates.push_back({&t,&room,distance});
            if(room.node&&room.ready) {
                auto p=room.plan.local(at);
                // Sensor on both sides, with a hold time and a wide safety zone.
                const bool sensor=!driving&&!sailing&&!piloting&&std::abs(p.x)<room.plan.width/2+1.5&&std::abs(p.y)<3.5;
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
            }
        }
        if(active>=kInteriorRooms||candidates.empty())return;
        auto& room=*candidates.front().room;auto& t=*candidates.front().tile;const auto& p=room.plan;
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
            text("stream-status","Intérieur indisponible : budget de géométrie atteint.");return;
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
            saida::Log::info("[World interiors] queued ",p.name," recipe=",p.recipe," vertices=",vertices,
                " furniture_instances=",room.layout.fixtures.size()," prototypes=",furnishings.size());
        } catch(const std::exception& e) {
            root.reset();trim();
            room.refused=true;room.leaves[0]=room.leaves[1]=nullptr;
            saida::Log::error("[World interiors] ",p.name,": ",e.what());
            text("stream-status","Impossible de charger cet intérieur.");
        }
    }
    int retailTestPhase=0;int64_t retailTestId=0;double retailTestTime=0,retailTestV=-3;const char* retailTestWait="";
    bool retailTest() const { return smoke&&(std::getenv("R1WORLD_RETAIL_SMOKE")||std::getenv("R1WORLD_INTERIOR_SMOKE")); }
    // R1WORLD_RETAIL_SHOT=<png> with R1WORLD_RETAIL_SHOT_AT=outside|inside|
    // anchor photographs the store the traversal chose, facing into it (or,
    // for "anchor", from behind a mall's checkouts toward its supermarket),
    // then ends the run: the test says the door opens, only a picture says
    // it looks like a shop (rule 1 of CLAUDE.md).
    bool retailShot(const char* moment,const r1::InteriorPlan& plan,r1::P2 facing={0,0}) {
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
    void runRetailTest(double dt) {
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
                const auto bytes=r1::writeGlb(owner->served->cooked.parts);
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
    std::unique_ptr<saida::Node> clonePlant(saida::Node& source) {
        std::unique_ptr<saida::Node> out;
        if(source.mesh()){
            auto mesh=std::make_unique<saida::MeshNode>(source.name(),source.mesh(),source.material());
            if(auto* original=dynamic_cast<saida::MeshNode*>(&source))mesh->castShadows()=original->castShadows();
            out=std::move(mesh);
        }
        else out=std::make_unique<saida::Node>(source.name());
        out->transform()=source.transform();
        for(auto& child:source.children())out->addChild(clonePlant(*child));
        return out;
    }
    // A clone of a prototype with its empty nodes removed: every mesh it holds
    // comes back as one node carrying the transform it would have inherited.
    //
    // Uniform scale is required to compose nested TRS without introducing shear.
    static bool uniformScale(const glm::vec3& s) {
        return std::abs(s.x-s.y)<1e-6f&&std::abs(s.y-s.z)<1e-6f;
    }
    bool flattenable(saida::Node& node) const {
        if(!uniformScale(node.transform().scale))return false;
        for(auto& child:node.children())if(!flattenable(*child))return false;
        return true;
    }
    void collectMeshes(saida::Node& source,const saida::Transform& above,
                       std::vector<std::unique_ptr<saida::Node>>& out) {
        saida::Transform here;
        here.rotation=above.rotation*source.transform().rotation;
        here.scale=above.scale*source.transform().scale;
        here.position=above.position+above.rotation*(above.scale*source.transform().position);
        if(source.mesh()) {
            auto mesh=std::make_unique<saida::MeshNode>(source.name(),source.mesh(),source.material());
            if(auto* original=dynamic_cast<saida::MeshNode*>(&source))mesh->castShadows()=original->castShadows();
            mesh->transform()=here;
            out.push_back(std::move(mesh));
        }
        for(auto& child:source.children())collectMeshes(*child,here,out);
    }
    // Each shared model parsed once: instances share its mesh and material
    // pointers, avoiding thousands of repeated GLB and texture decodes.
    std::map<std::string,saida::AssetHandle> prototypeLoads;
    saida::Node* prototype(const std::string& path) {
        auto& found=naturePrototypes[path];
        if(!found) {
            auto& handle=prototypeLoads[path];
            if(!handle)handle=saida::GLTFLoader::request((game/path).string(),engine.resources());
            if(!handle||handle.failed())throw std::runtime_error("Model load failed: "+path+" "+handle.error());
            if(!handle.ready())return nullptr;
            auto imported=std::make_unique<saida::Node>(fs::path(path).stem().string());
            if(!saida::GLTFLoader::instantiate(handle,*imported,engine.resources()))
                throw std::runtime_error("Model instantiation failed: "+path);
            if(path=="assets/models/external/nature_selected/urban_tree.glb"
               ||path=="assets/models/external/nature_selected/urban_tree_mid.glb") {
                // Keep the scanned leaf colours and gloss map, but give dry
                // foliage a softer lobe, including its bright grazing angles.
                // Applied once to shared prototypes, never per tree or frame.
                constexpr float kDryLeafSpecularStrength=.25f;
                imported->traverse([&](saida::Node& n,const glm::mat4&){
                    auto* mesh=dynamic_cast<saida::MeshNode*>(&n);
                    if(!mesh||!mesh->material())return;
                    auto desc=mesh->material()->desc();
                    if(desc.alphaCutoff<=0.f)return; // Bark keeps its authored material.
                    desc.specularStrength*=kDryLeafSpecularStrength;
                    mesh->setMaterial(engine.resources().getMaterial(desc));
                });
            }
            found=prototypes->addChild(std::move(imported));
            prototypeLoads.erase(path);
        }
        return found;
    }
    bool modelsReady(const json& doc) {
        bool ready=true;
        const auto path=doc.value("importedFrom",std::string());
        if(!path.empty()&&!prototype(path))ready=false;
        for(const auto& child:doc.value("children",json::array()))if(!modelsReady(child))ready=false;
        return ready;
    }
    saida::Node* prototypeForStartup(const std::string& path) {
        // Before the frame loop only: CPU decoding still uses the worker,
        // while GPU publication waits for the normal resource pump.
        for(;;) {
            if(auto* node=prototype(path))return node;
            engine.resources().assetLoader().pump();
            std::this_thread::yield();
        }
    }
    // Warm shared models through the engine worker while the map is on screen.
    std::vector<std::string> warmList;
    void warm() {
        if(warmList.empty())return;
        const std::string path=warmList.back();
        if(prototype(path))warmList.pop_back();
    }
    std::unique_ptr<saida::Node> plantNode(json doc) {
        auto path=doc.value("importedFrom",std::string());
        auto children=doc.value("children",json::array());
        doc.erase("importedFrom");doc.erase("children");
        auto node=saida::SceneSerializer::nodeFromJson(doc.dump(),engine.resources());
        if(!path.empty()){
            saida::Node* prototype=this->prototype(path);
            if(!prototype)throw std::runtime_error("Nature prototype failed: "+path);
            if(flattenable(*prototype)) {
                std::vector<std::unique_ptr<saida::Node>> meshes;
                collectMeshes(*prototype,saida::Transform{},meshes);
                for(auto& mesh:meshes)node->addChild(std::move(mesh));
            } else node->addChild(clonePlant(*prototype));
        }
        for(auto& child:children)node->addChild(plantNode(child));
        // A holder whose whole content is one mesh becomes that mesh. The
        // tile authors a plant as root -> {Near, Far} -> model, and after
        // flattening each holder has exactly one child and nothing else of its
        // own: five nodes to say "this tree has two levels of detail". The
        // holder's name is what the LOD switch looks for, so the mesh takes it.
        //
        // The top-level plant keeps its own node: it carries the groups the
        // rest of the file finds it by, and `blocked` reads its position.
        if(node->children().size()==1&&node->behaviours().empty()
           &&!node->isInGroup("vegetation")&&node->children()[0]->mesh()
           &&node->children()[0]->children().empty()
           &&uniformScale(node->transform().scale)
           &&uniformScale(node->children()[0]->transform().scale)) {
            auto only=node->detachChild(node->children()[0].get());
            const saida::Transform above=node->transform(),below=only->transform();
            saida::Transform merged;
            merged.rotation=above.rotation*below.rotation;
            merged.scale=above.scale*below.scale;
            merged.position=above.position+above.rotation*(above.scale*below.position);
            only->transform()=merged;
            only->setName(node->name());
            only->setEnabled(node->enabled());
            return only;
        }
        if(node->isInGroup("vegetation") && node->findByPath("Near") && node->findByPath("Far")) {
            auto* lod = node->addBehaviour<saida::LODGroupBehaviour>();
            // Coverage is the bounding sphere's projected diameter, squared
            // (MeshLod.cpp). Mid ends at 9%: roughly 80 m for a nine-metre
            // tree at 60 degrees. Beyond it the Mid-derived view atlas retains
            // its silhouette with one camera-facing quad. Full geometry is for
            // within kTreeNearRadius (`updateNature`), which cover far more
            // than 10% even when small.
            if(node->findByPath("Mid"))lod->setLevels({{"Near", .1f}, {"Mid", .09f}, {"Far", 0.f}});
            else lod->setLevels({{"Near", .06f}, {"Far", 0.f}});
            // A level dissolves into the next over half a second instead of
            // swapping in one frame (Saida's screen-door cross-fade).
            lod->setCrossFade(.5f);
        }
        return node;
    }
    // `reach` is §5's detail band, 1 on foot and less at the wheel: the
    // radius a plant is drawn in shrinks with the speed it is passed at, which
    // is the cheapest of the plan's levers and the only one that costs nothing
    // when standing still.
    void updateNature(double reach) {
        for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles){
            const auto pos=t.frame.local(ecef(lon,lat,alt));
            for(Plant& plant:t.vegetation){
                if(plant.tree&&!debugTrees)continue;
                const auto p=plant.node->transform().position;
                double d=std::hypot(p.x-pos.x,p.z-pos.z);
                plant.node->setVisible(d<(plant.grass?65:550)*reach);
                if(plant.lod) {
                    // Entered inside the radius, left a tenth beyond it, so
                    // a tree at the edge does not flicker between two models.
                    plant.close=d<kTreeNearRadius*(plant.close?1.1:1.);
                    plant.lod->setFinestLevel(plant.close?0:1);
                }
            }
        }
    }
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
    void readFarLandmarks() {
        for(const auto& l:r1::landmarks()) {
            FarLandmark f;
            f.slug=l.slug;f.tile=r1::tileAt(l.lon,l.lat).key();
            f.lon=l.lon;f.lat=l.lat;f.alt=l.groundAlt;
            for(size_t i=1;i<l.levels.size();++i)f.levels.push_back({l.levels[i].path,l.levels[i].until,l.levels[i].vertices});
            farLandmarks.push_back(std::move(f));
        }
        saida::Log::info("[World landmarks] ",farLandmarks.size()," landmarks visible from afar, to ",
                         r1::landmarkFarRange()," m");
    }
    bool nearModel(FarLandmark& f) {
        auto it=loaded.find(f.tile);
        if(it==loaded.end())return false;
        auto& t=it->second;
        if(f.seenTile!=t.node) {
            f.seenTile=t.node;f.nearIndex=-1;
            const std::string name="landmark "+f.slug;
            for(size_t i=0;i<t.props.size();++i)
                if(t.props[i].value("name",std::string())==name){f.nearIndex=long(i);break;}
        }
        // A tile cooked before its landmark carries none: keep the far model.
        return f.nearIndex>=0&&long(t.nextProp)>f.nearIndex;
    }
    void placeFar(FarLandmark& f) {
        const auto position=glm::vec3(origin.local(ecef(f.lon,f.lat,f.alt)));
        const Frame own(f.lon,f.lat,f.alt);
        const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
        engine.sceneTree().world().rebaseSubtreeTo(*f.node,position,rotation);
    }
    size_t farVertices() const {
        size_t n=0;
        for(const auto& f:farLandmarks)if(f.node&&f.shown>=0)n+=f.levels[size_t(f.shown)].vertices;
        return n;
    }
    void updateFar() {
        for(auto& f:farLandmarks) {
            const glm::dvec3 here=origin.local(ecef(f.lon,f.lat,f.alt));
            const double distance=std::hypot(here.x,here.z);
            int want=-1;
            if(playing&&!nearModel(f))
                for(size_t i=0;i<f.levels.size();++i)if(distance<=f.levels[i].until){want=int(i);break;}
            if(want==f.shown)continue;
            if(want<0) {
                if(f.node){f.node->queueFree();f.node=nullptr;}
                f.shown=-1;f.requested=-1;f.loading.reset();continue;
            }
            const auto& level=f.levels[size_t(want)];
            if(f.requested!=want) {
                f.loading=saida::GLTFLoader::request((game/level.path).string(),engine.resources());
                f.requested=want;
            }
            if(f.loading&&!f.loading.failed()&&!f.loading.ready())continue;
            auto n=std::make_unique<saida::Node>("far "+f.slug);
            if(!f.loading||f.loading.failed()||!saida::GLTFLoader::instantiate(f.loading,*n,engine.resources())) {
                // Keeps `shown`, so a model that will not load is reported
                // once per distance band rather than once a frame.
                saida::Log::warn("[World landmarks] ",f.slug," level ",want+1," failed to load: ",level.path," ",f.loading.error());
                if(f.node){f.node->queueFree();f.node=nullptr;}
                f.shown=want;f.loading.reset();
                if(smoke){testFailed=true;engine.sceneTree().quit();}
                continue;
            }
            if(f.node)f.node->queueFree();
            f.shown=want;
            f.node=engine.sceneTree().world().addChild(std::move(n));
            placeFar(f);
            saida::Log::info("[World landmarks] ",f.slug," level ",want+1," at ",int(distance)," m");
        }
    }
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
    void placeFarPack() {
      for(auto* pack:{&farPack,&nextFarPack}) {
        if(!pack->node)continue;
        const auto position=glm::vec3(origin.local(ecef(pack->lon,pack->lat,0.)));
        const Frame own(pack->lon,pack->lat,0.);
        const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
        engine.sceneTree().world().rebaseSubtreeTo(*pack->node,position,rotation);
      }
    }
    void updateFarPack() {
        Loaded* here=playing&&!pending?tile(lon,lat):nullptr;
        const auto ice=here&&here->seaIce?here->served->cooked.seaIce:nullptr;
        if(!ice) {
            if(nextFarPack.node){nextFarPack.node->queueFree();nextFarPack=FarPack{};}
            if(farPack.node){farPack.node->queueFree();farPack=FarPack{};saida::Log::info("[World ice] far pack taken down");}
            return;
        }
        if(nextFarPack.node) {
            if(nextFarPack.ice!=ice||metresBetween(lon,lat,nextFarPack.lon,nextFarPack.lat)>=2000.) {
                nextFarPack.node->queueFree();nextFarPack=FarPack{};
            } else {
                if(nextFarPack.refused)return;
                bool ready=true;
                nextFarPack.node->traverse([&](saida::Node& node,const glm::mat4&) {
                    if(auto* mesh=node.mesh())ready=ready&&mesh->loaded();
                });
                if(!ready) {
                    if(engine.resources().assetLoadsSettled()) {
                        nextFarPack.refused=true;
                        saida::Log::error("[World ice] far pack geometry upload failed");
                        text("stream-status","Banquise lointaine indisponible : transfert de géométrie refusé.");
                        if(smoke){testFailed=true;engine.sceneTree().quit();}
                    }
                    return;
                }
                if(farPack.node)farPack.node->queueFree();
                farPack=nextFarPack;nextFarPack=FarPack{};farPack.node->setVisible(true);
            }
        }
        if(farPack.node&&farPack.ice==ice&&metresBetween(lon,lat,farPack.lon,farPack.lat)<2000.)return;
        const auto started=std::chrono::steady_clock::now();
        const auto parts=r1::buildFarPack(*ice,lon,lat);
        auto root=std::make_unique<saida::Node>("far pack");
        size_t vertices=0;
        for(size_t i=0;i<parts.size();++i) {
            PartUpload up=uploadOf(parts[i],i);
            const auto count=up.vertices.size();
            auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(
                saida::prepareMesh({std::move(up.vertices),std::move(up.indices)})));
            if(!mesh){saida::Log::warn("[World ice] the geometry arena refused ",parts[i].name);continue;}
            root->addChild(std::make_unique<saida::MeshNode>(parts[i].name,mesh,material(parts[i].material)));
            vertices+=count;
        }
        // The old pack remains visible and owned until all replacement meshes
        // are draw-ready; a hidden branch pins the new queued resources.
        root->setVisible(false);
        nextFarPack=FarPack{engine.sceneTree().world().addChild(std::move(root)),lon,lat,vertices,ice};
        placeFarPack();
        saida::Log::info("[World ice] far pack to ",r1::kFarPackRadius/1000.," km around ",lon,", ",lat,
                         ": ",vertices," vertices, ",ice->measured?"measured ":"inferred ",ice->date,
                         " build_ms=",msSince(started));
    }
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
    void placeFarRelief() {
        if(!farRelief.node)return;
        const auto position=glm::vec3(origin.local(ecef(farRelief.lon,farRelief.lat,0.)));
        const Frame own(farRelief.lon,farRelief.lat,0.);
        const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
        engine.sceneTree().world().rebaseSubtreeTo(*farRelief.node,position,rotation);
    }
    void updateFarRelief() {
        // Where it cannot be drawn: before a spawn, over the pack (whose own
        // far field is the horizon there), and past the Mercator images' 85 degrees.
        const bool wanted=playing&&!pending&&!farPack.node&&std::abs(lat)<84.5;
        if(!wanted) {
            if(farRelief.node&&farRelief.node->visible())farRelief.node->setVisible(false);
            return;
        }
        if(!farRelief.node) {
            auto n=std::make_unique<saida::TerrainRingsNode>();
            n->setName("far relief");
            n->levels=kFarLevels;n->baseSpacing=float(kFarBaseSpacing);
            farRelief.node=static_cast<saida::TerrainRingsNode*>(engine.sceneTree().world().addChild(std::move(n)));
        }
        if(!farRelief.node->visible())farRelief.node->setVisible(true);
        auto& f=farRelief;
        // A new tangent frame where the player is, when the old one is far.
        glm::dvec2 focus{0.};
        if(f.job) {const r1::P3 p=f.anchor.toEngine(lon,lat,0.);focus={p.x,p.z};}
        if(!f.job||glm::length(focus)>kFarReanchor) {
            f.lon=lon;f.lat=lat;f.anchor=r1::Anchor::at(lon,lat,0.);
            f.layers=std::make_shared<r1::FarLayers>(r1::climateAt(r1::profileFor(lon,lat).climate,lat));
            f.layersSeen=0;
            f.node->clearLevels();f.asked={};f.retryAt={};f.reach=0;++f.generation;
            f.job=std::make_shared<FarJob>();
            focus={0.,0.};
            placeFarRelief();
            saida::Log::info("[World far] rings anchored at ",lon,", ",lat);
        }
        f.node->focus=focus;
        // What came back from the thread.
        std::vector<std::pair<int,r1::FarLevel>> done;std::vector<std::string> said;
        {std::lock_guard<std::mutex> g(f.job->lock);done.swap(f.job->done);said.swap(f.job->said);}
        for(const auto& line:said)saida::Log::info("[World far] ",line);
        for(auto& [k,level]:done) {
            f.retryAt[size_t(k)]=level.installedFallback?std::chrono::steady_clock::now()+kFarRetry:
                std::chrono::steady_clock::time_point{};
            saida::TerrainRingsNode::Level l;
            l.origin={level.originX,level.originZ};l.spacing=level.spacing;
            l.heights=std::move(level.heights);l.layers=std::move(level.layers);
            if(f.node->setLevel(k,std::move(l)))
                f.reach=std::max(f.reach,0.5*saida::TerrainRingsNode::kResolution*level.spacing);
        }
        if(f.layers->revision()!=f.layersSeen) {
            f.layersSeen=f.layers->revision();
            const auto swatches=f.layers->swatches();
            const auto surfaces=f.layers->materials();
            // Stand-scale variation remains visible beyond the physical
            // texture's detail; its mean stays the Atlas's measured albedo.
            constexpr float kGroundMacroMetres=128.0f, kForestMacroMetres=12.0f;
            constexpr float kGroundVariation=0.35f, kForestVariation=0.65f, kSnowVariation=0.06f;
            constexpr float kCanopyNormalStrength=0.45f;
            for(size_t i=0;i<size_t(saida::TerrainRingsNode::kMaxLayers);++i) {
                if(i>=swatches.size()) {f.node->setLayer(int(i),{});continue;}
                saida::TerrainRingsNode::Layer layer;
                layer.albedo={float(swatches[i].color[0]),float(swatches[i].color[1]),float(swatches[i].color[2])};
                layer.roughness=float(swatches[i].roughness);
                if(i==r1::FarLayers::kWater) {
                    layer.water=true;
                    layer.albedo={.01f,.07f,.11f};
                    layer.roughness=.05f;
                    layer.waveAmplitude=.12f;
                    layer.wavelength=9.0f;
                }
                if(!surfaces[i].baseColorTexture.empty())layer.material=material(surfaces[i]);
                layer.textureScale=float(surfaces[i].uvScale);
                layer.macroSize=i==r1::FarLayers::kForest?kForestMacroMetres:kGroundMacroMetres;
                layer.macroVariation=i==r1::FarLayers::kWater?0.0f:
                    i==r1::FarLayers::kSnow?kSnowVariation:
                    i==r1::FarLayers::kForest?kForestVariation:kGroundVariation;
                layer.macroNormalStrength=i==r1::FarLayers::kForest?kCanopyNormalStrength:0.0f;
                f.node->setLayer(int(i),layer);
            }
        }
        // The resident tiles draw their own ground: holes in the rings.
        std::vector<saida::TerrainRingsNode::Hole> holes;
        for(const auto& [key,t]:loaded) {
            if(!t.geography||t.ocean)continue;
            auto at=[&](double x,double y){const r1::P3 p=f.anchor.toEngine(x,y,0.);return glm::dvec2(p.x,p.z);};
            holes.push_back({at(t.west,t.south),at(t.east,t.south),at(t.east,t.north),at(t.west,t.north)});
        }
        f.node->setHoles(std::move(holes));
        // The rings whose samples no longer sit around the player, finest first.
        {std::lock_guard<std::mutex> g(f.job->lock);if(f.job->busy)return;}
        std::vector<std::pair<int,glm::dvec2>> asks;
        for(int k=0;k<kFarLevels;++k) {
            const glm::dvec2 o=f.node->levelOrigin(k,focus);
            const auto retry=f.retryAt[size_t(k)];
            const bool due=retry!=std::chrono::steady_clock::time_point{}&&std::chrono::steady_clock::now()>=retry;
            if(f.asked[size_t(k)]&&o==f.origins[size_t(k)]&&!due)continue;
            f.origins[size_t(k)]=o;f.asked[size_t(k)]=true;asks.push_back({k,o});
            f.retryAt[size_t(k)]=std::chrono::steady_clock::now()+kFarRetry;
        }
        if(asks.empty())return;
        f.job->busy=true;
        std::thread([job=f.job,layers=f.layers,anchor=f.anchor,asks,root=game.string()] {
            const r1::ObservationStore store(root);
            for(const auto& [k,o]:asks) {
                const double spacing=kFarBaseSpacing*std::ldexp(1.,k);
                const auto started=std::chrono::steady_clock::now();
                try {
                    auto level=r1::sampleWorldFarLevel(store,anchor,o.x,o.y,spacing,
                                                      saida::TerrainRingsNode::kResolution,*layers);
                    std::ostringstream line;
                    line<<"ring "<<k<<": "<<spacing<<" m, "
                        <<(spacing>=r1::kInstalledReliefSpacing?"local relief":"Terrain Tiles z"+std::to_string(level.zoom))
                        <<(level.installedFallback?" (installed fallback; retry pending)":"")
                        <<", sea "<<level.seaCells<<" snow "<<level.snowCells<<" rock "<<level.rockCells<<" forest "<<level.forestCells<<" scrub "<<level.scrubCells
                        <<" cells inferred, build_ms="<<msSince(started);
                    std::lock_guard<std::mutex> g(job->lock);
                    job->said.push_back(line.str());
                    job->done.push_back({k,std::move(level)});
                } catch(const std::exception& e) {
                    std::lock_guard<std::mutex> g(job->lock);
                    job->said.push_back("ring "+std::to_string(k)+" refused: no relief at "+std::to_string(spacing)+" m: "+e.what());
                }
            }
            std::lock_guard<std::mutex> g(job->lock);job->busy=false;
        }).detach();
    }
    // The trunk of a tree that has just been mounted. The compound is rebuilt
    // at the next physics sync, once however many trees joined it this frame.
    void addTrunk(Loaded& l,const json& at) {
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
    // `roadside`: only what is read from the road -- the signs, which the
    // tile lists before its furniture -- for a driver above 15 km/h.
    void streamProps(bool roadside=false) {
        const auto start=std::chrono::steady_clock::now();
        // Scene/GPU APIs stay on the render thread. Yield between objects;
        // CPU model requests yield; node instantiation retains a soft budget.
        // A replacement gets its props while hidden, so its trees and signs
        // are there the frame it is shown (World::promoteIncoming).
        for(auto t:ring)for(auto* tiles:{&loaded,&incoming}) {
            auto it=tiles->find(t.key());if(it==tiles->end())continue;
            auto& l=it->second;
            try { mountLettering(l); }
            catch(const std::exception& e) {
                l.nextLettering=l.data.value("lettering",json::array()).size();
                saida::Log::error("[World lettering] ",t.key(),": ",e.what());
                text("stream-status","Impossible de charger une enseigne.");
                if(smoke){testFailed=true;engine.sceneTree().quit();return;}
            }
            if(msSince(start)>=kPropImportMs)return;
            while(l.nextProp<l.props.size()) {
                if(roadside) {
                    const auto& doc=l.props[l.nextProp];
                    if(!doc.contains("groups")||std::find(doc["groups"].begin(),doc["groups"].end(),"roadside")==doc["groups"].end())break;
                }
                try {
                    auto doc=l.props[l.nextProp];
                    bool plant=doc.contains("groups")&&std::find(doc["groups"].begin(),doc["groups"].end(),"vegetation")!=doc["groups"].end();
                    if(!modelsReady(doc))return;
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
                        if(entry.tree)n->setEnabled(debugTrees);
                        if(n->findByPath("Mid"))entry.lod=n->getBehaviour<saida::LODGroupBehaviour>();
                        l.vegetation.push_back(entry);
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
    // The nodes the parts go up under (World::uploadParts).
    static void shape(Loaded& tile) {
        tile.geography=tile.node->findByPath("Geography");
        tile.grass=dynamic_cast<saida::GrassNode*>(tile.node->findByPath("Grass"));
        for(const auto& child:tile.node->children())
            if(auto* water=dynamic_cast<saida::WaterNode*>(child.get()))tile.waters.push_back(water);
    }
    // Everything a tile in `loaded` does beyond being drawn: its density, its
    // traffic and crowd, its aircraft. After the emplace, never before: the
    // flow holds a pointer to the graph, and a graph built in a temporary
    // would be moved out from under it.
    void activate(const std::string& key,Loaded& tile) {
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
        mountAircraft(tile);
        placeTiles();
        reparkIfCovered(key);
    }
    // A replacement is shown once every part of its ground is draw-ready and
    // solid, in the frame its predecessor is taken down: the street is never
    // missing in between. Replacements go up together -- a tile's ground is
    // joined to its neighbours' (gen/seams), so one swapped alone would meet
    // the old version of the next one with a step -- unless the first ready
    // has waited a second for the others. One that cannot get ready says why
    // and is shown as it is rather than leaving the old version up for good.
    static constexpr double kReplaceWaitMs=10000.,kBatchWaitMs=1000.;
    bool replacementReady(const std::string& key,const Loaded& next) {
        const auto& parts=std::any_cast<const PreparedTile&>(next.served->prepared).parts;
        bool ready=!next.geography||next.nextPart>=parts.size();
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
    bool promoteIncoming() {
        std::vector<std::string> due;bool holding=false;double longest=0;
        for(const auto& [key,next]:incoming) {
            const double waited=msSince(next.builtAt);
            if(replacementReady(key,next)){due.push_back(key);longest=std::max(longest,waited);}
            else if(waited>=kReplaceWaitMs) {
                // A timed-out replacement is due too: otherwise a younger
                // unready tile holds the batch forever and repeats this warning.
                longest=std::max(longest,waited);
                const auto& parts=std::any_cast<const PreparedTile&>(next.served->prepared).parts;
                saida::Log::warn("[World streaming] ",key," replacement not draw-ready after ",
                                 int(waited)," ms (parts ",next.nextPart,"/",parts.size(),", props ",
                                 next.nextProp,"/",next.props.size(),"): shown as it is");
                due.push_back(key);
            } else holding=true;
        }
        if(due.empty()||(holding&&longest<kBatchWaitMs))return false;
        for(const auto& key:due) {
            const double waited=msSince(incoming.at(key).builtAt);
            if(auto old=loaded.find(key);old!=loaded.end()){old->second.node->queueFree();loaded.erase(old);}
            auto handle=incoming.extract(key);
            handle.mapped().node->setVisible(true);
            Loaded& tile=loaded.insert(std::move(handle)).position->second;
            activate(key,tile);
            saida::Log::info("[World streaming] replaced ",key," after ",int(waited)," ms cook_ms=",
                             tile.served->cooked.cookMs);
        }
        return true;
    }
    void stream() {
        double x=(pending||warming)?pickLon:lon,y=(pending||warming)?pickLat:lat;
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
        for(auto* tiles:{&loaded,&incoming})for(auto& [k,t]:*tiles)if(wanted.count(k))
            {count+=t.data.at("vertices").get<size_t>();indices+=t.data.value("indices",size_t(0));}
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
            const json& data=current->cooked.manifest;
            const size_t vertices=data.at("vertices").get<size_t>(),tileIndices=data.value("indices",size_t(0));
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
                    it->second.node->queueFree();it=loaded.erase(it);removed=true;
                }
            } else {
                // Both versions do not fit side by side: the old one goes first
                // and the tile is missing until the new one is up. Said, since
                // it is seen.
                saida::Log::info("[World streaming] ",it->first," replaced in place: ",count," + ",vertices,
                                 " of ",residentVertexBudget," resident vertices, ",indices," + ",tileIndices,
                                 " of ",residentIndexBudget," indices");
                if(wanted.count(it->first)) {
                    const auto& old=it->second.data;
                    count-=old.at("vertices").get<size_t>();indices-=old.value("indices",size_t(0));
                }
                it->second.node->queueFree();it=loaded.erase(it);removed=true;
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
                const size_t vertices=data.at("vertices").get<size_t>(),tileIndices=data.value("indices",size_t(0));
                if(count+vertices>residentVertexBudget||indices+tileIndices>residentIndexBudget) {
                    text("stream-status","Limite de détail atteinte dans cette zone.");
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
                                 msSince(goStarted));
            }catch(const std::exception& e){
                text("status",std::string("Tuile indisponible : ")+e.what());
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
            text("status","Ce quartier dépasse la mémoire géométrique disponible. Choisissez un point voisin.");
            return;
        }
        auto ready=[&](double x,double y){auto* t=tile(x,y);if(!t)return false;
            if(t->geography&&t->nextPart<std::any_cast<const PreparedTile&>(t->served->prepared).parts.size())return false;
            for(auto* mesh:t->partMeshes)if(!mesh->loaded())return false;
            return !t->geography||(!t->geography->children().empty()&&
                !static_cast<saida::CollisionObjectNode*>(t->geography->children().back().get())->bodyId().IsInvalid());};
        if(pending && ready(pickLon,pickLat)) {
            const bool waterSpawn=onWater(pickLon,pickLat);
            double x0=pickLon,y0=pickLat;bool found=waterSpawn||!blocked(x0,y0);
            for(int i=1;!found&&i<=160;++i) {
                double a=i*2.39996323,d=2.*std::sqrt(double(i));auto q=advance(pickLon,pickLat,d*cos(a),d*sin(a));
                if(ready(q.x,q.y)&&!blocked(q.x,q.y)){x0=q.x;y0=q.y;found=true;}
            }
            if(!found) {
                // A safe point can lie just across the tile boundary. Only
                // that exceptional spawn waits for the rest of the search area.
                if(!surroundingReady && refused.empty())return;
                pending=false;
                text("status","Point situé dans un bâtiment. Choisissez une rue voisine.");
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
                return;
            }
            lon=x0;lat=y0;alt=waterSpawn?waterLevel(lon,lat):height(lon,lat);
            conditions=json::object();weather=Weather{};
            origin=Frame(lon,lat,alt);placeTiles();moveSun();
            jumpOffset=jumpVelocity=0;followDistance=kOnFootFollow;wasJump=false;
            // Teleporting leaves the current vehicle. A water arrival starts
            // swimming at the selected coordinate, including in the open sea.
            // Carry the vehicle's view into the walking heading, just as a
            // normal exit does: the camera and movement must share one yaw.
            yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
            driving=false;swimming=waterSpawn;swimTime=0;swimLean=0;swimHeading=yaw;
            clearBoats();
            clearAircraft();
            player->setEnabled(true);
            player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
            parkCar();
            playing=true;pending=false;warming=false;showMap(false);request(lon,lat);
            saida::Log::info("[World] spawned ",lon,", ",lat," altitude=",alt,
                             waterSpawn?" swimming":" on foot",
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
            if(!status.error.empty())text("status","Erreur de chargement, nouvelle tentative automatique. "+status.error);
            else if(status.offline)text("status","Une source ne répond pas : affichage des données déjà reçues.");
            else text("status",kPreparing);
        }
    }
public:
    World(saida::Engine& e,fs::path g,bool test,saida::CaptureRequest capture,double startLon,double startLat,bool hop,double hopX,double hopY,saida::runtime::CaptureViewpoint view,bool sail=false,bool fly=false):engine(e),game(g),smokeSail(sail),smokeFly(fly),pickLon(startLon),pickLat(startLat),mapCenterLon(startLon),mapCenterLat(startLat),menuTiles(g),smoke(test),hopLon(hopX),hopLat(hopY),hopWanted(hop),worldCapture(capture),captureView(view) {
        performanceSmoke=smoke&&std::getenv("R1WORLD_DEBUG_SMOKE");
        residentVertexBudget=size_t(double(e.resources().geometryCapacity().vertices)*kTileGeometryShare);
        residentIndexBudget=size_t(double(e.resources().geometryCapacity().indices)*kTileGeometryShare);
        ui=dynamic_cast<saida::WebCanvasNode*>(e.sceneTree().firstInGroup("world-ui"));
        minimapUi=dynamic_cast<saida::WebCanvasNode*>(e.sceneTree().firstInGroup("minimap-ui"));
        camera=dynamic_cast<saida::CameraNode*>(e.sceneTree().firstInGroup("camera"));
        if(!ui||!minimapUi||!camera)throw std::runtime_error("Earth scene is missing its camera, map or minimap");
        minimapUi->setEnabled(false);
        {
            std::ifstream input(game/"assets/world/timezone-countries.json");
            if(!input)throw std::runtime_error("Missing assets/world/timezone-countries.json");
            const json places=json::parse(input);
            zoneCountries=places.at("zones").get<std::map<std::string,std::string>>();
            countryNames=places.at("countries").get<std::map<std::string,std::string>>();
        }
        loadOptions();
        player=e.sceneTree().firstInGroup("player");
        if(!player)throw std::runtime_error("Earth scene is missing the player");
        player->findBehavioursInChildren(animators);
        if(animators.empty())throw std::runtime_error("Player model has no Animator");
        loadHumans();
        // The body is drawn at the people's scale; its height is the manifest's.
        for(auto& child:player->children())child->transform().scale=glm::vec3(float(humanScale));
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
        prototypes=e.sceneTree().world().createChild<saida::Node>("Shared prototypes");
        prototypes->setEnabled(false);
        loadPaints();
        buildTrafficPrototype();vehicleCollider(*car,vehicleSpec(*car));
        buildAircraftPrototypes();
        saida::Log::info("[World traffic] ready, ",paints.size()," paints");
        // Textures resident at once. A city neighbourhood shows about fifteen
        // photographed materials (ground, streets, walls, roofs; see
        // assets/textures/surfaces.json), some 90 MB, on top of what 256 MB already held.
        // The reference GTX 1060 has 4.5 GB of VRAM to spend (plan §3 I4), and
        // the geometry arena takes about 50 MB of it.
        e.resources().setGpuBudget(512ull*1024*1024);
        r1::WorldService::Options options;
        options.gameRoot=game.string();
        // Nine tiles can be resident together. Leave 5% of their share for
        // landmarks that remain visible across a tile boundary.
        options.tileVertexTarget=std::min(r1::kTileVertexBudget,residentVertexBudget*95/900);
        options.enrichRetail=true;
        options.fetchCanopy=true;
        options.prepare=[](r1::ServedTile& tile){tile.prepared=prepareTile(tile.cooked);};
        options.log=[](const std::string& line){saida::Log::info("[World service] ",line);};
        service=std::make_unique<r1::WorldService>(std::move(options));
        // Nearest first to be needed last: warm() takes from the back.
        for(const auto& b:r1::palette().boats)warmList.push_back("assets/models/external/kenney_boats/"+b.model+".glb");
        for(const auto& k:r1::palette().props)for(const auto& m:k.models)warmList.push_back(m);
        for(const auto& [code,sign]:r1::palette().signs)for(const auto& [mount,m]:sign.mounts)warmList.push_back(m);
        for(const auto* kits:{&r1::palette().townSigns,&r1::palette().streetSigns})for(const auto& [country,kit]:*kits) {
            for(const auto& m:{kit.post,kit.bar})if(!m.empty())warmList.push_back(m);
            for(const auto& [lines,p]:kit.plates)for(const auto& m:{p.left,p.middle,p.right})warmList.push_back(m);
            for(const auto& [ch,g]:kit.glyphs)warmList.push_back(g.first);
        }
        for(const char* dir:{"assets/models/external/nature_selected","assets/models/external/nature_cards"}) {
            std::error_code ec;
            for(const auto& f:fs::directory_iterator(game/dir,ec))
                if(f.path().extension()==".glb")warmList.push_back(std::string(dir)+"/"+f.path().filename().string());
        }
        std::sort(warmList.begin(),warmList.end());
        warmList.erase(std::unique(warmList.begin(),warmList.end()),warmList.end());
        sea=std::make_unique<r1::SeaService>(game.string(),[](const std::string& line){saida::Log::info("[World sea] ",line);});
        sky=std::make_unique<r1::ConditionsService>(game.string(),[](const std::string& line){saida::Log::info("[World sky] ",line);});
        readFarLandmarks();
        e.window().setCursorCaptured(false);
    }
    ~World(){for(auto* e:listeners)if(ui->isLiveElement(e,generation)){
        e->RemoveEventListener("click",this);e->RemoveEventListener("mousedown",this);}}
    bool failed() const {return testFailed;}
    void inspectInstant(double unixSeconds) {inspectAt=unixSeconds;}
    void inspectConditions(std::array<double,3> conditions) {
        if(!inspectAt||worldCapture.pngPath.empty())throw std::runtime_error("--weather requires --at and --screenshot");
        inspectWeather=conditions;
    }
    void captureFromAltitude(double metres) {captureAltitude=metres;}
    void captureAtGeography(std::array<double,3> camera) {captureGeo=camera;}
    void ProcessEvent(Rml::Event& event) override {
        auto id=event.GetCurrentElement()->GetId();
        const bool press=event.GetType()=="mousedown";
        if(!press&&id==pressedId&&clock-pressedAt<.5)return;  // already acted on the press
        if(press){pressedId=id;pressedAt=clock;}
        if(id=="map") {
            auto* map=ui->findElementById("map");auto o=map->GetAbsoluteOffset();
            double x=event.GetParameter<float>("mouse_x",0)-o.x,y=event.GetParameter<float>("mouse_y",0)-o.y;
            const r1::MapPoint clicked=zoom>1?r1::MapView(mapCenterLon,mapCenterLat,mapZoomLevel()).pointAt(x,y)
                                                :r1::MapPoint{x/r1::MapView::width*360.-180.,90.-y/r1::MapView::height*180.};
            select(clicked.lon,clicked.lat);
            field("city-query",std::string());showCityChoices({});
            warming=true;if(!smoke)request(pickLon,pickLat);
        } else if(id=="go")go();
        else if(id=="options")showOptions(true);
        else if(id=="options-back")showOptions(false);
        else if(id=="forced-apply")applyTypedTime();
        else if(id=="forced-clear"){forceTime(std::nullopt);field("forced-time",std::string());}
        else if(id.rfind("forced-preset-",0)==0)forceTime(std::stoi(id.substr(14))*60);
        else if(id=="resume"&&playing){pending=false;warming=false;request(lon,lat);showMap(false);}
        else if(id=="zoom-in")zoomMap(zoom*2);
        else if(id=="zoom-out")zoomMap(zoom/2);
        else if(id=="street-map")zoomMap(std::max(zoom,32768.));
        else if(id=="reset-map")zoomMap(1);
        else if(id.rfind("city-choice-",0)==0) {
            const int choice=id.back()-'0';
            if(choice>=0&&choice<int(cityChoices.size())) {
                const auto selected=cityChoices[size_t(choice)];
                select(selected.lon,selected.lat);zoomMap(std::max(zoom,2048.));
                field("city-query",selected.label);
                cityInput=citySubmitted=selected.label;
                showCityChoices({});
                warming=true;if(!smoke)request(pickLon,pickLat);
            }
        }
        else {std::map<std::string,glm::dvec2> places{{"paris",{2.3522,48.8566}},{"tokyo",{139.7671,35.6812}},{"newyork",{-73.9855,40.758}},{"lawrence",{-95.2436,38.9585}},{"cape",{18.4241,-33.9249}},{"sydney",{151.2093,-33.8688}},{"pole",{0.,90.}}};
            if(places.count(id)){auto p=places.at(id);select(p.x,p.y);zoomMap(zoom);
                field("city-query",std::string());showCityChoices({});
                warming=true;request(pickLon,pickLat);}}
    }
    // The Options screen, as a player uses it: open it, force an hour, see the
    // Sun take it, have a bad hour refused without losing the good one, go
    // back to the real clock, close it. Presses only, like every other click.
    bool smokeOptions() {
        auto fail=[&](const std::string& why){
            saida::Log::error("[World E2E] FAIL options: ",why);testFailed=true;engine.sceneTree().quit();return false;
        };
        testClick("options",false);
        if(!optionsOpen)return fail("the Options press did not open the screen");
        field("forced-time","14h30");testClick("forced-apply",false);
        if(forcedMinutes!=14*60+30)return fail("14h30 was not read as 14:30");
        double at=0;bool known=false;const int offset=utcOffset(known);
        if(!gameTime(at)||std::abs(std::fmod(at+offset,86400.)-(14*3600.+1800.))>1.)
            return fail("the Sun is not at 14:30 local (gameTime "+number(at,0)+", offset "+std::to_string(offset)+")");
        field("forced-time","25:00");testClick("forced-apply",false);
        if(forcedMinutes!=14*60+30)return fail("25:00 replaced the forced hour instead of being refused");
        testClick("forced-preset-12",false);
        if(forcedMinutes!=12*60)return fail("the 12:00 preset did not apply");
        testClick("forced-clear",false);
        if(forcedMinutes||forcedSent)return fail("Heure réelle did not give the Sun back to the clock");
        testClick("options-back",false);
        if(optionsOpen)return fail("Retour did not close the screen");
        saida::Log::info("[World E2E] options passed (forced hour reaches the Sun, bad hour refused, real time restored)");
        return true;
    }
    // The driver's own two phases, kept out of update() so the flow reads:
    // walk, drive, then either teleport or finish.
    void startSmokeDrive() {
        smokeDroveOnce=true;
        if(!carParked) {
            // The car not being there is a real outcome with a real cause --
            // no free kerb within 14 m -- and it fails on that cause rather
            // than on the drive that could not be attempted.
            saida::Log::error("[World E2E] FAIL no car parked within reach of the spawn");
            testFailed=true;engine.sceneTree().quit();return;
        }
        // A test driver chooses a free direction, like a human: aiming the car
        // at the nearest real wall is not a failure of the car. Same search the
        // spawn runs for the walk, over a road's worth of distance.
        smokeClear=0;double heading=carYaw;
        for(int direction=0;direction<32;++direction) {
            double candidate=direction*11.25,clear=clearAhead(carLon,carLat,carAlt,candidate,1.,60.);
            if(clear>smokeClear){smokeClear=clear;heading=candidate;}
        }
        carYaw=heading;
        // The car is where the player spawned and the walk phase has taken him
        // eleven metres away from it, which is the first thing this test found
        // and the right thing for it to find: a door has a handle, not a
        // radius. So the driver walks back to it, the way a player does.
        smokeApproach=true;smokeApproachTime=0;
        saida::Log::info("[World E2E] walking back to the car, ",carDistance(),"m away");
    }
    void beginSmokeDriving() {
        if(!enterCar()) {
            saida::Log::error("[World E2E] FAIL could not enter the car, ",
                              carDistance(),"m away, parked=",carParked);
            testFailed=true;engine.sceneTree().quit();return;
        }
        smokeDriving=true;smokeDriveTime=0;smokeTopSpeed=0;smokeDriveStart=ecef(lon,lat,alt);
        churn=0;churnFrames=0;dirtyFrames=0;
        saida::Log::info("[World E2E] driving, ",smokeClear,"m clear on heading ",carYaw);
    }
    // Getting into a car that belongs to the traffic, which is the whole of
    // "n'importe quelle voiture" and the one part of it a player cannot be
    // talked out of noticing when it is broken.
    //
    // The driver stands the player beside a live traffic car rather than
    // walking him to it: the car is moving, so walking to it is a pursuit
    // problem and this test is not about pursuit. Standing beside it is
    // exactly where stepping out of a car leaves him, so the reach it tests is
    // the reach a player gets.
    void runSmokeTakeover() {
        smokeTookOver=true;
        Loaded* tile=nullptr;size_t index=0;
        for(auto& [key,l]:loaded) {
            for(size_t i=0;i<l.cars.size();++i)
                if(l.cars[i]&&trafficSlotLive(l,i)){tile=&l;index=i;break;}
            if(tile)break;
        }
        if(!tile) {
            saida::Log::error("[World E2E] FAIL no traffic car to take over");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Node* wanted=tile->cars[index];
        saida::Node* previous=car;
        const double previousLon=carLon,previousLat=carLat;
        const auto where=geodeticOf(*tile,wanted->transform().position);
        // Two and a half metres to the east of it, which is a door's width and
        // inside kCarReach.
        const auto beside=advance(where.x,where.y,2.5,0.);
        if(!World::tile(beside.x,beside.y)) {
            saida::Log::error("[World E2E] FAIL the traffic car stands off the loaded world");
            testFailed=true;engine.sceneTree().quit();return;
        }
        lon=beside.x;lat=beside.y;alt=height(lon,lat);
        if(!enterCar()||!driving) {
            saida::Log::error("[World E2E] FAIL could not take over a traffic car 2.5 m away");
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(car!=wanted) {
            saida::Log::error("[World E2E] FAIL took over a different car than the one reached");
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(tile->cars[index]!=nullptr||tile->flow.agents()[index].alive) {
            saida::Log::error("[World E2E] FAIL the traffic still owns the car the player is in");
            testFailed=true;engine.sceneTree().quit();return;
        }
        // And the car he stepped out of is still standing where he left it,
        // which is the half of this that copying a model would have broken.
        const auto left=std::find_if(parked.begin(),parked.end(),
                                     [&](const Parked& p){return p.node==previous;});
        if(left==parked.end()||std::abs(left->lon-previousLon)>1e-9||std::abs(left->lat-previousLat)>1e-9) {
            saida::Log::error("[World E2E] FAIL the previous car did not stay where it was left");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] took over a traffic car; ",parked.size(),
                         " car(s) left standing");
        if(!leaveCar()) {
            saida::Log::error("[World E2E] FAIL could not step out of the taken car");
            testFailed=true;engine.sceneTree().quit();return;
        }
    }

    // `--sail`: find a moored boat, stand beside it, take the helm, sail it
    // out at full throttle, be refused a landing at speed, then stop. Every
    // step fails on its own reason (rule 3 of CLAUDE.md).
    void runSmokeSail(float delta) {
        const double dt=std::min(.05,double(delta));
        smokeSailTime+=dt;
        if(smokeSailWait) {
            BoatReach best;double bestDistance=1e30;
            for(auto& [key,t]:loaded)
                for(size_t i=0;i<t.boats.size();++i) {
                    Mooring& m=t.boats[i];
                    if(m.taken||!mooredNode(t,m)||m.kind=="cargo"||m.kind=="liner")continue;
                    const double d=hullDistance(t.frame,m.local,m.heading,m.length,m.beam);
                    if(d<bestDistance){bestDistance=d;best={&t,i,BoatSource::Moored};}
                }
            if(!best.tile) {
                if(smokeSailTime>60.) {
                    saida::Log::error("[World E2E] FAIL sail: no moored boat streamed within 60 s");
                    testFailed=true;engine.sceneTree().quit();
                }
                return;
            }
            // Stand the player on the first dry spot beside the hull, which is
            // where a quay or a pier leaves him.
            Mooring& m=best.tile->boats[best.index];
            const auto centre=geodeticOf(*best.tile,glm::vec3(m.local));
            const double s=std::sin(m.heading*rad),c=std::cos(m.heading*rad);
            bool placed=false;
            for(double off:{1.,2.,3.}) {
                for(double along=-m.length*.4;along<=m.length*.4&&!placed;along+=1.)
                    for(double side:{1.,-1.}) {
                        const double o=(m.beam*.5+off)*side;
                        auto q=advance(centre.x,centre.y,s*along+c*o,c*along-s*o);
                        if(tile(q.x,q.y)&&!blocked(q.x,q.y)){lon=q.x;lat=q.y;alt=height(lon,lat);placed=true;break;}
                    }
                if(placed)break;
            }
            if(!placed) {
                saida::Log::error("[World E2E] FAIL sail: no standable ground beside the nearest boat (",
                                  m.kind,", ",m.name,")");
                testFailed=true;engine.sceneTree().quit();return;
            }
            smokeSailWait=false;
            if(!enterBoat()) {
                saida::Log::error("[World E2E] FAIL sail: could not take the helm from beside the hull");
                testFailed=true;engine.sceneTree().quit();return;
            }
            // Head for the longest run of open water, as a skipper would.
            smokeSailClear=0;double heading=boat.yaw;
            for(int direction=0;direction<32;++direction) {
                double candidate=direction*11.25,clear=0;
                for(double d=2.;d<=120.;d+=2.) {
                    auto q=onward(boat.lon,boat.lat,std::sin(candidate*rad)*d,std::cos(candidate*rad)*d);
                    if(!navigable(q.x,q.y))break;
                    clear=d;
                }
                if(clear>smokeSailClear){smokeSailClear=clear;heading=candidate;}
            }
            boat.yaw=yaw=heading;
            smokeSailing=true;smokeSailBrake=false;smokeSailTime=0;smokeSailTop=0;
            smokeSailStart=ecef(lon,lat,alt);
            saida::Log::info("[World E2E] sailing a ",boat.kind,", ",smokeSailClear,"m of open water ahead");
            return;
        }
        smokeSailTop=std::max(smokeSailTop,boat.speed);
        if(!navigable(lon,lat)) {
            saida::Log::error("[World E2E] FAIL sail: the boat left the water at ",lon,", ",lat);
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(!smokeSailBrake) {
            if(smokeSailTime<8.)return;
            const double covered=glm::length(ecef(lon,lat,alt)-smokeSailStart);
            if(covered<std::min(15.,smokeSailClear*.4)||smokeSailTop<std::min(3.,boat.top*.5)) {
                saida::Log::error("[World E2E] FAIL sail: covered=",covered,"m of ",smokeSailClear,
                                  "m clear, top=",smokeSailTop," m/s");
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] sail passed, covered=",covered,"m top=",smokeSailTop," m/s");
            if(std::abs(boat.speed)>kBoatExitSpeed&&leaveBoat()) {
                saida::Log::error("[World E2E] FAIL sail: stepped ashore at ",std::abs(boat.speed)," m/s");
                testFailed=true;engine.sceneTree().quit();return;
            }
            smokeSailBrake=true;smokeSailTime=0;return;
        }
        if(std::abs(boat.speed)>kBoatExitSpeed) {
            if(smokeSailTime>40.) {
                saida::Log::error("[World E2E] FAIL sail: the boat would not slow down, still ",
                                  std::abs(boat.speed)," m/s");
                testFailed=true;engine.sceneTree().quit();
            }
            return;
        }
        smokeSailing=false;
        const double stopped=std::abs(boat.speed);
        smokeSwimBoat=boat.node;
        if(!leaveBoat()||!swimming) {
            saida::Log::error("[World E2E] FAIL swim: could not enter the water from a stopped boat");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] PASS sailing: stopped at ",stopped," m/s and entered the water");
        smokeSwimming=true;smokeSailTime=0;smokeSwimStart=ecef(lon,lat,alt);
    }
    void runSmokeSwim(float delta) {
        smokeSailTime+=std::min(.05,double(delta));
        if(smokeSailTime<1.5)return;
        const double covered=glm::length(ecef(lon,lat,alt)-smokeSwimStart);
        const double rootBelow=origin.local(ecef(lon,lat,alt)).y-player->transform().position.y;
        if(!swimming||!onWater(lon,lat)||covered<1.||rootBelow<playerHeight*.39||rootBelow>playerHeight*.78) {
            saida::Log::error("[World E2E] FAIL swim: covered=",covered,"m, water=",onWater(lon,lat),
                              ", body depth=",rootBelow,"m");
            testFailed=true;engine.sceneTree().quit();return;
        }
        smokeSwimming=false;
        if(!car) {
            saida::Log::error("[World E2E] FAIL car sink: no car model to test");
            testFailed=true;engine.sceneTree().quit();return;
        }
        // A car already on this water cell must eject its driver and descend.
        carParked=true;car->setEnabled(true);
        carLon=lon;carLat=lat;carAlt=alt;carYaw=yaw;
        carSinking=false;driving=true;swimming=false;
        driveCar(.05,1.,0.,false);
        updateSinkingCar(1.);
        if(driving||!swimming||!carSinking||carAlt>carWaterLevel-1.) {
            saida::Log::error("[World E2E] FAIL car sink: driver afloat=",swimming,
                              ", car depth=",carWaterLevel-carAlt,"m");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] PASS car sink: driver swimming, car ",carWaterLevel-carAlt,"m below water");
        if(!enterBoat()||boat.node!=smokeSwimBoat) {
            saida::Log::error("[World E2E] FAIL swim: could not reboard the same boat");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] PASS swim: ",covered,"m through water, reboarded the same boat");
        smokeSeaWait=true;smokeSailTime=0;
    }
    // Then the ships at sea: predicted offline from the local base, they must
    // be out within half a minute, or the run says why not.
    void runSmokeSea(float delta) {
        smokeSailTime+=std::min(.05,double(delta));
        size_t under=0;
        for(auto& s:seaShips)under+=s.v.speed>0;
        if(!seaShips.empty()&&smokeSailTime>12.) {
            saida::Log::info("[World E2E] PASS sea: ",seaShips.size()," ships out, ",under," under way");
            const SeaShip* target=nullptr;
            double nearest=1e30;
            for(const auto& ship:seaShips)if(ship.v.speed>2.) {
                const double distance=metresFrom(ship.v.lon,ship.v.lat);
                if(distance<nearest){nearest=distance;target=&ship;}
            }
            if(!target) {
                saida::Log::error("[World E2E] FAIL sea helm: no moving ship to approach");
                testFailed=true;engine.sceneTree().quit();return;
            }
            const Vessel& v=target->v;
            const double side=v.beam*.5+boat.beam*.5+1.;
            const auto alongside=advance(v.lon,v.lat,std::cos(v.yaw*rad)*side,-std::sin(v.yaw*rad)*side);
            boat.lon=lon=alongside.x;boat.lat=lat=alongside.y;
            boat.alt=alt=v.alt;boat.yaw=yaw=v.yaw;boat.speed=v.speed;
            const auto* hull=v.node;
            const double speed=v.speed;
            if(hopAboard()!=HopResult::Boarded||!sailing||boat.node!=hull||std::abs(boat.speed-speed)>1e-6) {
                saida::Log::error("[World E2E] FAIL sea helm: could not take the same moving hull from alongside");
                testFailed=true;
            } else saida::Log::info("[World E2E] PASS sea helm: took over the same moving hull at ",boat.speed," m/s");
            engine.sceneTree().quit();
        } else if(smokeSailTime>30.) {
            std::string last=sea->lastLine();
            if(last.empty())last="the sea said nothing";
            saida::Log::error("[World E2E] FAIL sea: no ship at sea after 30 s (",last,")");
            testFailed=true;engine.sceneTree().quit();
        }
    }

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
    void flyShot(const char* moment) {
        const char* path=std::getenv("R1WORLD_FLY_SHOT");
        const char* at=std::getenv("R1WORLD_FLY_SHOT_AT");
        if(!path||!at||std::string(at)!=moment||captureQueued)return;
        saida::CaptureRequest shot;
        shot.pngPath=path;shot.frame=3;shot.fixedStep=1.f/60.f;shot.settleTimeoutFrames=900;
        captureQueued=true;
        saida::Log::info("[World E2E] photographing '",moment,"' to ",path);
        engine.captureFrameThenExit(shot);
    }
    void flyFail(const std::string& why) {
        saida::Log::error("[World E2E] FAIL fly: ",why);
        testFailed=true;engine.sceneTree().quit();
    }
    static std::string flyCategory(const r1::AircraftType& t) {return t.klass=="helicopter"?"helicopter":"plane";}
    // The parked aircraft of a category not flown yet: business jets first
    // (they take off in the shortest run), then airliners, then helicopters.
    bool smokeNextAircraft(Loaded*& found,size_t& index) {
        found=nullptr;double best=1e30;int bestRank=9;
        for(auto& [key,t]:loaded) {
            const glm::dvec3 me=t.frame.local(ecef(lon,lat,alt));
            for(size_t i=0;i<t.aircraft.size();++i) {
                const AircraftSpot& s=t.aircraft[i];
                const auto* type=r1::aircraftType(s.type);
                if(s.taken||!s.node||!type||smokeFlown.count(flyCategory(*type)))continue;
                const int rank=type->klass=="jet"?0:type->klass=="airliner"?1:2;
                const double d=std::hypot(me.x-s.local.x,me.z-s.local.z);
                if(rank<bestRank||(rank==bestRank&&d<best)){bestRank=rank;best=d;found=&t;index=i;}
            }
        }
        return found!=nullptr;
    }
    // How far the aircraft could go along a heading before a building, the
    // water (on the ground) or the edge of the loaded world.
    double clearRun(double heading,double reach,double step,double above) {
        double clear=0;
        for(double d=step;d<=reach;d+=step) {
            const auto q=onward(plane.lon,plane.lat,std::sin(heading*rad)*d,std::cos(heading*rad)*d);
            if(!tile(q.x,q.y)||aircraftHits(q.x,q.y,plane.alt+above,heading)||(above<1.&&onWater(q.x,q.y)))break;
            clear=d;
        }
        return clear;
    }
    void runSmokeFly(float delta) {
        const double dt=std::min(.05,double(delta));
        smokeFlyTime+=dt;
        smokeF=smokeR=smokeUp=0;
        if(smokeFlyWait) {
            Loaded* t=nullptr;size_t i=0;
            if(!smokeNextAircraft(t,i)) {
                if(smokeFlown.empty()&&smokeFlyTime<90.)return;
                if(smokeFlown.empty())return flyFail("no aircraft streamed within 90 s of the spawn");
                saida::Log::info("[World E2E] PASS fly: flew ",smokeFlown.size()," kind(s) of aircraft");
                engine.sceneTree().quit();return;
            }
            // Line up only once the neighbourhood is resident: a run measured
            // over three tiles of four misses the buildings of the fourth.
            size_t resident=0;
            for(const auto& k:ring)resident+=loaded.count(k.key());
            if(resident<ring.size()&&smokeFlyTime<45.)return;
            AircraftSpot& spot=t->aircraft[i];
            const auto* type=r1::aircraftType(spot.type);
            // Stand the pilot beside the fuselage, where a walk would bring him.
            const auto centre=geodeticOf(*t,glm::vec3(spot.local));
            const double s=std::sin(spot.heading*rad),c=std::cos(spot.heading*rad);
            bool placed=false;
            for(double along:{type->length*.3,0.,-type->length*.25})
                for(double side:{-1.,1.}) {
                    if(placed)break;
                    const double o=(fuselageHalf(*type)+1.5)*side;
                    const auto q=advance(centre.x,centre.y,s*along+c*o,c*along-s*o);
                    if(tile(q.x,q.y)&&!blocked(q.x,q.y)){lon=q.x;lat=q.y;alt=height(lon,lat);placed=true;}
                }
            if(!placed)return flyFail("no standable ground beside the "+spot.type+" ("+spot.name+")");
            saida::Node* wanted=spot.node;
            if(!enterAircraft()||!piloting||plane.node!=wanted)
                return flyFail("could not take the controls of the "+spot.type+" the pilot stands beside");
            smokeFlyWait=false;smokeFlyTime=0;smokeFlyTop=0;
            smokeFlyStart=ecef(plane.lon,plane.lat,plane.alt);smokeFlyAlt=plane.alt;
            flyShot(plane.helicopter()?"heli-stand":"stand");
            if(plane.helicopter()) {
                smokeFlyPhase=10;
                saida::Log::info("[World E2E] flying a ",plane.type->name," (",spot.name,")");
            } else {
                // Line up as a pilot would: the longest clear run on the ground.
                smokeFlyClear=0;double heading=plane.yaw;
                for(int k=0;k<36;++k) {
                    const double h=k*10.,run=clearRun(h,1500.,10.,0.);
                    if(run>smokeFlyClear){smokeFlyClear=run;heading=h;}
                }
                plane.yaw=yaw=heading;
                smokeFlyPhase=1;
                saida::Log::info("[World E2E] flying a ",plane.type->name," (",spot.name,"), ",smokeFlyClear,
                                 " m clear on heading ",heading);
            }
            return;
        }
        Aircraft& p=plane;
        smokeFlyTop=std::max(smokeFlyTop,p.speed);
        switch(smokeFlyPhase) {
        case 1: {  // the take-off roll
            smokeF=1.;smokeUp=p.speed>=p.type->rotate?1.:0.;
            if(p.airborne&&p.alt-smokeFlyAlt>=25.) {
                saida::Log::info("[World E2E] PASS take-off: ",p.type->name," up ",p.alt-smokeFlyAlt," m, top ",
                                 smokeFlyTop," m/s, ",glm::length(ecef(p.lon,p.lat,p.alt)-smokeFlyStart)," m from the stand");
                flyShot("climb");
                smokeFlyPhase=2;smokeFlyTime=0;
            } else if(smokeFlyTime>60.) {
                flyFail("no take-off in 60 s: top "+number(smokeFlyTop,1)+" m/s for a rotation at "+
                        number(p.type->rotate,1)+", "+number(smokeFlyClear,0)+" m clear, airborne="+(p.airborne?"yes":"no"));
            }
            return;
        }
        case 2: {  // flown low at the nearest tall building: it must stop the aircraft
            const Loaded* in=nullptr;glm::dvec2 centre(0);double radius=0,best=1e30;
            for(auto& [key,t]:loaded) {
                const glm::dvec3 me=t.frame.local(ecef(p.lon,p.lat,p.alt));
                for(const auto& f:t.footprints) {
                    if(f.top>1e29||f.points.size()<3)continue;
                    glm::dvec2 c(0);
                    for(const auto& q:f.points)c+=q;
                    c/=double(f.points.size());
                    double r=0;
                    for(const auto& q:f.points)r=std::max(r,glm::length(q-c));
                    const auto at=geodeticOf(t,glm::vec3(float(c.x),0.f,float(c.y)));
                    const double tall=f.top-t.frame.local(ecef(at.x,at.y,groundAt(at.x,at.y,p.alt))).y;
                    const double d=std::hypot(me.x-c.x,me.z-c.y);
                    if(r<6.||tall<8.||d>=best)continue;
                    best=d;in=&t;centre=c;radius=r;
                }
            }
            double chosen=-1;
            if(in) {
                const auto middle=geodeticOf(*in,glm::vec3(float(centre.x),0.f,float(centre.y)));
                const double away=radius+p.type->length*.5+p.type->span*.5+25.;
                for(int k=0;k<36&&chosen<0;++k) {
                    const double h=k*10.;
                    const auto start=advance(middle.x,middle.y,-std::sin(h*rad)*away,-std::cos(h*rad)*away);
                    if(!tile(start.x,start.y)||onWater(start.x,start.y))continue;
                    const double ground=groundAt(start.x,start.y,p.alt);
                    if(aircraftHits(start.x,start.y,ground+3.,h))continue;
                    chosen=h;p.lon=start.x;p.lat=start.y;p.alt=ground+3.;
                }
                if(chosen>=0)saida::Log::info("[World E2E] flying at a building ",away," m ahead, ",
                                              p.type->stall*1.3," m/s, 3 m up");
            }
            if(chosen<0) {
                saida::Log::info("[World E2E] no building with a clear approach here; the stop is not tested");
                smokeFlyPhase=4;smokeFlyTime=0;return;
            }
            p.yaw=yaw=chosen;p.pitch=0;p.roll=0;p.climb=0;p.airborne=true;
            p.speed=p.type->stall*1.3;p.lever=1;p.thrust=1;planeStopped=false;
            smokeFlyPhase=3;smokeFlyTime=0;
            return;
        }
        case 3: {  // stopped, not crashed, and not inside the building
            smokeF=1.;
            if(planeStopped&&p.speed==0.) {
                if(buildingAt(p.lon,p.lat,p.alt+.4))return flyFail("the aircraft stopped inside the building");
                saida::Log::info("[World E2E] PASS stopped by a building, no crash, ",p.alt-planeGround," m up");
                flyShot("stop");
                smokeFlyPhase=4;smokeFlyTime=0;
            } else if(smokeFlyTime>6.) {
                flyFail("flew at a building for 6 s and was never stopped");
            }
            return;
        }
        case 4: {  // down, stopped, and out
            smokeF=-1.;smokeUp=-1.;
            if(p.airborne||std::abs(p.speed)>kAircraftExitSpeed) {
                if(smokeFlyTime>40.)flyFail("the plane did not come down and stop within 40 s");
                return;
            }
            if(!leaveAircraft())return flyFail("could not step down from the landed plane");
            saida::Log::info("[World E2E] PASS plane: down, stopped and out");
            smokeFlown.insert("plane");smokeFlyPhase=0;smokeFlyWait=true;smokeFlyTime=0;
            return;
        }
        case 10: {  // spin up and climb
            smokeUp=1.;
            if(p.alt-smokeFlyAlt>=20.) {
                saida::Log::info("[World E2E] PASS helicopter climb: ",p.alt-smokeFlyAlt," m in ",smokeFlyTime," s");
                flyShot("heli");
                double run=0,heading=p.yaw;
                for(int k=0;k<36;++k){const double h=k*10.,r=clearRun(h,300.,5.,0.);if(r>run){run=r;heading=h;}}
                p.yaw=yaw=heading;
                smokeFlyStart=ecef(p.lon,p.lat,p.alt);smokeFlyPhase=11;smokeFlyTime=0;
            } else if(smokeFlyTime>15.) {
                flyFail("the helicopter climbed "+number(p.alt-smokeFlyAlt,1)+" m in 15 s");
            }
            return;
        }
        case 11: {  // forward
            smokeF=1.;
            if(smokeFlyTime>=4.) {
                const double covered=glm::length(ecef(p.lon,p.lat,p.alt)-smokeFlyStart);
                if(covered<20.&&!planeStopped)return flyFail("the helicopter covered "+number(covered,1)+" m in 4 s");
                saida::Log::info("[World E2E] PASS helicopter forward: ",covered," m, top ",smokeFlyTop," m/s");
                smokeFlyPhase=12;smokeFlyTime=0;
            }
            return;
        }
        case 12: {  // brake to a hover, and jump out in the air
            smokeF=p.speed>1.?-1.:0.;
            if(p.airborne&&std::abs(p.speed)<=1.) {
                const double up=p.alt-groundAt(p.lon,p.lat,p.alt);
                if(!leaveAircraft()||piloting)return flyFail("F did not let the pilot out "+number(up,1)+" m up");
                if(jumpOffset<5.)return flyFail("jumped from "+number(up,1)+" m up but falls only "+number(jumpOffset,1)+" m");
                saida::Log::info("[World E2E] PASS jumped from the helicopter, ",jumpOffset," m up");
                smokeFlyPhase=13;smokeFlyTime=0;
            } else if(smokeFlyTime>30.) {
                flyFail("the helicopter did not come to a hover within 30 s");
            }
            return;
        }
        case 13: {  // he lands, and the helicopter he left comes down
            const bool down=jumpOffset<=0&&(leftAircraft.empty()||!leftAircraft.back().airborne);
            if(down) {
                if(!swimming&&blocked(lon,lat))return flyFail("the jump landed inside a building");
                flyShot("jump");
                saida::Log::info("[World E2E] PASS landed ",swimming?"in the water":"on the ground",
                                 " in ",smokeFlyTime," s, and the helicopter came down");
                smokeFlown.insert("helicopter");smokeFlyPhase=0;smokeFlyWait=true;smokeFlyTime=0;
            } else if(smokeFlyTime>20.) {
                flyFail("still falling after 20 s: "+number(jumpOffset,1)+" m up, helicopter airborne="+
                        (leftAircraft.empty()?"none":leftAircraft.back().airborne?"yes":"no"));
            }
            return;
        }
        default: return;
        }
    }

    // Diagnostic scene composition; this traversal is only run by the smoke test.
    void measureWalk() {
        const auto started=std::chrono::steady_clock::now();
        size_t nodes=0;
        engine.sceneTree().world().traverse([&](saida::Node&,const glm::mat4&){++nodes;});
        const double ms=msSince(started);
        saida::Log::info("[World perf] one bare traversal of ",nodes," nodes took ",ms," ms");
        // Where they are. A walk is paid per node, so the composition is the
        // whole optimisation brief.
        auto subtree=[](saida::Node& n){
            size_t count=0;
            std::function<void(saida::Node&)> go=[&](saida::Node& x){
                ++count;for(auto& c:x.children())go(*c);
            };
            go(n);return count;
        };
        size_t plants=0,plantNodes=0,otherNodes=0,tiles=0;
        for(auto& [key,t]:loaded) {
            ++tiles;
            size_t here=subtree(*t.node);
            for(const Plant& plant:t.vegetation){++plants;plantNodes+=subtree(*plant.node);}
            otherNodes+=here;
        }
        saida::Log::info("[World perf] ",tiles," tiles hold ",otherNodes," nodes, of which ",
                         plantNodes," belong to ",plants," plants (",
                         plants?double(plantNodes)/double(plants):0.," per plant)");
    }
    void reportChurn(const char* phase) {
        saida::Log::info("[World perf] ",phase,": ",
                         int(churnFrames>0?100.*dirtyFrames/churnFrames:0.),
                         "% of frames updated scene membership (",
                         churnFrames>0?churn/churnFrames:0.," indexed nodes per frame over ",
                         int(churnFrames)," frames)");
        churn=0;churnFrames=0;dirtyFrames=0;
    }
    void smokeFinishPhase() {
        reportChurn("whole run");
        if(hopWanted&&!hopDone) {
            // Arrive from the driver's seat while looking out of a side window.
            // A stale lookYaw made forward walking go sideways after loading.
            if(!driving&&carParked&&carDistance()<kCarReach)enterCar();
            if(driving) {
                lookYaw=90.;lookIdle=.4;
                smokeArrivalHeading=wrap(yaw+lookYaw);
            }
            // Teleport the way a player does: open the map, type the
            // destination, press Go. Nothing here reaches past the UI.
            hopDone=true;showMap(true);
            field("longitude",hopLon);field("latitude",hopLat);
            hopArmed=true;hopWait=0;
            return;
        }
        showMap(true);testResume=true;
    }
    void sayBodies() {
        auto* physics=engine.sceneTree().world().physics();
        if(!physics)return;
        // The densest tile's static bodies, what kTileStaticBodies answers for.
        size_t trunks=0,densest=0;std::string densestKey;
        for(const auto& [key,t]:loaded) {
            if(auto* body=t.node->findByPath("Tree trunks"))trunks+=body->children().size();
            size_t statics=0;
            std::function<void(const saida::Node&)> count=[&](const saida::Node& n) {
                // A car's box is a traffic slot's body, counted in kTileMovingBodies.
                if(auto* body=dynamic_cast<const saida::StaticBodyNode*>(&n);
                   body&&!body->bodyId().IsInvalid()&&body->name()!="Vehicle collider")++statics;
                for(const auto& c:n.children())count(*c);
            };
            count(*t.node);
            if(statics>densest){densest=statics;densestKey=key;}
        }
        saida::Log::info("[World physics] ",physics->bodyCount()," bodies of ",physics->capacity().bodies,
                         " (kPhysicsBodies) across ",loaded.size()," tiles, ",trunks," tree trunks among them; densest tile ",
                         densestKey," holds ",densest," static bodies of ",kTileStaticBodies," (kTileStaticBodies)");
    }
    // A tile's trunks are one compound body: the query the car stops on and
    // the feet's solver meet every tile's first mounted trunk, a metre off the ground.
    bool trunksSolid() {
        auto* physics=engine.sceneTree().world().physics();
        size_t checked=0;
        for(const auto& [key,t]:loaded) {
            auto* body=dynamic_cast<saida::StaticBodyNode*>(t.node->findByPath("Tree trunks"));
            if(!body||body->children().empty())continue;
            // Trunks join as their trees stream: a body whose first trees
            // arrived this frame is built at the next sync, refused never.
            if(body->bodyId().IsInvalid()&&!body->bodyRefused())continue;
            auto* shape=dynamic_cast<saida::CollisionShapeNode*>(body->children().front().get());
            const glm::vec3 at(body->worldTransform()*glm::vec4(shape->offset-glm::vec3(0,1.f,0),1.f));
            bool met=false;
            if(physics)for(auto id:physics->overlapSphere(at,.32f,obstacleFilter()))met=met||id==body->bodyId();
            if(!met) {
                saida::Log::error("[World E2E] FAIL trunks: ",key,"'s first trunk is not solid (",
                                  body->children().size()," trunks, body ",body->bodyRefused()?"refused":body->bodyId().IsInvalid()?"unbuilt":"built",")");
                return false;
            }
            ++checked;
        }
        saida::Log::info("[World E2E] trunks solid in ",checked," tiles");
        return true;
    }
    // The height the walk and the car follow is the ground drawn and collided
    // with. A patch that disagreed by 30 cm stopped the car on open grass.
    // Coastal tiles are left out: the sea's levelling (gen/harbours
    // Cells::adjust) moves the drawn ground and not the manifest's grid.
    bool groundMatchesDrawing() {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
        const Loaded* t=tile(lon,lat);if(!t||t->ocean)return true;
        if(std::find(t->water.begin(),t->water.end(),uint8_t(2))!=t->water.end()) {
            saida::Log::info("[World E2E] ground check skipped: the tile meets the sea");return true;
        }
        double worst=0,worstLon=0,worstLat=0,worstHeight=0;size_t samples=0;
        for(int i=1;i<16;++i)for(int j=1;j<16;++j) {
            const double x=t->west+(t->east-t->west)*i/16.,y=t->south+(t->north-t->south)*j/16.;
            const double h=terrainHeight(*t,x,y);
            const auto hit=physics->raycast(glm::vec3(origin.local(ecef(x,y,h+2.))),{0,-1,0},4.f,obstacleFilter());
            if(!hit.hit)continue;
            auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
            if(!node||node->name().rfind("Ground",0)!=0)continue; // a street, a roof or a trunk is above it
            const double error=std::abs(double(hit.point.y)-origin.local(ecef(x,y,h)).y);
            if(error>worst){worst=error;worstLon=x;worstLat=y;worstHeight=h;}++samples;
        }
        if(worst>.05) {
            saida::Log::error("[World E2E] FAIL ground: the height followed is ",worst,
                              " m off the ground drawn (",samples," samples) at ",worstLon,",",worstLat,
                              " followed=",worstHeight," grid=",gridHeight(*t,worstLon,worstLat),
                              " onDeck=",onDeck(*t,worstLon,worstLat));
            return false;
        }
        saida::Log::info("[World E2E] ground followed within ",worst," m of the ground drawn (",samples," samples)");
        return true;
    }
    // Two tiles meet where their ground meets (gen/seams): a step at an edge
    // stopped the car on a bare field. Well under the 0.28 m the car meets as
    // an obstacle, with room for what smoothing a staggered seam leaves on a
    // quay wall. Tiles meeting the sea are left out, as above, and the ground
    // dug under a bridge is measured apart: each tile solves its bridges on
    // its own window (gen/bridges), so a dig on a seam may differ by a little.
    bool seamsClosed() {
        auto coastal=[](const Loaded& t){return t.ocean||std::find(t.water.begin(),t.water.end(),uint8_t(2))!=t.water.end();};
        // A dug node bends the ground out to the next node, a grid step away.
        auto dug=[](const Loaded& t,double x,double y){
            const double step=(t.north-t.south)*kMetresPerDegree/std::max(1,t.gridSize-1);
            for(const auto& d:t.data.value("groundDigs",json::array()))
                if(std::hypot((x-d[0].get<double>())*kMetresPerDegree*std::cos(y*rad),(y-d[1].get<double>())*kMetresPerDegree)<d[2].get<double>()+step)return true;
            return false;
        };
        // A tile whose next version is already cooked is on its way out: its
        // ground was joined to neighbours that have since moved on.
        auto leaving=[&](const std::string& key,const Loaded& t){
            const auto current=service->find(t.served->cooked.tile);
            return incoming.count(key)||(current&&current->serial!=t.served->serial);
        };
        double worst=0,worstDug=0;std::string where;size_t seams=0,changing=0;
        for(const auto& [ka,a]:loaded)for(const auto& [kb,b]:loaded) {
            if(&a==&b||a.gridSize<2||b.gridSize<2||coastal(a)||coastal(b))continue;
            const bool rows=std::abs(a.north-b.south)<1e-9,sides=std::abs(a.east-b.west)<1e-9&&std::abs(a.south-b.south)<1e-9;
            if(!rows&&!sides)continue;
            const double from=rows?std::max(a.west,b.west):a.south,to=rows?std::min(a.east,b.east):a.north;
            if(to-from<1e-9)continue;
            if(leaving(ka,a)||leaving(kb,b)){++changing;continue;}
            ++seams;
            for(int k=0;k<=200;++k) {
                const double s=from+(to-from)*k/200.,x=rows?s:a.east,y=rows?a.north:s;
                const double step=std::abs(gridHeight(a,x,y)-gridHeight(b,x,y));
                if(dug(a,x,y)||dug(b,x,y)){worstDug=std::max(worstDug,step);continue;}
                if(step>worst) {
                    worst=step;
                    auto said=[](const Loaded& t){return t.data.value("elevationSource",std::string())+", moved "+
                                                         t.data.value("groundSeams",json::object()).dump();};
                    where=ka+" ("+said(a)+") | "+kb+" ("+said(b)+") at "+std::to_string(x)+", "+std::to_string(y);
                }
            }
        }
        if(worst>.15) {
            saida::Log::error("[World E2E] FAIL seams: a ",worst," m step between ",where," (",seams," seams)");
            return false;
        }
        if(worstDug>kCarKerb) {
            saida::Log::error("[World E2E] FAIL seams: a ",worstDug," m step under a bridge dug differently by two tiles");
            return false;
        }
        saida::Log::info("[World E2E] ",seams," seams closed within ",worst," m; under a dug bridge, within ",worstDug,
                         " m; ",changing," pairs left out while a newer cook replaces one side");
        return true;
    }
    // True when the smoke has just failed on a refused body.
    bool checkPhysics() {
        auto* physics=engine.sceneTree().world().physics();
        if(!physics||physics->refusedBodies()==physicsRefused)return false;
        physicsRefused=physics->refusedBodies();
        saida::Log::error("[World physics] ",physicsRefused," bodies refused: ",physics->bodyCount()," of ",
                          physics->capacity().bodies," in use (kPhysicsBodies) across ",loaded.size()," tiles");
        if(!smoke)return false;
        saida::Log::error("[World E2E] FAIL physics: the world refused ",physicsRefused," bodies");
        testFailed=true;engine.sceneTree().quit();return true;
    }
    void update(float delta) {
        const auto now=std::chrono::steady_clock::now();
        const double frameSeconds=lastFrame.time_since_epoch().count()
            ?std::chrono::duration<double>(now-lastFrame).count():0.;
        if(!arrivalSaid&&lastFrame.time_since_epoch().count()) {
            const double frameMs=std::chrono::duration<double,std::milli>(now-lastFrame).count();
            ++arrivalFrames;arrivalHitches+=frameMs>33.;arrivalWorst=std::max(arrivalWorst,frameMs);
            if(frameMs>50.) {
                const double ours=cost.stream+cost.parts+cost.warm+cost.props+cost.distant+cost.world;
                saida::Log::info("[World frame] ",frameMs," ms: stream ",cost.stream," parts ",cost.parts," warm ",cost.warm,
                                 " props ",cost.props," distant ",cost.distant," world ",cost.world,
                                 " engine ",frameMs-ours);
            }
            if(std::chrono::duration<double>(now-goStarted).count()>10.) {
                saida::Log::info("[World streaming] arrival: ",arrivalHitches," of ",arrivalFrames,
                                 " frames over 33 ms, worst ",arrivalWorst," ms");
                sayBodies();
                arrivalSaid=true;
            }
        }
        lastFrame=now;
        const auto& debugWindow=engine.window();
        const bool debugKeys[]={debugWindow.keyDown(GLFW_KEY_R),debugWindow.keyDown(GLFW_KEY_V),
            debugWindow.keyDown(GLFW_KEY_Y),debugWindow.keyDown(GLFW_KEY_M),debugWindow.keyDown(GLFW_KEY_T)};
        samplePerformance(frameSeconds);handlePerformanceKeys(debugKeys);placePerformanceCanvas();
        cost=FrameCost{};
        if(checkPhysics())return;
        if(generation!=ui->documentGeneration()||listeners.empty()) {
            generation=ui->documentGeneration();listeners.clear();
            for(auto id:{"map","go","resume","zoom-in","zoom-out","street-map","reset-map","paris","tokyo","newyork","lawrence","cape","sydney","pole",
                         "city-choice-0","city-choice-1","city-choice-2","city-choice-3","city-choice-4",
                         "options","options-back","forced-apply","forced-clear",
                         "forced-preset-8","forced-preset-12","forced-preset-17","forced-preset-22"})
                if(auto* e=ui->findElementById(id)){e->AddEventListener("click",this);
                    e->AddEventListener("mousedown",this);listeners.push_back(e);}
            if(!listeners.empty()) {
                select(pickLon,pickLat);
                if(smoke) {
                    double sx=pickLon,sy=pickLat;
                    testClick("map");
                    double expectedLon=10./r1::MapView::width*360.-180.,expectedLat=90.-10./r1::MapView::height*180.;
                    if(std::abs(pickLon-expectedLon)>1e-5||std::abs(pickLat-expectedLat)>1e-5){
                        saida::Log::error("[World E2E] FAIL map click coordinate conversion");testFailed=true;engine.sceneTree().quit();return;
                    }
                    select(sx,sy);zoomMap(2);
                    const r1::MapPoint zoomClick=r1::MapView(sx,sy,mapZoomLevel()).pointAt(10,10);
                    testClick("map");
                    if(std::abs(pickLon-zoomClick.lon)>1e-5||std::abs(pickLat-zoomClick.lat)>1e-5){
                        saida::Log::error("[World E2E] FAIL detailed map click coordinate conversion");
                        testFailed=true;engine.sceneTree().quit();return;
                    }
                    select(sx,sy);testClick("street-map",false);
                    if(mapZoomLevel()!=17){
                        saida::Log::error("[World E2E] FAIL street map zoom level");testFailed=true;engine.sceneTree().quit();return;
                    }
                    zoomMap(1);
                    showCityChoices({{"Paris","Paris · France",2.3522,48.8566}});
                    testClick("city-choice-0",false);
                    if(std::abs(pickLon-2.3522)>1e-5||std::abs(pickLat-48.8566)>1e-5||
                       value("longitude")!=number(2.3522)||value("latitude")!=number(48.8566)) {
                        saida::Log::error("[World E2E] FAIL city suggestion did not fill destination");
                        testFailed=true;engine.sceneTree().quit();return;
                    }
                    field("city-query",std::string());select(sx,sy);
                    // The press alone must start the journey.
                    goCount=0;testClick("go",false);
                    if(!pending||goCount!=1){saida::Log::error("[World E2E] FAIL Go press, pending=",pending," calls=",goCount);testFailed=true;engine.sceneTree().quit();return;}
                    // A full press+release must still act exactly once.
                    testClick("go");
                    if(goCount!=2){saida::Log::error("[World E2E] FAIL Go press+release ran ",goCount," times");testFailed=true;engine.sceneTree().quit();return;}
                    saida::Log::info("[World E2E] map click and Go passed (press-only and press+release)");
                    // Last: the screen it hides is still laid out over the menu
                    // until the next frame, as after any menu change (hopArmed).
                    if(!smokeOptions())return;
                }
            }
        }
        clock+=delta;
        applyForcedTime();
        updateCityLookup();
        if(menu&&zoom>1&&menuTiles.pump())renderMapTiles();
        if(smoke && pending && refused==tileAt(pickLon,pickLat).key()) {
            saida::Log::error("[World E2E] FAIL ",refused," did not fit the resident vertex budget");
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(smoke){testElapsed+=delta;if(testElapsed>(hopWanted?420:smokeFly?300:180)){saida::Log::error("[World E2E] FAIL data timeout");testFailed=true;engine.sceneTree().quit();return;}}
        if(testResume){
            resumeWait+=delta;
            if(resumeWait>.25){
                testClick("resume");double d=glm::length(ecef(lon,lat,alt)-smokeStart);
                testFailed=d<=2||menu||!smokeRan||!smokeJumped;
                saida::Log::info("[World E2E] ",testFailed?"FAIL":"PASS"," walking=",d,"m resident=",loaded.size()," resume=",!menu);
                engine.sceneTree().quit();
            }
            return;
        }
        if(hopArmed) {
            // A menu that has just been shown has not been laid out yet, and an
            // element's absolute offset is still the hidden document's until it
            // has. Clicking in that frame aims at the top-left corner and hits
            // nothing -- which looks exactly like a dead button, and is not one.
            hopWait+=delta;
            if(hopWait>.3) {
                hopArmed=false;testClick("go",false);
                if(!pending){saida::Log::error("[World E2E] FAIL the second Go did nothing");testFailed=true;engine.sceneTree().quit();return;}
                saida::Log::info("[World E2E] teleporting to ",hopLon,", ",hopLat);
            }
        }
        bool key=(!performanceDebug&&engine.window().keyDown(GLFW_KEY_M))||engine.window().keyDown(GLFW_KEY_ESCAPE);
        if(key&&!wasMenuKey){if(menu&&playing){pending=false;warming=false;request(lon,lat);showMap(false);}else showMap(true);}wasMenuKey=key;
        poll+=delta;hud+=delta;
        if(poll>(pending?.016:.10)){poll=0;if(pending||playing||warming)cost.stream=timed([&]{stream();});}
        cost.parts=timed([&]{uploadParts();});
        if(!playing)cost.warm=timed([&]{warm();});
        if(!playing||menu)return;
        updateInteriors(delta);
        const bool fast=(driving&&std::abs(carSpeed)>kFastDetail)||(sailing&&std::abs(boat.speed)>kFastDetail)
                        ||(piloting&&std::abs(plane.speed)>kFastDetail);
        seaTime+=delta;
        // §5, "le détail suit la vitesse": above 15 km/h the plan drops L5 --
        // mobilier, clutter, détail de façade -- and shrinks the rest. Street
        // furniture is streamed in rather than drawn from a pool, so dropping
        // it means not spending the frame's 2 ms importing what the player is
        // about to leave behind. It resumes the moment he slows down. The
        // signs do not wait: they are what a driver reads (gen/predict.hpp).
        ring=nearby(lon,lat);
        const uint64_t moved=engine.sceneTree().world().indexedNodesTotal()-indexedAtFrame;
        indexedAtFrame=engine.sceneTree().world().indexedNodesTotal();
        churn+=double(moved);churnFrames+=1;dirtyFrames+=moved?1:0;
        cost.props=timed([&]{streamProps(fast);});
        cost.distant=timed([&]{updateFar();updateFarPack();updateFarRelief();});
        cost.world=timed([&]{updateNature(fast?.55:1.);updateTraffic(delta);updateCrowd(delta);updateSea(delta);bendGrass();});
        conditionsRead+=delta;
        if(conditionsRead>.5){conditionsRead=0;readConditions();}
        if((farPack.node!=nullptr)!=fogFar)applyWeather();
        // The view reaches the horizon where something is drawn out to it:
        // 4.7 km at eye height, 113 km from a thousand metres, never past
        // the far pack. Elsewhere it stops at the 5 km haze, as it always has.
        camera->farZ=farPack.node?float(std::clamp(std::sqrt(2.*r1::kRMean*std::max(2.,alt+10.))*1.2+1500.,5000.,r1::kFarPackRadius+5000.)):5000.f;
        // Out to the farthest ring drawn: a summit 200 km off stands above
        // the horizon from a beach, and the haze, not the far plane, hides it.
        if(farRelief.node&&farRelief.node->visible()&&farRelief.reach>0)
            camera->farZ=std::max(camera->farZ,float(farRelief.reach+2000.));
        updateSnow(delta);
        if(checkStanding)keepStanding();
        updateSnowCover();
        engine.window().setCursorCaptured(true);
        auto mouse=saida::Input::mouseDelta();
        // At the wheel the mouse does not steer -- the car does -- so it turns
        // the head instead: a free yaw around the car, which is what you want
        // at a junction and what the first drive was missing entirely. It
        // recentres behind the car once the mouse stops and the car is rolling,
        // so the default view is still the road ahead.
        if(driving||sailing||piloting) {
            if(std::abs(mouse.x)>1e-4){lookYaw=wrap(lookYaw+mouse.x*.12);lookIdle=0;}
            else lookIdle+=delta;
            if(lookIdle>.6&&std::abs(driving?carSpeed:sailing?boat.speed:plane.speed)>2.)
                lookYaw-=lookYaw*(1-std::exp(-2.2*std::min(.05,double(delta))));
        } else yaw+=mouse.x*.12;
        pitch=std::clamp(pitch-mouse.y*.12,-65.,25.);
        auto& w=engine.window();
        double f=(w.keyDown(GLFW_KEY_W)||w.keyDown(GLFW_KEY_Z)?1.:0.)-(w.keyDown(GLFW_KEY_S)?1.:0.);
        double r=(w.keyDown(GLFW_KEY_D)?1.:0.)-(w.keyDown(GLFW_KEY_A)||w.keyDown(GLFW_KEY_Q)?1.:0.);
        if(smokeStarted&&worldCapture.pngPath.empty()){f=1;r=0;smokeWalk+=std::min(.05,double(delta));}
        if(retailTest())f=r=0;
        if(performanceSmoke){f=r=0;smokeStarted=false;}
        if(smokeApproach) {
            auto to=origin.local(ecef(carLon,carLat,carAlt))-origin.local(ecef(lon,lat,alt));
            yaw=std::atan2(to.x,-to.z)/rad;f=1;r=0;
            smokeApproachTime+=std::min(.05,double(delta));
        }
        double length=std::hypot(f,r),dt=std::min(.05,double(delta));
        // One door key for both directions, and the refusals are inside
        // enterCar/leaveCar so that every one of them says something.
        bool doorKey=w.keyDown(GLFW_KEY_F);
        if(doorKey&&!wasEnterKey) {
            if(driving)leaveCar();
            else if(sailing){if(hopAboard()==HopResult::NoTarget)leaveBoat();}
            else if(piloting)leaveAircraft();
            else if(swimming){
                if(!enterBoat()&&!enterAircraft())text("stream-status","Aucun bateau à portée — nagez vers une embarcation.");
            }
            else {
                // The nearest of a car, a boat and an aircraft: one key for every vehicle.
                Reach carReach;BoatReach boatReach;PlaneReach planeReach;double carAway=0,boatAway=0,planeAway=0;
                const bool car=nearestCar(carReach,carAway),vessel=nearestBoat(boatReach,boatAway);
                const bool aircraft=nearestAircraft(planeReach,planeAway);
                if(aircraft&&(!car||planeAway<carAway)&&(!vessel||planeAway<boatAway))enterAircraft();
                else if(vessel&&(!car||boatAway<carAway))enterBoat();
                else if(!enterCar()) {
                    // Pressing the door key from across the street is a question,
                    // and it gets an answer rather than silence.
                    if(carParked||!parked.empty()||trafficLive()>0)
                        text("stream-status",car
                             ?"Portière bloquée."
                             :"Aucun véhicule à portée — approchez-vous.");
                }
            }
        }
        wasEnterKey=doorKey;
        bool moving=false;
        footEast=footNorth=0;
        stagger=std::max(0.,stagger-dt);
        // His feet are a body while he is: not at the wheel, at the helm or
        // at the controls, where people meet the vehicle instead.
        if(feet->enabled()!=player->enabled())feet->setEnabled(player->enabled());
        if(driving) {
            if(smokeDriving){f=smokeBrake?-1.:1.;r=0;}
            driveCar(dt,f,r,w.keyDown(GLFW_KEY_SPACE)&&!smokeDriving);
            moving=std::abs(carSpeed)>.2;
        } else if(sailing) {
            if(smokeSailing){f=smokeSailBrake?-1.:1.;r=0;}
            sailBoat(dt,f,r);
            moving=std::abs(boat.speed)>.2;
        } else if(piloting) {
            double up=(w.keyDown(GLFW_KEY_SPACE)||w.keyDown(GLFW_KEY_UP)?1.:0.)
                     -(w.keyDown(GLFW_KEY_LEFT_SHIFT)||w.keyDown(GLFW_KEY_LEFT_CONTROL)||w.keyDown(GLFW_KEY_DOWN)?1.:0.);
            if(smokeFly){f=smokeF;r=smokeR;up=smokeUp;}
            if(plane.helicopter())flyHelicopter(dt,f,r,up);else flyPlane(dt,f,r,up);
            lon=plane.lon;lat=plane.lat;alt=plane.alt;yaw=plane.yaw;
            moving=std::abs(plane.speed)>.2||plane.airborne;
            request(lon,lat);
        } else if(swimming) {
            swimTime+=dt;
            if(smokeSwimming){f=1.;r=0.;length=1.;}
            if(smokeWaterSpawn){f=1.;r=0.;length=1.;smokeWalk+=dt;}
            if(length>0) {
                const double speed=w.keyDown(GLFW_KEY_LEFT_SHIFT)?2.2:1.5;
                const double east=(sin(yaw*rad)*f+cos(yaw*rad)*r)/length*speed*dt;
                const double north=(cos(yaw*rad)*f-sin(yaw*rad)*r)/length*speed*dt;
                const auto next=onward(lon,lat,east,north);
                if(tile(next.x,next.y)) {
                    if(navigable(next.x,next.y)) {
                        lon=next.x;lat=next.y;alt=waterLevel(lon,lat);moving=true;
                        swimHeading=wrap(std::atan2(east,north)/rad);
                    } else if(!blocked(next.x,next.y,alt)) {
                        lon=next.x;lat=next.y;alt=height(lon,lat,alt);swimming=false;moving=true;
                        text("stream-status","À terre.");
                        saida::Log::info("[World swim] ashore at ",lon,", ",lat);
                    }
                    if(moving)swimHeading=wrap(std::atan2(east,north)/rad);
                } else text("stream-status","Les données suivantes arrivent… nage retenue au bord du terrain.");
            }
            if(moving)request(lon,lat);
        } else if(length>0&&jumpOffset<3.) {  // falling from higher, he cannot steer
            double speed=w.keyDown(GLFW_KEY_LEFT_SHIFT)?7.:2.8;
            // A stride broken by running into someone (World::staggerPlayer).
            if(stagger>0)speed*=.35;
            double east=(sin(yaw*rad)*f+cos(yaw*rad)*r)/length*speed*dt;
            double north=(cos(yaw*rad)*f-sin(yaw*rad)*r)/length*speed*dt;
            footEast=east/dt;footNorth=north/dt;
            // People are bodies: he stops at them and slides round them.
            auto next=moveFeet(east,north,dt);
            if(tile(next.x,next.y)) {
                if(onWater(next.x,next.y,alt)) {
                    lon=next.x;lat=next.y;alt=waterLevel(lon,lat);
                    swimming=true;swimTime=0;swimLean=0;
                    swimHeading=wrap(std::atan2(east,north)/rad);
                    jumpOffset=jumpVelocity=0;moving=true;
                    text("stream-status","À l'eau — nagez vers la rive.");
                    saida::Log::info("[World swim] entered water at ",lon,", ",lat);
                } else {
                    lon=next.x;lat=next.y;alt=std::max(height(lon,lat,alt),next.z);moving=true;
                    auto facing=glm::angleAxis(float(-std::atan2(east,north)),glm::vec3(0,1,0));
                    player->transform().rotation=glm::slerp(player->transform().rotation,facing,float(1-std::exp(-16*dt)));
                }
            }else text("stream-status","Les données suivantes arrivent… déplacement retenu au bord du terrain.");
            request(lon,lat);
        } else if(jumpOffset<=0) {
            // Standing still, he is still a body: whoever walks into him
            // meets his feet, and a shoulder moves him a little.
            const auto nudged=moveFeet(0,0,dt);
            if(tile(nudged.x,nudged.y)&&!onWater(nudged.x,nudged.y,alt)){lon=nudged.x;lat=nudged.y;alt=std::max(height(lon,lat,alt),nudged.z);}
        }
        if(glm::length(origin.local(ecef(lon,lat,alt)))>kRebaseDistance)rebaseOrigin();
        if(!driving&&!sailing&&!piloting) {
            if(swimming) {
                jumpOffset=jumpVelocity=0;wasJump=false;
                swimLean+=(double(moving)-swimLean)*(1-std::exp(-5*dt));
                const double lean=35.*rad*swimLean;
                const double heave=.035*std::sin(swimTime*4.);
                const double rootBelow=playerHeight*(std::cos(lean)-kSwimHeadAbove);
                player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt-rootBelow+heave)));
                player->transform().rotation=glm::angleAxis(float(-swimHeading*rad),glm::vec3(0,1,0))
                                             *glm::angleAxis(float(-lean),glm::vec3(1,0,0));
                for(auto* a:animators)a->play(moving?"run":"idle");
            } else {
                bool jump=w.keyDown(GLFW_KEY_SPACE);
                if(smokeStarted&&worldCapture.pngPath.empty()&&smokeWalk>.6&&smokeWalk<.8)jump=true;
                if(jump&&!wasJump&&jumpOffset<=0)jumpVelocity=std::sqrt(2*22.*1.5);
                wasJump=jump;
                const double previous=jumpOffset;
                if(jumpVelocity-22*dt>-kFallTerminal){jumpOffset+=jumpVelocity*dt-11*dt*dt;jumpVelocity-=22*dt;}
                else{jumpVelocity=-kFallTerminal;jumpOffset+=jumpVelocity*dt;}
                if(jumpOffset<=0) {
                    // Come down on water, from an aircraft: he swims.
                    if(jumpVelocity<-5.&&tile(lon,lat)&&onWater(lon,lat,alt)) {
                        swimming=true;swimTime=0;swimLean=0;swimHeading=yaw;alt=waterLevel(lon,lat);
                        text("stream-status","À l'eau — nagez vers la rive.");
                    }
                    jumpOffset=0;jumpVelocity=0;
                }
                if(jumpOffset>0) {
                    feet->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+previous+.06)));
                    const auto end=feet->moveAndSlide({0,float((jumpOffset-previous)/dt),0},float(dt));
                    const double resolved=end.y-origin.local(ecef(lon,lat,alt+.06)).y;
                    if(resolved<jumpOffset-.01&&jumpVelocity>0)jumpVelocity=0;
                    jumpOffset=std::max(0.,resolved);
                    if(jumpVelocity<0&&feet->isOnFloor()) {
                        alt+=jumpOffset;jumpOffset=jumpVelocity=0;
                    }
                }
                player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+jumpOffset+.06)));
                const bool sprint=moving&&w.keyDown(GLFW_KEY_LEFT_SHIFT);
                for(auto* a:animators)a->play(jumpOffset>0?"jump":sprint?"sprint":moving?"run":"idle");
            }
        }
        updateSinkingCar(dt);
        placeCar();
        placeBoats();
        placeAircraft(dt);
        if(smokeStarted&&worldCapture.pngPath.empty()){
            smokeRan=smokeRan||(moving&&animators.front()->currentClip()=="run");
            smokeJumped=smokeJumped||(jumpOffset>.5&&animators.front()->currentClip()=="jump");
        }
        // At the controls of a plane the view pitches with the nose, a little.
        const double viewPitch=std::clamp(pitch+(piloting&&!plane.helicopter()?plane.pitch*.6:0.),-80.,40.);
        camera->transform().rotation=glm::angleAxis(float(-(yaw+lookYaw)*rad),glm::vec3(0,1,0))*glm::angleAxis(float(viewPitch*rad),glm::vec3(1,0,0));
        // A car is longer than a man and moves twice as fast, so the camera
        // stands further back -- far enough to see the bonnet turn.
        // A boat's camera stands off by its own length: a container ship is
        // two hundred metres of hull, and eight metres behind its bridge is
        // inside it.
        const double maxFollow=sailing?std::clamp(boat.length*1.1+6.,9.,230.)
                              :piloting?plane.type->length*.9+10.
                              :driving?std::max(kDrivingFollow,vehicleSpec(*car).length*.7+5.):kOnFootFollow;
        const double eye=sailing?std::clamp(boat.length*.12+2.,2.,26.):piloting?plane.type->height*.75+1.5:driving?vehicleSpec(*car).height+.5:swimming?playerHeight*1.17:playerHeight*.89;
        const glm::vec3 anchor=sailing&&boat.node?boat.node->transform().position
                              :piloting&&plane.node?plane.node->transform().position
                              :driving?car->transform().position:player->transform().position;
        const glm::vec3 target=anchor+glm::vec3(0,eye,0);
        const glm::vec3 backward=camera->transform().rotation*glm::vec3(0,0,1);
        double clear=maxFollow;
        if(auto* physics=engine.sceneTree().world().physics()) {
            const auto hit=physics->raycast(target,backward,float(maxFollow),obstacleFilter());
            if(hit.hit)clear=std::max(.15,double(hit.distance)-.3);
        }
        // Keep the camera above the geographic surface and inside loaded tiles.
        for(double d=.25;d<=clear;d+=.25){
            auto q=onward(lon,lat,backward.x*d,-backward.z*d);
            if(!tile(q.x,q.y)&&piloting)continue;  // in the air, unstreamed ground ahead is no wall
            if(!tile(q.x,q.y)||height(q.x,q.y,alt+jumpOffset)+.3>alt+jumpOffset+.06+eye+backward.y*d){clear=std::max(.15,d-.25);break;}
        }
        followDistance=clear<followDistance?clear:followDistance+(clear-followDistance)*(1-std::exp(-7*dt));
        camera->transform().position=target+backward*float(followDistance);
        if(smoke&&!worldCapture.pngPath.empty()) {
            if(captureView.set) {
                // The viewpoint is relative to where the player stands now, not
                // to the origin of the spawn: a first visit spawns on ground
                // that the surveyed relief later raises (Kyoto: 0 m, then the
                // hillside), and a camera 30 m above the old ground is inside
                // the hill.
                glm::vec3 at=player->transform().position;
                if(captureAltitude)at.y+=float(*captureAltitude-alt);
                glm::vec3 position=at+glm::vec3(captureView.position[0],captureView.position[1],captureView.position[2]);
                glm::vec3 target=at+glm::vec3(captureView.target[0],captureView.target[1],captureView.target[2]);
                if(captureGeo) {
                    // A surveyed inspection camera may overlap the spawn.
                    // Keep the player active for streaming, hide only its mesh.
                    player->setVisible(false);
                    const auto& geo=*captureGeo;
                    const double eye=captureAltitude?*captureAltitude:height(geo[0],geo[1])+geo[2];
                    Frame photo(geo[0],geo[1],eye);
                    position=origin.local(photo.origin);
                    const glm::dvec3 direction(captureView.target[0]-captureView.position[0],
                        captureView.target[1]-captureView.position[1],captureView.target[2]-captureView.position[2]);
                    target=origin.local(photo.origin+photo.basis*direction);
                }
                camera->transform().position=position;
                camera->transform().rotation=glm::quatLookAt(glm::normalize(target-position),glm::vec3(0,1,0));
            }
            auto want=nearby(lon,lat);
            captureWait+=dt;
            // A first visit mounts its ground before its streets: a picture of
            // that is a picture of nothing. The capture waits for OSM, and
            // after two minutes takes what there is and says so.
            // The same for the surveyed summits: a mountain photographed
            // before its summit list landed is the relief they correct.
            size_t provisional=0,summitsPending=0;
            // And the rings to the horizon, where they are drawn.
            const bool farPending=farRelief.node&&farRelief.node->visible()&&
                (!farRelief.job||[&]{std::lock_guard<std::mutex> g(farRelief.job->lock);return farRelief.job->busy||!farRelief.job->done.empty();}()
                 ||[&]{for(int k=0;k<kFarLevels;++k)if(!farRelief.node->level(k))return true;return false;}());
            bool settled=engine.resources().assetLoadsSettled()&&std::all_of(want.begin(),want.end(),[&](Tile t){
                auto i=loaded.find(t.key());if(i==loaded.end())return false;
                if(i->second.data.value("provisional",false))++provisional;
                if(i->second.data.contains("peaks")&&i->second.data.at("peaks").value("pending",false))++summitsPending;
                // Its meshes too: a tile cooked again uploads them over
                // several frames (uploadParts), and its props may be done first.
                const auto& resident=i->second;
                if(!resident.geography||resident.nextPart<std::any_cast<const PreparedTile&>(resident.served->prepared).parts.size())return false;
                return resident.nextProp==resident.props.size()&&
                    resident.nextLettering==resident.data.value("lettering",json::array()).size();
            });
            // A capture that waits says what for, every ten seconds: otherwise
            // the only report is the smoke's data timeout, which blames the
            // network for whatever it was (CLAUDE.md §3).
            captureReport+=dt;
            if(!settled&&!captureQueued&&captureReport>=10.) {
                captureReport=0;
                std::string why;
                for(Tile t:want) {
                    auto i=loaded.find(t.key());
                    if(i==loaded.end()){why+=" "+t.key()+" not mounted;";continue;}
                    const auto& r=i->second;
                    const size_t parts=r.served?std::any_cast<const PreparedTile&>(r.served->prepared).parts.size():0;
                    if(!r.geography)why+=" "+t.key()+" no geography;";
                    else if(r.nextPart<parts)why+=" "+t.key()+" parts "+std::to_string(r.nextPart)+"/"+std::to_string(parts)+";";
                    else if(r.nextProp<r.props.size())why+=" "+t.key()+" props "+std::to_string(r.nextProp)+"/"+std::to_string(r.props.size())+";";
                }
                saida::Log::info("[World capture wait] ",int(captureWait)," s, not settled:",why.empty()?" every tile is in":why);
            }
            // Summits refine a relief that is already there: a minute less.
            if(settled&&((provisional&&captureWait<120.)||((summitsPending||farPending)&&captureWait<45.)))settled=false;
            if(settled&&provisional&&!captureQueued)
                saida::Log::error("[World capture] ",provisional," of ",want.size(),
                                  " tiles still wait for OSM after two minutes; photographed as they are");
            if(settled&&summitsPending&&!captureQueued)
                saida::Log::error("[World capture] ",summitsPending," of ",want.size(),
                                  " tiles still wait for their surveyed summits after 45 s; photographed as they are");
            // R1WORLD_CAPTURE_SEA: a picture of the sea waits for its ships
            // (half a minute at most -- the log then says there were none).
            if(settled)captureSeaWait+=dt;
            const bool seaReady=(!std::getenv("R1WORLD_CAPTURE_SEA")||(!seaShips.empty()&&captureSeaWait>4.)
                ||captureSeaWait>30.)
                // The local weather is part of the picture: a few seconds for it.
                &&(weather.known||inspectAt||captureSeaWait>6.);
            if(settled&&seaReady&&!captureQueued){
                // An inspection picture is of the world: no HUD, no minimap.
                if(inspectAt){style("hud","display","none");minimapUi->setEnabled(false);}
                size_t plants=0;for(auto& [key,t]:loaded)plants+=t.vegetation.size();
                saida::Log::info("[World nature] resident plants=",plants," shared static prototypes=",naturePrototypes.size());
                saida::Log::info("[World traffic] resident cars=",trafficLive()," of ",trafficWanted()," asked for");
                const auto& rendering=engine.sceneTree().world().settings();
                saida::Log::info("[World PBR] ibl=",rendering.iblEnabled," specular=",rendering.iblSpecularIntensity,
                                 " sky=",rendering.skyboxTexture," exposure=",rendering.skyboxExposure);
                car->traverse([&](saida::Node& n,const glm::mat4&){
                    if(n.material()&&n.name().rfind("paint-",0)==0) {
                        const auto& m=n.material()->desc();
                        saida::Log::info("[World PBR] ",n.name()," metallic=",m.metallic," roughness=",m.roughness,
                                         " type=",int(m.type)," rgb=",m.baseColor.r,",",m.baseColor.g,",",m.baseColor.b);
                    }
                });
                captureQueued=true;engine.captureFrameThenExit(worldCapture);
            }
        }
        if(hud>.5) {
            hud=0;
            updateMinimap();
            text("local-conditions",weatherLabel());
            text("coordinates",number(lat)+"°  /  "+number(lon)+"°     "+number(alt,1)+" m"
                 +(driving?"     "+std::to_string(int(std::round(std::abs(carSpeed)*3.6)))+" km/h":"")
                 +(sailing?"     "+std::to_string(int(std::round(std::abs(boat.speed)*1.943844)))+" nœuds":"")
                 +(piloting?"     "+std::to_string(int(std::round(std::abs(plane.speed)*3.6)))+" km/h · "
                    +std::to_string(int(std::round(plane.alt-planeGround)))+" m sol"
                    +(plane.helicopter()?"":" · gaz "+std::to_string(int(std::round(plane.lever*100)))+" %"):""));
            Reach within;BoatReach moored;PlaneReach standing;double howFar=0,boatFar=0,planeFar=0;
            std::string mode=driving
                ?" · Au volant · F : descendre"
                :piloting?std::string(plane.helicopter()
                    ?" · Hélicoptère · Espace/Maj : monter/descendre · Z/S : avancer/reculer · Q/D : tourner · F : descendre"
                    :" · Avion · Z/S : gaz · Q/D : virer · Espace/Maj : cabrer/piquer · F : descendre")
                  +(tile(lon,lat)?"":" · relief inconnu sous l'appareil")
                :sailing?" · À la barre · F : débarquer ou passer à bord"
                :swimming?" · À l'eau · ZQSD/WASD : nager · F : remonter à bord"
                :nearestAircraft(standing,planeFar)?" · F : prendre l'appareil"
                :nearestBoat(moored,boatFar)?" · F : prendre le bateau"
                :(nearestCar(within,howFar)?" · F : monter dans la voiture":performanceDebug?" · Échap : carte":" · M : carte");
            const auto* currentTile=tile(lon,lat);
            if(currentTile&&currentTile->data.value("provisional",false))
                mode+=" · Rues et bâtiments en route (OpenStreetMap)";
            if(currentTile&&currentTile->data.value("groundPending",false))
                mode+=" · Relief provisoire : altitude en cours de chargement";
            std::string retailPlace;
            for(const auto& [key,t]:loaded) {
                auto p=t.frame.local(ecef(lon,lat,alt));const r1::P2 q{p.x,p.z};
                for(const auto& room:t.interiors)if(r1::pointInPolygon(q,room.plan.ring))retailPlace=room.plan.place(q);
                if(!retailPlace.empty())break;
                for(const auto& lot:t.parking){bool in=false;for(const auto& ring:lot.rings)in^=r1::pointInPolygon(q,ring);
                    if(in){retailPlace="Parking de "+lot.name;break;}}
            }
            if(!retailPlace.empty())mode=" · "+retailPlace+mode;
            text("stream-status",std::to_string(loaded.size())+" tuiles actives"+mode
                 +(fast?" · détail réduit à cette vitesse":"")
                 +(physicsRefused?" · collisions incomplètes : le monde physique est plein":""));
        }
        if(performanceSmoke){runPerformanceSmoke(delta);return;}
        if(smokeWaterSpawn&&smokeWalk>1.5) {
            const double covered=glm::length(ecef(lon,lat,alt)-smokeStart);
            const double rootBelow=origin.local(ecef(lon,lat,alt)).y-player->transform().position.y;
            testFailed=!swimming||!onWater(lon,lat)||carParked||covered<1.||rootBelow<playerHeight*.39||rootBelow>playerHeight*.78;
            saida::Log::info("[World E2E] ",testFailed?"FAIL":"PASS"," water spawn: swam ",covered,
                             "m, body depth=",rootBelow,"m, car parked=",carParked);
            engine.sceneTree().quit();return;
        }
        if(retailTest()){runRetailTest(delta);return;}
        if(smokeFlyWait||smokeFlyPhase){runSmokeFly(delta);return;}
        if(smokeSailWait||smokeSailing){runSmokeSail(delta);return;}
        if(smokeSwimming){runSmokeSwim(delta);return;}
        if(smokeSeaWait){runSmokeSea(delta);return;}
        if(smokeStarted&&smokeWalk>4) {
            if(!smokeRan||!smokeJumped||jumpOffset!=0||followDistance<.15||followDistance>kOnFootFollow+.001){
                saida::Log::error("[World E2E] FAIL player animation/jump/follow");testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] player run/jump/landing/follow passed, distance=",followDistance);
            sayBodies();
            if(!trunksSolid()||!groundMatchesDrawing()||!seamsClosed()){testFailed=true;engine.sceneTree().quit();return;}
            // A neighbourhood that asked for people and shows none is the
            // failure; a moor at night that asked for none is not one.
            if(bodiesRefused>0||feetRefused) {
                saida::Log::error("[World E2E] FAIL crowd: ",bodiesRefused," people cannot look or stagger",
                                  feetRefused?", and the player's feet are no physics body":"");
                testFailed=true;engine.sceneTree().quit();return;
            }
            if(crowdWanted()>0&&crowdLive()==0) {
                saida::Log::error("[World E2E] FAIL crowd: ",crowdWanted()," people asked for, none placed");
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] crowd: ",crowdLive()," people of ",crowdWanted(),
                             " asked for at ",localSolarHour(),"h solar (factor ",crowdFactor(),"), bumps ",playerBumps);
            smokeStarted=false;
            if(!smokeDroveOnce&&!onSeaIce(lon,lat)){startSmokeDrive();return;}
            if(!smokeDroveOnce)saida::Log::info("[World E2E] on the sea ice: no car to drive, the walk is the test");
            smokeFinishPhase();
        }
        if(smokeApproach) {
            if(carDistance()<=kCarReach*.8){smokeApproach=false;beginSmokeDriving();return;}
            if(smokeApproachTime>20.) {
                // What stands in the way, said: the next step toward the car.
                auto to=origin.local(ecef(carLon,carLat,carAlt))-origin.local(ecef(lon,lat,alt));
                const double l=std::max(1e-6,std::hypot(to.x,to.z));
                const auto q=onward(lon,lat,to.x/l*.5,-to.z/l*.5);
                saida::Log::error("[World E2E] FAIL could not walk back to the car, still ",
                                  carDistance(),"m away after ",smokeApproachTime,"s; player at ",lon,", ",lat,
                                  " alt ",alt,", car alt ",carAlt,"; next step: water=",onWater(q.x,q.y,alt),
                                  " obstacle=",blocked(q.x,q.y,alt)," blocked=",blocked(q.x,q.y,alt),
                                  " level there ",height(q.x,q.y,alt));
                testFailed=true;engine.sceneTree().quit();
            }
            return;
        }
        if(smokeDriving&&smokeBrake) {
            smokeDriveTime+=std::min(.05,double(delta));
            // The door refusing to open at 76 km/h is the feature working, and
            // the driver answers it the way a driver does: it brakes. A test
            // that stepped out at speed would be testing a car this one is not.
            if(std::abs(carSpeed)>kCarExitSpeed) {
                if(smokeDriveTime>20.) {
                    saida::Log::error("[World E2E] FAIL the car would not slow down, still ",
                                      std::abs(carSpeed)," m/s");
                    testFailed=true;engine.sceneTree().quit();
                }
                return;
            }
            smokeDriving=false;
            saida::Log::info("[World E2E] braked to ",std::abs(carSpeed)," m/s");
            if(!leaveCar()||driving||!player->enabled()) {
                saida::Log::error("[World E2E] FAIL could not step out of the car at ",
                                  std::abs(carSpeed)," m/s");
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] on foot again at ",lon,", ",lat);
            if(!smokeTookOver&&trafficLive()>0){runSmokeTakeover();if(testFailed)return;}
            smokeFinishPhase();
            return;
        }
        if(smokeDriving) {
            smokeDriveTime+=std::min(.05,double(delta));
            smokeTopSpeed=std::max(smokeTopSpeed,carSpeed);
            size_t live=0;
            for(auto& [key,t]:loaded)
                for(const auto& a:t.flow.agents()) {
                    if(!a.alive)continue;
                    ++live;
                    smokeTrafficMoved=smokeTrafficMoved||a.speed>2.f;
                }
            smokeTrafficSeen=std::max(smokeTrafficSeen,live);
            // Check the exit while the car is still moving. In a dense city a
            // clear 60 m stretch can end before five seconds at full throttle.
            const double covered=glm::length(ecef(lon,lat,alt)-smokeDriveStart);
            const double needed=std::min(20.,smokeClear*.6);
            if(smokeDriveTime<2. || (smokeDriveTime<5. &&
               (covered<needed||std::abs(carSpeed)<=kCarExitSpeed)))return;
            // A street that asked for cars and got none is the failure this
            // assertion exists for -- and a spawn on a moor that asked for
            // none is not one, so the claim is checked against the ask.
            if(trafficWanted()>0&&(smokeTrafficSeen==0||!smokeTrafficMoved)) {
                saida::Log::error("[World E2E] FAIL traffic: ",trafficWanted(),
                                  " cars asked for, ",smokeTrafficSeen," seen, moving=",
                                  smokeTrafficMoved);
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] traffic: ",smokeTrafficSeen," cars driving of ",
                             trafficWanted()," the neighbourhood asked for");
            // What is actually being asserted: that the car goes somewhere, and
            // that it goes there faster than a man can run (7 m/s sprinting).
            // The distance is measured against the clearance the driver found
            // rather than against a fixed number, because a spawn is wherever
            // the map was clicked and some of them are courtyards.
            if(covered<needed||smokeTopSpeed<7.2||std::abs(carSpeed)<=kCarExitSpeed) {
                saida::Log::error("[World E2E] FAIL drive: covered=",covered,
                                  "m of ",smokeClear,"m clear, top=",smokeTopSpeed," m/s","; last stopped by ",carStop," at ",carLon,", ",carLat," alt ",carAlt);
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] drive passed, covered=",covered,
                             "m top=",smokeTopSpeed," m/s (",int(smokeTopSpeed*3.6)," km/h)");
            reportChurn("driving");measureWalk();
            // Getting out is half the feature, and its refusals are the half
            // that used to be silent everywhere else in this file. The exit at
            // speed must be refused, and then the brake must earn it.
            if(leaveCar()) {
                saida::Log::error("[World E2E] FAIL stepped out of a car doing ",
                                  std::abs(carSpeed)," m/s");
                testFailed=true;engine.sceneTree().quit();return;
            }
            smokeBrake=true;smokeDriveTime=0;
            return;
        }
    }
};

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
        bool smoke=false,sail=false,fly=false;double startLon=2.3522,startLat=48.8566;bool hop=false;double hopLon=0,hopLat=0;
        for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--project"&&i+1<argc)game=fs::absolute(argv[++i]);else if(a=="--smoke")smoke=true;else if(a=="--sail")sail=true;else if(a=="--fly")fly=true;else if(a=="--spawn"&&i+2<argc){startLon=std::stod(argv[++i]);startLat=std::stod(argv[++i]);}else if(a=="--spawn2"&&i+2<argc){hop=true;hopLon=std::stod(argv[++i]);hopLat=std::stod(argv[++i]);}}
        if(!std::isfinite(startLon)||!std::isfinite(startLat)||std::abs(startLat)>90||std::abs(startLon)>180)throw std::runtime_error("Invalid --spawn coordinate");
        // This development executable uses the existing engine's baked paths
        // for shaders/fonts, and the project's root for content.
        saida::PhysicsCapacity physics;physics.bodies=kPhysicsBodies;
        saida::Engine engine(nullptr,(game/"R1World.saidaproj").string(),false,{},physics);
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
        World world(engine,game,smoke,capture,startLon,startLat,hop,hopLon,hopLat,view,sail,fly);
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
        engine.setOnFrame([&](float dt){world.update(dt);});
        if(!smoke&&!capture.pngPath.empty())engine.captureFrameThenExit(capture);
        engine.run();return engine.captureFailed()||world.failed()?1:0;
    }catch(const std::exception& e){saida::Log::error("R1World: ",e.what());return 1;}
}
