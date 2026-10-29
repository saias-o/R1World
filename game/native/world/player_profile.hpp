#pragma once
#include "../combat.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace r1::world {
enum class PhotoCategory { World, Animal, Monument, Event };
const char* categoryKey(PhotoCategory category);
const char* categoryLabel(PhotoCategory category);

struct PhotoAnimal {
    std::string species, name;
};

struct Photograph {
    std::string id, file, title, takenUtc, place;
    PhotoCategory category=PhotoCategory::World;
    double longitude=0, latitude=0, altitude=0;
    float fovDegrees=62;
    std::vector<PhotoAnimal> animals;
};
bool hasPhotoSpecies(const Photograph& photo,const std::string& species);

class Wallet {
public:
    static constexpr int64_t kMaximum=1'000'000'000'000;
    int64_t balance() const { return balance_; }
    bool credit(int64_t amount);
    bool spend(int64_t amount);
private:
    int64_t balance_=0; // cents; arithmetic never passes through floating point
};

struct PlayerProfile {
    Wallet wallet;
    std::map<std::string,int> items{{"camera",1}};
    combat::Arsenal::Snapshot arsenal;
    std::vector<Photograph> photos;
    bool addItem(const std::string& id,int count=1);
    bool consumeItem(const std::string& id,int count=1);
};

// Profile and images live outside the disposable geographic cache. A damaged
// or future save is reported and protected against replacement for this run.
class PlayerProfileStore {
public:
    explicit PlayerProfileStore(std::filesystem::path directory):directory_(std::move(directory)){}
    PlayerProfile load(std::string& error);
    bool save(const PlayerProfile& profile,std::string& error) const;
    std::filesystem::path photoPath(const Photograph& photo) const;
    Photograph newPhoto() const;
    bool writable() const { return writable_; }
private:
    std::filesystem::path directory_;
    bool writable_=true;
};
} // namespace r1::world
