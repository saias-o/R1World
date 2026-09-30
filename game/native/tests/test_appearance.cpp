#include "check.hpp"
#include "gen/appearance.hpp"
#include <fstream>

using namespace r1;

TEST(Appearance, country_lookup_and_local_variety) {
    const auto map = nlohmann::json::parse(R"({"features":[
      {"properties":{"ISO_A2_EH":"BF","SUBREGION":"Western Africa"},"geometry":{"type":"Polygon","coordinates":[[[-5,9],[2,9],[2,15],[-5,15],[-5,9]]]}},
      {"properties":{"ISO_A2_EH":"FR","SUBREGION":"Western Europe"},"geometry":{"type":"Polygon","coordinates":[[[-6,42],[9,42],[9,51],[-6,51],[-6,42]]]}}
    ]})");
    CountryCrowd crowd;
    crowd.load(map);
    const Country burkina=crowd.at(-1.5197,12.3714), france=crowd.at(2.3522,48.8566);
    CHECK(burkina.code == "BF");
    CHECK(france.code == "FR");
    CHECK(crowd.at(-30,0).code.empty());
    const std::vector<CrowdAppearance> avatars = {
        {'m',"light"},{'m',"medium"},{'m',"dark"},
        {'f',"light"},{'f',"medium"},{'f',"dark"}
    };
    int burkinaDark=0, franceDark=0, burkinaWomen=0;
    for (size_t slot=0; slot<1000; ++slot) {
        const size_t a=crowd.choose(burkina,avatars,12345,slot);
        const size_t b=crowd.choose(france,avatars,12345,slot);
        CHECK(a==crowd.choose(burkina,avatars,12345,slot));
        burkinaDark += avatars[a].skinTone=="dark";
        franceDark += avatars[b].skinTone=="dark";
        burkinaWomen += avatars[a].sex=='f';
    }
    CHECK(burkinaDark>930);
    CHECK(franceDark<250);
    CHECK(burkinaWomen>400 && burkinaWomen<600);
    for (const char* region : {"Western Africa","Middle Africa","Eastern Africa","Southern Africa"}) {
        int dark=0;
        for (size_t slot=0;slot<1000;++slot)
            dark += avatars[crowd.choose(Country{"",region},avatars,12345,slot)].skinTone=="dark";
        CHECK(dark>930);
        for (uint32_t seed : {1u,12345u,987654u}) {
            int firstSixtyDark=0;
            for (size_t slot=0;slot<60;++slot) {
                const bool selectedDark=avatars[crowd.choose(Country{"",region},avatars,seed,slot)].skinTone=="dark";
                if (slot<12) CHECK(selectedDark);
                firstSixtyDark += selectedDark;
            }
            CHECK(firstSixtyDark>=58);
        }
    }
}

TEST(Appearance, bundled_country_map_finds_ouagadougou_and_paris) {
    std::ifstream source("assets/world/countries.geojson");
    CHECK(bool(source));
    nlohmann::json geojson; source >> geojson;
    CountryCrowd crowd;
    crowd.load(geojson);
    const Country burkina=crowd.at(-1.5197,12.3714), france=crowd.at(2.3522,48.8566);
    CHECK(burkina.code == "BF");
    CHECK(france.code == "FR");
    std::ifstream humans("assets/models/humans/humans.json");
    CHECK(bool(humans));
    nlohmann::json manifest; humans >> manifest;
    std::vector<CrowdAppearance> avatars;
    for (const auto& person : manifest.at("crowd"))
        avatars.push_back({person.at("sex").get<std::string>()[0],person.at("skinTone").get<std::string>()});
    int darkBurkina=0, darkFrance=0;
    for (size_t slot=0;slot<1000;++slot) {
        darkBurkina += avatars[crowd.choose(burkina,avatars,56789,slot)].skinTone=="dark";
        darkFrance += avatars[crowd.choose(france,avatars,56789,slot)].skinTone=="dark";
    }
    CHECK(darkBurkina>930);
    CHECK(darkFrance<250);
    for (size_t slot=0;slot<12;++slot)
        CHECK(avatars[crowd.choose(burkina,avatars,56789,slot)].skinTone=="dark");
}
