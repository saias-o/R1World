#include "check.hpp"
#include "gen/places.hpp"

using namespace r1;

TEST(Places, suggestions_keep_rank_and_distinguish_namesakes) {
    const auto choices = parsePlaceChoices(R"({"features":[
        {"geometry":{"type":"Point","coordinates":[2.3522,48.8566]},"properties":{"name":"Paris","county":"Paris","country":"France"}},
        {"geometry":{"type":"Point","coordinates":[-95.5555,33.6609]},"properties":{"name":"Paris","state":"Texas","country":"États-Unis"}},
        {"geometry":{"type":"Point","coordinates":[2.4,48.84]},"properties":{"name":"Paris","county":"Paris","country":"France"}}
    ]})");
    CHECK(choices.size() == 2);
    CHECK(choices[0].label == "Paris · France");
    CHECK(choices[1].label == "Paris · Texas · États-Unis");
    NEAR(choices[1].lon, -95.5555, 0);
}

TEST(Places, malformed_coordinates_are_never_offered) {
    const auto choices = parsePlaceChoices(R"({"features":[
        {"geometry":{"type":"Point","coordinates":[181,48]},"properties":{"name":"Bad"}},
        {"geometry":{"type":"LineString","coordinates":[[2,48],[3,48]]},"properties":{"name":"Road"}},
        {"geometry":{"type":"Point","coordinates":[-1.55,47.22]},"properties":{"name":"Nantes","country":"France"}}
    ]})");
    CHECK(choices.size() == 1);
    CHECK(choices[0].name == "Nantes");
}
