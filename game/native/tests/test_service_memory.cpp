// A cook that runs out of memory (gen/service): said, fewer at once, and the
// tile served all the same once memory allows -- never a crash later.
#include "check.hpp"

#include "gen/service.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <new>
#include <limits>
#include <thread>

using namespace r1;

namespace {
std::shared_ptr<const ServedTile> waitMemoryTile(WorldService& service, const Tile& tile) {
    for (int i=0;i<400;++i) {
        if (auto served=service.find(tile)) return served;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return {};
}
}

TEST(Service, memory_pressure_pauses_before_allocating_and_cache_has_a_hard_limit) {
    const auto headroom=std::make_shared<std::atomic<uint64_t>>(768ull*1024*1024-1);
    const auto prepared=std::make_shared<std::atomic<int>>(0);
    WorldService::Options options;
    options.gameRoot=r1test::gameRoot();
    options.threads=2;
    options.keep=1;
    options.requestLimit=1;
    options.availableMemory=[headroom] { return std::optional<uint64_t>{headroom->load()}; };
    options.prepare=[prepared](ServedTile&) { ++*prepared; };
    WorldService service(std::move(options));
    const auto first=tileAt(2.3522,48.8566), second=tileAt(2.3622,48.8566);
    CHECK(!(first==second));
    service.want({first,second},{});
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(service.status().memoryPaused);
    CHECK(*prepared==0);
    CHECK(service.status().cooking==0);
    headroom->store(std::numeric_limits<uint64_t>::max());
    auto tile=waitMemoryTile(service,first);
    CHECK(tile);
    CHECK(!service.find(second)); // beyond explicit admission limit: no eviction/re-cook loop
    std::weak_ptr<const ServedTile> released=tile;
    tile.reset();
    service.want({second},{});
    CHECK(waitMemoryTile(service,second));
    CHECK(!service.find(first));
    CHECK(released.expired());
    CHECK(service.status().outOfMemory==0);
}

TEST(Service, admission_reserves_memory_for_cooks_that_have_not_allocated_yet) {
    struct Work { std::atomic<int> active{0}, peak{0}; };
    const auto work=std::make_shared<Work>();
    WorldService::Options options;
    options.gameRoot=r1test::gameRoot();
    options.threads=4;
    options.keep=3;
    options.availableMemory=[] { return std::optional<uint64_t>{768ull*1024*1024}; };
    options.prepare=[work](ServedTile&) {
        const int active=++work->active;
        work->peak.store(std::max(work->peak.load(),active));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        --work->active;
    };
    WorldService service(std::move(options));
    const std::vector<Tile> tiles={tileAt(2.3522,48.8566),tileAt(2.3622,48.8566),tileAt(2.3722,48.8566)};
    service.want(tiles,{});
    for (const auto& tile:tiles) CHECK(waitMemoryTile(service,tile));
    CHECK(work->peak==1);
    CHECK(service.status().outOfMemory==0);
}

TEST(Service, a_cook_out_of_memory_is_said_halves_the_cooks_and_is_served_again) {
#ifdef _WIN32
    _putenv_s("HTTPS_PROXY", "http://127.0.0.1:9");
    _putenv_s("HTTP_PROXY", "http://127.0.0.1:9");
#endif
    // The cooking threads outlive the service: they own what they write to.
    struct Said { std::mutex lock; std::vector<std::string> lines; std::atomic<int> prepared{0}; };
    const auto said = std::make_shared<Said>();
    WorldService::Options options;
    options.gameRoot = r1test::gameRoot();
    options.threads = 4;
    options.prepare = [said](ServedTile&) {
        if (said->prepared++ == 0) throw std::bad_alloc();
    };
    options.log = [said](const std::string& s) { std::lock_guard<std::mutex> g(said->lock); said->lines.push_back(s); };
    WorldService service(std::move(options));
    const Tile t = tileAt(2.3522, 48.8566);
    service.want({t}, {});
    std::shared_ptr<const ServedTile> served;
    for (int i = 0; i < 400 && !(served = service.find(t)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_MSG(served, t.key() << " was not served again within 20 s of running out of memory");
    const auto status = service.status();
    CHECK(status.outOfMemory == 1);
    CHECK(status.allowed == 2);
    std::lock_guard<std::mutex> g(said->lock);
    bool told = false;
    for (const auto& s : said->lines) told |= s.rfind("OUT-OF-MEMORY " + t.key(), 0) == 0;
    CHECK(told);
}

TEST(Service, request_capacity_is_independent_of_the_cold_cache_target) {
    WorldService::Options options;
    options.gameRoot=r1test::gameRoot();options.keep=2;options.requestLimit=15;
    options.availableMemory=[] {return std::optional<uint64_t>{0};};
    WorldService service(std::move(options));
    for(size_t locals:{9u,10u,12u}) {
        std::vector<Tile> priority;
        for(size_t i=0;i<locals+8;++i)priority.push_back(Tile{18000,int(i)});
        const auto admission=service.want(priority,{});
        CHECK(admission.accepted.size()==15);
        CHECK(admission.refused.size()==priority.size()-15);
        for(size_t i=0;i<locals;++i)CHECK(admission.accepted[i]==priority[i]);
    }
}
