#include "remains_store.hpp"
#include "../atomic_file.hpp"
#include <cmath>
#include <fstream>

namespace r1::world {
RemainsWriter::RemainsWriter():worker_([this]{run();}){}
RemainsWriter::~RemainsWriter() {
    flush();
    {std::lock_guard<std::mutex> guard(lock_);stopping_=true;}
    wake_.notify_all();worker_.join();
}
bool RemainsWriter::submit(const std::filesystem::path& path,nlohmann::json snapshot) {
    auto immutable=std::make_shared<const nlohmann::json>(std::move(snapshot));
    {
        std::lock_guard<std::mutex> guard(lock_);
        if(!error_.empty() || (!pending_.count(path)&&pending_.size()>=kPending))return false;
        pending_[path]=std::move(immutable);
    }
    wake_.notify_one();return true;
}
nlohmann::json RemainsWriter::read(const std::filesystem::path& path) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        if(auto it=pending_.find(path);it!=pending_.end())return *it->second;
        if(writing_&&writingPath_==path)return *writing_;
    }
    std::ifstream input(path);return nlohmann::json::parse(input);
}
void RemainsWriter::flush() {
    std::unique_lock<std::mutex> guard(lock_);
    wake_.wait(guard,[&]{return !error_.empty()||(pending_.empty()&&!writing_);});
}
std::string RemainsWriter::error() const {std::lock_guard<std::mutex> guard(lock_);return error_;}
void RemainsWriter::run() {
    for(;;) {
        {
            std::unique_lock<std::mutex> guard(lock_);
            wake_.wait(guard,[&]{return stopping_||(!pending_.empty()&&error_.empty());});
            if(stopping_)return;
            auto it=pending_.begin();writingPath_=it->first;writing_=it->second;pending_.erase(it);
        }
        try {atomicWrite(writingPath_,writing_->dump());}
        catch(const std::exception& e) {
            std::lock_guard<std::mutex> guard(lock_);error_=e.what();
        }
        {
            std::lock_guard<std::mutex> guard(lock_);
            // On failure retain the immutable snapshot for reads and recovery.
            if(error_.empty()) {writing_.reset();writingPath_.clear();}
        }
        wake_.notify_all();
    }
}
RemainsIndex::Cell RemainsIndex::cell(double x,double y,double z) {
    return {int(std::floor(x/2000.)),int(std::floor(y/2000.)),int(std::floor(z/2000.))};
}
void RemainsIndex::add(size_t id,double x,double y,double z) {cells_[cell(x,y,z)].push_back(id);}
std::vector<size_t> RemainsIndex::nearby(double x,double y,double z) const {
    std::vector<size_t> out;const auto centre=cell(x,y,z);
    for(int a=-1;a<=1;++a)for(int b=-1;b<=1;++b)for(int c=-1;c<=1;++c) {
        const auto it=cells_.find({centre[0]+a,centre[1]+b,centre[2]+c});
        if(it!=cells_.end())out.insert(out.end(),it->second.begin(),it->second.end());
    }
    return out;
}
}
