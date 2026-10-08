#include "check.hpp"
#include "../density_policy.hpp"
#include <chrono>

TEST(Density, one_frame_over_threshold_is_permanent_and_local) {
    const auto root = std::filesystem::path(r1test::gameRoot()) / "generated" /
        ("density-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const r1::Tile tile{27038,26176}, neighbour{27038,26177};
    r1::DensityPolicy policy(root);
    CHECK(!policy.observe(tile,120000));
    CHECK(!policy.reduced(tile));
    CHECK(policy.observe(tile,120001));
    CHECK(policy.reduced(tile));
    CHECK(!policy.observe(tile,1));
    CHECK(!policy.reduced(neighbour));
    r1::DensityPolicy restarted(root);
    CHECK(restarted.reduced(tile));
    CHECK(!restarted.reduced(neighbour));
    NEAR(r1::DensityPolicy::kCrowdScale,.25,0);
    std::filesystem::remove_all(root);
}
