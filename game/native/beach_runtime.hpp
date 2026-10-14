// R1World's coastal visitors. Included inside World so ownership stays with
// streamed tiles; generation and attendance policy are independently testable.
size_t beachLive=0;
double beachClock=0,beachSunAge=1e9,beachSun=0;
saida::Node* makeBeachFurniture(Loaded& tile,bool parasol,unsigned color) {
    auto root=std::make_unique<saida::Node>("Beach towel and shade");
    const auto parts=r1::beachFurniture(parasol,color);
    for(size_t i=0;i<parts.size();++i)if(!parts[i].mesh.empty()) {
        const auto key="generated/beach/v48/"+std::to_string(parasol)+"/"+std::to_string(color%4)+"/"+std::to_string(i);
        auto up=uploadOf(parts[i],i);
        auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(
            saida::prepareMesh({std::move(up.vertices),std::move(up.indices)}),key));
        if(!mesh)throw std::runtime_error("beach furniture geometry allocation refused");
        root->addChild(std::make_unique<saida::MeshNode>(parts[i].name,mesh,material(parts[i].material)));
    }
    return tile.node->addChild(std::move(root));
}
void updateBeach(float dt) {
    SAIDA_PROFILE_SCOPE("World/Beach");
    beachClock+=dt;beachSunAge+=dt;beachLive=0;
    bool nearby=false;for(const auto& [key,t]:loaded)nearby|=!t.beachSpots.empty();
    if(!nearby)return;
    if(beachSunAge>=.5&&sunScript) {
        json result;beachSunAge=0;beachSun=0;
        if(sunScript->callExport("sunElevation",json::array(),result)==saida::ScriptCallStatus::Succeeded&&result.is_number())
            beachSun=result.get<double>();
    }
    const double factor=r1::beachAttendance(localSolarHour(),beachSun,effectiveWeather());
    size_t budget=24;bool created=false;
    for(auto tile:ring) {
        auto found=loaded.find(tile.key());if(found==loaded.end())continue;
        auto& t=found->second;if(t.beachSpots.empty())continue;
        const auto eye=cameraTileLocal(t);
        auto eligible=[&](int index) {
            const auto& p=t.beachSpots[size_t(index)];
            const double rank=double((unsigned(index)*37u+11u)%101u)/101.;
            return rank<factor/(t.reducedDensity?4.:1.)&&std::hypot(p.towel.x-eye.x,p.towel.z-eye.z)<100.;
        };
        for(auto& v:t.beachVisitors) {
            if(v.spot>=0&&(!eligible(v.spot)||!budget)) {
                if(v.person.node)v.person.node->setEnabled(false);
                if(v.furniture)v.furniture->setEnabled(false);
                v.spot=-1;
            }
            if(v.spot<0&&budget&&!created&&factor>0) {
                int best=-1;double distance=100.;
                for(size_t i=0;i<t.beachSpots.size();++i) {
                    if(!eligible(int(i)))continue;
                    if(std::any_of(t.beachVisitors.begin(),t.beachVisitors.end(),[&](const auto& other){return other.spot==int(i);}))continue;
                    const auto& p=t.beachSpots[i];double d=std::hypot(p.towel.x-eye.x,p.towel.z-eye.z);
                    if(d<distance){distance=d;best=int(i);}
                }
                if(best>=0) {
                    size_t slot=size_t(&v-t.beachVisitors.data());
                    if(!v.person.node&&!makePerson(t,slot,v.person))continue;
                    if(v.furniture)v.furniture->queueFree();
                    v.furniture=makeBeachFurniture(t,t.beachSpots[best].parasol,unsigned(best));
                    v.spot=best;v.age=0;created=true;
                    v.person.node->setEnabled(true);
                }
            }
            if(v.spot<0)continue;
            --budget;++beachLive;v.age+=std::min(double(dt),.1);
            const auto& spot=t.beachSpots[size_t(v.spot)];
            auto pose=r1::beachPose(spot.towel,spot.water,v.age,unsigned(v.spot));
            auto& p=v.person;
            if(p.body)p.body->setEnabled(false);
            const double ground=groundUnder(t,pose.position.x,pose.position.z,pose.position.y);
            const double y=pose.swimming?-.85+.04*std::sin(beachClock*2+v.spot):ground+.06;
            glm::quat facing=glm::angleAxis(float(-pose.heading),glm::vec3(0,1,0))*glm::quat(0,0,1,0);
            glm::vec3 recliningOffset=facing*glm::vec3(0,float(.13*pose.recline),float(-playerHeight*.5*pose.recline));
            p.node->transform().position=glm::vec3(float(pose.position.x),float(y),float(pose.position.z))+recliningOffset;
            p.node->transform().rotation=facing*glm::angleAxis(float(pose.recline*r1::kPi/2),glm::vec3(1,0,0));
            const char* clip=pose.walking?"walk":"idle";
            if(std::strcmp(p.clip,clip)!=0){p.animator->play(clip);p.clip=clip;}
            const double d=std::hypot(pose.position.x-eye.x,pose.position.z-eye.z);
            const float rate=d<15?30.f:d<40?12.f:4.f;
            if(rate!=p.poseRate){p.animator->setPoseRate(rate,saida::Animator::PoseRateMode::Hold);p.poseRate=rate;}
            for(int arm=0;arm<2;++arm)if(p.arms[arm]) {
                const float side=arm==0?-1.f:1.f;
                const double phase=beachClock*1.8+arm*r1::kPi;
                glm::vec3 target(side*float(.4+.18*std::sin(phase)),float(1.05+.12*std::cos(phase)),.3f);
                p.arms[arm]->reach(target,pose.swimming?1.f:0.f);
                p.arms[arm]->setWeightNow(pose.swimming?1.f:0.f);
            }
            v.furniture->transform().position=glm::vec3(spot.towel.x,spot.towel.y,spot.towel.z);
            const double heading=std::atan2(spot.water.x-spot.towel.x,-(spot.water.z-spot.towel.z));
            v.furniture->transform().rotation=glm::angleAxis(float(-heading),glm::vec3(0,1,0))*glm::quat(0,0,1,0);
        }
    }
}
