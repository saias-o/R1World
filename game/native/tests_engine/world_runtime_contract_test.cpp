#include "world/remains_store.hpp"
#include "world/simulation_time.hpp"
#include "world/travel_state.hpp"
#include "combat.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

using namespace r1::world;
int main() {
    using namespace r1::combat;
    int referenceShots=-1;
    for(int fps:{60,30,15,10}) {
        Arsenal weapon;weapon.select(Weapon::Pistol);weapon.update(1);
        Health health;health.protect(20);
        double walked=0,police=0,elapsed=0;int shots=0;
        for(int frame=0;frame<fps*10;++frame) {
            const SimulationTime time(1./fps);
            for(int step=0;step<time.steps;++step) {
                assert(time.step<=SimulationTime::kMaxStep+1e-12);
                walked+=4.*time.step;police+=6.*time.step;elapsed+=time.step;
                health.tick(time.step);weapon.update(time.step);
                shots+=weapon.pull()==Arsenal::Trigger::Fired;
            }
        }
        assert(std::abs(elapsed-10)<1e-9 && std::abs(walked-40)<1e-8);
        assert(std::abs(police/walked-1.5)<1e-9 && std::abs(health.shield-10)<1e-9);
        if(referenceShots<0)referenceShots=shots;
        assert(std::abs(shots-referenceShots)<=2);
    }
    assert(std::abs(SimulationTime(.2).seconds-.2)<1e-12);
    assert(SimulationTime(2).seconds==.25 && SimulationTime(2).dropped==1.75);
    assert(SimulationTime(-1).steps==0);

    const TravelMode modes[]={TravelMode::Foot,TravelMode::Swim,TravelMode::Car,TravelMode::Boat,TravelMode::Aircraft};
    for(auto from:modes)for(auto to:modes) {
        TravelState state;
        state.transition(from,[](auto,auto){},[](auto,auto){});
        bool detached=false,attached=false;
        state.transition(to,[&](auto previous,auto destination) {
            assert(previous==from&&destination==to&&state.mode()==from);detached=true;
        },[&](auto previous,auto destination) {
            assert(detached&&previous==from&&destination==to&&state.mode()==to);attached=true;
        });
        assert(attached&&state.mode()==to);
    }

    const auto directory=std::filesystem::temp_directory_path()/
        ("r1-remains-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    RemainsIndex index;
    double submitWorstMs=0;
    {
        RemainsWriter writer;
        for(size_t i=0;i<2048;++i) {
            index.add(i,double(i)*10000,0,0);
            nlohmann::json record={{"resident",std::to_string(i)},{"pose",{1,2,3}},{"settled",true}};
            const auto started=std::chrono::steady_clock::now();
            if(!writer.submit(directory/(std::to_string(i)+".json"),record)) {
                writer.flush();assert(writer.error().empty());
                assert(writer.submit(directory/(std::to_string(i)+".json"),record));
            } else submitWorstMs=std::max(submitWorstMs,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
        }
        for(size_t i=0;i<2048;++i)assert(index.nearby(double(i)*10000,0,0)==std::vector<size_t>{i});
        writer.flush();assert(writer.error().empty());
        for(int revision=0;revision<200;++revision)
            assert(writer.submit(directory/"0.json",{{"resident","0"},{"revision",revision},{"pose",{revision,2,3}}}));
        assert(writer.read(directory/"0.json").at("revision")==199);
    } // controlled flush/join
    {
        RemainsWriter restarted;
        assert(restarted.read(directory/"0.json").at("pose").at(0)==199);
        for(size_t i=1;i<2048;++i)assert(restarted.read(directory/(std::to_string(i)+".json")).at("resident")==std::to_string(i));
        // A refused destination is terminal and preserves existing durable data.
        const auto blocked=directory/"blocked.json";std::filesystem::create_directory(blocked);
        std::ofstream(blocked/"keep.txt")<<"preserved";
        assert(restarted.submit(blocked,{{"resident","blocked"}}));
        restarted.flush();assert(!restarted.error().empty());
        assert(std::filesystem::exists(blocked/"keep.txt"));
    }
    std::cout<<"PASS runtime contracts: clock at 60/30/15/10 fps, travel matrix, 2048 indexed deaths, restart/coalescing/refusal; worst nonblocking submit_ms="<<submitWorstMs<<"\n";
}
