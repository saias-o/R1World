cmake_minimum_required(VERSION 3.20)
project(R1World LANGUAGES CXX)
find_package(SaidaEngine CONFIG REQUIRED)

file(GLOB generator CONFIGURE_DEPENDS "native/gen/*.cpp")
file(GLOB clipper CONFIGURE_DEPENDS "native/third_party/clipper2/src/*.cpp")
saida_add_game(R1World CUSTOM_MAIN PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/world-windows"
    SOURCES native/world.cpp native/minimap.cpp ${generator} ${clipper})
target_include_directories(R1World PRIVATE native
    native/third_party/clipper2/include native/third_party/earcut
    "${SaidaEngine_SOURCE_DIR}/plugins/traffic/include")
target_link_libraries(R1World PRIVATE winhttp)
set_source_files_properties(${generator} ${clipper} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# Integration checks consume the same SDK as the game, never copied flags.
foreach(check vehicle_runtime motorcycle_pose vehicle_radio)
    add_executable(r1_${check}_test EXCLUDE_FROM_ALL "native/tests_engine/${check}_test.cpp")
    target_link_libraries(r1_${check}_test PRIVATE Saida::Engine)
    target_compile_options(r1_${check}_test PRIVATE -UNDEBUG)
    target_include_directories(r1_${check}_test PRIVATE native)
    set_target_properties(r1_${check}_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/native-tests")
endforeach()
target_sources(r1_vehicle_radio_test PRIVATE native/gen/radio.cpp native/gen/net.cpp)
target_link_libraries(r1_vehicle_radio_test PRIVATE winhttp)
