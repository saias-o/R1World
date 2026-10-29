cmake_minimum_required(VERSION 3.20)
project(R1World LANGUAGES CXX)
find_package(SaidaEngine CONFIG REQUIRED)

add_subdirectory("${SaidaEngine_SOURCE_DIR}/plugins/xrgesture"
    "${CMAKE_CURRENT_BINARY_DIR}/xrgesture")

file(GLOB generator CONFIGURE_DEPENDS "native/gen/*.cpp")
file(GLOB clipper CONFIGURE_DEPENDS "native/third_party/clipper2/src/*.cpp")
set(world_runtime
    native/world/world.cpp
    native/world/geography.cpp
    native/world/tile_preparation.cpp
    native/world/menu_tile_cache.cpp
    native/world/lifecycle.cpp
    native/world/update.cpp
    native/world/player.cpp
    native/world/camera.cpp
    native/world/capture.cpp
    native/world/photography.cpp
    native/world/photo_animals.cpp
    native/world/photo_detection.cpp
    native/world/player_profile.cpp
    native/world/hud.cpp
    native/world/smoke_update.cpp
    native/world/menu.cpp
    native/world/options.cpp
    native/world/options_store.cpp
    native/world/arrival.cpp
    native/world/streaming.cpp
    native/world/tile_streaming.cpp
    native/world/terrain.cpp
    native/world/assets.cpp
    native/world/distant.cpp
    native/world/environment.cpp
    native/world/interiors.cpp
    native/world/travel.cpp
    native/world/vehicles.cpp
    native/world/boats.cpp
    native/world/aircraft.cpp
    native/world/sea.cpp
    native/world/traffic.cpp
    native/world/people.cpp
    native/world/beach.cpp
    native/world/fauna.cpp
    native/world/bird.cpp
    native/world/animal_remains.cpp
    native/world/remains_store.cpp
    native/world/combat.cpp
    native/world/rocket.cpp
    native/world/police.cpp
    native/world/cheats.cpp
    native/world/performance.cpp
    native/world/smoke.cpp
    native/world/rocket_smoke.cpp)
saida_add_game(R1World CUSTOM_MAIN PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/world-windows"
    SOURCES native/world.cpp ${world_runtime} native/minimap.cpp native/menu_globe.cpp native/menu_globe_view.cpp ${generator} ${clipper})
target_include_directories(R1World PRIVATE native
    native/third_party/clipper2/include native/third_party/earcut
    "${SaidaEngine_SOURCE_DIR}/plugins/traffic/include"
    "${SaidaEngine_SOURCE_DIR}/plugins/wildlife/include")
target_link_libraries(R1World PRIVATE winhttp Saida::XRGesture)
find_program(R1_GLSLC glslc REQUIRED)
set(menu_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/generated/world-windows/menu-shaders")
foreach(stage vert frag)
    add_custom_command(OUTPUT "${menu_shader_dir}/menu_globe.${stage}.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${menu_shader_dir}"
        COMMAND ${R1_GLSLC} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/menu_globe.${stage}"
            -o "${menu_shader_dir}/menu_globe.${stage}.spv"
        DEPENDS "shaders/menu_globe.${stage}" VERBATIM)
    list(APPEND menu_shaders "${menu_shader_dir}/menu_globe.${stage}.spv")
endforeach()

add_executable(r1_menu_globe_test EXCLUDE_FROM_ALL native/tests_engine/menu_globe_test.cpp native/menu_globe_view.cpp)

target_link_libraries(r1_menu_globe_test PRIVATE Saida::Engine)
target_include_directories(r1_menu_globe_test PRIVATE native)
target_compile_options(r1_menu_globe_test PRIVATE -UNDEBUG)
set_target_properties(r1_menu_globe_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/world-windows")

add_executable(r1_menu_intro_test EXCLUDE_FROM_ALL native/tests_engine/menu_intro_test.cpp)
target_compile_features(r1_menu_intro_test PRIVATE cxx_std_17)
target_include_directories(r1_menu_intro_test PRIVATE native)
target_compile_options(r1_menu_intro_test PRIVATE -UNDEBUG)
set_target_properties(r1_menu_intro_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/native-tests")

add_executable(r1_menu_preview EXCLUDE_FROM_ALL native/tests_engine/menu_preview.cpp native/menu_globe.cpp native/menu_globe_view.cpp)
target_link_libraries(r1_menu_preview PRIVATE Saida::Engine)
target_include_directories(r1_menu_preview PRIVATE native)
add_dependencies(r1_menu_preview r1_menu_shaders)
set_target_properties(r1_menu_preview PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/world-windows")
add_custom_target(r1_menu_shaders DEPENDS ${menu_shaders})
add_dependencies(R1World r1_menu_shaders)

set_source_files_properties(${generator} ${clipper} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# Integration checks consume the same SDK as the game, never copied flags.
foreach(check vehicle_runtime motorcycle_pose vehicle_radio animal_physics bird_animation)
    add_executable(r1_${check}_test EXCLUDE_FROM_ALL "native/tests_engine/${check}_test.cpp")
    target_link_libraries(r1_${check}_test PRIVATE Saida::Engine)
    target_compile_options(r1_${check}_test PRIVATE -UNDEBUG)
    target_include_directories(r1_${check}_test PRIVATE native)
    set_target_properties(r1_${check}_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/native-tests")
endforeach()
target_sources(r1_vehicle_radio_test PRIVATE native/gen/radio.cpp native/gen/net.cpp)
target_link_libraries(r1_vehicle_radio_test PRIVATE winhttp)

add_executable(r1_xr_gesture_test EXCLUDE_FROM_ALL native/tests_engine/xr_gesture_test.cpp)
target_link_libraries(r1_xr_gesture_test PRIVATE Saida::XRGesture)
target_include_directories(r1_xr_gesture_test PRIVATE native)
target_compile_options(r1_xr_gesture_test PRIVATE -UNDEBUG)
set_target_properties(r1_xr_gesture_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/native-tests")

# Runtime module contracts. These executables never create an Engine/window/GPU.
add_executable(r1_world_geography_test EXCLUDE_FROM_ALL
    native/tests_engine/world_geography_test.cpp native/world/geography.cpp)
target_link_libraries(r1_world_geography_test PRIVATE glm::glm)

add_executable(r1_world_tile_preparation_test EXCLUDE_FROM_ALL
    native/tests_engine/world_tile_preparation_test.cpp
    native/world/tile_preparation.cpp native/gen/mesh.cpp)
target_link_libraries(r1_world_tile_preparation_test PRIVATE Saida::Engine)

add_executable(r1_world_menu_tile_cache_test EXCLUDE_FROM_ALL
    native/tests_engine/world_menu_tile_cache_test.cpp
    native/world/menu_tile_cache.cpp native/gen/net.cpp)
target_link_libraries(r1_world_menu_tile_cache_test PRIVATE Saida::Engine winhttp)

add_executable(r1_world_options_store_test EXCLUDE_FROM_ALL
    native/tests_engine/world_options_store_test.cpp native/world/options_store.cpp)
target_include_directories(r1_world_options_store_test PRIVATE "${SaidaEngine_SOURCE_DIR}/third_party/json")

add_executable(r1_world_runtime_contract_test EXCLUDE_FROM_ALL
    native/tests_engine/world_runtime_contract_test.cpp native/world/remains_store.cpp)
target_include_directories(r1_world_runtime_contract_test PRIVATE "${SaidaEngine_SOURCE_DIR}/third_party/json")

enable_testing()
add_executable(r1_world_player_profile_test EXCLUDE_FROM_ALL
    native/tests_engine/world_player_profile_test.cpp native/world/player_profile.cpp)
target_include_directories(r1_world_player_profile_test PRIVATE "${SaidaEngine_SOURCE_DIR}/third_party/json")
add_executable(r1_world_photo_detection_test EXCLUDE_FROM_ALL
    native/tests_engine/world_photo_detection_test.cpp native/world/photo_detection.cpp)
target_link_libraries(r1_world_photo_detection_test PRIVATE Saida::Engine)
foreach(check geography tile_preparation menu_tile_cache options_store runtime_contract player_profile photo_detection)
    target_compile_features(r1_world_${check}_test PRIVATE cxx_std_17)
    target_compile_options(r1_world_${check}_test PRIVATE -UNDEBUG)
    target_include_directories(r1_world_${check}_test PRIVATE native)
    set_target_properties(r1_world_${check}_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/native-tests")
    add_test(NAME world_${check} COMMAND r1_world_${check}_test)
    set_tests_properties(world_${check} PROPERTIES LABELS "headless;world" TIMEOUT 30)
endforeach()
add_custom_target(r1_world_headless_tests DEPENDS
    r1_world_geography_test r1_world_tile_preparation_test r1_world_menu_tile_cache_test r1_world_options_store_test r1_world_runtime_contract_test r1_world_player_profile_test r1_world_photo_detection_test)

# The same generator/service tests as build_tools.sh, now compiled by CMake
# so a headless run cannot silently exercise an old standalone binary.
file(GLOB generator_checks CONFIGURE_DEPENDS "native/tests/*.cpp")
add_executable(r1_generator_test EXCLUDE_FROM_ALL ${generator_checks} ${generator} ${clipper} native/minimap.cpp)
target_compile_features(r1_generator_test PRIVATE cxx_std_17)
target_compile_options(r1_generator_test PRIVATE -ffp-contract=off)
target_include_directories(r1_generator_test PRIVATE native native/third_party/clipper2/include
    native/third_party/earcut "${SaidaEngine_SOURCE_DIR}/third_party/json")
target_link_libraries(r1_generator_test PRIVATE winhttp)
add_dependencies(r1_world_headless_tests r1_generator_test)
foreach(suite Service Sources Seams)
    add_test(NAME generator_${suite} COMMAND r1_generator_test --game "${CMAKE_CURRENT_SOURCE_DIR}" "${suite}.")
    set_tests_properties(generator_${suite} PROPERTIES LABELS "headless;generator" TIMEOUT 180
        ENVIRONMENT "HTTP_PROXY=http://127.0.0.1:9;HTTPS_PROXY=http://127.0.0.1:9")
endforeach()
