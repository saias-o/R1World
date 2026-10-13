#include "scatter.hpp"
#include "streets.hpp"
#include "traffic_format.hpp"
#include <algorithm>
#include <charconv>
#include <tuple>
#include <limits>
#include <cstdlib>
#include <cctype>
#include <set>
#include <stdexcept>

namespace r1 {
namespace {
constexpr double kNodeWeld = .75, kLevelWeld = 3., kMinimumLane = .05;
constexpr double kSignalAttachment = 12., kIntersectionAssociation = 40.;
constexpr double kGeographicPrecision = 1e7;
constexpr int kMaximumCarsPerTile = 22;
const std::map<std::string, double> kClassSpeed{
    {"motorway",33.},{"trunk",25.},{"primary",16.7},{"secondary",13.9},{"tertiary",13.9},
    {"residential",8.3},{"living_street",5.6},{"unclassified",11.1}};
const std::map<std::string, double> kClassWeight{
    {"motorway",6.},{"trunk",5.},{"primary",4.},{"secondary",3.},{"tertiary",2.},
    {"residential",.7},{"living_street",.25},{"unclassified",1.}};
bool drivesLeft(double lon, double lat) {
    constexpr double boxes[][4]{{-11.,49.8,2.1,61.},{60.,5.,92.5,37.},{127.,24.,146.5,46.},
        {95.,-11.,141.5,21.},{112.,-48.,179.5,-8.},{11.,-35.,42.,5.},{-78.5,17.5,-76.,18.6}};
    for (const auto& box : boxes) if (box[0]<=lon && lon<=box[2] && box[1]<=lat && lat<=box[3]) return true;
    return false;
}
uint64_t geographicKey(P2 p, int level) {
    uint64_t value = uint64_t(pyround(p.x*kGeographicPrecision));
    value = (value ^ uint64_t(pyround(p.y*kGeographicPrecision))) * 1099511628211ULL;
    return (value ^ uint64_t(level)) * 1099511628211ULL;
}
int taggedCount(const Tags& tags, const char* name) {
    const auto value = tag(tags,name);
    if (!value) return 0;
    int count = 0;
    const auto parsed = std::from_chars(value->data(), value->data()+value->size(),count);
    return parsed.ec == std::errc() && parsed.ptr == value->data()+value->size() && count>0 && count<=12 ? count : 0;
}
double roadSpeed(const Tags& tags, bool& measured) {
    const auto highway = tagOr(tags,"highway");
    double speed = kClassSpeed.count(highway) ? kClassSpeed.at(highway) : 11.1;
    if (const auto raw = tag(tags,"maxspeed")) {
        char* end = nullptr;
        const double number = std::strtod(raw->c_str(), &end);
        std::string unit = end ? end : "";
        unit.erase(std::remove_if(unit.begin(),unit.end(),[](unsigned char c){return std::isspace(c);}),unit.end());
        std::transform(unit.begin(),unit.end(),unit.begin(),[](unsigned char c){return char(std::tolower(c));});
        if (std::isfinite(number) && number>0 && (unit.empty() || unit=="mph" || unit=="km/h")) {
            speed = number * (unit=="mph" ? .44704 : 1./3.6); measured=true;
        }
    }
    return speed;
}
uint8_t allowedTurns(const Tags& tags, bool reverse, int number) {
    auto raw = tag(tags, reverse ? "turn:lanes:backward" : "turn:lanes:forward");
    if (!raw) raw = tag(tags,"turn:lanes");
    if (!raw) return 15;
    size_t begin=0;
    for (int lane=0;lane<number;++lane) { begin=raw->find('|',begin); if(begin==std::string::npos)return 15; ++begin; }
    const auto end=raw->find('|',begin);
    const auto text=raw->substr(begin,end==std::string::npos?end:end-begin);
    if (text.empty() || text=="none") return 15;
    uint8_t mask=0;
    if(text.find("through")!=std::string::npos)mask|=1;
    if(text.find("left")!=std::string::npos)mask|=2;
    if(text.find("right")!=std::string::npos)mask|=4;
    if(text.find("reverse")!=std::string::npos)mask|=8;
    return mask ? mask : 15;
}
struct StopSite { P3 position; P2 direction; double width; };
StopSite stopSite(const nlohmann::json& lanes,const std::vector<P3>& nodes,size_t laneIndex,double setback,bool leftHand) {
    size_t current=laneIndex;
    for(int step=0;step<64;++step) {
        const auto& lane=lanes[current];
        const P3 a=nodes[lane[0].get<size_t>()],b=nodes[lane[1].get<size_t>()];
        const double span=std::hypot(b.x-a.x,b.z-a.z),dx=(b.x-a.x)/span,dz=(b.z-a.z)/span;
        size_t previous=lanes.size();double best=-1e30;
        if(setback>span)for(size_t i=0;i<lanes.size();++i) {
            const auto& candidate=lanes[i];
            if(candidate[1]!=lane[0]||candidate[0]==lane[1]||candidate[14]!=lane[14])continue;
            const auto from=nodes[candidate[0].get<size_t>()];
            const double length=std::hypot(a.x-from.x,a.z-from.z);
            const double alignment=((a.x-from.x)*dx+(a.z-from.z)*dz)/length;
            if(alignment<-.7)continue;
            const double score=alignment+(candidate[6]==lane[6]?20.:0.)+(candidate[7]==lane[7]?10.:0.);
            if(score>best){best=score;previous=i;}
        }
        if(setback>span&&previous!=lanes.size()){setback-=span;current=previous;continue;}
        const double offset=lane[4].get<double>()*(leftHand?-1.:1.);
        return {{b.x-dx*setback-dz*offset,b.y+(a.y-b.y)*setback/span,b.z-dz*setback+dx*offset},
            {dx,dz},lane[5].get<double>()};
    }
    throw std::runtime_error("traffic stop line exceeds the bounded approach trace");
}
}

nlohmann::json buildLaneGraph(const std::vector<OsmWay>& roads, const GroundAt& ground, int buildings,
                             double lon, double lat, const std::vector<OsmNode>& features,
                             const std::vector<OsmTurnRestriction>& restrictions) {
    std::vector<P3> nodes;
    std::vector<P2> geographic;
    std::vector<uint64_t> nodeKeys;
    std::map<std::tuple<long long,long long,long long,int>,int> index;
    nlohmann::json lanes=nlohmann::json::array(), signals=nlohmann::json::array(), controls=nlohmann::json::array();
    int measured=0,inferred=0;
    double metres=0.;
    const bool leftHand=drivesLeft(lon,lat);
    auto nodeAt=[&](P3 p,P2 geo,int level) {
        const auto key=std::make_tuple(pyround(p.x/kNodeWeld),pyround(p.z/kNodeWeld),pyround(p.y/kLevelWeld),level);
        auto found=index.find(key);
        if(found!=index.end())return found->second;
        const int id=int(nodes.size());index[key]=id;nodes.push_back(p);geographic.push_back(geo);nodeKeys.push_back(geographicKey(geo,level));return id;
    };
    for(const auto& road:roads) {
        const auto highway=tagOr(road.tags,"highway");
        if(!isMotorway(highway)||highway=="service"||tagOr(road.tags,"area")=="yes"||taggedYes(road.tags,"tunnel"))continue;
        const auto profile=roadProfile(road.tags);
        const int direction=profile.direction;
        const int forward=direction<0?0:taggedCount(road.tags,"lanes:forward")?taggedCount(road.tags,"lanes:forward"):direction?profile.lanes:std::max(1,(profile.lanes+1)/2);
        const int backward=direction>0?0:taggedCount(road.tags,"lanes:backward")?taggedCount(road.tags,"lanes:backward"):direction?profile.lanes:std::max(1,profile.lanes/2);
        const double laneWidth=(profile.width-profile.leftShoulder-profile.rightShoulder)/std::max(1,forward+backward);
        bool tagged=false,used=false;
        const double speed=roadSpeed(road.tags,tagged);
        const double weight=kClassWeight.count(highway)?kClassWeight.at(highway):1.;
        const int priority=tagOr(road.tags,"junction")=="roundabout"?8:int(weight);
        const int level=int(lengthTag(tag(road.tags,"layer"),0.));
        for(size_t i=0;i+1<road.points.size();++i) {
            const P3 a=ground(road.points[i].x,road.points[i].y),b=ground(road.points[i+1].x,road.points[i+1].y);
            const double span=std::hypot(a.x-b.x,a.z-b.z);
            if(span<kMinimumLane)continue;
            const int first=nodeAt(a,road.points[i],level),second=nodeAt(b,road.points[i+1],level);
            if(first==second)continue;
            auto append=[&](int count,bool reverse) {
                for(int number=0;number<count;++number) {
                    const double offset=direction ? (number+.5-count*.5)*laneWidth*(leftHand?-1:1) :
                        (leftHand?count-number-.5:number+.5)*laneWidth;
                    lanes.push_back({reverse?second:first,reverse?first:second,pyround(speed,1),weight,
                        pyround(offset,3),pyround(laneWidth,3),road.id,number,priority,-1,0,false,false,allowedTurns(road.tags,reverse,number),level,2.});
                }
            };
            append(forward,false);append(backward,true);
            metres+=span;used=true;
        }
        if(used)++(tagged?measured:inferred);
    }
    auto nearest=[&](P3 point,double radius) {
        int result=-1;
        for(size_t i=0;i<nodes.size();++i) {
            const double distance=std::hypot(nodes[i].x-point.x,nodes[i].z-point.z);
            if(distance<radius && std::abs(nodes[i].y-point.y)<kLevelWeld) { radius=distance;result=int(i); }
        }
        return result;
    };
    std::vector<std::set<int>> approaches(nodes.size());
    for(const auto& lane:lanes) {
        approaches[lane[1].get<size_t>()].insert(lane[0].get<int>());
        approaches[lane[0].get<size_t>()].insert(lane[1].get<int>());
    }
    // Divided carriageways can describe one physical junction with several
    // nearby graph junctions linked across its mouth. Internal connectors do
    // not need another stop line or another signal program.
    std::vector<int> junctionRoot(nodes.size());
    std::vector<double> junctionHalfWidth(nodes.size(),0.);
    for(size_t i=0;i<nodes.size();++i)junctionRoot[i]=int(i);
    for(const auto& lane:lanes)for(int node:{lane[0].get<int>(),lane[1].get<int>()})
        junctionHalfWidth[node]=std::max(junctionHalfWidth[node],std::abs(lane[4].get<double>())+lane[5].get<double>()*.5);
    auto root=[&](int node){while(junctionRoot[node]!=node)node=junctionRoot[node];return node;};
    constexpr double kJunctionMergeClearance=3.;
    for(const auto& lane:lanes) {
        const int from=lane[0],to=lane[1];
        if(approaches[from].size()<3||approaches[to].size()<3)continue;
        if(std::hypot(nodes[from].x-nodes[to].x,nodes[from].z-nodes[to].z)>
            junctionHalfWidth[from]+junctionHalfWidth[to]+kJunctionMergeClearance)continue;
        const int a=root(from),b=root(to);
        if(a!=b){const bool first=nodeKeys[a]<nodeKeys[b];junctionRoot[first?b:a]=first?a:b;}
    }
    std::map<uint64_t,int> signalIndices;
    std::set<std::pair<size_t,std::string>> assignedControls;
    for(const auto& feature:features) {
        const auto kind=tagOr(feature.tags,"highway");
        if(kind!="traffic_signals"&&kind!="stop"&&kind!="give_way")continue;
        const P3 point=ground(feature.lon,feature.lat);
        const int node=nearest(point,kSignalAttachment);
        if(node<0)continue;
        int signal=-1,controlledNode=node;
        if(kind=="traffic_signals") {
            int junction=node;double distance=kIntersectionAssociation;
            for(size_t i=0;i<nodes.size();++i)if(approaches[i].size()>2) {
                const double d=std::hypot(nodes[i].x-point.x,nodes[i].z-point.z);
                if(d<distance && std::abs(nodes[i].y-point.y)<kLevelWeld){distance=d;junction=int(i);}
            }
            junction=root(junction);
            const auto key=nodeKeys[junction];
            controlledNode=junction;
            const auto found=signalIndices.find(key);
            if(found==signalIndices.end()) {
                signal=int(signals.size());signalIndices[key]=signal;
                signals.push_back({{"key",key},{"position",{nodes[junction].x,nodes[junction].y,nodes[junction].z}},
                    {"offset",double(key%60)},{"timingSource","inferred"}});
            } else signal=found->second;
        }
        for(size_t laneIndex=0;laneIndex<lanes.size();++laneIndex) {
            auto& lane=lanes[laneIndex];
            const int endpoint=lane[1];
            if((kind=="traffic_signals"?root(endpoint):endpoint)!=controlledNode)continue;
            if(kind=="traffic_signals"&&root(lane[0].get<int>())==controlledNode)continue;
            const P3 a=nodes[lane[0].get<size_t>()],b=nodes[lane[1].get<size_t>()];
            const double span=std::hypot(b.x-a.x,b.z-a.z),dx=(b.x-a.x)/span,dz=(b.z-a.z)/span;
            // A surveyed signal establishes a controlled junction. Missing
            // approach heads are inferred too, or an untagged cross street
            // would drive through the other approach's red phase.
            if(kind=="traffic_signals"&&approaches[controlledNode].size()<3&&controlledNode!=root(node)) {
                const double along=(b.x-point.x)*dx+(b.z-point.z)*dz;
                const double across=std::abs((b.x-point.x)*dz-(b.z-point.z)*dx);
                if(along<0||across>lane[traffic_format::Width].get<double>()+3.)continue;
            }
            if(!assignedControls.emplace(laneIndex,kind).second)continue;
            lane[9]=signal;
            lane[10]=std::abs(b.x-a.x)>std::abs(b.z-a.z)?(b.x>a.x?1:3):(b.z<a.z?0:2);
            lane[11]=kind=="stop";lane[12]=kind=="give_way";
            double setback=2.;
            if(kind=="traffic_signals")for(const auto& crossing:lanes) {
                if(crossing[traffic_format::Layer]!=lane[traffic_format::Layer])continue;
                const int from=crossing[0],to=crossing[1];
                if(from!=endpoint&&to!=endpoint)continue;
                const double cx=nodes[to].x-nodes[from].x,cz=nodes[to].z-nodes[from].z;
                const double length=std::hypot(cx,cz);
                if(std::abs((cx*dx+cz*dz)/length)>.7)continue;
                setback=std::max(setback,std::abs(crossing[traffic_format::Offset].get<double>())+
                    crossing[traffic_format::Width].get<double>()*.5+.75);
            }
            // Keep the stop line upstream of an observed pedestrian crossing.
            if(kind=="traffic_signals")for(const auto& crossing:features) {
                if(tagOr(crossing.tags,"highway")!="crossing")continue;
                const auto p=ground(crossing.lon,crossing.lat);
                const double along=(b.x-p.x)*dx+(b.z-p.z)*dz;
                const double across=std::abs((b.x-p.x)*dz-(b.z-p.z)*dx);
                if(along>0&&along<20.&&across<lane[traffic_format::Width].get<double>()+2.)
                    setback=std::max(setback,along+lengthTag(tag(crossing.tags,"width"),3.)*.5+1.);
            }
            lane[traffic_format::StopDistance]=pyround(setback,3);
            const auto site=stopSite(lanes,nodes,laneIndex,setback,leftHand);
            controls.push_back({{"osm",feature.id},{"lane",laneIndex}, {"kind",kind},
                {"position",{point.x,point.y,point.z}},
                {"stopPosition",{pyround(site.position.x,3),site.position.y,pyround(site.position.z,3)}},
                {"stopDirection",{site.direction.x,site.direction.y}},{"stopWidth",site.width},
                {"assignment","inferred"},{"markingsSource","inferred from surveyed control"}});
        }
    }
    nlohmann::json turns=nlohmann::json::array();
    for(const auto& restriction:restrictions) {
        const int node=nearest(ground(restriction.via.x,restriction.via.y),kNodeWeld);
        if(node>=0)turns.push_back({restriction.fromWay,restriction.toWay,node,restriction.only});
    }
    nlohmann::json nodeList=nlohmann::json::array();
    for(const auto& n:nodes)nodeList.push_back({pyround(n.x,2),pyround(n.y,2),pyround(n.z,2)});
    const int cars=lanes.empty()?0:int(std::min<long long>(kMaximumCarsPerTile,pyround(buildings/28.+metres/1000.*1.6)));
    return {{"schema",traffic_format::kSchemaVersion},{"nodes",nodeList},{"nodeKeys",nodeKeys},{"lanes",lanes},{"cars",cars},{"leftHand",leftHand},
        {"roadMetres",pyround(metres,1)},{"speedsTagged",measured},{"speedsInferred",inferred},
        {"signals",signals},{"controls",controls},{"restrictions",turns},{"lanePolicy","road profile; directional counts tagged then inferred"}};
}
} // namespace r1
