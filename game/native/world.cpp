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
#include "scene/animation/Animator.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Material.hpp"
#include "nodes/CameraNode.hpp"
#include "nodes/MeshNode.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "nodes/WebCanvasNode.hpp"
#include "scripting/ScriptBehaviour.hpp"
#include "runtime/CaptureArgs.hpp"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include "saida/traffic/Traffic.hpp"
#include "gen/landmarks.hpp"
#include "gen/palette.hpp"
#include "gen/sea.hpp"
#include "gen/service.hpp"
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
#include <functional>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr double rad = 3.141592653589793 / 180.;
constexpr double kTwoPi = 6.283185307179586;
double wrap(double x) { return x - 360.*std::floor((x+180.)/360.); }
int columns(int r) { return std::max(1, int(std::nearbyint(72000.*std::cos((-90.+(r+.5)*.005)*rad)))); }
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
constexpr double kPlayerHeight=1.8;     // measured from player.glb's mesh accessor
constexpr double kSwimHeadAbove=.60;    // keep the head and neck clear of the water
// §5: "on ne voit pas les poignées de porte à 130 km/h". 15 km/h is where
// the plan's own table stops calling it walking.
constexpr double kFastDetail=4.2;       // m/s
constexpr double kStreamAheadSeconds=45.; // prepare the road before the car reaches it
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
constexpr double kAircraftExitSpeed=2.; // m/s -- above it, leaving is refused
constexpr size_t kLeftAircraft=4;
constexpr double kGravity=9.81;
// How many traffic cars the whole neighbourhood may show at once. Not a memory
// budget -- every one of them is the same shared mesh (§5) -- but a draw-call
// one: a car is five primitives, so forty cars is two hundred draws, which is
// what the reference machine can spare beside a city.
constexpr size_t kTrafficCars=80;
// How many cars the player may leave standing around before the oldest is
// cleared. They cost a node each and nothing else -- the mesh is shared -- but
// a city paved with the player's abandoned cars is its own kind of wrong.
constexpr size_t kAbandonedCars=6;

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
    int r=std::clamp(int(std::floor((lat+90.)*200.)),0,35999);
    return {r,std::min(columns(r)-1,int(std::floor((wrap(lon)+180.)/360.*columns(r))))};
}
std::vector<Tile> nearby(double lon,double lat) {
    Tile center=tileAt(lon,lat);
    std::vector<Tile> out{center}; std::set<Tile> seen{center};
    for(int dr=-1;dr<=1;++dr) {
        int r=std::clamp(center.r+dr,0,35999),n=columns(r);
        int c=int(std::floor((wrap(lon)+180.)/360.*n));
        for(int dc=-1;dc<=1;++dc) {
            Tile t{r,(c+dc+n)%n}; if(seen.insert(t).second)out.push_back(t);
        }
    }
    // Center first, then the closest terrain: the next boundary matters more
    // than the arbitrary row/column iteration order. Wrap at the date line.
    auto distance=[&](Tile t) {
        double p=-90.+(t.r+.5)*.005;
        double l=-180.+(t.c+.5)*360./columns(t.r);
        return std::hypot(wrap(l-lon)*std::cos(lat*rad),p-lat);
    };
    std::stable_sort(out.begin()+1,out.end(),[&](Tile a,Tile b){return distance(a)<distance(b);});
    return out;
}
glm::dvec3 ecef(double lon,double lat,double alt=0) {
    double s=std::sin(lat*rad), c=std::cos(lat*rad), n=6378137./std::sqrt(1.-.0066943799901413165*s*s);
    return {(n+alt)*c*std::cos(lon*rad),(n+alt)*c*std::sin(lon*rad),(n*(1.-.0066943799901413165)+alt)*s};
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
// Great-circle step; finite at both poles, longitude wraps across the date line.
glm::dvec2 advance(double lon,double lat,double east,double north) {
    double d=std::hypot(east,north)/6371008.8;
    if(d==0)return {lon,lat};
    double bearing=std::atan2(east,north),p=lat*rad;
    double q=std::asin(std::clamp(sin(p)*cos(d)+cos(p)*sin(d)*cos(bearing),-1.,1.));
    double l=lon*rad+std::atan2(sin(bearing)*sin(d)*cos(p),cos(d)-sin(p)*sin(q));
    return {wrap(l/rad),q/rad};
}
struct Plant {
    saida::Node* node=nullptr;
    bool grass=false;
    bool tree=false;
};
struct Footprint {
    std::vector<glm::dvec2> points;
    glm::dvec2 low{1e30},high{-1e30};
    double top=1e30;  // the highest point over it, in its tile's frame: what an aircraft clears
};
// A walkable deck over the water -- a pier -- in its tile's frame.
struct Deck {
    std::vector<glm::dvec2> points;
    glm::dvec2 low{1e30},high{-1e30};
    double y=0;
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
};
// Everything the frame needs of a cooked tile, made on the worker that cooked
// it: the parts as the GPU takes them, and the footprints `blocked` walks.
struct PreparedTile {
    std::vector<PartUpload> parts;
    std::vector<Footprint> footprints;
};

PreparedTile prepareTile(const r1::CookedTile& tile) {
    PreparedTile prepared;
    auto& out=prepared.parts;
    out.reserve(tile.parts.size());
    for(size_t i=0;i<tile.parts.size();++i) {
        const r1::MeshPart& part=tile.parts[i];
        const r1::Mesh& m=part.mesh;
        const double k=part.material.uvScale;
        const auto tangents=r1::tangents(m,k);
        PartUpload up{part.name,{},m.indices,i};
        up.vertices.resize(m.positions.size());
        for(size_t v=0;v<m.positions.size();++v) {
            saida::Vertex& x=up.vertices[v];
            x.pos=glm::vec3(m.positions[v].x,m.positions[v].y,m.positions[v].z);
            x.normal=glm::vec3(m.normals[v].x,m.normals[v].y,m.normals[v].z);
            x.color=glm::vec3(1.f);
            x.texCoord=glm::vec2(m.texcoords[v].u*k,m.texcoords[v].v*k);
            x.tangent=glm::vec4(tangents[v][0],tangents[v][1],tangents[v][2],tangents[v][3]);
        }
        out.push_back(std::move(up));
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

struct Loaded {
    saida::Node* node=nullptr;
    // The cooked tile this was mounted from. Its manifest and its prop list
    // are read where they are, never copied.
    std::shared_ptr<const r1::ServedTile> served;
    const json& data; Frame frame; const json& props; size_t nextProp=0;
    // The tile's own geometry goes up a few parts a frame (World::uploadParts).
    saida::Node* geography=nullptr; size_t nextPart=0;
    Loaded(saida::Node* n,std::shared_ptr<const r1::ServedTile> s)
        :node(n),served(std::move(s)),data(served->cooked.manifest),
         frame(data.at("lon").get<double>(),data.at("lat").get<double>()),props(served->cooked.props){}
    std::vector<Footprint> footprints; std::vector<Plant> vegetation;
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
    // The manifest unpacked once at mount, because a frame cannot afford to
    // read JSON. See World::unpack.
    double west=0,east=0,south=0,north=0;
    int gridSize=0; std::vector<float> elevation;
    // 0 land, 1 inland water, 2 sea-level water (see r1/harbours.Cells).
    int waterRows=0,waterCols=0; std::vector<uint8_t> water;
    bool ocean=false;
    std::vector<Deck> decks; glm::dvec2 deckLow{1e30},deckHigh{-1e30};
    std::vector<Mooring> boats;
    std::vector<AircraftSpot> aircraft;
};

class World : public Rml::EventListener {
    saida::Engine& engine; fs::path game;
    saida::WebCanvasNode* ui; saida::CameraNode* camera;
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
    saida::Node* prototypes=nullptr;
    std::map<std::string,saida::Node*> naturePrototypes;
    struct VehicleModel {
        std::string name;
        double length=3.6,width=1.94,height=1.5,wheelbase=2.38,wheelRadius=.285;
        saida::Node* prototype=nullptr;
    };
    std::vector<VehicleModel> fleet;
    std::vector<glm::vec3> paints;
    // Tiles are cooked in this process, on the service's threads (gen/).
    std::unique_ptr<r1::WorldService> service;
    std::map<std::string,saida::AssetID> textures;
    size_t residentVertexBudget=0;
    Frame origin; double lon=2.3522,lat=48.8566,alt=0,yaw=0,pitch=-12;
    double pickLon=2.3522,pickLat=48.8566,zoom=1,mapX=0,mapY=0;
    bool menu=true,playing=false,pending=false,warming=false,wasMenuKey=false;
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
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
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
    // The driver used to test one cold spawn and stop. Teleporting from a
    // place you are already standing in is a different path -- tiles are
    // evicted, the resource arena is trimmed and the origin moves half a
    // planet -- and it is the path a player takes every time after the
    // first. --spawn2 makes the run do it.
    double hopLon=0,hopLat=0,hopWait=0; bool hopWanted=false,hopDone=false,hopArmed=false;
    saida::ScriptBehaviour* sunScript=nullptr; bool sunReported=false;
    saida::CaptureRequest worldCapture;
    saida::runtime::CaptureViewpoint captureView;
    bool captureQueued=false;
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
    std::string value(const std::string& id) {
        auto* e=dynamic_cast<Rml::ElementFormControl*>(ui->findElementById(id));return e?e->GetValue():"";
    }
    // Compared with what the field shows, not with what was last written: the
    // player may have typed in it since.
    void field(const std::string& id,double x) {
        const std::string value=number(x);
        auto* e=dynamic_cast<Rml::ElementFormControl*>(ui->findElementById(id));
        if(!e||e->GetValue()==value)return;
        e->SetValue(value);
        ui->notifyJsMutation();
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
        double px=(pickLon+180)/360*1344*zoom-mapX,py=(90-pickLat)/180*560*zoom-mapY;
        style("pin","left",number(px,2)+"px");style("pin","top",number(py,2)+"px");
    }
    void showMap(bool show) {
        menu=show;style("menu","display",show?"block":"none");style("hud","display",show?"none":"block");
        style("resume","display",playing?"inline-block":"none");
        engine.window().setCursorCaptured(!show);
    }
    void zoomMap(double z) {
        zoom=std::clamp(z,1.,128.);
        mapX=std::clamp((pickLon+180)/360*1344*zoom-672.,0.,1344*(zoom-1));
        mapY=std::clamp((90-pickLat)/180*560*zoom-280.,0.,560*(zoom-1));
        style("earth","width",number(1344*zoom,1)+"px");style("earth","height",number(560*zoom,1)+"px");
        style("earth","left",number(-mapX,1)+"px");style("earth","top",number(-mapY,1)+"px");select(pickLon,pickLat);
    }
    double streamPriority(Tile t,double x,double y,double heading) const {
        const double tileLat=-90.+(t.r+.5)*.005;
        const double tileLon=-180.+(t.c+.5)*360./columns(t.r);
        const double east=wrap(tileLon-x)*111320.*std::cos(y*rad);
        const double north=(tileLat-y)*111320.;
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
    bool tellSun() {
        if(!sunScript) {
            auto* node=engine.sceneTree().firstInGroup("sun");
            if(!node)return false;
            for(auto& b:node->behaviours())
                if(auto* script=dynamic_cast<saida::ScriptBehaviour*>(b.get()))sunScript=script;
            if(!sunScript)return false;
        }
        nlohmann::json result;
        if(!worldCapture.pngPath.empty()) {
            if(sunScript->callExport("setInspectionMode",json::array({true}),result)
                    !=saida::ScriptCallStatus::Succeeded)return false;
        }
        if(sunScript->callExport("setObserver",json::array({lon,lat,alt}),result)
               !=saida::ScriptCallStatus::Succeeded)return false;
        return result.is_boolean() && result.get<bool>();
    }
    void moveSun() {
        // A failure here is a scene that keeps lighting the wrong hemisphere
        // rather than a crash, so it has to be said out loud once.
        if(tellSun()||sunReported)return;
        sunReported=true;
        saida::Log::error("[World] the sun cycle did not accept the observer; "
                          "the light stays on the scene's opening instant");
    }
    Loaded* tile(double x,double y) {
        auto i=loaded.find(tileAt(x,y).key());return i==loaded.end()?nullptr:&i->second;
    }
    // ── the tile's data, in the form the frame reads it ─────────────────────
    //
    // The tile's manifest is the contract with the generator. What
    // it must not be is the thing a hot loop reads: every `data["bounds"]` is a
    // hash lookup, every `poly[i][0]` is a bounds-checked variant unwrap, and
    // `water[row].get<std::string>()` allocates a string per query. Walking or
    // driving asks these questions tens of thousands of times a second --
    // `blocked` alone crossed ~45 000 JSON element accesses per frame across
    // nine tiles -- for numbers that never change once the tile is mounted.
    //
    // So they are unpacked once, at mount, into plain arrays. Nothing here is a
    // different answer to any question: it is the same data, read the way a
    // frame can afford to read it.
    static void unpack(Loaded& tile) {
        const auto& bounds=tile.data.at("bounds");
        tile.west=bounds.at("west");tile.east=bounds.at("east");
        tile.south=bounds.at("south");tile.north=bounds.at("north");
        const auto& grid=tile.data.at("elevations");
        tile.gridSize=int(grid.size());
        tile.elevation.resize(size_t(tile.gridSize)*size_t(tile.gridSize));
        for(int row=0;row<tile.gridSize;++row) {
            const auto& line=grid[size_t(row)];
            for(int col=0;col<tile.gridSize;++col)
                tile.elevation[size_t(row)*size_t(tile.gridSize)+size_t(col)]=float(line[size_t(col)]);
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
    double height(double x,double y) {
        auto* t=tile(x,y);if(!t)throw std::runtime_error("Missing terrain");
        double deck=0;if(onDeck(*t,x,y,&deck))return deck;
        const int n=t->gridSize;
        double u=std::clamp((wrap(x)-t->west)/(t->east-t->west),0.,1.)*(n-1);
        double v=std::clamp((y-t->south)/(t->north-t->south),0.,1.)*(n-1);
        int ix=std::min(n-2,int(u)),iy=std::min(n-2,int(v));u-=ix;v-=iy;
        const float* g=t->elevation.data();
        const size_t low=size_t(iy)*size_t(n),high=low+size_t(n);
        return (double(g[low+size_t(ix)])*(1-u)+double(g[low+size_t(ix)+1])*u)*(1-v)
              +(double(g[high+size_t(ix)])*(1-u)+double(g[high+size_t(ix)+1])*u)*v;
    }
    // The tile's water grid and fully oceanic tiles must answer the same
    // question for walkers, cars and boats. Piers remain dry walkable decks.
    bool onWater(double x,double y) {
        const auto* t=tile(x,y);
        return t&&!onDeck(*t,x,y)&&(t->ocean||waterCode(*t,x,y)!=0);
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
    bool blocked(double x,double y) {
        if(onWater(x,y))return true;
        for(auto& [key,t]:loaded) {
            // A point can only be inside a building or a tree of a tile it is
            // in. Two metres of margin covers a footprint digitised a hair over
            // its own tile edge; the other eight tiles cost one comparison.
            if(!inside(t,x,y,2e-5))continue;
            glm::dvec3 p=t.frame.local(ecef(x,y,height(x,y)));
            const glm::dvec2 q(p.x,p.z);
            for(const Plant& plant:t.vegetation)
                if(plant.tree&&std::hypot(q.x-plant.node->transform().position.x,
                                          q.y-plant.node->transform().position.z)<.55)return true;
            for(const auto& shape:t.footprints) {
                // The rings are already parsed and already carry their box
                // (see Footprint). Testing the box first is what turns five
                // hundred polygon walks into a handful.
                if(q.x<shape.low.x-.32||q.x>shape.high.x+.32||
                   q.y<shape.low.y-.32||q.y>shape.high.y+.32)continue;
                const auto& poly=shape.points;
                bool in=false;const size_t n=poly.size();
                for(size_t i=0,j=n-1;i<n;j=i++) {
                    const glm::dvec2 a=poly[i],b=poly[j],d=b-a;
                    const double len=glm::dot(d,d);
                    const double f=len>0?std::clamp(glm::dot(q-a,d)/len,0.,1.):0.;
                    if(glm::length(q-(a+f*d))<.32)return true;
                    if((a.y>q.y)!=(b.y>q.y) && q.x<(b.x-a.x)*(q.y-a.y)/(b.y-a.y)+a.x)in=!in;
                }
                if(in)return true;
            }
        }
        return false;
    }
    // ── the car ─────────────────────────────────────────────────────────────
    //
    // Everything below shares the walk's vocabulary on purpose: the same
    // `advance` on the ellipsoid, the same `blocked` against streamed
    // footprints and water, the same `height` off the tile's own grid. A car
    // that used a second notion of where the ground is would disagree with the
    // player about it the first time he stepped out.

    // Terrain height, or `fallback` where no tile answers yet. `height` throws
    // by design -- walking off the edge of the loaded world is a refusal the
    // walk reports -- and the car asks four times a frame to sit on a slope, so
    // it asks the question that has an answer.
    double groundAt(double x,double y,double fallback) {
        return tile(x,y)?height(x,y):fallback;
    }
    // Yaw, then the slope the wheels are actually standing on. Measured off the
    // terrain grid over the car's own wheelbase and track rather than inferred
    // from a normal: it is the same surface the collision reads (§3 I5).
    glm::quat carRotation() {
        const auto& spec=vehicleSpec(*car);
        const double halfLength=spec.wheelbase*.5,halfWidth=spec.width*.43;
        auto sample=[&](double east,double north){
            auto q=advance(carLon,carLat,east,north);return groundAt(q.x,q.y,carAlt);
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
    bool freeSpot(double x0,double y0,double radius,double& outLon,double& outLat) {
        if(tile(x0,y0)&&!blocked(x0,y0)){outLon=x0;outLat=y0;return true;}
        for(int i=1;i<=120;++i) {
            double a=i*2.39996323,d=radius*std::sqrt(double(i)/120.);
            auto q=advance(x0,y0,d*cos(a),d*sin(a));
            if(tile(q.x,q.y)&&!blocked(q.x,q.y)){outLon=q.x;outLat=q.y;return true;}
        }
        return false;
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
        // Three metres to his right, which is where a car that dropped him off
        // would be, and a spiral out from there when that spot is a wall.
        auto beside=advance(lon,lat,cos(yaw*rad)*3.,-sin(yaw*rad)*3.);
        double x,y;
        if(!freeSpot(beside.x,beside.y,14.,x,y)) {
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
            auto q=advance(carLon,carLat,cos(carYaw*rad)*side,-sin(carYaw*rad)*side);
            if(tile(q.x,q.y)&&!blocked(q.x,q.y)){x=q.x;y=q.y;found=true;break;}
        }
        if(!found)found=freeSpot(carLon,carLat,9.,x,y);
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
        if(onWater(lon,lat)){sinkCar(lon,lat);return;}
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
        auto next=advance(lon,lat,sin(carYaw*rad)*carSpeed*dt,cos(carYaw*rad)*carSpeed*dt);
        if(!tile(next.x,next.y))stream(); // Mount a prefetched tile before stopping at its edge.
        if(!tile(next.x,next.y)) {
            // Outrunning the streamer. Said rather than shown as a stutter: at
            // 130 km/h a car reaches the edge of the loaded world in seconds,
            // and a wall of nothing has to announce itself as one.
            carSpeed=0;
            text("stream-status","Bord du terrain chargé — les données suivantes arrivent.");
            return;
        }
        if(onWater(next.x,next.y)){sinkCar(next.x,next.y);return;}
        if(blocked(next.x,next.y)) {
            carSpeed=0;
            text("stream-status","Obstacle — la voiture s'arrête.");
            return;
        }
        // Traffic is the one obstacle that moves, so `blocked` -- which reads
        // footprints and water -- cannot know about it.
        if(trafficAt(next.x,next.y,2.6)) {
            carSpeed=0;
            text("stream-status","Voiture devant — la circulation vous arrête.");
            return;
        }
        lon=next.x;lat=next.y;alt=groundAt(lon,lat,alt);
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
                    const auto q=advance(boat.lon,boat.lat,s*a+c*o,c*a-s*o);
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
        auto next=advance(lon,lat,s*boat.speed*dt,c*boat.speed*dt);
        const double lead=boat.length*.5*(boat.speed>=0?1.:-1.);
        auto bow=advance(next.x,next.y,s*lead,c*lead);
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
                auto* source=prototype(level.first);
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
    static bool within(const std::vector<glm::dvec2>& poly,glm::dvec2 q) {
        bool in=false;
        for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++)
            if((poly[i].y>q.y)!=(poly[j].y>q.y)&&q.x<(poly[j].x-poly[i].x)*(q.y-poly[i].y)/(poly[j].y-poly[i].y)+poly[i].x)in=!in;
        return in;
    }
    // The altitude of the roof under (x, y) if a point at `above` is on or
    // over it -- where a helicopter sets down -- and -inf otherwise. Tops are
    // in their tile's frame, so they are compared there and converted back.
    double roofAt(double x,double y,double above) {
        double best=-1e30;
        for(auto& [key,t]:loaded) {
            if(!inside(t,x,y,2e-5))continue;
            const glm::dvec3 p=t.frame.local(ecef(x,y,above));
            const glm::dvec2 q(p.x,p.z);
            for(const auto& shape:t.footprints) {
                if(shape.top>1e29||q.x<shape.low.x||q.x>shape.high.x||q.y<shape.low.y||q.y>shape.high.y)continue;
                if(p.y>=shape.top-1.&&within(shape.points,q))best=std::max(best,above+(shape.top-p.y));
            }
        }
        return best;
    }
    // Whether a point at altitude `alt` is inside a building, below its top.
    bool buildingAt(double x,double y,double alt) {
        for(auto& [key,t]:loaded) {
            if(!inside(t,x,y,2e-5))continue;
            const glm::dvec3 p=t.frame.local(ecef(x,y,alt));
            const glm::dvec2 q(p.x,p.z);
            for(const auto& shape:t.footprints) {
                if(q.x<shape.low.x||q.x>shape.high.x||q.y<shape.low.y||q.y>shape.high.y)continue;
                if(p.y<shape.top&&within(shape.points,q))return true;
            }
        }
        return false;
    }
    // What an aircraft rests on at (x, y): the terrain, a deck, the water's
    // level, or a roof it is above. Where no tile answers yet, the last
    // surface it saw -- said on screen, never guessed at.
    double surfaceUnder(double x,double y,double above,bool* onRoof=nullptr,bool* wet=nullptr) {
        if(onRoof)*onRoof=false;
        if(wet)*wet=false;
        if(!tile(x,y))return planeGround;
        double surface=height(x,y);
        if(onWater(x,y)){surface=waterLevel(x,y);if(wet)*wet=true;}
        const double roof=roofAt(x,y,above);
        if(roof>surface){surface=roof;if(onRoof)*onRoof=true;if(wet)*wet=false;}
        planeGround=surface;
        return surface;
    }
    // The aircraft's extremities -- nose, tail and the two tips of its wings
    // or its rotor -- against the buildings, at the height of its underside.
    bool aircraftHits(double x,double y,double alt,double heading) {
        const auto& t=*plane.type;
        const double s=std::sin(heading*rad),c=std::cos(heading*rad);
        const double half=t.length*.5,span=t.span*.5,body=alt+.4;
        for(const auto& [a,o]:{std::pair{0.,0.},std::pair{half,0.},std::pair{-half,0.},std::pair{0.,span},std::pair{0.,-span}}) {
            const auto q=advance(x,y,s*a+c*o,c*a-s*o);
            if(buildingAt(q.x,q.y,body))return true;
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
    // Refused in the air, at speed and on a roof, each out loud (rule 3 of
    // CLAUDE.md). On the ground the pilot steps down beside the nose, on the
    // left where the doors are; on water he drops into it.
    bool leaveAircraft() {
        if(!piloting)return false;
        if(plane.airborne) {
            text("stream-status","En vol — posez l'appareil pour descendre.");
            saida::Log::info("[World aircraft] exit refused in the air, ",plane.alt-planeGround," m up");
            return false;
        }
        if(std::abs(plane.speed)>kAircraftExitSpeed) {
            text("stream-status","Trop rapide pour descendre — freinez.");
            saida::Log::info("[World aircraft] exit refused at ",std::abs(plane.speed)," m/s");
            return false;
        }
        bool onRoof=false;
        surfaceUnder(plane.lon,plane.lat,plane.alt+1.,&onRoof);
        if(onRoof) {
            text("stream-status","Sur un toit — posez-vous au sol pour descendre.");
            saida::Log::info("[World aircraft] exit refused on a roof");
            return false;
        }
        const auto& t=*plane.type;
        const double s=std::sin(plane.yaw*rad),c=std::cos(plane.yaw*rad),half=fuselageHalf(t);
        std::optional<glm::dvec2> dry,wet;
        for(double along:{t.length*.3,t.length*.42,0.,-t.length*.25})
            for(double side:{-1.,1.})
                for(double off:{2.,4.}) {
                    if(dry)break;
                    const double o=(half+off)*side;
                    const auto q=advance(plane.lon,plane.lat,s*along+c*o,c*along-s*o);
                    if(!tile(q.x,q.y))continue;
                    if(navigable(q.x,q.y)){if(!wet)wet=q;}
                    else if(!blocked(q.x,q.y))dry=q;
                }
        double x=0,y=0;
        if(!dry&&!wet&&freeSpot(plane.lon,plane.lat,t.span*.5+8.,x,y))dry=glm::dvec2(x,y);
        if(!dry&&!wet) {
            text("stream-status","Impossible de descendre ici — déplacez l'appareil.");
            saida::Log::warn("[World aircraft] no standable ground beside the aircraft at ",plane.lon,", ",plane.lat);
            return false;
        }
        const glm::dvec2 at=dry?*dry:*wet;
        piloting=false;keepAircraft(plane);plane=Aircraft{};
        yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
        lon=at.x;lat=at.y;swimming=!dry;
        swimTime=0;swimHeading=yaw;swimLean=0;
        alt=swimming?waterLevel(lon,lat):height(lon,lat);
        player->setEnabled(true);
        player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
        followDistance=std::min(followDistance,kOnFootFollow);
        text("stream-status",swimming?"À l'eau. F : remonter à bord.":"À pied. F : reprendre l'appareil.");
        saida::Log::info("[World aircraft] stepped down at ",lon,", ",lat,swimming?" into the water":"");
        request(lon,lat);
        return true;
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
        const auto next=advance(plane.lon,plane.lat,s*horizontal*dt,c*horizontal*dt);
        if(!tile(next.x,next.y))stream();
        if(!tile(next.x,next.y)&&!plane.airborne) {
            plane.speed=0;
            text("stream-status","Bord du terrain chargé — les données suivantes arrivent.");
            return false;
        }
        const double heading=horizontal<0?wrap(plane.yaw+180.):plane.yaw;
        if(aircraftHits(next.x,next.y,plane.alt,heading)&&!aircraftHits(plane.lon,plane.lat,plane.alt,heading)) {
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
    void readConditions() {
        auto latest=sky->latest();
        if(!latest||latest==conditionsDoc)return;
        conditionsDoc=latest;conditions=*latest;
        if(!localConditions()||!sunScript)return;
        const auto weather=conditions.value("weather",json());
        const double cover=weather.is_object()?std::clamp(weather.value("cloudCover",0.)/100.,0.,1.):0.;
        const double rain=weather.is_object()?std::max(0.,weather.value("precipitation",0.)):0.;
        json result;
        sunScript->callExport("setWeather",json::array({cover,rain}),result);
    }
    std::string localClock() {
        double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
        const bool known=localConditions()&&conditions.value("timeSource","")=="zone";
        const int offset=known?conditions.value("utcOffsetSeconds",0):int(std::round(lon/15.))*3600;
        const std::time_t local=std::time_t(unixSeconds)+offset;
        std::tm parts{};gmtime_s(&parts,&local);
        std::ostringstream out;out<<std::put_time(&parts,"%d/%m %H:%M");
        if(known) {
            std::string zone=conditions.value("timezone",std::string(""));
            const auto slash=zone.rfind('/');if(slash!=std::string::npos)zone=zone.substr(slash+1);
            std::replace(zone.begin(),zone.end(),'_',' ');
            return out.str()+" · "+zone;
        }
        return out.str()+" · fuseau estimé par longitude";
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
        lod->setLevels({{"Near",.055f},{"Far",0.f}});
    }
    void buildTrafficPrototype() {
        std::ifstream input(game/"assets/models/vehicles/fleet.json");
        if(!input)throw std::runtime_error("Missing road vehicle fleet manifest");
        json doc;input>>doc;
        for(const auto& entry:doc.at("vehicles")) {
            VehicleModel spec;
            spec.name=entry.at("name");spec.length=entry.at("length");spec.width=entry.at("width");
            spec.height=entry.at("height");spec.wheelbase=entry.at("wheelbase");spec.wheelRadius=entry.at("wheelRadius");
            auto root=std::make_unique<saida::Node>("vehicle-"+spec.name);
            for(const auto& level:std::vector<std::pair<std::string,std::string>>{{"near","Near"},{"far","Far"}}) {
                auto* source=prototype(entry.at(level.first).at("path").get<std::string>());
                if(!source)throw std::runtime_error("Vehicle asset failed: "+spec.name);
                auto child=clonePlant(*source);child->setName(level.second);
                child->transform().rotation=glm::quat(0,0,1,0); // Authored forward +Z.
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
        if(fleet.empty())return;
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
        return slot<agents.size()&&agents[slot].alive;
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
                vehicleLod(*node);
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
                tile.cars[i]->setVisible(false);
                continue;
            }
            tile.cars[i]->setVisible(true);
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
    // Is a traffic car standing where the player's car is about to be? The
    // player's own collision reads streamed footprints and water (`blocked`);
    // traffic is the one obstacle that moves, so it is asked separately and in
    // each tile's own frame.
    bool trafficAt(double x,double y,double clearance) const {
        for(const auto& [key,tile]:loaded) {
            if(tile.cars.empty())continue;
            const glm::dvec3 p=tile.frame.local(ecef(x,y,alt));
            for(size_t i=0;i<tile.cars.size();++i) {
                if(!tile.cars[i]||!trafficSlotLive(tile,i))continue;
                const auto& position=tile.cars[i]->transform().position;
                const auto& spec=fleet[tile.carKinds[i]];
                const glm::vec3 offset=glm::inverse(tile.cars[i]->transform().rotation)*
                    glm::vec3(float(p.x-position.x),0,float(p.z-position.z));
                const double padding=std::max(0.,clearance-1.8);
                if(std::abs(offset.x)<spec.width*.5+padding&&std::abs(offset.z)<spec.length*.5+padding)return true;
            }
        }
        return false;
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
            auto sea=saida::SceneSerializer::nodeFromJson(cooked.ocean.dump(),engine.resources());
            if(!sea)throw std::runtime_error("sea node refused by the scene loader");
            root->addChild(std::move(sea));
        } else root->createChild<saida::Node>("Geography");
        return root;
    }
    // A mount used to upload a whole tile in one frame -- 30 to 60 ms, the
    // hitch every new tile was felt as. Now the ground goes first, a few
    // milliseconds of parts a frame, and the rest follows over the next frames.
    // Collision never waits: it reads the manifest, which is there at mount.
    void uploadParts() {
        const auto start=std::chrono::steady_clock::now();
        for(auto& [key,t]:loaded) {
            if(!t.geography)continue;
            const auto& parts=std::any_cast<const PreparedTile&>(t.served->prepared).parts;
            while(t.nextPart<parts.size()) {
                const PartUpload& part=parts[t.nextPart++];
                auto* mesh=engine.resources().getMesh(engine.resources().registerMemoryMesh(part.vertices,part.indices));
                if(!mesh) {
                    // Said, never silent: a tile missing a part looks like OSM missing it.
                    saida::Log::warn("[World streaming] the geometry arena refused ",part.name," of ",key);
                    continue;
                }
                t.geography->addChild(std::make_unique<saida::MeshNode>(part.name,mesh,
                    material(t.served->cooked.parts[part.material].material)));
                if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=3.)return;
            }
        }
    }
    void placeTiles() {
        for(auto& [key,t]:loaded) {
            const auto position=glm::vec3(origin.local(t.frame.origin));
            const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*t.frame.basis));
            const auto turn=rotation*glm::inverse(t.node->transform().rotation);
            engine.sceneTree().world().rebaseSubtree(*t.node,
                position-turn*t.node->transform().position,turn);
        }
        for(auto& f:farLandmarks)if(f.node)placeFar(f);
    }
    void trim() {
        engine.sceneTree().applyDeferred();
        engine.resources().trimUnused(engine.sceneTree().world().resourceUsage());
    }
    std::unique_ptr<saida::Node> clonePlant(saida::Node& source) {
        std::unique_ptr<saida::Node> out;
        if(source.mesh())out=std::make_unique<saida::MeshNode>(source.name(),source.mesh(),source.material());
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
            mesh->transform()=here;
            out.push_back(std::move(mesh));
        }
        for(auto& child:source.children())collectMeshes(*child,here,out);
    }
    // Each shared model parsed once: instances share its mesh and material
    // pointers, avoiding thousands of repeated GLB and texture decodes.
    saida::Node* prototype(const std::string& path) {
        auto& found=naturePrototypes[path];
        if(!found) {
            auto imported=saida::SceneSerializer::nodeFromJson(json({{"type","Node"},{"importedFrom",path}}).dump(),engine.resources());
            if(imported)found=prototypes->addChild(std::move(imported));
        }
        return found;
    }
    // The shared models a tile may ask for, parsed one a frame while the map is
    // on screen: the first import of a photoscanned tree is 50 to 150 ms, and
    // it belongs on the map, not in the first seconds of play.
    std::vector<std::string> warmList;
    void warm() {
        if(warmList.empty())return;
        const std::string path=warmList.back();warmList.pop_back();
        if(!prototype(path))saida::Log::warn("[World streaming] shared model will not load: ",path);
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
            lod->setLevels({{"Near", .06f}, {"Far", 0.f}});
        }
        return node;
    }
    // `reach` is §5's detail band, 1 on foot and less at the wheel: the
    // radius a plant is drawn in shrinks with the speed it is passed at, which
    // is the cheapest of the plan's levers and the only one that costs nothing
    // when standing still.
    void updateNature(double reach) {
        for(auto& [key,t]:loaded){
            const auto pos=t.frame.local(ecef(lon,lat,alt));
            for(const Plant& plant:t.vegetation){
                const auto p=plant.node->transform().position;
                double d=std::hypot(p.x-pos.x,p.z-pos.z);
                plant.node->setVisible(d<(plant.grass?65:550)*reach);

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
        saida::Node* node=nullptr; int shown=-1;
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
        const auto turn=rotation*glm::inverse(f.node->transform().rotation);
        engine.sceneTree().world().rebaseSubtree(*f.node,position-turn*f.node->transform().position,turn);
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
            if(f.node){f.node->queueFree();f.node=nullptr;}
            f.shown=want;
            if(want<0)continue;
            const auto& level=f.levels[size_t(want)];
            auto n=saida::SceneSerializer::nodeFromJson(json({{"type","Node"},{"name","far "+f.slug},
                {"importedFrom",level.path}}).dump(),engine.resources());
            if(!n) {
                // Keeps `shown`, so a model that will not load is reported
                // once per distance band rather than once a frame.
                saida::Log::warn("[World landmarks] ",f.slug," level ",want+1," failed to load: ",level.path);
                if(smoke){testFailed=true;engine.sceneTree().quit();}
                continue;
            }
            f.node=engine.sceneTree().world().addChild(std::move(n));
            placeFar(f);
            saida::Log::info("[World landmarks] ",f.slug," level ",want+1," at ",int(distance)," m");
        }
    }
    void streamProps() {
        const auto start=std::chrono::steady_clock::now();
        // Scene/GPU APIs stay on the render thread. Yield between objects;
        // the budget is soft because one model import is indivisible.
        for(auto t:ring) {
            auto it=loaded.find(t.key());if(it==loaded.end())continue;
            auto& l=it->second;
            while(l.nextProp<l.props.size()) {
                try {
                    const auto& doc=l.props[l.nextProp];
                    bool plant=doc.contains("groups")&&std::find(doc["groups"].begin(),doc["groups"].end(),"vegetation")!=doc["groups"].end();
                    auto n=(plant||doc.contains("importedFrom"))?plantNode(doc):saida::SceneSerializer::nodeFromJson(doc.dump(),engine.resources());
                    if(!n)throw std::runtime_error("Object import failed");
                    if(plant) {
                        Plant entry;
                        entry.node=n.get();
                        entry.grass=n->isInGroup("grass");
                        entry.tree=n->isInGroup("tree");
                        l.vegetation.push_back(entry);
                        n->setVisible(false);
                    }
                    l.node->addChild(std::move(n));
                } catch(const std::exception& e) {
                    saida::Log::warn("[World streaming] prop failed in ",t.key(),": ",e.what());
                    if(smoke){testFailed=true;engine.sceneTree().quit();return;}
                }
                ++l.nextProp;
                if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=2.)return;
            }
        }
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
        for(auto it=loaded.begin();it!=loaded.end();) {
            // A tile the service cooked again -- offline first, then from real
            // observations -- is taken down and mounted anew below.
            const auto current=service->find(it->second.served->cooked.tile);
            const bool stale=current&&current->serial!=it->second.served->serial;
            if(!keep.count(it->first)||stale){it->second.node->queueFree();it=loaded.erase(it);removed=true;}else ++it;
        }
        if(removed)trim();
        size_t count=0;
        for(auto& [k,t]:loaded)if(wanted.count(k))count+=t.data.at("vertices").get<size_t>();
        // The far landmarks share the arena with the tiles (gen/landmarks.cpp).
        count+=farVertices();
        const auto tick=std::chrono::steady_clock::now();
        for(auto t:want) {
            if(loaded.count(t.key()))continue;
            auto served=service->find(t.gen());
            if(!served)continue;
            const json& data=served->cooked.manifest;
            try {
                const size_t vertices=data.at("vertices").get<size_t>();
                if(count+vertices>residentVertexBudget) {
                    text("stream-status","Limite de détail atteinte dans cette zone.");
                    if(refused.empty()) {
                        refused=t.key();
                        // Loud on purpose. A skipped tile used to be a UI string
                        // and nothing else, so a neighbourhood that did not fit
                        // looked exactly like a slow download -- the worst shape
                        // a failure can take (CLAUDE.md rule 3).
                        saida::Log::warn("[World] ",t.key()," does not fit: ",count,
                                         " + ",vertices," > ",residentVertexBudget," resident vertices");
                    }
                    continue;
                }
                const auto mountStarted=std::chrono::steady_clock::now();
                auto* ptr=engine.sceneTree().world().addChild(buildTile(t.key(),*served));
                auto [entry,inserted]=loaded.try_emplace(t.key(),ptr,served);
                entry->second.geography=ptr->findByPath("Geography");
                entry->second.footprints=std::any_cast<const PreparedTile&>(served->prepared).footprints;
                // After the emplace, never before: the flow holds a pointer to
                // the graph, and a graph built in a temporary would be moved
                // out from under it.
                unpack(entry->second);
                readGraph(entry->second);
                mountAircraft(entry->second);
                placeTiles();
                lastMountMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-mountStarted).count();
                saida::Log::info("[World streaming] mounted ",t.key()," mount_ms=",lastMountMs,
                                 " cook_ms=",served->cooked.cookMs," since_go_ms=",
                                 std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-goStarted).count());
            }catch(const std::exception& e){text("status",std::string("Tuile indisponible : ")+e.what());}
            // Mounting is cheap now that the parts go up over the next frames;
            // several tiles fit in one tick, as long as the tick stays short.
            if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tick).count()>=4.)break;
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
        if(pending)text("status",kPreparing);
        if(pending && tile(pickLon,pickLat)) {
            const bool waterSpawn=onWater(pickLon,pickLat);
            double x0=pickLon,y0=pickLat;bool found=waterSpawn||!blocked(x0,y0);
            for(int i=1;!found&&i<=160;++i) {
                double a=i*2.39996323,d=2.*std::sqrt(double(i));auto q=advance(pickLon,pickLat,d*cos(a),d*sin(a));
                if(tile(q.x,q.y)&&!blocked(q.x,q.y)){x0=q.x;y0=q.y;found=true;}
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
            origin=Frame(lon,lat,alt);placeTiles();moveSun();
            conditions=json::object();
            if(sunScript){json result;sunScript->callExport("setWeather",json::array({0.,0.}),result);}
            jumpOffset=jumpVelocity=0;followDistance=kOnFootFollow;wasJump=false;
            // Teleporting leaves the current vehicle. A water arrival starts
            // swimming at the selected coordinate, including in the open sea.
            driving=false;swimming=waterSpawn;swimTime=0;swimLean=0;swimHeading=yaw;
            clearBoats();
            clearAircraft();
            player->setEnabled(true);
            player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
            parkCar();
            playing=true;pending=false;warming=false;showMap(false);request(lon,lat);
            saida::Log::info("[World] spawned ",lon,", ",lat," altitude=",alt,
                             waterSpawn?" swimming":" on foot",
                             tile(lon,lat)->data.value("offlineApproximation",false)
                                 ?" (simplified offline terrain)":"");
            saida::Log::info("[World streaming] go_to_play_ms=",
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-goStarted).count(),
                " resident=",loaded.size()," target=",want.size());
            if(smoke){
                // A test driver must choose a free direction, like a human:
                // walking straight into the nearest real wall is not a failure.
                double best=0;
                for(int direction=0;direction<32;++direction){
                    double heading=direction*11.25,clear=0;
                    for(double d=.5;d<=12;d+=.5){auto q=advance(lon,lat,sin(heading*rad)*d,cos(heading*rad)*d);if(!tile(q.x,q.y)||blocked(q.x,q.y))break;clear=d;}
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
            else if(status.offline)text("status","Réseau indisponible : terrain simplifié en attendant.");
        }
    }
public:
    World(saida::Engine& e,fs::path g,bool test,saida::CaptureRequest capture,double startLon,double startLat,bool hop,double hopX,double hopY,saida::runtime::CaptureViewpoint view,bool sail=false,bool fly=false):engine(e),game(g),smokeSail(sail),smokeFly(fly),pickLon(startLon),pickLat(startLat),smoke(test),hopLon(hopX),hopLat(hopY),hopWanted(hop),worldCapture(capture),captureView(view) {
        residentVertexBudget=size_t(double(e.resources().geometryCapacity().vertices)*kTileGeometryShare);
        ui=dynamic_cast<saida::WebCanvasNode*>(e.sceneTree().firstInGroup("world-ui"));
        camera=dynamic_cast<saida::CameraNode*>(e.sceneTree().firstInGroup("camera"));
        if(!ui||!camera)throw std::runtime_error("Earth scene is missing its camera or map");
        player=e.sceneTree().firstInGroup("player");
        if(!player)throw std::runtime_error("Earth scene is missing the player");
        player->findBehavioursInChildren(animators);
        if(animators.empty())throw std::runtime_error("Player model has no Animator");
        for(auto* a:animators) {
            for(auto clip:{"idle","run","jump"})
                if(!a->clips().count(clip))throw std::runtime_error(std::string("Player animation missing: ")+clip);
            a->play("idle");
        }
        player->setEnabled(false);
        saida::Log::info("[World player] model ready, animators=",animators.size());
        car=e.sceneTree().firstInGroup("vehicle");
        if(!car)throw std::runtime_error("Earth scene is missing the player's car");
        car->setEnabled(false);
        // The kit names its four wheels, and the names are the contract: a
        // re-export that renamed them would leave a car sliding on frozen
        // wheels, which is a silent regression. It is said once, out loud.
        collectWheels(*car);
        saida::Log::info("[World car] model ready, wheels=",frontWheels.size()+rearWheels.size());
        prototypes=e.sceneTree().world().createChild<saida::Node>("Shared prototypes");
        prototypes->setEnabled(false);
        loadPaints();
        buildTrafficPrototype();
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
        options.prepare=[](r1::ServedTile& tile){tile.prepared=prepareTile(tile.cooked);};
        options.log=[](const std::string& line){saida::Log::info("[World service] ",line);};
        service=std::make_unique<r1::WorldService>(std::move(options));
        // Nearest first to be needed last: warm() takes from the back.
        for(const auto& b:r1::palette().boats)warmList.push_back("assets/models/external/kenney_boats/"+b.model+".glb");
        for(const auto& k:r1::palette().props)for(const auto& m:k.models)warmList.push_back(m);
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
    void ProcessEvent(Rml::Event& event) override {
        auto id=event.GetCurrentElement()->GetId();
        const bool press=event.GetType()=="mousedown";
        if(!press&&id==pressedId&&clock-pressedAt<.5)return;  // already acted on the press
        if(press){pressedId=id;pressedAt=clock;}
        if(id=="map") {
            auto* map=ui->findElementById("map");auto o=map->GetAbsoluteOffset();
            double x=event.GetParameter<float>("mouse_x",0)-o.x,y=event.GetParameter<float>("mouse_y",0)-o.y;
            select((x+mapX)/(1344*zoom)*360-180,90-(y+mapY)/(560*zoom)*180);
            warming=true;if(!smoke)request(pickLon,pickLat);
        } else if(id=="go")go();
        else if(id=="resume"&&playing){pending=false;warming=false;request(lon,lat);showMap(false);}
        else if(id=="zoom-in")zoomMap(zoom*2);
        else if(id=="zoom-out")zoomMap(zoom/2);
        else if(id=="reset-map")zoomMap(1);
        else {std::map<std::string,glm::dvec2> places{{"paris",{2.3522,48.8566}},{"tokyo",{139.7671,35.6812}},{"newyork",{-73.9855,40.758}},{"cape",{18.4241,-33.9249}},{"sydney",{151.2093,-33.8688}}};
            if(places.count(id)){auto p=places.at(id);select(p.x,p.y);zoomMap(zoom);warming=true;request(pickLon,pickLat);}}
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
            double candidate=direction*11.25,clear=0;
            for(double d=1.;d<=60.;d+=1.) {
                auto q=advance(carLon,carLat,sin(candidate*rad)*d,cos(candidate*rad)*d);
                if(!tile(q.x,q.y)||blocked(q.x,q.y))break;
                clear=d;
            }
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
                    auto q=advance(boat.lon,boat.lat,std::sin(candidate*rad)*d,std::cos(candidate*rad)*d);
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
        if(!swimming||!onWater(lon,lat)||covered<1.||rootBelow<.7||rootBelow>1.4) {
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
            const auto q=advance(plane.lon,plane.lat,std::sin(heading*rad)*d,std::cos(heading*rad)*d);
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
                if(leaveAircraft()||!piloting)return flyFail("the door opened in the air");
                saida::Log::info("[World E2E] PASS the door stays shut in the air");
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
        case 12: {  // brake, set down, step out
            smokeUp=-1.;smokeF=p.speed>1.?-1.:0.;
            if(!p.airborne&&std::abs(p.speed)<=kAircraftExitSpeed) {
                bool onRoof=false;
                surfaceUnder(p.lon,p.lat,p.alt+1.,&onRoof);
                if(onRoof) {
                    if(leaveAircraft())return flyFail("the pilot stepped out onto a roof");
                    saida::Log::info("[World E2E] PASS helicopter set down on a roof, the door stays shut");
                } else {
                    if(!leaveAircraft())return flyFail("could not step down from the landed helicopter");
                    saida::Log::info("[World E2E] PASS helicopter set down and out");
                }
                smokeFlown.insert("helicopter");smokeFlyPhase=0;smokeFlyWait=true;smokeFlyTime=0;
            } else if(smokeFlyTime>30.) {
                flyFail("the helicopter did not set down within 30 s");
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
        const double ms=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-started).count();
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
            // Teleport the way a player does: open the map, type the
            // destination, press Go. Nothing here reaches past the UI.
            hopDone=true;showMap(true);
            field("longitude",hopLon);field("latitude",hopLat);
            hopArmed=true;hopWait=0;
            return;
        }
        showMap(true);testResume=true;
    }
    void update(float delta) {
        const auto now=std::chrono::steady_clock::now();
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
                arrivalSaid=true;
            }
        }
        lastFrame=now;
        cost=FrameCost{};
        if(generation!=ui->documentGeneration()||listeners.empty()) {
            generation=ui->documentGeneration();listeners.clear();
            for(auto id:{"map","go","resume","zoom-in","zoom-out","reset-map","paris","tokyo","newyork","cape","sydney"})
                if(auto* e=ui->findElementById(id)){e->AddEventListener("click",this);
                    e->AddEventListener("mousedown",this);listeners.push_back(e);}
            if(!listeners.empty()) {
                select(pickLon,pickLat);
                if(smoke) {
                    double sx=pickLon,sy=pickLat;
                    testClick("map");
                    double expectedLon=10./1344.*360.-180.,expectedLat=90.-10./560.*180.;
                    if(std::abs(pickLon-expectedLon)>1e-5||std::abs(pickLat-expectedLat)>1e-5){
                        saida::Log::error("[World E2E] FAIL map click coordinate conversion");testFailed=true;engine.sceneTree().quit();return;
                    }
                    select(sx,sy);
                    // The press alone must start the journey.
                    goCount=0;testClick("go",false);
                    if(!pending||goCount!=1){saida::Log::error("[World E2E] FAIL Go press, pending=",pending," calls=",goCount);testFailed=true;engine.sceneTree().quit();return;}
                    // A full press+release must still act exactly once.
                    testClick("go");
                    if(goCount!=2){saida::Log::error("[World E2E] FAIL Go press+release ran ",goCount," times");testFailed=true;engine.sceneTree().quit();return;}
                    saida::Log::info("[World E2E] map click and Go passed (press-only and press+release)");
                }
            }
        }
        clock+=delta;
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
        bool key=engine.window().keyDown(GLFW_KEY_M)||engine.window().keyDown(GLFW_KEY_ESCAPE);
        if(key&&!wasMenuKey){if(menu&&playing){pending=false;warming=false;request(lon,lat);showMap(false);}else showMap(true);}wasMenuKey=key;
        poll+=delta;hud+=delta;
        if(poll>(pending?.016:.10)){poll=0;if(pending||playing||warming)cost.stream=timed([&]{stream();});}
        cost.parts=timed([&]{uploadParts();});
        if(!playing)cost.warm=timed([&]{warm();});
        if(!playing||menu)return;
        const bool fast=(driving&&std::abs(carSpeed)>kFastDetail)||(sailing&&std::abs(boat.speed)>kFastDetail)
                        ||(piloting&&std::abs(plane.speed)>kFastDetail);
        seaTime+=delta;
        // §5, "le détail suit la vitesse": above 15 km/h the plan drops L5 --
        // mobilier, clutter, détail de façade -- and shrinks the rest. Street
        // furniture is streamed in rather than drawn from a pool, so dropping
        // it means not spending the frame's 2 ms importing what the player is
        // about to leave behind. It resumes the moment he slows down.
        ring=nearby(lon,lat);
        const uint64_t moved=engine.sceneTree().world().indexedNodesTotal()-indexedAtFrame;
        indexedAtFrame=engine.sceneTree().world().indexedNodesTotal();
        churn+=double(moved);churnFrames+=1;dirtyFrames+=moved?1:0;
        if(!fast)cost.props=timed([&]{streamProps();});
        cost.distant=timed([&]{updateFar();});
        cost.world=timed([&]{updateNature(fast?.55:1.);updateTraffic(delta);updateSea(delta);});
        conditionsRead+=delta;
        if(conditionsRead>.5){conditionsRead=0;readConditions();}
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
                const auto next=advance(lon,lat,east,north);
                if(tile(next.x,next.y)) {
                    if(navigable(next.x,next.y)) {
                        lon=next.x;lat=next.y;alt=waterLevel(lon,lat);moving=true;
                        swimHeading=wrap(std::atan2(east,north)/rad);
                    } else if(!blocked(next.x,next.y)) {
                        lon=next.x;lat=next.y;alt=height(lon,lat);swimming=false;moving=true;
                        text("stream-status","À terre.");
                        saida::Log::info("[World swim] ashore at ",lon,", ",lat);
                    }
                    if(moving)swimHeading=wrap(std::atan2(east,north)/rad);
                } else text("stream-status","Les données suivantes arrivent… nage retenue au bord du terrain.");
            }
            if(moving)request(lon,lat);
        } else if(length>0) {
            double speed=w.keyDown(GLFW_KEY_LEFT_SHIFT)?7.:2.8;
            double east=(sin(yaw*rad)*f+cos(yaw*rad)*r)/length*speed*dt;
            double north=(cos(yaw*rad)*f-sin(yaw*rad)*r)/length*speed*dt;
            auto next=advance(lon,lat,east,north);
            if(tile(next.x,next.y)) {
                if(onWater(next.x,next.y)) {
                    lon=next.x;lat=next.y;alt=waterLevel(lon,lat);
                    swimming=true;swimTime=0;swimLean=0;
                    swimHeading=wrap(std::atan2(east,north)/rad);
                    jumpOffset=jumpVelocity=0;moving=true;
                    text("stream-status","À l'eau — nagez vers la rive.");
                    saida::Log::info("[World swim] entered water at ",lon,", ",lat);
                } else if(!blocked(next.x,next.y)){
                    lon=next.x;lat=next.y;alt=height(lon,lat);moving=true;
                    auto facing=glm::angleAxis(float(-std::atan2(east,north)),glm::vec3(0,1,0));
                    player->transform().rotation=glm::slerp(player->transform().rotation,facing,float(1-std::exp(-16*dt)));
                }
            }else text("stream-status","Les données suivantes arrivent… déplacement retenu au bord du terrain.");
            request(lon,lat);
        }
        if(glm::length(origin.local(ecef(lon,lat,alt)))>350.){origin=Frame(lon,lat,alt);placeTiles();moveSun();}
        if(!driving&&!sailing&&!piloting) {
            if(swimming) {
                jumpOffset=jumpVelocity=0;wasJump=false;
                swimLean+=(double(moving)-swimLean)*(1-std::exp(-5*dt));
                const double lean=35.*rad*swimLean;
                const double heave=.035*std::sin(swimTime*4.);
                const double rootBelow=kPlayerHeight*std::cos(lean)-kSwimHeadAbove;
                player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt-rootBelow+heave)));
                player->transform().rotation=glm::angleAxis(float(-swimHeading*rad),glm::vec3(0,1,0))
                                             *glm::angleAxis(float(-lean),glm::vec3(1,0,0));
                for(auto* a:animators)a->play(moving?"run":"idle");
            } else {
                bool jump=w.keyDown(GLFW_KEY_SPACE);
                if(smokeStarted&&worldCapture.pngPath.empty()&&smokeWalk>.6&&smokeWalk<.8)jump=true;
                if(jump&&!wasJump&&jumpOffset<=0)jumpVelocity=std::sqrt(2*22.*1.5);
                wasJump=jump;
                jumpOffset+=jumpVelocity*dt-11*dt*dt;
                jumpVelocity-=22*dt;
                if(jumpOffset<=0){jumpOffset=0;jumpVelocity=0;}
                player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+jumpOffset+.06)));
                for(auto* a:animators)a->play(jumpOffset>0?"jump":moving?"run":"idle");
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
        const double eye=sailing?std::clamp(boat.length*.12+2.,2.,26.):piloting?plane.type->height*.75+1.5:driving?vehicleSpec(*car).height+.5:swimming?2.1:1.6;
        const glm::vec3 anchor=sailing&&boat.node?boat.node->transform().position
                              :piloting&&plane.node?plane.node->transform().position
                              :driving?car->transform().position:player->transform().position;
        const glm::vec3 target=anchor+glm::vec3(0,eye,0);
        const glm::vec3 backward=camera->transform().rotation*glm::vec3(0,0,1);
        // Sweep against streamed building footprints, not an empty physics
        // scene. One segment/edge pass; no scene raycasts or mesh reconstruction.
        double clear=maxFollow;
        auto end=advance(lon,lat,backward.x*clear,-backward.z*clear);
        for(auto& [key,t]:loaded){
            auto a3=t.frame.local(ecef(lon,lat,alt)),b3=t.frame.local(ecef(end.x,end.y,alt));
            glm::dvec2 a(a3.x,a3.z),v(b3.x-a3.x,b3.z-a3.z);
            auto cross=[](glm::dvec2 a,glm::dvec2 b){return a.x*b.y-a.y*b.x;};
            auto low=glm::min(a,a+v)-glm::dvec2(.3),high=glm::max(a,a+v)+glm::dvec2(.3);
            const double eyeY=t.frame.local(ecef(lon,lat,alt+eye)).y;
            for(auto& shape:t.footprints){
                if(shape.high.x<low.x||shape.low.x>high.x||shape.high.y<low.y||shape.low.y>high.y)continue;
                if(shape.top<eyeY)continue;  // lower than the view: it hides nothing
                auto& poly=shape.points;
                for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++){
                    glm::dvec2 p=poly[j],q=poly[i];
                    auto edge=q-p;double den=cross(v,edge);
                    if(std::abs(den)<1e-9)continue;
                    double along=cross(p-a,edge)/den,side=cross(p-a,v)/den;
                    if(along>=0&&along<=1&&side>=0&&side<=1)clear=std::min(clear,std::max(.15,along*maxFollow-.3));
                }
            }
        }
        // Keep the camera above terrain and inside loaded tiles as well.
        for(double d=.25;d<=clear;d+=.25){
            auto q=advance(lon,lat,backward.x*d,-backward.z*d);
            if(!tile(q.x,q.y)&&piloting)continue;  // in the air, unstreamed ground ahead is no wall
            if(!tile(q.x,q.y)||height(q.x,q.y)+.3>alt+jumpOffset+.06+eye+backward.y*d){clear=std::max(.15,d-.25);break;}
        }
        followDistance=clear<followDistance?clear:followDistance+(clear-followDistance)*(1-std::exp(-7*dt));
        camera->transform().position=target+backward*float(followDistance);
        if(smoke&&!worldCapture.pngPath.empty()) {
            if(captureView.set) {
                glm::vec3 position(captureView.position[0],captureView.position[1],captureView.position[2]);
                glm::vec3 target(captureView.target[0],captureView.target[1],captureView.target[2]);
                camera->transform().position=position;
                camera->transform().rotation=glm::quatLookAt(glm::normalize(target-position),glm::vec3(0,1,0));
            }
            auto want=nearby(lon,lat);
            bool settled=std::all_of(want.begin(),want.end(),[&](Tile t){
                auto i=loaded.find(t.key());return i!=loaded.end()&&i->second.nextProp==i->second.props.size();
            });
            // R1WORLD_CAPTURE_SEA: a picture of the sea waits for its ships
            // (half a minute at most -- the log then says there were none).
            if(settled)captureSeaWait+=dt;
            const bool seaReady=!std::getenv("R1WORLD_CAPTURE_SEA")||(!seaShips.empty()&&captureSeaWait>4.)
                ||captureSeaWait>30.;
            if(settled&&seaReady&&!captureQueued){
                size_t plants=0;for(auto& [key,t]:loaded)plants+=t.vegetation.size();
                saida::Log::info("[World nature] resident plants=",plants," shared static prototypes=",naturePrototypes.size());
                saida::Log::info("[World traffic] resident cars=",trafficLive()," of ",trafficWanted()," asked for");
                captureQueued=true;engine.captureFrameThenExit(worldCapture);
            }
        }
        if(hud>.5) {
            hud=0;
            text("local-conditions",localClock()+"  |  "+weatherLabel());
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
                :(nearestCar(within,howFar)?" · F : monter dans la voiture":" · M : carte");
            const auto* currentTile=tile(lon,lat);
            if(currentTile&&currentTile->data.value("offlineApproximation",false))
                mode+=" · Hors ligne : terrain simplifié";
            text("stream-status",std::to_string(loaded.size())+" tuiles actives · Relief réel / bâtiments OSM"+mode
                 +(fast?" · détail réduit à cette vitesse":""));
        }
        if(smokeWaterSpawn&&smokeWalk>1.5) {
            const double covered=glm::length(ecef(lon,lat,alt)-smokeStart);
            const double rootBelow=origin.local(ecef(lon,lat,alt)).y-player->transform().position.y;
            testFailed=!swimming||!onWater(lon,lat)||carParked||covered<1.||rootBelow<.7||rootBelow>1.4;
            saida::Log::info("[World E2E] ",testFailed?"FAIL":"PASS"," water spawn: swam ",covered,
                             "m, body depth=",rootBelow,"m, car parked=",carParked);
            engine.sceneTree().quit();return;
        }
        if(smokeFlyWait||smokeFlyPhase){runSmokeFly(delta);return;}
        if(smokeSailWait||smokeSailing){runSmokeSail(delta);return;}
        if(smokeSwimming){runSmokeSwim(delta);return;}
        if(smokeSeaWait){runSmokeSea(delta);return;}
        if(smokeStarted&&smokeWalk>4) {
            if(!smokeRan||!smokeJumped||jumpOffset!=0||followDistance<.15||followDistance>kOnFootFollow+.001){
                saida::Log::error("[World E2E] FAIL player animation/jump/follow");testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] player run/jump/landing/follow passed, distance=",followDistance);
            smokeStarted=false;
            if(!smokeDroveOnce){startSmokeDrive();return;}
            smokeFinishPhase();
        }
        if(smokeApproach) {
            if(carDistance()<=kCarReach*.8){smokeApproach=false;beginSmokeDriving();return;}
            if(smokeApproachTime>20.) {
                saida::Log::error("[World E2E] FAIL could not walk back to the car, still ",
                                  carDistance(),"m away after ",smokeApproachTime,"s");
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
                                  "m of ",smokeClear,"m clear, top=",smokeTopSpeed," m/s");
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
        saida::Engine engine(nullptr,(game/"R1World.saidaproj").string(),false);
        if(!saida::SceneSerializer::loadIntoScene(engine.scene(),engine.resources(),(game/"scenes/earth.scene").string()))throw std::runtime_error("Cannot load Earth scene");
        engine.mountWorld();saida::Time::setScale(1);
        saida::CaptureRequest capture;saida::runtime::CaptureViewpoint view;std::string error;
        if(!saida::runtime::parseCaptureArgs(argc,argv,capture,view,error))throw std::runtime_error(error);
        // The Atlas, the ground classes and the surfaces: data the generator
        // reads before any tile is cooked, and refuses to run without.
        r1::loadPalette(game.string());
        World world(engine,game,smoke,capture,startLon,startLat,hop,hopLon,hopLat,view,sail,fly);
        engine.setOnFrame([&](float dt){world.update(dt);});
        if(!smoke&&!capture.pngPath.empty())engine.captureFrameThenExit(capture);
        engine.run();return engine.captureFailed()||world.failed()?1:0;
    }catch(const std::exception& e){saida::Log::error("R1World: ",e.what());return 1;}
}
