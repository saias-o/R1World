#pragma once
#include "audio/AudioStream.hpp"
#include "audio/AudioManager.hpp"
#include "core/Log.hpp"
#include "gen/radio.hpp"
#include "net/HttpRequest.hpp"
#include <algorithm>
#include <cmath>

namespace r1 {
class VehicleRadio {
public:
    explicit VehicleRadio(std::filesystem::path game):game_(std::move(game)){}
    void update(double dt,double lon,double lat,bool aboard,bool toggle,bool next,bool previous,int volumeStep) {
        if(toggle&&aboard){wanted_=!wanted_;if(!wanted_){stream_.stop();playing_=false;}else{attempts_=0;retry_=0;}}
        if(volumeStep)volume_=std::clamp(volume_+volumeStep*.05f,0.f,1.f);
        const float gain=volume_*saida::AudioManager::get().masterVolume();
        if(gain!=gain_){gain_=gain;stream_.setVolume(gain);}
        if(query_) {
            if(auto response=request_.take()){requestPending_=false;query_->accept(response->status,response->body,response->error);}
            if(query_->ready()) {
                auto result=query_->take();query_.reset();request_.cancel();
                if(radio::distanceMetres(lon,lat,askLon_,askLat_)<15000.) {
                    catalogue_=std::move(result);selected_=0;attempts_=0;
                    if(!catalogue_.error.empty())saida::Log::warn("[World radio] ",catalogue_.error);
                    if(wanted_&&aboard&&!catalogue_.stations.empty())tune(0);
                }
            } else if(!requestPending_) {
                request_.get(query_->url(),query_->timeout());requestPending_=true;
            }
        }
        if(!aboard) {
            if(playing_){stream_.stop();playing_=false;}
            attempts_=0;waiting_=0;
            return;
        }
        if(!wanted_)return;
        if(!saida::AudioStream::supported()){catalogue_.error="Lecture radio indisponible sur cette plateforme.";return;}
        retry_-=dt;
        if((next||previous)&&catalogue_.stations.empty())retry_=0;
        const bool moved=radio::distanceMetres(lon,lat,askLon_,askLat_)>10000.;
        if(!query_&&retry_<=0&&(catalogue_.stations.empty()||moved)) {
            askLon_=lon;askLat_=lat;retry_=30.;
            query_=std::make_unique<radio::Discovery>(game_,lon,lat,!saida::AudioStream::allowsInsecureHttp());
            requestPending_=false;
        }
        if(catalogue_.stations.empty())return;
        if(next||previous){attempts_=0;tune(previous?-1:1);}
        else if(!playing_&&attempts_==0)tune(0);
        if(!playing_)return;
        const auto state=stream_.status();
        if(state.state==saida::AudioStream::State::Playing){waiting_=0;return;}
        if(state.state==saida::AudioStream::State::AwaitingGesture){waiting_=0;return;}
        waiting_+=dt;
        if(state.state==saida::AudioStream::State::Failed||state.state==saida::AudioStream::State::Ended||waiting_>15.) {
            saida::Log::warn("[World radio] stream unavailable: ",catalogue_.stations[selected_].name," error=",state.error);
            if(attempts_<catalogue_.stations.size())tune(1);
            else {stream_.stop();playing_=false;catalogue_.error="Stations unavailable — N to retry.";}
        }
    }
    std::string label(double lon,double lat) const {
        const std::string keys="B: radio · N / Shift+N: station · PgUp/PgDn: volume";
        if(!wanted_)return "Radio off · "+keys;
        if(!catalogue_.error.empty())return catalogue_.error+" · B: turn off";
        if(catalogue_.stations.empty())return "Looking for local stations… · B: turn off";
        const auto& station=catalogue_.stations[selected_];
        const double km=radio::distanceMetres(lon,lat,station.lon,station.lat)/1000.;
        const auto state=stream_.status().state;
        const std::string status=state==saida::AudioStream::State::Playing?"":state==saida::AudioStream::State::AwaitingGesture?" · Tap to listen":" · Connecting…";
        return station.name+" · "+std::to_string(int(std::round(km)))+" km"+status+
            " · "+std::to_string(int(volume_*100))+" %\n"+keys+" · Radio Browser";
    }
private:
    void tune(int direction) {
        const auto size=catalogue_.stations.size();if(!size)return;
        selected_=(selected_+size+direction)%size;
        catalogue_.error.clear();waiting_=0;++attempts_;
        const auto& station=catalogue_.stations[selected_];
        playing_=true;stream_.open(station.url);
        saida::Log::info("[World radio] tuning ",station.name," distance=",int(station.distance),"m");
    }
    std::filesystem::path game_;
    saida::AudioStream stream_;
    std::unique_ptr<radio::Discovery> query_;
    saida::HttpRequest request_;
    bool requestPending_=false;
    radio::Catalogue catalogue_;
    bool wanted_=false,playing_=false;
    float volume_=.35f,gain_=-1;
    double askLon_=0,askLat_=90,retry_=0,waiting_=0;
    size_t selected_=0,attempts_=0;
};
} // namespace r1
