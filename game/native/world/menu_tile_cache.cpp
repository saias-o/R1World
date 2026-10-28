#include "menu_tile_cache.hpp"
#include "gen/net.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace r1::world {
std::string MenuTileCache::key(const r1::MapTile& t){return std::to_string(t.z)+"/"+std::to_string(t.x)+"/"+std::to_string(t.y);}

bool MenuTileCache::validImage(const std::string& body) {
    return (body.size()>8&&std::memcmp(body.data(),"\x89PNG\r\n\x1a\n",8)==0)||
           (body.size()>3&&std::memcmp(body.data(),"\xff\xd8\xff",3)==0);
}

MenuTileCache::MenuTileCache(fs::path project,Source s):source(std::move(s)),root(std::move(project)/"cache/map-tiles"/source.name){}

fs::path MenuTileCache::path(const r1::MapTile& tile) const {
    return root/std::to_string(tile.z)/std::to_string(tile.x)/(std::to_string(tile.y)+source.extension);
}

bool MenuTileCache::ready(const r1::MapTile& tile) {
    const auto k=key(tile);
    if(present.count(k))return true;
    if(unread.count(k))return false;
    std::error_code ec;const auto p=path(tile);
    if(!fs::is_regular_file(p,ec)||fs::file_size(p,ec)<=8){unread.insert(k);return false;}
    present.insert(k);return true;
}

fs::path MenuTileCache::marker(const r1::MapTile& tile) const {return path(tile).string()+".none";}

bool MenuTileCache::missingForGood(const r1::MapTile& tile) {
    const auto k=key(tile);
    if(absent.count(k))return true;
    if(!checkedAbsent.insert(k).second)return false;
    std::error_code ec;
    if(!fs::exists(marker(tile),ec))return false;
    absent.insert(k);return true;
}

void MenuTileCache::setVisible(std::vector<r1::MapTile> tiles){visibleTiles=std::move(tiles);}

size_t MenuTileCache::missing() {
    return size_t(std::count_if(visibleTiles.begin(),visibleTiles.end(),[&](const auto& tile){return !ready(tile)&&!missingForGood(tile);}));
}

bool MenuTileCache::unavailable() const {return !lastError.empty()&&std::chrono::steady_clock::now()<retryAfter;}

bool MenuTileCache::pump() {
    const auto now=std::chrono::steady_clock::now();
    bool changed=false;
    for(auto it=flights.begin();it!=flights.end();) {
        if(it->second.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
        const Result result=it->second.get();
        if(result.ready){lastError.clear();present.insert(it->first);unread.erase(it->first);}
        else if(result.refused) {
            absent.insert(it->first);
            {std::error_code ec;fs::create_directories(root/it->first.substr(0,it->first.rfind('/')),ec);}
            std::ofstream(root/(it->first+source.extension+".none"));
            saida::Log::info("[World map] ",source.name," has no tile ",it->first,": ",result.error);
        } else {
            lastError=result.error;
            retryAfter=now+std::chrono::seconds(30);
            saida::Log::info("[World map] ",source.name," tiles unavailable: ",lastError);
        }
        changed=true;it=flights.erase(it);
    }
    if(now<retryAfter)return changed;
    for(const auto& tile:visibleTiles) {
        if(flights.size()>=source.parallel)break;
        const auto k=key(tile);
        if(ready(tile)||missingForGood(tile)||std::any_of(flights.begin(),flights.end(),[&](const auto& f){return f.first==k;}))continue;
        const fs::path target=path(tile);
        const std::string url=source.url(tile);
        flights.emplace_back(k,std::async(std::launch::async,[this,url,target]() -> Result {
            try {
                const auto response=r1::net::request("GET",url,{}, {},10.);
                if(response.status!=200||!validImage(response.body))
                    return {false,(response.status>=400&&response.status<500)||response.status==200,
                            "HTTP "+std::to_string(response.status)+", "+std::to_string(response.body.size())+" bytes, no image, from "+url};
                fs::create_directories(target.parent_path());
                const fs::path temporary=target.string()+".part";
                {std::ofstream output(temporary,std::ios::binary);
                 output.write(response.body.data(),std::streamsize(response.body.size()));
                 if(!output)throw std::runtime_error("cannot cache map tile");}
                fs::rename(temporary,target);
                return {true,false,{}};
            } catch(const std::exception& e) {return {false,false,e.what()};}
        }));
    }
    return changed;
}

MenuTileCache::~MenuTileCache(){for(auto& f:flights)if(f.second.valid())f.second.wait();}
} // namespace r1::world
