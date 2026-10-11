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
                        pyround(offset,3),pyround(laneWidth,3),road.id,number,priority,-1,0,false,false,allowedTurns(road.tags,reverse,number),level});
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
    for(const auto& lane:lanes)approaches[lane[1].get<size_t>()].insert(lane[0].get<int>());
    std::map<uint64_t,int> signalIndices;
    for(const auto& feature:features) {
        const auto kind=tagOr(feature.tags,"highway");
        if(kind!="traffic_signals"&&kind!="stop"&&kind!="give_way")continue;
        const P3 point=ground(feature.lon,feature.lat);
        const int node=nearest(point,kSignalAttachment);
        if(node<0)continue;
        int signal=-1;
        if(kind=="traffic_signals") {
            int junction=node;double distance=kIntersectionAssociation;
            for(size_t i=0;i<nodes.size();++i)if(approaches[i].size()>2) {
                const double d=std::hypot(nodes[i].x-point.x,nodes[i].z-point.z);
                if(d<distance && std::abs(nodes[i].y-point.y)<kLevelWeld){distance=d;junction=int(i);}
            }
            const auto key=nodeKeys[junction];
            const auto found=signalIndices.find(key);
            if(found==signalIndices.end()) {
                signal=int(signals.size());signalIndices[key]=signal;
                signals.push_back({{"key",key},{"position",{nodes[junction].x,nodes[junction].y,nodes[junction].z}},
                    {"offset",double(key%60)},{"timingSource","inferred"}});
            } else signal=found->second;
        }
        for(size_t laneIndex=0;laneIndex<lanes.size();++laneIndex) {
            auto& lane=lanes[laneIndex];
            if(lane[1].get<int>()!=node)continue;
            lane[9]=signal;
            const P3 a=nodes[lane[0].get<size_t>()],b=nodes[lane[1].get<size_t>()];
            lane[10]=std::abs(b.x-a.x)>std::abs(b.z-a.z)?(b.x>a.x?1:3):(b.z<a.z?0:2);
            lane[11]=kind=="stop";lane[12]=kind=="give_way";
            controls.push_back({{"osm",feature.id},{"lane",laneIndex}, {"kind",kind},
                {"position",{point.x,point.y,point.z}},{"assignment","inferred"}});
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
