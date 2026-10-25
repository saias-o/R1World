#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace r1::police {

constexpr size_t kCars=4, kOfficers=kCars*2;
constexpr size_t kGeometryVertices=45000, kGeometryIndices=kGeometryVertices*3;
enum class Crime { RecklessDriving, Collision, Theft, Assault, Gunshot, Killing, OfficerAssault, OfficerKilling };

// Only attributed, observed offences enter the dispatcher's record. Searching
// officers retain their last observation; time alone never reveals a suspect.
class Wanted {
public:
    double percent=0, unseen=0;
    bool violent=false, helicopter=false;
    void report(Crime crime) {
        const double severity[]={4,8,12,14,10,38,28,55};
        percent=std::min(100.,percent+severity[size_t(crime)]);
        violent=violent||crime==Crime::Gunshot||crime==Crime::Killing||crime==Crime::OfficerKilling;
        unseen=0;
        if(percent>=100)helicopter=true;
    }
    void update(double dt,bool observed) {
        if(!(dt>0)||!std::isfinite(dt)||percent<=0)return;
        if(observed)unseen=0;
        else {
            const double before=std::max(0.,unseen-12.);
            unseen+=dt;
            percent=std::max(0.,percent-(std::max(0.,unseen-12.)-before)*.8);
        }
        if(percent<=0)clear();
        else if(percent<60)helicopter=false;
    }
    void clear(){percent=unseen=0;violent=helicopter=false;}
    bool lethal()const{return percent>=40||violent;}
    size_t cars()const{return percent>0?std::min(kCars,size_t(1+percent/25)):1;}
    bool searching()const{return percent>0&&unseen>2;}
};

// Escalation belongs to the pursuit, not the number of officers firing.
// Only time under recent, unsuccessful gunfire counts; hiding cools it down.
struct FireEscalation {
    double unsuccessful=0, sinceShot=10;
    void update(double dt,bool engaged) {
        if(!(dt>0)||!std::isfinite(dt))return;
        sinceShot+=dt;
        if(engaged&&sinceShot<1.6)unsuccessful=std::min(102.,unsuccessful+dt);
        else if(!engaged)unsuccessful=std::max(0.,unsuccessful-dt*.5);
    }
    void gunshot(bool hitPlayer) {sinceShot=0;if(hitPlayer)unsuccessful=0;}
    double rocketShare(double wanted)const {
        if(wanted>=100)return 1;
        if(wanted<60)return 0;
        const double pressure=std::clamp((unsuccessful-12.)/90.,0.,1.);
        const double alert=std::clamp(wanted/100.,0.,1.);
        return pressure*alert*alert*alert*alert;
    }
};
// Choose once per attack, never per frame. Fractional credit spaces out rare
// rocket attacks; a chosen rocket stays selected throughout its aiming delay.
struct WeaponChoice {
    double credit=0;
    bool selected=false, heavy=false;
    bool choose(double share) {
        if(share<=0){selected=true;heavy=false;credit=0;return false;}
        if(share>=1){selected=true;heavy=true;return true;}
        if(!selected) {credit+=share;heavy=credit>=1;if(heavy)credit-=1;selected=true;}
        return heavy;
    }
    void fired(){selected=false;}
    void cancel(){selected=false;heavy=false;}
};

struct Identity {
    std::string label;
    bool redBlue=false;
    float uniformR=.018f,uniformG=.025f,uniformB=.055f;
};
inline Identity identity(const std::string& country,bool gendarmerie=false) {
    if(country=="FR")return {gendarmerie?"GENDARMERIE":"POLICE",false};
    if(country=="DE"||country=="AT")return {"POLIZEI",false};
    if(country=="ES")return {gendarmerie?"GUARDIA CIVIL":"POLICIA",false};
    if(country=="IT")return {gendarmerie?"CARABINIERI":"POLIZIA",false};
    if(country=="BR"||country=="PT")return {"POLICIA",true};
    if(country=="US"||country=="CA")return {"POLICE",true};
    if(country=="IN")return {"POLICE",true,.22f,.17f,.10f};
    return {"POLICE",country!="GB"};
}

// The game joins resident OSM lanes and pavements into these small routing
// graphs. Heights keep bridges separate. Routes never cut across buildings.
class Routes {
public:
    struct Point {float x=0,y=0,z=0;};
    struct Edge {int to;float cost;};
    std::vector<Point> points;
    std::vector<std::vector<Edge>> edges;
    static float distance(Point a,Point b){return std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z));}
    int add(Point p){points.push_back(p);edges.emplace_back();return int(points.size()-1);}
    void link(int a,int b){if(a!=b)edges[size_t(a)].push_back({b,distance(points[size_t(a)],points[size_t(b)])});}
    int nearest(Point p,float within=80)const {
        int best=-1;for(size_t i=0;i<points.size();++i){const float d=distance(p,points[i]);if(d<within){within=d;best=int(i);}}
        return best;
    }
    std::vector<Point> route(Point from,Point to,bool approach=false)const {
        const int start=nearest(from);int goal=nearest(to);
        if(start<0||(!approach&&goal<0))return {};
        std::vector<float> cost(points.size(),std::numeric_limits<float>::infinity());
        std::vector<int> previous(points.size(),-1);
        using Entry=std::pair<float,int>;
        std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> open;
        cost[size_t(start)]=0;open.push({0,start});
        while(!open.empty()) {
            const auto [score,node]=open.top();open.pop();
            if(score>cost[size_t(node)])continue;
            if(node==goal&&!approach)break;
            for(const auto& edge:edges[size_t(node)])if(score+edge.cost<cost[size_t(edge.to)]) {
                cost[size_t(edge.to)]=score+edge.cost;previous[size_t(edge.to)]=node;open.push({cost[size_t(edge.to)],edge.to});
            }
        }
        if(approach) {
            float best=std::numeric_limits<float>::infinity();goal=-1;
            for(size_t i=0;i<points.size();++i)if(std::isfinite(cost[i])) {
                const float gap=distance(points[i],to);
                if(gap<best){best=gap;goal=int(i);}
            }
        }
        if(goal<0||!std::isfinite(cost[size_t(goal)]))return {};
        std::vector<Point> path;
        for(int at=goal;at>=0;at=previous[size_t(at)])path.push_back(points[size_t(at)]);
        std::reverse(path.begin(),path.end());return path;
    }
    // For every point, the closest any route leaving it comes to `to`: one
    // pass over the reversed edges instead of one search per candidate.
    std::vector<float> approaches(Point to)const {
        std::vector<std::vector<int>> into(points.size());
        for(size_t i=0;i<edges.size();++i)for(const auto& edge:edges[i])into[size_t(edge.to)].push_back(int(i));
        std::vector<float> gap(points.size());
        using Entry=std::pair<float,int>;
        std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> open;
        for(size_t i=0;i<points.size();++i){gap[i]=distance(points[i],to);open.push({gap[i],int(i)});}
        while(!open.empty()) {
            const auto [score,node]=open.top();open.pop();
            if(score>gap[size_t(node)])continue;
            for(int from:into[size_t(node)])if(score<gap[size_t(from)]){gap[size_t(from)]=score;open.push({score,from});}
        }
        return gap;
    }
    // The driving cost from every point to the nearest of `goals`, infinite
    // where no route reaches one: dispatch sends the car with the shortest drive.
    std::vector<float> costsTo(const std::vector<int>& goals)const {
        std::vector<std::vector<Edge>> into(points.size());
        for(size_t i=0;i<edges.size();++i)for(const auto& edge:edges[i])into[size_t(edge.to)].push_back({int(i),edge.cost});
        std::vector<float> cost(points.size(),std::numeric_limits<float>::infinity());
        using Entry=std::pair<float,int>;
        std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> open;
        for(int goal:goals){cost[size_t(goal)]=0;open.push({0,goal});}
        while(!open.empty()) {
            const auto [score,node]=open.top();open.pop();
            if(score>cost[size_t(node)])continue;
            for(const auto& edge:into[size_t(node)])if(score+edge.cost<cost[size_t(edge.to)]) {
                cost[size_t(edge.to)]=score+edge.cost;open.push({cost[size_t(edge.to)],edge.to});
            }
        }
        return cost;
    }
};
} // namespace r1::police
