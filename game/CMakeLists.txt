cmake_minimum_required(VERSION 3.20)
project(R1World LANGUAGES CXX)
find_package(SaidaEngine CONFIG REQUIRED)

add_subdirectory("${SaidaEngine_SOURCE_DIR}/plugins/xrgesture"
    "${CMAKE_CURRENT_BINARY_DIR}/xrgesture")

file(GLOB generator CONFIGURE_DEPENDS "native/gen/*.cpp")
file(GLOB clipper CONFIGURE_DEPENDS "native/third_party/clipper2/src/*.cpp")
saida_add_game(R1World CUSTOM_MAIN PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/world-windows"
    SOURCES native/world.cpp native/minimap.cpp native/menu_globe.cpp native/menu_globe_view.cpp ${generator} ${clipper})
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

add_executable(r1_xr_gesture_test EXCLUDE_FROM_ALL native/tests_engine/xr_gesture_test.cpp)
target_link_libraries(r1_xr_gesture_test PRIVATE Saida::XRGesture)
target_include_directories(r1_xr_gesture_test PRIVATE native)
target_compile_options(r1_xr_gesture_test PRIVATE -UNDEBUG)
set_target_properties(r1_xr_gesture_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/native-tests")
